#include "VoiceEngine.h"

#include <algorithm>
#include <cmath>

namespace forkmesh::voice {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kEps = 1e-9;
// The noise floor is the minimum over this many ~0.2 s windows.
constexpr int kMinWindows = 8;

double clamp(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

// JavaScript's Math.round, so this engine and the notes in the package tests
// agree on halves: round half up, never away from zero.
double roundHalfUp(double value)
{
    return std::floor(value + 0.5);
}

VoiceParams make(double targetPitchHz, double semitones, double formant,
                 double breath, double whisper, double brightness,
                 double roughness, double gainDb, double mix = 1)
{
    VoiceParams p;
    p.targetPitchHz = targetPitchHz;
    p.semitones = semitones;
    p.formant = formant;
    p.breath = breath;
    p.whisper = whisper;
    p.brightness = brightness;
    p.roughness = roughness;
    p.gainDb = gainDb;
    p.mix = mix;
    return p;
}

} // namespace

const std::vector<ParamBounds> &paramBounds()
{
    static const std::vector<ParamBounds> bounds = {
        {"targetPitchHz", 0, 0, 400},
        {"semitones", 0, -12, 12},
        {"formant", 1, 0.7, 1.45},
        {"breath", 0, 0, 1},
        {"whisper", 0, 0, 1},
        {"brightness", 0, -1, 1},
        {"roughness", 0, 0, 1},
        {"gainDb", 0, -12, 12},
        {"mix", 1, 0, 1},
    };
    return bounds;
}

double paramValue(const VoiceParams &p, const std::string &name)
{
    if (name == "targetPitchHz") return p.targetPitchHz;
    if (name == "semitones") return p.semitones;
    if (name == "formant") return p.formant;
    if (name == "breath") return p.breath;
    if (name == "whisper") return p.whisper;
    if (name == "brightness") return p.brightness;
    if (name == "roughness") return p.roughness;
    if (name == "gainDb") return p.gainDb;
    if (name == "mix") return p.mix;
    return 0;
}

void setParamValue(VoiceParams &p, const std::string &name, double value)
{
    if (name == "targetPitchHz") p.targetPitchHz = value;
    else if (name == "semitones") p.semitones = value;
    else if (name == "formant") p.formant = value;
    else if (name == "breath") p.breath = value;
    else if (name == "whisper") p.whisper = value;
    else if (name == "brightness") p.brightness = value;
    else if (name == "roughness") p.roughness = value;
    else if (name == "gainDb") p.gainDb = value;
    else if (name == "mix") p.mix = value;
}

VoiceParams VoiceParams::clean() const
{
    VoiceParams out;
    for (const ParamBounds &b : paramBounds()) {
        const double value = paramValue(*this, b.name);
        setParamValue(out, b.name,
                      std::isfinite(value) ? clamp(value, b.low, b.high)
                                           : b.fallback);
    }
    if (out.targetPitchHz > 0 && out.targetPitchHz < 60)
        out.targetPitchHz = 60;
    return out;
}

const std::vector<VoicePreset> &presets()
{
    //                          pitch  semi  formant breath whisper bright rough gain  mix
    static const std::vector<VoicePreset> list = {
        {"natural", "Natural (off)", make(0, 0, 1, 0, 0, 0, 0, 0, 0)},
        {"deep-baritone", "Deep baritone", make(92, 0, 0.86, 0, 0, -0.25, 0, 0)},
        {"warm-tenor", "Warm tenor", make(135, 0, 0.95, 0, 0, 0, 0, 0)},
        {"bright-alto", "Bright alto", make(190, 0, 1.12, 0, 0, 0.15, 0, 0)},
        {"clear-soprano", "Clear soprano", make(245, 0, 1.2, 0.08, 0, 0.2, 0, 0)},
        {"young", "Young voice", make(290, 0, 1.3, 0, 0, 0.25, 0, 0)},
        {"elder", "Elder", make(118, 0, 0.93, 0.2, 0, -0.2, 0.45, 0)},
        {"gravel", "Gravel", make(85, 0, 0.84, 0, 0, -0.35, 0.8, 0)},
        {"radio-host", "Radio host", make(105, 0, 0.9, 0, 0, 0.1, 0, 2)},
        {"whisper", "Whisper", make(0, 0, 1.02, 0.3, 1, 0, 0, 0)},
        {"giant", "Giant", make(0, -9, 0.74, 0, 0, -0.4, 0, 0)},
        {"chipmunk", "Chipmunk", make(0, 9, 1.4, 0, 0, 0.3, 0, 0)},
    };
    return list;
}

// --- FFT -----------------------------------------------------------------

Fft::Fft(int size)
    : m_size(size), m_cos(size / 2), m_sin(size / 2), m_reverse(size)
{
    for (int i = 0; i < size / 2; ++i) {
        m_cos[i] = std::cos(kTwoPi * i / size);
        m_sin[i] = std::sin(kTwoPi * i / size);
    }
    const int bits = int(std::lround(std::log2(size)));
    for (int i = 0; i < size; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            r |= uint32_t((i >> b) & 1) << (bits - 1 - b);
        m_reverse[i] = r;
    }
}

void Fft::transform(double *re, double *im, bool inverse) const
{
    const int n = m_size;
    for (int i = 0; i < n; ++i) {
        const int j = int(m_reverse[i]);
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    const double sign = inverse ? 1 : -1;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int start = 0; start < n; start += len) {
            for (int k = 0; k < half; ++k) {
                const double wr = m_cos[k * step];
                const double wi = sign * m_sin[k * step];
                const int a = start + k;
                const int b = a + half;
                const double xr = re[b] * wr - im[b] * wi;
                const double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
    if (inverse) {
        for (int i = 0; i < n; ++i) {
            re[i] /= n;
            im[i] /= n;
        }
    }
}

// --- pitch -----------------------------------------------------------------

PitchDetector::PitchDetector(double sampleRate, int size)
    : m_sampleRate(sampleRate), m_size(size), m_fft(size * 2),
      m_re(size * 2), m_im(size * 2)
{
    m_minLag = std::max(2, int(std::floor(sampleRate / 420)));
    m_maxLag = std::min(size - 2, int(std::ceil(sampleRate / 60)));
    m_nsdf.assign(m_maxLag + 2, 0);
}

double PitchDetector::detect(const double *samples)
{
    const int n = m_size;
    double mean = 0;
    for (int i = 0; i < n; ++i)
        mean += samples[i];
    mean /= n;
    double energy = 0;
    for (int i = 0; i < n; ++i) {
        const double v = samples[i] - mean;
        m_re[i] = v;
        m_im[i] = 0;
        energy += v * v;
    }
    if (std::sqrt(energy / n) < 0.004)
        return 0;
    std::fill(m_re.begin() + n, m_re.end(), 0);
    std::fill(m_im.begin() + n, m_im.end(), 0);
    m_fft.transform(m_re.data(), m_im.data(), false);
    for (int i = 0; i < n * 2; ++i) {
        m_re[i] = m_re[i] * m_re[i] + m_im[i] * m_im[i];
        m_im[i] = 0;
    }
    m_fft.transform(m_re.data(), m_im.data(), true);
    // m(tau) = sum x[j]^2 + x[j+tau]^2 over the overlap, updated per lag.
    double m = 2 * m_re[0];
    for (int tau = 0; tau <= m_maxLag + 1; ++tau) {
        if (tau > 0) {
            const double head = samples[tau - 1] - mean;
            const double tail = samples[n - tau] - mean;
            m -= head * head + tail * tail;
        }
        m_nsdf[tau] = m > kEps ? 2 * m_re[tau] / m : 0;
    }
    // Key maxima: the highest point between each positive-going and
    // negative-going zero crossing; take the first within 90% of the best.
    int peaks[64];
    int peakCount = 0;
    int tau = 1;
    while (tau < m_maxLag && m_nsdf[tau] > 0)
        ++tau;
    while (tau < m_maxLag && peakCount < 64) {
        while (tau < m_maxLag && m_nsdf[tau] <= 0)
            ++tau;
        int best = tau;
        while (tau < m_maxLag && m_nsdf[tau] > 0) {
            if (m_nsdf[tau] > m_nsdf[best])
                best = tau;
            ++tau;
        }
        if (best >= m_minLag && best < m_maxLag)
            peaks[peakCount++] = best;
    }
    if (!peakCount)
        return 0;
    double highest = 0;
    for (int i = 0; i < peakCount; ++i)
        highest = std::max(highest, m_nsdf[peaks[i]]);
    if (highest < 0.6)
        return 0;
    int lag = peaks[0];
    for (int i = 0; i < peakCount; ++i) {
        if (m_nsdf[peaks[i]] >= 0.9 * highest) {
            lag = peaks[i];
            break;
        }
    }
    const double a = m_nsdf[lag - 1];
    const double b = m_nsdf[lag];
    const double c = m_nsdf[lag + 1];
    const double denominator = a - 2 * b + c;
    const double shift = std::abs(denominator) > kEps
        ? clamp(0.5 * (a - c) / denominator, -1, 1) : 0;
    return m_sampleRate / (lag + shift);
}

// --- engine ------------------------------------------------------------------

VoiceEngine::VoiceEngine(double sampleRate, double userPitchHz)
    : m_sampleRate(sampleRate), m_size(sampleRate > 32000 ? 1024 : 512),
      m_hop(m_size / 8), m_half(m_size / 2), m_fft(m_size),
      m_tail(m_size / 2), m_window(m_size), m_synthesis(m_size),
      m_olaScale(double(m_size / 8) / (m_size / 4)), m_inFifo(m_size),
      m_outFifo(m_size), m_accum(m_size * 2),
      m_fifoPos(m_size - m_size / 8), m_re(m_size), m_im(m_size),
      m_cre(m_size), m_cim(m_size), m_anaMag(m_size / 2 + 1),
      m_envelope(m_size / 2 + 1), m_phaseIn(m_size / 2 + 1),
      m_tilt(m_size / 2 + 1),
      m_lifter(std::max(12, int(roundHalfUp(sampleRate / 650)))),
      m_decimate(std::max(1, int(roundHalfUp(sampleRate / 12000)))),
      m_pitch(sampleRate / std::max(1, int(roundHalfUp(sampleRate / 12000))), 1024),
      m_pitchRing(1024), m_pitchScratch(1024),
      m_userLogPitch(std::log(userPitchHz > 0 ? userPitchHz : 150)),
      m_shifter(sampleRate),
      m_noisePower(m_size / 2 + 1), m_trackPower(m_size / 2 + 1),
      m_minPower(m_size / 2 + 1), m_cleanPower(m_size / 2 + 1),
      m_minHistory(kMinWindows * size_t(m_size / 2 + 1))
{
    const int rise = m_size - m_tail / 2;
    const int fall = m_size - m_tail;
    for (int i = 0; i < m_size; ++i) {
        const double hann = i < rise
            ? 0.5 - 0.5 * std::cos(kPi * i / rise)
            : 0.5 - 0.5 * std::cos(kTwoPi * (i - fall) / m_tail);
        m_window[i] = std::sqrt(hann);
        m_windowPower += hann / m_size;
        m_synthesis[i] = i < fall ? 0
            : (0.5 - 0.5 * std::cos(kTwoPi * (i - fall) / m_tail)) / m_window[i];
    }
    m_dry.assign(size_t(latencySamples()), 0);
    setParams(VoiceParams{});
}

void VoiceEngine::setParams(const VoiceParams &params)
{
    m_params = params.clean();
    const double tiltDb = m_params.brightness * 6;
    const double binHz = m_sampleRate / m_size;
    for (int k = 0; k <= m_half; ++k) {
        const double hz = std::max(k * binHz, 60.0);
        m_tilt[k] = std::pow(10, tiltDb * std::log2(hz / 1000) / 20);
    }
}

void VoiceEngine::setNoiseReduction(double amount)
{
    const double wanted = std::isfinite(amount) ? clamp(amount, 0, 1) : 0;
    if (wanted > 0 && m_denoise <= 0) {
        m_hpX1 = m_hpX2 = m_hpY1 = m_hpY2 = 0;
        m_noiseReady = false;
        m_denoiseFrames = 0;
        m_minFrames = 0;
    }
    m_denoise = wanted;
    // Second-order Butterworth high-pass at 70 Hz: rumble, handling noise and
    // DC, well under any voice's fundamental that matters.
    const double w0 = kTwoPi * 70 / m_sampleRate;
    const double alpha = std::sin(w0) / std::sqrt(2.0);
    const double a0 = 1 + alpha;
    m_hpB0 = (1 + std::cos(w0)) / 2 / a0;
    m_hpB1 = -(1 + std::cos(w0)) / a0;
    m_hpB2 = m_hpB0;
    m_hpA1 = -2 * std::cos(w0) / a0;
    m_hpA2 = (1 - alpha) / a0;
}

void VoiceEngine::setUserPitch(double hz)
{
    if (hz > 50 && hz < 500)
        m_userLogPitch = std::log(hz);
}

VoiceStats VoiceEngine::stats() const
{
    VoiceStats s;
    s.userPitchHz = int(roundHalfUp(std::exp(m_userLogPitch)));
    s.pitchHz = int(roundHalfUp(m_lastPitchHz));
    s.voiced = m_voiced;
    s.ratio = m_ratio;
    s.level = m_level;
    s.latencyMs = int(roundHalfUp(latencySamples() / m_sampleRate * 1000));
    return s;
}

double VoiceEngine::random()
{
    // xorshift32: deterministic, allocation-free noise for the audio thread.
    uint32_t x = m_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_seed = x;
    return x / 4294967296.0;
}

double VoiceEngine::targetRatio() const
{
    double ratio = std::pow(2, m_params.semitones / 12);
    if (m_params.targetPitchHz > 0)
        ratio *= m_params.targetPitchHz / std::exp(m_userLogPitch);
    if (m_params.roughness > 0)
        ratio *= std::pow(2, m_wobble * m_params.roughness * 0.6 / 12);
    return clamp(ratio, 0.45, 2.6);
}

void VoiceEngine::trackPitch(double sample)
{
    m_decAcc += sample;
    if (++m_decCount < m_decimate)
        return;
    m_pitchRing[m_pitchWrite] = m_decAcc / m_decCount;
    m_pitchWrite = (m_pitchWrite + 1) % int(m_pitchRing.size());
    m_decAcc = 0;
    m_decCount = 0;
}

void VoiceEngine::analysePitch()
{
    const int n = int(m_pitchRing.size());
    for (int i = 0; i < n; ++i)
        m_pitchScratch[i] = m_pitchRing[(m_pitchWrite + i) % n];
    const double hz = m_pitch.detect(m_pitchScratch.data());
    m_voiced = hz >= 60 && hz <= 420;
    if (!m_voiced) {
        // The shifter keeps the last pitch through a short dropout (a breathy
        // syllable end, a creak) so the voice does not flick back to the
        // speaker's own pitch for a moment.
        if (m_voicedHold > 0 && --m_voicedHold == 0)
            m_shiftPitchHz = 0;
        return;
    }
    // The median of the last three readings, so one octave slip does not
    // reach the shifter.
    if (m_voicedHold == 0)
        m_recentPitch[0] = m_recentPitch[1] = m_recentPitch[2] = hz;
    m_recentPitch[0] = m_recentPitch[1];
    m_recentPitch[1] = m_recentPitch[2];
    m_recentPitch[2] = hz;
    const double a = m_recentPitch[0], b = m_recentPitch[1], c = m_recentPitch[2];
    m_shiftPitchHz = std::max(std::min(a, b), std::min(std::max(a, b), c));
    m_voicedHold = 5;
    m_lastPitchHz = hz;
    // A slow running mean in the log domain: the speaker's own pitch.
    const double logHz = std::log(hz);
    if (std::abs(logHz - m_userLogPitch) < std::log(2.2))
        m_userLogPitch += 0.03 * (logHz - m_userLogPitch);
}

void VoiceEngine::process(const float *input, float *output, int count)
{
    if (m_bypass) {
        for (int i = 0; i < count; ++i) {
            trackPitch(input[i]);
            output[i] = input[i];
        }
        return;
    }
    const int n = m_size;
    const int hop = m_hop;
    const int latency = int(m_dry.size());
    const double dryGain = 1 - m_params.mix;
    const double wetGain = m_params.mix * std::pow(10, m_params.gainDb / 20);
    // A peak limiter: the shift and the level match can push peaks past full
    // scale, and the sound server clips those into crackle. Instant attack,
    // ~100 ms release.
    constexpr double kCeiling = 0.95;
    const double release = 1 / (0.1 * m_sampleRate);
    for (int i = 0; i < count; ++i) {
        const double sample = input[i];
        double changed = sample;
        if (m_denoise > 0) {
            changed = m_hpB0 * sample + m_hpB1 * m_hpX1 + m_hpB2 * m_hpX2 -
                      m_hpA1 * m_hpY1 - m_hpA2 * m_hpY2;
            m_hpX2 = m_hpX1;
            m_hpX1 = sample;
            m_hpY2 = m_hpY1;
            m_hpY1 = changed;
        }
        trackPitch(changed);
        m_inFifo[m_fifoPos] = changed;
        const double delayed = m_dry[m_dryPos];
        m_dry[m_dryPos] = sample;
        m_dryPos = m_dryPos + 1 == latency ? 0 : m_dryPos + 1;
        const double filtered = m_agc * m_outFifo[m_fifoPos - (n - hop)];
        // A whisper has no pitch to move; shifting its noise would give it one.
        const double period = m_shiftPitchHz > 0 && m_params.whisper < 0.5
            ? m_sampleRate / m_shiftPitchHz : 0;
        const double wet = m_shifter.process(filtered, period, m_ratio);
        const double mixed = wetGain * wet + dryGain * delayed;
        const double peak = std::abs(mixed) * m_limit;
        if (peak > kCeiling)
            m_limit = kCeiling / std::abs(mixed);
        else
            m_limit += release * (1 - m_limit);
        output[i] = float(mixed * m_limit);
        if (++m_fifoPos >= n) {
            m_fifoPos = n - hop;
            frame();
            std::copy(m_accum.begin(), m_accum.begin() + hop, m_outFifo.begin());
            std::copy(m_accum.begin() + hop, m_accum.begin() + hop + n,
                      m_accum.begin());
            std::fill(m_accum.begin() + n, m_accum.begin() + n + hop, 0);
            std::copy(m_inFifo.begin() + hop, m_inFifo.end(), m_inFifo.begin());
            levelMatch();
        }
    }
}

// Keep the loudness of the speaker: the envelope warp and the shift both move
// energy, and a voice that jumps in level sounds broken. The cleaned input
// spectrum is compared with the rebuilt one over ~300 ms of speech and held
// through pauses, so neither silence nor the noise that was taken out is
// pumped back up.
void VoiceEngine::levelMatch()
{
    if (m_inPower < 1e-6 || m_outPower < 1e-9)
        return;
    const double wanted = clamp(std::sqrt(m_inPower / m_outPower), 0.5, 4);
    m_agc += 0.05 * (wanted - m_agc);
}

// Background noise out of m_anaMag, bin by bin (minimum statistics). Each
// bin's power, smoothed over ~20 ms, has its minimum taken over the last
// ~1.6 s: eight 0.2 s windows, the oldest dropped as a new one starts. Speech
// fills a bin for a syllable at a time, so the minimum sits on the noise in
// the gaps between them; the smoothing is short enough to reach the floor
// inside a 100 ms gap. A noise periodogram smoothed that little dips to about
// a quarter of its mean over 1.6 s, which the bias factor puts back. The gain
// is Wiener's, with the a-priori SNR smoothed from the previous frame's clean
// power (decision-directed), which keeps leftover noise from turning into
// short random tones; and never below a floor, so the room is quieter rather
// than gone and quiet speech is never gated.
void VoiceEngine::denoise()
{
    // Learning starts with the first frame that is all real input, not the
    // silence the frame buffer starts out with.
    if (!m_noiseReady && ++m_denoiseFrames < m_oversample)
        return;
    const double framesPerSecond = m_sampleRate / m_hop;
    const double track = 1 - std::exp(-1 / (0.02 * framesPerSecond));
    const int windowFrames = std::max(1, int(0.2 * framesPerSecond));
    constexpr double kBias = 4.0;
    const double floorGain = std::pow(10, -18 * m_denoise / 20);
    const int bins = m_half + 1;
    // Every windowFrames the window just finished replaces the oldest one.
    const bool newWindow = m_noiseReady && ++m_minFrames >= windowFrames;
    const int finished = m_minSlot;
    if (newWindow) {
        m_minFrames = 0;
        m_minSlot = (m_minSlot + 1) % kMinWindows;
    }
    double speech = 0;
    double noise = 0;
    for (int k = 0; k <= m_half; ++k) {
        const double power = m_anaMag[k] * m_anaMag[k];
        double &tracked = m_trackPower[k];
        double &estimate = m_noisePower[k];
        double *history = m_minHistory.data() + k;
        if (!m_noiseReady) {
            tracked = power;
            for (int w = 0; w < kMinWindows; ++w)
                history[w * bins] = power;
        } else {
            tracked += track * (power - tracked);
        }
        // m_minPower: this window's minimum so far; the history holds each
        // finished window's.
        double &current = m_minPower[k];
        if (newWindow || !m_noiseReady) {
            if (m_noiseReady)
                history[finished * bins] = current;
            current = tracked;
        } else if (tracked < current) {
            current = tracked;
        }
        double minimum = current;
        for (int w = 0; w < kMinWindows; ++w)
            minimum = std::min(minimum, history[w * bins]);
        estimate = kBias * minimum;
        const double expected = estimate + 1e-12;
        const double posterior = power / expected;
        const double prior = std::max(0.96 * m_cleanPower[k] / expected +
                                      0.04 * std::max(posterior - 1, 0.0), 1e-3);
        const double gain = std::max(prior / (1 + prior), floorGain);
        m_cleanPower[k] = gain * gain * power;
        m_anaMag[k] *= gain;
        speech += power;
        noise += estimate;
    }
    m_noiseReady = true;
    m_speechEnergy = speech;
    m_noiseEnergy = noise;
}

void VoiceEngine::frame()
{
    const int n = m_size;
    const int half = m_half;
    const int hop = m_hop;
    double *re = m_re.data();
    double *im = m_im.data();
    const VoiceParams &p = m_params;
    if (++m_framesSincePitch >= 4) {
        m_framesSincePitch = 0;
        analysePitch();
    }
    if (p.roughness > 0 && random() < 0.08)
        m_wobbleTarget = random() * 2 - 1;
    m_wobble += 0.12 * (m_wobbleTarget - m_wobble);
    m_ratio += 0.25 * (targetRatio() - m_ratio);

    // The frame is rotated half a frame (zero-phase windowing).
    double energyIn = 0;
    for (int i = 0; i < n; ++i) {
        const double v = m_inFifo[i] * m_window[i];
        re[(i + half) % n] = v;
        im[i] = 0;
        energyIn += v * v;
    }
    m_level = std::sqrt(energyIn / n * 0.375 / m_windowPower);
    m_fft.transform(re, im, false);

    // Spectral envelope by cepstral smoothing of the log magnitude.
    for (int k = 0; k <= half; ++k) {
        m_anaMag[k] = std::hypot(re[k], im[k]);
        m_phaseIn[k] = std::atan2(im[k], re[k]);
    }
    bool speech = true;
    if (m_denoise > 0) {
        denoise();
        // Level matching listens only while someone is talking over the room.
        speech = m_speechEnergy > 3 * m_noiseEnergy;
    }
    double *cre = m_cre.data();
    double *cim = m_cim.data();
    double spectrumIn = 0;
    for (int k = 0; k <= half; ++k) {
        spectrumIn += m_anaMag[k] * m_anaMag[k];
        cre[k] = std::log(m_anaMag[k] + kEps);
        cim[k] = 0;
        if (k > 0 && k < half) {
            cre[n - k] = cre[k];
            cim[n - k] = 0;
        }
    }
    m_fft.transform(cre, cim, true);
    for (int q = 0; q < n; ++q) {
        if (!(q < m_lifter || q > n - m_lifter))
            cre[q] = 0;
        cim[q] = 0;
    }
    m_fft.transform(cre, cim, false);
    for (int k = 0; k <= half; ++k)
        m_envelope[k] = std::exp(cre[k]);

    // The source: what is left once the vocal-tract filter is divided out.
    // The pitch moves later, in the time domain; here the harmonics stay put
    // and only the filter changes, so this stage never smears them.
    double meanSource = 0;
    for (int k = 1; k < half; ++k)
        meanSource += m_anaMag[k] / (m_envelope[k] + kEps);
    meanSource /= half - 1;

    // Rebuild: source x formant-warped envelope x tilt, plus breath.
    double spectrumOut = 0;
    for (int k = 0; k <= half; ++k) {
        const double sourceMag = m_anaMag[k] / (m_envelope[k] + kEps);
        double phase = m_phaseIn[k];
        const double source = k / p.formant;
        const int low = int(std::floor(source));
        const double env = low >= half
            ? m_envelope[half]
            : m_envelope[low] + (m_envelope[low + 1] - m_envelope[low]) * (source - low);
        const double magnitude = sourceMag * (1 - p.whisper) + meanSource * p.whisper;
        if (p.whisper > 0)
            phase = phase * (1 - p.whisper) + random() * kTwoPi * p.whisper;
        const double value = magnitude * env * m_tilt[k];
        double outRe = value * std::cos(phase);
        double outIm = value * std::sin(phase);
        if (p.breath > 0) {
            const double hz = double(k) * m_sampleRate / n;
            const double air = p.breath * meanSource * env * 0.6 *
                               clamp((hz - 800) / 3000, 0, 1);
            const double noisePhase = random() * kTwoPi;
            outRe += air * std::cos(noisePhase);
            outIm += air * std::sin(noisePhase);
        }
        re[k] = outRe;
        im[k] = outIm;
        spectrumOut += outRe * outRe + outIm * outIm;
        if (k > 0 && k < half) {
            re[n - k] = outRe;
            im[n - k] = -outIm;
        }
    }
    im[0] = 0;
    im[half] = 0;
    // Both spectra are of the same analysis window, so their energies compare
    // directly; scaled to mean power per sample.
    if (speech) {
        const double scale = 2.0 / (double(n) * n * m_windowPower);
        const double smoothing = 1 - std::exp(-double(hop) / (0.3 * m_sampleRate));
        m_inPower += smoothing * (spectrumIn * scale - m_inPower);
        m_outPower += smoothing * (spectrumOut * scale - m_outPower);
    }

    double rough = 1;
    if (p.roughness > 0)
        rough = 1 + p.roughness * 0.18 * (random() * 2 - 1);

    m_fft.transform(re, im, true);
    const double scale = m_olaScale * rough;
    const int fall = n - m_tail;
    for (int i = fall; i < n; ++i)
        m_accum[i - fall] += re[(i + half) % n] * m_synthesis[i] * scale;
}

// --- moving the pitch ---------------------------------------------------------

PitchShifter::PitchShifter(double sampleRate)
    : m_mask(8191),
      // Grains are at most ~21 ms (a 94 Hz period each side); a lower voice
      // gets a shorter grain than two periods, which still holds its pulse.
      m_maxHalf(int(sampleRate * 0.0107)),
      m_unvoiced(std::max(16, int(sampleRate * 0.005))),
      m_lookahead(m_maxHalf + m_maxHalf / 4),
      m_delay(m_lookahead + m_maxHalf + 1),
      m_in(size_t(m_mask) + 1), m_out(size_t(m_mask) + 1), m_marks(32)
{
}

void PitchShifter::addMarks(double period)
{
    const bool voiced = period >= 16;
    const int step = voiced ? int(period + 0.5) : m_unvoiced;
    const int half = voiced ? std::min(step, m_maxHalf) : m_unvoiced;
    const int search = voiced ? step / 8 : 0;
    for (;;) {
        const int64_t guess = m_last.at + step;
        if (guess + search + half >= m_written)
            return;
        int64_t best = guess;
        int measured = step;
        if (voiced && m_last.voiced) {
            // One period on, where the waveform best repeats the last mark's
            // (normalised cross-correlation over a period, every other sample).
            const int reach = std::max(8, half / 2);
            double bestScore = -1e300;
            for (int64_t c = guess - search; c <= guess + search; ++c) {
                double dot = 0;
                double energy = 0;
                for (int j = -reach; j <= reach; j += 2) {
                    const double v = at(c + j);
                    dot += at(m_last.at + j) * v;
                    energy += v * v;
                }
                const double score = dot / std::sqrt(energy + 1e-12);
                if (score > bestScore) {
                    bestScore = score;
                    best = c;
                }
            }
            // The period the waveform really had here, which the output's
            // pitch follows: the tracker's reading is a coarse, late average.
            measured = int(best - m_last.at);
        }
        m_last = {best, measured, half, voiced};
        m_marks[size_t(m_markWrite)] = m_last;
        m_markWrite = (m_markWrite + 1) % int(m_marks.size());
    }
}

void PitchShifter::placeGrains(double ratio)
{
    while (m_next + m_lookahead < double(m_written)) {
        const int64_t s = int64_t(m_next + 0.5);
        const Mark *mark = nullptr;
        int64_t nearest = 0;
        for (const Mark &m : m_marks) {
            const int64_t distance = m.at > s ? m.at - s : s - m.at;
            if (m.period > 0 && (!mark || distance < nearest)) {
                mark = &m;
                nearest = distance;
            }
        }
        if (!mark) {
            m_next += m_unvoiced;
            continue;
        }
        const int half = mark->half;
        const double hop = mark->voiced ? std::max(8.0, mark->period / ratio)
                                        : double(mark->period);
        // Hann grains a hop apart overlap to half / hop, whether they repeat
        // periods (a higher voice) or leave room between them (a lower one);
        // the square root keeps the voice's power either way.
        const double gain = mark->voiced ? std::sqrt(hop / half) : 1;
        for (int t = 1 - half; t < half; ++t) {
            const double w = 0.5 + 0.5 * std::cos(kPi * t / half);
            m_out[size_t((s + t) & m_mask)] += gain * w * at(mark->at + t);
        }
        m_next += hop;
    }
}

double PitchShifter::process(double sample, double period, double ratio)
{
    m_in[size_t(m_written & m_mask)] = sample;
    ++m_written;
    addMarks(period);
    placeGrains(ratio);
    const int64_t t = m_written - 1 - m_delay;
    if (t < 0)
        return 0;
    double &slot = m_out[size_t(t & m_mask)];
    const double shifted = slot;
    slot = 0;
    // With nothing to move, the plain delayed input (a 10 ms crossfade), so
    // the unchanged voice is bit-for-bit the input.
    const double wanted = std::abs(ratio - 1) > 2e-3 ? 1 : 0;
    const double step = 1.0 / (2 * m_unvoiced);
    m_shifting = wanted > m_shifting ? std::min(wanted, m_shifting + step)
                                     : std::max(wanted, m_shifting - step);
    return m_shifting * shifted + (1 - m_shifting) * at(t);
}

// --- keeping a playback queue steady ------------------------------------------

RateMatcher::RateMatcher(double inRate, double outRate, double targetMs)
    : m_inRate(inRate), m_outRate(outRate),
      m_target(std::max(1, int(outRate * targetMs / 1000))),
      m_ceiling(m_target + int(outRate * 0.15)), m_history(1, 0.f),
      m_rampStep(1 / (0.005 * outRate))
{
}

void RateMatcher::pad(int samples, std::vector<float> &out)
{
    out.insert(out.end(), size_t(std::max(0, samples)), 0.f);
    // What follows the silence fades in rather than starting mid-wave.
    m_gain = 0;
    m_gainTarget = 1;
}

void RateMatcher::process(const float *input, int count, int queued,
                          std::vector<float> &out)
{
    out.clear();
    if (queued >= 0) {
        const double seconds = count / m_inRate;
        if (m_dropping) {
            // Cutting back: input is thrown away (behind a fade) until the
            // player has worked the queue down to the target, however the
            // backlog arrives.
            if (queued <= m_target) {
                m_dropping = false;
                m_gainTarget = 1;
                m_level = queued;
            }
        } else if (!m_primed || queued == 0) {
            // First write, or the player ran dry and already played a gap:
            // refill to the target once, so the gap does not repeat every
            // cycle from here on.
            if (m_primed)
                ++m_underruns;
            m_primed = true;
            pad(m_target - queued, out);
            m_level = m_target;
        } else if (queued > m_ceiling) {
            ++m_drops;
            m_dropping = true;
            m_gainTarget = 0;
        } else {
            m_level += 0.05 * (queued - m_level);
            // A PI loop on the queue depth: the integral settles on the
            // clocks' real drift, the proportional part takes up what is left.
            const double error = clamp((m_target - m_level) / m_target, -1, 1);
            m_integral = clamp(m_integral + 0.002 * error * seconds, -0.008, 0.008);
            m_correction = clamp(0.005 * error + m_integral, -0.01, 0.01);
        }
    }
    // Input samples per output sample; a positive correction plays slower and
    // so makes more output.
    const double step = m_inRate / m_outRate / (1 + m_correction);
    m_history.insert(m_history.end(), input, input + count);
    out.reserve(out.size() + size_t(count / step) + 4);
    while (m_position + 2 < double(m_history.size())) {
        if (m_dropping && m_gain <= 0) {
            m_position = double(m_history.size()) - 2;
            break;
        }
        const int i = int(m_position);
        const double t = m_position - i;
        // Four-point cubic Hermite: flat enough across the voice band that a
        // slowly sliding fractional position does not ripple the highs.
        const double y0 = m_history[i - 1], y1 = m_history[i];
        const double y2 = m_history[i + 1], y3 = m_history[i + 2];
        const double c1 = 0.5 * (y2 - y0);
        const double c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
        const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        const double value = ((c3 * t + c2) * t + c1) * t + y1;
        if (m_gain < m_gainTarget)
            m_gain = std::min(m_gainTarget, m_gain + m_rampStep);
        else if (m_gain > m_gainTarget)
            m_gain = std::max(m_gainTarget, m_gain - m_rampStep);
        out.push_back(float(value * m_gain));
        m_position += step;
    }
    // Keep one sample before the read position for the next interpolation.
    const int keep = std::max(0, int(m_position) - 1);
    m_history.erase(m_history.begin(), m_history.begin() + keep);
    m_position -= keep;
}

// --- learning a voice ------------------------------------------------------------

namespace {

struct Envelope {
    std::vector<double> log;
    double binHz = 0;
    double pitchHz = 0;
    int frames = 0;
};

Envelope averageEnvelope(const std::vector<float> &samples, double sampleRate)
{
    Envelope result;
    const int n = sampleRate > 32000 ? 1024 : 512;
    const int hop = n / 4;
    const int half = n / 2;
    const int lifter = std::max(12, int(roundHalfUp(sampleRate / 650)));
    Fft fft(n);
    std::vector<double> window(n), re(n), im(n);
    for (int i = 0; i < n; ++i)
        window[i] = 0.5 - 0.5 * std::cos(kTwoPi * i / n);
    result.log.assign(half + 1, 0);
    for (size_t start = 0; start + n <= samples.size(); start += hop) {
        double energy = 0;
        for (int i = 0; i < n; ++i) {
            const double v = samples[start + i] * window[i];
            re[i] = v;
            im[i] = 0;
            energy += v * v;
        }
        if (std::sqrt(energy / n) < 0.004)
            continue;
        fft.transform(re.data(), im.data(), false);
        for (int k = 0; k <= half; ++k) {
            re[k] = std::log(std::hypot(re[k], im[k]) + kEps);
            im[k] = 0;
            if (k > 0 && k < half) {
                re[n - k] = re[k];
                im[n - k] = 0;
            }
        }
        fft.transform(re.data(), im.data(), true);
        for (int q = 0; q < n; ++q) {
            if (!(q < lifter || q > n - lifter))
                re[q] = 0;
            im[q] = 0;
        }
        fft.transform(re.data(), im.data(), false);
        for (int k = 0; k <= half; ++k)
            result.log[k] += re[k];
        ++result.frames;
    }
    if (!result.frames)
        return result;
    for (double &value : result.log)
        value /= result.frames;
    result.binHz = sampleRate / n;

    // Pitch on the same ~12 kHz decimated signal the live tracker hears.
    const int step = std::max(1, int(roundHalfUp(sampleRate / 12000)));
    std::vector<double> decimated(samples.size() / step);
    for (size_t i = 0; i < decimated.size(); ++i) {
        double total = 0;
        for (int j = 0; j < step; ++j)
            total += samples[i * step + j];
        decimated[i] = total / step;
    }
    PitchDetector detector(sampleRate / step, 1024);
    std::vector<double> pitches;
    for (size_t start = 0; start + 1024 <= decimated.size(); start += 256) {
        const double hz = detector.detect(decimated.data() + start);
        if (hz >= 60 && hz <= 420)
            pitches.push_back(hz);
    }
    std::sort(pitches.begin(), pitches.end());
    result.pitchHz = pitches.size() >= 5 ? pitches[pitches.size() / 2] : 0;
    return result;
}

double sampleEnvelope(const std::vector<double> &env, double bin)
{
    const int low = int(std::floor(bin));
    if (low + 1 >= int(env.size()))
        return env.back();
    return env[low] + (env[low + 1] - env[low]) * (bin - low);
}

} // namespace

LearnedVoice learnVoice(const std::vector<float> &speaker,
                        const std::vector<float> &target, double sampleRate)
{
    LearnedVoice learned;
    const Envelope mine = averageEnvelope(speaker, sampleRate);
    const Envelope theirs = averageEnvelope(target, sampleRate);
    if (!mine.frames || !theirs.frames || theirs.pitchHz <= 0)
        return learned;
    const int lowBin = int(std::ceil(300 / mine.binHz));
    const int highBin = int(std::floor(4500 / mine.binHz));
    const int count = highBin - lowBin + 1;
    std::vector<double> xs(count), diff(count);
    double xm = 0;
    for (int i = 0; i < count; ++i) {
        xs[i] = std::log2((lowBin + i) * mine.binHz / 1000);
        xm += xs[i];
    }
    xm /= count;
    // Search the warp (and a linear log-tilt) that minimises the mean-removed
    // log-envelope distance between the warped speaker and the target.
    double bestError = 1e300;
    double bestFormant = 1;
    double bestTilt = 0;
    for (int step = 0; step <= 75; ++step) {
        const double formant = 0.7 + step * 0.01;
        double mean = 0;
        for (int i = 0; i < count; ++i) {
            const int k = lowBin + i;
            diff[i] = theirs.log[k] - sampleEnvelope(mine.log, k / formant);
            mean += diff[i];
        }
        mean /= count;
        double sxx = 0;
        double sxy = 0;
        for (int i = 0; i < count; ++i) {
            sxx += (xs[i] - xm) * (xs[i] - xm);
            sxy += (xs[i] - xm) * (diff[i] - mean);
        }
        const double slope = sxx > 0 ? sxy / sxx : 0;
        double error = 0;
        for (int i = 0; i < count; ++i) {
            const double residual = diff[i] - mean - slope * (xs[i] - xm);
            error += residual * residual;
        }
        error /= count;
        if (error < bestError) {
            bestError = error;
            bestFormant = formant;
            bestTilt = slope;
        }
    }
    // The slope is natural-log units per octave; brightness 1 = 6 dB/octave.
    VoiceParams params;
    params.targetPitchHz = roundHalfUp(theirs.pitchHz);
    params.formant = roundHalfUp(bestFormant * 100) / 100;
    params.brightness =
        roundHalfUp(clamp(bestTilt * 20 / std::log(10.0) / 6, -1, 1) * 100) / 100;
    learned.ok = true;
    learned.params = params.clean();
    learned.speakerPitchHz = int(roundHalfUp(mine.pitchHz));
    learned.fit = bestError;
    return learned;
}

} // namespace forkmesh::voice
