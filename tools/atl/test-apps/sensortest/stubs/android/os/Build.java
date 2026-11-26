// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package android.os;
public class Build {
	public static final String MANUFACTURER = null, BRAND = null, MODEL = null, DEVICE = null, PRODUCT = null, BOARD = null,
		HARDWARE = null, FINGERPRINT = null, ID = null, DISPLAY = null, TAGS = null, TYPE = null, SERIAL = null,
		SOC_MANUFACTURER = null, SOC_MODEL = null;
	public static final String[] SUPPORTED_ABIS = null;
	public static String getSerial() { return null; }
	public static class VERSION {
		public static final String RELEASE = null, SECURITY_PATCH = null, INCREMENTAL = null;
		public static final int SDK_INT = Integer.parseInt("0"); /* not a constant: no inlining */
	}
}
