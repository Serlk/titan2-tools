// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package android.hardware;
public final class Sensor {
	public static final int TYPE_ACCELEROMETER = 1, TYPE_MAGNETIC_FIELD = 2, TYPE_GYROSCOPE = 4, TYPE_LIGHT = 5,
		TYPE_PRESSURE = 6, TYPE_PROXIMITY = 8, TYPE_ALL = -1;
	public String getName() { return null; }
	public String getVendor() { return null; }
	public String getStringType() { return null; }
	public int getType() { return 0; }
	public float getMaximumRange() { return 0; }
	public float getResolution() { return 0; }
	public int getMinDelay() { return 0; }
}
