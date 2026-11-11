/* blobspy: LD_PRELOAD for the 32-bit Arm Mali blob (kbase r29p0, Samsung
 * 64-byte atoms). On every KBASE_IOCTL_JOB_SUBMIT it writes a snapshot of all
 * CPU-readable /dev/mali0 mappings plus the submitted atoms to
 * $BLOBSPY_DIR/subNNNN.bin, for decoding on the host with blobdecode.
 *
 * Each mapping is stored at its GPU VA, which kbase puts in the mmap file
 * offset (/proc/self/maps column 3): SAME_VA regions have offset == CPU
 * address, the rest (exec, tiler heap, above 4 GiB) have offset == GPU VA. kbase mappings are PFNMAP (no
 * process_vm_readv), and grow-on-fault regions may have unbacked pages, so
 * pages are copied one at a time under a SIGBUS/SIGSEGV guard; unreadable
 * pages are written as zeros.
 *
 * File format (little endian):
 *   "BSPY" u32 version=1 u32 n_atoms u32 n_regions
 *   n_atoms x { u64 jc; u32 core_req; u32 atom_number }
 *   n_regions x { u64 gpu_va; u64 len; u8 data[len] }
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_REGION (64u << 20)

static sigjmp_buf jb;
static void on_fault(int sig) { siglongjmp(jb, 1); }

static void copy_page(void *dst, const void *src, size_t n)
{
	struct sigaction sa = { .sa_handler = on_fault }, old_segv, old_bus;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_segv);
	sigaction(SIGBUS, &sa, &old_bus);
	if (sigsetjmp(jb, 1) == 0)
		memcpy(dst, src, n);
	else
		memset(dst, 0, n);
	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);
}

static void dump(const void *atoms, uint32_t nr, uint32_t stride)
{
	static int seq;
	const char *dir = getenv("BLOBSPY_DIR");
	char path[256];
	snprintf(path, sizeof(path), "%s/sub%04d.bin", dir ? dir : "/data/local/tmp/blobspy", seq++);
	FILE *out = fopen(path, "wb");
	if (!out) return;

	/* Collect readable mali mappings. */
	struct { unsigned long lo, hi; unsigned long long gpu; } reg[1024];
	uint32_t nreg = 0;
	FILE *m = fopen("/proc/self/maps", "r");
	char line[512];
	/* Keep the raw /dev/mali lines next to the snapshot, for debugging. */
	snprintf(path, sizeof(path), "%s/sub%04d.maps", dir ? dir : "/data/local/tmp/blobspy", seq - 1);
	FILE *mt = fopen(path, "w");
	while (m && fgets(line, sizeof(line), m) && nreg < 1024) {
		unsigned long lo, hi;
		unsigned long long off;
		char perm[8];
		if (!strstr(line, "/dev/mali")) continue;
		if (mt) fputs(line, mt);
		if (sscanf(line, "%lx-%lx %7s %llx", &lo, &hi, perm, &off) != 4 || perm[0] != 'r') continue;
		if (hi - lo > MAX_REGION) continue;
		reg[nreg].lo = lo;
		reg[nreg].hi = hi;
		reg[nreg].gpu = off;
		nreg++;
	}
	if (m) fclose(m);
	if (mt) fclose(mt);

	uint32_t hdr[4] = { 0x59505342 /* "BSPY" */, 1, nr, nreg };
	fwrite(hdr, sizeof(hdr), 1, out);
	for (uint32_t i = 0; i < nr; i++) {
		const uint8_t *a = (const uint8_t *)atoms + (size_t)i * stride;
		struct { uint64_t jc; uint32_t core_req, atom_number; } rec;
		memcpy(&rec.jc, a + 0, 8);
		memcpy(&rec.core_req, a + 44, 4);
		rec.atom_number = a[40];
		fwrite(&rec, sizeof(rec), 1, out);
	}
	static uint8_t page[4096];
	for (uint32_t i = 0; i < nreg; i++) {
		uint64_t va = reg[i].gpu, len = reg[i].hi - reg[i].lo;
		fwrite(&va, 8, 1, out);
		fwrite(&len, 8, 1, out);
		for (uint64_t off = 0; off < len; off += sizeof(page)) {
			copy_page(page, (const void *)(uintptr_t)(reg[i].lo + off), sizeof(page));
			fwrite(page, sizeof(page), 1, out);
		}
	}
	fclose(out);
}

static FILE *ilog(void)
{
	static FILE *f;
	if (!f) {
		const char *dir = getenv("BLOBSPY_DIR");
		char path[256];
		snprintf(path, sizeof(path), "%s/ioctl.log", dir ? dir : "/data/local/tmp/blobspy");
		f = fopen(path, "w");
		if (f) setvbuf(f, NULL, _IOLBF, 0);
	}
	return f;
}

/* Log kbase memory ioctls: MEM_ALLOC (5), MEM_FREE (7), MEM_ALIAS (21). */
static void log_before(unsigned nr, const void *arg)
{
	FILE *f = ilog();
	if (!f) return;
	const uint64_t *u = arg;
	if (nr == 21) {
		fprintf(f, "MEM_ALIAS in flags=%llx stride=%llx nents=%llu\n", (unsigned long long)u[0],
		        (unsigned long long)u[1], (unsigned long long)u[2]);
		const uint64_t *ai = (const uint64_t *)(uintptr_t)u[3];
		for (uint64_t i = 0; i < u[2] && i < 16; i++)
			fprintf(f, "  [%llu] handle=%llx offset=%llx length=%llx\n", (unsigned long long)i,
			        (unsigned long long)ai[i * 3], (unsigned long long)ai[i * 3 + 1],
			        (unsigned long long)ai[i * 3 + 2]);
	}
}

static void log_after(unsigned nr, const void *arg, int ret)
{
	FILE *f = ilog();
	if (!f) return;
	const uint64_t *u = arg;
	if (nr == 5)
		fprintf(f, "MEM_ALLOC ret=%d -> flags=%llx gpu_va=%llx\n", ret, (unsigned long long)u[0],
		        (unsigned long long)u[1]);
	else if (nr == 7)
		fprintf(f, "MEM_FREE gpu_addr=%llx\n", (unsigned long long)u[0]);
	else if (nr == 21)
		fprintf(f, "MEM_ALIAS ret=%d -> flags=%llx gpu_va=%llx va_pages=%llx\n", ret,
		        (unsigned long long)u[0], (unsigned long long)u[1], (unsigned long long)u[2]);
	else if (nr == 2)
		fprintf(f, "JOB_SUBMIT\n");
}

int ioctl(int fd, int req_, ...)
{
	static int (*real)(int, int, ...);
	if (!real) real = dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, req_);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	unsigned req = (unsigned)req_;
	/* KBASE_IOCTL_JOB_SUBMIT = _IOW(0x80, 2, {u64 addr; u32 nr_atoms; u32 stride}) */
	if ((req & 0xffff) == 0x8002 && ((req >> 16) & 0x3fff) == 16) {
		const struct { uint64_t addr; uint32_t nr, stride; } *s = arg;
		dump((const void *)(uintptr_t)s->addr, s->nr, s->stride);
	}
	unsigned nr = req & 0xff, type = (req >> 8) & 0xff;
	if (type == 0x80) log_before(nr, arg);
	int ret = real(fd, req_, arg);
	if (type == 0x80) log_after(nr, arg, ret);
	return ret;
}
