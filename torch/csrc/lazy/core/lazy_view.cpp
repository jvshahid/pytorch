#include <torch/csrc/lazy/core/lazy_view.h>

#include <torch/csrc/lazy/core/helpers.h>
#include <torch/csrc/lazy/core/permutation_util.h>
#include <torch/csrc/lazy/core/view_ops/as_strided.h>
#include <torch/csrc/lazy/core/view_ops/as_strided_view_update.h>
#include <torch/csrc/lazy/core/view_ops/diagonal.h>
#include <torch/csrc/lazy/core/view_ops/diagonal_view_update.h>
#include <torch/csrc/lazy/core/view_ops/narrow.h>
#include <torch/csrc/lazy/core/view_ops/narrow_view_update.h>
#include <torch/csrc/lazy/core/view_ops/permute.h>
#include <torch/csrc/lazy/core/view_ops/resize.h>
#include <torch/csrc/lazy/core/view_ops/select.h>
#include <torch/csrc/lazy/core/view_ops/squeeze.h>
#include <torch/csrc/lazy/core/view_ops/unsqueeze.h>
#include <torch/csrc/lazy/core/view_ops/select_view_update.h>
#include <torch/csrc/lazy/core/view_ops/view.h>

#include <c10/util/Exception.h>
#include <algorithm>
#include <functional>
#include <numeric>
#include "lazy/core/internal_ops/ltc_ops.h"

