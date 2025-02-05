/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * Copyright (c) 2018 by Contributors
 * \file nhwc_batch_norm.h
 * \brief CUDA NHWC Batch Normalization code
 * \author Shankara Rao Thejaswi Nanditale, Dick Carter
*/
#ifndef MXNET_OPERATOR_NN_CUDNN_NHWC_BATCH_NORM_H_
#define MXNET_OPERATOR_NN_CUDNN_NHWC_BATCH_NORM_H_

#include <cudnn.h>

#include <algorithm>
#include <vector>
#include <string>

#include "nhwc_batch_norm_kernel.h"
#include "../../../common/cuda_utils.h"


#define VERBOSE_DEFAULT false

class NhwcBatchNorm {
 public:
  NhwcBatchNorm() {
    name_ = "nhwc_batchnorm";
    createTensorDescriptor(&X_tensor_desc_);
    createTensorDescriptor(&Y_tensor_desc_);
  }

  ~NhwcBatchNorm() {
    destroyTensorDescriptor(X_tensor_desc_);
    destroyTensorDescriptor(Y_tensor_desc_);
  }

  void die() {
    std::cerr << "batchnorm not initialized" << std::endl;
    exit(-1);
  }

  void fwd(cudaStream_t stream, bool use_relu, int device_id);
  void dgrad(cudaStream_t stream, bool use_relu, int device_id);
  void fwdInference(cudaStream_t stream, bool use_relu);
  dim3 calc_fwd_grid(int device_id, int *loop);
  dim3 calc_bwd_grid(int device_id, int *loop);

  void setInputDescriptor(const cudnnTensorFormat_t format,
                                  const cudnnDataType_t     data_type,
                                  int n, int c, int h, int w) {
    m_ = n * h * w;
    c_ = c;
    setTensorDescriptor(X_tensor_desc_, format, data_type, n, c, h, w);
  }

  void setOutputDescriptor(const cudnnTensorFormat_t format,
                                   const cudnnDataType_t     data_type,
                                   int n, int c, int h, int w) {
    setTensorDescriptor(Y_tensor_desc_, format, data_type, n, c, h, w);
  }

  const std::vector<size_t> numWorkspaceBytes() const;

  void setWorkspacePointers(
      const std::vector<void*>&  workspace,
      const std::vector<size_t>& num_workspace_bytes);

  void setInputOutputPointers(void* X, void* dX, void* Y, void *dY) {
    X_ = X;
    dX_  = dX;
    Y_   = Y;
    dY_  = dY;
  }

  // Sets the pointers for the scale and weight (in that order) data and derivative buffers.
  void setWeightPointers(const std::vector<void*>& weight_pointers,
                                 const std::vector<void*>& deriv_pointers) {
    assert(weight_pointers.size() == 2);
    assert(deriv_pointers.size()  == 2);
    scale_  = static_cast<float*>(weight_pointers[0]);
    bias_   = static_cast<float*>(weight_pointers[1]);
    dscale_ = static_cast<float*>(deriv_pointers[0]);
    dbias_  = static_cast<float*>(deriv_pointers[1]);
  }

  // Sets the pointers for the population mean and variance buffers, in that order.
  void setParameterPointers(const std::vector<void*>& param_pointers) {
    assert(param_pointers.size() == 2);
    population_mean_     = static_cast<float*>(param_pointers[0]);
    population_variance_ = static_cast<float*>(param_pointers[1]);
  }

  void setConstants(const double exp_avg_factor, const double eps) {
    exp_avg_factor_ = exp_avg_factor;
    eps_ = eps;
  }

  void processCudnnStatus(const cudnnStatus_t& status,
                          const std::string& string = std::string(),
                          bool verbose = VERBOSE_DEFAULT) {
    if (status != CUDNN_STATUS_SUCCESS)
      LOG(FATAL) << string << " " << cudnnGetErrorString(status);
    else if (verbose)
      LOG(INFO) << string << " " << cudnnGetErrorString(status);
  }

  void checkCudaStatus(const std::string& string = std::string(),
                       bool verbose = VERBOSE_DEFAULT) {
    cudaError_t status = cudaGetLastError();
    if (status != cudaSuccess)
      LOG(FATAL) << string << " " << cudaGetErrorString(status);
    else if (verbose)
      LOG(INFO) << string << " " << cudaGetErrorString(status);
  }

