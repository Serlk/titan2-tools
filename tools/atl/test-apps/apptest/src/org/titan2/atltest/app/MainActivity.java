// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package org.titan2.atltest.app;

import android.app.Activity;
import android.content.ComponentName;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;

/**
 * One activity, several scripted checks chosen with the string extra "mode" (-e mode=...).
 * Every result is one log line "ATLTEST <mode> <key>=<value>", so a test run is compared by
 * grepping the log; the last line of a mode is "ATLTEST <mode> done".
 *
 *   info       what the package sees of itself and of the other installed packages
 *   persist    SharedPreferences, SQLite and a file: each run increments and reports them
 *   edit       a multi-line EditText with a TextWatcher; scripted changes (script=1), then free
 *              typing; leaving saves the text to files/note.txt, the next start loads it.
 *              wrap=1 puts the field, as high as its text, into a ScrollView; wrap=2 into a
 *              HorizontalScrollView inside a ScrollView, both filling the viewport. Every hardware key
 *              is reported with its key code, character and input device.
 *   perm       checkSelfPermission, requestPermissions and the result callback
 *   component  getComponentEnabledSetting / setComponentEnabledSetting on the launcher alias
 *   tls        TLS connections with SSLSessionContext.setSessionTimeout() after every handshake,
 *              as an application that tunes the session cache per connection does: first one
 *              after the other (the cache then holds sessions of closed connections), then
 *              from several threads at once. Hosts from the extra "hosts" (comma separated).
 *   tlsget     a large HTTPS body read in a loop over several connections at once, as a speed
 *              test does: extras host, path, streams (default 4), mb (megabytes to read per
 *              stream, default 20). One line per stream with the bytes read and the error, if any.
 *              via=url reads through HttpsURLConnection instead of a bare TLS socket;
 *              ua=... sets the User-Agent header;
 *              rounds=N repeats the set (later rounds resume cached sessions).
 *   tlscut     a TLS connection through a relay inside this process that drops the connection
 *              after "cut" bytes from the server (default 20000) without a TLS close: what the
 *              reader sees then (extras host, path; rst=1 resets instead of closing)
 *   trust      the system's trusted roots as an application finds them: the hashed certificate
 *              directory, the AndroidCAStore key store, the default trust manager, the
 *              environment hints for native TLS libraries
 *   open       activities of another package (extra "peer"), each in that package's own process:
 *              its launcher activity, its main activity by name with an extra (the peer then
 *              runs mode=info and says who started it), its private activity (must be refused),
 *              an activity of a package that is not installed (must be refused), and a start
 *              for a result
 *   peer       the other test package (extra "peer"): signature relation, its exported and its
 *              private provider and service
 */
public class MainActivity extends Activity {
	static final String TAG = "ATLTEST";
	private String mode = "info";
	private EditText edit;
	private TextView status;
	private final Handler handler = new Handler(Looper.getMainLooper());

	void out(String key, Object value) {
		final String line = mode + " " + key + "=" + value;
		Log.i(TAG, line);
		if (status == null)
			return;
		// the screen is only touched from the main thread
		if (Looper.myLooper() == Looper.getMainLooper()) {
			status.setText(status.getText() + "\n" + line);
		} else {
			handler.post(new Runnable() {
				@Override
				public void run() {
					status.setText(status.getText() + "\n" + line);
				}
			});
		}
	}

