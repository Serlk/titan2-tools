// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package org.titan2.atltest.app;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

/** Not exported: another package must not be able to bind it. */
public class PrivateService extends Service {
	@Override
	public IBinder onBind(Intent intent) {
		Log.i(MainActivity.TAG, "private service onBind in " + getPackageName() + " (must only happen for the own package)");
		return new Binder();
	}
}
