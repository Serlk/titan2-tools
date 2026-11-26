// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package android.hardware; public interface SensorEventListener { void onSensorChanged(SensorEvent e); void onAccuracyChanged(Sensor s, int a); }
