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

#include "cinn/hlir/pe/reduction.h"

#include <iostream>
#include <vector>

#include "cinn/hlir/framework/node.h"
#include "cinn/hlir/framework/op.h"
#include "cinn/hlir/framework/op_strategy.h"
#include "cinn/hlir/pe/broadcast.h"
#include "cinn/hlir/pe/ir_schedule_pe.h"
#include "cinn/hlir/pe/schedule.h"
#include "cinn/hlir/pe/transform.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/ir_schedule.h"

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

using BlockReduceFunc = std::function<std::vector<ir::Tensor>(
    const ir::Tensor &, const std::vector<int> &, const bool, const std::string &)>;
using ReduceFunc =
    std::function<ir::Tensor(const ir::Tensor &, const std::vector<int> &, const bool, const std::string &)>;

#define STRATEGY_FOR_REDUCE(                                                                                  \
    op_name_, reduce_op_, gpu_reduce_with_last_axis_func, gpu_reduce_without_last_axis_func, cpu_reduce_func) \
  std::shared_ptr<OpStrategy> StrategyFor##reduce_op_(const framework::NodeAttr &attrs,                       \
                                                      const std::vector<ir::Tensor> &inputs,                  \
                                                      const std::vector<Type> &out_type,                      \
                                                      const std::vector<std::vector<int>> &output_shapes,     \
                                                      const Target &target) {                                 \
    return StrategyForReduce(attrs,                                                                           \
                             inputs,                                                                          \
                             out_type,                                                                        \
                             output_shapes,                                                                   \
                             target,                                                                          \
                             #op_name_,                                                                       \
                             gpu_reduce_with_last_axis_func,                                                  \
                             gpu_reduce_without_last_axis_func,                                               \
                             cpu_reduce_func);                                                                \
  }