namespace torch {
namespace lazy {
namespace {

Value ApplyViewInfo(Value ir_value, const ViewInfo& view_info) {
  switch (view_info.view_type) {
    case ViewInfo::Type::kSelect:
      return ReuseOrMakeNode<Select>(
          OpKind(at::aten::select),
          ir_value,
          view_info.select->dim,
          view_info.select->start,
          view_info.select->end,
          view_info.select->stride);
    case ViewInfo::Type::kNarrow:
      return ReuseOrMakeNode<Narrow>(
          OpKind(at::aten::narrow),
          ir_value,
          view_info.indices,
          view_info.shape.sizes());
    case ViewInfo::Type::kNoOp:
      return ir_value;
    case ViewInfo::Type::kPermute:
      return ReuseOrMakeNode<Permute>(
          OpKind(at::aten::permute), ir_value, view_info.permutation);
    case ViewInfo::Type::kReshape:
      return ReuseOrMakeNode<View>(
          OpKind(at::aten::view), ir_value, view_info.shape.sizes().vec());
    case ViewInfo::Type::kResize:
      return ReuseOrMakeNode<Resize>(
          OpKind(at::aten::resize), ir_value, view_info.shape.sizes().vec());
    case ViewInfo::Type::kSqueeze:
      return ReuseOrMakeNode<Squeeze>(
          OpKind(at::aten::squeeze), ir_value, view_info.squeeze_index);
    case ViewInfo::Type::kUnsqueeze:
      return ReuseOrMakeNode<Unsqueeze>(
          OpKind(at::aten::unsqueeze), ir_value, view_info.squeeze_index);
    case ViewInfo::Type::kAsStrided:
      return ReuseOrMakeNode<AsStrided>(
          OpKind(at::aten::as_strided),
          ir_value,
          view_info.shape.sizes().vec(),
          view_info.as_strided->stride,
          view_info.as_strided->offset);
    case ViewInfo::Type::kDiagonal:
      return ReuseOrMakeNode<Diagonal>(
          OpKind(at::aten::diagonal),
          ir_value,
          view_info.diagonal->offset,
          view_info.diagonal->dim1,
          view_info.diagonal->dim2);
    default:
      TORCH_INTERNAL_ASSERT(
          false, "Invalid view type: ", GetEnumValue(view_info.view_type));
  }
}

Value ApplyUpdate(Value ir_value, const Alias::UpdateData& update_data) {
  // We first bring the source IR value forward, by reshaping and slicing.
  std::vector<Value> tmp_values({ir_value});
  for (const ViewInfo& view_info : update_data.view_infos) {
    tmp_values.push_back(ApplyViewInfo(tmp_values.back(), view_info));
  }
  // We then move backward given the source update value, by reshaping and
  // slice-updating.
  Value result = update_data.ir_value;
  for (size_t i = update_data.view_infos.size(); i > 0; --i) {
    const ViewInfo& view_info = update_data.view_infos[i - 1];
    switch (view_info.view_type) {
      case ViewInfo::Type::kSelect:
        result = ReuseOrMakeNode<SelectViewUpdate>(
            OpKind(ltc_select_view_update),
            tmp_values[i - 1],
            result,
            view_info.select->dim,
            view_info.select->start,
            view_info.select->end,
            view_info.select->stride);
        break;
      case ViewInfo::Type::kNarrow:
        result = ReuseOrMakeNode<NarrowViewUpdate>(
            ltc_narrow_view_update,
            tmp_values[i - 1],
            result,
            view_info.indices);
        break;
      case ViewInfo::Type::kNoOp:
        break;
      case ViewInfo::Type::kPermute:
        result = ReuseOrMakeNode<Permute>(
            OpKind(at::aten::permute),
            result,
            InversePermutation(view_info.permutation));
        break;
      case ViewInfo::Type::kReshape:
        result = ReuseOrMakeNode<View>(
            OpKind(at::aten::view),
            result,
            view_info.source_shape.sizes().vec());
        break;
      case ViewInfo::Type::kResize:
        result = ReuseOrMakeNode<Resize>(
            OpKind(at::aten::resize),
            result,
            view_info.source_shape.sizes().vec());
        break;
      case ViewInfo::Type::kSqueeze:
        result = ReuseOrMakeNode<torch::lazy::Unsqueeze>(
            OpKind(at::aten::unsqueeze), ir_value, view_info.squeeze_index);
        break;
      case ViewInfo::Type::kUnsqueeze:
        result = ReuseOrMakeNode<torch::lazy::Squeeze>(
            OpKind(at::aten::squeeze), ir_value, view_info.squeeze_index);
        break;
      case ViewInfo::Type::kAsStrided:
        result = ReuseOrMakeNode<AsStridedViewUpdate>(
            ltc_as_strided_view_update,
            tmp_values[i - 1],
            result,
            view_info.source_shape.sizes().vec(),
            view_info.as_strided->stride,
            view_info.as_strided->offset);
        break;
      case ViewInfo::Type::kDiagonal:
        result = ReuseOrMakeNode<DiagonalViewUpdate>(
            ltc_diagonal_view_update,
            tmp_values[i - 1],
            result,
            view_info.diagonal->offset,
            view_info.diagonal->dim1,
            view_info.diagonal->dim2);
        break;
      default:
        TORCH_INTERNAL_ASSERT(
            false, "Invalid view type: ", GetEnumValue(view_info.view_type));
    }
  }
  return result;
}

} // namespace

ViewInfo::ViewInfo(Type view_type, Shape shape, Shape source_shape)
    : view_type(view_type),
      shape(std::move(shape)),
      indices(source_shape.dim(), 0),
      source_shape(std::move(source_shape)) {}

ViewInfo::ViewInfo(Type view_type, Shape shape, Shape source_shape, int64_t sqi)
    : view_type(view_type),
      shape(std::move(shape)),
      source_shape(std::move(source_shape)),
      squeeze_index(sqi)
{
  TORCH_CHECK(view_type == Type::kSqueeze);
}

ViewInfo::ViewInfo(
    Type view_type,
    Shape source_shape,
    std::vector<int64_t> permutation)
    : view_type(view_type),
      shape(Permute::MakePermuteShape(source_shape, permutation)),
      source_shape(std::move(source_shape)),
      permutation(std::move(permutation)) {
  TORCH_CHECK(view_type == Type::kPermute);
}

ViewInfo::ViewInfo(Type view_type, const Shape& source_shape, SelectInfo select)
    : view_type(view_type),
      shape(Select::MakeSelectShape(
          source_shape,
          select.dim,
          select.start,
          select.end,
          select.stride)),
      source_shape(source_shape),
      select(select) {
  TORCH_CHECK(view_type == Type::kSelect);
}

ViewInfo::ViewInfo(
    Type view_type,
    Shape shape,
    Shape source_shape,
    AsStridedInfo as_strided)
    : view_type(view_type),
      shape(std::move(shape)),
      source_shape(std::move(source_shape)),
      as_strided(std::move(as_strided)) {
  TORCH_CHECK(view_type == Type::kAsStrided);
}

ViewInfo::ViewInfo(
    Type view_type,
    const Shape& source_shape,
    DiagonalInfo diagonal)
    : view_type(view_type),
      shape(Diagonal::MakeDiagonalShape(
          source_shape,
          diagonal.offset,
          diagonal.dim1,
          diagonal.dim2)),
      source_shape(source_shape),
      diagonal(diagonal) {
  TORCH_CHECK(view_type == Type::kDiagonal);
}

void Alias::Update(Value ir_value, std::vector<ViewInfo> view_infos) {
  if (!updates_.empty() && updates_.back().view_infos == view_infos) {
    updates_.back().ir_value = std::move(ir_value);
  } else {
    updates_.push_back({std::move(ir_value), std::move(view_infos)});
  }
  ++generation_;
}

Value Alias::SyncUpdateOperations() {
  for (auto& update_data : updates_) {
    root_ir_value_ = ApplyUpdate(root_ir_value_, update_data);
  }
  updates_.clear();
  return root_ir_value_;
}

LazyView::LazyView(
    Shape shape,
    std::shared_ptr<Alias> alias,
    ViewInfo view_info)
    : shape_(std::move(shape)), alias_(std::move(alias)) {
  view_infos_.push_back(std::move(view_info));
}

LazyView::LazyView(
    Shape shape,
    std::shared_ptr<Alias> alias,
    std::vector<ViewInfo> view_infos)
    : view_infos_(std::move(view_infos)),
      shape_(std::move(shape)),
      alias_(std::move(alias)) {}

void LazyView::Update(Value ir_value) {
  alias_->Update(std::move(ir_value), view_infos_);
}

std::shared_ptr<LazyView> LazyView::CreateSubView(
    Shape shape,
    ViewInfo view_info) {
  std::vector<ViewInfo> view_infos(view_infos_);
  view_infos.push_back(std::move(view_info));
  return std::make_shared<LazyView>(
      std::move(shape), alias_, std::move(view_infos));
}

std::tuple<Value, bool> LazyView::GetViewIrNode() {
  if (IsUpToDate()) {
    return std::make_tuple(ir_value_, false);
  }
  Value update = alias_->SyncUpdateOperations();
  for (auto& view_info : view_infos_) {
    update = ApplyViewInfo(update, view_info);
  }
  ir_value_ = update;
  generation_ = alias_->generation();
  return std::make_tuple(ir_value_, true);
}

} // namespace lazy
} // namespace torch
