/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CORE_KERNELS_FAKE_QUANT_OPS_FUNCTOR_H_
#define TENSORFLOW_CORE_KERNELS_FAKE_QUANT_OPS_FUNCTOR_H_

#include <tuple>
#include <math.h>

#define EIGEN_STACK_ALLOCATION_LIMIT 0
#define EIGEN_USE_THREADS

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/platform/types.h"

EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE float StdRound(float input) {
// On Android, std::round() isn't present, just round().
#if defined(__ANDROID__)
  return round(input);
#else
  return std::round(input);
#endif
}

namespace tensorflow {

// Gymnastics with nudged zero point is to ensure that real zero maps to
// an integer, which is required for e.g. zero-padding in convolutional layers.
// Outputs nudged_min, nudged_max, nudged_scale.
EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE void Nudge(
    const float min, const float max, const int quant_min, const int quant_max,
    float* nudged_min, float* nudged_max, float* scale, const int tensor_type,
    const bool ev_quant, const float weights_scale = 1.0,
    const float inputs_scale = (1/127.5)) {
  const float quant_min_float = static_cast<float>(quant_min);
  const float quant_max_float = static_cast<float>(quant_max);
  if(ev_quant) { //EVQuantization Logic
    //EVQuant formula to calculate Weight Scale
    if(tensor_type == 0){
      const int weight_max_value = 127;//Signed max or (256/2 - 1)
      const float abs_max = std::max(-min, max);
     *scale = 1 / (weight_max_value / abs_max);
      }
    //EVQuant formula to calculate Activation Scale
    else if(tensor_type == 1){
      const int num_bits = 8;//8 bits for unsigned / (8-1)bits for signed
      const float abs_act_max = std::max(-min, max);
      const float value = (1/inputs_scale) * (1/weights_scale);
      const float multiplier = value * abs_act_max;
      const int bits_to_shift = (std::ceil(log2(multiplier))) - num_bits;
      *scale = 1 / (value / pow(2, bits_to_shift));
      }
    else if(tensor_type == 2){
      const int layer_max_value = 127;//Signed max or (256/2 - 1)
      const float abs_max = std::max(-min, max);
     *scale = 1 / (layer_max_value / abs_max);
    }
  const float zero_point_from_max = quant_max_float - (max / (*scale));
  const uint16 nudged_zero_point = [zero_point_from_max, quant_min,
                                    quant_min_float, quant_max,
                                    quant_max_float] {
    if (zero_point_from_max < quant_min_float) {
      return static_cast<uint16>(quant_min);
    }
    if (zero_point_from_max > quant_max_float) {
      return static_cast<uint16>(quant_max);
    }
    return static_cast<uint16>(StdRound(zero_point_from_max));
  }();
  *nudged_min = (quant_min_float - nudged_zero_point) * (*scale);
  *nudged_max = (quant_max_float - nudged_zero_point) * (*scale);
  }
  else {  //TFLogic
    *scale = (max - min) / (quant_max_float - quant_min_float);
    const float zero_point_from_min = quant_min_float - min / *scale;
    const uint16 nudged_zero_point = [zero_point_from_min, quant_min,
                                     quant_min_float, quant_max,
                                     quant_max_float] {
      if (zero_point_from_min < quant_min_float) {
        return static_cast<uint16>(quant_min);
      }
      if (zero_point_from_min > quant_max_float) {
        return static_cast<uint16>(quant_max);
      }
      return static_cast<uint16>(StdRound(zero_point_from_min));
    }();
    *nudged_min = (quant_min_float - nudged_zero_point) * (*scale);
    *nudged_max = (quant_max_float - nudged_zero_point) * (*scale);
  }
}

template <typename T>
using ConstScalar = typename tensorflow::TTypes<T>::ConstScalar;
template <typename T>
using Scalar = typename tensorflow::TTypes<T>::Scalar;
template <typename T>
using ConstVec = typename tensorflow::TTypes<T>::ConstVec;
template <typename T>
using Vec = typename tensorflow::TTypes<T>::Vec;
template <typename T>
using ConstFlat = typename tensorflow::TTypes<T>::ConstFlat;
template <typename T>
using Flat = typename tensorflow::TTypes<T>::Flat;

// Functor called by FakeQuantWithMinMaxArgsOp to do the work.  Compiles both
// for CPU and GPU.
template <typename Device>
struct FakeQuantWithMinMaxArgsFunctor {
  void operator()(const Device& d, ConstFlat<float> inputs, const float min,
                  const float max, const int quant_min, const int quant_max,
                  const int tensor_type, const bool ev_quant, Flat<float> outputs) {
    eigen_assert(min <= 0.0f && "min should be <= 0.0");
    eigen_assert(max >= 0.0f && "max should be >= 0.0");
    eigen_assert(min < max && "min should be < max");

    float nudged_min, nudged_max, nudged_scale;
    Nudge(min, max, quant_min, quant_max, &nudged_min, &nudged_max,
          &nudged_scale, tensor_type, ev_quant);
    const float inv_nudged_scale = 1.0f / nudged_scale;

    auto clamped = inputs.cwiseMin(nudged_max).cwiseMax(nudged_min);
    auto clamped_shifted = clamped - nudged_min;
    outputs.device(d) =
        (clamped_shifted * inv_nudged_scale + 0.5f).floor() * nudged_scale +
        nudged_min;
  }
};

// Functor called by FakeQuantWithMinMaxArgsGradientOp to do the work.  Compiles
// both for CPU and GPU.
template <typename Device>
struct FakeQuantWithMinMaxArgsGradientFunctor {
  void operator()(const Device& d, ConstFlat<float> gradients,
                  ConstFlat<float> inputs, const float min, const float max,
                  const int quant_min, const int quant_max,
                  const int tensor_type, const bool ev_quant, Flat<float> backprops) {
    eigen_assert(min <= 0.0f && "min should be <= 0.0");
    eigen_assert(max >= 0.0f && "max should be >= 0.0");
    eigen_assert(min < max && "min should be < max");

    float nudged_min, nudged_max, nudged_scale;
    Nudge(min, max, quant_min, quant_max, &nudged_min, &nudged_max,
          &nudged_scale, tensor_type, ev_quant);

    auto between_nudged_min_max =
        (inputs >= nudged_min && inputs <= nudged_max)
            .select(inputs.constant(1.0f), inputs.constant(0.0f));
    backprops.device(d) = gradients * between_nudged_min_max;
  }
};

// Functor called by FakeQuantWithMinMaxVarsOp to do the work.  Compiles both
// for CPU and GPU.
template <typename Device>
struct FakeQuantWithMinMaxVarsFunctor {
  void operator()(const Device& d, ConstFlat<float> inputs,
                  ConstScalar<float> min, ConstScalar<float> max,
                  const int quant_min, const int quant_max,
                  const int tensor_type, const bool ev_quant,
                  ConstScalar<float> w_scale,
                  ConstScalar<float> ip_scale, Flat<float> outputs,
                  float* op_w_scale, float* op_ip_scale) {
    const float min_val = min();
    const float max_val = max();
    const float weights_scale = w_scale();
    const float inputs_scale = ip_scale();
    // If min and max are both zero, we should just return zero.
    if (min_val == 0.0f && max_val == 0.0f) {
      outputs.device(d) = outputs.constant(0.0f);
      return;
    }
    float nudged_min, nudged_max, nudged_scale;
    if(ev_quant){ //EVQuantization Logic
      if(tensor_type == 0 or tensor_type == 2) {
        Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
              &nudged_scale, tensor_type, ev_quant);
      }
      else if(tensor_type == 1){
        Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
              &nudged_scale, tensor_type, ev_quant, weights_scale, inputs_scale);
      }
    }
    else{ //TFLogic
      Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
            &nudged_scale, tensor_type, ev_quant);
    }
    const auto nudged_scale_repl = inputs.constant(nudged_scale);
    const auto clamped = inputs.cwiseMin(nudged_max).cwiseMax(nudged_min);
    const auto clamped_shifted = clamped - nudged_min;

    if(ev_quant){
      outputs.device(d) = (clamped_shifted / nudged_scale_repl + 0.5f).floor() *
                               nudged_scale_repl + nudged_min;
      //Update calculated Weight Scale in op_w_scale
      if(tensor_type == 0){
        *op_ip_scale = inputs_scale;
        *op_w_scale = nudged_scale;
      }
      //Update calculated Activation Scale in op_ip_scale
      else if(tensor_type == 1 or tensor_type == 2){
        *op_ip_scale = nudged_scale;
        *op_w_scale = weights_scale;
      }
    }
    else{
      outputs.device(d) = (clamped_shifted / nudged_scale_repl + 0.5f).floor() *
                              nudged_scale_repl + nudged_min;
    }
  }
};

