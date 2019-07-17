/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuda_runtime.h>

#include <iostream>
#include "HoltWinters.hpp"
#include "holtwinters_utils.hpp"
#include "hw_cu_utils.hpp"
#include "utils.h"

namespace ML {

void HWInit() {
  cublas::get_handle();
  cusolver::get_handle();
  // return void::HW_SUCCESS;  // TODO(ahmad): check cublas
}

void HWDestroy() {
  cublas::destroy_handle();
  cusolver::destroy_handle();
  // return void::HW_SUCCESS;  // TODO(ahmad): check cublas
}

template <typename Dtype>
void HWTranspose(const Dtype *data_in, int m, int n, Dtype *data_out) {
  ASSERT(!(!data_in || !data_out || n < 1 || m < 1), "HW error in in line %d",
         __LINE__);

  transpose_gpu(data_in, m, n, data_out);
}

void HoltWintersBufferSize(int n, int batch_size, int frequency, bool use_beta,
                           bool use_gamma, int *start_leveltrend_len,
                           int *start_season_len, int *components_len,
                           int *error_len, int *leveltrend_coef_shift,
                           int *season_coef_shift) {
  int w_len = use_gamma ? frequency : (use_beta ? 2 : 1);

  if (start_leveltrend_len) *start_leveltrend_len = batch_size;
  if (use_gamma && start_season_len) *start_season_len = frequency * batch_size;

  if (components_len) *components_len = (n - w_len) * batch_size;

  if (leveltrend_coef_shift)
    *leveltrend_coef_shift = (n - w_len - 1) * batch_size;
  if (use_gamma && season_coef_shift)
    *season_coef_shift = (n - w_len - frequency) * batch_size;

  if (error_len) *error_len = batch_size;
}

template <typename Dtype>
void HoltWintersDecompose(const Dtype *ts, int n, int batch_size, int frequency,
                          Dtype *start_level, Dtype *start_trend,
                          Dtype *start_season, int start_periods,
                          SeasonalType seasonal) {
  cudaStream_t stream;
  CUDA_CHECK(cudaStreamCreate(&stream));
  if (start_level != nullptr && start_trend == nullptr &&
      start_season == nullptr) {  // level decomposition
    MLCommon::updateDevice(start_level, ts, batch_size, stream);
  } else if (start_level != nullptr && start_trend != nullptr &&
             start_season == nullptr) {  // trend decomposition
    MLCommon::updateDevice(start_level, ts + batch_size, batch_size, stream);
    MLCommon::updateDevice(start_trend, ts + batch_size, batch_size, stream);
    cublas::axpy(batch_size, (Dtype)-1., ts, start_trend);
  } else if (start_level != nullptr && start_trend != nullptr &&
             start_season != nullptr) {
    stl_decomposition_gpu(ts, n, batch_size, frequency, start_periods,
                          start_level, start_trend, start_season, seasonal);
  }
}

template <typename Dtype>
void HoltWintersEval(const Dtype *ts, int n, int batch_size, int frequency,
                     const Dtype *start_level, const Dtype *start_trend,
                     const Dtype *start_season, const Dtype *alpha,
                     const Dtype *beta, const Dtype *gamma, Dtype *level,
                     Dtype *trend, Dtype *season, Dtype *xhat, Dtype *error,
                     SeasonalType seasonal) {
  ASSERT(!((!start_trend) != (!beta) || (!start_season) != (!gamma)),
         "HW error in in line %d", __LINE__);
  ASSERT(!(!alpha || !start_level), "HW error in in line %d", __LINE__);
  ASSERT(!(start_season != nullptr && frequency < 2), "HW error in in line %d",
         __LINE__);
  if (!(!level && !trend && !season && !xhat && !error)) {
    holtwinters_eval_gpu(ts, n, batch_size, frequency, start_level, start_trend,
                         start_season, alpha, beta, gamma, level, trend, season,
                         xhat, error,
                         seasonal);  // TODO(ahmad): return value
  }
}

// TODO(ahmad): expose line search step size
// TODO(ahmad): add the dynamic step size to CPU version
// TODO(ahmad): min_error_diff is actually min_param_diff
// TODO(ahmad): add a min_error_diff criterion
// TODO(ahmad): update default optim params in the doc
// TODO(ahmad): if linesearch_iter_limit is reached, we update wrong nx values (nx values that don't minimze loss).
// change this to at least keep the old xs
template <typename Dtype>
void HoltWintersOptim(const Dtype *ts, int n, int batch_size, int frequency,
                      const Dtype *start_level, const Dtype *start_trend,
                      const Dtype *start_season, Dtype *alpha, bool optim_alpha,
                      Dtype *beta, bool optim_beta, Dtype *gamma,
                      bool optim_gamma, Dtype *level, Dtype *trend,
                      Dtype *season, Dtype *xhat, Dtype *error,
                      OptimCriterion *optim_result,
                      OptimParams<Dtype> *optim_params, SeasonalType seasonal) {
  // default values
  OptimParams<Dtype> optim_params_;
  optim_params_.eps = (Dtype)2.2204e-3;
  optim_params_.min_param_diff = (Dtype)1e-8;
  optim_params_.min_error_diff = (Dtype)1e-8;
  optim_params_.min_grad_norm = (Dtype)1e-4;
  optim_params_.bfgs_iter_limit = 1000;
  optim_params_.linesearch_iter_limit = 100;
  optim_params_.linesearch_tau = (Dtype)0.5;
  optim_params_.linesearch_c = (Dtype)0.8;
  optim_params_.linesearch_step_size = (Dtype)-1;

  if (optim_params) {
    if (optim_params->eps > .0) optim_params_.eps = optim_params->eps;
    if (optim_params->min_param_diff >= .0)
      optim_params_.min_param_diff = optim_params->min_param_diff;
    if (optim_params->min_error_diff >= .0)
      optim_params_.min_error_diff = optim_params->min_error_diff;
    if (optim_params->min_grad_norm >= .0)
      optim_params_.min_grad_norm = optim_params->min_grad_norm;
    if (optim_params->bfgs_iter_limit > 0)
      optim_params_.bfgs_iter_limit = optim_params->bfgs_iter_limit;
    if (optim_params->linesearch_iter_limit > 0)
      optim_params_.linesearch_iter_limit = optim_params->linesearch_iter_limit;
    if (optim_params->linesearch_tau > .0)
      optim_params_.linesearch_tau = optim_params->linesearch_tau;
    if (optim_params->linesearch_c > .0)
      optim_params_.linesearch_c = optim_params->linesearch_c;
    if (optim_params->linesearch_step_size > 0)
      optim_params_.linesearch_step_size = optim_params->linesearch_step_size;
  }

  ASSERT(alpha && start_level, "HW error in in line %d", __LINE__);
  ASSERT(!((!start_trend) != (!beta) || (!start_season) != (!gamma)),
         "HW error in in line %d", __LINE__);
  ASSERT(!(start_season && frequency < 2), "HW error in in line %d", __LINE__);
  ASSERT(!(!optim_alpha && !optim_beta && !optim_gamma),
         "HW error in in line %d", __LINE__);
  ASSERT(!((optim_beta && !beta) || (optim_gamma && !gamma)),
         "HW error in in line %d", __LINE__);
  if (!(!alpha && !beta && !gamma & !level && !trend && !season && !xhat &&
        !error)) {
    holtwinters_optim_gpu(ts, n, batch_size, frequency, start_level,
                          start_trend, start_season, alpha, optim_alpha, beta,
                          optim_beta, gamma, optim_gamma, level, trend, season,
                          xhat, error, optim_result, seasonal,
                          optim_params_);  // TODO(ahmad): return
  }
}

template <typename Dtype>
void HoltWintersForecast(Dtype *forecast, int h, int batch_size, int frequency,
                         const Dtype *level_coef, const Dtype *trend_coef,
                         const Dtype *season_coef, SeasonalType seasonal) {
  ASSERT(!(!level_coef && !trend_coef && !season_coef),
         "HW error in in line %d", __LINE__);
  ASSERT(!(season_coef && frequency < 2), "HW error in in line %d", __LINE__);
  holtwinters_forecast_gpu(forecast, h, batch_size, frequency, level_coef,
                           trend_coef, season_coef,
                           seasonal);  // TODO(ahmad): return value
}

template <typename Dtype>
void HoltWintersFitPredict(int n, int batch_size, int frequency, int h,
                           int start_periods, SeasonalType seasonal,
                           Dtype *data, Dtype *alpha_ptr, Dtype *beta_ptr,
                           Dtype *gamma_ptr, Dtype *SSE_error_ptr,
                           Dtype *forecast_ptr) {
  HWInit();

  cudaStream_t stream;
  CUDA_CHECK(cudaStreamCreate(&stream));

  bool optim_alpha = true, optim_beta = true, optim_gamma = true;
  // initial values for alpha, beta and gamma
  std::vector<Dtype> alpha_h(batch_size, 0.4);
  std::vector<Dtype> beta_h(batch_size, 0.3);
  std::vector<Dtype> gamma_h(batch_size, 0.3);

  int leveltrend_seed_len, season_seed_len, components_len;
  int leveltrend_coef_offset, season_coef_offset;
  int error_len;

  HoltWintersBufferSize(
    n, batch_size, frequency, optim_beta, optim_gamma,
    &leveltrend_seed_len,     // = batch_size
    &season_seed_len,         // = frequency*batch_size
    &components_len,          // = (n-w_len)*batch_size
    &error_len,               // = batch_size
    &leveltrend_coef_offset,  // = (n-wlen-1)*batch_size (last row)
    &season_coef_offset);     // = (n-wlen-frequency)*batch_size(last freq rows)

  Dtype *dataset_d;
  Dtype *forecast_d;
  Dtype *level_seed_d, *trend_seed_d = nullptr, *start_season_d = nullptr;
  Dtype *level_d, *trend_d = nullptr, *season_d = nullptr;
  Dtype *alpha_d, *beta_d = nullptr, *gamma_d = nullptr;
  Dtype *error_d;

  MLCommon::allocate(dataset_d, batch_size * n);
  MLCommon::allocate(forecast_d, batch_size * h);
  MLCommon::allocate(alpha_d, batch_size);
  MLCommon::updateDevice(alpha_d, alpha_h.data(), batch_size, stream);
  MLCommon::allocate(level_seed_d, leveltrend_seed_len);
  MLCommon::allocate(level_d, components_len);

  if (optim_beta) {
    MLCommon::allocate(beta_d, batch_size);
    MLCommon::updateDevice(beta_d, beta_h.data(), batch_size, stream);
    MLCommon::allocate(trend_seed_d, leveltrend_seed_len);
    MLCommon::allocate(trend_d, components_len);
  }

  if (optim_gamma) {
    MLCommon::allocate(gamma_d, batch_size);
    MLCommon::updateDevice(gamma_d, gamma_h.data(), batch_size, stream);
    MLCommon::allocate(start_season_d, season_seed_len);
    MLCommon::allocate(season_d, components_len);
  }

  MLCommon::allocate(error_d, error_len);

  // Step 1: transpose the dataset (ML expects col major dataset)
  HWTranspose(data, batch_size, n, dataset_d);

  // Step 2: Decompose dataset to get seed for level, trend and seasonal values
  HoltWintersDecompose(dataset_d, n, batch_size, frequency, level_seed_d,
                       trend_seed_d, start_season_d, start_periods, seasonal);

  // Step 3: Find optimal alpha, beta and gamma values (seasonal HW)
  HoltWintersOptim(dataset_d, n, batch_size, frequency, level_seed_d,
                   trend_seed_d, start_season_d, alpha_d, optim_alpha, beta_d,
                   optim_beta, gamma_d, optim_gamma, level_d, trend_d, season_d,
                   (Dtype *)nullptr, error_d, (OptimCriterion *)nullptr,
                   (OptimParams<Dtype> *)nullptr, seasonal);

  // Step 4: Do forecast
  HoltWintersForecast(
    forecast_d, h, batch_size, frequency, level_d + leveltrend_coef_offset,
    trend_d + leveltrend_coef_offset, season_d + season_coef_offset, seasonal);

  //getting alpha values from Device to Host for output:
  MLCommon::updateHost(alpha_ptr, alpha_d, batch_size, stream);

  //getting beta values Device to Host for output:
  MLCommon::updateHost(beta_ptr, beta_d, batch_size, stream);

  //getting gamma values Device to Host for output:
  MLCommon::updateHost(gamma_ptr, gamma_d, batch_size, stream);

  //getting error values Device to Host for output:
  MLCommon::updateHost(SSE_error_ptr, error_d, batch_size, stream);

  std::vector<Dtype> forecast(batch_size * h);
  //getting forecasted values
  MLCommon::updateHost(forecast.data(), forecast_d, batch_size * h, stream);

  // Get data from 1-D column major to 1-D row major for output
  long index = 0;
  for (auto i = 0; i < batch_size; ++i) {
    for (auto j = 0; j < h; ++j)
      forecast_ptr[index++] = forecast[i + j * batch_size];
  }

  CUDA_CHECK(cudaStreamSynchronize(stream));
  CUDA_CHECK(cudaStreamDestroy(stream));

  HWDestroy();

  // Free the allocated memory on GPU
  CUDA_CHECK(cudaFree(dataset_d));
  CUDA_CHECK(cudaFree(forecast_d));
  CUDA_CHECK(cudaFree(level_seed_d));
  CUDA_CHECK(cudaFree(trend_seed_d));
  CUDA_CHECK(cudaFree(start_season_d));
  CUDA_CHECK(cudaFree(level_d));
  CUDA_CHECK(cudaFree(trend_d));
  CUDA_CHECK(cudaFree(season_d));
  CUDA_CHECK(cudaFree(alpha_d));
  CUDA_CHECK(cudaFree(beta_d));
  CUDA_CHECK(cudaFree(gamma_d));
  CUDA_CHECK(cudaFree(error_d));
}

// template void HWTranspose<float>(const float *data_in, int m, int n,
//                                  float *data_out);
// template void HWTranspose<double>(const double *data_in, int m, int n,
//                                   double *data_out);

// template void HoltWintersDecompose<float>(
//   const float *ts, int n, int batch_size, int frequency, float *start_level,
//   float *start_trend, float *start_season, int start_periods,
//   SeasonalType seasonal);
// template void HoltWintersDecompose<double>(
//   const double *ts, int n, int batch_size, int frequency, double *start_level,
//   double *start_trend, double *start_season, int start_periods,
//   SeasonalType seasonal);

// template void HoltWintersEval<float>(const float *ts, int n, int batch_size,
//                                      int frequency, const float *start_level,
//                                      const float *start_trend,
//                                      const float *start_season,
//                                      const float *alpha, const float *beta,
//                                      const float *gamma, float *level,
//                                      float *trend, float *season, float *xhat,
//                                      float *error, SeasonalType seasonal);
// template void HoltWintersEval<double>(
//   const double *ts, int n, int batch_size, int frequency,
//   const double *start_level, const double *start_trend,
//   const double *start_season, const double *alpha, const double *beta,
//   const double *gamma, double *level, double *trend, double *season,
//   double *xhat, double *error, SeasonalType seasonal);

// template void HoltWintersOptim(
//   const float *ts, int n, int batch_size, int frequency,
//   const float *start_level, const float *start_trend, const float *start_season,
//   float *alpha, bool optim_alpha, float *beta, bool optim_beta, float *gamma,
//   bool optim_gamma, float *level, float *trend, float *season, float *xhat,
//   float *error, OptimCriterion *optim_result, OptimParams<float> *optim_params,
//   SeasonalType seasonal);
// template void HoltWintersOptim(const double *ts, int n, int batch_size,
//                                int frequency, const double *start_level,
//                                const double *start_trend,
//                                const double *start_season, double *alpha,
//                                bool optim_alpha, double *beta, bool optim_beta,
//                                double *gamma, bool optim_gamma, double *level,
//                                double *trend, double *season, double *xhat,
//                                double *error, OptimCriterion *optim_result,
//                                OptimParams<double> *optim_params,
//                                SeasonalType seasonal);

// template void HoltWintersForecast<float>(float *forecast, int h, int batch_size,
//                                          int frequency, const float *level_coef,
//                                          const float *trend_coef,
//                                          const float *season_coef,
//                                          SeasonalType seasonal);
// template void HoltWintersForecast<double>(double *forecast, int h,
//                                           int batch_size, int frequency,
//                                           const double *level_coef,
//                                           const double *trend_coef,
//                                           const double *season_coef,
//                                           SeasonalType seasonal);

// template void HoltWintersFitPredict<float>(
//   int n, int batch_size, int frequency, int h, int start_periods,
//   SeasonalType seasonal, float *data, float *alpha_ptr, float *beta_ptr,
//   float *gamma_ptr, float *SSE_error_ptr, float *forecast_ptr);
// template void HoltWintersFitPredict<double>(
//   int n, int batch_size, int frequency, int h, int start_periods,
//   SeasonalType seasonal, double *data, double *alpha_ptr, double *beta_ptr,
//   double *gamma_ptr, double *SSE_error_ptr, double *forecast_ptr);

}  // namespace ML