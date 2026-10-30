#ifndef FM_OPUS_H
#define FM_OPUS_H

#include "opus.h"

static inline int fm_opus_encoder_set(OpusEncoder *st, int request, opus_int32 value) {
    return opus_encoder_ctl(st, request, value);
}

static inline int fm_opus_encoder_get(OpusEncoder *st, int request, opus_int32 *value) {
    return opus_encoder_ctl(st, request, value);
}

static inline int fm_opus_encoder_reset(OpusEncoder *st) {
    return opus_encoder_ctl(st, OPUS_RESET_STATE);
}

static inline int fm_opus_decoder_set(OpusDecoder *st, int request, opus_int32 value) {
    return opus_decoder_ctl(st, request, value);
}

static inline int fm_opus_decoder_get(OpusDecoder *st, int request, opus_int32 *value) {
    return opus_decoder_ctl(st, request, value);
}

static inline int fm_opus_decoder_reset(OpusDecoder *st) {
    return opus_decoder_ctl(st, OPUS_RESET_STATE);
}

#endif
