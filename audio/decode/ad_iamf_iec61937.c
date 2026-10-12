/*
 * ad_iamf_iec61937.c — carry an IAMF track as IEC 61937 bursts.
 *
 * Opt in with `--ad=iamf-iec61937` on an IAMF track. Nothing is decoded: each
 * temporal unit (one Matroska block) goes out as one burst of a private IEC
 * 61937 data type, on a PCM-shaped carrier of 16-bit words, for an Omniphony
 * renderer at the other end of a link that carries PCM and with it a clock
 * (Omniphony docs/iamf-transport-plan.md). No other equipment knows the type:
 * the IEC assigned IAMF none.
 *
 * A burst is Pa Pb Pc Pd, the payload, and zeros up to the repetition period,
 * which is the unit's duration in carrier frames, so the carrier's position
 * is the stream's time and mpv's A/V sync works as with any output. The
 * payload is the unit's OBUs, preceded by the sequence's descriptors: as they
 * are when the stream (re)starts, which opens a new sequence at the receiver,
 * and as a redundant copy every so often, for a receiver that joins late.
 *
 * The frames are flagged as a compressed format (AF_FORMAT_S_IAMF), so no
 * filter, volume or resampler touches the words. The carrier is
 * --ad-iamf-iec61937-channels wide at the track's own rate; a unit longer
 * than its period needs a wider one.
 *
 * The same stream is written by `orender wrap-iec61937` from a standalone
 * IAMF file (Omniphony's spdif crate), byte for byte.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "common/codecs.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/options.h"

#include "ad_iamf_iec61937.h"

#define OPT_BASE_STRUCT struct ad_iamf_iec61937_params

const struct m_sub_options ad_iamf_iec61937_conf = {
    .opts = (const m_option_t[]) {
        /* Carrier width: its byte rate is channels x rate x 2. */
        {"channels", OPT_INT(channels), M_RANGE(1, MP_NUM_CHANNELS)},
        /* How often a redundant copy of the descriptors is carried, for a
         * receiver that joins late; 0 carries none. */
        {"copy-interval", OPT_INT(copy_interval_ms), M_RANGE(0, 60000)},
        {0}
    },
    .size = sizeof(struct ad_iamf_iec61937_params),
    .defaults = &(const struct ad_iamf_iec61937_params){
        .channels = 8,
        .copy_interval_ms = 1000,
    },
};

/* IEC 61937 preamble, as little-endian 16-bit words. */
#define SYNCWORD_PA 0xF872
#define SYNCWORD_PB 0x4E1F
/* The private data type (Pc bits 0-4): spdif::DATA_TYPE_IAMF_PRIVATE. */
#define DATA_TYPE_IAMF_PRIVATE 0x1D
#define BURST_HEADER_BYTES 8
/* Pd holds the low 16 bits of the payload length, Pc bits 8-12 the next
 * five. */
#define MAX_PAYLOAD_BYTES ((1 << 21) - 1)

#define OBU_CODEC_CONFIG 0
#define OBU_REDUNDANT_COPY 0x04
#define OBU_EXTENSION 0x01

struct priv {
    struct mp_log *log;
    struct mp_codec_params *codec;
    struct mp_aframe *fmt;
    struct mp_aframe_pool *pool;

    /* The sequence's descriptor OBUs, as CodecPrivate has them, and the
     * same with obu_redundant_copy set on each. */
    uint8_t *descriptors;
    uint8_t *redundant;
    size_t descriptors_len;

    int rate;               // carrier frames a second
    int sstride;            // bytes per carrier frame
    int period_frames;      // carrier frames one unit lasts
    int copy_every;         // units between redundant copies, 0: none
    int since_copy;         // units since the descriptors last went out
    bool restart;           // the next burst opens the sequence
    /* After a seek the player drops the frames that end before its target
     * — whole, since an IEC 61937 frame is never cut — so a burst that ends
     * before it never reaches the receiver. MP_NOPTS_VALUE: none dropped. */
    double start_pts;
    uint64_t too_long;      // units that did not fit their period
    struct mp_decoder public;
};

