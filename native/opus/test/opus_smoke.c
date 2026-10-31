#include <math.h>
#include <stdio.h>
#include <string.h>

#include <opus/opus.h>

#define RATE 48000
#define FRAME 960
#define FRAMES 50
#define TOTAL (FRAME * FRAMES)
#define MAX_PACKET 1500
#define DROPPED 30
#define MAX_LAG 640

static float input[TOTAL];
static float clean[TOTAL];
static float fec_out[TOTAL];
static float plc_out[TOTAL];
static unsigned char packets[FRAMES][MAX_PACKET];
static opus_int32 lengths[FRAMES];

static double snr_db(const float *ref, const float *test, int n) {
  double signal = 0, noise = 0;
  for (int i = 0; i < n; i++) {
    double d = (double)ref[i] - (double)test[i];
    signal += (double)ref[i] * ref[i];
    noise += d * d;
  }
  if (noise <= 0) return 200.0;
  return 10.0 * log10(signal / noise);
}

static int best_lag(const float *ref, const float *test, int start, int n) {
  int best = 0;
  double best_corr = -1e30;
  for (int lag = 0; lag < MAX_LAG; lag++) {
    double corr = 0;
    for (int i = start; i < start + n; i++) corr += (double)ref[i] * test[i + lag];
    if (corr > best_corr) {
      best_corr = corr;
      best = lag;
    }
  }
  return best;
}

static int fail(const char *what, int code) {
  printf("{\"ok\": false, \"failed\": \"%s\", \"code\": %d}\n", what, code);
  return 1;
}

#define VOICE_FRAMES 162
#define VOICE_TOTAL (FRAME * VOICE_FRAMES)
#define BINS 160
#define RECEIVERS 4

static float voice[VOICE_TOTAL];
static float heard[RECEIVERS][VOICE_TOTAL];
static unsigned char voice_packets[VOICE_FRAMES][MAX_PACKET];
static opus_int32 voice_lengths[VOICE_FRAMES];
static double cos_table[FRAME];
static double sin_table[FRAME];
static const char *receiver_names[RECEIVERS] = {"clean", "plc", "deep_plc", "dred"};

static int voice_lost(int f) {
  static const int pattern[40] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
  return f >= 2 && f < VOICE_FRAMES - 1 && pattern[(f - 2) % 40];
}

static void make_voice(void) {
  double phase = 0;
  for (int i = 0; i < VOICE_TOTAL; i++) {
    double t = (double)i / RATE;
    double f0 = 140.0 + 40.0 * sin(2.0 * M_PI * 0.7 * t) + 15.0 * sin(2.0 * M_PI * 3.1 * t);
    phase += 2.0 * M_PI * f0 / RATE;
    double s = 0;
    for (int k = 1; k * f0 < 7000.0; k++) {
      double f = k * f0;
      double env = 1.0 / (1.0 + pow((f - 600.0) / 250.0, 2)) + 0.6 / (1.0 + pow((f - 1700.0) / 350.0, 2)) +
                   0.3 / (1.0 + pow((f - 2600.0) / 450.0, 2));
      s += env * sin(k * phase) / sqrt((double)k);
    }
    voice[i] = (float)(0.12 * (0.55 + 0.45 * sin(2.0 * M_PI * 4.0 * t)) * s);
  }
}

static void spectrum(const float *x, double *power) {
  for (int k = 0; k < BINS; k++) {
    double re = 0, im = 0;
    for (int n = 0; n < FRAME; n++) {
      double w = x[n] * (0.5 - 0.5 * cos_table[n]);
      int idx = (k * n) % FRAME;
      re += w * cos_table[idx];
      im -= w * sin_table[idx];
    }
    power[k] = re * re + im * im + 1e-3;
  }
}

static double lsd_db(const float *ref, const float *test) {
  double a[BINS], b[BINS], sum = 0;
  spectrum(ref, a);
  spectrum(test, b);
  for (int k = 0; k < BINS; k++) {
    double d = 10.0 * log10(a[k] / b[k]);
    sum += d * d;
  }
  return sqrt(sum / BINS);
}

