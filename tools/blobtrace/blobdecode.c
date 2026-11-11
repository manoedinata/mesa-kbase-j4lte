/* blobdecode: decode blobspy snapshots with Mesa's pandecode (Midgard v4).
 *
 *   blobdecode [-g gpu_id] subNNNN.bin...
 *   blobdecode -d gpu_va size subNNNN.bin    disassemble raw Midgard code
 *
 * Each atom's job chain is decoded with the memory captured at submit time.
 * Soft-job atoms (core_req bit 9, BASE_JD_REQ_SOFT_JOB) are skipped. Output
 * goes to stderr (pandecode), headers to stdout: run with 2>&1.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pandecode_context;
struct pandecode_context *pandecode_create_context(bool to_stderr);
void pandecode_destroy_context(struct pandecode_context *ctx);
void pandecode_inject_mmap(struct pandecode_context *ctx, uint64_t gpu_va, void *cpu, unsigned sz,
                           const char *name);
void pandecode_jc(struct pandecode_context *ctx, uint64_t jc_gpu_va, unsigned gpu_id);

void disassemble_midgard(FILE *fp, const void *code, size_t size, unsigned gpu_id, bool verbose);

/* Only Midgard is decoded; the Bifrost/Valhall disassemblers are not linked. */
void disassemble_bifrost(FILE *fp, const void *code, size_t size, bool verbose) {}
void disassemble_valhall(FILE *fp, const void *code, size_t size, bool verbose) {}

#define BASE_JD_REQ_SOFT_JOB (1u << 9)

static int decode_file(const char *path, unsigned gpu_id)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	uint32_t hdr[4];
	if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != 0x59505342 || hdr[1] != 1) {
		fprintf(stderr, "%s: not a blobspy v1 file\n", path);
		fclose(f);
		return 1;
	}
	struct { uint64_t jc; uint32_t core_req, atom_number; } *atoms = calloc(hdr[2], 16);
	if (fread(atoms, 16, hdr[2], f) != hdr[2]) { fclose(f); return 1; }

	struct pandecode_context *ctx = pandecode_create_context(true);
	void **bufs = calloc(hdr[3], sizeof(void *));
	struct { uint64_t va, len; } seen[1024];
	unsigned nseen = 0;
	for (uint32_t i = 0; i < hdr[3]; i++) {
		uint64_t va, len;
		if (fread(&va, 8, 1, f) != 1 || fread(&len, 8, 1, f) != 1) break;
		bufs[i] = malloc(len);
		if (fread(bufs[i], 1, len, f) != len) break;
		/* The blob can map one GPU region twice (e.g. the shader region
		 * appears again as a one-page alias); keep the first, larger map. */
		bool dup = false;
		for (unsigned k = 0; k < nseen; k++)
			if (va < seen[k].va + seen[k].len && seen[k].va < va + len)
				dup = true;
		if (dup) continue;
		if (nseen < 1024) { seen[nseen].va = va; seen[nseen].len = len; nseen++; }
		char name[32];
		snprintf(name, sizeof(name), "r%u", i);
		pandecode_inject_mmap(ctx, va, bufs[i], (unsigned)len, name);
	}
	fclose(f);

	printf("==== %s: %u atoms, %u regions\n", path, hdr[2], hdr[3]);
	fflush(stdout);
	for (uint32_t i = 0; i < hdr[2]; i++) {
		printf("==== atom %u jc=%llx core_req=%x\n", atoms[i].atom_number,
		       (unsigned long long)atoms[i].jc, atoms[i].core_req);
		fflush(stdout);
		if ((atoms[i].core_req & BASE_JD_REQ_SOFT_JOB) || !atoms[i].jc)
			continue;
		pandecode_jc(ctx, atoms[i].jc, gpu_id);
		fflush(stdout);
	}
	pandecode_destroy_context(ctx);
	for (uint32_t i = 0; i < hdr[3]; i++) free(bufs[i]);
	free(bufs);
	free(atoms);
	return 0;
}

/* Find gpu_va in a snapshot and disassemble size bytes from there. */
static int disasm(const char *path, uint64_t addr, size_t size)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	uint32_t hdr[4];
	if (fread(hdr, sizeof(hdr), 1, f) != 1) return 1;
	fseek(f, 16 * hdr[2], SEEK_CUR);
	for (uint32_t i = 0; i < hdr[3]; i++) {
		uint64_t va, len;
		if (fread(&va, 8, 1, f) != 1 || fread(&len, 8, 1, f) != 1) break;
		if (addr >= va && addr + size <= va + len) {
			uint8_t *buf = malloc(len);
			if (fread(buf, 1, len, f) != len) break;
			disassemble_midgard(stdout, buf + (addr - va), size, 0x720, true);
			free(buf);
			fclose(f);
			return 0;
		}
		fseek(f, len, SEEK_CUR);
	}
	fclose(f);
	fprintf(stderr, "%llx not found\n", (unsigned long long)addr);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "-d"))
		return disasm(argv[4], strtoull(argv[2], NULL, 16), strtoul(argv[3], NULL, 0));
	unsigned gpu_id = 0x720;
	int i = 1;
	if (argc > 2 && !strcmp(argv[1], "-g")) {
		gpu_id = strtoul(argv[2], NULL, 0);
		i = 3;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: %s [-g gpu_id] subNNNN.bin...\n", argv[0]);
		return 1;
	}
	int rc = 0;
	for (; i < argc; i++) rc |= decode_file(argv[i], gpu_id);
	return rc;
}
