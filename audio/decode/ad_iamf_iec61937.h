/*
 * Options of the IAMF IEC 61937 carrier (audio/decode/ad_iamf_iec61937.c).
 */

#ifndef MP_AD_IAMF_IEC61937_H
#define MP_AD_IAMF_IEC61937_H

#include "options/m_option.h"

struct ad_iamf_iec61937_params {
    int channels;           // carrier width
    int copy_interval_ms;   // redundant descriptor copies, 0: none
};

extern const struct m_sub_options ad_iamf_iec61937_conf;

struct mp_aframe;

/* A frame is about to be handed to the audio output, past everything that
 * drops frames: the first IAMF burst to get there after a (re)start is made
 * the one that opens the sequence at the receiver. Any other frame is left
 * alone. Thread-safe. */
void ad_iamf_iec61937_frame_sent(struct mp_aframe *frame);

#endif
