// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/message_loop/message_pump_glib.h"

#include <fcntl.h>
#include <math.h>

#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"

namespace base {

  MessagePumpGlib::MessagePumpGlib() { }
  MessagePumpGlib::~MessagePumpGlib() { }
  int MessagePumpGlib::HandlePrepare() { return 0; }
  bool MessagePumpGlib::HandleCheck() { return false; }
  void MessagePumpGlib::Run(Delegate* delegate) { }
  void MessagePumpGlib::Quit() { }
  void MessagePumpGlib::ScheduleWork() { }
  void MessagePumpGlib::ScheduleDelayedWork(const TimeTicks& delayed_work_time) { }

}  // namespace base
