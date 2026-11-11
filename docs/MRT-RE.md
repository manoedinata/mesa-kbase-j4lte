# Multiple render targets on Mali-T720 (Midgard v4): what the Arm blob does

Panfrost exposes one render target on Midgard v4 (T600/T620/T720): the v4
framebuffer descriptor is the single-target SFBD, and Mesa only emits the
multi-target MFBD on v5+. The Arm blob (r29p0, 32-bit, on the same phone)
exposes `GL_MAX_DRAW_BUFFERS=4` and passes every MRT case in
`tests/mrtprobe.c`. Traces show it never uses an MFBD on T720; it has two
SFBD-based paths.

Captured on SM-J400F, stock kbase r29p0, 32-bit `libGLES_mali.so`
(`OpenGL ES 3.1 v1.r29p0-01rel0`), with `tools/blobtrace/`.

## Tools

- `tools/blobtrace/blobspy.c`: 32-bit `LD_PRELOAD` for the blob. On each
  `KBASE_IOCTL_JOB_SUBMIT` it snapshots every readable `/dev/mali0` mapping
  and the atoms; logs `MEM_ALLOC` / `MEM_ALIAS` / `MEM_FREE`.
- `tools/blobtrace/blobdecode.c`: host decoder built on Mesa's pandecode and
  Midgard disassembler. `-d <va> <size> <file>` disassembles raw code.
- `tools/blobtrace/capture.sh <name> <cmd>`: capture + decode into
  `work/re/<name>/`.
- `tools/blobtrace/summarize.py work/re/<name> [--shaders]`: aliases, fragment
  job FBDs, draw state.
- `tools/blobtrace/build.sh`: builds both tools (host Mesa decode libs go to
  `work/mesa-24.3.4/build-host`).

Mapping facts needed for the snapshot:

- The `/proc/self/maps` file offset of a `/dev/mali0` mapping is its GPU VA
  (SAME_VA regions: offset == CPU address; exec, tiler heap, alias regions
  live above 4 GiB and are mapped elsewhere in the 32-bit address space).
- kbase mappings are PFNMAP: `process_vm_readv` fails on them, read directly.
- The blob maps the shader region twice (a one-page alias of page 0).
- The Midgard disassembler stops after the first writeout loop; shader
  variants placed after it must be disassembled with `blobdecode -d`.

## Path A: kbase alias + layered "samples" (one fragment pass)

Used when every attachment is 32 bpp, has the same row stride and sits at a
page-aligned address (draw, rt3, rt4, clear, clrdraw, blend, depth, level,
and the MRT blit target of the msaa case).

1. `KBASE_IOCTL_MEM_ALIAS` builds one GPU VA region from the attachments:

       MEM_ALIAS flags=0x100f stride=4 pages nents=4
         [0] handle=<RT0 BO> offset=<page of RT0> length=4
         [1] handle=<RT1 BO> offset=<page of RT1> length=4
         [2] handle=0x4000 (BASE_MEM_WRITE_ALLOC_PAGES_HANDLE: sink) ...
         [3] handle=0x4000
       -> gpu_va=0x10016f000 va_pages=16

   Stride = the size of one attachment in pages (64x64 RGBA8 = 4). Unused
   slots point at the write-alloc sink. Mip levels work if page-aligned
   (`level`: RT1 at page offset 0x14 of a 128x128 texture).
2. SFBD parameters: `Internal Format = Raw Value`, `Sample Count = 4`,
   `Color Writeback Format = 1_32B_CHANNEL`, `MSAA = Layered`,
   `Color Writeback = {alias base, row stride, Surface Stride = alias stride}`.
   Each "sample" of the tilebuffer is one RT, written back to its own surface.
   Word 36 of the parameters (FBD + 0xb0) is 0xc in this mode (unknown field;
   pandecode warns about it); it is 0 for normal and for real-MSAA FBDs.
3. Rasterization is single-sampled: RSD `Multisample enable = false`,
   `Sample mask = 0xffff`. All 4 samples share coverage, so no edge problems.
   Depth/stencil stays a normal 1x buffer (`depth` passes).
4. The fragment shader writes one raw 32-bit value per RT into the lanes of
   the output vec4, already packed to the attachment format:

       vmul.MOV.i32 R0, ..., <0xFF0000FF, 0xFFFF0000, 0, 0>   ; RT0 red, RT1 blue (RGBA8)
       br.write.always

