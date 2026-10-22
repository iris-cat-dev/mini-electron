// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_CONTAINERS_STACK_CONTAINER_H_
#define BASE_CONTAINERS_STACK_CONTAINER_H_

#include <stddef.h>
#include <iterator>
#include <memory>
#include <vector>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "build/build_config.h"

namespace base {

// An STL allocator whose first fitting allocation uses a caller-owned stack
// buffer. Further or oversized allocations use the fallback allocator. The
// Source must outlive every allocator/container referring to it.
template <typename T, size_t stack_capacity, typename FallbackAllocator>
class StackAllocator : public FallbackAllocator {
 public:
  using pointer = typename std::allocator_traits<FallbackAllocator>::pointer;
  using size_type = typename std::allocator_traits<FallbackAllocator>::size_type;

  struct Source {
    Source() : used_stack_buffer_(false) {}

    NO_SANITIZE("cfi-unrelated-cast")
    T* stack_buffer() { return reinterpret_cast<T*>(stack_buffer_); }
    NO_SANITIZE("cfi-unrelated-cast")
    const T* stack_buffer() const {
      return reinterpret_cast<const T*>(&stack_buffer_);
    }

    // Raw aligned storage avoids constructing unused elements in the capacity.
    alignas(T) char stack_buffer_[sizeof(T[stack_capacity])];
#if defined(__GNUC__) && !defined(ARCH_CPU_X86_FAMILY)
    static_assert(alignof(T) <= 16, "http://crbug.com/115612");
#endif
    bool used_stack_buffer_;
  };

  template <typename U>
  struct rebind {
    using other = StackAllocator<U, stack_capacity, FallbackAllocator>;
  };

  StackAllocator(const StackAllocator& other) : source_(other.source_) {}

  // Differently typed sources cannot safely share raw storage.
  template <typename U, size_t other_capacity, typename FA>
  StackAllocator(const StackAllocator<U, other_capacity, FA>&)
      : source_(nullptr) {}

  StackAllocator() : source_(nullptr) {}
  explicit StackAllocator(Source* source) : source_(source) {}

  pointer allocate(size_type count) {
    if (source_ && !source_->used_stack_buffer_ && count <= stack_capacity) {
      source_->used_stack_buffer_ = true;
      return source_->stack_buffer();
    }
    return std::allocator_traits<FallbackAllocator>::allocate(*this, count);
  }

  void deallocate(pointer buffer, size_type count) {
    if (source_ && buffer == source_->stack_buffer())
      source_->used_stack_buffer_ = false;
    else
      std::allocator_traits<FallbackAllocator>::deallocate(*this, buffer, count);
  }

 private:
  RAW_PTR_EXCLUSION Source* source_;
};

// Reserve the stack capacity before the first insertion so a smaller vector
// allocation cannot consume the single stack allocation. Copies of container()
// share its allocator/source and must not outlive this object.
template <typename TContainerType, int stack_capacity>
class StackContainer {
 public:
  using ContainerType = TContainerType;
  using ContainedType = typename ContainerType::value_type;
  using Allocator = typename ContainerType::allocator_type;

  StackContainer() : allocator_(&stack_data_), container_(allocator_) {
    container_.reserve(stack_capacity);
  }
  StackContainer(const StackContainer&) = delete;
  StackContainer& operator=(const StackContainer&) = delete;

  ContainerType& container() { return container_; }
  const ContainerType& container() const { return container_; }
  ContainerType* operator->() { return &container_; }
  const ContainerType* operator->() const { return &container_; }

#ifdef UNIT_TEST
  const typename Allocator::Source& stack_data() const { return stack_data_; }
#endif

 protected:
  // Declaration order keeps the backing source alive through deallocation.
  typename Allocator::Source stack_data_;
  NO_UNIQUE_ADDRESS Allocator allocator_;
  ContainerType container_;
};

template <typename TContainerType, int stack_capacity>
auto begin(const StackContainer<TContainerType, stack_capacity>& container)
    -> decltype(begin(container.container())) {
  return begin(container.container());
}

template <typename TContainerType, int stack_capacity>
auto begin(StackContainer<TContainerType, stack_capacity>& container)
    -> decltype(begin(container.container())) {
  return begin(container.container());
}

template <typename TContainerType, int stack_capacity>
auto end(StackContainer<TContainerType, stack_capacity>& container)
    -> decltype(end(container.container())) {
  return end(container.container());
}

template <typename TContainerType, int stack_capacity>
auto end(const StackContainer<TContainerType, stack_capacity>& container)
    -> decltype(end(container.container())) {
  return end(container.container());
}

template <typename T, size_t stack_capacity,
          typename FallbackAllocator = std::allocator<T>>
class StackVector
    : public StackContainer<
          std::vector<T, StackAllocator<T, stack_capacity, FallbackAllocator>>,
          stack_capacity> {
 public:
  StackVector()
      : StackContainer<
            std::vector<T, StackAllocator<T, stack_capacity, FallbackAllocator>>,
            stack_capacity>() {}

  // Copy elements into this object's own stack buffer, never the other source.
  StackVector(const StackVector& other) : StackVector() {
    this->container().assign(other->begin(), other->end());
  }

  StackVector& operator=(const StackVector& other) {
    this->container().assign(other->begin(), other->end());
    return *this;
  }

  T& operator[](size_t index) { return this->container().operator[](index); }
  const T& operator[](size_t index) const {
    return this->container().operator[](index);
  }
};

}  // namespace base

#endif  // BASE_CONTAINERS_STACK_CONTAINER_H_
