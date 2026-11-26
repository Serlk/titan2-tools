// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
//
// ATL test app for patches 0006 (device identity) and 0007 (SensorManager on IIO): prints
// Build/SystemProperties/ANDROID_ID, the sensor list and sensor events to stdout (prefix
// "ATLTEST") and to the window.
package org.titan2.atltest;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.widget.TextView;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class MainActivity extends Activity implements SensorEventListener {
	private TextView text;
	private final StringBuilder info = new StringBuilder();
	private final Map<String, String> latest = new HashMap<String, String>();
	private final Map<String, Integer> counts = new HashMap<String, Integer>();
	private SensorManager sm;
	private Thread mainThread;

	private void log(String s) {
		System.out.println("ATLTEST " + s);
	}

	private void line(String s) {
		log(s);
		info.append(s).append('\n');
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		mainThread = Thread.currentThread();
		text = new TextView(this);
		setContentView(text);

		line("MANUFACTURER=" + Build.MANUFACTURER + " BRAND=" + Build.BRAND + " MODEL=" + Build.MODEL);
		line("DEVICE=" + Build.DEVICE + " PRODUCT=" + Build.PRODUCT + " BOARD=" + Build.BOARD + " HARDWARE=" + Build.HARDWARE);
		line("FINGERPRINT=" + Build.FINGERPRINT);
		line("ID=" + Build.ID + " DISPLAY=" + Build.DISPLAY + " TAGS=" + Build.TAGS + " TYPE=" + Build.TYPE);
		line("RELEASE=" + Build.VERSION.RELEASE + " SDK_INT=" + Build.VERSION.SDK_INT + " SECURITY_PATCH=" + Build.VERSION.SECURITY_PATCH + " INCREMENTAL=" + Build.VERSION.INCREMENTAL);
		line("SUPPORTED_ABIS=" + Arrays.toString(Build.SUPPORTED_ABIS) + " SERIAL=" + Build.SERIAL + " getSerial=" + Build.getSerial());
		line("SOC=" + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL);
		try {
			Class<?> sp = Class.forName("android.os.SystemProperties");
			Method get = sp.getMethod("get", String.class);
			Method getDef = sp.getMethod("get", String.class, String.class);
			line("reflection ro.product.model=" + get.invoke(null, "ro.product.model") + " ro.build.version.sdk=" + get.invoke(null, "ro.build.version.sdk") + " ro.board.platform=" + get.invoke(null, "ro.board.platform") + " unset=[" + get.invoke(null, "ro.nonexistent") + "] def=" + getDef.invoke(null, "ro.nonexistent", "dflt"));
		} catch (Exception e) {
			line("reflection failed: " + e);
		}
		line("ANDROID_ID=" + Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID));

		sm = (SensorManager)getSystemService(SENSOR_SERVICE);
		List<Sensor> all = sm.getSensorList(Sensor.TYPE_ALL);
		line("sensors: " + all.size());
		for (Sensor s : all)
			line("  " + s.getName() + " type=" + s.getType() + " (" + s.getStringType() + ") vendor=" + s.getVendor() + " max=" + s.getMaximumRange() + " res=" + s.getResolution() + " minDelay=" + s.getMinDelay());
		line("default pressure: " + sm.getDefaultSensor(Sensor.TYPE_PRESSURE));
		int[] types = {Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_MAGNETIC_FIELD, Sensor.TYPE_LIGHT, Sensor.TYPE_PROXIMITY};
		for (int t : types) {
			Sensor s = sm.getDefaultSensor(t);
			line("register type " + t + ": " + (s == null ? "none" : s.getName() + " -> " + sm.registerListener(this, s, SensorManager.SENSOR_DELAY_UI)));
		}
		text.setText(info);
		new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
			@Override
			public void run() {
				log("counts after 40 s: " + counts);
				sm.unregisterListener(MainActivity.this);
				log("unregistered");
			}
		}, 40000);
	}

	@Override
	public void onSensorChanged(SensorEvent event) {
		String name = event.sensor.getName();
		Integer n = counts.get(name);
		n = n == null ? 1 : n + 1;
		counts.put(name, n);
		String v = String.format("%s: %.3f %.3f %.3f", name, event.values[0], event.values[1], event.values[2]);
		if (n <= 3 || event.sensor.getType() == Sensor.TYPE_LIGHT || event.sensor.getType() == Sensor.TYPE_PROXIMITY)
			log("event #" + n + " " + v + " ts=" + event.timestamp + " main=" + (Thread.currentThread() == mainThread));
		latest.put(name, v);
		StringBuilder sb = new StringBuilder(info);
		for (String s : latest.values())
			sb.append(s).append('\n');
		text.setText(sb);
	}

	@Override
	public void onAccuracyChanged(Sensor sensor, int accuracy) {
		log("accuracy " + sensor.getName() + " " + accuracy);
	}

	@Override
	protected void onDestroy() {
		sm.unregisterListener(this);
		super.onDestroy();
	}
}
