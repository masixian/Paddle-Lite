// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
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

#include "lite/operators/slice_op.h"
#include <gtest/gtest.h>
#include <utility>
#include <vector>
#include "lite/core/op_lite.h"
#include "lite/core/op_registry.h"
#include "lite/kernels/mlu/bridges/test_helper.h"
#include "lite/kernels/npu/bridges/registry.h"

namespace paddle {
namespace lite {
namespace subgraph {
namespace mlu {

static void slice_ref(const float* input,
                      std::vector<int64_t> in_dims,
                      std::vector<int> axes,
                      std::vector<int> starts,
                      std::vector<int> ends,
                      float* out) {
  auto out_dims = in_dims;
  std::vector<int> real_starts(in_dims.size(), 0);
  std::vector<int> real_ends(in_dims.size(), 0);
  std::vector<int> real_step(in_dims.size(), 0);
  for (int i = 0; i < in_dims.size(); i++) {
    real_ends[i] = in_dims[i];
  }
  for (int i = 0; i < axes.size(); i++) {
    int dim_value = in_dims[axes[i]];
    if (dim_value > 0) {
      int start = starts[i] < 0 ? (starts[i] + dim_value) : starts[i];
      int end = ends[i] < 0 ? (ends[i] + dim_value) : ends[i];
      start = std::max(start, 0);
      end = std::max(end, 0);
      end = std::min(end, dim_value);
      out_dims[axes[i]] = end - start;
      real_starts[axes[i]] = start;
      real_ends[axes[i]] = end;
    }
  }
  const int LEN = in_dims.size();
  int dst_step[LEN];
  for (int i = 0; i < in_dims.size(); ++i) {
    dst_step[i] = 1;
  }
  int src_step[LEN];
  for (int i = 0; i < in_dims.size(); ++i) {
    src_step[i] = 1;
  }
  int out_num = out_dims[in_dims.size() - 1];
  for (int i = in_dims.size() - 2; i >= 0; i--) {
    dst_step[i] = out_dims[i + 1] * dst_step[i + 1];
    src_step[i] = in_dims[i + 1] * src_step[i + 1];
    out_num *= out_dims[i];
  }

  for (int dst_id = 0; dst_id < out_num; dst_id++) {
    int src_id = 0;
    int index_id = dst_id;
    for (int j = 0; j < out_dims.size(); j++) {
      int cur_id = index_id / dst_step[j];
      index_id = index_id % dst_step[j];
      src_id += (cur_id + real_starts[j]) * src_step[j];
    }
    out[dst_id] = input[src_id];
  }
}

static void test_case(std::vector<int64_t> x_shape,
                      std::vector<int64_t> out_shape,
                      std::vector<int> starts,
                      std::vector<int> ends,
                      std::vector<int> axes) {
  Scope scope;

  std::string x_var_name = "x";
  std::string out_var_name = "out";
  auto* x = scope.NewTensor(x_var_name);
  auto* out = scope.NewTensor(out_var_name);
  x->Resize(lite::DDim(x_shape));
  out->Resize(lite::DDim(out_shape));

  auto x_data = x->mutable_data<float>();
  FillTensor<float, float>(x, 0.f, 2.f);

  cpp::OpDesc opdesc;
  opdesc.SetType("slice");
  opdesc.SetInput("Input", {x_var_name});
  opdesc.SetOutput("Out", {out_var_name});
  opdesc.SetAttr("axes", axes);
  opdesc.SetAttr("starts", starts);
  opdesc.SetAttr("ends", ends);

  std::vector<float> out_ref(out->data_size(), 0);
  slice_ref(x_data, x_shape, axes, starts, ends, out_ref.data());

  Tensor input_x;
  input_x.Resize(x->dims());
  transpose(x->mutable_data<float>(),
            input_x.mutable_data<float>(),
            {static_cast<int>(x_shape[0]),
             static_cast<int>(x_shape[1]),
             static_cast<int>(x_shape[2]),
             static_cast<int>(x_shape[3])},
            {0, 2, 3, 1});
  x->CopyDataFrom(input_x);

  auto op = CreateOp<operators::SliceOp>(opdesc, &scope);
  LaunchOp(op, {x_var_name}, {out_var_name});

  Tensor output_trans;
  auto os = out->dims();
  output_trans.Resize(os);
  transpose(out->mutable_data<float>(),
            output_trans.mutable_data<float>(),
            {static_cast<int>(os[0]),
             static_cast<int>(os[2]),
             static_cast<int>(os[3]),
             static_cast<int>(os[1])},
            {0, 3, 1, 2});

  auto out_data = output_trans.mutable_data<float>();
  for (int i = 0; i < out->dims().production(); i++) {
    EXPECT_NEAR(out_ref[i], out_data[i], 1e-4);
  }
}

TEST(MLUBridges, slice) {
  /* test_case({3}, {3}, {-3}, {3}, {0}); */
  /* test_case({3, 4}, {3, 4}, {-3, 0}, {3, 100}, {0, 1}); */
  /* test_case({3, 4, 5}, {3, 4, 2}, {-3, 0, 2}, {3, 100, -1}, {0, 1, 2}); */
  test_case({3, 4, 5, 6}, {3, 4, 2, 6}, {-3, 0, 2}, {3, 100, -1}, {0, 1, 2});
  /* test_case({3, 4, 5, 6, 3}, {3, 4, 2, 6, 3}, {-3, 0, 2}, {3, 100, -1}, {0,
   * 1, 2}); */
  /* test_case({3, 4, 5, 6, 5, 2}, {3, 4, 2, 6, 5, 2}, {-3, 0, 2}, {3, 100, 1},
   * {0, 1, 2}); */
}

// void test_tensor_case1(lite::Tensor *x, lite::Tensor *out) {
//   std::vector<int64_t> x_shape({10});
//   x.Resize(lite::DDim(x_shape));
//   std::vector<int64_t> out_shape({5});
//   out.Resize(lite::DDim(out_shape));
//
//   auto x_data = x.mutable_data<float>();
//   auto out_data = out.mutable_data<float>();
//
//   for (int64_t i = 0; i < x.dims().production(); ++i) {
//     x_data[i] = static_cast<float>(i);
//   }
//
//   std::vector<int> starts({3});
//   std::vector<int> ends({8});
//   std::vector<int> axes({0});
//
//   // SliceCompute slice;
//   SliceCompute<float> slice;
//   operators::SliceParam param;
//
//   param.X = &x;
//   param.Out = &out;
//   param.axes = axes;
//   lite::Tensor starts_tensor, ends_tensor;
//   starts_tensor.Resize(DDim({1}));
//   ends_tensor.Resize(DDim({1}));
//   starts_tensor.mutable_data<int>()[0] = starts[0];
//   ends_tensor.mutable_data<int>()[0] = ends[0];
//   param.StartsTensor = &starts_tensor;
//   param.EndsTensor = &ends_tensor;
//
//   std::unique_ptr<KernelContext> ctx(new KernelContext);
//   ctx->As<X86Context>();
//   slice.SetContext(std::move(ctx));
//   slice.SetParam(param);
//   slice.Run();
//
//   std::vector<float> out_ref(out.numel(), 0);
//   slice_ref(x_data, x_shape, axes, starts, ends, out_ref.data());
//
//   for (int i = 0; i < out.dims().production(); i++) {
//     EXPECT_NEAR(out_ref[i], out_data[i], 1e-4);
//   }
// }
//
// void test_tensor_case3(lite::Tensor *x, lite::Tensor *out) {
//   std::vector<int64_t> x_shape({3, 4, 5});
//   x.Resize(lite::DDim(x_shape));
//   std::vector<int64_t> out_shape({3, 4, 2});
//   out.Resize(lite::DDim(out_shape));
//
//   auto x_data = x.mutable_data<float>();
//   auto out_data = out.mutable_data<float>();
//
//   for (int64_t i = 0; i < x.dims().production(); ++i) {
//     x_data[i] = static_cast<float>(i);
//   }
//
//   std::vector<int> starts({-3, 0, 2});
//   std::vector<int> ends({3, 100, -1});
//   std::vector<int> axes({0, 1, 2});
//
//   // SliceCompute slice;
//   SliceCompute<float> slice;
//   operators::SliceParam param;
//
//   param.X = &x;
//   param.Out = &out;
//   param.axes = axes;
//   lite::Tensor starts_tensor, ends_tensor;
//   starts_tensor.Resize(DDim({3}));
//   ends_tensor.Resize(DDim({3}));
//   for (int i = 0; i < starts.size(); ++i) {
//     starts_tensor.mutable_data<int>()[i] = starts[i];
//     ends_tensor.mutable_data<int>()[i] = ends[i];
//   }
//   param.StartsTensor = &starts_tensor;
//   param.EndsTensor = &ends_tensor;
//
//   std::unique_ptr<KernelContext> ctx(new KernelContext);
//   ctx->As<X86Context>();
//   slice.SetContext(std::move(ctx));
//   slice.SetParam(param);
//   slice.Run();
//
//   std::vector<float> out_ref(out.numel(), 0);
//   slice_ref(x_data, x_shape, axes, starts, ends, out_ref.data());
//
//   for (int i = 0; i < out.dims().production(); i++) {
//     EXPECT_NEAR(out_ref[i], out_data[i], 1e-4);
//   }
// }

// TEST(MLUBridges, slice_tensor) {
//   auto* x = scope.Var(x_var_name)->GetMutable<Tensor>();
//   auto* out = scope.Var(y_var_name)->GetMutable<Tensor>();
//
//   test_tensor_case1(x, out);
//   test_tensor_case3(x, out);
// }

// void test_tensor_list_case1(lite::Tensor x, lite::Tensor out) {
//   std::vector<int64_t> x_shape({10});
//   x.Resize(lite::DDim(x_shape));
//   std::vector<int64_t> out_shape({5});
//   out.Resize(lite::DDim(out_shape));
//
//   auto x_data = x.mutable_data<float>();
//   auto out_data = out.mutable_data<float>();
//
//   for (int64_t i = 0; i < x.dims().production(); ++i) {
//     x_data[i] = static_cast<float>(i);
//   }
//
//   std::vector<int> starts({3});
//   std::vector<int> ends({8});
//   std::vector<int> axes({0});
//
//   // SliceCompute slice;
//   SliceCompute<float> slice;
//   operators::SliceParam param;
//
//   param.X = &x;
//   param.Out = &out;
//   param.axes = axes;
//   param.StartsTensorList.clear();
//   param.EndsTensorList.clear();
//   lite::Tensor starts_tensor, ends_tensor;
//   for (int i = 0; i < 1; ++i) {
//     starts_tensor.Resize(DDim({1}));
//     ends_tensor.Resize(DDim({1}));
//     starts_tensor.mutable_data<int>()[0] = starts[0];
//     ends_tensor.mutable_data<int>()[0] = ends[0];
//     param.StartsTensorList.push_back(&starts_tensor);
//     param.EndsTensorList.push_back(&ends_tensor);
//   }
//
//   std::unique_ptr<KernelContext> ctx(new KernelContext);
//   ctx->As<X86Context>();
//   slice.SetContext(std::move(ctx));
//   slice.SetParam(param);
//   slice.Run();
//
//   std::vector<float> out_ref(out.numel(), 0);
//   slice_ref(x_data, x_shape, axes, starts, ends, out_ref.data());
//
//   for (int i = 0; i < out.dims().production(); i++) {
//     EXPECT_NEAR(out_ref[i], out_data[i], 1e-4);
//   }
// }
//
// void test_tensor_list_case3(lite::Tensor x, lite::Tensor out) {
//   std::vector<int64_t> x_shape({3, 4, 5});
//   x.Resize(lite::DDim(x_shape));
//   std::vector<int64_t> out_shape({3, 4, 2});
//   out.Resize(lite::DDim(out_shape));
//
//   auto x_data = x.mutable_data<float>();
//   auto out_data = out.mutable_data<float>();
//
//   for (int64_t i = 0; i < x.dims().production(); ++i) {
//     x_data[i] = static_cast<float>(i);
//   }
//
//   std::vector<int> starts({-3, 0, 2});
//   std::vector<int> ends({3, 100, -1});
//   std::vector<int> axes({0, 1, 2});
//
//   // SliceCompute slice;
//   SliceCompute<float> slice;
//   operators::SliceParam param;
//
//   param.X = &x;
//   param.Out = &out;
//   param.axes = axes;
//   param.StartsTensorList.clear();
//   param.EndsTensorList.clear();
//   lite::Tensor starts_tensor0, ends_tensor0;
//   lite::Tensor starts_tensor1, ends_tensor1;
//   lite::Tensor starts_tensor2, ends_tensor2;
//   starts_tensor0.Resize(DDim({1}));
//   starts_tensor1.Resize(DDim({1}));
//   starts_tensor2.Resize(DDim({1}));
//   ends_tensor0.Resize(DDim({1}));
//   ends_tensor1.Resize(DDim({1}));
//   ends_tensor2.Resize(DDim({1}));
//   starts_tensor0.mutable_data<int>()[0] = starts[0];
//   starts_tensor1.mutable_data<int>()[0] = starts[1];
//   starts_tensor2.mutable_data<int>()[0] = starts[2];
//   ends_tensor0.mutable_data<int>()[0] = ends[0];
//   ends_tensor1.mutable_data<int>()[0] = ends[1];
//   ends_tensor2.mutable_data<int>()[0] = ends[2];
//   param.StartsTensorList.emplace_back(&starts_tensor0);
//   param.StartsTensorList.emplace_back(&starts_tensor1);
//   param.StartsTensorList.emplace_back(&starts_tensor2);
//   param.EndsTensorList.emplace_back(&ends_tensor0);
//   param.EndsTensorList.emplace_back(&ends_tensor1);
//   param.EndsTensorList.emplace_back(&ends_tensor2);
//
//   std::unique_ptr<KernelContext> ctx(new KernelContext);
//   ctx->As<X86Context>();
//   slice.SetContext(std::move(ctx));
//   slice.SetParam(param);
//   slice.Run();
//
//   std::vector<float> out_ref(out.numel(), 0);
//   slice_ref(x_data, x_shape, axes, starts, ends, out_ref.data());
//
//   for (int i = 0; i < out.dims().production(); i++) {
//     EXPECT_NEAR(out_ref[i], out_data[i], 1e-4);
//   }
// }

// TEST(MLUBridges, slice_tensor_list) {
//   lite::Tensor x;
//   lite::Tensor out;
//
//   test_tensor_list_case1(x, out);
//   test_tensor_list_case3(x, out);
// }

}  // namespace mlu
}  // namespace subgraph
}  // namespace lite
}  // namespace paddle

USE_SUBGRAPH_BRIDGE(slice, kMLU);