// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package org.titan2.atltest.app;

import android.app.Service;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.util.Log;

/** Answers a Messenger message with its text and the name of the package that runs the service. */
public class EchoService extends Service {
	private final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper()) {
		@Override
		public void handleMessage(Message msg) {
			String text = msg.getData() == null ? null : msg.getData().getString("text");
			Log.i(MainActivity.TAG, "service request=" + text + " in " + getPackageName());
			if (msg.replyTo == null)
				return;
			try {
				Message reply = Message.obtain(null, 2);
				Bundle b = new Bundle();
				b.putString("text", "echo: " + text);
				b.putString("package", getPackageName());
				reply.setData(b);
				msg.replyTo.send(reply);
			} catch (Exception e) {
				Log.e(MainActivity.TAG, "service reply failed", e);
			}
		}
	});

	@Override
	public IBinder onBind(Intent intent) {
		Log.i(MainActivity.TAG, "service onBind in " + getPackageName());
		return messenger.getBinder();
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		Log.i(MainActivity.TAG, "service onStartCommand in " + getPackageName() + " action=" + (intent == null ? null : intent.getAction()));
		return START_NOT_STICKY;
	}
}