5. A blend shader is always used (`Blend shader = true`, `Load destination =
   true`, `Color Mask = 0`). With `MIDGARD_OLD_BLEND`, `LD_SPECIAL.u32 ... +0x10`
   (selector 16, the old-blend tilebuffer read, as in midgard_compile.c)
   returns the 4 samples of the pixel as 4 lanes. The blend shader unpacks,
   blends/converts each written lane, keeps the lanes not written by this draw
   (`LD_SPECIAL R2.~yzw` / `R2.x~zw` masks), and writes all 4 lanes back.
6. Clears: `Clear Color 0..3` are the raw per-sample values, so each RT gets
   its own clear color (`glClearBufferfv` per RT: `0xFFFF00FF`, `0xFFFFFF00`,
   with the last value repeated in unused slots).
7. Preload (no clear): one draw per RT before the user's draws; it samples
   the attachment texture and writes only lane i through a blend shader.

## Path B: one tiler pass, one fragment job per RT (DCD Offset)

Used for anything path A can't alias: mixed bpp (`mixed2`: RGBA8 + R8),
integer + normalized (`int`), different sizes/row strides (`sizes`), and
multisampled attachments (`msaa`).

1. The vertex/tiler chain runs once; all fragment jobs share the polygon list.
2. Each draw gets one Draw descriptor (DCD) per RT, 128 bytes apart. The
   tiler job's Draw section is at job+64, so copy i is at job+64+128*i;
   the blob reserves that space after the 192-byte tiler job. Copies differ
   only in the RSD pointer; RSD i (64 bytes, also placed consecutively) points
   at a fragment shader variant that writes only RT i.
3. One SFBD and one fragment job per RT. FBD parameter `DCD Offset` (word 4)
   = 128*i selects the DCD copy for that pass; the rest of the FBD is a normal
   single-target FBD for RT i (its own format, stride, CRC buffer, clear).
4. Depth/stencil: every pass starts from the same ZS state (clear or
   preload); `ZS Write Enable` is set only on the last pass. Same primitives,
   same order, so each pass computes the same depth result (`mixdepth`).
5. Preload draws are also per-RT DCD variants.

## Consequences for Mesa

- Path B fits Panfrost without kernel help: v4 SFBD already has `DCD Offset`
  (`lib/genxml/v4.xml`), tiler jobs need `128 * (rt_count - 1)` extra bytes
  per draw, and the Gallium driver needs per-RT fragment shader variants
  (output i -> location 0, other outputs dropped), per-RT RSDs, and N
  fragment jobs per batch with per-RT FBDs. Things to keep single-shot:
  occlusion queries (Draw `Occlusion query = Disabled` in copies after the
  first) and fragment side effects (ES 3.1 SSBO/image stores). Cost: N
  fragment passes, but no extra vertex/tiler work.
- Path A is faster (one pass) but needs: a `MEM_ALIAS` hook through
  libdrm-kbase (no panfrost.ko equivalent), packing every RT to raw 32-bit in
  the fragment shader, blend shaders that work on all 4 lanes, and the
  word-36 field. Worth it only after B works, as an optimization for the
  common same-format case.

## Implementation (patches/mesa/0003)

Path B is implemented in Panfrost for v4 (`GL_MAX_DRAW_BUFFERS=4`):

- `pan_shader.c`, `pan_nir_remove_fragcolor_stores.c`: fragment shader key
  `v4_mrt_rt`; variant i keeps only RT i's colour output, moved to RT0.
- `pan_context.c`: `panfrost_get_blend_as_rt0()`, RT i's blend shader built
  as RT0.
- `pan_cmdstream.c`: per draw, each pass gets its own RSD (variant i, RT i
  blend state), uniforms/push constants (laid out per variant) and fragment
  varying descriptors (linked against the full shader, since the vertex
  shader runs once).
- `pan_jm.c`: tiler jobs get `128 * (n - 1)` extra bytes and one DRAW copy
  per pass (occlusion only in the first); one SFBD per non-null colour buffer
  with `DCD Offset = 128 * i`, Z/S written only by the last pass; a chain of
  fragment jobs, each allocated separately (64-byte alignment).
- `pan_fb_preload.c`: preload jobs get the same per-pass DCD copies (colour
  copy i loads RT i; culled placeholder where RT i has nothing to load).

With MaxColorAttachments >= 4, Mesa reports OpenGL ES 3.0 without
`MESA_GLES_VERSION_OVERRIDE`. All `tests/mrtprobe.c` scenarios and
`tests/gles3test.c` pass on SM-J400F; SurfaceFlinger and apps run on ES 3.0.

Not handled: fragment shader side effects (SSBO/image stores, ES 3.1) run
once per pass; framebuffer fetch is still limited to one render target.
