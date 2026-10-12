/*
 * Options of the IAMF IEC 61937 carrier (audio/decode/ad_iamf_iec61937.c).
 */

#ifndef MP_AD_IAMF_IEC61937_H
#define MP_AD_IAMF_IEC61937_H

#include <stdbool.h>

#include "options/m_option.h"

struct ad_iamf_iec61937_params {
    int channels;           // carrier width
    int copy_interval_ms;   // redundant descriptor copies, 0: none
};

extern const struct m_sub_options ad_iamf_iec61937_conf;

struct mp_aframe;

/* `frame` is an IAMF burst leading with a redundant copy of the descriptors:
 * the kind the player reports to the decoder that made it, through its
 * wrapper, before handing it to the audio output (ADCTRL_IAMF_BURST_SENT in
 * filters/f_decoder_wrapper.h). The first reported after a (re)start is made
 * the burst that opens the sequence at the receiver. Any other frame need
 * not be reported. */
bool ad_iamf_iec61937_leads_with_copy(struct mp_aframe *frame);

#endif
