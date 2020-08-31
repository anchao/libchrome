// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "nuttx/base/message_loop/message_pump_libevent.h"

#include <errno.h>
#include <unistd.h>

#include <utility>

#include "base/auto_reset.h"
#include "base/compiler_specific.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"

namespace base {
  MessagePumpLibevent::MessagePumpLibevent() { }
  MessagePumpLibevent::~MessagePumpLibevent() { }
  void MessagePumpLibevent::Run(Delegate* delegate) { }
  void MessagePumpLibevent::Quit() { }
  void MessagePumpLibevent::ScheduleWork() { }
  void MessagePumpLibevent::ScheduleDelayedWork(const TimeTicks& delayed_work_time) { }
  bool MessagePumpLibevent::Init() { return true; }
  void MessagePumpLibevent::OnLibeventNotification(int fd, short flags, void* context) { }
  void MessagePumpLibevent::OnWakeup(int socket, short flags, void* context) { }
}  // namespace base