// Functor called by FakeQuantWithMinMaxVarsGradientOp to do the work.  Compiles
// both for CPU and GPU.
template <typename Device>
struct FakeQuantWithMinMaxVarsGradientFunctor {
  void operator()(const Device& d, ConstFlat<float> gradients,
                  ConstFlat<float> inputs, ConstScalar<float> min,
                  ConstScalar<float> max, const int quant_min,
                  const int quant_max, const int tensor_type,
                  const bool ev_quant,
                  ConstScalar<float> w_scale, ConstScalar<float> ip_scale,
                  Flat<float> backprops_wrt_input,
                  Scalar<float> backprop_wrt_min,
                  Scalar<float> backprop_wrt_max,
                  Scalar<float> backprop_wrt_w_scale,
                  Scalar<float> backprop_wrt_ip_scale) {
    const float min_val = min();
    const float max_val = max();
    const float weights_scale = w_scale();
    const float inputs_scale = ip_scale();
    // If min and max are both zero, we propagate everything to inputs.
    if (min_val == 0.0f && max_val == 0.0f) {
      backprops_wrt_input.device(d) = gradients;
      backprop_wrt_min.device(d) = backprop_wrt_min.constant(0.0f);
      backprop_wrt_max.device(d) = backprop_wrt_max.constant(0.0f);
      return;
    }
    float nudged_min, nudged_max, nudged_scale;
    if(ev_quant){ //EVQuantization Logic
      if(tensor_type == 0) {
        Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
              &nudged_scale, tensor_type, ev_quant);
      }
      else if(tensor_type == 1){
        Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
              &nudged_scale, tensor_type, ev_quant, weights_scale, inputs_scale);
      }
    }
    else{ //TFLogic
      Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
            &nudged_scale, tensor_type, ev_quant);
    }
    const auto between_min_max =
        (inputs >= nudged_min && inputs <= nudged_max)
            .select(inputs.constant(1.0f), inputs.constant(0.0f));
    backprops_wrt_input.device(d) = gradients * between_min_max;

    const auto below_min =
        (inputs < nudged_min)
            .select(inputs.constant(1.0f), inputs.constant(0.0f));
    backprop_wrt_min.device(d) = (gradients * below_min).sum();

    const auto above_max =
        (inputs > nudged_max)
            .select(inputs.constant(1.0f), inputs.constant(0.0f));
    backprop_wrt_max.device(d) = (gradients * above_max).sum();
  }
};

