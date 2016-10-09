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

#ifndef TILEDARRAY_TENSOR_IMPL_H__INCLUDED
#define TILEDARRAY_TENSOR_IMPL_H__INCLUDED

#include "error.h"
#include "madness.h"
#include "policies/dense_policy.h"
#include "policies/sparse_policy.h"

namespace TiledArray {
  namespace detail {

    /// Tensor implementation and base for other tensor implementation objects

    /// This implementation object holds the meta data for tensor object, which
    /// includes tiled range, shape, and process map.
    /// \note The process map must be set before data elements can be set.
    /// \note It is the users responsibility to ensure the process maps on all
    /// nodes are identical.
    template <typename Policy>
    class TensorImpl : private NO_DEFAULTS {
    public:
      typedef TensorImpl<Policy> TensorImpl_;
      typedef typename Policy::trange_type trange_type; ///< Tiled range type
      typedef typename Policy::range_type range_type; ///< Tile range type
      typedef typename Policy::size_type size_type; ///< Size type
      typedef typename Policy::shape_type shape_type; ///< Tensor shape type
      typedef typename Policy::pmap_interface pmap_interface; ///< Process map interface type

    private:
      World& world_; ///< World that contains
      const trange_type trange_; ///< Tiled range type
      const shape_type shape_; ///< Tensor shape
      std::shared_ptr<pmap_interface> pmap_; ///< Process map for tiles

    public:

      /// Constructor

      /// The size of shape must be equal to the volume of the tiled range tiles.
      /// \param world The world where this tensor will live
      /// \param trange The tiled range for this tensor
      /// \param shape The shape of this tensor
      /// \param pmap The tile-process map
      /// \throw TiledArray::Exception When the size of shape is not equal to
      /// zero
      TensorImpl(World& world, const trange_type& trange, const shape_type& shape,
          const std::shared_ptr<pmap_interface>& pmap) :
        world_(world), trange_(trange), shape_(shape), pmap_(pmap)
      {
        // Validate input data.
        TA_ASSERT(pmap_);
        TA_ASSERT(pmap_->size() == trange_.tiles().volume());
        TA_ASSERT(pmap_->rank() == typename pmap_interface::size_type(world_.rank()));
        TA_ASSERT(pmap_->procs() == typename pmap_interface::size_type(world_.size()));
        TA_ASSERT(shape_.validate(trange_.tiles()));
      }

      /// Virtual destructor
      virtual ~TensorImpl() { }

      /// Tensor process map accessor

      /// \return A shared pointer to the process map of this tensor
      /// \throw nothing
      const std::shared_ptr<pmap_interface>& pmap() const { return pmap_; }

      /// Tensor tile size array accessor

      /// \return The size array of the tensor tiles
      /// \throw nothing
      const range_type& range() const { return trange_.tiles(); }

      /// Tensor tile volume accessor

      /// \return The number of tiles in the tensor
      /// \throw nothing
      size_type size() const { return trange_.tiles().volume(); }

      /// Local element count

      /// This function is primarily available for debugging  purposes. The
      /// returned value is volatile and may change at any time; you should not
      /// rely on it in your algorithms.
      /// \return The current number of local tiles stored in the tensor.
      size_type local_size() const { return pmap_->local_size(); }

      /// Query a tile owner

      /// \tparam Index The index type
      /// \param i The tile index to query
      /// \return The process ID of the node that owns tile \c i
      /// \throw TiledArray::Exception When \c i is outside the tiled range tile
      /// range
      /// \throw TiledArray::Exception When the process map has not been set
      template <typename Index>
      ProcessID owner(const Index& i) const {
        TA_ASSERT(trange_.tiles().includes(i));
        return pmap_->owner(trange_.tiles().ordinal(i));
      }

      /// Query for a locally owned tile

      /// \tparam Index The index type
      /// \param i The tile index to query
      /// \return \c true if the tile is owned by this node, otherwise \c false
      /// \throw TiledArray::Exception When the process map has not been set
      template <typename Index>
      bool is_local(const Index& i) const {
        TA_ASSERT(trange_.tiles().includes(i));
        return pmap_->is_local(trange_.tiles().ordinal(i));
      }

      /// Query for a zero tile

      /// \tparam Index The index type
      /// \param i The tile index to query
      /// \return \c true if the tile is zero, otherwise \c false
      /// \throw TiledArray::Exception When \c i is outside the tiled range tile
      /// range
      template <typename Index>
      bool is_zero(const Index& i) const {
        TA_ASSERT(trange_.tiles().includes(i));
        return shape_.is_zero(trange_.tiles().ordinal(i));
      }

      /// Query the density of the tensor

      /// \return \c true if the tensor is dense, otherwise false
      /// \throw nothing
      bool is_dense() const { return shape_.is_dense(); }

      /// Tensor shape accessor

      /// \return A reference to the tensor shape map
      /// \throw TiledArray::Exception When this tensor is dense
      const shape_type& shape() const { return shape_; }

      /// Tiled range accessor

      /// \return The tiled range of the tensor
      const trange_type& trange() const { return trange_; }

      /// \deprecated use TensorImpl::world()
      DEPRECATED World& get_world() const { return world_; }

      /// World accessor

      /// \return A reference to the world that contains this tensor
      World& world() const { return world_; }

    }; // class TensorImpl


#ifndef TILEDARRAY_HEADER_ONLY

    extern template
    class TensorImpl<DensePolicy>;
    extern template
    class TensorImpl<SparsePolicy>;

#endif // TILEDARRAY_HEADER_ONLY

  }  // namespace detail
}  // namespace TiledArray

#endif // TILEDARRAY_TENSOR_IMPL_H__INCLUDED
