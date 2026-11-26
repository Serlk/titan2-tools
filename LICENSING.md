# Licensing

titan2-tools: build and test tools for mainline Linux on the Unihertz Titan 2

Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. The full text of version 2 is in [LICENSE](LICENSE). Individual files state their
license with an SPDX-License-Identifier line; see LICENSES/ for every license
used in this repository.

Exceptions: the patches in `patches/android-translation-layer/` modify the Android Translation
Layer and are GPL-3.0-only like it (text in `LICENSES/GPL-3.0-only.txt`); the patches in
`patches/art_standalone/` modify wolfSSL JNI and are GPL-2.0-or-later; the patches in
`tools/titan2-vendor-runtime/patches/bionic_translation/` are Apache-2.0 like bionic_translation;
the Mesa patches are MIT (see `REUSE.toml`).
