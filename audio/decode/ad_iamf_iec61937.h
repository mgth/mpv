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

#endif
