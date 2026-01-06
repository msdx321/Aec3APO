#pragma once

#include <immintrin.h>

#if defined(__AVX2__)
#define SIMD_SIZE 8
using __mx = __m256;
inline __mx simd_setzero() { return _mm256_setzero_ps(); }
inline __mx simd_set1(float v) { return _mm256_set1_ps(v); }
inline __mx simd_loadu(const float *p) { return _mm256_loadu_ps(p); }
inline void simd_storeu(float *p, __mx v) { _mm256_storeu_ps(p, v); }
inline __mx simd_add(__mx a, __mx b) { return _mm256_add_ps(a, b); }
inline __mx simd_sub(__mx a, __mx b) { return _mm256_sub_ps(a, b); }
inline __mx simd_mul(__mx a, __mx b) { return _mm256_mul_ps(a, b); }
inline __mx simd_cmpge(__mx a, __mx b) { return _mm256_cmp_ps(a, b, _CMP_GE_OQ); }
inline __mx simd_and(__mx a, __mx b) { return _mm256_and_ps(a, b); }
inline __mx simd_andnot(__mx a, __mx b) { return _mm256_andnot_ps(a, b); }
inline __mx simd_or(__mx a, __mx b) { return _mm256_or_ps(a, b); }
#else
#define SIMD_SIZE 4
using __mx = __m128;
inline __mx simd_setzero() { return _mm_setzero_ps(); }
inline __mx simd_set1(float v) { return _mm_set1_ps(v); }
inline __mx simd_loadu(const float *p) { return _mm_loadu_ps(p); }
inline void simd_storeu(float *p, __mx v) { _mm_storeu_ps(p, v); }
inline __mx simd_add(__mx a, __mx b) { return _mm_add_ps(a, b); }
inline __mx simd_sub(__mx a, __mx b) { return _mm_sub_ps(a, b); }
inline __mx simd_mul(__mx a, __mx b) { return _mm_mul_ps(a, b); }
inline __mx simd_cmpge(__mx a, __mx b) { return _mm_cmpge_ps(a, b); }
inline __mx simd_and(__mx a, __mx b) { return _mm_and_ps(a, b); }
inline __mx simd_andnot(__mx a, __mx b) { return _mm_andnot_ps(a, b); }
inline __mx simd_or(__mx a, __mx b) { return _mm_or_ps(a, b); }
#endif
