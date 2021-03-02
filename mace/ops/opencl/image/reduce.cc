// Copyright 2018 The MACE Authors. All Rights Reserved.
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

#include "mace/ops/opencl/image/reduce.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "mace/runtimes/opencl/opencl_runtime.h"

namespace mace {
namespace ops {
namespace opencl {
namespace image {

namespace {
const index_t TILE_SIZE = 16;

cl::Image *GetScratchImage(OpContext *context, MemoryType mem_type,
                           DataType dtype, const std::vector<index_t> &shape) {
  std::vector<size_t> image_shape;
  OpenCLUtil::CalImage2DShape(shape, BufferContentType::IN_OUT_CHANNEL,
                              &image_shape);

  auto *runtime = context->runtime();
  MemInfo mem_info(mem_type, dtype, MemInfo::IndexT(image_shape));
  auto mace_image = runtime->ObtainBuffer(mem_info, RENT_SCRATCH);
  cl::Image *image = mace_image->mutable_memory<cl::Image>();

  return image;
}

}  // namespace

MaceStatus ReduceKernel::BuildReduceKernel(OpenclExecutor *executor) {
  std::set<std::string> built_options;
  MACE_OUT_OF_RANGE_CONFIG;
  MACE_NON_UNIFORM_WG_CONFIG;
  std::string kernel_name = MACE_OBFUSCATE_SYMBOL("reduce");
  built_options.emplace("-Dreduce=" + kernel_name);
  built_options.emplace("-DDATA_TYPE=" + DtToCLDt(DT_FLOAT));
  built_options.emplace("-DCMD_DATA_TYPE=" + DtToCLCMDDt(DT_FLOAT));
  built_options.emplace(MakeString("-DREDUCE_TYPE=", reduce_type_));
  MACE_RETURN_IF_ERROR(executor->BuildKernel(
      "reduce", kernel_name, built_options, &kernel_));
  kwg_size_ =
      static_cast<uint32_t>(executor->GetKernelMaxWorkGroupSize(kernel_));

  return MaceStatus::MACE_SUCCESS;
}

MaceStatus ReduceKernel::GraduallyComputeReduce(
    OpContext *context, const index_t batch, const index_t channel_blocks,
    const index_t in_height, const index_t in_width,
    const index_t out_height, const index_t out_width,
    const index_t org_height, const index_t org_width,
    const cl::Image *input, cl::Image *output) {
  MACE_OUT_OF_RANGE_DEFINITION;
  auto *executor = OpenclRuntime::Get(context)->GetOpenclExecutor();
  if (kernel_.get() == nullptr) {
    MACE_RETURN_IF_ERROR(BuildReduceKernel(executor));
  }

  const uint32_t gws[3] = {static_cast<uint32_t>(out_width),
                           static_cast<uint32_t>(out_height),
                           static_cast<uint32_t>(batch * channel_blocks)};
  std::vector<uint32_t> lws = Default3DLocalWS(executor, gws, kwg_size_);

  MACE_OUT_OF_RANGE_INIT(kernel_);
  uint32_t idx = 0;
  MACE_OUT_OF_RANGE_SET_ARGS(kernel_);
  MACE_SET_3D_GWS_ARGS(kernel_, gws);
  kernel_.setArg(idx++, *input);
  kernel_.setArg(idx++, static_cast<int>(out_height));
  kernel_.setArg(idx++, static_cast<int>(out_width));
  kernel_.setArg(idx++, static_cast<int>(in_height));
  kernel_.setArg(idx++, static_cast<int>(in_width));
  kernel_.setArg(idx++, static_cast<int>(org_height));
  kernel_.setArg(idx++, static_cast<int>(org_width));
  kernel_.setArg(idx++, static_cast<int>(channel_blocks));
  kernel_.setArg(idx++, *output);

  std::string tuning_key = Concat(
      "reduce_opencl_kernel", gws[0], gws[1], gws[2]);

  MACE_RETURN_IF_ERROR(TuningOrRun3DKernel(executor, kernel_, tuning_key,
                                           gws, lws, context->future()));
  MACE_OUT_OF_RANGE_VALIDATION;

  return MaceStatus::MACE_SUCCESS;
}

MaceStatus ReduceKernel::Compute(
    OpContext *context,
    const Tensor *input,
    Tensor *output) {
  MACE_CHECK_NOTNULL(input);
  const index_t batch = input->dim(0);
  const index_t org_height = input->dim(1);
  const index_t org_width = input->dim(2);
  index_t in_height = org_height;
  index_t in_width = org_width;
  const index_t channels = input->dim(3);
  const index_t channel_blocks = RoundUpDiv4(channels);

  std::vector<index_t> output_shape{batch, 1, 1, channels};
  MACE_RETURN_IF_ERROR(output->Resize(output_shape));

  MaceStatus result = MaceStatus::MACE_RUNTIME_ERROR;
  auto *input_image = input->memory<cl::Image>();
  auto *output_image = output->mutable_memory<cl::Image>();
  if (in_height <= TILE_SIZE && in_width <= TILE_SIZE) {
    result = GraduallyComputeReduce(context, batch, channel_blocks, in_height,
                                    in_width, 1, 1, org_height, org_width,
                                    input_image, output_image);
  } else {
    auto out_height = RoundUpDiv(in_height, TILE_SIZE);
    auto out_width = RoundUpDiv(in_width, TILE_SIZE);
    const std::vector<index_t> inter_shape =
        {{batch, out_height, out_width, channels}};
    cl::Image *inter_image = GetScratchImage(context, input->memory_type(),
                                             input->dtype(), inter_shape);

    result = GraduallyComputeReduce(context, batch, channel_blocks, in_height,
                                    in_width, out_height, out_width,
                                    org_height, org_width,
                                    input_image, inter_image);
    MACE_RETURN_IF_ERROR(result);

    in_height = out_height;
    in_width = out_width;
    out_height = RoundUpDiv(in_height, TILE_SIZE);
    out_width = RoundUpDiv(in_width, TILE_SIZE);

    if (in_height > TILE_SIZE || in_width > TILE_SIZE) {
      const std::vector<index_t> inter2_shape =
          {{batch, out_height, out_width, channels}};
      cl::Image *inter2_image = GetScratchImage(context, input->memory_type(),
                                                input->dtype(), inter2_shape);

      while (out_height > 1 || out_width > 1) {
        result = GraduallyComputeReduce(context, batch, channel_blocks,
                                        in_height, in_width, out_height,
                                        out_width, org_height, org_width,
                                        inter_image, inter2_image);
        MACE_RETURN_IF_ERROR(result);
        in_height = out_height;
        in_width = out_width;
        out_height = RoundUpDiv(in_height, TILE_SIZE);
        out_width = RoundUpDiv(in_width, TILE_SIZE);
        std::swap(inter_image, inter2_image);
      }
    }

    result = GraduallyComputeReduce(context, batch, channel_blocks, in_height,
                                    in_width, 1, 1, org_height, org_width,
                                    inter_image, output_image);
  }

  return result;
}

}  // namespace image
}  // namespace opencl
}  // namespace ops
}  // namespace mace
