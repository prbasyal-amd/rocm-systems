////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
//
// Copyright (c) 2025, Advanced Micro Devices, Inc. All rights reserved.
//
// Developed by:
//
//                 AMD Research and AMD HSA Software Development
//
//                 Advanced Micro Devices, Inc.
//
//                 www.amd.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include "core/inc/amd_pm4_queue.h"

#include <cstdint>

#include "core/inc/amd_gpu_agent.h"
#include "core/inc/exceptions.h"
#include "core/util/atomic_helpers.h"

namespace rocr {
namespace AMD {

PM4Queue::PM4Queue(core::SharedQueue* shared_queue, GpuAgent& agent, uint32_t node_id,
                   uint64_t flags)
    : Queue(shared_queue, false, !agent.is_xgmi_cpu_gpu()), agent_(agent) {
  HsaQueueResource queue_resource = {0};

  if (IsDeviceMemRingBuf()) {
    ring_buf_ = agent.finegrain_allocator()(
        HSA_PAGE_SIZE_4KB,
        core::MemoryRegion::AllocateExecutable | core::MemoryRegion::AllocateUncached);
  } else {
    ring_buf_ = agent.system_allocator()(
        4096, 4096, core::MemoryRegion::AllocateExecutable | core::MemoryRegion::AllocateUncached);
  }

  hsa_status_t err =
      agent.driver().CreateQueue(node_id, HSA_QUEUE_COMPUTE, 100, HSA_QUEUE_PRIORITY_NORMAL, 0,
                                 ring_buf_, 4096, nullptr, queue_resource);
  if (err != HSA_STATUS_SUCCESS) throw hsa_exception(err, "Driver failed to create a PM4 queue.\n");
}

PM4Queue::~PM4Queue() {
  Inactivate();

  if (shared_queue_) {
    if (IsDeviceMemQueueDescriptor()) {
      agent_.finegrain_deallocator()(static_cast<void*>(shared_queue_));
    } else {
      core::Runtime::runtime_singleton_->system_deallocator()(static_cast<void*>(shared_queue_));
    }
  }

  if (ring_buf_) {
    if (IsDeviceMemRingBuf()) {
      agent_.finegrain_deallocator()(ring_buf_);
    } else {
      agent_.system_deallocator()(ring_buf_);
    }
  }
}

hsa_status_t PM4Queue::Inactivate() {
  hsa_status_t err = HSA_STATUS_SUCCESS;

  if (active_.exchange(false, std::memory_order_relaxed)) {
    err = agent_.driver().DestroyQueue(queue_id_);
    atomic::Fence(std::memory_order_acquire);
  }

  return err;
}

hsa_status_t PM4Queue::SetPriority(HSA_QUEUE_PRIORITY priority) {
  return static_cast<hsa_status_t>(HSA_STATUS_ERROR_NOT_SUPPORTED);
}

uint64_t PM4Queue::LoadReadIndexAcquire() {
  return atomic::Load(queue_descriptor().read_id, std::memory_order_acquire);
};

uint64_t PM4Queue::LoadReadIndexRelaxed() {
  return atomic::Load(queue_descriptor().read_id, std::memory_order_relaxed);
}

uint64_t PM4Queue::LoadWriteIndexAcquire() {
  return atomic::Load(queue_descriptor().write_id, std::memory_order_acquire);
}

uint64_t PM4Queue::LoadWriteIndexRelaxed() {
  return atomic::Load(queue_descriptor().write_id, std::memory_order_relaxed);
}

void PM4Queue::StoreReadIndexRelease(uint64_t value) {
  atomic::Store(queue_descriptor().read_id, value, std::memory_order_release);
};

void PM4Queue::StoreReadIndexRelaxed(uint64_t value) {
  atomic::Store(queue_descriptor().read_id, value, std::memory_order_relaxed);
};

void PM4Queue::StoreWriteIndexRelease(uint64_t value) {
  atomic::Store(queue_descriptor().write_id, value, std::memory_order_release);
};

void PM4Queue::StoreWriteIndexRelaxed(uint64_t value) {
  atomic::Store(queue_descriptor().write_id, value, std::memory_order_relaxed);
};

uint64_t PM4Queue::CasWriteIndexAcquire(uint64_t expected, uint64_t value) {
  return atomic::Cas(queue_descriptor().write_id, value, expected, std::memory_order_acquire);
}

uint64_t PM4Queue::CasWriteIndexRelease(uint64_t expected, uint64_t value) {
  return atomic::Cas(queue_descriptor().write_id, value, expected, std::memory_order_release);
}

uint64_t PM4Queue::CasWriteIndexAcqRel(uint64_t expected, uint64_t value) {
  return atomic::Cas(queue_descriptor().write_id, value, expected, std::memory_order_acq_rel);
}

uint64_t PM4Queue::CasWriteIndexRelaxed(uint64_t expected, uint64_t value) {
  return atomic::Cas(queue_descriptor().write_id, value, expected, std::memory_order_relaxed);
}

uint64_t PM4Queue::AddWriteIndexAcquire(uint64_t value) {
  return atomic::Add(queue_descriptor().write_id, value, std::memory_order_acquire);
}

uint64_t PM4Queue::AddWriteIndexRelease(uint64_t value) {
  return atomic::Add(queue_descriptor().write_id, value, std::memory_order_release);
}

uint64_t PM4Queue::AddWriteIndexAcqRel(uint64_t value) {
  return atomic::Add(queue_descriptor().write_id, value, std::memory_order_acq_rel);
}

uint64_t PM4Queue::AddWriteIndexRelaxed(uint64_t value) {
  return atomic::Add(queue_descriptor().write_id, value, std::memory_order_relaxed);
}

hsa_status_t PM4Queue::SetCUMasking(uint32_t num_cu_mask_count, const uint32_t* cu_mask) {
  return static_cast<hsa_status_t>(HSA_STATUS_ERROR_NOT_SUPPORTED);
}

hsa_status_t PM4Queue::GetCUMasking(uint32_t num_cu_mask_count, uint32_t* cu_mask) {
  return static_cast<hsa_status_t>(HSA_STATUS_ERROR_NOT_SUPPORTED);
}

void PM4Queue::ExecutePM4(uint32_t* cmd_data, size_t cmd_size_bytes,
                          hsa_fence_scope_t acquire_fence, hsa_fence_scope_t release_fence,
                          hsa_signal_t* signal) {
  throw hsa_exception(static_cast<hsa_status_t>(HSA_STATUS_ERROR_NOT_SUPPORTED),
                      "ExecutePM4 not support for PM4Queue.\n");
}

void PM4Queue::SetProfiling(bool enabled) {
  throw hsa_exception(static_cast<hsa_status_t>(HSA_STATUS_ERROR_NOT_SUPPORTED),
                      "SetProfiling not support for PM4Queue.\n");
}

hsa_status_t PM4Queue::GetInfo(hsa_queue_info_attribute_t attribute, void* value) {
  return HSA_STATUS_SUCCESS;
}

bool PM4Queue::_IsA(Queue::rtti_t id) const { return id == &rtti_id_; }

}  // namespace AMD
}  // namespace rocr
