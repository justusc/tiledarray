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
 *  Justus Calvin
 *  Department of Chemistry, Virginia Tech
 *
 *  dist_op.h
 *  Oct 11, 2013
 *
 */

#ifndef TILEDARRAY_DIST_EVAL_DIST_OP_H__INCLUDED
#define TILEDARRAY_DIST_EVAL_DIST_OP_H__INCLUDED

#include <TiledArray/madness.h>
#include <TiledArray/error.h>
#include <TiledArray/dist_op/dist_cache.h>
#include <TiledArray/dist_op/group.h>
#include <TiledArray/dist_op/lazy_sync.h>

namespace TiledArray {

  class Communicator {
  private:

    mutable madness::World* world_;

    /// Delayed send callback object

    /// This callback object is used to send local data to a remove process
    /// once it has been set.
    /// \tparam T The type of data to be sent
    template <typename Key, typename T>
    class DelayedSend : public madness::CallbackInterface {
    private:
      madness::World& world_; ///< The communication world
      const ProcessID dest_; ///< The destination process id
      const Key key_; ///< The distributed id associated with \c value_
      madness::Future<T> value_; ///< The data to be sent

      // Not allowed
      DelayedSend(const DelayedSend<Key, T>&);
      DelayedSend<Key, T>& operator=(const DelayedSend<Key, T>&);

    public:

      /// Constructor

      /// \param ds The distributed container that owns element i
      /// \param i The element to be moved
      DelayedSend(madness::World& world, const ProcessID dest,
          const Key& key, const madness::Future<T>& value) :
        world_(world), dest_(dest), key_(key), value_(value)
      { }

      virtual ~DelayedSend() { }

      /// Notify this object that the future has been set.

      /// This will set the value of the future on the remote node and delete
      /// this callback object.
      virtual void notify() {
        TA_ASSERT(value_.probe());
        Communicator d_op(world_);
        d_op.send(dest_, key_, value_.get());
        delete this;
      }
    }; // class DelayedSend


    template <typename Key, typename T>
    static void bcast_children(madness::World* world, const Key& key,
        const T& value, const ProcessID root)
    {
      // Get the parent and child processes in the binary tree that will be used
      // to broadcast the data.
      ProcessID parent = -1, child0 = -1, child1 = -1;
      world->mpi.binary_tree_info(root, parent, child0, child1);

      if(child0 != -1)
        world->taskq.add(child0, Communicator::template bcast_task<Key, T>, world,
            key, value, root, madness::TaskAttributes::hipri());
      if(child1 != -1)
        world->taskq.add(child1, Communicator::template bcast_task<Key, T>, world,
            key, value, root, madness::TaskAttributes::hipri());
    }

    template <typename Key, typename T>
    static void bcast_task(madness::World* world, const Key& key,
        const T& value, ProcessID root)
    {
      dist_op::DistCache<Key>::set_cache_data(key, value);
      bcast_children(world, key, value, root);
    }

    template <typename Key, typename T>
    static void group_bcast_children(madness::World* world,
        const dist_op::Group& group, const Key& key, const T& value,
        const ProcessID group_root)
    {
      // Get the parent and child processes in the binary tree that will be used
      // to broadcast the data.
      ProcessID parent = -1, child0 = -1, child1 = -1;
      group.make_tree(parent, child0, child1, group_root);

      if(child0 != -1)
        world->taskq.add(child0, Communicator::template group_bcast_task<Key, T>,
            world, group.id(), key, value, group_root, madness::TaskAttributes::hipri());
      if(child1 != -1)
        world->taskq.add(child1, Communicator::template group_bcast_task<Key, T>,
            world, group.id(), key, value, group_root, madness::TaskAttributes::hipri());
    }