/* A leb128 of at most 8 bytes at data[0..len): its value in *out, the bytes
 * it took as the return value, 0 when it does not end. */
static int leb128(const uint8_t *data, size_t len, uint64_t *out)
{
    uint64_t value = 0;
    for (int n = 0; n < 8 && (size_t)n < len; n++) {
        value |= (uint64_t)(data[n] & 0x7F) << (7 * n);
        if (!(data[n] & 0x80)) {
            *out = value;
            return n + 1;
        }
    }
    return 0;
}

/* The OBU at data[0..len): its whole length, 0 when it is cut short. The
 * body's offset goes to *body. */
static size_t obu_len(const uint8_t *data, size_t len, size_t *body)
{
    uint64_t size;
    int n = len ? leb128(data + 1, len - 1, &size) : 0;
    if (!n || size > len - 1 - n)
        return 0;
    *body = 1 + n;
    return 1 + n + (size_t)size;
}

/* What the first codec config states: num_samples_per_frame, and the sample
 * rate when the codec's own config says it (0 otherwise). */
static bool unit_timing(const uint8_t *desc, size_t len, uint32_t *samples,
                        uint32_t *rate)
{
    for (size_t pos = 0; pos < len;) {
        size_t body;
        size_t obu = obu_len(desc + pos, len - pos, &body);
        if (!obu)
            return false;
        if (desc[pos] >> 3 == OBU_CODEC_CONFIG) {
            const uint8_t *b = desc + pos + body;
            size_t b_len = obu - body;
            uint64_t v;
            int n;
            if (desc[pos] & OBU_EXTENSION) {
                if (!(n = leb128(b, b_len, &v)) || v > b_len - n)
                    return false;
                b += n + v;
                b_len -= n + v;
            }
            if (!(n = leb128(b, b_len, &v)) || b_len - n < 4)
                return false;
            const uint8_t *codec_id = b + n;
            b += n + 4;
            b_len -= n + 4;
            if (!(n = leb128(b, b_len, &v)) || v == 0 || v > UINT32_MAX)
                return false;
            *samples = v;
            /* audio_roll_distance, then the codec's own config. */
            b += n + 2;
            b_len = b_len >= (size_t)n + 2 ? b_len - n - 2 : 0;
            *rate = 0;
            if (memcmp(codec_id, "Opus", 4) == 0) {
                *rate = 48000;
            } else if (memcmp(codec_id, "ipcm", 4) == 0 && b_len >= 6) {
                /* sample_format_flags, sample_size, sample_rate. */
                *rate = (uint32_t)b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5];
            } else if (memcmp(codec_id, "fLaC", 4) == 0 && b_len >= 17) {
                /* A METADATA_BLOCK header, then STREAMINFO: block and frame
                 * sizes (10 bytes), then the rate on 20 bits. */
                *rate = (uint32_t)b[14] << 12 | b[15] << 4 | b[16] >> 4;
            }
            return true;
        }
        pos += obu;
    }
    return false;
}

static void put16le(uint8_t *out, unsigned v)
{
    out[0] = v & 0xFF;
    out[1] = v >> 8;
}

