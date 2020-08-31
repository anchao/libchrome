// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/debug/stack_trace.h"

namespace base {
namespace debug {

  StackTrace::StackTrace(size_t count) { }
  void StackTrace::Print() const { }
  void StackTrace::OutputToStream(std::ostream* os) const { }

}  // namespace debug
}  // namespace base
