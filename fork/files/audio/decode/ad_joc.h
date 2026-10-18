/*
 * MIT License, Copyright (c) 2026 TheM14
 *
 * This file is compiled into mpv, so the resulting player is distributed under
 * mpv's own license (LGPL-2.1-or-later, or GPL-2.0-or-later when built with
 * -Dgpl=true).  mpv's license text is in LICENSE-mpv.
 */

/* --ad-joc-* options, shared by the decoder (audio/decode/ad_joc.c) and the
 * option table (options/options.c).  These are ordinary mpv options, so
 * mpv.conf carries them: "ad-joc-binaural=yes", "ad-joc-hrtf=...". */
#pragma once

#include <stdbool.h>

#include "options/m_option.h"

struct ad_joc_params {
    bool binaural;      /* binaural (stereo) output; off uses the speaker layout */
    char *hrtf;         /* SOFA / Rosella / .jochrtf; the kind is detected */
    char *layout;       /* speaker layout name, e.g. "7.1" or "9.1.6" */
    bool int24;         /* quantise the output to the renderer's int24 rule */
    int binaural_mode;  /* JOC_BINAURAL_NEAR(1) / FAR(2) / MID(3) */
    double gain_db;
};

extern const struct m_sub_options ad_joc_conf;
