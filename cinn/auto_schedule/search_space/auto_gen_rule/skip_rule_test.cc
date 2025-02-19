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

#include "cinn/auto_schedule/search_space/auto_gen_rule/skip_rule.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <vector>

#include "cinn/auto_schedule/search_space/auto_gen_rule/auto_gen_rule.h"
#include "cinn/cinn.h"
#include "cinn/ir/ir.h"
#include "cinn/ir/ir_base.h"
#include "cinn/ir/ir_schedule.h"
#include "cinn/ir/tensor.h"
#include "cinn/lang/compute.h"
#include "cinn/lang/lower.h"
#include "cinn/poly/stage.h"

namespace cinn {
namespace auto_schedule {

TEST(SkipRule, Basic) {
  srand(0);
  Context::Global().ResetNameId();
#ifdef CINN_WITH_CUDA
  Target target = common::DefaultNVGPUTarget();
#else
  Target target = common::DefaultHostTarget();
#endif

  Expr M(32);
  Expr N(128);

  Placeholder<float> A("A", {M});
  Placeholder<float> B("B", {N});

  ir::Tensor C = Compute(
      {M, N}, [&](Var i, Var j) { return A(i) + B(j); }, "C");

  poly::StageMap stages              = CreateStages({C});
  std::vector<ir::LoweredFunc> funcs = lang::LowerVec("TestSkipRule_Basic", stages, {C}, {}, {}, nullptr, target, true);

  ir::Expr ast_expr = funcs[0]->body;
  VLOG(6) << "Expr before SkipRule: ";
  VLOG(6) << ast_expr;

  SkipRule skip_rule(target);
  ir::ModuleExpr mod_expr_before_skip(std::vector<ir::Expr>{ast_expr});
  EXPECT_EQ(skip_rule.Init(mod_expr_before_skip), RuleApplyType::kApply);

  EXPECT_EQ(skip_rule.NumberApplicable(), 1);
  ir::ModuleExpr mod_expr_after_skip = skip_rule.ApplyRandomly();
  std::vector<ir::Expr> exprs        = mod_expr_after_skip.GetExprs();
  EXPECT_EQ(exprs.size(), 1UL);

  EXPECT_EQ(ast_expr, exprs[0]);
}

}  // namespace auto_schedule
}  // namespace cinn
