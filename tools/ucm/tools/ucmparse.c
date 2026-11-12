// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
// Build: cc -o ucmparse ucmparse.c -lasound. Run on a copy of the UCM tree with
// ${CardId} replaced: ALSA_CONFIG_UCM2=<dir with ucm.conf + Titan2/> ./ucmparse Titan2
/* offline UCM parse check: opens the config by name (no card), lists verbs,
 * devices, modifiers and the Value entries that do not need a card */
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/use-case.h>

static const char *vals[] = { "PlaybackPCM", "CapturePCM", "PlaybackPriority",
	"CapturePriority", "PlaybackMixerElem", "CaptureMixerElem",
	"PlaybackVolume", "CaptureVolume", "TQ", NULL };

int main(int argc, char **argv)
{
	snd_use_case_mgr_t *m;
	const char **verbs, **devs, **mods;
	int nv, nd, nm, rc = 0;

	if (argc < 2) return 2;
	int err = snd_use_case_mgr_open(&m, argv[1]);
	if (err < 0) { fprintf(stderr, "open %s: %s\n", argv[1], snd_strerror(err)); return 1; }
	nv = snd_use_case_get_list(m, "_verbs", &verbs);
	if (nv < 0) { fprintf(stderr, "_verbs: %s\n", snd_strerror(nv)); return 1; }
	for (int i = 0; i < nv; i += 2) {
		char id[256];
		printf("verb '%s' (%s)\n", verbs[i], verbs[i + 1]);
		snprintf(id, sizeof id, "_devices/%s", verbs[i]);
		nd = snd_use_case_get_list(m, id, &devs);
		if (nd < 0) { fprintf(stderr, "  %s: %s\n", id, snd_strerror(nd)); rc = 1; continue; }
		for (int j = 0; j < nd; j += 2) {
			printf("  device '%s' (%s)\n", devs[j], devs[j + 1]);
			for (int k = 0; vals[k]; k++) {
				const char *v;
				snprintf(id, sizeof id, "%s/%s/%s", vals[k], devs[j], verbs[i]);
				if (snd_use_case_get(m, id, &v) == 0) {
					printf("    %s = %s\n", vals[k], v);
					free((void *)v);
				}
			}
		}
		snd_use_case_free_list(devs, nd);
		snprintf(id, sizeof id, "_modifiers/%s", verbs[i]);
		nm = snd_use_case_get_list(m, id, &mods);
		if (nm > 0) { printf("  %d modifiers\n", nm / 2); snd_use_case_free_list(mods, nm); }
		{
			const char *v;
			snprintf(id, sizeof id, "TQ//%s", verbs[i]);
			if (snd_use_case_get(m, id, &v) == 0) { printf("  TQ = %s\n", v); free((void *)v); }
		}
	}
	snd_use_case_free_list(verbs, nv);
	snd_use_case_mgr_close(m);
	return rc;
}