    template <typename Key, typename T>
    static void group_bcast_task(madness::World* world,
        const dist_op::DistributedID& group_key, const Key& key, const T& value,
        ProcessID group_root)
    {
      dist_op::DistCache<Key>::set_cache_data(key, value);
      const madness::Future<dist_op::Group> group = dist_op::Group::get_group(group_key);

      if(group.probe()) {
        group_bcast_children(world, group, key, value, group_root);
      } else {
        world->taskq.add(& Communicator::template group_bcast_children<Key, T>,
            world, group, key, value, group_root, madness::TaskAttributes::hipri());
      }
    }

  public:

    /// Constructor

    /// \param world The world that will be used to send/receive messages
    Communicator(madness::World& world) : world_(&world) { }

    /// Copy constructor

    /// \param other The object to be copied
    Communicator(const Communicator& other) : world_(other.world_) { }

    /// Copy assignment operator

    /// \param other The object to be copied
    /// \return A reference to this object
    Communicator& operator=(const Communicator& other) {
      world_ = other.world_;
      return *this;
    }

    /// Receive data from remote node

    /// \tparam T The data type stored in cache
    /// \param did The distributed ID
    /// \return A future to the data
    template <typename T, typename Key>
    static madness::Future<T> recv(const Key& key) {
      return dist_op::DistCache<Key>::template get_cache_data<T>(key);
    }

    /// Send value to \c dest

    /// \tparam T The value type
    /// \param world The world that will be used to send the value
    /// \param dest The node where the data will be sent
    /// \param did The distributed id that is associatied with the data
    /// \param value The data to be sent
    template <typename Key, typename T>
    typename madness::disable_if<madness::is_future<T> >::type
    send(const ProcessID dest, const Key& key, const T& value) const {
      typedef TiledArray::dist_op::DistCache<Key> dist_cache;

      if(world_->rank() == dest) {
        // When dest is this process, skip the task and set the future immediately.
        dist_cache::set_cache_data(key, value);
      } else {
        // Spawn a remote task to set the value
        world_->taskq.add(dest, dist_cache::template set_cache_data<T>, key, value,
            madness::TaskAttributes::hipri());
      }
    }

    /// Send \c value to \c dest

    /// \tparam T The value type
    /// \param world The world that will be used to send the value
    /// \param dest The node where the data will be sent
    /// \param did The distributed id that is associated with the data
    /// \param value The data to be sent
    template <typename Key, typename T>
    void send(ProcessID dest, const Key& key, const madness::Future<T>& value) const {
      typedef TiledArray::dist_op::DistCache<Key> dist_cache;

      if(world_->rank() == dest) {
        dist_cache::set_cache_data(key, value);
      } else {
        // The destination is not this node, so send it to the destination.
        if(value.probe()) {
          // Spawn a remote task to set the value
          world_->taskq.add(dest, dist_cache::template set_cache_data<T>, key,
              value.get(), madness::TaskAttributes::hipri());
        } else {
          // The future is not ready, so create a callback object that will
          // send value to the destination node when it is ready.
          DelayedSend<Key, T>* delayed_send_callback =
              new DelayedSend<Key, T>(*world_, dest, key, value);
          const_cast<madness::Future<T>&>(value).register_callback(delayed_send_callback);

        }
      }
    }

    /// Lazy sync

    /// Lazy sync functions are asynchronous barriers with a nullary functor
    /// that is called after all processes have called lazy sync with the same
    /// key.
    /// \param key The sync key
    /// \param op The sync operation to be executed on this process
    /// \note It is the user's responsibility to ensure that the key for each
    /// lazy sync operation is unique. You may reuse keys after the associated
    /// sync operations have been completed.
    template <typename Key, typename Op>
    void lazy_sync(const Key& key, const Op& op) const {
      dist_op::LazySync<Key, Op>::make(*world_, key, op);
    }


    /// Group lazy sync

    /// Lazy sync functions are asynchronous barriers with a nullary functor
    /// that is called after all processes have called lazy sync with the same
    /// key.
    /// \param key The sync key
    /// \param op The sync operation to be executed on this process
    /// \throw TiledArray::Exception When the world id of the group and the
    /// world id of this communicator are not equal.
    /// \throw TiledArray::Exception When this process is not in the group.
    /// \note It is the user's responsibility to ensure that the key for each
    /// lazy sync operation is unique. You may reuse keys after the associated
    /// sync operations have been completed.
    template <typename Key, typename Op>
    void lazy_sync(const Key& key, const Op& op, const dist_op::Group& group) const {
      TA_ASSERT(group.get_world().id() == world_->id());
      TA_ASSERT(group.rank(world_->rank()) != -1);
      dist_op::LazySync<Key, Op>::make(group, key, op);
    }

