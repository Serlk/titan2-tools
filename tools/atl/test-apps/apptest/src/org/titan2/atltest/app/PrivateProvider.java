// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
package org.titan2.atltest.app;

/** Not exported: another package must get a refusal. */
public class PrivateProvider extends NotesProvider {
	@Override
	protected String kind() {
		return "private";
	}
}
