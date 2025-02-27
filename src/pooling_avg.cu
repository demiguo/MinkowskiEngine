/* Copyright (c) Chris Choy (chrischoy@ai.stanford.edu).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Please cite "4D Spatio-Temporal ConvNets: Minkowski Convolutional Neural
 * Networks", CVPR'19 (https://arxiv.org/abs/1904.08755) if you use any part
 * of the code.
 */
#ifndef GPU_POOLING_AVG
#define GPU_POOLING_AVG

#include <limits>

#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/fill.h>
#include <thrust/host_vector.h>

#include <thrust/functional.h>
#include <thrust/iterator/discard_iterator.h>
#include <thrust/reduce.h>
#include <thrust/sort.h>

#include "gpu.cuh"
#include "pooling_avg.cuh"
#include "utils.hpp"

template <typename Dtype>
__global__ void fill(const int n, Dtype *in_feat, Dtype val) {
  CUDA_KERNEL_LOOP(index, n) { in_feat[index] = val; }
}

template <typename Dtype>
__global__ void col2row_major(const int n, const int nrows, const int ncols,
                              const Dtype *colA, Dtype *rowA) {
  int i, j;
  CUDA_KERNEL_LOOP(index, n) {
    i = index % nrows;
    j = index / nrows;
    rowA[i * ncols + j] = colA[index];
  }
}

template <typename Dtype>
__global__ void col2row_major_with_div(const int n, const int nrows,
                                       const int ncols,
                                       const Dtype *num_nonzero,
                                       const Dtype *colA, Dtype *rowA) {
  int i, j;
  CUDA_KERNEL_LOOP(index, n) {
    i = index % nrows;
    j = index / nrows;
    if (num_nonzero[i]) {
      rowA[i * ncols + j] = colA[index] / num_nonzero[i];
    } else {
      rowA[i * ncols + j] = colA[index];
    }
  }
}

template <typename Dtype, typename Itype>
__global__ void set_gradient(const int n, const Dtype *d_grad_out,
                             Dtype *d_grad_in, const Itype *out_index,
                             int nchannel) {
  CUDA_KERNEL_LOOP(index, n) {
    atomicAdd(&d_grad_in[out_index[index]], d_grad_out[index]);
  }
}

template <typename Dtype, typename Itype>
__global__ void
set_gradient_nonzero(const int n, const Dtype *d_grad_out, Dtype *d_grad_in,
                     int nchannel, const Itype *in_map, const Itype *out_map) {
  CUDA_KERNEL_LOOP(index, n) {
    int nrow = index / nchannel;
    int ch = index % nchannel;
    atomicAdd(&d_grad_in[in_map[nrow] * nchannel + ch],
              d_grad_out[out_map[nrow] * nchannel + ch]);
  }
}

template <typename Dtype, typename Itype>
__global__ void
set_gradient_nonzero_avg(const int n, const Dtype *d_grad_out, Dtype *d_grad_in,
                         int nchannel, const Dtype *d_num_nonzero,
                         const Itype *in_map, const Itype *out_map) {
  CUDA_KERNEL_LOOP(index, n) {
    int nrow = index / nchannel;
    int ch = index % nchannel;
    int curr_num_nonzero = d_num_nonzero[out_map[nrow]];
    if (curr_num_nonzero > 0)
      atomicAdd(&d_grad_in[in_map[nrow] * nchannel + ch],
                d_grad_out[out_map[nrow] * nchannel + ch] / curr_num_nonzero);
  }
}