  size_t size_retired_ctas(int grid_y) const {
    // Note that the value of max_grid_y to handle known GPUs is about 160.
    const int max_grid_y = 1024;
    if (grid_y > max_grid_y)
      LOG(INFO) << "GPU capabilities exceeds assumptions.";
    const int retired_cta_bytes = max_grid_y * 2 * sizeof(int);
    // Since the region will be initialized once and used for many kernels,
    // the idea is to return an ample size that will cover all uses.
    return retired_cta_bytes;
  }

  cudnnTensorDescriptor_t  X_tensor_desc_ = nullptr;
  cudnnTensorDescriptor_t  Y_tensor_desc_ = nullptr;

  void*  X_ = nullptr;
  void* dX_ = nullptr;
  void*  Y_ = nullptr;
  void* dY_ = nullptr;

  // Learned scale and bias weights.
  float* scale_  = nullptr;
  float* dscale_ = nullptr;
  float* bias_   = nullptr;
  float* dbias_  = nullptr;

  // Computed population mean and variance parameters.
  float* population_mean_     = nullptr;
  float* population_variance_ = nullptr;

  // Workspace buffers for minibatch mean and variance (computed in fwd, needed by bwd).
  float* minibatch_mean_     = nullptr;
  float* minibatch_variance_ = nullptr;

  int m_ = 0;  // Number of values per channel that BN is normalizing.
  int c_ = 0;  // Number of channels over which BN is normalizing.

  double exp_avg_factor_ = 0.;
  double eps_            = 0.;
  std::string name_;

 private:
  void setTensorDescriptor(cudnnTensorDescriptor_t descriptor,
                           cudnnTensorFormat_t format,
                           cudnnDataType_t     data_type,
                           int n, int c, int h, int w) {
    cudnnStatus_t status = CUDNN_STATUS_SUCCESS;
    status = cudnnSetTensor4dDescriptor(descriptor, format, data_type, n, c, h, w);
    processCudnnStatus(status, "set tensor descriptor");
  }

  void createTensorDescriptor(cudnnTensorDescriptor_t *descriptor) {
    cudnnStatus_t status = CUDNN_STATUS_SUCCESS;
    status = cudnnCreateTensorDescriptor(descriptor);
    processCudnnStatus(status, "create tensor_descriptor");
  }

  void destroyTensorDescriptor(cudnnTensorDescriptor_t descriptor) {
    cudnnStatus_t status = CUDNN_STATUS_SUCCESS;
    status = cudnnDestroyTensorDescriptor(descriptor);
    processCudnnStatus(status, "destroy tensor_descriptor");
  }

 protected:
  float *partial_sums_ = nullptr;
  int *partial_counts_ = nullptr;
  int *retired_ctas_   = nullptr;

  void _setFwdParams(NhwcBatchNormFwdParams *params) const;
  void _setFwdInferenceParams(NhwcBatchNormFwdInferenceParams *params) const;
  void _setBwdParams(NhwcBatchNormBwdParams *params) const;

  // @todo: ability to configure these?
  // Kernel params
  static const int USE_ONLINE_APPROACH = 1;
  static const int THREADS_PER_CTA = 512;
  static const int THREADS_PER_PIXEL = 16;
  static const int C_ELEMENTS_PER_CTA = 64;
  static const int ELEMENTS_PER_LDG = C_ELEMENTS_PER_CTA / THREADS_PER_PIXEL;
  static const int MAX_SMEM_WITHOUT_OPT_IN = 48 * 1024;

  typedef uint16_t StorageType;
  // increasing this to 6 causes spills in fwd kernel!
  static const int PIXELS_PER_THREAD_IN_REGISTERS_FWD = 5;
  static const int PIXELS_PER_THREAD_IN_REGISTERS_BWD = 3;
  static const int PIXELS_PER_THREAD_IN_SMEM_FWD = 10;
  static const int PIXELS_PER_THREAD_IN_SMEM_BWD = 5;

  static const int PIXELS_PER_THREAD_FWD = PIXELS_PER_THREAD_IN_REGISTERS_FWD + \
      PIXELS_PER_THREAD_IN_SMEM_FWD;
  static const int PIXELS_PER_THREAD_BWD = PIXELS_PER_THREAD_IN_REGISTERS_BWD + \
      PIXELS_PER_THREAD_IN_SMEM_BWD;
  static const int PIXELS_PER_THREAD_FWD_INFERENCE = 4;