using Index = typename tensorflow::TTypes<float>::ConstTensor::Index;

// Functor called by FakeQuantWithMinMaxVarsPerChannelOp to do the work.
// Compiles both for CPU and GPU.
//
// Already verified: inputs, outputs are of shape [b, d], min, max are of shape
// [d].
template <typename Device>
struct FakeQuantWithMinMaxVarsPerChannelFunctor {
  void operator()(const Device& d, TTypes<float>::ConstMatrix inputs,
                  ConstVec<float> min, ConstVec<float> max, const int quant_min,
                  const int quant_max, const int tensor_type,
                  const bool ev_quant, TTypes<float>::Matrix outputs) {
    for (Index i = 0; i < min.size(); ++i) {
      const float min_val = min(i);
      const float max_val = max(i);
      // If min and max are both zero, we should just return zero.
      if (min_val == 0.0f && max_val == 0.0f) {
        auto chip = outputs.chip<1>(i);
        chip.device(d) = chip.constant(0.0f);
        continue;
      }
      float nudged_min, nudged_max, nudged_scale;
      Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
            &nudged_scale, tensor_type, ev_quant);
      const auto clamped =
          inputs.chip<1>(i).cwiseMin(nudged_max).cwiseMax(nudged_min);
      const auto clamped_shifted = clamped - nudged_min;

      outputs.chip<1>(i).device(d) =
          (clamped_shifted / nudged_scale + 0.5f).floor() * nudged_scale +
          nudged_min;
    }
  }
};