static void process(struct mp_filter *da)
{
    struct priv *p = da->priv;

    if (!mp_pin_can_transfer_data(da->ppins[1], da->ppins[0]))
        return;

    struct mp_frame inframe = mp_pin_out_read(da->ppins[0]);
    if (inframe.type == MP_FRAME_EOF) {
        mp_pin_in_write(da->ppins[1], inframe);
        return;
    } else if (inframe.type != MP_FRAME_PACKET) {
        if (inframe.type) {
            MP_ERR(da, "unknown frame type\n");
            mp_filter_internal_mark_failed(da);
        }
        return;
    }

    struct demux_packet *mpkt = inframe.data;
    struct mp_aframe *out = mp_aframe_new_ref(p->fmt);
    if (mp_aframe_pool_allocate(p->pool, out, p->period_frames) < 0) {
        TA_FREEP(&out);
        goto done;
    }
    uint8_t **data = mp_aframe_get_data_rw(out);
    if (!data) {
        TA_FREEP(&out);
        goto done;
    }
    uint8_t *burst = data[0];
    size_t period = (size_t)p->period_frames * p->sstride;
    memset(burst, 0, period);

    /* The descriptors as they are open a new sequence at the receiver: at
     * the start, and after a seek, where its decoder starts afresh. A copy
     * is skipped by a receiver that has the sequence. */
    const uint8_t *lead = NULL;
    if (p->restart) {
        lead = p->descriptors;
    } else if (p->copy_every && p->since_copy >= p->copy_every) {
        lead = p->redundant;
    }
    size_t lead_len = lead ? p->descriptors_len : 0;
    size_t payload = lead_len + mpkt->len;

    if (payload > MAX_PAYLOAD_BYTES || BURST_HEADER_BYTES + payload > period) {
        /* The period goes out empty: the carrier keeps its time, the
         * receiver hears a gap. */
        if (!(p->too_long & (p->too_long + 1))) {
            MP_ERR(da, "a unit of %zu bytes does not fit its %zu-byte period "
                       "on %d channels: raise --ad-iamf-iec61937-channels "
                       "(%" PRIu64 " so far)\n",
                   payload, period, p->sstride / 2, p->too_long + 1);
        }
        p->too_long++;
    } else {
        put16le(burst, SYNCWORD_PA);
        put16le(burst + 2, SYNCWORD_PB);
        burst[4] = DATA_TYPE_IAMF_PRIVATE;
        burst[5] = (payload >> 16) & 0x1F;
        put16le(burst + 6, payload & 0xFFFF);
        if (lead)
            memcpy(burst + BURST_HEADER_BYTES, lead, lead_len);
        memcpy(burst + BURST_HEADER_BYTES + lead_len, mpkt->buffer, mpkt->len);
        if (lead) {
            /* The descriptors that open the sequence count only once a
             * burst carrying them reaches the output: one the player drops
             * after a seek would leave the receiver without them, so the
             * next burst carries them again. */
            bool reaches = p->start_pts == MP_NOPTS_VALUE ||
                           mpkt->pts == MP_NOPTS_VALUE ||
                           mpkt->pts + (double)p->period_frames / p->rate > p->start_pts;
            if (reaches) {
                p->since_copy = 0;
                p->restart = false;
            }
        }
    }
    p->since_copy++;
    mp_aframe_set_pts(out, mpkt->pts);

done:
    talloc_free(mpkt);
    if (out) {
        mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
    } else {
        mp_filter_internal_mark_failed(da);
    }
}

/* A seek: what follows does not continue what was sent. */
static void reset(struct mp_filter *da)
{
    struct priv *p = da->priv;
    p->restart = true;
}

static int control(struct mp_filter *da, enum dec_ctrl cmd, void *arg)
{
    struct priv *p = da->priv;
    if (cmd == VDCTRL_SET_START_PTS) {
        p->start_pts = *(double *)arg;
        return CONTROL_OK;
    }
    return CONTROL_UNKNOWN;
}