  // Derived params
  static const size_t SMEM_SIZE_FWD = PIXELS_PER_THREAD_IN_SMEM_FWD*THREADS_PER_CTA*\
      ELEMENTS_PER_LDG*sizeof(uint16_t);
  static const size_t SMEM_SIZE_BWD = PIXELS_PER_THREAD_IN_SMEM_BWD*THREADS_PER_CTA*\
      ELEMENTS_PER_LDG*2*sizeof(uint16_t);
  static const int PIXELS_PER_LDG = THREADS_PER_CTA / THREADS_PER_PIXEL;
  static const int PIXELS_PER_CTA_FWD = THREADS_PER_CTA/THREADS_PER_PIXEL * \
      PIXELS_PER_THREAD_FWD;
  static const int PIXELS_PER_CTA_BWD = THREADS_PER_CTA/THREADS_PER_PIXEL * \
      PIXELS_PER_THREAD_BWD;
  static const int PIXELS_PER_CTA_FWD_INFERENCE = THREADS_PER_CTA/THREADS_PER_PIXEL * \
      PIXELS_PER_THREAD_FWD_INFERENCE;

  // Required CTA margin for cooperative kernel launch on cuda arch 70 and later.
  static const size_t LAUNCH_MARGIN = 16;

  // Helper function to launch the forward kernel.

  // We calculate (based on smem usage) the achievable occupancy and make sure we run a kernel
  // version that was compiled with that occupancy in its launch bounds.  This way, we avoid
  // needless register spills.
  void _fwdKernelLauncher(cudaStream_t stream, NhwcBatchNormFwdParams params,
                                dim3 grid_dim, int outer_loops, bool use_relu, int device_id) {
#define LAUNCH_FWD_KERNEL(OUTER_LOOPS, USE_RELU, USE_ADD_RELU, COMPILED_FOR_OCCUPANCY) \
    do { \
        CHECK(SMEM_SIZE_FWD <= MAX_SMEM_WITHOUT_OPT_IN) << "Nhwc batchnorm kernel smem too big."; \
        auto fwd_func = nhwc_batch_norm_fwd< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_FWD, \
                        PIXELS_PER_THREAD_IN_SMEM_FWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        USE_RELU, \
                        USE_ADD_RELU, \
                        COMPILED_FOR_OCCUPANCY>; \
        cudaFuncSetAttribute(fwd_func, cudaFuncAttributePreferredSharedMemoryCarveout, 100); \
        checkCudaStatus(name_ + " fwd ser coop kernel (cudaFuncSetAttribute carveout)"); \
        void *params_ptr = static_cast<void*>(&params); \
        using FWD_FUNC = decltype(nhwc_batch_norm_fwd< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_FWD, \
                        PIXELS_PER_THREAD_IN_SMEM_FWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        USE_RELU, \
                        USE_ADD_RELU, \
                        COMPILED_FOR_OCCUPANCY>); \
        cudaLaunchCooperativeKernel<FWD_FUNC>(fwd_func, \
            grid_dim, \
            THREADS_PER_CTA, \
            &params_ptr, \
            SMEM_SIZE_FWD, \
            stream); \
        checkCudaStatus(name_ + " fwd ser coop kernel"); \
    } while (0)

    // Don't try for an occupancy > 2 as this will squeeze register use and create spills.
    int occupancy = smem_driven_fwd_occupancy(device_id);
    if (outer_loops == 1 && use_relu) {
      if (occupancy >= 2)
        LAUNCH_FWD_KERNEL(1, true, false, 2);
      else
        LAUNCH_FWD_KERNEL(1, true, false, 1);
    } else if (outer_loops == 1 && !use_relu) {
      if (occupancy >= 2)
        LAUNCH_FWD_KERNEL(1, false, false, 2);
      else
        LAUNCH_FWD_KERNEL(1, false, false, 1);
    } else if (use_relu) {
      if (occupancy >= 2)
        LAUNCH_FWD_KERNEL(0, true, false, 2);
      else
        LAUNCH_FWD_KERNEL(0, true, false, 1);
    } else {
      if (occupancy >= 2)
        LAUNCH_FWD_KERNEL(0, false, false, 2);
      else
        LAUNCH_FWD_KERNEL(0, false, false, 1);
    }
#undef LAUNCH_FWD_KERNEL
  }

