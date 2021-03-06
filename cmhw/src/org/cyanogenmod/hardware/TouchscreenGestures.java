/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.cyanogenmod.hardware;

import org.cyanogenmod.internal.util.FileUtils;

import cyanogenmod.hardware.TouchscreenGesture;

/**
 * Touchscreen gestures API
 *
 * A device may implement several touchscreen gestures for use while
 * the display is turned off, such as drawing alphabets and shapes.
 * These gestures can be interpreted by userspace to activate certain
 * actions and launch certain apps, such as to skip music tracks,
 * to turn on the flashlight, or to launch the camera app.
 *
 * This *should always* be supported by the hardware directly.
 * A lot of recent touch controllers have a firmware option for this.
 *
 * This API provides support for enumerating the gestures
 * supported by the touchscreen.
 */
public class TouchscreenGestures {

    private static final String[] GESTURE_SYSFS = {
        "DOWN",
        "LEFT",
        "RIGHT",
        "o",
        "v",
    };

    // Id, name, keycode
    private static final TouchscreenGesture[] TOUCHSCREEN_GESTURES = {
        new TouchscreenGesture(1, "Down arrow", 199),
        new TouchscreenGesture(2, "Left arrow", 253),
        new TouchscreenGesture(3, "Right arrow", 254),
        new TouchscreenGesture(7, "Letter O", 250),
        new TouchscreenGesture(7, "Letter V", 252),
    };

    /**
     * Whether device supports touchscreen gestures
     *
     * @return boolean Supported devices must return always true
     */
    public static boolean isSupported() {
        for (String path : GESTURE_PATHS) {
				String actual_path = "/sys/devices/soc.0/f9924000.i2c/i2c-2/2-004a/" + path;
            if (!FileUtils.isFileWritable(actual_path) ||
                    !FileUtils.isFileReadable(actual_path)) {
                return false;
            }
        }
        return true;
    }

    /*
     * Get the list of available gestures. A mode has an integer
     * identifier and a string name.
     *
     * It is the responsibility of the upper layers to
     * map the name to a human-readable format or perform translation.
     */
    public static TouchscreenGesture[] getAvailableGestures() {
        return TOUCHSCREEN_GESTURES;
    }

    /**
     * This method allows to set the activation status of a gesture
     *
     * @param gesture The gesture to be activated
     *        state   The new activation status of the gesture
     * @return boolean Must be false if gesture is not supported
     *         or the operation failed; true in any other case.
     */
    public static boolean setGestureEnabled(
            final TouchscreenGesture gesture, final boolean state) {
        final String stateStr = state ? "0x01" : "0x02";
		  final String actual_path = "/sys/devices/soc.0/f9924000.i2c/i2c-2/2-004a/" + GESTURE_PATHS[gesture.id];
        FileUtils.writeLine("/sys/devices/soc.0/f9924000.i2c/i2c-2/2-004a/en_gesture", "1");
        return FileUtils.writeLine(actual_path, getEnableString(enable));
    }
}
