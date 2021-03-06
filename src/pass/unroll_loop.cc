/*!
 *  Copyright (c) 2017 by Contributors
 *  Loop unrolling as in Halide pipeline.
 * \file unroll_loop.cc
 */
// Unrolls the loop as in Halide pipeline.
#include <tvm/ir.h>
#include <tvm/ir_pass.h>
#include <tvm/ir_mutator.h>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "../arithmetic/compute_expr.h"

namespace tvm {
namespace ir {

class LoopUnroller : public IRMutator {
 public:
  explicit LoopUnroller(int auto_max_step,
                        int auto_max_depth,
                        bool explicit_unroll)
      : auto_max_step_(auto_max_step),
        auto_max_depth_(auto_max_depth),
        explicit_unroll_(explicit_unroll) {
  }

  Stmt Mutate_(const For* op, const Stmt& s) {
    Stmt stmt = IRMutator::Mutate_(op, s);
    op = stmt.as<For>();
    // constant folding.
    Expr extent = ir::Simplify(op->extent);
    const IntImm* v1 = extent.as<IntImm>();
    const UIntImm* v2 = extent.as<UIntImm>();
    int value = -1;
    if (v1 != nullptr) {
      value = static_cast<int>(v1->value);
    }
    if (v2 != nullptr) {
      value = static_cast<int>(v2->value);
    }
    // condition for auto unroll
    bool auto_unroll = (
        op->for_type == ForType::Serial &&
        normal_loop_depth_ == 0 &&
        value >= 0 &&
        unroll_depth_ <= auto_max_depth_ &&
        value * step_count_ <= auto_max_step_);

    if (op->for_type == ForType::Unrolled) {
      CHECK_GE(value, 0)
          << "Cannot unroll non-constant loop";
      auto_unroll = true;
    }

    if (auto_unroll) {
      step_count_  *=  value;
      unroll_depth_ += 1;
    } else {
      normal_loop_depth_ += 1;
    }

    if (auto_unroll && explicit_unroll_) {
      using arith::ComputeExpr;
      if (value == 0) return Evaluate::make(0);
      Stmt body = op->body;
      Map<Var, Expr> vmap;
      Stmt unrolled;
      for (int i = 0; i < value; ++i) {
        Var lv(op->loop_var.node_);
        vmap.Set(lv,
                 ComputeExpr<Add>(
                     op->min, make_const(op->loop_var.type(), i)));
        Stmt step = Substitute(body, vmap);
        if (unrolled.defined()) {
          unrolled = Block::make(unrolled, step);
        } else {
          unrolled = step;
        }
      }
      return unrolled;
    } else {
      if (auto_unroll) {
        if (op->for_type != ForType::Unrolled) {
          return For::make(
              op->loop_var, op->min, op->extent,
              ForType::Unrolled, op->device_api, op->body);
        }
      }
      return stmt;
    }
  }

  Stmt Mutate_(const Store* op, const Stmt& stmt) final {
    ++step_count_;
    return IRMutator::Mutate_(op, stmt);
  }

  Stmt Mutate_(const Evaluate* op, const Stmt& stmt) final {
    ++step_count_;
    return IRMutator::Mutate_(op, stmt);
  }

  Stmt Mutate_(const Block* op, const Stmt& stmt) final {
    Stmt first = this->Mutate(op->first);
    // cleanup state
    int step_count = step_count_;
    int unroll_depth = unroll_depth_;
    int normal_loop_depth = normal_loop_depth_;
    step_count_ = 0;
    unroll_depth_ = 0;
    normal_loop_depth_ = 0;
    // work on rest part
    Stmt rest = this->Mutate(op->rest);
    step_count_ += step_count;
    normal_loop_depth_ = std::max(normal_loop_depth, normal_loop_depth_);
    unroll_depth_ = std::max(unroll_depth_, unroll_depth);
    if (first.same_as(op->first) &&
        rest.same_as(op->rest)) {
      return stmt;
    } else {
      return Block::make(first, rest);
    }
  }

 private:
  // maximum number of step to perform auto unroll.
  int auto_max_step_;
  int auto_max_depth_;
  bool explicit_unroll_;
  // Number of normal loops in scope
  int normal_loop_depth_{0};
  // number of unrolled cases in current scope.
  int unroll_depth_{0};
  // Number of total steps unrolled
  int step_count_{0};
};


Stmt UnrollLoop(Stmt stmt,
                int auto_max_step,
                int auto_max_depth,
                bool explicit_unroll) {
  Stmt ret = LoopUnroller(
      auto_max_step,
      auto_max_depth,
      explicit_unroll).Mutate(stmt);
  if (!ret.same_as(stmt)) {
    return ConvertSSA(ret);
  } else {
    return ret;
  }
}

}  // namespace ir
}  // namespace tvm
