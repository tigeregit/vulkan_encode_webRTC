#include <cuda_runtime.h>
// BT.709 limited range. One thread owns a complete 2x2 chroma block.
__global__ void rgba_to_nv12(cudaSurfaceObject_t src, unsigned char *dst, int pitch, int w, int h) {
  int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2,
      y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
  if (x >= w || y >= h)
    return;
  float r = 0, g = 0, b = 0;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      uchar4 c = surf2Dread<uchar4>(src, (x + i) * 4, y + j);
      float yy = 16.f + .182586f * c.x + .614231f * c.y + .062007f * c.z;
      dst[(y + j) * pitch + x + i] = (unsigned char)fminf(235.f, fmaxf(16.f, yy + .5f));
      r += c.x;
      g += c.y;
      b += c.z;
    }
  r *= .25f;
  g *= .25f;
  b *= .25f;
  int p = pitch * h + (y / 2) * pitch + x;
  dst[p] = (unsigned char)fminf(
      240.f, fmaxf(16.f, 128.f - .100644f * r - .338572f * g + .439216f * b + .5f));
  dst[p + 1] = (unsigned char)fminf(
      240.f, fmaxf(16.f, 128.f + .439216f * r - .398942f * g - .040274f * b + .5f));
}
extern "C" cudaError_t convert_nv12(cudaSurfaceObject_t src, unsigned char *dst, int pitch, int w,
                                    int h, cudaStream_t stream) {
  if (!stream || w % 2 || h % 2)
    return cudaErrorInvalidValue;
  rgba_to_nv12<<<dim3((w + 31) / 32, (h + 31) / 32), dim3(16, 16), 0, stream>>>(src, dst, pitch, w,
                                                                                h);
  return cudaGetLastError();
}