    /// Broadcast

    /// Broadcast data from the \c root process to all processes in \c world.
    /// The input/output data is held by \c value.
    /// \param[in] key The key associated with this broadcast
    /// \param[in,out] value On the \c root process, this is used as the input
    /// data that will be broadcast to all other processes in the group.
    /// On other processes it is used as the output to the broadcast
    /// \param root The process that owns the data to be broadcast
    /// \throw TiledArray::Exception When \c root is less than 0 or
    /// greater than or equal to the world size.
    /// \throw TiledArray::Exception When \c value has been set, except on the
    /// \c root process.
    template <typename Key, typename T>
    void bcast(const Key& key, madness::Future<T>& value, const ProcessID root) const {
      TA_ASSERT(root >= 0 && root < world_->size());
      TA_ASSERT((world_->rank() == root) || (! value.probe()));

      if(world_->size() > 1) { // Do nothing for the trivial case
        if(world_->rank() == root) {
          // This is the process that owns the data to be broadcast

          // Spawn remote tasks that will set the local cache for this broadcast
          // on other nodes.
          if(value.probe())
            // The value is ready so send it now
            bcast_children(world_, key, value.get(), root);
          else
            // The value is not ready so spawn a task to send the data when it
            // is ready.
            world_->taskq.add(Communicator::template bcast_children<Key, T>,
                world_, key, value, root, madness::TaskAttributes::hipri());
        } else {
          // Get the local cache value for the broad cast
          TA_ASSERT(! value.probe());
          dist_op::DistCache<Key>::get_cache_data(key, value);
        }
      }
    }

    /// Group broadcast

    /// Broadcast data from the \c group_root process to all processes in
    /// \c group. The input/output data is held by \c value.
    /// \param[in] key The key associated with this broadcast
    /// \param[in,out] value On the \c group_root process, this is used as the
    /// input data that will be broadcast to all other processes in the group.
    /// On other processes it is used as the output to the broadcast
    /// \param group_root The process in \c group that owns the data to be
    /// broadcast
    /// \param group The process group where value will be broadcast
    /// \throw TiledArray::Exception When the world id of \c group is not
    /// equal to that of the world used to construct this communicator.
    /// \throw TiledArray::Exception When \c group_root is less than 0 or
    /// greater than or equal to \c group size.
    /// \throw TiledArray::Exception When \c data has been set except on the
    /// \c root process.
    /// \throw TiledArray::Exception When this process is not in the group.
    template <typename Key, typename T>
    void bcast(const Key& key, madness::Future<T>& value,
        const ProcessID group_root, const dist_op::Group& group) const
    {
      TA_ASSERT(group.get_world().id() == world_->id());
      TA_ASSERT(group_root >= 0 && group_root < group.size());
      TA_ASSERT((group.rank() == group_root) || (! value.probe()));
      TA_ASSERT(group.rank(world_->rank()) != -1);

      if(group.size() > 1) { // Do nothing for the trivial case
        if(group.rank() == group_root) {
          // This is the process that owns the data to be broadcast
          if(value.probe())
            group_bcast_children(world_, group, key, value.get(), group_root);
          else
            world_->taskq.add(& Communicator::template group_bcast_children<Key, T>,
                world_, group, key, value, group_root, madness::TaskAttributes::hipri());
        } else {
          // This is not the root process, so retrieve the broadcast data
          dist_op::DistCache<Key>::get_cache_data(key, value);
        }
      }
    }

  }; // class Communicator

} // namespace TiledArray

#endif // TILEDARRAY_DIST_EVAL_DIST_OP_H__INCLUDED