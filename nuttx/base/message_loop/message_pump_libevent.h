// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_MESSAGE_LOOP_MESSAGE_PUMP_LIBEVENT_H_
#define BASE_MESSAGE_LOOP_MESSAGE_PUMP_LIBEVENT_H_

#include <memory>

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/watchable_io_message_pump_posix.h"
#include "base/threading/thread_checker.h"
#include "base/time/time.h"

// Declare structs we need from libevent.h rather than including it
struct event_base;
struct event;

namespace base {

// Class to monitor sockets and issue callbacks when sockets are ready for I/O
// TODO(dkegel): add support for background file IO somehow
class BASE_EXPORT MessagePumpLibevent : public MessagePump,
                                        public WatchableIOMessagePumpPosix {
 public:
  MessagePumpLibevent();
  ~MessagePumpLibevent() override;

  // MessagePump methods:
  void Run(Delegate* delegate) override;
  void Quit() override;
  void ScheduleWork() override;
  void ScheduleDelayedWork(const TimeTicks& delayed_work_time) override;

 private:
  friend class MessagePumpLibeventTest;

  // Risky part of constructor.  Returns true on success.
  bool Init();

  // Called by libevent to tell us a registered FD can be read/written to.
  static void OnLibeventNotification(int fd, short flags, void* context);

  // Unix pipe used to implement ScheduleWork()
  // ... callback; called by libevent inside Run() when pipe is ready to read
  static void OnWakeup(int socket, short flags, void* context);

  // This flag is set to false when Run should return.
  bool keep_running_;

  // This flag is set when inside Run.
  bool in_run_;

  // This flag is set if libevent has processed I/O events.
  bool processed_io_events_;

  // The time at which we should call DoDelayedWork.
  TimeTicks delayed_work_time_;

  // Libevent dispatcher.  Watches all sockets registered with it, and sends
  // readiness callbacks when a socket is ready for I/O.
  event_base* event_base_;

  // ... write end; ScheduleWork() writes a single byte to it
  int wakeup_pipe_in_;
  // ... read end; OnWakeup reads it and then breaks Run() out of its sleep
  int wakeup_pipe_out_;
  // ... libevent wrapper for read end
  event* wakeup_event_;

  ThreadChecker watch_file_descriptor_caller_checker_;
  DISALLOW_COPY_AND_ASSIGN(MessagePumpLibevent);
};

}  // namespace base

#endif  // BASE_MESSAGE_LOOP_MESSAGE_PUMP_LIBEVENT_H_
