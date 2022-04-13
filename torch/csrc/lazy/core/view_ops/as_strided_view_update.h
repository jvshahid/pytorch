#pragma once

#include <torch/csrc/lazy/ts_backend/ts_node.h>

#include <vector>

namespace torch {
namespace lazy {

class TORCH_API AsStridedViewUpdate : public TsNode {
 public:
  AsStridedViewUpdate(
      const Value& target,
      const Value& input,
      std::vector<int64_t> size,
      std::vector<int64_t> stride,
      int64_t storage_offset);

  bool Equal(
      const Value& target,
      const Value& input,
      std::vector<int64_t> size,
      std::vector<int64_t> stride,
      int64_t storage_offset) const {
    size_t i = 0;
    return (
        operand(i++) == target && operand(i++) == input && size_ == size &&
        stride_ == stride && storage_offset_ == storage_offset);
  }

  std::string ToString() const override;

  const std::vector<int64_t>& size() const {
    return size_;
  }

  const std::vector<int64_t>& stride() const {
    return stride_;
  }

  int64_t storage_offset() const {
    return storage_offset_;
  }

 private:
  std::vector<int64_t> size_;
  std::vector<int64_t> stride_;
  int64_t storage_offset_;
};

} // namespace lazy
} // namespace torch