static int dred_run(void) {
  int err = 0;
  for (int n = 0; n < FRAME; n++) {
    cos_table[n] = cos(2.0 * M_PI * n / FRAME);
    sin_table[n] = sin(2.0 * M_PI * n / FRAME);
  }
  make_voice();

  OpusEncoder *enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK) return fail("dred_encoder_create", err);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));
  opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(30));
  int dred_ctl = opus_encoder_ctl(enc, OPUS_SET_DRED_DURATION(100));
  if (dred_ctl != OPUS_OK) return fail("set_dred_duration", dred_ctl);
  opus_int32 dred_duration = 0, lookahead = 0;
  opus_encoder_ctl(enc, OPUS_GET_DRED_DURATION(&dred_duration));
  opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
  long bytes = 0;
  for (int f = 0; f < VOICE_FRAMES; f++) {
    voice_lengths[f] = opus_encode_float(enc, voice + f * FRAME, FRAME, voice_packets[f], MAX_PACKET);
    if (voice_lengths[f] < 0) return fail("dred_encode", voice_lengths[f]);
    bytes += voice_lengths[f];
  }
  opus_encoder_destroy(enc);

  OpusDecoder *dec[RECEIVERS];
  static const int complexity[RECEIVERS] = {10, 0, 10, 10};
  for (int r = 0; r < RECEIVERS; r++) {
    dec[r] = opus_decoder_create(RATE, 1, &err);
    if (err != OPUS_OK) return fail("dred_decoder_create", err);
    int c = opus_decoder_ctl(dec[r], OPUS_SET_COMPLEXITY(complexity[r]));
    if (c != OPUS_OK) return fail("decoder_set_complexity", c);
  }
  OpusDecoder *bwe_dec = opus_decoder_create(RATE, 1, &err);
  if (err != OPUS_OK) return fail("bwe_decoder_create", err);
  int bwe_ctl = opus_decoder_ctl(bwe_dec, OPUS_SET_OSCE_BWE(1));
  opus_decoder_destroy(bwe_dec);
  OpusDREDDecoder *dred_dec = opus_dred_decoder_create(&err);
  if (err != OPUS_OK) return fail("dred_decoder_create", err);
  OpusDRED *dred = opus_dred_alloc(&err);
  if (err != OPUS_OK) return fail("dred_alloc", err);

  int lost = 0, recovered = 0, pending = 0, max_dred = 0, dred_packets = 0;
  for (int f = 0; f < VOICE_FRAMES; f++) {
    const unsigned char *pkt = voice_packets[f];
    opus_int32 len = voice_lengths[f];
    int n = opus_decode_float(dec[0], pkt, len, heard[0] + f * FRAME, FRAME, 0);
    if (n != FRAME) return fail("dred_clean_decode", n);
    int dred_end = 0;
    int available = opus_dred_parse(dred_dec, dred, pkt, len, RATE, RATE, &dred_end, 0);
    if (available > 0) {
      dred_packets++;
      if (available > max_dred) max_dred = available;
    }
    if (voice_lost(f)) {
      lost++;
      pending++;
      for (int r = 1; r <= 2; r++) {
        n = opus_decode_float(dec[r], NULL, 0, heard[r] + f * FRAME, FRAME, 0);
        if (n != FRAME) return fail("dred_plc_decode", n);
      }
      continue;
    }
    if (pending > 0) {
      for (int k = 0; k < pending; k++) {
        int slot = f - pending + k;
        opus_int32 offset = (pending - k) * FRAME;
        if (available >= offset) {
          n = opus_decoder_dred_decode_float(dec[3], dred, offset, heard[3] + slot * FRAME, FRAME);
          if (n != FRAME) return fail("dred_decode", n);
          recovered++;
        } else {
          n = opus_decode_float(dec[3], NULL, 0, heard[3] + slot * FRAME, FRAME, 0);
          if (n != FRAME) return fail("dred_fallback_plc", n);
        }
      }
      pending = 0;
    }
    for (int r = 1; r < RECEIVERS; r++) {
      n = opus_decode_float(dec[r], pkt, len, heard[r] + f * FRAME, FRAME, 0);
      if (n != FRAME) return fail("dred_stream_decode", n);
    }
  }

  int lag = lookahead;
  double best = -1e30;
  for (int l = lookahead - 32; l <= lookahead + 32; l++) {
    double corr = 0;
    for (int i = 20 * FRAME; i < 60 * FRAME; i++) corr += (double)voice[i - l] * heard[0][i];
    if (corr > best) {
      best = corr;
      lag = l;
    }
  }

  double signal[RECEIVERS] = {0}, noise[RECEIVERS] = {0}, lsd[RECEIVERS] = {0};
  int measured = 0;
  for (int f = 0; f < VOICE_FRAMES; f++) {
    if (!voice_lost(f)) continue;
    const float *ref = voice + f * FRAME - lag;
    measured++;
    for (int r = 0; r < RECEIVERS; r++) {
      const float *x = heard[r] + f * FRAME;
      for (int i = 0; i < FRAME; i++) {
        double d = (double)ref[i] - x[i];
        signal[r] += (double)ref[i] * ref[i];
        noise[r] += d * d;
      }
      lsd[r] += lsd_db(ref, x);
    }
  }

  printf("\"dred\": {\"duration\": %d, \"bytes\": %ld, \"frames\": %d, \"lost\": %d, \"recovered\": %d, "
         "\"dred_packets\": %d, \"max_dred_samples\": %d, \"lag\": %d, \"osce_bwe_ctl\": %d",
         dred_duration, bytes, VOICE_FRAMES, lost, recovered, dred_packets, max_dred, lag, bwe_ctl);
  for (int r = 0; r < RECEIVERS; r++) {
    printf(", \"%s_snr_db\": %.2f, \"%s_lsd_db\": %.2f", receiver_names[r], 10.0 * log10(signal[r] / noise[r]),
           receiver_names[r], lsd[r] / measured);
  }
  printf("}}\n");

  for (int r = 0; r < RECEIVERS; r++) opus_decoder_destroy(dec[r]);
  opus_dred_free(dred);
  opus_dred_decoder_destroy(dred_dec);
  return 0;
}

