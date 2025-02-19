// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/phi/kernels/box_coder_kernel.h"

#include "paddle/phi/backends/cpu/cpu_context.h"
#include "paddle/phi/core/kernel_registry.h"
#include "paddle/phi/kernels/funcs/math_function.h"
#include "paddle/phi/kernels/impl/box_coder.h"

namespace phi {

template <typename T>
void EncodeCenterSize(const DenseTensor *target_box,
                      const DenseTensor *prior_box,
                      const DenseTensor *prior_box_var,
                      const bool normalized,
                      const std::vector<float> variance,
                      T *output) {
  int64_t row = target_box->dims()[0];
  int64_t col = prior_box->dims()[0];
  int64_t len = prior_box->dims()[1];

#ifdef PADDLE_WITH_MKLML
#pragma omp parallel for collapse(2)
#endif
  for (int64_t i = 0; i < row; ++i) {
    for (int64_t j = 0; j < col; ++j) {
      auto *target_box_data = target_box->data<T>();
      auto *prior_box_data = prior_box->data<T>();
      size_t offset = i * col * len + j * len;
      T prior_box_width = prior_box_data[j * len + 2] -
                          prior_box_data[j * len] + (normalized == false);
      T prior_box_height = prior_box_data[j * len + 3] -
                           prior_box_data[j * len + 1] + (normalized == false);
      T prior_box_center_x = prior_box_data[j * len] + prior_box_width / 2;
      T prior_box_center_y = prior_box_data[j * len + 1] + prior_box_height / 2;

      T target_box_center_x =
          (target_box_data[i * len + 2] + target_box_data[i * len]) / 2;
      T target_box_center_y =
          (target_box_data[i * len + 3] + target_box_data[i * len + 1]) / 2;
      T target_box_width = target_box_data[i * len + 2] -
                           target_box_data[i * len] + (normalized == false);
      T target_box_height = target_box_data[i * len + 3] -
                            target_box_data[i * len + 1] +
                            (normalized == false);

      output[offset] =
          (target_box_center_x - prior_box_center_x) / prior_box_width;
      output[offset + 1] =
          (target_box_center_y - prior_box_center_y) / prior_box_height;
      output[offset + 2] =
          std::log(std::fabs(target_box_width / prior_box_width));
      output[offset + 3] =
          std::log(std::fabs(target_box_height / prior_box_height));
    }
  }

  if (prior_box_var) {
    const T *prior_box_var_data = prior_box_var->data<T>();
#ifdef PADDLE_WITH_MKLML
#pragma omp parallel for collapse(3)
#endif
    for (int64_t i = 0; i < row; ++i) {
      for (int64_t j = 0; j < col; ++j) {
        for (int k = 0; k < 4; ++k) {
          size_t offset = i * col * len + j * len;
          int prior_var_offset = j * len;
          output[offset + k] /= prior_box_var_data[prior_var_offset + k];
        }
      }
    }
  } else if (!(variance.empty())) {
#ifdef PADDLE_WITH_MKLML
#pragma omp parallel for collapse(3)
#endif
    for (int64_t i = 0; i < row; ++i) {
      for (int64_t j = 0; j < col; ++j) {
        for (int k = 0; k < 4; ++k) {
          size_t offset = i * col * len + j * len;
          output[offset + k] /= static_cast<T>(variance[k]);
        }
      }
    }
  }
}

template <typename T, int axis, int var_size>
void DecodeCenterSize(const DenseTensor *target_box,
                      const DenseTensor *prior_box,
                      const DenseTensor *prior_box_var,
                      const bool normalized,
                      std::vector<float> variance,
                      T *output) {
  int64_t row = target_box->dims()[0];
  int64_t col = target_box->dims()[1];
  int64_t len = target_box->dims()[2];

#ifdef PADDLE_WITH_MKLML
#pragma omp parallel for collapse(2)
#endif
  for (int64_t i = 0; i < row; ++i) {
    for (int64_t j = 0; j < col; ++j) {
      auto *target_box_data = target_box->data<T>();
      auto *prior_box_data = prior_box->data<T>();

      T var_data[4] = {1., 1., 1., 1.};
      T *var_ptr = var_data;
      size_t offset = i * col * len + j * len;
      int prior_box_offset = axis == 0 ? j * len : i * len;

      T prior_box_width = prior_box_data[prior_box_offset + 2] -
                          prior_box_data[prior_box_offset] +
                          (normalized == false);
      T prior_box_height = prior_box_data[prior_box_offset + 3] -
                           prior_box_data[prior_box_offset + 1] +
                           (normalized == false);
      T prior_box_center_x =
          prior_box_data[prior_box_offset] + prior_box_width / 2;
      T prior_box_center_y =
          prior_box_data[prior_box_offset + 1] + prior_box_height / 2;

      T target_box_center_x = 0, target_box_center_y = 0;
      T target_box_width = 0, target_box_height = 0;
      int prior_var_offset = axis == 0 ? j * len : i * len;
      if (var_size == 2) {
        std::memcpy(var_ptr,
                    prior_box_var->data<T>() + prior_var_offset,
                    4 * sizeof(T));
      } else if (var_size == 1) {
        var_ptr = reinterpret_cast<T *>(variance.data());
      }
      T box_var_x = *var_ptr;
      T box_var_y = *(var_ptr + 1);
      T box_var_w = *(var_ptr + 2);
      T box_var_h = *(var_ptr + 3);

      target_box_center_x =
          box_var_x * target_box_data[offset] * prior_box_width +
          prior_box_center_x;
      target_box_center_y =
          box_var_y * target_box_data[offset + 1] * prior_box_height +
          prior_box_center_y;
      target_box_width =
          std::exp(box_var_w * target_box_data[offset + 2]) * prior_box_width;
      target_box_height =
          std::exp(box_var_h * target_box_data[offset + 3]) * prior_box_height;

      output[offset] = target_box_center_x - target_box_width / 2;
      output[offset + 1] = target_box_center_y - target_box_height / 2;
      output[offset + 2] =
          target_box_center_x + target_box_width / 2 - (normalized == false);
      output[offset + 3] =
          target_box_center_y + target_box_height / 2 - (normalized == false);
    }
  }
}

template <typename T, typename Context>
void BoxCoderKernel(const Context &dev_ctx,
                    const DenseTensor &prior_box,
                    const paddle::optional<DenseTensor> &prior_box_var,
                    const DenseTensor &target_box,
                    const std::string &code_type_str,
                    bool normalized,
                    int axis,
                    const std::vector<float> &variance,
                    DenseTensor *output_box) {
  if (!target_box.lod().empty()) {
    PADDLE_ENFORCE_EQ(target_box.lod().size(),
                      1UL,
                      phi::errors::InvalidArgument(
                          "Input(TargetBox) of BoxCoder operator "
                          "supports LoD with only one level. But received "
                          "level = %d",
                          target_box.lod().size()));
  }
  if (prior_box_var) {
    PADDLE_ENFORCE_EQ(variance.empty(),
                      true,
                      phi::errors::InvalidArgument(
                          "Input 'PriorBoxVar' and attribute 'variance' "
                          "of BoxCoder operator should not be used at the "
                          "same time."));
  }
  if (!(variance.empty())) {
    PADDLE_ENFORCE_EQ(
        static_cast<int>(variance.size()),
        4,
        phi::errors::InvalidArgument("Size of attribute 'variance' of BoxCoder "
                                     "operator should be 4. But received "
                                     "size = %d",
                                     variance.size()));
  }

  auto code_type = phi::funcs::GetBoxCodeType(code_type_str);
  auto row = target_box.dims()[0];
  auto col = prior_box.dims()[0];
  if (code_type == phi::funcs::BoxCodeType::kDecodeCenterSize) {
    col = target_box.dims()[1];
  }
  auto len = prior_box.dims()[1];
  output_box->Resize({row, col, len});
  dev_ctx.template Alloc<T>(output_box);
  T *output = output_box->data<T>();
  if (code_type == phi::funcs::BoxCodeType::kEncodeCenterSize) {
    EncodeCenterSize<T>(&target_box,
                        &prior_box,
                        prior_box_var.get_ptr(),
                        normalized,
                        variance,
                        output);
  } else if (code_type == phi::funcs::BoxCodeType::kDecodeCenterSize) {
    if (prior_box_var) {
      if (axis == 0) {
        DecodeCenterSize<T, 0, 2>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      } else {
        DecodeCenterSize<T, 1, 2>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      }
    } else if (!(variance.empty())) {
      if (axis == 0) {
        DecodeCenterSize<T, 0, 1>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      } else {
        DecodeCenterSize<T, 1, 1>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      }
    } else {
      if (axis == 0) {
        DecodeCenterSize<T, 0, 0>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      } else {
        DecodeCenterSize<T, 1, 0>(&target_box,
                                  &prior_box,
                                  prior_box_var.get_ptr(),
                                  normalized,
                                  variance,
                                  output);
      }
    }
  }
}

}  // namespace phi

PD_REGISTER_KERNEL(
    box_coder, CPU, ALL_LAYOUT, phi::BoxCoderKernel, float, double) {}