  // Helper function to launch the backward kernel.

  void _bwdKernelLauncher(cudaStream_t stream, NhwcBatchNormBwdParams params,
                                dim3 grid_dim, int outer_loops, bool use_relu, int device_id) {
#define LAUNCH_BWD_KERNEL(OUTER_LOOPS, COMPILED_FOR_OCCUPANCY) \
    do { \
        CHECK(SMEM_SIZE_BWD <= MAX_SMEM_WITHOUT_OPT_IN) << "Nhwc batchnorm kernel smem too big."; \
        auto bwd_func = nhwc_batch_norm_bwd< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_BWD, \
                        PIXELS_PER_THREAD_IN_SMEM_BWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        COMPILED_FOR_OCCUPANCY>; \
        cudaFuncSetAttribute(bwd_func, cudaFuncAttributePreferredSharedMemoryCarveout, 100); \
        checkCudaStatus(name_ + " bwd coop serial kernel (cudaFuncSetAttribute carveout)"); \
        void *params_ptr = static_cast<void*>(&params); \
        using BWD_FUNC = decltype(nhwc_batch_norm_bwd< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_BWD, \
                        PIXELS_PER_THREAD_IN_SMEM_BWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        COMPILED_FOR_OCCUPANCY>); \
        cudaLaunchCooperativeKernel<BWD_FUNC>(bwd_func, \
            grid_dim, \
            THREADS_PER_CTA, \
            &params_ptr, \
            SMEM_SIZE_BWD, \
            stream); \
        checkCudaStatus(name_ + " bwd coop serial kernel"); \
    } while (0)

#define LAUNCH_BWD_RELU_KERNEL(OUTER_LOOPS, COMPILED_FOR_OCCUPANCY) \
    do { \
        CHECK(SMEM_SIZE_BWD <= MAX_SMEM_WITHOUT_OPT_IN) << "Nhwc batchnorm kernel smem too big."; \
        auto bwd_relu_func = nhwc_batch_norm_bwd_relu< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_BWD, \
                        PIXELS_PER_THREAD_IN_SMEM_BWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        COMPILED_FOR_OCCUPANCY>; \
        cudaFuncSetAttribute(bwd_relu_func, cudaFuncAttributePreferredSharedMemoryCarveout, 100); \
        checkCudaStatus(name_ + " bwd-relu coop serial kernel (cudaFuncSetAttribute carveout)"); \
        void *params_ptr = static_cast<void*>(&params); \
        using BWD_RELU_FUNC = decltype(nhwc_batch_norm_bwd_relu< \
                        StorageType, \
                        THREADS_PER_CTA, \
                        THREADS_PER_PIXEL, \
                        PIXELS_PER_THREAD_IN_REGISTERS_BWD, \
                        PIXELS_PER_THREAD_IN_SMEM_BWD, \
                        ELEMENTS_PER_LDG, \
                        USE_ONLINE_APPROACH, \
                        OUTER_LOOPS, \
                        COMPILED_FOR_OCCUPANCY>); \
        cudaLaunchCooperativeKernel<BWD_RELU_FUNC>(bwd_relu_func, \
            grid_dim, \
            THREADS_PER_CTA, \
            &params_ptr, \
            SMEM_SIZE_BWD, \
            stream); \
        checkCudaStatus(name_ + " bwd-relu coop serial kernel"); \
    } while (0)

    // Don't try for an occupancy > 2 as this will squeeze register use and create spills.
    int occupancy = smem_driven_bwd_occupancy(device_id);
    if (outer_loops == 1 && use_relu) {
      if (occupancy >= 2)
        LAUNCH_BWD_RELU_KERNEL(1, 2);
      else
        LAUNCH_BWD_RELU_KERNEL(1, 1);
    } else if (outer_loops == 1 && !use_relu) {
      if (occupancy >= 2)
        LAUNCH_BWD_KERNEL(1, 2);
      else
        LAUNCH_BWD_KERNEL(1, 1);
    } else if (use_relu) {
      if (occupancy >= 2)
        LAUNCH_BWD_RELU_KERNEL(0, 2);
      else
        LAUNCH_BWD_RELU_KERNEL(0, 1);
    } else {
      if (occupancy >= 2)
        LAUNCH_BWD_KERNEL(0, 2);
      else
        LAUNCH_BWD_KERNEL(0, 1);
    }
#undef LAUNCH_BWD_KERNEL
  }