template <typename Dtype, typename Itype>
void NonzeroAvgPoolingForwardKernelGPU(
    const Dtype *d_in_feat, int in_nrows, Dtype *d_out_feat, int out_nrows,
    Dtype *d_num_nonzero, int nchannel,
    const std::vector<std::vector<Itype>> &in_maps,
    const std::vector<std::vector<Itype>> &out_maps, bool use_avg, Itype *d_scr,
    cusparseHandle_t cushandle, cudaStream_t stream) {
  int nnz = 0;
  const Dtype alpha = 1;
  const Dtype beta = 0;
  cusparseMatDescr_t descr = 0;
  Itype *d_in_map, *d_out_map, *d_csr_row;
  Dtype *d_ones, *d_csr_val, *d_tmp_out_feat;

  // Copy all maps to one vector
  for (const auto &map : in_maps)
    nnz += map.size();

  // CUDA_CHECK(cudaMalloc((void **)&d_in_map,
  //                       (2 * nnz + out_nrows + 1) * sizeof(Itype)));
  d_in_map = d_scr;
  d_out_map = d_in_map + nnz;
  d_csr_row = d_out_map + nnz;

  Itype *d_in_map_iter = d_in_map, *d_out_map_iter = d_out_map;
  for (int k = 0; k < in_maps.size(); k++) {
    int curr_n = in_maps[k].size();
    if (curr_n > 0) {
      CUDA_CHECK(cudaMemcpy(d_in_map_iter, in_maps[k].data(),
                            sizeof(Itype) * curr_n, cudaMemcpyHostToDevice));
      CUDA_CHECK(cudaMemcpy(d_out_map_iter, out_maps[k].data(),
                            sizeof(Itype) * curr_n, cudaMemcpyHostToDevice));
      d_in_map_iter += curr_n;
      d_out_map_iter += curr_n;
    }
  }

  d_ones = (Dtype*)(d_scr) + 2 * nnz + out_nrows + 1;
  if (use_avg) {
    // CUDA_CHECK(
    //     cudaMalloc((void **)&d_ones,
    //                (in_nrows + nnz + nchannel * out_nrows) * sizeof(Dtype)));
    d_csr_val = d_ones + in_nrows;
    d_tmp_out_feat = d_csr_val + nnz;
    fill<Dtype><<<GET_BLOCKS(in_nrows), CUDA_NUM_THREADS, 0, stream>>>(
        in_nrows, d_ones, (Dtype)1.);
  } else {
    // CUDA_CHECK(cudaMalloc((void **)&d_ones,
    //                       (nnz + nchannel * out_nrows) * sizeof(Dtype)));
    d_csr_val = d_ones;
    d_tmp_out_feat = d_csr_val + nnz;
    fill<Dtype><<<GET_BLOCKS(nnz), CUDA_NUM_THREADS, 0, stream>>>(
        nnz, d_csr_val, (Dtype)1.);
  }

  CUSPARSE_CHECK(cusparseCreateMatDescr(&descr));
  cusparseSetMatType(descr, CUSPARSE_MATRIX_TYPE_GENERAL);
  cusparseSetMatIndexBase(descr, CUSPARSE_INDEX_BASE_ZERO);

  // Sort COO first
  sort_coo_gpu(cushandle, out_nrows, in_nrows, nnz, d_out_map, d_in_map);

  // For CRS, sort row and col inds by row major.
  CUSPARSE_CHECK(cusparseXcoo2csr(cushandle, d_out_map, nnz, out_nrows,
                                  d_csr_row, CUSPARSE_INDEX_BASE_ZERO));

  CUSPARSE_CHECK(
      cusparse_csrmm<Dtype>(cushandle,
                            CUSPARSE_OPERATION_NON_TRANSPOSE, // op(A)
                            CUSPARSE_OPERATION_TRANSPOSE,     // op(B)
                            out_nrows,                        // M
                            nchannel,                         // N
                            in_nrows,                         // K
                            nnz, &alpha, descr,
                            d_csr_val, // val
                            d_csr_row, // row
                            d_in_map,  // col
                            d_in_feat, // B
                            nchannel,  // ldb
                            &beta,
                            d_tmp_out_feat, // C
                            out_nrows       // ldc
                            ));

  if (use_avg) {
    CUSPARSE_CHECK(
        cusparse_csrmv<Dtype>(cushandle,
                              CUSPARSE_OPERATION_NON_TRANSPOSE, // op(A)
                              out_nrows,                        // M
                              in_nrows,                         // K
                              nnz, &alpha, descr,
                              d_csr_val, // val
                              d_csr_row, // row
                              d_in_map,  // col
                              d_ones,    // B (in_nrows > out_nrows)
                              &beta,
                              d_num_nonzero)); // C

    col2row_major_with_div<Dtype>
        <<<GET_BLOCKS(out_nrows * nchannel), CUDA_NUM_THREADS, 0, stream>>>(
            out_nrows * nchannel, out_nrows, nchannel, d_num_nonzero,
            d_tmp_out_feat, d_out_feat);
  } else {
    col2row_major<Dtype>
        <<<GET_BLOCKS(out_nrows * nchannel), CUDA_NUM_THREADS, 0, stream>>>(
            out_nrows * nchannel, out_nrows, nchannel, d_tmp_out_feat,
            d_out_feat);
  }

  CUSPARSE_CHECK(cusparseDestroyMatDescr(descr));
  // cudaFree(d_in_map);
  // cudaFree(d_ones);
}