static const struct mp_filter_info ad_iamf_iec61937_filter = {
    .name = "ad_iamf_iec61937",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    if (!codec->codec || strcmp(codec->codec, "iamf") != 0)
        return NULL;

    /* IAMF extradata is the IAConfigurationBox payload: version 1, a leb128
     * size, then the descriptor OBUs. */
    const uint8_t *x = codec->extradata;
    size_t x_len = codec->extradata_size;
    uint64_t size = 0;
    int n = x && x_len > 1 && x[0] == 1 ? leb128(x + 1, x_len - 1, &size) : 0;
    if (!n || size == 0 || size > x_len - 1 - n) {
        mp_err(parent->log, "IAMF track without a usable configuration "
                            "(CodecPrivate must be IAConfigurationBox "
                            "version 1)\n");
        return NULL;
    }
    const uint8_t *desc = x + 1 + n;
    size_t desc_len = size;

    uint32_t unit_samples, stated_rate;
    if (!unit_timing(desc, desc_len, &unit_samples, &stated_rate)) {
        mp_err(parent->log, "IAMF configuration without a readable codec "
                            "config\n");
        return NULL;
    }
    /* The carrier runs at the stream's rate: a unit is that many frames. */
    int rate = stated_rate ? stated_rate : codec->samplerate;
    if (rate <= 0) {
        mp_err(parent->log, "IAMF track without a sample rate\n");
        return NULL;
    }

    struct mp_filter *da = mp_filter_create(parent, &ad_iamf_iec61937_filter);
    if (!da)
        return NULL;

    mp_filter_add_pin(da, MP_PIN_IN, "in");
    mp_filter_add_pin(da, MP_PIN_OUT, "out");

    da->log = mp_log_new(da, parent->log, NULL);

    struct priv *p = da->priv;
    p->log = da->log;
    p->codec = codec;
    p->pool = mp_aframe_pool_create(p);
    p->public.f = da;
    p->public.control = control;
    p->start_pts = MP_NOPTS_VALUE;

    struct ad_iamf_iec61937_params *opts =
        mp_get_config_group(da, da->global, &ad_iamf_iec61937_conf);

    /* The frame pool sizes a period in int arithmetic — the samples aligned
     * to 32, the bytes to 64 — and process() takes frames x stride as the
     * burst's length: bound the count and the products before anything is
     * sized by them, or a stated count past INT_MAX turns negative. */
    int sstride = opts->channels * 2;
    uint64_t aligned = MP_ALIGN_UP((uint64_t)unit_samples, 32);
    if (unit_samples > (uint32_t)(INT_MAX - 32) ||
        MP_ALIGN_UP(aligned * (uint64_t)sstride, 64) > (uint64_t)INT_MAX)
    {
        MP_ERR(da, "IAMF units of %" PRIu32 " samples do not fit a carrier "
                   "period on %d channels\n", unit_samples, opts->channels);
        talloc_free(da);
        return NULL;
    }

    p->descriptors_len = desc_len;
    p->descriptors = talloc_memdup(p, (void *)desc, desc_len);
    p->redundant = talloc_memdup(p, (void *)desc, desc_len);
    for (size_t pos = 0; pos < desc_len;) {
        size_t body;
        size_t obu = obu_len(p->redundant + pos, desc_len - pos, &body);
        if (!obu) {
            MP_ERR(da, "IAMF configuration is not whole OBUs\n");
            talloc_free(da);
            return NULL;
        }
        p->redundant[pos] |= OBU_REDUNDANT_COPY;
        pos += obu;
    }

    struct mp_chmap chmap;
    mp_chmap_set_unknown(&chmap, opts->channels);
    p->rate = rate;
    p->sstride = sstride;
    p->period_frames = unit_samples;
    p->restart = true;
    if (opts->copy_interval_ms) {
        int64_t units = ((int64_t)opts->copy_interval_ms * rate + 500LL * unit_samples) /
                        (1000LL * unit_samples);
        p->copy_every = MPCLAMP(units, 1, INT32_MAX);
    }

    p->fmt = mp_aframe_create();
    talloc_steal(p, p->fmt);
    mp_aframe_set_chmap(p->fmt, &chmap);
    mp_aframe_set_format(p->fmt, AF_FORMAT_S_IAMF);
    mp_aframe_set_rate(p->fmt, rate);

    MP_INFO(da, "IAMF as IEC 61937 bursts: %u-sample units on %d ch at %d Hz "
                "(%d-byte period, %d kbit/s), descriptors (%zu bytes) every "
                "%d unit(s)\n",
            unit_samples, opts->channels, rate, p->period_frames * p->sstride,
            p->sstride * rate / 125, desc_len, p->copy_every);

    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "iamf", "iamf-iec61937",
                   "IAMF as private IEC 61937 bursts, for an Omniphony renderer");
}

const struct mp_decoder_fns ad_iamf_iec61937 = {
    .create = create,
    .add_decoders = add_decoders,
};
