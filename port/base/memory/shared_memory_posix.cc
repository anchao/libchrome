// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/shared_memory.h"
#include "base/memory/shared_memory_helper.h"

namespace base {

  bool SharedMemory::MapAt(off_t, size_t) { return false; }
  SharedMemory::SharedMemory() = default;
  SharedMemory::~SharedMemory() { }
  bool SharedMemory::Create(const SharedMemoryCreateOptions& options) { return false; }
  SharedMemory::SharedMemory(SharedMemoryHandle const&, bool) { }
  SharedMemoryHandle SharedMemory::GetReadOnlyHandle() const { return readonly_shm_; }

}  // namespace base
