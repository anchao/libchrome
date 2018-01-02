// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base;

import android.support.test.InstrumentationRegistry;
import android.support.test.filters.SmallTest;
import android.support.test.runner.AndroidJUnit4;

import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Feature;

/**
 * Test class for {@link CommandLineInitUtil}.
 */
@RunWith(AndroidJUnit4.class)
public class CommandLineInitUtilTest {
    @Before
    public void setUp() throws Exception {
        CommandLineInitUtil.initCommandLine(
                InstrumentationRegistry.getInstrumentation().getTargetContext(),
                CommandLineFlags.getTestCmdLineFile());
    }

    /**
     * Verifies that the default command line flags get set for Chrome Public tests.
     */
    @Test
    @SmallTest
    @Feature({"CommandLine"})
    public void testDefaultCommandLineFlagsSet() {
        Assert.assertTrue("CommandLine not initialized after startup", CommandLine.isInitialized());

        final CommandLine commandLine = CommandLine.getInstance();
        Assert.assertTrue(commandLine.hasSwitch("enable-test-intents"));
    }
}
