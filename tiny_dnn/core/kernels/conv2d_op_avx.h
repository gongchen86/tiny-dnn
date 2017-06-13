/*
    Copyright (c) 2013, Taiga Nomi and the respective contributors
    All rights reserved.

    Use of this source code is governed by a BSD-style license that can be found
    in the LICENSE file.
*/
#pragma once

#include <vector>
#include "tiny_dnn/core/kernels/conv2d_op_internal.h"
#include "tiny_dnn/core/params/conv_params.h"

#ifdef CNN_USE_AVX
#include "tiny_dnn/core/kernels/avx_kernel_common.h"
#endif

namespace tiny_dnn {
namespace kernels {

#ifdef CNN_USE_AVX

// float ver
template <typename S1, typename S2, typename S3, typename S4>
void avx_conv2d_5x5_kernel(const core::conv_params &params,
                           const Tensor<float, S1> &in,
                           const Tensor<float, S2> &W,
                           const Tensor<float, S3> &bias,
                           Tensor<float, S4> &a,
                           const bool layer_parallelize) {
  CNN_UNREFERENCED_PARAMETER(layer_parallelize);
  assert(params.weight.height_ == 5 && params.weight.width_ == 5);

  auto &out       = params.out;
  auto &in_padded = params.in_padded;
  auto &tbl       = params.tbl;
  auto w_stride   = params.w_stride;

  const serial_size_t out_area = out.area();
  serial_size_t oidx           = 0;
  float bias_scale             = params.has_bias ? 1.0f : 0.0f;
  const serial_size_t stride   = params.h_stride * in_padded.width_;
  const serial_size_t inarea   = in_padded.area();

  static const __m256i imask = _mm256_setr_epi32(-1, -1, -1, -1, -1, 0, 0, 0);
  // static const __m256 mask = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1,
  // -1, -1, -1, 0, 0, 0));

  auto a_begin    = a.host_pbegin();
  auto bias_begin = bias.host_pbegin();
  const float *pw = W.host_pbegin();

  const __m128 y_bias_scale = _mm_set_ss(bias_scale);
  if (out.height_ == 1 && out.width_ == 1) {
    for (serial_size_t o = 0; o < out.depth_; ++o) {
      __m256 sum0     = _mm256_setzero_ps();
      __m256 sum1     = _mm256_setzero_ps();
      __m256 sum2     = _mm256_setzero_ps();
      __m128 sum3     = _mm_setzero_ps();
      const float *pi = in.host_pbegin();
      for (serial_size_t inc = 0; inc < params.in.depth_;
           ++inc, pw += 25, pi += inarea) {
        if (!tbl.is_connected(o, inc)) {
          continue;
        }
        __m256 w0   = _mm256_loadu_ps(pw + 0);
        __m256 w1   = _mm256_loadu_ps(pw + 8);
        __m256 w2   = _mm256_loadu_ps(pw + 16);
        __m256 i0   = _mm256_loadu_ps(pi + 0);
        __m256 i1   = _mm256_loadu_ps(pi + 8);
        __m256 i2   = _mm256_loadu_ps(pi + 16);
        __m128 w3   = _mm_load_ss(pw + 24);
        __m128 i3   = _mm_load_ss(pi + 24);
        __m256 tmp0 = _mm256_mul_ps(w0, i0);
        __m256 tmp1 = _mm256_mul_ps(w1, i1);
        __m256 tmp2 = _mm256_mul_ps(w2, i2);
        __m128 tmp3 = _mm_mul_ps(w3, i3);
        sum0        = _mm256_add_ps(tmp0, sum0);
        sum1        = _mm256_add_ps(tmp1, sum1);
        sum2        = _mm256_add_ps(tmp2, sum2);
        sum3        = _mm_add_ps(tmp3, sum3);
      }
      __m256 sum  = _mm256_add_ps(_mm256_add_ps(sum0, sum1), sum2);
      __m128 b    = _mm_load_ss(&bias_begin[o]);
      __m128 hsum = hsum256_ps(sum);
      b           = madd128_ss(b, y_bias_scale, sum3);
      _mm_store_ss(&a_begin[o], _mm_add_ss(hsum, b));
    }
  } else {
    const serial_size_t nblocks = out.width_ / 4;
    for (serial_size_t o = 0; o < out.depth_; ++o, oidx += out_area) {
      float *pa = &a_begin[oidx];
      // init to bias value
      float b = bias.host_at(0, o) * bias_scale;
      {
        size_t headSize = 0;
        __m256 b2       = _mm256_set1_ps(b);
        if (oidx & 7) {
          headSize = 8 - (oidx & 7);
          assert(headSize < out_area);
          for (size_t i = 0; i < headSize; ++i) {
            _mm_store_ss(&pa[i], _mm256_castps256_ps128(b2));
          }
        }
        size_t cnt = (out_area - headSize) / 16;
        float *pa2 = pa + headSize;
        for (size_t i = 0; i < cnt; ++i) {
          _mm256_store_ps(&pa2[i * 16 + 0], b2);
          _mm256_store_ps(&pa2[i * 16 + 8], b2);
        }
        for (size_t i = headSize + cnt * 16; i < out_area; ++i) {
          pa[i] = b;
        }
      }
      const float *weights_begin = W.host_pbegin();
      const float *in_begin      = in.host_pbegin();
      for (serial_size_t inc = 0; inc < params.in.depth_; ++inc) {
        if (!tbl.is_connected(o, inc)) continue;

        const float *pw = &weights_begin[25 * (params.in.depth_ * o + inc)];
        const float *pi = &in_begin[in_padded.get_index(0, 0, inc)];

        __m256 w0a = _mm256_maskload_ps(pw + 0, imask);
        __m256 w1a = _mm256_maskload_ps(pw + 5, imask);
        __m256 w2a = _mm256_maskload_ps(pw + 10, imask);
        __m256 w3a = _mm256_maskload_ps(pw + 15, imask);
        __m256 w4a = _mm256_maskload_ps(pw + 20, imask);
        __m256 w0b = leftShift<4>(w0a);
        __m256 w1b = leftShift<4>(w1a);
        __m256 w2b = leftShift<4>(w2a);
        __m256 w3b = leftShift<4>(w3a);
        __m256 w4b = leftShift<4>(w4a);
        __m256 w0c = leftShift<8>(w0a);
        __m256 w1c = leftShift<8>(w1a);
        __m256 w2c = leftShift<8>(w2a);
        __m256 w3c = leftShift<8>(w3a);
        __m256 w4c = leftShift<8>(w4a);
        __m256 w0d = leftShift<12>(w0a);
        __m256 w1d = leftShift<12>(w1a);
        __m256 w2d = leftShift<12>(w2a);
        __m256 w3d = leftShift<12>(w3a);
        __m256 w4d = leftShift<12>(w4a);
        float *ppa = pa;
        if (w_stride == 1) {
          if (nblocks) {
            for (serial_size_t y = 0; y < out.height_; ++y, ppa += out.width_) {
              const float *pi0 = (pi + y * stride);
              const float *pi1 = pi0 + 1 * in_padded.width_;
              const float *pi2 = pi0 + 2 * in_padded.width_;
              const float *pi3 = pi0 + 3 * in_padded.width_;
              const float *pi4 = pi0 + 4 * in_padded.width_;
              __m256 dst0, dst1, dst2, dst3;
              __m256 i0 = _mm256_loadu_ps(pi0);
              __m256 i1 = _mm256_loadu_ps(pi1);
              __m256 i2 = _mm256_loadu_ps(pi2);
              __m256 i3 = _mm256_loadu_ps(pi3);
              __m256 i4 = _mm256_loadu_ps(pi4);
              dst0      = _mm256_mul_ps(w0a, i0);
              dst1      = _mm256_mul_ps(w0b, i0);
              dst2      = _mm256_mul_ps(w0c, i0);
              dst3      = _mm256_mul_ps(w0d, i0);
              dst0      = madd256_ps(w1a, i1, dst0);
              dst1      = madd256_ps(w1b, i1, dst1);
              dst2      = madd256_ps(w1c, i1, dst2);
              dst3      = madd256_ps(w1d, i1, dst3);
              dst0      = madd256_ps(w2a, i2, dst0);
              dst1      = madd256_ps(w2b, i2, dst1);
              dst2      = madd256_ps(w2c, i2, dst2);
              dst3      = madd256_ps(w2d, i2, dst3);
              dst0      = madd256_ps(w3a, i3, dst0);
              dst1      = madd256_ps(w3b, i3, dst1);
              dst2      = madd256_ps(w3c, i3, dst2);
              dst3      = madd256_ps(w3d, i3, dst3);
              dst0      = madd256_ps(w4a, i4, dst0);
              dst1      = madd256_ps(w4b, i4, dst1);
              dst2      = madd256_ps(w4c, i4, dst2);
              dst3      = madd256_ps(w4d, i4, dst3);

              __m128 sum      = _mm_loadu_ps(ppa);
              __m128 hsum0123 = hsum4x256_ps(dst0, dst1, dst2, dst3);
              sum             = _mm_add_ps(sum, hsum0123);
              _mm_storeu_ps(ppa, sum);
              for (size_t i = 1; i < nblocks; ++i) {
                i0   = _mm256_loadu_ps(pi0 + i * 4);
                i1   = _mm256_loadu_ps(pi1 + i * 4);
                i2   = _mm256_loadu_ps(pi2 + i * 4);
                i3   = _mm256_loadu_ps(pi3 + i * 4);
                i4   = _mm256_loadu_ps(pi4 + i * 4);
                dst0 = _mm256_mul_ps(w0a, i0);
                dst1 = _mm256_mul_ps(w0b, i0);
                dst2 = _mm256_mul_ps(w0c, i0);
                dst3 = _mm256_mul_ps(w0d, i0);
                dst0 = madd256_ps(w1a, i1, dst0);
                dst1 = madd256_ps(w1b, i1, dst1);
                dst2 = madd256_ps(w1c, i1, dst2);
                dst3 = madd256_ps(w1d, i1, dst3);
                dst0 = madd256_ps(w2a, i2, dst0);
                dst1 = madd256_ps(w2b, i2, dst1);
                dst2 = madd256_ps(w2c, i2, dst2);
                dst3 = madd256_ps(w2d, i2, dst3);
                dst0 = madd256_ps(w3a, i3, dst0);
                dst1 = madd256_ps(w3b, i3, dst1);
                dst2 = madd256_ps(w3c, i3, dst2);
                dst3 = madd256_ps(w3d, i3, dst3);
                dst0 = madd256_ps(w4a, i4, dst0);
                dst1 = madd256_ps(w4b, i4, dst1);
                dst2 = madd256_ps(w4c, i4, dst2);
                dst3 = madd256_ps(w4d, i4, dst3);

                sum      = _mm_loadu_ps(ppa + i * 4);
                hsum0123 = hsum4x256_ps(dst0, dst1, dst2, dst3);
                sum      = _mm_add_ps(sum, hsum0123);
                _mm_storeu_ps(ppa + i * 4, sum);
              }
              for (serial_size_t x = nblocks * 4; x < out.width_; ++x) {
                sum         = _mm_load_ss(&ppa[x]);
                i0          = _mm256_loadu_ps(pi0 + x);
                i1          = _mm256_loadu_ps(pi1 + x);
                i2          = _mm256_loadu_ps(pi2 + x);
                i3          = _mm256_loadu_ps(pi3 + x);
                i4          = _mm256_maskload_ps(pi4 + x, imask);
                __m256 sum0 = _mm256_mul_ps(w0a, i0);
                __m256 sum1 = _mm256_mul_ps(w1a, i1);
                sum0        = madd256_ps(w2a, i2, sum0);
                sum1        = madd256_ps(w3a, i3, sum1);
                sum0        = madd256_ps(w4a, i4, sum0);
                sum0        = _mm256_add_ps(sum0, sum1);
                _mm_store_ss(&ppa[x], _mm_add_ss(sum, hsum256_ps(sum0)));
              }     // x loop
            }       // y loop
          } else {  // if (nblocks) {
            for (serial_size_t y = 0; y < out.height_; ++y, ppa += out.width_) {
              const float *pi0 = (pi + y * stride);
              const float *pi1 = pi0 + 1 * in_padded.width_;
              const float *pi2 = pi0 + 2 * in_padded.width_;
              const float *pi3 = pi0 + 3 * in_padded.width_;
              const float *pi4 = pi0 + 4 * in_padded.width_;
              for (serial_size_t x = 0; x < out.width_; ++x) {
                __m128 sum  = _mm_load_ss(&ppa[x]);
                __m256 i0   = _mm256_loadu_ps(pi0 + x);
                __m256 i1   = _mm256_loadu_ps(pi1 + x);
                __m256 i2   = _mm256_loadu_ps(pi2 + x);
                __m256 i3   = _mm256_loadu_ps(pi3 + x);
                __m256 i4   = _mm256_maskload_ps(pi4 + x, imask);
                __m256 sum0 = _mm256_mul_ps(w0a, i0);
                __m256 sum1 = _mm256_mul_ps(w1a, i1);
                sum0        = madd256_ps(w2a, i2, sum0);
                sum1        = madd256_ps(w3a, i3, sum1);
                sum0        = madd256_ps(w4a, i4, sum0);
                sum0        = _mm256_add_ps(sum0, sum1);
                _mm_store_ss(&ppa[x], _mm_add_ss(sum, hsum256_ps(sum0)));
              }  // x loop
            }    // y loop
          }
        } else {  // if (w_stride == 1) {
          for (serial_size_t y = 0; y < out.height_; ++y, ppa += out.width_) {
            const float *pi0 = (pi + y * stride);
            const float *pi1 = pi0 + 1 * in_padded.width_;
            const float *pi2 = pi0 + 2 * in_padded.width_;
            const float *pi3 = pi0 + 3 * in_padded.width_;
            const float *pi4 = pi0 + 4 * in_padded.width_;
            for (serial_size_t x = 0; x < out.width_; ++x) {
              __m128 sum  = _mm_load_ss(&ppa[x]);
              __m256 i0   = _mm256_loadu_ps(pi0);
              __m256 i1   = _mm256_loadu_ps(pi1);
              __m256 i2   = _mm256_loadu_ps(pi2);
              __m256 i3   = _mm256_loadu_ps(pi3);
              __m256 i4   = _mm256_maskload_ps(pi4, imask);
              __m256 sum0 = _mm256_mul_ps(w0a, i0);
              __m256 sum1 = _mm256_mul_ps(w1a, i1);
              sum0        = madd256_ps(w2a, i2, sum0);
              sum1        = madd256_ps(w3a, i3, sum1);
              sum0        = madd256_ps(w4a, i4, sum0);
              sum0        = _mm256_add_ps(sum0, sum1);
              _mm_store_ss(&ppa[x], _mm_add_ss(sum, hsum256_ps(sum0)));
              pi0 += w_stride;
              pi1 += w_stride;
              pi2 += w_stride;
              pi3 += w_stride;
              pi4 += w_stride;
            }  // x loop
          }    // y loop
        }
      }  // in depth loop
    }    // out depth loop
  }      // else
}  // avx_conv2d_5x5_kernel float ver

// double ver
template <typename S1, typename S2, typename S3, typename S4>
void avx_conv2d_5x5_kernel(const core::conv_params &params,
                           const Tensor<double, S1> &in,
                           const Tensor<double, S2> &W,
                           const Tensor<double, S3> &bias,
                           Tensor<double, S4> &a,
                           const bool layer_parallelize) {
  assert(params.weight.height_ == 5 && params.weight.width_ == 5);

  auto &out       = params.out;
  auto &in_padded = params.in_padded;
  auto &tbl       = params.tbl;
  auto w_stride   = params.w_stride;

  const size_t out_area      = out.area();
  double bias_scale          = params.has_bias ? 1.0 : 0.0;
  const __m128d y_bias_scale = _mm_set_sd(bias_scale);
  serial_size_t oidx         = 0;

  const size_t in_stride      = params.h_stride * in_padded.width_;
  const size_t in_padded_area = in_padded.area();

  auto a_begin    = a.host_pbegin();
  auto bias_begin = bias.host_pbegin();
  auto w_begin    = W.host_pbegin();
  auto in_begin   = in.host_pbegin();
  if (out.height_ == 1 && out.width_ == 1) {
    const double *pw = w_begin;
    for (serial_size_t o = 0; o < out.depth_; ++o) {
      __m256d sum0 = _mm256_setzero_pd();
      __m256d sum1 = _mm256_setzero_pd();
      __m256d sum2 = _mm256_setzero_pd();
      __m256d sum3 = _mm256_setzero_pd();
      __m256d sum4 = _mm256_setzero_pd();
      __m256d sum5 = _mm256_setzero_pd();
      __m128d sum6 = _mm_setzero_pd();
      size_t inidx = 0;

      const double *i_begin = in.host_pbegin();
      for (serial_size_t inc = 0; inc < params.in.depth_;
           ++inc, pw += 25, inidx += in_padded_area) {
        if (!tbl.is_connected(o, inc)) {
          continue;
        }
        __m256d w0 = _mm256_loadu_pd(pw + 0);
        __m256d w1 = _mm256_loadu_pd(pw + 4);
        __m256d w2 = _mm256_loadu_pd(pw + 8);
        __m256d w3 = _mm256_loadu_pd(pw + 12);
        __m256d w4 = _mm256_loadu_pd(pw + 16);
        __m256d w5 = _mm256_loadu_pd(pw + 20);
        __m128d w6 = _mm_load_sd(pw + 24);

        const double *pi = &i_begin[inidx];

        __m256d i0   = _mm256_loadu_pd(pi + 0);
        __m256d i1   = _mm256_loadu_pd(pi + 4);
        __m256d i2   = _mm256_loadu_pd(pi + 8);
        __m256d i3   = _mm256_loadu_pd(pi + 12);
        __m256d i4   = _mm256_loadu_pd(pi + 16);
        __m256d i5   = _mm256_loadu_pd(pi + 20);
        __m128d i6   = _mm_load_sd(pi + 24);
        __m256d tmp0 = _mm256_mul_pd(w0, i0);
        __m256d tmp1 = _mm256_mul_pd(w1, i1);
        __m256d tmp2 = _mm256_mul_pd(w2, i2);
        __m256d tmp3 = _mm256_mul_pd(w3, i3);
        __m256d tmp4 = _mm256_mul_pd(w4, i4);
        __m256d tmp5 = _mm256_mul_pd(w5, i5);
        __m128d tmp6 = _mm_mul_pd(w6, i6);
        sum0         = _mm256_add_pd(tmp0, sum0);
        sum1         = _mm256_add_pd(tmp1, sum1);
        sum2         = _mm256_add_pd(tmp2, sum2);
        sum3         = _mm256_add_pd(tmp3, sum3);
        sum4         = _mm256_add_pd(tmp4, sum4);
        sum5         = _mm256_add_pd(tmp5, sum5);
        sum6         = _mm_add_pd(tmp6, sum6);
      }
      sum0         = _mm256_add_pd(sum0, sum1);
      sum2         = _mm256_add_pd(sum2, sum3);
      sum4         = _mm256_add_pd(sum4, sum5);
      sum0         = _mm256_add_pd(sum0, sum2);
      __m256d sum  = _mm256_add_pd(sum0, sum4);
      __m128d b    = _mm_load_sd(&bias_begin[o]);
      __m128d hsum = hsum256_pd(sum);
      b            = madd128_sd(b, y_bias_scale, sum6);
      _mm_store_sd(&a_begin[o], _mm_add_sd(hsum, b));
    }
  } else {
    for (serial_size_t o = 0; o < out.depth_; ++o, oidx += out_area) {
      double *pa = &a_begin[oidx];
      double b   = bias.host_at(0, o) * bias_scale;
      {
        size_t headSize = 0;
        __m256d b2      = _mm256_set1_pd(b);
        if (oidx & 3) {
          headSize = 4 - (oidx & 3);
          assert(headSize < out_area);
          for (size_t i = 0; i < headSize; ++i) {
            _mm_store_sd(&pa[i], _mm256_castpd256_pd128(b2));
          }
        }
        size_t cnt  = (out_area - headSize) / 8;
        double *pa2 = pa + headSize;
        for (size_t i = 0; i < cnt; ++i) {
          _mm256_store_pd(&pa2[i * 8 + 0], b2);
          _mm256_store_pd(&pa2[i * 8 + 4], b2);
        }
        for (size_t i = headSize + cnt * 8; i < out_area; ++i) {
          _mm_store_sd(&pa[i], _mm256_castpd256_pd128(b2));
        }
      }

      for (serial_size_t inc = 0; inc < params.in.depth_; ++inc) {
        if (!tbl.is_connected(o, inc)) continue;

        const double *pw = &w_begin[25 * (params.in.depth_ * o + inc)];
        const double *pi = &in_begin[in_padded.get_index(0, 0, inc)];

        __m256d w0a = _mm256_loadu_pd(pw + 0);
        __m128d w0b = _mm_load_sd(pw + 4);
        __m256d w1a = _mm256_loadu_pd(pw + 5);
        __m128d w1b = _mm_load_sd(pw + 9);
        __m256d w2a = _mm256_loadu_pd(pw + 10);
        __m128d w2b = _mm_load_sd(pw + 14);
        __m256d w3a = _mm256_loadu_pd(pw + 15);
        __m128d w3b = _mm_load_sd(pw + 19);
        __m256d w4a = _mm256_loadu_pd(pw + 20);
        __m128d w4b = _mm_load_sd(pw + 24);

        double *ppa = pa;
        for (serial_size_t y = 0; y < out.height_;
             ++y, pi += in_stride, ppa += out.width_) {
          const double *pi0 = pi + 0 * in_padded.width_;
          const double *pi1 = pi + 1 * in_padded.width_;
          const double *pi2 = pi + 2 * in_padded.width_;
          const double *pi3 = pi + 3 * in_padded.width_;
          const double *pi4 = pi + 4 * in_padded.width_;
          for (serial_size_t x = 0; x < out.width_; ++x) {
            __m128d sum   = _mm_load_sd(&ppa[x]);
            __m256d i0a   = _mm256_loadu_pd(pi0);
            __m128d i0b   = _mm_load_sd(pi0 + 4);
            __m256d i1a   = _mm256_loadu_pd(pi1);
            __m128d i1b   = _mm_load_sd(pi1 + 4);
            __m256d i2a   = _mm256_loadu_pd(pi2);
            __m128d i2b   = _mm_load_sd(pi2 + 4);
            __m256d i3a   = _mm256_loadu_pd(pi3);
            __m128d i3b   = _mm_load_sd(pi3 + 4);
            __m256d i4a   = _mm256_loadu_pd(pi4);
            __m128d i4b   = _mm_load_sd(pi4 + 4);
            __m256d sum_a = _mm256_mul_pd(w0a, i0a);
            __m128d sum_b = _mm_mul_sd(w0b, i0b);
            sum_a         = madd256_pd(w1a, i1a, sum_a);
            sum_b         = madd128_pd(w1b, i1b, sum_b);
            sum_a         = madd256_pd(w2a, i2a, sum_a);
            sum_b         = madd128_pd(w2b, i2b, sum_b);
            sum_a         = madd256_pd(w3a, i3a, sum_a);
            sum_b         = madd128_pd(w3b, i3b, sum_b);
            sum_a         = madd256_pd(w4a, i4a, sum_a);
            sum_b         = madd128_pd(w4b, i4b, sum_b);
            __m128d sum_c = hsum256_pd(sum_a);
            sum           = _mm_add_sd(sum, sum_b);
            _mm_store_sd(&ppa[x], _mm_add_sd(sum, sum_c));
            pi0 += w_stride;
            pi1 += w_stride;
            pi2 += w_stride;
            pi3 += w_stride;
            pi4 += w_stride;
          }  // x loop
        }    // y loop
      }      // in depth loop
    }        // out depth loop
  }          // else
}  // avx_conv2d_5x5_kernel double ver

#endif  // CNN_USE_AVX
template <typename S1, typename S2, typename S3, typename S4>
inline void conv2d_op_avx(Tensor<float_t, S1> &in_data,
                          const Tensor<float_t, S2> &weights,
                          const Tensor<float_t, S3> &bias,
                          Tensor<float_t, S4> &out_data,
                          const core::conv_params &params,
                          const bool layer_parallelize) {
#ifdef CNN_USE_AVX
  if (params.weight.height_ == 5 && params.weight.width_ == 5) {
    // @todo consider better parallelization
    for_i(layer_parallelize, in_data.shape()[0], [&](size_t i) {
      auto in  = in_data.subView({i, i + 1ull}, {0ull, in_data.shape()[1]});
      auto out = out_data.subView({i, i + 1ull}, {0ull, out_data.shape()[1]});
      avx_conv2d_5x5_kernel(params, in, weights, bias, out, layer_parallelize);
    });
    return;
  }
#endif
  conv2d_op_internal(in_data, weights, bias, out_data, params,
                     layer_parallelize);
}

}  // namespace kernels
}  // namespace tiny_dnn