// Functor called by FakeQuantWithMinMaxVarsPerChannelGradientOp to do the work.
// Compiles both for CPU and GPU.
//
// Already verified: gradients, inputs, backprops_wrt_input are of shape [b, d],
// min, max, backprop_wrt_min, backprop_wrt_max are of shape [d].
template <typename Device>
struct FakeQuantWithMinMaxVarsPerChannelGradientFunctor {
  void operator()(const Device& d, TTypes<float>::ConstMatrix gradients,
                  TTypes<float>::ConstMatrix inputs, ConstVec<float> min,
                  ConstVec<float> max, const int quant_min, const int quant_max,
                  const int tensor_type, const bool ev_quant,
                  TTypes<float>::Matrix backprops_wrt_input,
                  Vec<float> backprop_wrt_min, Vec<float> backprop_wrt_max) {
    for (Index i = 0; i < min.size(); ++i) {
      const float min_val = min(i);
      const float max_val = max(i);
      const auto gradients_chip = gradients.chip<1>(i);
      const auto inputs_chip = inputs.chip<1>(i);
      // If min and max are both zero, we propagate everything to inputs.
      if (min_val == 0.0f && max_val == 0.0f) {
        backprops_wrt_input.chip<1>(i).device(d) = gradients_chip;
        auto min_chip = backprop_wrt_min.chip<0>(i);
        auto max_chip = backprop_wrt_max.chip<0>(i);
        min_chip.device(d) = min_chip.constant(0.0f);
        max_chip.device(d) = max_chip.constant(0.0f);
        continue;
      }
      float nudged_min, nudged_max, nudged_scale;
      Nudge(min_val, max_val, quant_min, quant_max, &nudged_min, &nudged_max,
            &nudged_scale, tensor_type, ev_quant);

      const auto between_min_max =
          (inputs_chip >= nudged_min && inputs_chip <= nudged_max)
              .select(inputs_chip.constant(1.0f), inputs_chip.constant(0.0f));
      backprops_wrt_input.chip<1>(i).device(d) =
          gradients_chip * between_min_max;

      const auto below_min =
          (inputs_chip < nudged_min)
              .select(inputs_chip.constant(1.0f), inputs_chip.constant(0.0f));
      Eigen::DSizes<Index, 1> reduce(0);
      backprop_wrt_min.chip<0>(i).device(d) =
          (gradients_chip * below_min).sum(reduce);

      const auto above_max =
          (inputs_chip > nudged_max)
              .select(inputs_chip.constant(1.0f), inputs_chip.constant(0.0f));
      backprop_wrt_max.chip<0>(i).device(d) =
          (gradients_chip * above_max).sum(reduce);
    }
  }
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_KERNELS_FAKE_QUANT_OPS_FUNCTOR_H_
