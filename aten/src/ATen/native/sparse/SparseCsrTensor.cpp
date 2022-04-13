// Basic functions on sparse tensors
#define TORCH_ASSERT_ONLY_METHOD_OPERATORS

#include <ATen/core/Tensor.h>
#include <ATen/Dispatch.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/Layout.h>
#include <ATen/Parallel.h>
#include <ATen/SparseCsrTensorImpl.h>
#include <ATen/SparseCsrTensorUtils.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/native/LinearAlgebraUtils.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/_nnz_native.h>
#include <ATen/ops/_sparse_compressed_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_csr_tensor_unsafe_native.h>
#include <ATen/ops/_validate_sparse_compressed_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_csr_tensor_args_native.h>
#include <ATen/ops/clone_native.h>
#include <ATen/ops/col_indices_native.h>
#include <ATen/ops/copy_native.h>
#include <ATen/ops/crow_indices_native.h>
#include <ATen/ops/empty.h>
#include <ATen/ops/empty_like_native.h>
#include <ATen/ops/empty_native.h>
#include <ATen/ops/resize_as_sparse_native.h>
#include <ATen/ops/resize_native.h>
#include <ATen/ops/sparse_csr_tensor_native.h>
#include <ATen/ops/values_native.h>
#endif

namespace at {
namespace native {

using namespace at::sparse_csr;

namespace {
} // end anonymous namespace

void _validate_sparse_compressed_tensor_args(const Tensor& compressed_indices, const Tensor& plain_indices, const Tensor& values, IntArrayRef size, c10::optional<Layout> layout_) {
  Layout layout = layout_.value_or(kDummyLayout);
  const std::string layout_name = at::sparse_csr::layoutToString(layout, /*upper=*/ true);
  const std::string compressed_indices_name = compressedIndicesName(layout);
  const std::string plain_indices_name = plainIndicesName(layout);

  // Layout Invariants
  TORCH_CHECK(
      plain_indices.layout() == kStrided && plain_indices.is_contiguous(),
      "expected ", plain_indices_name, " to be a strided and contiguous tensor");

  TORCH_CHECK(
      compressed_indices.layout() == kStrided && compressed_indices.is_contiguous(),
      "expected ", compressed_indices_name ," to be a strided and contiguous tensor");

  TORCH_CHECK(
      values.layout() == kStrided && values.is_contiguous(),
      "expected values to be a strided and contiguous tensor");

  // Shape and Strides invariants
  switch (layout) {
  case kSparseCsr:
  case kSparseCsc:
    TORCH_CHECK(
                size.size() >= 2,
                "size of a batched ", layout_name, " tensor must have length >= 2, but got: ",
                size.size());
    TORCH_CHECK(
                compressed_indices.dim() >= 1,
                compressed_indices_name, " must have dim >= 1 but got ", compressed_indices_name, ".dim() = ",
                compressed_indices.dim());
    TORCH_CHECK(
                plain_indices.dim() >= 1,
                plain_indices_name, " must have dim >= 1 but got ", plain_indices_name, ".dim() = ",
                plain_indices.dim());
    TORCH_CHECK(
                values.dim() >= 1,
                "values must have dim >= 1 but got values.dim() = ",
                values.dim());
    break;
  case kSparseBsr:
  case kSparseBsc:
    TORCH_CHECK(false, "_validate_sparse_csr_tensor_args: layout ", layout, " is not yet supported");  // TODO
    break;
  default:
    TORCH_CHECK(false, "_validate_sparse_csr_tensor_args: layout ", layout, " is not supported");
  }

  TORCH_CHECK(
      compressed_indices.dim() == plain_indices.dim(),
      "Number of dimensions of ", compressed_indices_name, " and ", plain_indices_name, " must be the same.");
  TORCH_CHECK(
      compressed_indices.dim() == values.dim(),
      "Number of dimensions of indices and values must be the same.");
  TORCH_CHECK(
      static_cast<size_t>(compressed_indices.dim()) == size.size() - 1,
      "Number of dimensions of indices must be one less than the number of dimensions of the provided size.");

  // All batch sizes must be the same
  auto batch_size = size.slice(0, size.size() - 2);
  auto compressed_indices_batch_size = compressed_indices.sizes().slice(0, compressed_indices.dim() - 1);
  auto plain_indices_batch_size = plain_indices.sizes().slice(0, plain_indices.dim() - 1);
  auto values_batch_size = values.sizes().slice(0, values.dim() - 1);
  TORCH_CHECK(
      batch_size == compressed_indices_batch_size &&
      batch_size == plain_indices_batch_size &&
      batch_size == values_batch_size,
      "All batch dimensions of the provided size (", batch_size, "), indices (",
      compressed_indices_batch_size,", ", plain_indices_batch_size, "), and values (",
      values_batch_size,") must be the same.");
  // Note, this check also enforces `compressed_indices.size(-1) >= 1`
  TORCH_CHECK(
              compressed_indices.size(-1) == (size[size.size() - 2] + 1),  // TODO: BSR/BSC
              compressed_indices_name, ".size(-1) must be equal to size[-2] + 1 (that is ", size[size.size() - 2] + 1, "), but got: ",
              compressed_indices.size(-1));
  TORCH_CHECK(
      plain_indices.numel() == values.numel(),
      plain_indices_name, " and values must have the same number of elements, but got ", plain_indices_name, ".numel(): ",
      plain_indices.numel(),
      ", values.numel(): ",
      values.numel());

  // Indices invariants
  AT_DISPATCH_INDEX_TYPES(compressed_indices.scalar_type(), "csr_construct_check", [&] {
    Tensor compressed_indices_cpu = compressed_indices.to(kCPU);
    auto compressed_indices_data_ptr = compressed_indices_cpu.data_ptr<index_t>();
    auto batch_stride = compressed_indices_cpu.dim() >= 2 ? compressed_indices_cpu.stride(-2) : 0;
    switch (layout) {
    case kSparseCsr:
    case kSparseCsc:
      for (const auto batch_id : c10::irange(batchCount(compressed_indices_cpu))) {
        TORCH_CHECK(
                    compressed_indices_data_ptr[batch_id*batch_stride] == 0,
                    "(Batch element ", batch_id, ") ",
                    ": 0th value of ", compressed_indices_name, " must be 0, but it is ", compressed_indices_data_ptr[batch_id*batch_stride]);
        TORCH_CHECK(
                    compressed_indices_data_ptr[batch_id*batch_stride + compressed_indices.size(-1) - 1] == plain_indices.size(-1),
                    "(Batch element ", batch_id, ") ",
                    "last value of ", compressed_indices_name, " should be equal to the length of ", plain_indices_name, ".");
        for (int i =  1; i <= size[size.size() - 2]; i++) {
          TORCH_CHECK(
                      compressed_indices_data_ptr[batch_id*batch_stride + i - 1] <= compressed_indices_data_ptr[batch_id*batch_stride + i],
                      "(Batch element ", batch_id, ") ",
                      "at position i = ", i, ", the condition ", compressed_indices_name, "[i - 1] <= ", compressed_indices_name, "[i] fails");
        }
      }
      if (plain_indices.numel() > 0) {
        TORCH_CHECK(0 <= plain_indices.min().item<index_t>(), plain_indices_name, ".min() should be greater or equal to zero");
        TORCH_CHECK(size[size.size() - 1] > plain_indices.max().item<index_t>(), "size[-1] should be greater than ", plain_indices_name, ".max()");
      }
      break;
    case kSparseBsr:
    case kSparseBsc:
      TORCH_CHECK(false, "_validate_sparse_csr_tensor_args: layout ", layout, " is not yet supported");  // TODO
    default:
      TORCH_CHECK(false, "_validate_sparse_csr_tensor_args: layout ", layout, " is not supported");
    }
  });

  // CSR Type Invariants
  auto compressed_indices_type = compressed_indices.scalar_type();
  auto plain_indices_type = plain_indices.scalar_type();
  TORCH_CHECK(
      compressed_indices_type == plain_indices_type,
      "both ", compressed_indices_name, " and ", plain_indices_name, " should have the same type.");
  TORCH_CHECK(
      compressed_indices_type == kInt || compressed_indices_type == kLong,
      compressed_indices_name, " and ", plain_indices_name, " must be an int32 or int64 type, but got: ",
      compressed_indices_type);

  // CSR Device Invariants
  TORCH_CHECK(
      plain_indices.get_device() == compressed_indices.get_device(),
      compressed_indices_name, " and ", plain_indices_name, " devices (",
      compressed_indices.get_device(),
      ", ",
      plain_indices.get_device(),
      ") must match");
  TORCH_CHECK(
      compressed_indices.get_device() == values.get_device(),
      "device of ", compressed_indices_name, " (",
      compressed_indices.get_device(),
      ") must match device of values (",
      values.get_device(),
      ")");
  TORCH_CHECK(
      values.device().type() == kCPU || values.device().type() == kCUDA,
      "device type of values (",
      values.device().type(),
      ") must be CPU or CUDA");
}

void _validate_sparse_csr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args(crow_indices, col_indices, values, size, kSparseCsr);
}
void _validate_sparse_csc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args(ccol_indices, row_indices, values, size, kSparseCsc);
}
void _validate_sparse_bsr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args(crow_indices, col_indices, values, size, kSparseBsr);
}
void _validate_sparse_bsc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args(ccol_indices, row_indices, values, size, kSparseBsc);
}