template void NonzeroAvgPoolingForwardKernelGPU<float, int32_t>(
    const float *d_in_feat, int in_nrows, float *d_out_feat, int out_nrows,
    float *d_num_nonzero, int nchannel,
    const std::vector<std::vector<int32_t>> &in_map,
    const std::vector<std::vector<int32_t>> &out_map, bool use_avg,
    int32_t *d_scr, cusparseHandle_t cushandle, cudaStream_t stream);

template void NonzeroAvgPoolingForwardKernelGPU<double, int32_t>(
    const double *d_in_feat, int in_nrows, double *d_out_feat, int out_nrows,
    double *d_num_nonzero, int nchannel,
    const std::vector<std::vector<int32_t>> &in_map,
    const std::vector<std::vector<int32_t>> &out_map, bool use_avg,
    int32_t *d_scr, cusparseHandle_t cushandle, cudaStream_t stream);

template <typename Dtype, typename Itype>
void NonzeroAvgPoolingBackwardKernelGPU(
    Dtype *d_grad_in_feat, int in_nrows, const Dtype *d_grad_out_feat,
    int out_nrows, const Dtype *d_num_nonzero, int nchannel,
    const std::vector<std::vector<Itype>> &in_maps,
    const std::vector<std::vector<Itype>> &out_maps, bool use_avg,
    Itype *d_scr, cudaStream_t stream) {
  int nnz = 0;
  Itype *d_in_map, *d_out_map;
  // Copy all maps to one vector
  for (const auto &map : in_maps)
    nnz += map.size();

  // CUDA_CHECK(cudaMalloc((void **)&d_in_map, 2 * nnz * sizeof(Itype)));
  d_in_map = d_scr;
  d_out_map = d_in_map + nnz;

  // Cleanup gradients
  CUDA_CHECK(
      cudaMemset(d_grad_in_feat, 0, in_nrows * nchannel * sizeof(Dtype)));

  Itype *d_in_map_iter = d_in_map, *d_out_map_iter = d_out_map;
  for (int k = 0; k < in_maps.size(); k++) {
    int curr_n = in_maps[k].size();
    if (curr_n > 0) {
      CUDA_CHECK(cudaMemcpy(d_in_map_iter, in_maps[k].data(),
                            sizeof(Itype) * curr_n, cudaMemcpyHostToDevice));
      CUDA_CHECK(cudaMemcpy(d_out_map_iter, out_maps[k].data(),
                            sizeof(Itype) * curr_n, cudaMemcpyHostToDevice));
      d_in_map_iter += curr_n;
      d_out_map_iter += curr_n;
    }
  }

  if (use_avg) {
    set_gradient_nonzero_avg<Dtype>
        <<<GET_BLOCKS(nnz * nchannel), CUDA_NUM_THREADS, 0, stream>>>(
            nnz * nchannel, d_grad_out_feat, d_grad_in_feat, nchannel,
            d_num_nonzero, d_in_map, d_out_map);
  } else {
    set_gradient_nonzero<Dtype>
        <<<GET_BLOCKS(nnz * nchannel), CUDA_NUM_THREADS, 0, stream>>>(
            nnz * nchannel, d_grad_out_feat, d_grad_in_feat, nchannel, d_in_map,
            d_out_map);
  }

  // cudaFree(d_in_map);
}

template void NonzeroAvgPoolingBackwardKernelGPU<float, int32_t>(
    float *d_grad_in_feat, int in_nrows, const float *d_grad_out_feat,
    int out_nrows, const float *d_num_nonzero, int nchannel,
    const std::vector<std::vector<int32_t>> &in_map,
    const std::vector<std::vector<int32_t>> &out_map, bool use_avg,
    int32_t *d_scr, cudaStream_t stream);

template void NonzeroAvgPoolingBackwardKernelGPU<double, int32_t>(
    double *d_grad_in_feat, int in_nrows, const double *d_grad_out_feat,
    int out_nrows, const double *d_num_nonzero, int nchannel,
    const std::vector<std::vector<int32_t>> &in_map,
    const std::vector<std::vector<int32_t>> &out_map, bool use_avg,
    int32_t *d_scr, cudaStream_t stream);
#endif
