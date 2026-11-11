#!/usr/bin/env python3
"""Summarize a blobtrace capture: MEM_ALIAS calls, fragment-job framebuffer
parameters, and per-draw shader / blend state.

    summarize.py work/re/<name> [--shaders]
"""
import re
import sys
from pathlib import Path

FBD_KEYS = ("Internal Format", "Sample Count", "Color Writeback Format", "MSAA",
            "Color Preload Enable", "Color Write Enable", "Sample Layout",
            "ZS Format", "ZS Write Enable", "ZS Preload Enable", "S Write Enable")
STATE_KEYS = ("Multisample enable", "Evaluate per-sample", "Sample mask", "Blend shader",
              "Load destination", "Depth function", "Depth write mask",
              "Alpha-to-coverage", "Write enable", "Color Mask")


def blocks(lines, start_re):
    """Split lines into blocks starting at lines matching start_re."""
    cur = None
    for ln in lines:
        if re.match(start_re, ln):
            if cur:
                yield cur
            cur = [ln]
        elif cur is not None:
            cur.append(ln)
    if cur:
        yield cur


def field(block, key):
    for ln in block:
        m = re.match(r"\s*" + re.escape(key) + r": (.*)", ln)
        if m:
            return m.group(1)
    return None


def sub(block, header):
    """Lines of an indented sub-struct like 'Color Writeback:'."""
    out, on, ind = [], False, 0
    for ln in block:
        if ln.strip() == header + ":":
            on, ind = True, len(ln) - len(ln.lstrip())
            continue
        if on:
            if len(ln) - len(ln.lstrip()) <= ind:
                break
            out.append(ln.strip())
    return out


def main():
    d = Path(sys.argv[1])
    show_shaders = "--shaders" in sys.argv
    log = d / "ioctl.log"
    print(f"### {d.name}")
    if log.exists():
        for ln in log.read_text().splitlines():
            if "ALIAS" in ln or ln.startswith("  ["):
                print("  " + ln)
    text = (d / "decode.txt").read_text().splitlines()
    for jb in blocks(text, r"^(Job Header|==== )"):
        head = jb[0]
        if head.startswith("===="):
            if "sub" in head:
                print(head)
            continue
        jtype = field(jb, "Type")
        if jtype == "Fragment":
            # The fragment job's FBD is the last "Framebuffer @" or "Parameters:" block.
            print(f"  FRAGMENT {head.split('(')[1].rstrip('):')}")
            for k in FBD_KEYS:
                print(f"    {k}: {field(jb, k)}")
            for s in ("Color Writeback", "ZS Writeback", "S Writeback"):
                v = sub(jb, s)
                if v and "Base: 0x0" not in v[0]:
                    print(f"    {s}: {', '.join(v)}")
            cc = [field(jb, f"Clear Color {i}") for i in range(4)]
            print(f"    Clear Colors: {cc}  Z Clear: {field(jb, 'Z Clear')}")
            for ln in jb:
                if "Invalid field" in ln:
                    print("    " + ln.strip())
        elif jtype == "Tiler":
            st = {k: field(jb, k) for k in STATE_KEYS}
            print(f"  TILER {head.split('(')[1].rstrip('):')}: " +
                  ", ".join(f"{k}={v}" for k, v in st.items() if v is not None))
            if show_shaders:
                for sb in blocks(jb, r"^Shader 0x"):
                    va = re.search(r"GPU VA (\w+)", sb[0]).group(1)
                    code = []
                    for ln in sb[1:]:
                        if ln.startswith(("State:", "Viewport", "// ", "Tiler Job", "Blend")) or \
                                ln.startswith("  "):
                            break
                        if ln.strip():
                            code.append(ln)
                    print(f"    shader @{va}:")
                    for c in code:
                        print("      " + c)


if __name__ == "__main__":
    main()