// Construction of CSR tensors.
SparseCsrTensor new_compressed_tensor(const TensorOptions& options) {
  // TODO: remove this comment after enabling autograd support for CSR tensor
  // constructor.
  // TORCH_INTERNAL_ASSERT(impl::variable_excluded_from_dispatch());
  DispatchKey dispatch_key;
  Layout layout = options.layout();
  TORCH_CHECK_NOT_IMPLEMENTED(
    options.device().type() == kCPU || options.device().type() == kCUDA,
    "Could not run 'sparse_compressed_tensor' from the '", options.device(), "' device.)");

  if (options.device().is_cuda()) {
    dispatch_key = DispatchKey::SparseCsrCUDA;
  } else {
    dispatch_key = DispatchKey::SparseCsrCPU;
  }

  return detail::make_tensor<SparseCsrTensorImpl>(
    DispatchKeySet(dispatch_key), layout, options.dtype());
}

Tensor _sparse_compressed_tensor_unsafe(const Tensor& crow_indices, const Tensor& col_indices,
                                        const Tensor& values,
                                        IntArrayRef size,
                                        c10::optional<ScalarType> dtype,
                                        c10::optional<Layout> layout,
                                        c10::optional<Device> device,
                                        c10::optional<bool> pin_memory) {
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout.value()).device(device).pinned_memory(pin_memory);
  SparseCsrTensor self = new_compressed_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(crow_indices, col_indices, values, size);
  return self;
}

