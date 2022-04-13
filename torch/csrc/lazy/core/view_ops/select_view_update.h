#pragma once

#include <torch/csrc/lazy/ts_backend/ts_node.h>

namespace torch {
namespace lazy {

class TORCH_API SelectViewUpdate : public TsNode {
 public:
  SelectViewUpdate(
      const Value& target,
      const Value& source,
      int64_t dim,
      int64_t start,
      int64_t end,
      int64_t stride);

  bool Equal(
      const Value& target,
      const Value& source,
      int64_t dim,
      int64_t start,
      int64_t end,
      int64_t stride) const {
    size_t i = 0;
    return (
        operand(i++) == target && operand(i++) == source && dim_ == dim &&
        start_ == start && end_ == end && stride_ == stride);
  }

  std::string ToString() const override;

  int64_t dim() const {
    return dim_;
  }

  int64_t start() const {
    return start_;
  }

  int64_t end() const {
    return end_;
  }

  int64_t stride() const {
    return stride_;
  }

 private:
  int64_t dim_;
  int64_t start_;
  int64_t end_;
  int64_t stride_;
};

} // namespace lazy
} // namespace torch
