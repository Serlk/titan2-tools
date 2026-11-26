// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package android.hardware;
public class SensorManager {
	public static final int SENSOR_DELAY_UI = 2;
	public java.util.List<Sensor> getSensorList(int type) { return null; }
	public Sensor getDefaultSensor(int type) { return null; }
	public boolean registerListener(SensorEventListener l, Sensor s, int period) { return false; }
	public void unregisterListener(SensorEventListener l) {}
}
