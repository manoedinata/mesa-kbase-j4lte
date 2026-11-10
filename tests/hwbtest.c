/* Allocate an AHardwareBuffer like the camera's YUV capture stream, dump the
 * gralloc handle and what lockPlanes returns. usage: hwbtest [w h fmt usage] */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <android/hardware_buffer.h>

typedef struct { int version, numFds, numInts, data[0]; } nh_t;

int main(int argc, char **argv)
{
   AHardwareBuffer_Desc d = { 0 };
   d.width = argc > 1 ? atoi(argv[1]) : 2048;
   d.height = argc > 2 ? atoi(argv[2]) : 1536;
   d.format = argc > 3 ? strtoul(argv[3], NULL, 0) : 0x23;
   d.usage = argc > 4 ? strtoull(argv[4], NULL, 0) : 0x20003;
   d.layers = 1;
   AHardwareBuffer *b = NULL;
   int r = AHardwareBuffer_allocate(&d, &b);
   printf("allocate %ux%u fmt %#x usage %#llx -> %d\n", d.width, d.height, d.format,
          (unsigned long long)d.usage, r);
   if (r)
      return 1;
   const nh_t *(*get)(const AHardwareBuffer *) =
      (void *)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
   if (get) {
      const nh_t *h = get(b);
      printf("handle: numFds %d numInts %d\n", h->numFds, h->numInts);
      const int *w = (const int *)h; /* print as 32-bit words from byte 0 */
      int n = 3 + h->numFds + h->numInts;
      for (int i = 0; i < n; i++)
         printf("%3d:%08x%s", i * 4, w[i], (i % 8 == 7) ? "\n" : "  ");
      printf("\n");
   }
   AHardwareBuffer_Planes pl;
   r = AHardwareBuffer_lockPlanes(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &pl);
   printf("lockPlanes -> %d, %u planes\n", r, r ? 0 : pl.planeCount);
   for (unsigned i = 0; !r && i < pl.planeCount; i++)
      printf("  plane %u: data %p pixelStride %u rowStride %u (off from y %td)\n", i,
             pl.planes[i].data, pl.planes[i].pixelStride, pl.planes[i].rowStride,
             (uint8_t *)pl.planes[i].data - (uint8_t *)pl.planes[0].data);
   if (!r)
      AHardwareBuffer_unlock(b, NULL);
   AHardwareBuffer_release(b);
   return 0;
}
