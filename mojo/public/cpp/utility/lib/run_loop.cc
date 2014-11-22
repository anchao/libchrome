// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/utility/run_loop.h"

#include <assert.h>

#include <algorithm>
#include <vector>

#include "mojo/public/cpp/utility/lib/thread_local.h"
#include "mojo/public/cpp/utility/run_loop_handler.h"

namespace mojo {
namespace {

internal::ThreadLocalPointer<RunLoop> current_run_loop;

const MojoTimeTicks kInvalidTimeTicks = static_cast<MojoTimeTicks>(0);

}  // namespace

// State needed for one iteration of WaitMany().
struct RunLoop::WaitState {
  WaitState() : deadline(MOJO_DEADLINE_INDEFINITE) {}

  std::vector<Handle> handles;
  std::vector<MojoHandleSignals> handle_signals;
  MojoDeadline deadline;
};

struct RunLoop::RunState {
  RunState() : should_quit(false) {}

  bool should_quit;
};

RunLoop::RunLoop()
    : run_state_(NULL), next_handler_id_(0), next_sequence_number_(0) {
  assert(!current());
  current_run_loop.Set(this);
}

RunLoop::~RunLoop() {
  assert(current() == this);
  NotifyHandlers(MOJO_RESULT_ABORTED, IGNORE_DEADLINE);
  current_run_loop.Set(NULL);
}

// static
void RunLoop::SetUp() {
  current_run_loop.Allocate();
}

// static
void RunLoop::TearDown() {
  assert(!current());
  current_run_loop.Free();
}

// static
RunLoop* RunLoop::current() {
  return current_run_loop.Get();
}

void RunLoop::AddHandler(RunLoopHandler* handler,
                         const Handle& handle,
                         MojoHandleSignals handle_signals,
                         MojoDeadline deadline) {
  assert(current() == this);
  assert(handler);
  assert(handle.is_valid());
  // Assume it's an error if someone tries to reregister an existing handle.
  assert(0u == handler_data_.count(handle));
  HandlerData handler_data;
  handler_data.handler = handler;
  handler_data.handle_signals = handle_signals;
  handler_data.deadline =
      (deadline == MOJO_DEADLINE_INDEFINITE)
          ? kInvalidTimeTicks
          : GetTimeTicksNow() + static_cast<MojoTimeTicks>(deadline);
  handler_data.id = next_handler_id_++;
  handler_data_[handle] = handler_data;
}

void RunLoop::RemoveHandler(const Handle& handle) {
  assert(current() == this);
  handler_data_.erase(handle);
}

bool RunLoop::HasHandler(const Handle& handle) const {
  return handler_data_.find(handle) != handler_data_.end();
}

void RunLoop::Run() {
  RunInternal(UNTIL_EMPTY);
}

void RunLoop::RunUntilIdle() {
  RunInternal(UNTIL_IDLE);
}

void RunLoop::RunInternal(RunMode run_mode) {
  assert(current() == this);
  RunState* old_state = run_state_;
  RunState run_state;
  run_state_ = &run_state;
  for (;;) {
    bool did_work = DoDelayedWork();
    if (run_state.should_quit)
      break;
    did_work |= Wait(run_mode == UNTIL_IDLE);
    if (run_state.should_quit)
      break;
    if (!did_work && run_mode == UNTIL_IDLE)
      break;
  }
  run_state_ = old_state;
}

bool RunLoop::DoDelayedWork() {
  MojoTimeTicks now = GetTimeTicksNow();
  if (!delayed_tasks_.empty() && delayed_tasks_.top().run_time <= now) {
    PendingTask task = delayed_tasks_.top();
    delayed_tasks_.pop();
    task.task.Run();
    return true;
  }
  return false;
}

void RunLoop::Quit() {
  assert(current() == this);
  if (run_state_)
    run_state_->should_quit = true;
}

void RunLoop::PostDelayedTask(const Closure& task, MojoTimeTicks delay) {
  assert(current() == this);
  MojoTimeTicks run_time = delay + GetTimeTicksNow();
  delayed_tasks_.push(PendingTask(task, run_time, next_sequence_number_++));
}

bool RunLoop::Wait(bool non_blocking) {
  const WaitState wait_state = GetWaitState(non_blocking);
  if (wait_state.handles.empty() && delayed_tasks_.empty()) {
    Quit();
    return false;
  }

  const MojoResult result = WaitMany(
      wait_state.handles, wait_state.handle_signals, wait_state.deadline);
  if (result >= 0) {
    const size_t index = static_cast<size_t>(result);
    assert(handler_data_.find(wait_state.handles[index]) !=
           handler_data_.end());
    handler_data_[wait_state.handles[index]].handler->OnHandleReady(
        wait_state.handles[index]);
    return true;
  }

  switch (result) {
    case MOJO_RESULT_INVALID_ARGUMENT:
    case MOJO_RESULT_FAILED_PRECONDITION:
      return RemoveFirstInvalidHandle(wait_state);
    case MOJO_RESULT_DEADLINE_EXCEEDED:
      return NotifyHandlers(MOJO_RESULT_DEADLINE_EXCEEDED, CHECK_DEADLINE);
  }

  assert(false);
  return false;
}

bool RunLoop::NotifyHandlers(MojoResult error, CheckDeadline check) {
  bool notified = false;

  // Make a copy in case someone tries to add/remove new handlers as part of
  // notifying.
  const HandleToHandlerData cloned_handlers(handler_data_);
  const MojoTimeTicks now(GetTimeTicksNow());
  for (HandleToHandlerData::const_iterator i = cloned_handlers.begin();
       i != cloned_handlers.end();
       ++i) {
    // Only check deadline exceeded if that's what we're notifying.
    if (check == CHECK_DEADLINE &&
        (i->second.deadline == kInvalidTimeTicks || i->second.deadline > now)) {
      continue;
    }

    // Since we're iterating over a clone of the handlers, verify the handler
    // is still valid before notifying.
    if (handler_data_.find(i->first) == handler_data_.end() ||
        handler_data_[i->first].id != i->second.id) {
      continue;
    }

    RunLoopHandler* handler = i->second.handler;
    handler_data_.erase(i->first);
    handler->OnHandleError(i->first, error);
    notified = true;
  }

  return notified;
}

bool RunLoop::RemoveFirstInvalidHandle(const WaitState& wait_state) {
  for (size_t i = 0; i < wait_state.handles.size(); ++i) {
    const MojoResult result = mojo::Wait(wait_state.handles[i],
                                         wait_state.handle_signals[i],
                                         static_cast<MojoDeadline>(0));
    if (result == MOJO_RESULT_INVALID_ARGUMENT ||
        result == MOJO_RESULT_FAILED_PRECONDITION) {
      // Remove the handle first, this way if OnHandleError() tries to remove
      // the handle our iterator isn't invalidated.
      assert(handler_data_.find(wait_state.handles[i]) != handler_data_.end());
      RunLoopHandler* handler = handler_data_[wait_state.handles[i]].handler;
      handler_data_.erase(wait_state.handles[i]);
      handler->OnHandleError(wait_state.handles[i], result);
      return true;
    }
    assert(MOJO_RESULT_DEADLINE_EXCEEDED == result || MOJO_RESULT_OK == result);
  }
  return false;
}

RunLoop::WaitState RunLoop::GetWaitState(bool non_blocking) const {
  WaitState wait_state;
  MojoTimeTicks min_time = kInvalidTimeTicks;
  for (HandleToHandlerData::const_iterator i = handler_data_.begin();
       i != handler_data_.end();
       ++i) {
    wait_state.handles.push_back(i->first);
    wait_state.handle_signals.push_back(i->second.handle_signals);
    if (!non_blocking && i->second.deadline != kInvalidTimeTicks &&
        (min_time == kInvalidTimeTicks || i->second.deadline < min_time)) {
      min_time = i->second.deadline;
    }
  }
  if (!delayed_tasks_.empty()) {
    MojoTimeTicks delayed_min_time = delayed_tasks_.top().run_time;
    if (min_time == kInvalidTimeTicks)
      min_time = delayed_min_time;
    else
      min_time = std::min(min_time, delayed_min_time);
  }
  if (non_blocking) {
    wait_state.deadline = static_cast<MojoDeadline>(0);
  } else if (min_time != kInvalidTimeTicks) {
    const MojoTimeTicks now = GetTimeTicksNow();
    if (min_time < now)
      wait_state.deadline = static_cast<MojoDeadline>(0);
    else
      wait_state.deadline = static_cast<MojoDeadline>(min_time - now);
  }
  return wait_state;
}

RunLoop::PendingTask::PendingTask(const Closure& task,
                                  MojoTimeTicks run_time,
                                  uint64_t sequence_number)
    : task(task), run_time(run_time), sequence_number(sequence_number) {
}

RunLoop::PendingTask::~PendingTask() {
}

bool RunLoop::PendingTask::operator<(const RunLoop::PendingTask& other) const {
  if (run_time != other.run_time) {
    // std::priority_queue<> puts the least element at the end of the queue. We
    // want the soonest eligible task to be at the head of the queue, so
    // run_times further in the future are considered lesser.
    return run_time > other.run_time;
  }

  return sequence_number > other.sequence_number;
}

}  // namespace mojo
