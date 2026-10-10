/*
 * --ad-orender-* option group, shared between the decoder
 * (audio/decode/ad_orender.c) and the liborender runtime loader
 * (common/orender_dl.c), which needs the library path option before any
 * decoder exists.
 *
 * This file is part of mpv. mpv is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#ifndef MP_AD_ORENDER_H
#define MP_AD_ORENDER_H

#include <stdbool.h>

#include "options/m_option.h"

/* All paths/hosts default to empty → passed to liborender as NULL, which then
 * resolves the shared omniphony config (~/.config/omniphony/config.yaml, the
 * same one the CLI + studio use) for the bridge paths, speaker layout, and OSC
 * settings. These options only override the config per mpv invocation. */
struct ad_orender_params {
    char *library_path;         // explicit liborender path (else search order)
    char *config_path;          // override render config YAML (else shared default)
    char *bridge_path;          // override render.bridge_paths (one path or a path list)
    bool osc;                   // force OSC on (else follows config render.osc)
    int osc_port;               // outgoing/monitoring port (0 = config/default)
    int osc_rx_port;            // incoming control port  (0 = config/default 9000)
    char *osc_bind;             // listener bind address (else config/default)
    char *osc_monitor_target;   // monitoring host (else config/default)
    int channel_mode_idx;       // initial render override: 0=auto 1=host 2=spatial
    int host_decoder_idx;       // host-mode native decoder: 0=lavc 1=spdif
};

extern const struct m_sub_options ad_orender_conf;

/* What the listener is hearing: the pts of the audio the AO is playing now
 * (playing_audio_pts()), or MP_NOPTS_VALUE when nothing is. Set by the player
 * each time it feeds the AO; the decoder turns it into the engine's own
 * timeline and reports it (orender_set_option "heard_us", ABI 0.12), so
 * Omniphony Studio can show each block of the scene when it is heard rather
 * than when it was rendered, a whole audio buffer earlier. Thread-safe. */
void ad_orender_set_playing_pts(double pts);

#endif