int main(void) {
  int err = 0;
  for (int i = 0; i < TOTAL; i++) input[i] = 0.5f * (float)sin(2.0 * M_PI * 440.0 * i / RATE);

  OpusEncoder *enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK) return fail("encoder_create", err);
  if (opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000)) != OPUS_OK) return fail("set_bitrate", 0);
  if (opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1)) != OPUS_OK) return fail("set_inband_fec", 0);
  if (opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(20)) != OPUS_OK) return fail("set_packet_loss_perc", 0);
  if (opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) return fail("set_signal", 0);
  opus_int32 fec_on = 0, lookahead = 0;
  opus_encoder_ctl(enc, OPUS_GET_INBAND_FEC(&fec_on));
  opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

  int lbrr_packets = 0;
  long total_bytes = 0;
  for (int f = 0; f < FRAMES; f++) {
    lengths[f] = opus_encode_float(enc, input + f * FRAME, FRAME, packets[f], MAX_PACKET);
    if (lengths[f] < 0) return fail("encode", lengths[f]);
    total_bytes += lengths[f];
    if (opus_packet_has_lbrr(packets[f], lengths[f]) == 1) lbrr_packets++;
  }

  OpusDecoder *dec_clean = opus_decoder_create(RATE, 1, &err);
  if (err != OPUS_OK) return fail("decoder_create", err);
  OpusDecoder *dec_fec = opus_decoder_create(RATE, 1, &err);
  if (err != OPUS_OK) return fail("decoder_create", err);
  OpusDecoder *dec_plc = opus_decoder_create(RATE, 1, &err);
  if (err != OPUS_OK) return fail("decoder_create", err);

  int fec_samples = 0, plc_samples = 0;
  for (int f = 0; f < FRAMES; f++) {
    int n = opus_decode_float(dec_clean, packets[f], lengths[f], clean + f * FRAME, FRAME, 0);
    if (n != FRAME) return fail("decode", n);
    if (f == DROPPED) {
      plc_samples = opus_decode_float(dec_plc, NULL, 0, plc_out + f * FRAME, FRAME, 0);
      if (plc_samples != FRAME) return fail("plc_decode", plc_samples);
      continue;
    }
    if (f == DROPPED + 1) {
      fec_samples = opus_decode_float(dec_fec, packets[f], lengths[f], fec_out + DROPPED * FRAME, FRAME, 1);
      if (fec_samples != FRAME) return fail("fec_decode", fec_samples);
    }
    n = opus_decode_float(dec_fec, packets[f], lengths[f], fec_out + f * FRAME, FRAME, 0);
    if (n != FRAME) return fail("decode_fec_stream", n);
    n = opus_decode_float(dec_plc, packets[f], lengths[f], plc_out + f * FRAME, FRAME, 0);
    if (n != FRAME) return fail("decode_plc_stream", n);
  }

  int start = 10 * FRAME;
  int span = 20 * FRAME;
  int lag = best_lag(input, clean, start, span);
  double snr = snr_db(input + start, clean + start + lag, span);
  double fec_snr = snr_db(clean + DROPPED * FRAME, fec_out + DROPPED * FRAME, FRAME);
  double plc_snr = snr_db(clean + DROPPED * FRAME, plc_out + DROPPED * FRAME, FRAME);

  opus_encoder_ctl(enc, OPUS_RESET_STATE);
  opus_encoder_ctl(enc, OPUS_SET_DTX(1));
  static float silence[FRAME];
  unsigned char packet[MAX_PACKET];
  int dtx_packets = 0;
  for (int f = 0; f < FRAMES; f++) {
    opus_int32 len = opus_encode_float(enc, silence, FRAME, packet, MAX_PACKET);
    if (len < 0) return fail("encode_silence", len);
    if (len <= 2) dtx_packets++;
  }

  printf("{\"ok\": true, \"version\": \"%s\", \"lookahead\": %d, \"fec_enabled\": %d, \"lbrr_packets\": %d, "
         "\"bytes\": %ld, \"lag\": %d, \"snr_db\": %.2f, \"fec_samples\": %d, \"fec_snr_db\": %.2f, "
         "\"plc_samples\": %d, \"plc_snr_db\": %.2f, \"dtx_packets\": %d, \"frames\": %d, ",
         opus_get_version_string(), lookahead, fec_on, lbrr_packets, total_bytes, lag, snr, fec_samples, fec_snr,
         plc_samples, plc_snr, dtx_packets, FRAMES);

  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec_clean);
  opus_decoder_destroy(dec_fec);
  opus_decoder_destroy(dec_plc);
  return dred_run();
}
