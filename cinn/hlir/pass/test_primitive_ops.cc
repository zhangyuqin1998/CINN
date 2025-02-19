// Copyright (c) 2021 CINN Authors. All Rights Reserved.
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

#include <gtest/gtest.h>

#include <memory>

#include "cinn/cinn.h"
#include "cinn/frontend/syntax.h"
#include "cinn/hlir/framework/graph.h"
#include "cinn/hlir/framework/graph_compiler.h"
#include "cinn/hlir/framework/pass.h"
#include "cinn/hlir/op/use_ops.h"
#include "cinn/hlir/pass/use_pass.h"

DEFINE_string(model_dir, "", "");

namespace cinn {
namespace frontend {

using hlir::framework::Scope;
using utils::Join;

Target GetTarget() {
#ifdef CINN_WITH_CUDA
  return common::DefaultNVGPUTarget();
#else
  return common::DefaultHostTarget();
#endif
}

void SetRandData(const hlir::framework::Tensor& tensor, Target target) {
#ifdef CINN_WITH_CUDA
  auto* data = tensor->mutable_data<float>(target);
  std::vector<float> host_memory(tensor->shape().numel(), 0);
  for (float& v : host_memory) {
    v = (rand() * 1.f) / RAND_MAX;  // All random data
  }
  CUDA_CALL(cudaMemcpy(reinterpret_cast<void*>(data),
                       host_memory.data(),
                       tensor->shape().numel() * sizeof(float),
                       cudaMemcpyHostToDevice));
#else
  auto* data = tensor->mutable_data<float>(target);
  for (size_t j = 0; j < tensor->shape().numel(); j++) {
    data[j] = (rand() * 1.f) / RAND_MAX;  // All random data
  }
#endif
}

// batch_norm primitives
TEST(batch_norm_meta, batch_norm_meta) {
  Placeholder A(Float(32), {1, 64, 112, 112}, "A");

  Placeholder Scale(Float(32), {64}, "Scale");
  Placeholder Bias(Float(32), {64}, "Bias");
  Placeholder Mean(Float(32), {64}, "Mean");
  Placeholder Variance(Float(32), {64}, "Variance");

  Program program;
  absl::flat_hash_map<std::string, Program::attr_t> attrs;
  attrs["epsilon"] = static_cast<float>(0.001);

  auto a = program.batchnorm(A, Scale, Bias, Mean, Variance, attrs);

  auto b = program.fused_batchnorm_inference(A, Scale, Bias, Mean, Variance, attrs);

  Target target = GetTarget();
  program.SetInputs({A});
  program.Validate();
  LOG(INFO) << "Program:\n" << program;
  auto graph = std::make_shared<hlir::framework::Graph>(program, target);

  hlir::framework::ApplyPass(graph.get(), "InferShape");
#ifndef CINN_WITH_CUDA
  hlir::framework::ApplyPass(graph.get(), "AlterLayout");
#endif
  hlir::framework::ApplyPass(graph.get(), "OpFusion");
  auto scope = BuildScope(target, graph);
  LOG(INFO) << "graph:\n" << graph->Visualize();

  hlir::framework::GraphCompiler gc(target, scope, graph);
  auto runtime_program = gc.Build();

  scope->Var<hlir::framework::Tensor>("A");

  auto A1 = scope->GetTensor("A");
  SetRandData(A1, target);

  runtime_program->Execute();
}

TEST(reduction, reduce) {
  Placeholder A(Float(32), {1, 3, 224, 224}, "A");

  Program program;
  std::unordered_map<std::string, Program::attr_t> attrs;
  std::vector<int> axis = {1, 2};
  bool keep_dim         = false;

  auto a = program.reduce_max(A, axis, keep_dim);
  auto b = program.reduce_min(A, axis, keep_dim);
  auto c = program.reduce_prod(A, axis, keep_dim);
  auto d = program.reduce_sum(A, {0, 1, 2, 3}, keep_dim);

  Target target = GetTarget();
  program.SetInputs({A});
  program.Validate();
  LOG(INFO) << "Program:\n" << program;
  auto graph = std::make_shared<hlir::framework::Graph>(program, target);

  hlir::framework::ApplyPass(graph.get(), "InferShape");
#ifndef CINN_WITH_CUDA
  hlir::framework::ApplyPass(graph.get(), "AlterLayout");
#endif
  hlir::framework::ApplyPass(graph.get(), "OpFusion");
  auto scope = BuildScope(target, graph);
  LOG(INFO) << "graph:\n" << graph->Visualize();

  hlir::framework::GraphCompiler gc(target, scope, graph);
  auto runtime_program = gc.Build();

  scope->Var<hlir::framework::Tensor>("A");

  auto A1 = scope->GetTensor("A");
  SetRandData(A1, target);

  runtime_program->Execute();
}

TEST(Compare, Compare) {
  Placeholder A(Float(32), {1, 3, 224, 224}, "A");
  Placeholder B(Float(32), {1, 3, 224, 224}, "B");

  Program program;
  auto a = program.primitive_equal(A, B);

  Target target = GetTarget();
  program.SetInputs({A, B});
  program.Validate();
  LOG(INFO) << "Program:\n" << program;
  auto graph = std::make_shared<hlir::framework::Graph>(program, target);

  hlir::framework::ApplyPass(graph.get(), "InferShape");
#ifndef CINN_WITH_CUDA
  hlir::framework::ApplyPass(graph.get(), "AlterLayout");
#endif
  hlir::framework::ApplyPass(graph.get(), "OpFusion");
  auto scope = BuildScope(target, graph);
  LOG(INFO) << "graph:\n" << graph->Visualize();

  hlir::framework::GraphCompiler gc(target, scope, graph);
  auto runtime_program = gc.Build();

  scope->Var<hlir::framework::Tensor>("A");
  scope->Var<hlir::framework::Tensor>("B");

  auto A1 = scope->GetTensor("A");
  auto B1 = scope->GetTensor("B");
  SetRandData(A1, target);
  SetRandData(B1, target);

  runtime_program->Execute();
}

}  // namespace frontend
}  // namespace cinn