	void done() {
		Log.i(TAG, mode + " done");
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		String m = getIntent().getStringExtra("mode");
		if (m == null && getIntent().getData() != null)
			m = getIntent().getData().getHost();
		if (m != null)
			mode = m;
		LinearLayout root = new LinearLayout(this);
		root.setOrientation(LinearLayout.VERTICAL);
		status = new TextView(this);
		status.setText(getPackageName() + " mode=" + mode);
		root.addView(status, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
		if (mode.equals("edit")) {
			edit = new EditText(this);
			if ("2".equals(getIntent().getStringExtra("wrap"))) {
				// a field of wrap_content width in a horizontal scroller inside a vertical one, both
				// filling the viewport: the layout of a text editor without line wrapping
				android.widget.ScrollView scroll = new android.widget.ScrollView(this);
				scroll.setFillViewport(true);
				android.widget.HorizontalScrollView hscroll = new android.widget.HorizontalScrollView(this);
				hscroll.setFillViewport(true);
				hscroll.addView(edit, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT));
				scroll.addView(hscroll, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
				root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
			} else if ("1".equals(getIntent().getStringExtra("wrap"))) {
				// as a text editor lays it out: a field as high as its text inside a scroller
				android.widget.ScrollView scroll = new android.widget.ScrollView(this);
				scroll.addView(edit, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
				root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
			} else {
				root.addView(edit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 600));
			}
			edit.setOnKeyListener(new android.view.View.OnKeyListener() {
				@Override
				public boolean onKey(android.view.View v, int keyCode, android.view.KeyEvent event) {
					android.view.InputDevice d = event.getDevice();
					out("key", "action:" + event.getAction() + ",code:" + keyCode + ",char:" + event.getUnicodeChar() + ",device:" + event.getDeviceId()
					               + ",kbdType:" + (d == null ? "null" : String.valueOf(d.getKeyboardType())) + ",kcm:" + event.getKeyCharacterMap().getKeyboardType());
					return false;
				}
			});
		}
		setContentView(root);
		try {
			switch (mode) {
				case "persist":
					persist();
					break;
				case "edit":
					edit();
					break;
				case "perm":
					perm();
					break;
				case "component":
					component();
					break;
				case "tls":
					tls();
					break;
				case "tlsget":
					tlsGet();
					break;
				case "tlscut":
					tlsCut();
					break;
				case "trust":
					trust();
					break;
				case "open":
					open(getIntent().getStringExtra("peer"));
					break;
				case "peer":
					peer(getIntent().getStringExtra("peer"));
					break;
				default:
					info();
			}
		} catch (Throwable t) {
			Log.e(TAG, mode + " failed", t);
			out("exception", t);
			done();
		}
	}

	static String sha256(byte[] data) throws Exception {
		StringBuilder sb = new StringBuilder();
		for (byte b : MessageDigest.getInstance("SHA-256").digest(data))
			sb.append(String.format("%02x", b & 0xff));
		return sb.toString();
	}

	private void info() throws Exception {
		PackageManager pm = getPackageManager();
		PackageInfo pi = pm.getPackageInfo(getPackageName(), PackageManager.GET_SIGNATURES);
		out("package", getPackageName());
		out("startedBy", getIntent().getStringExtra("from"));
		out("processName", getApplicationInfo().processName);
		Intent battery = registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
		out("battery", battery == null ? "null" : "level:" + battery.getIntExtra("level", -1) + "/" + battery.getIntExtra("scale", -1) + ",status:" + battery.getIntExtra("status", -1) + ",plugged:" + battery.getIntExtra("plugged", -1));
		out("versionCode", pi.versionCode);
		out("sdkInt", Build.VERSION.SDK_INT);
		out("release", Build.VERSION.RELEASE);
		out("model", Build.MODEL);
		out("uid", getApplicationInfo().uid);
		out("dataDir", getApplicationInfo().dataDir);
		out("filesDir", getFilesDir());
		out("sourceDir", getApplicationInfo().sourceDir);
		out("nativeLibraryDir", getApplicationInfo().nativeLibraryDir);
		out("firstInstallTime", pi.firstInstallTime > 0);
		out("installer", pm.getInstallerPackageName(getPackageName()));
		for (Signature s : pi.signatures)
			out("signer", sha256(s.toByteArray()));
		StringBuilder names = new StringBuilder();
		for (PackageInfo p : pm.getInstalledPackages(0))
			names.append(p.packageName).append(':').append(p.versionCode).append(' ');
		out("installed", names.toString().trim());
		out("features", pm.getSystemAvailableFeatures().length);
		done();
	}

	private void persist() throws Exception {
		SharedPreferences prefs = getSharedPreferences("test", MODE_PRIVATE);
		int runs = prefs.getInt("runs", 0) + 1;
		prefs.edit().putInt("runs", runs).putString("lastVersion", String.valueOf(getPackageManager().getPackageInfo(getPackageName(), 0).versionCode)).commit();
		out("prefs.runs", runs);

		SQLiteDatabase db = openOrCreateDatabase("test.db", MODE_PRIVATE, null);
		// schema version 1 has (n), version 2 adds (version): an update of the package migrates the table
		int schema = db.getVersion();
		int wanted = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode >= 2 ? 2 : 1;
		if (schema == 0)
			db.execSQL("CREATE TABLE runs (n INTEGER)");
		if (schema < 2 && wanted == 2)
			db.execSQL("ALTER TABLE runs ADD COLUMN version INTEGER DEFAULT 1");
		if (schema < wanted)
			db.setVersion(wanted);
		ContentValues v = new ContentValues();
		v.put("n", runs);
		if (wanted == 2)
			v.put("version", 2);
		db.insert("runs", null, v);
		Cursor c = db.rawQuery("SELECT COUNT(*) FROM runs", null);
		c.moveToFirst();
		out("db.rows", c.getInt(0));
		c.close();
		out("db.schema", db.getVersion());
		db.close();

		File f = new File(getFilesDir(), "runs.txt");
		try (FileOutputStream o = new FileOutputStream(f, true)) {
			o.write(("run " + runs + "\n").getBytes("UTF-8"));
		}
		out("file.bytes", f.length());
		done();
	}

	private String readNote() {
		File f = new File(getFilesDir(), "note.txt");
		if (!f.isFile())
			return null;
		try (FileInputStream in = new FileInputStream(f)) {
			byte[] buf = new byte[(int)f.length()];
			int n = in.read(buf);
			return new String(buf, 0, Math.max(n, 0), "UTF-8");
		} catch (Exception e) {
			return null;
		}
	}

	private static String show(CharSequence s) {
		return "[" + s.toString().replace("\n", "\\n") + "]";
	}

	private void edit() {
		String saved = readNote();
		out("loaded", saved == null ? "none" : show(saved));
		edit.addTextChangedListener(new TextWatcher() {
			@Override
			public void beforeTextChanged(CharSequence s, int start, int count, int after) {
				out("before", "start:" + start + ",count:" + count + ",after:" + after);
			}
			@Override
			public void onTextChanged(CharSequence s, int start, int before, int count) {
				out("changed", "start:" + start + ",before:" + before + ",count:" + count + ",text:" + show(s));
			}
			@Override
			public void afterTextChanged(Editable s) {
				out("after", show(s) + ",sel:" + edit.getSelectionStart() + "-" + edit.getSelectionEnd());
			}
		});
		if ("1".equals(getIntent().getStringExtra("script"))) {
			// the scripted part: every step changes the text through the Editable the app got from getText()
			edit.setText("\u0410\u0411"); /* Cyrillic A, Be */
			out("step1.text", show(edit.getText()));
			edit.setSelection(1);
			out("step2.sel", edit.getSelectionStart() + "-" + edit.getSelectionEnd());
			Editable e = edit.getText();
			e.insert(1, "\u044f"); /* Cyrillic ya */
			out("step3.text", show(edit.getText()) + ",same:" + (e == edit.getText()));
			e.append("\n\u0432\u0442\u043e\u0440\u0430\u044f"); /* "second" in Russian */
			out("step4.text", show(edit.getText()) + ",lines:" + edit.getText().toString().split("\n").length);
			edit.setSelection(0, 1);
			e.replace(edit.getSelectionStart(), edit.getSelectionEnd(), "Z");
			out("step5.text", show(edit.getText()));
		} else if (saved != null) {
			edit.setText(saved);
			edit.setSelection(edit.length());
		}
		edit.requestFocus();
		handler.postDelayed(new Runnable() {
			@Override
			public void run() {
				out("focused", edit.isFocused());
				out("size", edit.getWidth() + "x" + edit.getHeight() + ",measured:" + edit.getMeasuredWidth() + "x" + edit.getMeasuredHeight());
				done();
			}
		}, 1500);
	}

	@Override
	protected void onPause() {
		super.onPause();
		if (edit != null) {
			try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "note.txt"))) {
				o.write(edit.getText().toString().getBytes("UTF-8"));
				out("saved", show(edit.getText()));
			} catch (Exception e) {
				out("save.exception", e);
			}
		}
	}

	private void tlsGet() {
		final String host = getIntent().getStringExtra("host") != null ? getIntent().getStringExtra("host") : "nbg1-speed.hetzner.com";
		final String path = getIntent().getStringExtra("path") != null ? getIntent().getStringExtra("path") : "/100MB.bin";
		String n = getIntent().getStringExtra("streams");
		String m = getIntent().getStringExtra("mb");
		final int streams = n != null ? Integer.parseInt(n) : 4;
		final long limit = (m != null ? Long.parseLong(m) : 20) * 1024 * 1024;
		final javax.net.ssl.SSLSocketFactory sockets;
		try {
			javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
			context.init(null, null, null);
			sockets = context.getSocketFactory();
		} catch (Exception e) {
			out("context", e);
			done();
			return;
		}
		final int[] failed = {0};
		final long[] total = {0};
		String rs = getIntent().getStringExtra("rounds");
		final int rounds = rs != null ? Integer.parseInt(rs) : 1;
		final boolean viaUrl = "url".equals(getIntent().getStringExtra("via"));
		final String agent = getIntent().getStringExtra("ua") != null ? getIntent().getStringExtra("ua") : "atltest";
		final Thread[] workers = new Thread[streams * rounds];
		for (int i = 0; i < streams * rounds; i++) {
			final int id = i;
			workers[i] = new Thread("tlsget-" + i) {
				@Override
				public void run() {
					long got = 0;
					long started = android.os.SystemClock.elapsedRealtime();
					String status = "?";
					javax.net.ssl.SSLSocket socket = null;
					if (viaUrl) {
						try {
							javax.net.ssl.HttpsURLConnection c = (javax.net.ssl.HttpsURLConnection)new java.net.URL("https://" + host + path).openConnection();
							c.setSSLSocketFactory(sockets);
							c.setConnectTimeout(15000);
							c.setReadTimeout(30000);
							c.setRequestProperty("User-Agent", agent);
							status = "HTTP " + c.getResponseCode();
							java.io.InputStream in = c.getResponseCode() >= 400 ? c.getErrorStream() : c.getInputStream();
							byte[] buf = new byte[16384];
							int r = 0;
							while (in != null && got < limit && (r = in.read(buf)) > 0)
								got += r;
							c.disconnect();
							long ms = Math.max(1, android.os.SystemClock.elapsedRealtime() - started);
							out("stream", id + " ok, " + status + ", " + got + " bytes in " + ms + " ms (" + (got * 8 / ms) + " kbit/s)" + (r <= 0 ? ", end of stream" : ""));
						} catch (Throwable t) {
							synchronized (failed) {
								failed[0]++;
							}
							out("stream", id + " FAILED after " + got + " bytes, " + (android.os.SystemClock.elapsedRealtime() - started) + " ms, " + status + ": " + t);
						}
						synchronized (total) {
							total[0] += got;
						}
						return;
					}
					try {
						// as an HTTP client does: a plain socket first, TLS layered over it
						java.net.Socket plain = new java.net.Socket();
						plain.connect(new java.net.InetSocketAddress(host, 443), 15000);
						socket = (javax.net.ssl.SSLSocket)sockets.createSocket(plain, host, 443, true);
						socket.setSoTimeout(30000);
						socket.startHandshake();
						java.io.OutputStream o = socket.getOutputStream();
						o.write(("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: " + agent + "\r\nAccept: */*\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
						o.flush();
						java.io.InputStream in = socket.getInputStream();
						byte[] buf = new byte[16384];
						int r = in.read(buf);
						if (r > 0) {
							status = new String(buf, 0, Math.min(r, 40), "US-ASCII").split("\r")[0];
							got = r;
						}
						while (got < limit && (r = in.read(buf)) > 0)
							got += r;
						long ms = Math.max(1, android.os.SystemClock.elapsedRealtime() - started);
						out("stream", id + " ok, " + status + ", " + got + " bytes in " + ms + " ms (" + (got * 8 / ms) + " kbit/s)" + (r <= 0 ? ", end of stream" : ""));
					} catch (Throwable t) {
						synchronized (failed) {
							failed[0]++;
						}
						long ms = android.os.SystemClock.elapsedRealtime() - started;
						out("stream", id + " FAILED after " + got + " bytes, " + ms + " ms, " + status + ": " + t);
					} finally {
						synchronized (total) {
							total[0] += got;
						}
						try {
							if (socket != null)
								socket.close();
						} catch (Exception e) {
							// closing
						}
					}
				}
			};
		}
		new Thread("tlsget") {
			@Override
			public void run() {
				for (int round = 0; round < rounds; round++) {
					for (int i = 0; i < streams; i++)
						workers[round * streams + i].start();
					for (int i = 0; i < streams; i++) {
						try {
							workers[round * streams + i].join();
						} catch (InterruptedException e) {
							// keep waiting
						}
					}
				}
				out("total", "streams:" + streams + ",rounds:" + rounds + ",failed:" + failed[0] + ",bytes:" + total[0]);
				done();
			}
		}.start();
	}

	private void tlsCut() {
		final String host = getIntent().getStringExtra("host") != null ? getIntent().getStringExtra("host") : "par.download.datapacket.com";
		final String path = getIntent().getStringExtra("path") != null ? getIntent().getStringExtra("path") : "/100mb.bin";
		String c = getIntent().getStringExtra("cut");
		final long cut = c != null ? Long.parseLong(c) : 20000;
		final boolean reset = "1".equals(getIntent().getStringExtra("rst"));
		new Thread("tlscut") {
			@Override
			public void run() {
				long got = 0;
				int reads = 0;
				try {
					final java.net.ServerSocket listener = new java.net.ServerSocket(0, 1, java.net.InetAddress.getByName("127.0.0.1"));
					// the relay: bytes both ways, then the client side is dropped in mid-stream
					new Thread("tlscut-relay") {
						@Override
						public void run() {
							try {
								final java.net.Socket client = listener.accept();
								final java.net.Socket server = new java.net.Socket(host, 443);
								// the upward half polls, so that it is never inside read() when the
								// socket is closed under it
								client.setSoTimeout(100);
								final boolean[] stop = {false};
								Thread up = new Thread("tlscut-up") {
									@Override
									public void run() {
										byte[] b = new byte[8192];
										while (!stop[0]) {
											try {
												int r = client.getInputStream().read(b);
												if (r <= 0)
													break;
												server.getOutputStream().write(b, 0, r);
												server.getOutputStream().flush();
											} catch (java.net.SocketTimeoutException e) {
												// look at the flag again
											} catch (Exception e) {
												break;
											}
										}
									}
								};
								up.start();
								byte[] b = new byte[4096];
								long passed = 0;
								int r;
								while (passed < cut && (r = server.getInputStream().read(b, 0, (int)Math.min(b.length, cut - passed))) > 0) {
									client.getOutputStream().write(b, 0, r);
									client.getOutputStream().flush();
									passed += r;
								}
								stop[0] = true;
								up.join();
								if (reset)
									client.setSoLinger(true, 0);
								client.close();
								server.close();
								listener.close();
							} catch (Exception e) {
								out("relay", e);
							}
						}
					}.start();
					javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
					context.init(null, null, null);
					java.net.Socket plain = new java.net.Socket("127.0.0.1", listener.getLocalPort());
					javax.net.ssl.SSLSocket socket = (javax.net.ssl.SSLSocket)context.getSocketFactory().createSocket(plain, host, 443, true);
					socket.setSoTimeout(20000);
					socket.startHandshake();
					socket.getOutputStream().write(("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
					socket.getOutputStream().flush();
					java.io.InputStream in = socket.getInputStream();
					byte[] buf = new byte[16384];
					String seen = "";
					// what a reader meets after the cut: the results of the next reads, in order
					for (int after = 0; after < 3;) {
						int r = in.read(buf);
						reads++;
						if (r > 0) {
							got += r;
							continue;
						}
						seen += (after == 0 ? "" : ",") + r;
						after++;
					}
					out("cut", (reset ? "reset" : "closed") + " after " + cut + " relayed bytes: " + got + " bytes of body read, then read() gave " + seen + ", single-byte read() gave " + in.read());
				} catch (Throwable t) {
					out("cut", (reset ? "reset" : "closed") + " after " + cut + " relayed bytes: " + got + " bytes read in " + reads + " reads, then FAILED: " + t);
				}
				done();
			}
		}.start();
	}

	private void trust() {
		for (String dir : new String[] {"/system/etc/security/cacerts", "/apex/com.android.conscrypt/cacerts"}) {
			java.io.File[] files = new java.io.File(dir).listFiles();
			int parsed = 0;
			String first = null;
			if (files != null) {
				java.util.Arrays.sort(files);
				for (java.io.File f : files) {
					try (java.io.InputStream in = new java.io.FileInputStream(f)) {
						java.security.cert.CertificateFactory.getInstance("X.509").generateCertificate(in);
						parsed++;
						if (first == null)
							first = f.getName();
					} catch (Exception e) {
						out("badFile", f + ": " + e);
					}
				}
			}
			out("dir", dir + ": " + (files == null ? "missing" : files.length + " files, " + parsed + " certificates, first " + first));
		}
		try {
			java.security.KeyStore store = java.security.KeyStore.getInstance("AndroidCAStore");
			store.load(null, null);
			String alias = store.aliases().hasMoreElements() ? store.aliases().nextElement() : null;
			java.security.cert.X509Certificate cert = alias == null ? null : (java.security.cert.X509Certificate)store.getCertificate(alias);
			String name = alias == null ? null : alias.substring(alias.indexOf(':') + 1);
			out("caStore", store.size() + " entries, first " + alias
			    + (cert == null ? "" : " (" + cert.getSubjectX500Principal().getName() + "), same name in the directory: " + new java.io.File("/system/etc/security/cacerts", name).isFile()
			    + ", certificate entry: " + store.isCertificateEntry(alias)));
		} catch (Throwable t) {
			out("caStore", "FAILED: " + t);
		}
		try {
			javax.net.ssl.TrustManagerFactory f = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
			f.init((java.security.KeyStore)null);
			int issuers = 0;
			for (javax.net.ssl.TrustManager m : f.getTrustManagers()) {
				if (m instanceof javax.net.ssl.X509TrustManager)
					issuers += ((javax.net.ssl.X509TrustManager)m).getAcceptedIssuers().length;
			}
			out("trustManager", f.getAlgorithm() + ", " + issuers + " accepted issuers");
		} catch (Throwable t) {
			out("trustManager", "FAILED: " + t);
		}
		out("env", "SSL_CERT_FILE=" + System.getenv("SSL_CERT_FILE") + " SSL_CERT_DIR=" + System.getenv("SSL_CERT_DIR"));
		done();
	}

	private void tryStart(String what, Intent intent, int requestCode) {
		try {
			if (requestCode >= 0)
				startActivityForResult(intent, requestCode);
			else
				startActivity(intent);
			out(what, "started");
		} catch (android.content.ActivityNotFoundException e) {
			out(what, "refused:ActivityNotFoundException");
		} catch (SecurityException e) {
			out(what, "refused:SecurityException");
		} catch (RuntimeException e) {
			out(what, "failed:" + e);
		}
	}

	private void open(final String peer) {
		Intent launch = getPackageManager().getLaunchIntentForPackage(peer);
		out("launchIntent", launch == null ? "null" : launch.getComponent().flattenToShortString());
		tryStart("missing", new Intent().setComponent(new ComponentName("org.titan2.atltest.nothing", "org.titan2.atltest.app.MainActivity")), -1);
		tryStart("private", new Intent().setComponent(new ComponentName(peer, "org.titan2.atltest.app.PrivateActivity")), -1);
		String step = getIntent().getStringExtra("step");
		if ("explicit".equals(step)) {
			tryStart("explicit", new Intent().setComponent(new ComponentName(peer, "org.titan2.atltest.app.MainActivity")).putExtra("mode", "info").putExtra("from", getPackageName()), -1);
		} else if ("result".equals(step)) {
			tryStart("forResult", new Intent().setComponent(new ComponentName(peer, "org.titan2.atltest.app.MainActivity")).putExtra("mode", "info").putExtra("from", getPackageName()), 7);
		} else if (launch != null) {
			tryStart("launch", launch, -1);
		}
		handler.postDelayed(new Runnable() {
			@Override
			public void run() {
				done();
			}
		}, 1000);
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		out("activityResult", "request:" + requestCode + ",result:" + resultCode);
	}

	private int tlsOk, tlsFailed;
	/** one context for all connections, as an HTTP client has: its session cache is what the test is about */
	private javax.net.ssl.SSLSocketFactory tlsSockets;

	private void tlsOnce(String host, int round) {
		javax.net.ssl.SSLSocket socket = null;
		try {
			socket = (javax.net.ssl.SSLSocket)tlsSockets.createSocket(host, 443);
			socket.setSoTimeout(20000);
			socket.startHandshake();
			javax.net.ssl.SSLSession session = socket.getSession();
			// walks the whole session cache of the context
			session.getSessionContext().setSessionTimeout(300 + round);
			java.util.Enumeration<byte[]> ids = session.getSessionContext().getIds();
			int cached = 0;
			while (ids.hasMoreElements()) {
				ids.nextElement();
				cached++;
			}
			java.io.OutputStream o = socket.getOutputStream();
			o.write(("HEAD / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
			o.flush();
			byte[] buf = new byte[64];
			int n = socket.getInputStream().read(buf);
			String status = n > 0 ? new String(buf, 0, n, "US-ASCII").split("\r")[0] : "no answer";
			synchronized (this) {
				tlsOk++;
			}
			out("conn", host + " round " + round + " ok, " + session.getProtocol() + ", cached sessions " + cached + ", " + status);
		} catch (Throwable t) {
			synchronized (this) {
				tlsFailed++;
			}
			StackTraceElement[] st = t.getStackTrace();
			out("conn", host + " round " + round + " FAILED " + t + (st.length > 0 ? " at " + st[0] : ""));
		} finally {
			try {
				if (socket != null)
					socket.close();
			} catch (Exception e) {
				// closing
			}
		}
	}

	private void tls() {
		String h = getIntent().getStringExtra("hosts");
		final String[] hosts = (h != null ? h : "f-droid.org,gitlab.com").split(",");
		try {
			javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
			context.init(null, null, null);
			tlsSockets = context.getSocketFactory();
			out("provider", context.getProvider().getName());
		} catch (Exception e) {
			out("context", e);
			done();
			return;
		}
		new Thread("tls-test") {
			@Override
			public void run() {
				// one after the other: from the second connection on the cache holds closed sessions
				for (int round = 0; round < 3; round++) {
					for (String host : hosts)
						tlsOnce(host, round);
				}
				out("sequential", "ok:" + tlsOk + ",failed:" + tlsFailed);
				// several at once: the cache is read and changed from all of them
				final int before = tlsOk + tlsFailed;
				Thread[] workers = new Thread[6];
				for (int i = 0; i < workers.length; i++) {
					final int n = i;
					workers[i] = new Thread("tls-worker-" + i) {
						@Override
						public void run() {
							for (int round = 0; round < 3; round++)
								tlsOnce(hosts[(n + round) % hosts.length], 10 + round);
						}
					};
					workers[i].start();
				}
				for (Thread w : workers) {
					try {
						w.join();
					} catch (InterruptedException e) {
						// keep waiting for the rest
					}
				}
				out("parallel", "connections:" + (tlsOk + tlsFailed - before));
				out("total", "ok:" + tlsOk + ",failed:" + tlsFailed);
				done();
			}
		}.start();
	}

	private static final String[] PERMS = {"android.permission.CAMERA", "android.permission.ACCESS_FINE_LOCATION", "android.permission.INTERNET", "android.permission.READ_CONTACTS"};

	private void perm() {
		for (String p : PERMS)
			out("check." + p.substring(p.lastIndexOf('.') + 1), checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED ? "granted" : "denied");
		out("rationale.CAMERA", shouldShowRequestPermissionRationale("android.permission.CAMERA"));
		if ("1".equals(getIntent().getStringExtra("request")))
			requestPermissions(PERMS, 42);
		else
			done();
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
		out("result.code", requestCode);
		for (int i = 0; i < permissions.length; i++)
			out("result." + permissions[i].substring(permissions[i].lastIndexOf('.') + 1), grantResults[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "denied");
		for (String p : PERMS)
			out("after." + p.substring(p.lastIndexOf('.') + 1), checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED ? "granted" : "denied");
		done();
	}

	private void component() {
		PackageManager pm = getPackageManager();
		ComponentName main = new ComponentName(getPackageName(), "org.titan2.atltest.app.MainActivity");
		ComponentName alt = new ComponentName(getPackageName(), "org.titan2.atltest.app.AltLauncher");
		out("main.before", pm.getComponentEnabledSetting(main));
		out("alt.before", pm.getComponentEnabledSetting(alt));
		String set = getIntent().getStringExtra("set");
		if ("alt".equals(set)) {
			pm.setComponentEnabledSetting(alt, PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);
			pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
		} else if ("main".equals(set)) {
			pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);
			pm.setComponentEnabledSetting(alt, PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
		} else if ("default".equals(set)) {
			pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, PackageManager.DONT_KILL_APP);
			pm.setComponentEnabledSetting(alt, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, PackageManager.DONT_KILL_APP);
		}
		out("main.after", pm.getComponentEnabledSetting(main));
		out("alt.after", pm.getComponentEnabledSetting(alt));
		Intent launch = pm.getLaunchIntentForPackage(getPackageName());
		out("launch", launch == null ? "none" : launch.getComponent().getClassName());
		try {
			pm.setComponentEnabledSetting(new ComponentName("org.titan2.atltest.other", "org.titan2.atltest.app.MainActivity"), PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0);
			out("foreign", "changed");
		} catch (SecurityException | IllegalArgumentException e) {
			out("foreign", "refused:" + e.getClass().getSimpleName());
		}
		done();
	}

	private void peer(final String peer) throws Exception {
		PackageManager pm = getPackageManager();
		try {
			PackageInfo pi = pm.getPackageInfo(peer, PackageManager.GET_SIGNATURES);
			out("peer.version", pi.versionCode);
			out("peer.uid", pi.applicationInfo.uid + ",own:" + getApplicationInfo().uid);
			out("peer.signer", sha256(pi.signatures[0].toByteArray()));
		} catch (PackageManager.NameNotFoundException e) {
			out("peer.version", "not installed");
		}
		out("signatures", pm.checkSignatures(getPackageName(), peer));
		out("perm.SAME_SIGNER", pm.checkPermission("org.titan2.atltest.permission.SAME_SIGNER", peer) == PackageManager.PERMISSION_GRANTED ? "granted" : "denied");
		for (String which : new String[] {"notes", "private"}) {
			Uri uri = Uri.parse("content://" + peer + "." + which + "/item");
			try {
				Cursor c = getContentResolver().query(uri, null, null, null, null);
				if (c == null) {
					out("provider." + which, "null");
				} else {
					c.moveToFirst();
					out("provider." + which, "row:" + c.getString(0));
					c.close();
				}
			} catch (SecurityException e) {
				out("provider." + which, "refused:SecurityException");
			} catch (RuntimeException e) {
				out("provider." + which, "failed:" + e);
			}
		}
		File foreign = new File(getApplicationInfo().dataDir).getParentFile();
		out("ownDataDir", getApplicationInfo().dataDir);
		Intent echo = new Intent("org.titan2.atltest.ECHO").setPackage(peer);
		final Messenger replies = new Messenger(new Handler(Looper.getMainLooper()) {
			@Override
			public void handleMessage(Message msg) {
				out("service.reply", msg.getData().getString("text") + ",from:" + msg.getData().getString("package"));
				done();
			}
		});
		boolean bound = false;
		try {
			bound = bindService(echo, new ServiceConnection() {
				@Override
				public void onServiceConnected(ComponentName name, IBinder service) {
					out("service.connected", name == null ? "null" : name.getClassName());
					try {
						Message m = Message.obtain(null, 1);
						Bundle b = new Bundle();
						b.putString("text", "ping from " + getPackageName());
						m.setData(b);
						m.replyTo = replies;
						new Messenger(service).send(m);
					} catch (Exception e) {
						out("service.send", e);
						done();
					}
				}
				@Override
				public void onServiceDisconnected(ComponentName name) {
					out("service.disconnected", name == null ? "null" : name.getClassName());
				}
			}, Context.BIND_AUTO_CREATE);
		} catch (SecurityException e) {
			out("service.bind", "refused:SecurityException");
		}
		out("service.bound", bound);
		try {
			boolean priv = bindService(new Intent().setComponent(new ComponentName(peer, "org.titan2.atltest.app.PrivateService")), new ServiceConnection() {
				@Override
				public void onServiceConnected(ComponentName name, IBinder service) {
					out("private.connected", "yes");
				}
				@Override
				public void onServiceDisconnected(ComponentName name) {
				}
			}, Context.BIND_AUTO_CREATE);
			out("private.bound", priv);
		} catch (SecurityException e) {
			out("private.bound", "refused:SecurityException");
		}
		if (!bound)
			done();
	}
}
