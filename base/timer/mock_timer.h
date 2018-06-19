// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TIMER_MOCK_TIMER_H_
#define BASE_TIMER_MOCK_TIMER_H_

#include "base/test/simple_test_tick_clock.h"
#include "base/timer/timer.h"

namespace base {

class TestSimpleTaskRunner;

// A mock implementation of base::Timer which requires being explicitly
// Fire()'d.
// Prefer using ScopedTaskEnvironment::MOCK_TIME + FastForward*() to this when
// possible
class MockTimer : public Timer {
 public:
  MockTimer(bool retain_user_task, bool is_repeating);
  ~MockTimer() override;

  // Testing method.
  void Fire();

 private:
  // Timer implementation.
  // MockTimer doesn't support SetTaskRunner. Do not use this.
  void SetTaskRunner(scoped_refptr<SequencedTaskRunner> task_runner) override;

  SimpleTestTickClock clock_;
  scoped_refptr<TestSimpleTaskRunner> test_task_runner_;
};

}  // namespace base

#endif  // BASE_TIMER_MOCK_TIMER_H_
