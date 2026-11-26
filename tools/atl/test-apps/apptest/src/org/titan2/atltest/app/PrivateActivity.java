// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package org.titan2.atltest.app;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/** Not exported: another package must not be able to start it. */
public class PrivateActivity extends Activity {
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		Log.i(MainActivity.TAG, "private activity created in " + getPackageName() + " (must only happen for the own package)");
	}
}