Tensor _sparse_csr_tensor_unsafe(const Tensor& crow_indices, const Tensor& col_indices,
                                 const Tensor& values,
                                 IntArrayRef size,
                                 c10::optional<ScalarType> dtype,
                                 c10::optional<Layout> layout,
                                 c10::optional<Device> device,
                                 c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return _sparse_compressed_tensor_unsafe(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor _sparse_csc_tensor_unsafe(const Tensor& ccol_indices, const Tensor& row_indices,
                                 const Tensor& values,
                                 IntArrayRef size,
                                 c10::optional<ScalarType> dtype,
                                 c10::optional<Layout> layout,
                                 c10::optional<Device> device,
                                 c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return _sparse_compressed_tensor_unsafe(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor _sparse_bsr_tensor_unsafe(const Tensor& crow_indices, const Tensor& col_indices,
                                 const Tensor& values,
                                 IntArrayRef size,
                                 c10::optional<ScalarType> dtype,
                                 c10::optional<Layout> layout,
                                 c10::optional<Device> device,
                                 c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return _sparse_compressed_tensor_unsafe(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor _sparse_bsc_tensor_unsafe(const Tensor& ccol_indices, const Tensor& row_indices,
                                 const Tensor& values,
                                 IntArrayRef size,
                                 c10::optional<ScalarType> dtype,
                                 c10::optional<Layout> layout,
                                 c10::optional<Device> device,
                                 c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return _sparse_compressed_tensor_unsafe(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

inline DimVector _estimate_sparse_compressed_tensor_size(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    Layout layout) {
  DimVector size = DimVector(IntArrayRef(plain_indices.sizes().data(), plain_indices.dim() - 1));
  switch (layout) {
  case kSparseCsr:
    if (plain_indices.size(-1) > 0) {
      size.push_back(compressed_indices.size(-1) - 1);

    } else {
      size.push_back(0);
    }
    AT_DISPATCH_INDEX_TYPES(plain_indices.scalar_type(), "csr_construct_check", [&] {
                                                                                size.push_back(plain_indices.max().item<index_t>() + 1);
                                                                              });
    break;
  case kSparseCsc:
    AT_DISPATCH_INDEX_TYPES(plain_indices.scalar_type(), "csr_construct_check", [&] {
                                                                                size.push_back(plain_indices.max().item<index_t>() + 1);
                                                                              });
    if (plain_indices.size(-1) > 0) {
      size.push_back(compressed_indices.size(-1) - 1);

    } else {
      size.push_back(0);
    }
    break;
  case kSparseBsr:
  case kSparseBsc:
    TORCH_CHECK(false, "estimate_sparse_compressed_tensor_size: layout ", layout, " is not yet supported");  // TODO
    break;
  default:
    TORCH_CHECK(false, "estimate_sparse_compressed_tensor_size: layout ", layout, " is not supported");
  }
  return size;
}

// TODO: This constructor should probably use an ATen abstract method in order
// to make autograd dispatch available for the CSR constructor. See the relevant
// note in native_functions.yaml.

Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  Layout layout_ = layout.value_or(kDummyLayout);
  TORCH_CHECK(layout_ != kDummyLayout, "sparse_compressed_tensor: expected layout.");
  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);

  _validate_sparse_compressed_tensor_args(compressed_indices, plain_indices, values, size, layout);

  return at::native::_sparse_compressed_tensor_unsafe(
      compressed_indices,
      plain_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  Layout layout_ = layout.value_or(kDummyLayout);
  TORCH_CHECK(layout_ != kDummyLayout, "sparse_compressed_tensor: expected layout.");
  IntArrayRef size = _estimate_sparse_compressed_tensor_size(compressed_indices, plain_indices, values, layout_);
  return sparse_compressed_tensor(compressed_indices, plain_indices, values, size, dtype, layout, device, pin_memory);
}

Tensor sparse_csr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return sparse_compressed_tensor(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_csc_tensor(
    const Tensor& ccol_indices,
    const Tensor& row_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return sparse_compressed_tensor(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_bsr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return sparse_compressed_tensor(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_bsc_tensor(
    const Tensor& ccol_indices,
    const Tensor& row_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  return sparse_compressed_tensor(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_csr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  IntArrayRef size = _estimate_sparse_compressed_tensor_size(crow_indices, col_indices, values, layout_);
  return sparse_compressed_tensor(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_csc_tensor(
    const Tensor& ccol_indices,
    const Tensor& row_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseCsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  IntArrayRef size = _estimate_sparse_compressed_tensor_size(ccol_indices, row_indices, values, layout_);
  return sparse_compressed_tensor(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_bsr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsr;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  IntArrayRef size = _estimate_sparse_compressed_tensor_size(crow_indices, col_indices, values, layout_);
  return sparse_compressed_tensor(crow_indices, col_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor sparse_bsc_tensor(
    const Tensor& ccol_indices,
    const Tensor& row_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  constexpr Layout expected_layout = kSparseBsc;
  Layout layout_ = layout.value_or(expected_layout);
  TORCH_CHECK(layout_ == expected_layout, "expected layout ", expected_layout, " but got ", layout_);
  IntArrayRef size = _estimate_sparse_compressed_tensor_size(ccol_indices, row_indices, values, layout_);
  return sparse_compressed_tensor(ccol_indices, row_indices, values, size, dtype, expected_layout, device, pin_memory);
}

Tensor empty_sparse_csr(
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  Layout layout_ = layout.value_or(kSparseCsr);
  const std::string layout_name = at::sparse_csr::layoutToString(layout_, /*upper=*/ true);
  TensorOptions options = TensorOptions().dtype(ScalarType::Long).layout(Layout::Strided).device(device).pinned_memory(pin_memory);
  Tensor crow_indices;
  Tensor col_indices;
  Tensor values;

  switch (layout_) {
  case kSparseCsr:
  case kSparseCsc:
    {
      TORCH_CHECK(size.size() >= 2, "torch.empty: Only batched sparse ", layout_name, " tensors are supported, but got size ", size);
      auto rows = size[size.size() - ((layout == kSparseCsr) ?  2 : 1)];
      auto crow_indices_size = DimVector(size.slice(0, size.size() - 2));
      crow_indices_size.push_back(rows + 1);
      auto col_indices_values_size = DimVector(size.slice(0, size.size() - 2));
      int64_t nnz = 0;
      col_indices_values_size.push_back(nnz);
      crow_indices = at::empty(crow_indices_size, options);
      col_indices = at::empty(col_indices_values_size, options);
      values = at::empty(col_indices_values_size, options.dtype(dtype));
    }
    break;
  case kSparseBsr:
  case kSparseBsc:
    TORCH_CHECK(size.size() >= 2, "torch.empty: At least 2D sparse ", layout_name, " tensors are supported.");
    TORCH_CHECK(false, "empty_sparse_csr: layout ", layout_, " is not yet supported");  // TODO: replace with implementation
    break;
  default:
    TORCH_CHECK(false, "empty_sparse_csr: layout ", layout_, " is not supported");
  }

  return at::native::_sparse_csr_tensor_unsafe(
      crow_indices,
      col_indices,
      values,
      size,
      dtype,
      layout,
      device,
      pin_memory);
}

const Tensor& resize_sparse_csr_(
    const Tensor& self,
    IntArrayRef size,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  Layout layout_ = self.layout();
  const std::string layout_name = at::sparse_csr::layoutToString(layout_, /*upper=*/ true);
  switch (layout_) {
  case kSparseCsr:
  case kSparseCsc:
    TORCH_CHECK(size.size() >= 2, "torch.resize_: Only batched sparse ", layout_name, " tensors are supported, but got size ", size);
    break;
  case kSparseBsr:
  case kSparseBsc:
    TORCH_CHECK(size.size() >= 2, "torch.resize_: At least 2D sparse ", layout_name, " tensors are supported, but got size ", size);  // FIXME
    break;
  default:
    TORCH_CHECK(false, "resize_sparse_csr_: layout ", layout_, " is not supported");
  }

  auto col_index = size.size() - (at::sparse_csr::isCompressedRow(layout_) ? 1 : 2);
  std::string plain_dimension_name = (at::sparse_csr::isCompressedRow(layout_) ? "columns" : "rows");
  TORCH_CHECK(
      self.size(col_index) <= size[col_index],
      "torch.resize_: Resizing ", plain_dimension_name, " of sparse ", layout_name, " tensors to a smaller value is not supported. ",
      "The original number of ", plain_dimension_name, " is ",
      self.size(col_index),
      " while the requested new number of ", plain_dimension_name, " is ", size[col_index], ".");
  get_sparse_csr_impl(self)->resize_(self._nnz(), size);
  return self;
}

Tensor& copy_sparse_csr_(Tensor& self, const Tensor& src, bool non_blocking) {
  TORCH_CHECK(
      self.sizes() == src.sizes(),
      "copy_sparse_csr_: only same size tensors are supported.");
  TORCH_CHECK(
      self.layout() == src.layout(),  // TODO: support CSR->BSR, CSC->BSC
      "copy between different layouts is not supported. Found self type = ",
      self.toString(),
      " and src type = ",
      src.toString());
  TORCH_CHECK(
      self._nnz() == src._nnz(),
      "copy_sparse_csr_: only tensors with the same number of specified elements are supported.");
  self.crow_indices().copy_(src.crow_indices(), non_blocking);
  self.col_indices().copy_(src.col_indices(), non_blocking);
  self.values().copy_(src.values(), non_blocking);
  get_sparse_csr_impl(self)->set_layout(src.layout());
  return self;
}

// Access members of CSR tensors.
int64_t _nnz_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->nnz();
}

Tensor values_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->values().alias();
}

Tensor crow_indices_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->crow_indices().alias();
}

Tensor col_indices_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->col_indices().alias();
}

bool _is_same_size_as_sparse_csr(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  return self.sizes().equals(src.sizes());
}

const SparseCsrTensor& resize_as_sparse_csr_(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  switch(self.layout()) {
  case kSparseCsr:
  case kSparseCsc:
  case kSparseBsr:
  case kSparseBsc:
    TORCH_CHECK(
                src.layout() == self.layout(),
                "resize_as_sparse_csr_: layout for self and src must be match but got ",
                self.layout(),
                " for self, and ",
                src.layout(),
                " for src");
    break;
  default:
    TORCH_CHECK(false, "resize_as_sparse_csr_: layout ", self.layout(), " is not supported");
  }
  if (!_is_same_size_as_sparse_csr(self, src)) {
    get_sparse_csr_impl(self)->resize_as_sparse_csr_tensor_(src);
  }
  return self;
}

SparseCsrTensor clone_sparse_csr(
    const SparseCsrTensor& self,
    c10::optional<c10::MemoryFormat> optional_memory_format) {
  TORCH_CHECK(
      !optional_memory_format.has_value(),
      "unsupported memory format option ",
      optional_memory_format.value());
  TensorOptions options = self.options();
  return at::native::_sparse_csr_tensor_unsafe(
                                               self.crow_indices().clone(),
                                               self.col_indices().clone(),
                                               self.values().clone(),
                                               self.sizes(),
                                               optTypeMetaToScalarType(options.dtype_opt()),
                                               self.layout(),
                                               options.device_opt(),
                                               options.pinned_memory_opt());
}

Tensor empty_like_sparse_csr(
    const Tensor& self,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<c10::MemoryFormat> optional_memory_format) {
  TensorOptions options_ = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);
  TensorOptions options =
      self.options()
          .merge_in(options_)
          .merge_memory_format(optional_memory_format);

  switch(options.layout()) {
  case kSparseCsr:
    //case kSparseCsc:
    //case kSparseBsr:
    //case kSparseBsc:
    return at::native::_sparse_csr_tensor_unsafe(
                                                 self.crow_indices().clone(),
                                                 self.col_indices().clone(),
                                                 at::empty(self.values().sizes(), options.layout(kStrided)),
                                                 self.sizes(),
                                                 dtype,
                                                 self.layout(),
                                                 device);
    break;
  // TODO: kSparse
  case kStrided:
    return at::native::empty_like(self, dtype, layout, device, pin_memory, optional_memory_format);
    break;
  default:
    TORCH_CHECK(false, "empty_like_sparse_csr: layout ", options.layout(), " is not supported");
  }
}

} // namespace native
} // namespace at
