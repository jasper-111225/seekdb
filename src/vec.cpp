// 向量内核：平方欧氏距离 + TopK。AVX-512 / AVX2+FMA / 标量三级运行时分发。
#include "simdb.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define MV_X86 1
#endif

namespace mv {

int simd_level() {
#if MV_X86
  static int lv = -1;
  if (lv < 0) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f")) lv = 3;
    else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) lv = 2;
    else lv = 0;
  }
  return lv;
#else
  return 0;
#endif
}

static float l2sq_scalar(const float* a, const float* b, int n) {
  float s = 0.f;
  for (int i = 0; i < n; i++) {
    float d = a[i] - b[i];
    s += d * d;
  }
  return s;
}

#if MV_X86
__attribute__((target("avx2,fma")))
static float l2sq_avx2(const float* a, const float* b, int n) {
  __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
  int i = 0;
  for (; i + 16 <= n; i += 16) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    a0 = _mm256_fmadd_ps(d0, d0, a0);
    a1 = _mm256_fmadd_ps(d1, d1, a1);
  }
  for (; i + 8 <= n; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    a0 = _mm256_fmadd_ps(d, d, a0);
  }
  __m256 s = _mm256_add_ps(a0, a1);
  __m128 lo = _mm256_castps256_ps128(s);
  __m128 hi = _mm256_extractf128_ps(s, 1);
  __m128 s4 = _mm_add_ps(lo, hi);
  s4 = _mm_hadd_ps(s4, s4);
  s4 = _mm_hadd_ps(s4, s4);
  float r = _mm_cvtss_f32(s4);
  for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
  return r;
}

__attribute__((target("avx512f")))
static float l2sq_avx512(const float* a, const float* b, int n) {
  __m512 a0 = _mm512_setzero_ps(), a1 = _mm512_setzero_ps(), a2 = _mm512_setzero_ps();
  int i = 0;
  for (; i + 48 <= n; i += 48) {
    __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
    __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
    __m512 d2 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 32), _mm512_loadu_ps(b + i + 32));
    a0 = _mm512_fmadd_ps(d0, d0, a0);
    a1 = _mm512_fmadd_ps(d1, d1, a1);
    a2 = _mm512_fmadd_ps(d2, d2, a2);
  }
  for (; i + 16 <= n; i += 16) {
    __m512 d = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
    a0 = _mm512_fmadd_ps(d, d, a0);
  }
  __m512 s = _mm512_add_ps(_mm512_add_ps(a0, a1), a2);
  float r = _mm512_reduce_add_ps(s);
  for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
  return r;
}
#endif

float vec_l2sq(const float* a, const float* b, int dim) {
#if MV_X86
  int lv = simd_level();
  if (lv >= 3) return l2sq_avx512(a, b, dim);
  if (lv >= 2) return l2sq_avx2(a, b, dim);
#endif
  return l2sq_scalar(a, b, dim);
}

namespace {

struct Cand {
  float d2;
  int64_t pk;
  uint32_t slot;
};

inline bool better(float d2a, int64_t pka, float d2b, int64_t pkb) {
  return d2a < d2b || (d2a == d2b && pka < pkb);
}

}  // namespace

// 单线程、无并发：thread_local 复用候选数组，避免每条查询重新分配。
size_t vec_topk(const Table& t, const float* q, const uint8_t* mask, size_t k, Hit* out) {
  if (k == 0 || t.sch.dim <= 0) return 0;
  if (k > 4096) k = 4096;
  static std::vector<Cand> heap;
  heap.clear();
  heap.reserve(k + 1);

  const int dim = t.sch.dim;
  const float* base = t.vecs.data();
  const uint32_t n = t.nslots;
  const int64_t* pks = t.pk.data();
  const uint8_t* used = t.used.data();

  int lv = simd_level();
  float (*dist)(const float*, const float*, int) = l2sq_scalar;
#if MV_X86
  if (lv >= 3) dist = l2sq_avx512;
  else if (lv >= 2) dist = l2sq_avx2;
#endif

  for (uint32_t s = 0; s < n; s++) {
    if (!used[s]) continue;
    if (mask && !mask[s]) continue;
    float d2 = dist(base + (size_t)s * dim, q, dim);
    int64_t pk = pks[s];
    if (heap.size() == k) {
      const Cand& w = heap.back();
      if (!better(d2, pk, w.d2, w.pk)) continue;
      // 插入到有序位置并丢掉最后一个
      size_t pos = heap.size() - 1;
      while (pos > 0 && better(d2, pk, heap[pos - 1].d2, heap[pos - 1].pk)) {
        heap[pos] = heap[pos - 1];
        pos--;
      }
      heap[pos] = Cand{d2, pk, s};
    } else {
      size_t pos = heap.size();
      heap.push_back(Cand{d2, pk, s});
      while (pos > 0 && better(d2, pk, heap[pos - 1].d2, heap[pos - 1].pk)) {
        heap[pos] = heap[pos - 1];
        pos--;
      }
      heap[pos] = Cand{d2, pk, s};
    }
  }
  size_t nout = heap.size();
  for (size_t i = 0; i < nout; i++) {
    out[i].d2 = heap[i].d2;
    out[i].pk = heap[i].pk;
    out[i].slot = heap[i].slot;
  }
  return nout;
}

}  // namespace mv
