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

#ifndef HSA_RUNTIME_CORE_INC_AMD_PM4_QUEUE_H_
#define HSA_RUNTIME_CORE_INC_AMD_PM4_QUEUE_H_

#include <atomic>
#include <cstdint>
#include <limits>

#include "core/inc/queue.h"
#include "inc/amd_hsa_pm4_queue.h"

namespace rocr {
namespace AMD {

class GpuAgent;

class PM4Queue : public core::Queue {
 public:
  using QueueDescriptorT = amd_pm4_queue_t;
  PM4Queue(core::SharedQueue* shared_queue, GpuAgent& agent, uint32_t node_id, uint64_t flags);
  ~PM4Queue();
  hsa_status_t Inactivate() override;
  hsa_status_t SetPriority(HSA_QUEUE_PRIORITY priority) override;
  uint64_t LoadReadIndexAcquire() override;
  uint64_t LoadReadIndexRelaxed() override;
  uint64_t LoadWriteIndexAcquire() override;
  uint64_t LoadWriteIndexRelaxed() override;
  void StoreReadIndexRelease(uint64_t value) override;
  void StoreReadIndexRelaxed(uint64_t value) override;
  void StoreWriteIndexRelease(uint64_t value) override;
  void StoreWriteIndexRelaxed(uint64_t value) override;
  uint64_t CasWriteIndexAcquire(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexRelease(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexAcqRel(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexRelaxed(uint64_t expected, uint64_t value) override;
  uint64_t AddWriteIndexAcquire(uint64_t value) override;
  uint64_t AddWriteIndexRelease(uint64_t value) override;
  uint64_t AddWriteIndexAcqRel(uint64_t value) override;
  uint64_t AddWriteIndexRelaxed(uint64_t value) override;
  hsa_status_t SetCUMasking(uint32_t num_cu_mask_count, const uint32_t* cu_mask) override;
  hsa_status_t GetCUMasking(uint32_t num_cu_mask_count, uint32_t* cu_mask) override;
  void ExecutePM4(uint32_t* cmd_data, size_t cmd_size_bytes, hsa_fence_scope_t acquire_fence,
                  hsa_fence_scope_t release_fence, hsa_signal_t* signal) override;
  void SetProfiling(bool enabled) override;
  hsa_status_t GetInfo(hsa_queue_info_attribute_t attribute, void* value) override;

  __forceinline QueueDescriptorT& queue_descriptor() {
    return *std::get<QueueDescriptorT*>(queue_descriptor_);
  }

 protected:
  bool _IsA(Queue::rtti_t id) const override;

 private:
  static constexpr uint64_t INVALID_QUEUE_ID = std::numeric_limits<uint64_t>::max();
  static inline const int rtti_id_ = 0;
  GpuAgent& agent_;
  uint64_t queue_id_ = INVALID_QUEUE_ID;
  std::atomic<bool> active_ = false;
  // PM4 packet ring buffer. CP expects this to be 4KB.
  void* ring_buf_ = nullptr;
};

}  // namespace AMD
}  // namespace rocr

#endif  // HSA_RUNTIME_CORE_INC_AMD_PM4_QUEUE_H_
