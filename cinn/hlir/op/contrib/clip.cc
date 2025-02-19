// Copyright (c) 2022 CINN Authors. All Rights Reserved.
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

#include "cinn/hlir/op/contrib/clip.h"

#include <gflags/gflags.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cinn/common/cas.h"
#include "cinn/common/common.h"
#include "cinn/common/context.h"
#include "cinn/common/macros.h"
#include "cinn/hlir/framework/node.h"
#include "cinn/hlir/framework/op.h"
#include "cinn/hlir/framework/op_strategy.h"
#include "cinn/hlir/pe/ir_schedule_pe.h"
#include "cinn/hlir/pe/nn.h"
#include "cinn/hlir/pe/schedule.h"
#include "cinn/ir/ir.h"
#include "cinn/ir/ir_base.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/tensor.h"
#include "cinn/lang/builtin.h"
#include "cinn/lang/compute.h"

DECLARE_bool(cinn_ir_schedule);

namespace cinn {
namespace hlir {
namespace op {

using common::_CINNValuePack_;
using common::CINNValue;
using common::CINNValuePack;
using framework::OpStrategy;
using framework::shape_t;
using framework::StrategyFunction;

std::vector<ir::Tensor> Clip(const ir::Tensor &in_tensor,
                             const float max_val,
                             const float min_val,
                             const std::string &output_name) {
  return {Compute(
      in_tensor->shape,
      [=](const std::vector<Expr> &indice) {
        ir::Tensor out_tensor(in_tensor);
        auto e = out_tensor(indice);
        return ir::Max::Make(ir::Min::Make(e, ir::Cast::Make(e->type(), Expr(max_val))),
                             ir::Cast::Make(e->type(), Expr(min_val)));
      },
      output_name)};
}

std::vector<std::vector<std::string>> InferLayoutForClip(const std::vector<framework::shape_t> &input_shapes,
                                                         const std::vector<std::string> &input_layouts,
                                                         const framework::NodeAttr &attrs,
                                                         const Target &target) {
  CHECK_EQ(input_layouts.size(), 1U) << "The input's layouts size is not 1! Please check again.";
  return {input_layouts, input_layouts};
}

std::vector<shape_t> InferShapeForClip(const std::vector<shape_t> &inputs_shape, const framework::AttrMapType &attrs) {
  CHECK_EQ(inputs_shape.size(), 1UL);
  std::vector<shape_t> res{inputs_shape[0]};
  return res;
}

std::vector<Type> InferDtypeForClip(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK(!inputs_type.empty()) << "The input's type size is 0! Please check again.";
  std::vector<Type> res{inputs_type[0]};
  return res;
}

std::shared_ptr<OpStrategy> StrategyForClip(const framework::NodeAttr &attrs,
                                            const std::vector<ir::Tensor> &inputs,
                                            const std::vector<Type> &out_type,
                                            const std::vector<std::vector<int>> &output_shapes,
                                            const Target &target) {
  CHECK(attrs.attr_store.count("max_val")) << "find no attr of max_val";
  CHECK(attrs.attr_store.count("min_val")) << "find no attr of min_val";
  float max_val = absl::get<float>(attrs.attr_store.at("max_val"));
  float min_val = absl::get<float>(attrs.attr_store.at("min_val"));

  std::string op_name("clip");

  framework::CINNCompute clip_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of " << op_name << " compute is empty! Please check.";
    CINNValuePack pack_args = args[0];
    CHECK_GE(pack_args.size(), 1U) << "1 input tensor for " << op_name << " compute";
    std::string tensor_name = UniqName(op_name + "_Out");
    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(pack_args.size(), 2U);
      tensor_name = pack_args[1].operator std::string();
    }
    Expr A_expr = pack_args[0];
    CHECK(A_expr.as_tensor());
    ir::Tensor A = A_expr.as_tensor_ref();
    auto out     = Clip(A, max_val, min_val, tensor_name);
    auto stages  = CreateStages({A});
    std::vector<CINNValue> res;
    for (auto &t : out) {
      stages->InsertLazily(t);
      res.push_back(CINNValue(t));
    }
    res.push_back(CINNValue(stages));
    *ret = CINNValuePack{res};
  });

  framework::CINNSchedule clip_schedule([=](lang::Args args, lang::RetValue *ret) {
    if (FLAGS_cinn_ir_schedule) {
      CHECK(!args.empty()) << "The input argument of " << op_name << " schedule is empty! Please check.";
      CINNValuePack arg_pack = args[0];
      Expr ast_expr          = arg_pack[0];
      std::vector<Expr> vec_ast{ast_expr};
      ir::ModuleExpr mod_expr(vec_ast);
      ir::IRSchedule ir_sch(mod_expr);
      if (target.arch == Target::Arch::NVGPU) {
        pe::IRCudaScheduleInjective(ir_sch, output_shapes.front(), target);
      } else if (target.arch == Target::Arch::X86) {
        pe::IRScheduleInjectiveCPU(ir_sch, output_shapes.front(), target);
      }
      std::vector<CINNValue> res;
      res.push_back(arg_pack[0]);
      *ret = CINNValuePack{res};
    } else {
      CHECK(!args.empty()) << "The input argument of " << op_name << " schedule is empty! Please check.";
      CINNValuePack arg_pack = args[0];
      CHECK_EQ(arg_pack.size(), 2UL);
      Expr Out              = arg_pack[0];
      poly::StageMap stages = arg_pack[1];
      CHECK(Out.as_tensor());
      if (target.arch == Target::Arch::NVGPU) {
        pe::CudaScheduleInjective(stages[Out.as_tensor_ref()], output_shapes.front(), target);
      } else if (target.arch == Target::Arch::X86) {
        pe::ScheduleInjectiveCPU(stages[Out.as_tensor_ref()], output_shapes.front(), target);
      }
      *ret = arg_pack;
    }
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(clip_compute, clip_schedule, "strategy.clip.x86", 1);

  return strategy;
}

}  // namespace op
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(clip_ops) {
  CINN_REGISTER_OP(clip)
      .describe("Clip the input tensors.")
      .set_num_outputs(1)
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyForClip)
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForClip))
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForClip))
#ifndef CINN_WITH_CUDA
      .set_attr("inferlayout", MakeOpFunction(cinn::hlir::op::InferLayoutForClip))
#endif
      .set_support_level(4);

  return true;
}