 private:
  // Calculate the max number of CTAs allowed in the grid for the fwd kernel.
  static size_t max_fwd_grid_x(int device_id) {
    using namespace mxnet::common::cuda;
    int answer = MultiprocessorCount(device_id) * smem_driven_fwd_occupancy(device_id);
    if (SMArch(device_id) >= 70)
      answer -= LAUNCH_MARGIN;
    return static_cast<size_t>(answer);
  }

  // Calculate the max number of CTAs allowed in the grid for the bwd kernel.
  static size_t max_bwd_grid_x(int device_id) {
    using namespace mxnet::common::cuda;
    int answer = MultiprocessorCount(device_id) * smem_driven_bwd_occupancy(device_id);
    if (SMArch(device_id) >= 70)
      answer -= LAUNCH_MARGIN;
    return static_cast<size_t>(answer);
  }

  // Calculate the expected fwd kernel occupancy, as dictated by shared memory usage.
  static int smem_driven_fwd_occupancy(int device_id) {
    using namespace mxnet::common::cuda;
    int fwd_reduction_bytes = THREADS_PER_PIXEL*(THREADS_PER_CTA/32)*ELEMENTS_PER_LDG*sizeof(float);
    int fwd_smem_bytes = SMEM_SIZE_FWD + fwd_reduction_bytes;
    int occupancy = MaxSharedMemoryPerMultiprocessor(device_id) / fwd_smem_bytes;
    return occupancy;
  }

  // Calculate the expected bwd kernel occupancy, as dictated by shared memory usage.
  static int smem_driven_bwd_occupancy(int device_id) {
    using namespace mxnet::common::cuda;
    int bwd_reduction_bytes = THREADS_PER_PIXEL*(THREADS_PER_CTA/32)*ELEMENTS_PER_LDG*sizeof(float);
    int bwd_smem_bytes = SMEM_SIZE_BWD + bwd_reduction_bytes;
    int occupancy = MaxSharedMemoryPerMultiprocessor(device_id) / bwd_smem_bytes;
    return occupancy;
  }
};

const std::vector<size_t> NhwcBatchNorm::numWorkspaceBytes() const {
  assert(c_ > 0);

  // choose the max memory required between fwd/bwd passes
  int grid_x_fwd = div_up(m_, PIXELS_PER_CTA_FWD);
  int grid_x_bwd = div_up(m_, PIXELS_PER_CTA_BWD);
  int grid_x = max(grid_x_fwd, grid_x_bwd);
  int grid_y = div_up(c_, C_ELEMENTS_PER_CTA);

  const size_t num_mean_bytes     = c_ * sizeof(float);
  const size_t num_variance_bytes = num_mean_bytes;
  const size_t size_sums          = grid_y*grid_x*THREADS_PER_PIXEL*\
      ELEMENTS_PER_LDG*2*sizeof(float);
  const size_t size_counts        = grid_y*grid_x*sizeof(int);

  return {num_mean_bytes, num_variance_bytes,
          size_retired_ctas(grid_y), size_sums, size_counts};
}

void NhwcBatchNorm::setWorkspacePointers(
      const std::vector<void*>& workspace,
      const std::vector<size_t>& num_workspace_bytes) {
  assert(workspace.size() == 5);
  assert(num_workspace_bytes.size() == 5);

  minibatch_mean_     = static_cast<float*>(workspace[0]);
  minibatch_variance_ = static_cast<float*>(workspace[1]);
  retired_ctas_       = static_cast<int*>(workspace[2]);
  partial_sums_       = static_cast<float*>(workspace[3]);
  partial_counts_     = static_cast<int*>(workspace[4]);
}

