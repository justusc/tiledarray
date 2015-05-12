/*
 *  This file is a part of TiledArray.
 *  Copyright (C) 2013  Virginia Tech
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef TILEDARRAY_TENSOR_H__INCLUDED
#define TILEDARRAY_TENSOR_H__INCLUDED

#include <TiledArray/perm_index.h>
#include <TiledArray/math/gemm_helper.h>
#include <TiledArray/math/blas.h>
#include <TiledArray/math/transpose.h>
//#include <TiledArray/tile_op/tile_interface.h>

namespace TiledArray {

  /// An N-dimensional tensor object

  /// \tparam T the value type of this tensor
  /// \tparam A The allocator type for the data
  template <typename T, typename A = Eigen::aligned_allocator<T> >
  class Tensor {
  private:
    // Internal type for enabling various constructors.
    struct Enabler { };
  public:
    typedef Tensor<T, A> Tensor_; ///< This class type
    typedef Range range_type; ///< Tensor range type
    typedef typename range_type::size_type size_type; ///< size type
    typedef A allocator_type; ///< Allocator type
    typedef typename allocator_type::value_type value_type; ///< Array element type
    typedef typename allocator_type::reference reference; ///< Element reference type
    typedef typename allocator_type::const_reference const_reference; ///< Element reference type
    typedef typename allocator_type::pointer pointer; ///< Element pointer type
    typedef typename allocator_type::const_pointer const_pointer; ///< Element const pointer type
    typedef typename allocator_type::difference_type difference_type; ///< Difference type
    typedef pointer iterator; ///< Element iterator type
    typedef const_pointer const_iterator; ///< Element const iterator type
    typedef typename TiledArray::detail::scalar_type<T>::type
        numeric_type; ///< the numeric type that supports T

    template <typename U>
    using param_type = TiledArray::detail::param_type<U>;

  private:

    template <typename U>
    using param_value_type = TiledArray::detail::param_type<typename U::value_type>;

    /// Evaluation tensor

    /// This tensor is used as an evaluated intermediate for other tensors.
    class Impl : public allocator_type {
    public:

      /// Default constructor

      /// Construct an empty tensor that has no data or dimensions
      Impl() : allocator_type(), range_(), data_(NULL) { }

      /// Construct an evaluated tensor

      /// \param range The N-dimensional range for this tensor
      explicit Impl(const range_type& range) :
        allocator_type(), range_(range), data_(NULL)
      {
        data_ = allocator_type::allocate(range.volume());
      }

      ~Impl() {
        math::destroy_vector(range_.volume(), data_);
        allocator_type::deallocate(data_, range_.volume());
        data_ = NULL;
      }

      range_type range_; ///< Tensor size info
      pointer data_; ///< Tensor data
    }; // class Impl

    template <typename U, enable_if_t<std::is_scalar<U>::value>* = nullptr>
    static void default_init(size_type, U*) { }

    template <typename U, enable_if_t<! std::is_scalar<U>::value>* = nullptr>
    static void default_init(size_type n, U* u) {
      math::uninitialized_fill_vector(n, U(), u);
    }

    /// Compute the fused dimensions for permutation

    /// This function will compute the fused dimensions of a tensor for use in
    /// permutation algorithms. The idea is to partition the stride 1 dimensions
    /// in both the input and output tensor, which yields a forth-order tensor
    /// (second- and third-order tensors have size of 1 and stride of 0 in the
    /// unused dimensions).
    void fuse_dimensions(size_type * restrict const fused_size,
        size_type * restrict const fused_weight,
        const size_type * restrict const size, const Permutation& perm)
    {
      const unsigned int ndim1 = perm.dim() - 1u;

      int i = ndim1;
      fused_size[3] = size[i--];
      while((i >= 0) && (perm[i + 1u] == (perm[i] + 1u)))
        fused_size[3] *= size[i--];
      fused_weight[3] = 1u;



      if((i >= 0) && (perm[i] != ndim1)) {
        fused_size[2] = size[i--];
        while((i >= 0) && (perm[i] != ndim1))
          fused_size[2] *= size[i--];

        fused_weight[2] = fused_size[3];

        fused_size[1] = size[i--];
        while((i >= 0) && (perm[i + 1] == (perm[i] + 1u)))
          fused_size[1] *= size[i--];

        fused_weight[1] = fused_size[2] * fused_weight[2];
      } else {
        fused_size[2] = 1ul;
        fused_weight[2] = 0ul;

        fused_size[1] = size[i--];
        while((i >= 0) && (perm[i + 1] == (perm[i] + 1u)))
          fused_size[1] *= size[i--];

        fused_weight[1] = fused_size[3];
      }

      if(i >= 0) {
        fused_size[0] = size[i--];
        while(i >= 0)
          fused_size[0] *= size[i--];

        fused_weight[0] = fused_size[1] * fused_weight[1];
      } else {
        fused_size[0] = 1ul;
        fused_weight[0] = 0ul;
      }
    }

    std::shared_ptr<Impl> pimpl_; ///< Shared pointer to implementation object
    static const range_type empty_range_; ///< Empty range

  public:

    /// Default constructor

    /// Construct an empty tensor that has no data or dimensions
    Tensor() : pimpl_() { }

    Tensor(const range_type& range) :
      pimpl_(new Impl(range))
    {
      default_init(range.volume(), pimpl_->data_);
    }

    /// Construct a tensor with a fill value

    /// \param range An array with the size of of each dimension
    /// \param value The value of the tensor elements
    Tensor(const range_type& range, param_type<value_type> value) :
      pimpl_(new Impl(range))
    {
      math::uninitialized_fill_vector(range.volume(), value, pimpl_->data_);
    }

    /// Construct an evaluated tensor
    template <typename InIter,
        enable_if_t<TiledArray::detail::is_input_iterator<InIter>::value &&
            ! std::is_pointer<InIter>::value>* = nullptr>
    Tensor(const range_type& range, InIter it) :
      pimpl_(new Impl(range))
    {
      size_type n = range.volume();
      pointer restrict const data = pimpl_->data_;
      for(size_type i = 0ul; i < n; ++i)
        data[i] = *it++;
    }

    template <typename U>
    Tensor(const Range& range, const U* u) :
      pimpl_(new Impl(range))
    {
      math::uninitialized_copy_vector(range.volume(), u, pimpl_->data_);
    }

    /// Construct a permuted tensor copy

    /// \tparam U The element type of other
    /// \tparam AU The allocator type of other
    /// \param other The tensor to be copied
    /// \param perm The permutation that will be applied to the copy
    template <typename U, typename AU>
    Tensor(const Tensor<U, AU>& other, const Permutation& perm) :
      pimpl_(new Impl(perm ^ other.range()))
    {
      // Check inputs.
      TA_ASSERT(! other.empty());
      TA_ASSERT(perm);
      TA_ASSERT(perm.dim() == other.range().dim());

      detail::PermIndex perm_index_op(other.range(), perm);

      // Cache constants
      const unsigned int ndim = other.range().dim();
      const unsigned int ndim1 = ndim - 1u;
      const size_type volume = other.range().volume();

      if(perm[ndim1] == ndim1) {
        // This is the simple case where the last dimension is not permuted.
        // Therefore, it can be shuffled in chunks.

        // Determine which dimensions can be permuted with the least significant
        // dimension.
        size_type block_size = other.range().size()[ndim1];
        for(int i = -1 + ndim1 ; i >= 0; --i) {
          if(int(perm[i]) != i)
            break;
          block_size *= other.range().size()[i];
        }

        // Permute the data
        for(size_type index = 0ul; index < volume; index += block_size) {
          const size_type perm_index = perm_index_op(index);

          // Copy the block
          math::uninitialized_copy_vector(block_size, other.data() + index,
              pimpl_->data_ + perm_index);
        }

      } else {
        // This is the more complicated case. Here we permute in terms of matrix
        // transposes. The data layout of the input and output matrices are
        // chosen such that they both contain stride one dimensions.

        size_type other_fused_size[4];
        size_type other_fused_weight[4];
        fuse_dimensions(other_fused_size, other_fused_weight,
            other.range().size(), perm);

        // Compute the fused stride for the result matrix transpose.
        size_type  result_outer_stride = 1ul;
        for(unsigned int i = perm[ndim1] + 1u; i < ndim; ++i)
          result_outer_stride *= pimpl_->range_.size()[i];

        // Copy data from the input to the output matrix via a series of matrix
        // transposes.
        for(size_type i = 0ul; i < other_fused_size[0]; ++i) {
          size_type index = i * other_fused_weight[0];
          for(size_type j = 0ul; j < other_fused_size[2]; ++j, index += other_fused_weight[2]) {
            // Compute the ordinal index of the input and output matrices.
            size_type perm_index = perm_index_op(index);

            // Copy a transposed matrix from the input tensor to the this tensor.
            auto copy_op = [] (param_value_type<Tensor<U, AU> > a) ->
                param_value_type<Tensor<U, AU> > { return a; };
            math::uninitialized_transpose(copy_op,
                other_fused_size[1], other_fused_size[3],
                result_outer_stride, pimpl_->data_ + perm_index,
                other_fused_weight[1], other.data() + index);
          }
        }
      }
    }

    /// Construct an evaluated tensor
    template <typename U, typename AU, typename Op>
    Tensor(const Tensor<U, AU>& other, const Op& op) :
      pimpl_(new Impl(other.range()))
    {
      math::vector_op(op, other.size(), pimpl_->data_, other.data());
    }

    /// Construct an evaluated tensor
    template <typename U, typename AU, typename Op>
    Tensor(const Tensor<U, AU>& other, const Op& op, const Permutation& perm) :
      pimpl_(new Impl(perm ^ other.range()))
    {
      // Check inputs.
      TA_ASSERT(! other.empty());
      TA_ASSERT(perm);
      TA_ASSERT(perm.dim() == other.range().dim());

      detail::PermIndex perm_index_op(other.range(), perm);

      // Cache constants
      const unsigned int ndim = other.range().dim();
      const unsigned int ndim1 = ndim - 1u;
      const size_type volume = other.range().volume();

      if(perm[ndim1] == ndim1) {
        // This is the simple case where the last dimension is not permuted.
        // Therefore, it can be shuffled in chunks.

        // Determine which dimensions can be permuted with the least significant
        // dimension.
        size_type block_size = other.range().size()[ndim1];
        for(int i = -1 + ndim1 ; i >= 0; --i) {
          if(int(perm[i]) != i)
            break;
          block_size *= other.range().size()[i];
        }

        // Permute the data
        for(size_type index = 0ul; index < volume; index += block_size) {
          const size_type perm_index = perm_index_op(index);

          // Copy the block
          math::uninitialized_unary_vector_op(block_size, other.data() + index,
              pimpl_->data_ + perm_index, op);
        }

      } else {
        // This is the more complicated case. Here we permute in terms of matrix
        // transposes. The data layout of the input and output matrices are
        // chosen such that they both contain stride one dimensions.

        size_type other_fused_size[4];
        size_type other_fused_weight[4];
        fuse_dimensions(other_fused_size, other_fused_weight,
            other.range().size(), perm);

        // Compute the fused stride for the result matrix transpose.
        size_type  result_outer_stride = 1ul;
        for(unsigned int i = perm[ndim1] + 1u; i < ndim; ++i)
          result_outer_stride *= pimpl_->range_.size()[i];

        // Copy data from the input to the output matrix via a series of matrix
        // transposes.
        for(size_type i = 0ul; i < other_fused_size[0]; ++i) {
          size_type index = i * other_fused_weight[0];
          for(size_type j = 0ul; j < other_fused_size[2]; ++j, index += other_fused_weight[2]) {
            // Compute the ordinal index of the input and output matrices.
            size_type perm_index = perm_index_op(index);

            // Copy a transposed matrix from the input tensor to the this tensor.
            math::uninitialized_transpose(op,
                other_fused_size[1], other_fused_size[3],
                result_outer_stride, pimpl_->data_ + perm_index,
                other_fused_weight[1], other.data() + index);
          }
        }
      }
    }

    /// Construct an evaluated tensor
    template <typename U, typename AU, typename V, typename AV, typename Op>
    Tensor(const Tensor<U, AU>& left, const Tensor<V, AV>& right, const Op& op) :
      pimpl_(new Impl(left.range()))
    {
      TA_ASSERT(left.range() == right.range());
      math::vector_op(op, left.size(), pimpl_->data_, left.data(), right.data());
    }

    /// Construct an evaluated tensor
    template <typename U, typename AU, typename V, typename AV, typename Op>
    Tensor(const Tensor<U, AU>& left, const Tensor<V, AV>& right, const Op& op, const Permutation& perm) :
      pimpl_(new Impl(perm ^ left.range()))
    {
      // Check inputs.
      TA_ASSERT(! left.empty());
      TA_ASSERT(! right.empty());
      TA_ASSERT(left.range() == right.range());
      TA_ASSERT(perm);
      TA_ASSERT(perm.dim() == left.range().dim());

      detail::PermIndex perm_index_op(left.range(), perm);

      // Cache constants
      const unsigned int ndim = left.range().dim();
      const unsigned int ndim1 = ndim - 1u;
      const size_type volume = left.range().volume();

      if(perm[ndim1] == ndim1) {
        // This is the simple case where the last dimension is not permuted.
        // Therefore, it can be shuffled in chunks.

        // Determine which dimensions can be permuted with the least significant
        // dimension.
        size_type block_size = left.range().size()[ndim1];
        for(int i = -1 + ndim1 ; i >= 0; --i) {
          if(int(perm[i]) != i)
            break;
          block_size *= left.range().size()[i];
        }

        // Permute the data
        for(size_type index = 0ul; index < volume; index += block_size) {
          const size_type perm_index = perm_index_op(index);

          // Copy the block
          math::uninitialized_binary_vector_op(block_size, left.data() + index,
              right.data() + index, pimpl_->data_ + perm_index, op);
        }

      } else {
        // This is the more complicated case. Here we permute in terms of matrix
        // transposes. The data layout of the input and output matrices are
        // chosen such that they both contain stride one dimensions.

        size_type other_fused_size[4];
        size_type other_fused_weight[4];
        fuse_dimensions(other_fused_size, other_fused_weight,
            left.range().size(), perm);

        // Compute the fused stride for the result matrix transpose.
        size_type  result_outer_stride = 1ul;
        for(unsigned int i = perm[ndim1] + 1u; i < ndim; ++i)
          result_outer_stride *= pimpl_->range_.size()[i];

        // Copy data from the input to the output matrix via a series of matrix
        // transposes.
        for(size_type i = 0ul; i < other_fused_size[0]; ++i) {
          size_type index = i * other_fused_weight[0];
          for(size_type j = 0ul; j < other_fused_size[2]; ++j, index += other_fused_weight[2]) {
            // Compute the ordinal index of the input and output matrices.
            size_type perm_index = perm_index_op(index);

            // Copy a transposed matrix from the input tensor to the this tensor.
            math::uninitialized_transpose(op,
                other_fused_size[1], other_fused_size[3],
                result_outer_stride, pimpl_->data_ + perm_index,
                other_fused_weight[1], left.data() + index, right.data() + index);
          }
        }
      }
    }

    /// Copy constructor

    /// Create a shallow copy of \c other
    /// \param other The tile to be copied.
    Tensor(const Tensor_& other) :
      pimpl_(other.pimpl_)
    { }

    /// Copy assignment

    /// Create a shallow copy of \c other
    /// \param other The tensor to be copied
    /// \return \c this tensor
    Tensor_& operator=(const Tensor_& other) {
      pimpl_ = other.pimpl_;

      return *this;
    }

    Tensor_ clone() const {
      return (pimpl_ ? Tensor_(pimpl_->range_, pimpl_->data_) : Tensor_());
    }

    /// Plus assignment

    /// Evaluate \c other to this tensor
    /// \param other The tensor to be copied
    /// \return this tensor
    template <typename U, typename AU>
    Tensor_& operator+=(const Tensor<U, AU>& other) { return add_to(other); }

    /// Minus assignment

    /// Evaluate \c other to this tensor
    /// \param other The tensor to be copied
    /// \return this tensor
    template <typename U, typename AU>
    Tensor_& operator-=(const Tensor<U, AU>& other) { return subt_to(other); }

    /// Multiply assignment

    /// Evaluate \c other to this tensor
    /// \param other The tensor to be copied
    /// \return this tensor
    template <typename U, typename AU>
    Tensor_& operator*=(const Tensor<U, AU>& other) { return mult_to(other); }

    /// Plus shift operator

    /// \param value The shift value
    /// \return this tensor
    Tensor_& operator+=(const numeric_type value) { return add_to(value); }

    /// Minus shift operator

    /// \param value The negative shift value
    /// \return this tensor
    Tensor_& operator-=(const numeric_type value) { return subt_to(value); }

    /// Scale operator

    /// \param value The scaling factor
    /// \return this tensor
    Tensor_& operator*=(const numeric_type value) { return scale_to(value); }

    /// Tensor range object accessor

    /// \return The tensor range object
    const range_type& range() const {
      return (pimpl_ ? pimpl_->range_ : empty_range_);
    }

    /// Tensor dimension size accessor

    /// \return The number of elements in the tensor
    size_type size() const {
      return (pimpl_ ? pimpl_->range_.volume() : 0ul);
    }

    /// Element accessor

    /// \return The element at the \c i position.
    const_reference operator[](const size_type i) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(i));
      return pimpl_->data_[i];
    }

    /// Element accessor

    /// \return The element at the \c i position.
    /// \throw TiledArray::Exception When this tensor is empty.
    reference operator[](const size_type i) {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(i));
      return pimpl_->data_[i];
    }


    /// Element accessor

    /// \return The element at the \c i position.
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename Index,
        enable_if_t<! std::is_integral<Index>::value>* = nullptr>
    const_reference operator[](const Index& i) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(i));
      return pimpl_->data_[pimpl_->range_.ord(i)];
    }

    /// Element accessor

    /// \return The element at the \c i position.
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename Index,
      enable_if_t<! std::is_integral<Index>::value>* = nullptr>
    reference operator[](const Index& i) {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(i));
      return pimpl_->data_[pimpl_->range_.ord(i)];
    }

    /// Element accessor

    /// \tparam Index index type pack
    /// \param idx The index pack
    template<typename... Index>
    reference operator()(const Index&... idx) {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(idx...));
      return pimpl_->data_[pimpl_->range_.ord(idx...)];
    }

    /// Element accessor

    /// \tparam Index index type pack
    /// \param idx The index pack
    template<typename... Index>
    const_reference operator()(const Index&... idx) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.includes(idx...));
      return pimpl_->data_[pimpl_->range_.ord(idx...)];
    }

    /// Iterator factory

    /// \return An iterator to the first data element
    const_iterator begin() const { return (pimpl_ ? pimpl_->data_ : NULL); }

    /// Iterator factory

    /// \return An iterator to the first data element
    iterator begin() { return (pimpl_ ? pimpl_->data_ : NULL); }

    /// Iterator factory

    /// \return An iterator to the last data element
    const_iterator end() const { return (pimpl_ ? pimpl_->data_ + pimpl_->range_.volume() : NULL); }

    /// Iterator factory

    /// \return An iterator to the last data element
    iterator end() { return (pimpl_ ? pimpl_->data_ + pimpl_->range_.volume() : NULL); }

    /// Data direct access

    /// \return A const pointer to the tensor data
    const_pointer data() const { return (pimpl_ ? pimpl_->data_ : NULL); }

    /// Data direct access

    /// \return A const pointer to the tensor data
    pointer data() { return (pimpl_ ? pimpl_->data_ : NULL); }

    bool empty() const { return !pimpl_; }

    template <typename Archive,
        enable_if_t<madness::archive::is_output_archive<Archive>::value>* = nullptr>
    void serialize(Archive& ar) {
      if(pimpl_) {
        ar & pimpl_->range_.volume();
        ar & madness::archive::wrap(pimpl_->data_, pimpl_->range_.volume());
        ar & pimpl_->range_;
      } else {
        ar & size_type(0ul);
      }
    }

    /// Serialize tensor data

    /// \tparam Archive The serialization archive type
    /// \param ar The serialization archive
    template <typename Archive,
        enable_if_t<madness::archive::is_input_archive<Archive>::value>* = nullptr>
    void serialize(Archive& ar) {
      size_type n = 0ul;
      ar & n;
      if(n) {
        std::shared_ptr<Impl> temp(new Impl());
        temp->data_ = temp->allocate(n);
        try {
          ar & madness::archive::wrap(temp->data_, n);
          ar & temp->range_;
        } catch(...) {
          temp->deallocate(temp->data_, n);
          throw;
        }

        pimpl_ = temp;
      } else {
        pimpl_.reset();
      }
    }



    /// Swap tensor data

    /// \param other The tensor to swap with this
    void swap(Tensor_& other) {
      std::swap(pimpl_, other.pimpl_);
    }


    /// Create a permuted copy of this tensor

    /// \param perm The permutation to be applied to this tensor
    /// \return A permuted copy of this tensor
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception The dimension of \c perm does not match
    /// that of this tensor.
    Tensor_ permute(const Permutation& perm) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(perm.dim() == pimpl_->range_.dim());
      return Tensor_(*this, perm);
    }

    // Generic vector operations

    /// Use a binary, element wise operation to construct a new tensor

    /// \tparam U \c other tensor element type
    /// \tparam AU \c other allocator type
    /// \tparam Op The binary operation type
    /// \param other The right-hand argument in the binary operation
    /// \param op The binary, element-wise operation
    /// \return A tensor where element \c i of the new tensor is equal to
    /// \c op(*this[i],other[i])
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception When \c other is empty.
    /// \throw TiledArray::Exception When the range of this tensor is not equal
    /// to the range of \c other.
    template <typename U, typename AU, typename Op>
    Tensor_ binary(const Tensor<U, AU>& other, const Op& op) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(! other.empty());
      TA_ASSERT(pimpl_->range_ == other.range());

      return Tensor_(*this, other, op);
    }

    /// Use a binary, element wise operation to construct a new, permuted tensor

    /// \tparam U \c other tensor element type
    /// \tparam AU \c other allocator type
    /// \tparam Op The binary operation type
    /// \param other The right-hand argument in the binary operation
    /// \param op The binary, element-wise operation
    /// \param perm The permutation to be applied to this tensor
    /// \return A tensor where element \c i of the new tensor is equal to
    /// \c op(*this[i],other[i])
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception When \c other is empty.
    /// \throw TiledArray::Exception When the range of this tensor is not equal
    /// to the range of \c other.
    /// \throw TiledArray::Exception The dimension of \c perm does not match
    /// that of this tensor.
    template <typename U, typename AU, typename Op>
    Tensor_ binary(const Tensor<U, AU>& other, const Op& op, const Permutation& perm) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(! other.empty());
      TA_ASSERT(pimpl_->range_ == other.range());
      TA_ASSERT(perm.dim() == pimpl_->range_.dim());

      return Tensor_(*this, other, op, perm);
    }

    /// Use a binary, element wise operation to modify this tensor

    /// \tparam U \c other tensor element type
    /// \tparam AU \c other allocator type
    /// \tparam Op The binary operation type
    /// \param other The right-hand argument in the binary operation
    /// \param op The binary, element-wise operation
    /// \return A reference to this object
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception When \c other is empty.
    /// \throw TiledArray::Exception When the range of this tensor is not equal
    /// to the range of \c other.
    /// \throw TiledArray::Exception When this and \c other are the same.
    template <typename U, typename AU, typename Op>
    Tensor_& inplace_binary(const Tensor<U, AU>& other, const Op& op) {
      TA_ASSERT(pimpl_);
      TA_ASSERT(! other.empty());
      TA_ASSERT(pimpl_->range_ == other.range());
      TA_ASSERT(pimpl_->data_ != other.data());

      math::vector_op(op, pimpl_->range_.volume(), pimpl_->data_, other.data());

      return *this;
    }

    /// Use a unary, element wise operation to construct a new tensor

    /// \tparam Op The unary operation type
    /// \param op The unary, element-wise operation
    /// \return A tensor where element \c i of the new tensor is equal to
    /// \c op(*this[i])
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename Op>
    Tensor_ unary(const Op& op) const {
      TA_ASSERT(pimpl_);

      return Tensor_(*this, op);
    }

    /// Use a unary, element wise operation to construct a new, permuted tensor

    /// \tparam Op The unary operation type
    /// \param op The unary operation
    /// \param perm The permutation to be applied to this tensor
    /// \return A permuted tensor with elements that have been modified by \c op
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception The dimension of \c perm does not match
    /// that of this tensor.
    template <typename Op>
    Tensor_ unary(const Op& op, const Permutation& perm) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(perm.dim() == pimpl_->range_.dim());

      return Tensor_(*this, op, perm);
    }

    /// Use a unary, element wise operation to modify this tensor

    /// \tparam Op The unary operation type
    /// \param op The unary, element-wise operation
    /// \return A reference to this object
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename Op>
    Tensor_& inplace_unary(const Op& op) {
      TA_ASSERT(pimpl_);

      math::vector_op(op, pimpl_->range_.volume(), pimpl_->data_);

      return *this;
    }

    // Scale operation

    /// Construct a scaled copy of this tensor

    /// \param factor The scaling factor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this are scaled by \c factor
    Tensor_ scale(numeric_type factor) const {
      return unary([=] (param_type<value_type> arg) { return arg * factor; });
    }

    /// Construct a scaled and permuted copy of this tensor

    /// \param factor The scaling factor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this are scaled by \c factor
    Tensor_ scale(numeric_type factor, const Permutation& perm) const {
      return unary([=] (param_type<value_type> arg) { return arg * factor; }, perm);
    }

    /// Scale this tensor

    /// \param factor The scaling factor
    /// \return A reference to this tensor
    Tensor_& scale_to(numeric_type factor) {
      return inplace_unary([=] (value_type& res) { res *= factor; });
    }

    // Addition operations

    /// Add this and \c other to construct a new tensors

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c other
    template <typename U, typename AU>
    Tensor_ add(const Tensor<U, AU>& other) const {
      return binary(other, [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right) { return left + right; });
    }

    /// Add this and \c other to construct a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c other
    template <typename U, typename AU>
    Tensor_ add(const Tensor<U, AU>& other, const Permutation& perm) const {
      return binary(other, [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right) { return left + right; },
          perm);
    }

    /// Scale and add this and \c other to construct a new tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \param factor The scaling factor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ add(const Tensor<U, AU>& other, const numeric_type factor) const {
      return binary(other, [=] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return (left + right) * factor; });
    }

    /// Scale and add this and \c other to construct a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \param factor The scaling factor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ add(const Tensor<U, AU>& other, const numeric_type factor,
        const Permutation& perm) const
    {
      return binary(other,  [=] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return (left + right) * factor; }, perm);
    }

    /// Add a constant to a copy of this tensor

    /// \param value The constant to be added to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c value
    Tensor_ add(const numeric_type value) const {
      return unary([=] (param_type<value_type> arg)
          { return arg + value; });
    }

    /// Add a constant to a permuted copy of this tensor

    /// \param value The constant to be added to this tensor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the sum of the elements of
    /// \c this and \c value
    Tensor_ add(const numeric_type value, const Permutation& perm) const {
      return unary([=] (param_type<value_type> arg) { return arg + value; }, perm);
    }

    /// Add \c other to this tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& add_to(const Tensor<U, AU>& other) {
      return inplace_binary(other, [] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { res += arg; });
    }

    /// Add \c other to this tensor, and scale the result

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be added to this tensor
    /// \param factor The scaling factor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& add_to(const Tensor<U, AU>& other, const numeric_type factor) {
      return inplace_binary(other, [=] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { (res += arg) *= factor; });
    }

    /// Add a constant to this tensor

    /// \param value The constant to be added
    /// \return A reference to this tensor
    Tensor_& add_to(const numeric_type value) {
      return inplace_unary([=] (value_type& res) { res += value; });
    }

    // Subtraction operations

    /// Subtract this and \c other to construct a new tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c other
    template <typename U, typename AU>
    Tensor_ subt(const Tensor<U, AU>& other) const {
      return binary(other, [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right) { return left - right; });
    }

    /// Subtract this and \c other to construct a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c other
    template <typename U, typename AU>
    Tensor_ subt(const Tensor<U, AU>& other, const Permutation& perm) const {
      return binary(other, [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return left - right; }, perm);
    }

    /// Scale and subtract this and \c other to construct a new tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \param factor The scaling factor
    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ subt(const Tensor<U, AU>& other, const numeric_type factor) const {
      return binary(other, [=] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return (left - right) * factor; });
    }

    /// Scale and subtract this and \c other to construct a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \param factor The scaling factor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ subt(const Tensor<U, AU>& other, const numeric_type factor, const Permutation& perm) const {
      return binary(other, [=] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return (left - right) * factor; }, perm);
    }

    /// Subtract a constant from a copy of this tensor

    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c value
    Tensor_ subt(const numeric_type value) const {
      return add(-value);
    }

    /// Subtract a constant from a permuted copy of this tensor

    /// \param value The constant to be subtracted
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the different between the
    /// elements of \c this and \c value
    Tensor_ subt(const numeric_type value, const Permutation& perm) const {
      return add(-value, perm);
    }

    /// Subtract \c other from this tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& subt_to(const Tensor<U, AU>& other) {
      return inplace_binary(other, [] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { res -= arg; });
    }

    /// Subtract \c other from and scale this tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be subtracted from this tensor
    /// \param factor The scaling factor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& subt_to(const Tensor<U, AU>& other, const numeric_type factor) {
      return inplace_binary(other, [=] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { (res -= arg) *= factor; });
    }

    /// Subtract a constant from this tensor

    /// \return A reference to this tensor
    Tensor_& subt_to(const numeric_type value) {
      return add_to(-value);
    }

    // Multiplication operations

    /// Multiply this by \c other to create a new tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \return A new tensor where the elements are the product of the elements
    /// of \c this and \c other
    template <typename U, typename AU>
    Tensor_ mult(const Tensor<U, AU>& other) const {
      return binary(other, [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return left * right; });
    }

    /// Multiply this by \c other to create a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the product of the elements
    /// of \c this and \c other
    template <typename U, typename AU>
    Tensor_ mult(const Tensor<U, AU>& other, const Permutation& perm) const {
      return binary(other,  [] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return left * right; }, perm);
    }

    /// Scale and multiply this by \c other to create a new tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \param factor The scaling factor
    /// \return A new tensor where the elements are the product of the elements
    /// of \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ mult(const Tensor<U, AU>& other, const numeric_type factor) const {
      return binary(other, [=] (param_type<value_type>left,
          param_value_type<Tensor<U, AU> > right)
          { return (left * right) * factor; });
    }

    /// Scale and multiply this by \c other to create a new, permuted tensor

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \param factor The scaling factor
    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor where the elements are the product of the elements
    /// of \c this and \c other, scaled by \c factor
    template <typename U, typename AU>
    Tensor_ mult(const Tensor<U, AU>& other, const numeric_type factor,
        const Permutation& perm) const
    {
      return binary(other,  [=] (param_type<value_type> left,
          param_value_type<Tensor<U, AU> > right)
          { return (left * right) * factor; }, perm);
    }

    /// Multiply this tensor by \c other

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& mult_to(const Tensor<U, AU>& other) {
      return inplace_binary(other, [] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { res *= arg; });
    }

    /// Scale and multiply this tensor by \c other

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be multiplied by this tensor
    /// \param factor The scaling factor
    /// \return A reference to this tensor
    template <typename U, typename AU>
    Tensor_& mult_to(const Tensor<U, AU>& other, const numeric_type factor) {
      return inplace_binary(other, [=] (value_type& res,
          param_value_type<Tensor<U, AU> > arg) { (res *= arg) *= factor; });
    }

    // Negation operations

    /// Create a negated copy of this tensor

    /// \return A new tensor that contains the negative values of this tensor
    Tensor_ neg() const {
      return unary([] (param_type<value_type> arg) { return -arg; });
    }

    /// Create a negated and permuted copy of this tensor

    /// \param perm The permutation to be applied to this tensor
    /// \return A new tensor that contains the negative values of this tensor
    Tensor_ neg(const Permutation& perm) const {
      return unary([] (param_type<value_type>arg) { return -arg; }, perm);
    }

    /// Negate elements of this tensor

    /// \return A reference to this tensor
    Tensor_& neg_to() {
      return inplace_unary([] (value_type& res) { res = -res; });
    }

    // GEMM operations

    /// Contract this tensor with \c other

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The tensor that will be contracted with this tensor
    /// \param factor The scaling factor
    /// \param gemm_helper The *GEMM operation meta data
    /// \return A new tensor which is the result of contracting this tensor with
    /// \c other
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception When \c other is empty.
    template <typename U, typename AU>
    Tensor_ gemm(const Tensor<U, AU>& other, const numeric_type factor, const math::GemmHelper& gemm_helper) const {
      // Check that this tensor is not empty and has the correct rank
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.dim() == gemm_helper.left_rank());

      // Check that the arguments are not empty and have the correct ranks
      TA_ASSERT(!other.empty());
      TA_ASSERT(other.range().dim() == gemm_helper.right_rank());

      // Construct the result Tensor
      Tensor_ result(gemm_helper.make_result_range<range_type>(pimpl_->range_, other.range()));

      // Check that the inner dimensions of left and right match
      TA_ASSERT(gemm_helper.left_right_coformal(pimpl_->range_.start(), other.range().start()));
      TA_ASSERT(gemm_helper.left_right_coformal(pimpl_->range_.finish(), other.range().finish()));
      TA_ASSERT(gemm_helper.left_right_coformal(pimpl_->range_.size(), other.range().size()));


      // Compute gemm dimensions
      integer m = 1, n = 1, k = 1;
      gemm_helper.compute_matrix_sizes(m, n, k, pimpl_->range_, other.range());

      // Get the leading dimension for left and right matrices.
      const integer lda = (gemm_helper.left_op() == madness::cblas::NoTrans ? k : m);
      const integer ldb = (gemm_helper.right_op() == madness::cblas::NoTrans ? n : k);

      math::gemm(gemm_helper.left_op(), gemm_helper.right_op(), m, n, k, factor,
          pimpl_->data_, lda, other.data(), ldb, numeric_type(0), result.data(), n);

      return result;
    }

    /// Contract two tensors and store the result in this tensor

    /// \tparam U The left-hand tensor element type
    /// \tparam AU The left-hand tensor allocator type
    /// \tparam V The right-hand tensor element type
    /// \tparam AV The right-hand tensor allocator type
    /// \param left The left-hand tensor that will be contracted
    /// \param right The right-hand tensor that will be contracted
    /// \param factor The scaling factor
    /// \param gemm_helper The *GEMM operation meta data
    /// \return A new tensor which is the result of contracting this tensor with
    /// other
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename U, typename AU, typename V, typename AV>
    Tensor_& gemm(const Tensor<U, AU>& left, const Tensor<V, AV>& right,
        const numeric_type factor, const math::GemmHelper& gemm_helper)
    {
      // Check that this tensor is not empty and has the correct rank
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_.dim() == gemm_helper.result_rank());

      // Check that the arguments are not empty and have the correct ranks
      TA_ASSERT(!left.empty());
      TA_ASSERT(left.range().dim() == gemm_helper.left_rank());
      TA_ASSERT(!right.empty());
      TA_ASSERT(right.range().dim() == gemm_helper.right_rank());

      // Check that the outer dimensions of left match the the corresponding dimensions in result
      TA_ASSERT(gemm_helper.left_result_coformal(left.range().start(), pimpl_->range_.start()));
      TA_ASSERT(gemm_helper.left_result_coformal(left.range().finish(), pimpl_->range_.finish()));
      TA_ASSERT(gemm_helper.left_result_coformal(left.range().size(), pimpl_->range_.size()));

      // Check that the outer dimensions of right match the the corresponding dimensions in result
      TA_ASSERT(gemm_helper.right_result_coformal(right.range().start(), pimpl_->range_.start()));
      TA_ASSERT(gemm_helper.right_result_coformal(right.range().finish(), pimpl_->range_.finish()));
      TA_ASSERT(gemm_helper.right_result_coformal(right.range().size(), pimpl_->range_.size()));

      // Check that the inner dimensions of left and right match
      TA_ASSERT(gemm_helper.left_right_coformal(left.range().start(), right.range().start()));
      TA_ASSERT(gemm_helper.left_right_coformal(left.range().finish(), right.range().finish()));
      TA_ASSERT(gemm_helper.left_right_coformal(left.range().size(), right.range().size()));

      // Compute gemm dimensions
      integer m, n, k;
      gemm_helper.compute_matrix_sizes(m, n, k, left.range(), right.range());

      // Get the leading dimension for left and right matrices.
      const integer lda = (gemm_helper.left_op() == madness::cblas::NoTrans ? k : m);
      const integer ldb = (gemm_helper.right_op() == madness::cblas::NoTrans ? n : k);

      math::gemm(gemm_helper.left_op(), gemm_helper.right_op(), m, n, k, factor,
          left.data(), lda, right.data(), ldb, numeric_type(1), pimpl_->data_, n);

      return *this;
    }

    // Reduction operations

    /// Generalized tensor trace

    /// This function will compute the sum of the hyper diagonal elements of
    /// tensor.
    /// \return The trace of this tensor
    /// \throw TiledArray::Exception When this tensor is empty.
    value_type trace() const {
      TA_ASSERT(pimpl_);

      // Get pointers to the range data
      const size_type n = pimpl_->range_.dim();
      const size_type* restrict const start = pimpl_->range_.start().data();
      const size_type* restrict const finish = pimpl_->range_.finish().data();
      const size_type* restrict const weight = pimpl_->range_.weight().data();

      // Search for the largest start index and the smallest finish
      size_type start_max = 0ul, finish_min = std::numeric_limits<size_type>::max();
      for(size_type i = 0ul; i < n; ++i) {
        const size_type start_i = start[i];
        const size_type finish_i = finish[i];

        start_max = std::max(start_max, start_i);
        finish_min = std::min(finish_min, finish_i);
      }

      value_type result = 0;

      if(start_max < finish_min) {
        // Compute the first and last ordinal index
        size_type first = 0ul, last = 0ul, stride = 0ul;
        for(size_type i = 0ul; i < n; ++i) {
          const size_type start_i = start[i];
          const size_type weight_i = weight[i];

          first += (start_max - start_i) * weight_i;
          last += (finish_min - start_i) * weight_i;
          stride += weight_i;
        }

        // Compute the trace
        const value_type* restrict const data = pimpl_->data_;
        for(; first < last; first += stride)
          result += data[first];
      }

      return result;
    }

  private:

    /// Unary reduction operation

    /// Perform an element-wise reduction of the tile data.
    /// \tparam U The numeric element type
    /// \tparam Op The reduction operation
    /// \param n The number of elements to reduce
    /// \param u The data to be reduced
    /// \param value The initial value of the reduction
    /// \param op The element-wise reduction operation
    template <typename U, typename Op,
        enable_if_t<TiledArray::detail::is_numeric<U>::value>* = nullptr>
    static void reduce(const size_type n, const U* u, numeric_type& value, const Op& op) {
      math::reduce_op(op, n, value, u);
    }

    /// Unary \c Tensor reduction

    /// Perform an element-wise reduction on an array of \c Tensors.
    /// \tparam U The tensor element type
    /// \tparam AU The tensor allocator type
    /// \tparam Op The reduction operation
    /// \param n The number of elements to reduce
    /// \param u The data to be reduced
    /// \param value The initial value of the reduction
    /// \param op The element-wise reduction operation
    template <typename U, typename AU, typename Op>
    static void reduce(const size_type n, const Tensor<U, AU>* u,
        numeric_type& value, const Op& op)
    {
      for(size_type i = 0ul; i < n; ++i)
        u[i].reduce(value, op);
    }

    /// Binary reduction operation

    /// Perform an element-wise reduction of the tile data.
    /// \tparam Left The left-hand element type
    /// \tparam Right The right-hand element type
    /// \tparam Op The reduction operation
    /// \param n The number of elements to reduce
    /// \param left The left-hand data to be reduced
    /// \param right The right-hand data to be reduced
    /// \param value The initial value of the reduction
    /// \param op The element-wise reduction operation
    template <typename Left, typename Right, typename Op>
    static typename std::enable_if<TiledArray::detail::is_numeric<Left>::value &&
        TiledArray::detail::is_numeric<Right>::value >::type
    reduce(const size_type n, const Left* left, const Right* right,
        numeric_type& value, const Op& op) {
      math::reduce_op(op, n, value, left, right);
    }

    /// Binary \c Tensor reduction

    /// Perform an element-wise reduction on arrays of \c Tensors.
    /// \tparam U The left-hand tensor element type
    /// \tparam AU The left-hand tensor allocator type
    /// \tparam V The right-hand tensor element type
    /// \tparam AV The right-hand tensor allocator type
    /// \tparam Op The reduction operation
    /// \param n The number of elements to reduce
    /// \param left The left-hand \c Tensors to be reduced
    /// \param right The right-hand \c Tensors to be reduced
    /// \param value The initial value of the reduction
    /// \param op The element-wise reduction operation
    template <typename U, typename AU, typename V, typename AV, typename Op>
    static void reduce(const size_type n, const Tensor<U, AU>* left,
        const Tensor<V, AV>* right, numeric_type& value, const Op& op)
    {
      for(size_type i = 0ul; i < n; ++i)
        left[i].reduce(right[i], value, op);
    }

  public:

    /// Unary reduction operation

    /// Perform an element-wise reduction of the tile data.
    /// \tparam Op The reduction operation
    /// \param init_value The initial value of the reduction
    /// \param op The element-wise reduction operation
    /// \throw TiledArray::Exception When this tensor is empty.
    template <typename Op>
    numeric_type reduce(numeric_type init_value, const Op& op) const {
      TA_ASSERT(pimpl_);
      reduce(pimpl_->range_.volume(), pimpl_->data_, init_value, op);
      return init_value;
    }

    /// Binary reduction operation

    /// \tparam U The element type of the right-hand argument
    /// \tparam AU The allocator type of the right-hand argument
    /// \tparam Op The reduction operation
    /// \param other The right-hand argument of the binary reduction
    /// \param init_value The initial value of the reduction
    /// \param op The element-wise reduction operation
    /// \throw TiledArray::Exception When this tensor is empty.
    /// \throw TiledArray::Exception When the range of this tensor is not equal
    /// to the range of \c other.
    template <typename U, typename AU, typename Op>
    numeric_type reduce(const Tensor<U, AU>& other, numeric_type init_value, const Op& op) const {
      TA_ASSERT(pimpl_);
      TA_ASSERT(pimpl_->range_ == other.range());

      reduce(pimpl_->range_.volume(), pimpl_->data_, other.data(), init_value, op);
      return init_value;
    }

    /// Sum of elements

    /// \return The sum of all elements of this tensor
    numeric_type sum() const {
      return reduce(0, [] (numeric_type& res, const numeric_type arg)
          { res += arg; });
    }

    /// Product of elements

    /// \return The product of all elements of this tensor
    numeric_type product() const {
      return reduce(1, [] (numeric_type& res, const numeric_type arg)
          { res *= arg; });
    }

    /// Square of vector 2-norm

    /// \return The vector norm of this tensor
    numeric_type squared_norm() const {
      return reduce(0, [] (numeric_type& res, const numeric_type value)
          { res += value * value; });
    }

    /// Vector 2-norm

    /// \return The vector norm of this tensor
    numeric_type norm() const {
      return std::sqrt(squared_norm());
    }

    /// Minimum element

    /// \return The minimum elements of this tensor
    numeric_type min() const {
      return reduce(std::numeric_limits<numeric_type>::max(),
          [] (numeric_type& res, const numeric_type arg)
          { res = std::min(res, arg); });
    }

    /// Maximum element

    /// \return The maximum elements of this tensor
    numeric_type max() const {
      return reduce(std::numeric_limits<numeric_type>::min(),
          [] (numeric_type& res, const numeric_type arg)
          { res = std::max(res, arg); });
    }

    /// Absolute minimum element

    /// \return The minimum elements of this tensor
    numeric_type abs_min() const {
      return reduce(std::numeric_limits<numeric_type>::max(),
          [] (numeric_type& res, const numeric_type arg)
          { res = std::min(res, std::abs(arg)); });
    }

    /// Absolute maximum element

    /// \return The maximum elements of this tensor
    numeric_type abs_max() const {
      return reduce(0,
          [] (numeric_type& res, const numeric_type arg)
          { res = std::max(res, std::abs(arg)); });
    }

    /// Vector dot product

    /// \tparam U The other tensor element type
    /// \tparam AU The other tensor allocator type
    /// \param other The other tensor to be reduced
    /// \return The inner product of the this and \c other
    template <typename U, typename AU>
    numeric_type dot(const Tensor<U, AU>& other) const {
      return reduce(other, 0, [] (numeric_type& res, const numeric_type left,
          const typename Tensor<U, AU>::numeric_type right)
          { res += left * right; });
    }

  }; // class Tensor

  template <typename T, typename A>
  const typename Tensor<T, A>::range_type Tensor<T, A>::empty_range_;



  /// Tensor plus operator

  /// Add two tensors
  /// \tparam T The element type of \c left
  /// \tparam AT The allocator type of \c left
  /// \tparam U The element type of \c right
  /// \tparam AU The allocator type of \c right
  /// \param left The left-hand tensor argument
  /// \param right The right-hand tensor argument
  /// \return A tensor where element \c i is equal to <tt> left[i] + right[i] </tt>
  template <typename T, typename AT, typename U, typename AU>
  inline Tensor<T, AT> operator+(const Tensor<T, AT>& left, const Tensor<U, AU>& right) {
    return left.add(right);
  }

  /// Tensor minus operator

  /// Subtract two tensors
  /// \tparam T The element type of \c left
  /// \tparam AT The allocator type of \c left
  /// \tparam U The element type of \c right
  /// \tparam AU The allocator type of \c right
  /// \param left The left-hand tensor argument
  /// \param right The right-hand tensor argument
  /// \return A tensor where element \c i is equal to <tt> left[i] - right[i] </tt>
  template <typename T, typename AT, typename U, typename AU>
  inline Tensor<T, AT> operator-(const Tensor<T, AT>& left, const Tensor<U, AU>& right) {
    return left.subt(right);
  }

  /// Tensor multiplication operator

  /// Element-wise multiplication of two tensors
  /// \tparam T The element type of \c left
  /// \tparam AT The allocator type of \c left
  /// \tparam U The element type of \c right
  /// \tparam AU The allocator type of \c right
  /// \param left The left-hand tensor argument
  /// \param right The right-hand tensor argument
  /// \return A tensor where element \c i is equal to <tt> left[i] * right[i] </tt>
  template <typename T, typename AT, typename U, typename AU>
  inline Tensor<T, AT> operator*(const Tensor<T, AT>& left, const Tensor<U, AU>& right) {
    return left.mult(right);
  }


  /// Create a copy of \c left that is scaled by \c right

  /// Scale a tensor
  /// \tparam T The element type of \c left
  /// \tparam AT The allocator type of \c left
  /// \tparam N Numeric type
  /// \param left The left-hand tensor argument
  /// \param right The right-hand scalar argument
  /// \return A tensor where element \c i is equal to <tt> left[i] * right </tt>
  template <typename T, typename AT, typename N,
      enable_if_t<TiledArray::detail::is_numeric<N>::value>* = nullptr>
  inline Tensor<T, AT>operator*(const Tensor<T, AT>& left, N right) {
    return left.scale(right);
  }

  /// Create a copy of \c right that is scaled by \c left

  /// \tparam N A numeric type
  /// \tparam T The element type of \c left
  /// \tparam AT The allocator type of \c left
  /// \param left The left-hand scalar argument
  /// \param right The right-hand tensor argument
  /// \return A tensor where element \c i is equal to <tt> left * right[i] </tt>
  template <typename N, typename T, typename AT,
    enable_if_t<detail::is_numeric<N>::value>* = nullptr>
  inline Tensor<T, AT> operator*(N left, const Tensor<T, AT>& right) {
    return right.scale(left);
  }

  /// Create a negated copy of \c arg

  /// \tparam T The element type of \c arg
  /// \tparam AT The allocator type of \c arg
  /// \param arg The argument tensor
  /// \return A tensor where element \c i is equal to \c -arg[i]
  template <typename T, typename AT>
  inline Tensor<T, AT> operator-(const Tensor<T, AT>& arg) {
    return arg.neg();
  }

  /// Create a permuted copy of \c arg

  /// \tparam T The tensor element type
  /// \tparam A The tensor allocator type
  /// \param perm The permutation to be applied to \c arg
  /// \param arg The argument tensor to be permuted by \c perm
  template <typename T, typename A>
  inline Tensor<T,A> operator^(const Permutation& perm, const Tensor<T, A>& arg) {
    return arg.permute(perm);
  }

  /// Tensor output operator

  /// Ouput tensor \c t to the output stream, \c os .
  /// \tparam T The element type of \c arg
  /// \tparam AT The allocator type of \c arg
  /// \param os The output stream
  /// \param t The tensor to be output
  /// \return A reference to the output stream
  template <typename T, typename AT>
  inline std::ostream& operator<<(std::ostream& os, const Tensor<T, AT>& t) {
    os << t.range() << " { ";
    for(typename Tensor<T, AT>::const_iterator it = t.begin(); it != t.end(); ++it) {
      os << *it << " ";
    }

    os << "}";

    return os;
  }

} // namespace TiledArray

#endif // TILEDARRAY_TENSOR_H__INCLUDED