std::shared_ptr<OpStrategy> StrategyForReduce(const framework::NodeAttr &attrs,
                                              const std::vector<ir::Tensor> &inputs,
                                              const std::vector<Type> &out_type,
                                              const std::vector<std::vector<int>> &output_shapes,
                                              const Target &target,
                                              const std::string &op_name,
                                              BlockReduceFunc gpu_reduce_with_last_axis_func,
                                              BlockReduceFunc gpu_reduce_without_last_axis_func,
                                              ReduceFunc cpu_reduce_func) {
  std::vector<int> reduce_axes;
  if (attrs.attr_store.count("dim")) {
    reduce_axes = absl::get<std::vector<int>>(attrs.attr_store.at("dim"));
    if (reduce_axes.empty()) {
      for (int i = 0; i < inputs[0]->shape.size(); ++i) {
        reduce_axes.push_back(i);
      }
    }
    std::sort(reduce_axes.begin(), reduce_axes.end());
    // check reduce_axes
    CHECK_LE(reduce_axes.size(), inputs[0]->shape.size());
    CHECK_LT(reduce_axes.back(), inputs[0]->shape.size());
    for (int idx = 1; idx < reduce_axes.size(); ++idx) {
      CHECK_NE(reduce_axes[idx - 1], reduce_axes[idx]);
    }
  } else {
    LOG(FATAL) << "reduce dimension is not set!";
  }

  bool keep_dim = false;
  if (attrs.attr_store.count("keep_dim")) {
    keep_dim = absl::get<bool>(attrs.attr_store.at("keep_dim"));
  }

  auto WithoutLastDimInReduce = [](const std::vector<ir::Expr> &inshape, const std::vector<int> &axes) {
    // if last axis is in reduce.
    if (std::find(axes.begin(), axes.end(), inshape.size() - 1) != axes.end() ||
        std::find(axes.begin(), axes.end(), -1) != axes.end()) {
      return false;
    }

    int sum_last_axes = 1;
    for (int idx = axes.back() + 1; idx < inshape.size(); ++idx) {
      sum_last_axes *= inshape[idx].as_int32();
    }

    if (sum_last_axes > 1) {
      return true;
    } else {
      return false;
    }
  };

  framework::CINNCompute reduction_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of " << op_name << " compute is empty! Please check.";
    CINNValuePack arg_packs = args[0];
    std::string tensor_name = UniqName(op_name + "_out");
    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(arg_packs.size(), 2U) << "There should be 2 input args for " << op_name << " compute";
      tensor_name = arg_packs[1].operator std::string();
    } else {
      CHECK_EQ(arg_packs.size(), 1U) << "There should be 1 input args for " << op_name << " compute";
    }
    Expr x_expr = arg_packs[0];
    CHECK(x_expr.as_tensor());
    ir::Tensor x = x_expr.as_tensor_ref();
    if (target == common::DefaultNVGPUTarget()) {
      if (!WithoutLastDimInReduce(inputs[0]->shape, reduce_axes)) {
        VLOG(3) << "Do Two Step Block Reduce Compute!";
        auto res    = gpu_reduce_with_last_axis_func(x, reduce_axes, keep_dim, tensor_name);
        auto stages = CreateStages(res);

        std::vector<CINNValue> cinn_values;
        for (auto &t : res) {
          cinn_values.emplace_back(t);
        }
        cinn_values.emplace_back(stages);
        *ret = CINNValuePack{cinn_values};
      } else {
        VLOG(3) << "Do Block Shuffle Reduce Compute!";
        auto res    = gpu_reduce_without_last_axis_func(x, reduce_axes, keep_dim, tensor_name);
        auto stages = CreateStages(res);

        std::vector<CINNValue> cinn_values;
        for (auto &t : res) {
          cinn_values.emplace_back(t);
        }
        cinn_values.emplace_back(stages);
        *ret = CINNValuePack{cinn_values};
      }
    } else {
      VLOG(3) << "Do Reduce Compute!";
      auto out    = cpu_reduce_func(x, reduce_axes, keep_dim, tensor_name);
      auto stages = CreateStages({out});

      std::vector<CINNValue> cinn_values{CINNValue(out), CINNValue(stages)};
      *ret = CINNValuePack{cinn_values};
    }
  });

  framework::CINNSchedule reduction_schedule([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of " << op_name << " schedule is empty! Please check.";
    CINNValuePack arg_pack = args[0];

    if (FLAGS_cinn_ir_schedule) {
      CHECK_GE(arg_pack.size(), 2UL);
      CHECK_LE(arg_pack.size(), 8UL);

      if (target.arch == Target::Arch::NVGPU) {
        if (!WithoutLastDimInReduce(inputs[0]->shape, reduce_axes)) {
          if (arg_pack.size() == 4) {
            Expr out     = arg_pack[0];
            Expr tmp_out = arg_pack[1];

            Expr expr0 = arg_pack[2];
            Expr expr1 = arg_pack[3];

            std::vector<Expr> vec_ast{expr0, expr1};
            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaScheduleBlockReduceInternal Schedule!";
            pe::IRCudaScheduleBlockReduceInternal(ir_sch, tmp_out.as_tensor_ref(), out.as_tensor_ref(), target);

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else if (arg_pack.size() == 6) {
            Expr out            = arg_pack[0];
            Expr tmp_out        = arg_pack[1];
            Expr reduce_tmp_out = arg_pack[2];

            Expr expr0 = arg_pack[3];
            Expr expr1 = arg_pack[4];
            Expr expr2 = arg_pack[5];

            std::vector<Expr> vec_ast{expr0, expr1, expr2};
            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaScheduleBlockReduce Schedule!";
            pe::IRCudaScheduleBlockReduce(
                ir_sch, reduce_tmp_out.as_tensor_ref(), tmp_out.as_tensor_ref(), out.as_tensor_ref(), target);

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else if (arg_pack.size() == 7) {
            Expr out            = arg_pack[0];
            Expr tmp_out        = arg_pack[1];
            Expr reduce_tmp_out = arg_pack[2];
            Expr reshape        = arg_pack[3];

            Expr expr0 = arg_pack[4];
            Expr expr1 = arg_pack[5];
            Expr expr2 = arg_pack[6];

            std::vector<Expr> vec_ast{expr0, expr1, expr2};
            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaTwoStepReduceSchedule Schedule!";
            pe::IRCudaTwoStepReduceSchedule(ir_sch,
                                            reshape.as_tensor_ref(),
                                            reduce_tmp_out.as_tensor_ref(),
                                            tmp_out.as_tensor_ref(),
                                            out.as_tensor_ref(),
                                            common::DefaultNVGPUTarget());

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else if (arg_pack.size() == 5) {
            Expr out            = arg_pack[0];
            Expr tmp_out        = arg_pack[1];
            Expr reduce_tmp_out = arg_pack[2];

            Expr expr0 = arg_pack[3];
            Expr expr1 = arg_pack[4];

            std::vector<Expr> vec_ast{expr0, expr1};
            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaScheduleBlockReduce Schedule!";
            pe::IRCudaScheduleBlockReduce(ir_sch,
                                          reduce_tmp_out.as_tensor_ref(),
                                          tmp_out.as_tensor_ref(),
                                          out.as_tensor_ref(),
                                          common::DefaultNVGPUTarget());

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else {
            LOG(FATAL) << "Unkown Reduce Type!";
          }
        } else {
          if (arg_pack.size() == 2) {
            Expr reduce_out = arg_pack[0];

            Expr expr0 = arg_pack[1];
            std::vector<Expr> vec_ast{expr0};

            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaScheduleReduce Schedule!";
            pe::IRCudaScheduleReduce(
                ir_sch, reduce_out.as_tensor_ref(), inputs[0]->shape.size() - reduce_axes.back() - 1, target);

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else if (arg_pack.size() == 5) {
            Expr reduce_out      = arg_pack[0];
            Expr reduce_internal = arg_pack[1];
            Expr reduce_reshape  = arg_pack[2];

            Expr expr0 = arg_pack[3];
            Expr expr1 = arg_pack[4];

            std::vector<Expr> vec_ast{expr0, expr1};
            ir::ModuleExpr model_expr(vec_ast);
            ir::IRSchedule ir_sch(model_expr);
            ir_sch.MergeExprs();

            VLOG(3) << "Do IRCudaScheduleBlockShuffleReduce Schedule!";
            pe::IRCudaScheduleBlockShuffleReduce(ir_sch,
                                                 reduce_reshape.as_tensor_ref(),
                                                 reduce_internal.as_tensor_ref(),
                                                 reduce_out.as_tensor_ref(),
                                                 target);

            std::vector<CINNValue> res{CINNValue(ir_sch.GetModule().GetExprs().at(0))};
            *ret = CINNValuePack{res};
          } else {
            LOG(FATAL) << "Unkown Reduce Type!";
          }
        }
      }
    } else {
      CHECK_GE(arg_pack.size(), 2UL);
      CHECK_LE(arg_pack.size(), 5UL);
      if (target.arch == Target::Arch::NVGPU) {
        if (!WithoutLastDimInReduce(inputs[0]->shape, reduce_axes)) {
          if (arg_pack.size() == 3) {
            Expr out              = arg_pack[0];
            Expr tmp_out          = arg_pack[1];
            poly::StageMap stages = arg_pack.back();
            VLOG(3) << "Do CudaBlockReduceInternalSchedule Schedule!";
            pe::CudaBlockReduceInternalSchedule(
                stages, tmp_out.as_tensor_ref(), out.as_tensor_ref(), common::DefaultNVGPUTarget());
          } else if (arg_pack.size() == 4) {
            Expr out              = arg_pack[0];
            Expr tmp_out          = arg_pack[1];
            Expr reduce_tmp_out   = arg_pack[2];
            poly::StageMap stages = arg_pack.back();
            VLOG(3) << "Do CudaBlockReduceSchedule Schedule!";
            pe::CudaBlockReduceSchedule(stages,
                                        reduce_tmp_out.as_tensor_ref(),
                                        tmp_out.as_tensor_ref(),
                                        out.as_tensor_ref(),
                                        common::DefaultNVGPUTarget());
          } else {
            Expr out              = arg_pack[0];
            Expr tmp_out          = arg_pack[1];
            Expr reduce_tmp_out   = arg_pack[2];
            Expr reshape          = arg_pack[3];
            poly::StageMap stages = arg_pack.back();
            VLOG(3) << "Do CudaTwoStepReduceSchedule Schedule!";
            pe::CudaTwoStepReduceSchedule(stages,
                                          reshape.as_tensor_ref(),
                                          reduce_tmp_out.as_tensor_ref(),
                                          tmp_out.as_tensor_ref(),
                                          out.as_tensor_ref(),
                                          common::DefaultNVGPUTarget());
          }
        } else {
          if (arg_pack.size() == 2) {
            Expr reduce_out       = arg_pack[0];
            poly::StageMap stages = arg_pack.back();
            VLOG(3) << "Do CudaReduceSchedule Schedule!";
            pe::CudaReduceSchedule(
                stages, reduce_out.as_tensor_ref(), inputs[0]->shape.size() - reduce_axes.back() - 1, target);
          } else {
            CHECK_EQ(arg_pack.size(), 4) << "args is not equal 4!";
            Expr reduce_reshape   = arg_pack[2];
            Expr reduce_internal  = arg_pack[1];
            Expr reduce_out       = arg_pack[0];
            poly::StageMap stages = arg_pack.back();
            VLOG(3) << "Do CudaBlockShuffleReduceSchedule Schedule!";
            pe::CudaBlockShuffleReduceSchedule(stages,
                                               reduce_reshape.as_tensor_ref(),
                                               reduce_internal.as_tensor_ref(),
                                               reduce_out.as_tensor_ref(),
                                               target);
          }
        }
      }
      *ret = arg_pack;
    }
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(reduction_compute, reduction_schedule, "strategy." + op_name + ".x86", 1);

  return strategy;
}

std::vector<shape_t> InferShapeForReduction(const std::vector<shape_t> &inputs_shape,
                                            const framework::AttrMapType &attrs) {
  CHECK(inputs_shape.size() == 1UL || inputs_shape.size() == 3UL);
  std::vector<int> dim;
  bool keep_dim = false;
  if (attrs.find("dim") != attrs.end()) {
    dim = absl::get<std::vector<int>>(attrs.at("dim"));
  }

  if (attrs.find("keep_dim") != attrs.end()) {
    keep_dim = absl::get<bool>(attrs.at("keep_dim"));
  }
  std::vector<int> out_shapes;
  if (!dim.empty()) {
    CHECK_LE(dim.size(), inputs_shape[0].size()) << "reduce dim should no more than the input size";
    auto ndim = inputs_shape[0].size();
    for (size_t i = 0; i < ndim; ++i) {
      if (std::find(dim.begin(), dim.end(), i) != dim.end()) {
        if (keep_dim) {
          out_shapes.push_back(1);
        }
      } else {
        out_shapes.push_back(inputs_shape[0][i]);
      }
    }
  }

  if (out_shapes.empty()) {
    out_shapes.push_back(1);
  }

  return {out_shapes};
}

std::vector<Type> InferDtypeForReduction(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK(!inputs_type.empty()) << "The input's type size is 0! Please check again.";
  std::vector<Type> res{inputs_type[0]};
  return res;
}

std::vector<std::vector<std::string>> InferLayoutForReduction(const std::vector<framework::shape_t> &input_shapes,
                                                              const std::vector<std::string> &input_layouts,
                                                              const framework::NodeAttr &attrs,
                                                              const Target &target) {
  CHECK_EQ(input_layouts.size(), 1U) << "The input's layouts size is not 1! Please check again.";
  std::vector<std::string> new_input_layouts = input_layouts;
  if (input_shapes[0].size() > 4) {
    // alter input layout back
    new_input_layouts[0] = "NCHW";
    VLOG(3) << "alter input layout from " << input_layouts[0] << " to " << new_input_layouts[0];
  }

  return {{""}, new_input_layouts};
}

std::vector<shape_t> InferShapeForBnOptimize(const std::vector<shape_t> &inputs_shape,
                                             const framework::AttrMapType &attrs) {
  auto shapes = InferShapeForReduction(inputs_shape, attrs);
  CHECK_GE(shapes.size(), 1) << "shapes's size less than 1, please check!";
  return {shapes[0], shapes[0]};
}

std::vector<Type> InferDtypeForBnOptimize(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK(!inputs_type.empty()) << "The input's type size is 0! Please check again.";
  return {inputs_type[0], inputs_type[0]};
}

std::vector<std::vector<std::string>> InferLayoutForBnOptimize(const std::vector<framework::shape_t> &input_shapes,
                                                               const std::vector<std::string> &input_layouts,
                                                               const framework::NodeAttr &attrs,
                                                               const Target &target) {
  return {{"", ""}, {"", ""}};
}

STRATEGY_FOR_REDUCE(reduce_sum, ReduceSum, pe::TwoStepBlockReduceSum, pe::BlockShuffleReduceSum, pe::ReduceSum);
STRATEGY_FOR_REDUCE(reduce_prod, ReduceProd, pe::TwoStepBlockReduceProd, pe::BlockShuffleReduceProd, pe::ReduceProd);
STRATEGY_FOR_REDUCE(reduce_max, ReduceMax, pe::TwoStepBlockReduceMax, pe::BlockShuffleReduceMax, pe::ReduceMax);
STRATEGY_FOR_REDUCE(reduce_min, ReduceMin, pe::TwoStepBlockReduceMin, pe::BlockShuffleReduceMin, pe::ReduceMin);

#undef STRATEGY_FOR_REDUCE

}  // namespace op
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(reduce_ops) {
#define CINN_REGISTER_REDUCTION(op__, op_stragegy__)                                                                  \
  CINN_REGISTER_OP(op__)                                                                                              \
      .describe(#op__ " function")                                                                                    \
      .set_num_inputs(1)                                                                                              \
      .set_num_outputs(1)                                                                                             \
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyFor##op_stragegy__)  \
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForReduction))                                 \
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForReduction))                                 \
      .set_attr("inferlayout", MakeOpFunction(cinn::hlir::op::InferLayoutForReduction))                               \
      .set_attr<cinn::hlir::framework::OpPatternKind>("OpPattern", cinn::hlir::framework::OpPatternKind::kCommReduce) \
      .set_support_level(4);

  CINN_REGISTER_REDUCTION(reduce_sum, ReduceSum);
  CINN_REGISTER_REDUCTION(reduce_prod, ReduceProd);
  CINN_REGISTER_REDUCTION(reduce_max, ReduceMax);
  CINN_REGISTER_REDUCTION(reduce_min, ReduceMin);

#undef CINN_REGISTER_REDUCTION

  return true;
}