void NhwcBatchNorm::_setFwdParams(NhwcBatchNormFwdParams *params) const {
  params->gmem_src          = static_cast<uint16_t*>(X_);
  params->gmem_dst          = static_cast<uint16_t*>(Y_);
  params->gmem_src1         = nullptr;
  params->gmem_bias         = bias_;
  params->gmem_scale        = scale_;
  params->gmem_running_mean = population_mean_;
  params->gmem_running_var  = population_variance_;
  params->gmem_saved_mean   = minibatch_mean_;
  params->gmem_saved_var    = minibatch_variance_;
  params->gmem_relu_bitmask = nullptr;
  params->nhw               = m_;
  params->c                 = c_;
  params->gmem_sums         = partial_sums_;
  params->gmem_counts       = partial_counts_;
  params->gmem_retired_ctas = retired_ctas_;
  params->var_eps           = eps_;
  params->outer_loops       = 0;
  params->exp_avg_factor    = static_cast<float>(exp_avg_factor_);
  params->c_blks            = div_up(c_, C_ELEMENTS_PER_CTA);
}

void NhwcBatchNorm::_setFwdInferenceParams(NhwcBatchNormFwdInferenceParams
                                                        *params) const {
  params->gmem_src   = static_cast<uint16_t*>(X_);
  params->gmem_dst   = static_cast<uint16_t*>(Y_);
  params->gmem_src1  = nullptr;
  params->gmem_bias  = bias_;
  params->gmem_scale = scale_;
  params->gmem_mean  = population_mean_;
  params->gmem_var   = population_variance_;
  params->nhw        = m_;
  params->c          = c_;
  params->var_eps    = eps_;
}

void NhwcBatchNorm::_setBwdParams(NhwcBatchNormBwdParams *params) const {
  params->gmem_src          = static_cast<uint16_t*>(X_);
  params->gmem_dy           = static_cast<uint16_t*>(dY_);
  params->gmem_dst          = static_cast<uint16_t*>(dX_);
  params->gmem_dst1         = nullptr;
  params->gmem_relu_bitmask = nullptr;
  params->gmem_dscale       = dscale_;
  params->gmem_dbias        = dbias_;
  params->gmem_scale        = scale_;
  params->gmem_bias         = bias_;
  params->gmem_saved_mean   = minibatch_mean_;
  params->gmem_saved_var    = minibatch_variance_;
  params->nhw               = m_;
  params->c                 = c_;
  params->gmem_sums         = partial_sums_;
  params->gmem_retired_ctas = retired_ctas_;
  params->outer_loops       = 0;
  params->c_blks            = div_up(c_, C_ELEMENTS_PER_CTA);
}

void NhwcBatchNorm::fwdInference(cudaStream_t stream, bool use_relu) {
  bool ptrs_are_set =
      X_tensor_desc_ != nullptr
      && Y_tensor_desc_ != nullptr
      && scale_ != nullptr
      && bias_ != nullptr
      //      && minibatch_mean_ != nullptr
      //      && minibatch_variance_ != nullptr
      && population_mean_ != nullptr
      && population_variance_ != nullptr
      && X_ != nullptr
      //      && dX_ != nullptr
      && Y_ != nullptr
      //      && dY_ != nullptr
      //      && dscale_ != nullptr
      //      && dbias_ != nullptr
      && partial_sums_   != nullptr
      && partial_counts_ != nullptr;

  if (!ptrs_are_set)
    die();

  dim3 grid_dim;
  grid_dim.x = div_up(m_, PIXELS_PER_CTA_FWD_INFERENCE);
  grid_dim.y = div_up(c_, C_ELEMENTS_PER_CTA);

  // @todo: maybe just move this inside initialize routine?
  NhwcBatchNormFwdInferenceParams params;
  _setFwdInferenceParams(&params);

  if (use_relu) {
    nhwc_batch_norm_fwd_inference
      <StorageType, THREADS_PER_CTA, THREADS_PER_PIXEL, ELEMENTS_PER_LDG, true, false>
    <<<grid_dim, THREADS_PER_CTA, 0, stream>>>(params);
    checkCudaStatus(name_ + " fwd_inference-relu kernel");
  } else {
    nhwc_batch_norm_fwd_inference
      <StorageType, THREADS_PER_CTA, THREADS_PER_PIXEL, ELEMENTS_PER_LDG, false, false>
    <<<grid_dim, THREADS_PER_CTA, 0, stream>>>(params);
    checkCudaStatus(name_ + " fwd_inference kernel");
  }
}

dim3 NhwcBatchNorm::calc_fwd_grid(int device_id, int *loop) {
  dim3 grid_dim;
  grid_dim.x = div_up(m_, PIXELS_PER_CTA_FWD);
  int c_blks = div_up(c_, C_ELEMENTS_PER_CTA);
  unsigned int max_grid_x = max_fwd_grid_x(device_id);
  if (grid_dim.x <= max_grid_x) {
    *loop = 1;
    if (max_grid_x / grid_dim.x > 1) {
      grid_dim.y = std::min(c_blks, static_cast<int>(max_grid_x / grid_dim.x));
    } else {
      grid_dim.y = 1;
    }
  } else {
    grid_dim.x = max_grid_x;
    grid_dim.y = 1;
    int nhw_in_regs = m_ - PIXELS_PER_THREAD_IN_SMEM_FWD*PIXELS_PER_LDG*grid_dim.x;
    int pixels_per_iteration = PIXELS_PER_THREAD_IN_REGISTERS_FWD*PIXELS_PER_LDG*grid_dim.x;
    *loop = div_up(nhw_in_regs, pixels_per_iteration);
  }
  return grid_dim;
}

dim3 NhwcBatchNorm::calc_bwd_grid(int device_id, int *loop) {
  dim3 grid_dim;
  grid_dim.x = div_up(m_, PIXELS_PER_CTA_BWD);
  int c_blks = div_up(c_, C_ELEMENTS_PER_CTA);
  unsigned int max_grid_x = max_bwd_grid_x(device_id);
  if (grid_dim.x <= max_grid_x) {
    *loop = 1;
    if (max_grid_x / grid_dim.x > 1) {
      grid_dim.y = std::min(c_blks, static_cast<int>(max_grid_x / grid_dim.x));
    } else {
      grid_dim.y = 1;
    }
  } else {
    grid_dim.x = max_grid_x;
    grid_dim.y = 1;
    int nhw_in_regs = m_ - PIXELS_PER_THREAD_IN_SMEM_BWD*PIXELS_PER_LDG*grid_dim.x;
    int pixels_per_iteration = PIXELS_PER_THREAD_IN_REGISTERS_BWD*PIXELS_PER_LDG*grid_dim.x;
    *loop = div_up(nhw_in_regs, pixels_per_iteration);
  }
  return grid_dim;
}

void NhwcBatchNorm::fwd(cudaStream_t stream, bool use_relu, int device_id) {
  bool ptrs_are_set =
      X_tensor_desc_ != nullptr
      && Y_tensor_desc_ != nullptr
      && scale_ != nullptr
      && bias_ != nullptr
      && minibatch_mean_ != nullptr
      && minibatch_variance_ != nullptr
      && population_mean_ != nullptr
      && population_variance_ != nullptr
      && X_ != nullptr
      //      && dX_ != nullptr
      && Y_ != nullptr
      //      && dY_ != nullptr
      //      && dscale_ != nullptr
      //      && dbias_ != nullptr
      && partial_sums_   != nullptr
      && partial_counts_ != nullptr
      && retired_ctas_   != nullptr;

  if (!ptrs_are_set)
    die();

  // reset of retired_cta_count no longer needed

  NhwcBatchNormFwdParams params;
  _setFwdParams(&params);

  dim3 grid_dim = calc_fwd_grid(device_id, &params.outer_loops);
  _fwdKernelLauncher(stream, params, grid_dim, params.outer_loops, use_relu, device_id);
}

void NhwcBatchNorm::dgrad(cudaStream_t stream, bool use_relu, int device_id) {
  bool ptrs_are_set =
      X_tensor_desc_ != nullptr
      && Y_tensor_desc_ != nullptr
      && scale_ != nullptr
      && (bias_ != nullptr || !use_relu)
      && minibatch_mean_ != nullptr
      && minibatch_variance_ != nullptr
      //      && population_mean_ != nullptr
      //      && population_variance_ != nullptr
      && X_ != nullptr
      && dX_ != nullptr
      //      && Y_ != nullptr
      && dY_ != nullptr
      && dscale_ != nullptr
      && dbias_ != nullptr;

  if (!ptrs_are_set)
    die();

  // reset of retired_cta_count no longer needed

  NhwcBatchNormBwdParams params;
  _setBwdParams(&params);

  dim3 grid_dim = calc_bwd_grid(device_id, &params.outer_loops);
  _bwdKernelLauncher(stream, params, grid_dim, params.outer_loops, use_relu, device_id);
}

#endif  // MXNET_OPERATOR_NN_CUDNN_NHWC_BATCH_NORM_H_
