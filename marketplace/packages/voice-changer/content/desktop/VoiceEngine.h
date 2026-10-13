#pragma once

// ForkMesh Voice Changer engine: turns one microphone into another voice.
//
// Speech is a source (the vocal folds: a pitch and its harmonics) shaped by a
// filter (the throat and mouth: the formants that make a voice sound like a
// particular person). Every STFT frame is split into the two with cepstral
// smoothing and the filter is stretched on its own by the formant ratio;
// harmonics stay where they are, so that stage is a clean, slowly changing
// equaliser. The pitch then moves in the time domain (PitchShifter): single
// glottal periods of the waveform are laid down again closer together or
// further apart. Every period keeps its own shape, so the voice stays one
// voice instead of the phasey, watery sound of shifting spectrum bins, and
// its formants stay where the first stage put them. Moving pitch without
// moving formants is what separates "another person" from "the same person
// sped up".
//
// The voice model adapts to the speaker: a pitch tracker learns the speaker's
// own average pitch while they talk, and a voice that names a target pitch in
// hertz is reached from wherever the speaker starts. learnVoice() fits a voice
// from recordings: the target's pitch and the frequency warp that best maps
// the speaker's average vocal-tract envelope onto the target's.
//
// Microphones hiss, hum and rumble, and a pitch shifter turns that floor into
// a watery static of its own. With noise reduction on, a high-pass filter
// takes off rumble and DC, and every frame's spectrum is cleaned before the
// split: the noise in each bin is the quietest it has been over the last
// second and a half (words always have gaps that short, so speech is never
// learned as noise, even from someone already talking when it starts) and is
// removed with a smoothed Wiener gain, down to a floor, so what is left
// sounds like quieter noise rather than warbling tones or gated speech.
//
// Plain C++ with no Qt, so the package test builds it with nothing else. The
// engine is not thread-safe: one thread calls process(); parameter changes
// from elsewhere go through the owner (VoiceAudioPipeline) first.

#include <cstdint>
#include <string>
#include <vector>

namespace forkmesh::voice {

// A voice. Every field is clamped by clean() to the engine's bounds, so a
// stored or AI-designed voice can never push the engine out of range.
struct VoiceParams {
    double targetPitchHz = 0;  // 0 keeps the speaker's pitch, then shifts
    double semitones = 0;      // -12 .. 12
    double formant = 1;        // vocal-tract ratio: 0.7 large .. 1.45 small
    double breath = 0;         // 0 .. 1
    double whisper = 0;        // 0 .. 1, replaces the voiced tone
    double brightness = 0;     // -1 dark .. 1 bright (about 6 dB/octave)
    double roughness = 0;      // 0 .. 1 gravel: pitch and level wobble
    double gainDb = 0;         // -12 .. 12
    double mix = 1;            // 0 dry .. 1 fully changed

    VoiceParams clean() const;
};

struct ParamBounds {
    const char *name;
    double fallback;
    double low;
    double high;
};
// In the order VoiceParams declares them; the Worker's PARAMS must agree.
const std::vector<ParamBounds> &paramBounds();
double paramValue(const VoiceParams &params, const std::string &name);
void setParamValue(VoiceParams &params, const std::string &name, double value);

struct VoicePreset {
    const char *id;
    const char *name;
    VoiceParams params;
};
const std::vector<VoicePreset> &presets();

class Fft
{
public:
    explicit Fft(int size);
    int size() const { return m_size; }
    void transform(double *re, double *im, bool inverse) const;

private:
    int m_size;
    std::vector<double> m_cos;
    std::vector<double> m_sin;
    std::vector<uint32_t> m_reverse;
};

// Normalised square difference (McLeod) pitch detector.
class PitchDetector
{
public:
    PitchDetector(double sampleRate, int size = 1024);
    // Returns the pitch in Hz, or 0 for silence and unvoiced sound.
    double detect(const double *samples);
    int size() const { return m_size; }

private:
    double m_sampleRate;
    int m_size;
    Fft m_fft;
    std::vector<double> m_re;
    std::vector<double> m_im;
    std::vector<double> m_nsdf;
    int m_minLag;
    int m_maxLag;
};

// Pitch-synchronous overlap-add. Analysis marks are laid one pitch period
// apart along the input, each nudged to where the waveform best repeats the
// one before; a two-period grain (Hann-windowed, centred on a mark) is then
// copied to every synthesis mark, and those are one period / ratio apart. The
// spectral envelope of each grain is the speaker's, so formants do not move
// with the pitch. Unvoiced sound (and ratio 1) is cut into short grains that
// fall back exactly where they came from, which is a clean delay.
class PitchShifter
{
public:
    explicit PitchShifter(double sampleRate);
    // period: the voice's pitch period in samples around the newest sample,
    // or 0 while it is not voiced; ratio: output pitch / input pitch.
    double process(double sample, double period, double ratio);
    int latency() const { return m_delay; }

private:
    struct Mark {
        int64_t at = 0;
        int period = 0;
        int half = 0;
        bool voiced = false;
    };
    double at(int64_t index) const { return m_in[size_t(index & m_mask)]; }
    void addMarks(double period);
    void placeGrains(double ratio);

    int m_mask;
    int m_maxHalf;
    int m_unvoiced;
    int m_lookahead;
    int m_delay;
    std::vector<double> m_in;
    std::vector<double> m_out;
    std::vector<Mark> m_marks;
    int m_markWrite = 0;
    int64_t m_written = 0;
    Mark m_last;
    double m_next = 0;
    double m_shifting = 0;
};

struct VoiceStats {
    int userPitchHz = 0;
    int pitchHz = 0;
    bool voiced = false;
    double ratio = 1;
    double level = 0;
    int latencyMs = 0;
};

class VoiceEngine
{
public:
    explicit VoiceEngine(double sampleRate, double userPitchHz = 0);

    void setParams(const VoiceParams &params);
    const VoiceParams &params() const { return m_params; }
    void setUserPitch(double hz);
    void setBypass(bool bypass) { m_bypass = bypass; }
    // 0 off (the engine is a clean delay) .. 1 full background-noise removal.
    void setNoiseReduction(double amount);
    double noiseReduction() const { return m_denoise; }
    // Changes count samples; input and output may be the same buffer.
    void process(const float *input, float *output, int count);
    VoiceStats stats() const;
    int latencySamples() const { return m_tail + m_shifter.latency(); }

private:
    void trackPitch(double sample);
    void analysePitch();
    double targetRatio() const;
    double random();
    void levelMatch();
    void denoise();
    void frame();

    double m_sampleRate;
    int m_size;
    int m_hop;
    int m_half;
    int m_oversample = 8;
    Fft m_fft;
    int m_tail;
    std::vector<double> m_window;
    std::vector<double> m_synthesis;
    double m_windowPower = 0;
    double m_olaScale;
    std::vector<double> m_inFifo;
    std::vector<double> m_outFifo;
    std::vector<double> m_accum;
    std::vector<double> m_dry;
    int m_fifoPos;
    int m_dryPos = 0;
    std::vector<double> m_re, m_im, m_cre, m_cim;
    std::vector<double> m_anaMag, m_envelope, m_phaseIn, m_tilt;
    int m_lifter;
    int m_decimate;
    PitchDetector m_pitch;
    std::vector<double> m_pitchRing;
    std::vector<double> m_pitchScratch;
    int m_pitchWrite = 0;
    double m_decAcc = 0;
    int m_decCount = 0;
    int m_framesSincePitch = 0;
    double m_userLogPitch;
    bool m_voiced = false;
    double m_lastPitchHz = 0;
    double m_recentPitch[3] = {0, 0, 0};
    int m_voicedHold = 0;
    double m_shiftPitchHz = 0;
    PitchShifter m_shifter;
    double m_ratio = 1;
    double m_level = 0;
    double m_agc = 1;
    double m_limit = 1;
    double m_inPower = 0;
    double m_outPower = 0;
    double m_speechEnergy = 0;
    double m_noiseEnergy = 0;
    double m_denoise = 0;
    double m_hpA1 = 0, m_hpA2 = 0, m_hpB0 = 1, m_hpB1 = 0, m_hpB2 = 0;
    double m_hpX1 = 0, m_hpX2 = 0, m_hpY1 = 0, m_hpY2 = 0;
    bool m_noiseReady = false;
    int m_denoiseFrames = 0;
    int m_minFrames = 0;
    int m_minSlot = 0;
    std::vector<double> m_noisePower, m_trackPower, m_minPower, m_cleanPower;
    std::vector<double> m_minHistory;
    double m_wobble = 0;
    double m_wobbleTarget = 0;
    uint32_t m_seed = 0x2545f491u;
    VoiceParams m_params;
    bool m_bypass = false;
};

// Feeds a playback queue this process does not own (the pipe into a sound
// server, an audio sink's buffer) and keeps it at a steady depth. The
// microphone and the playback side rarely share a clock: on a desktop the
// microphone was measured 0.3% slow against the virtual microphone's graph,
// so a fixed cushion drained in seconds and then every cycle played a gap,
// heard as loud static. Here a small resampling correction (at most 1%, a few
// cents of pitch) follows the drift; a queue that does run dry is refilled
// once with silence and faded back in, instead of crackling every cycle; and a
// queue grown long after a stall is cut back behind a short fade, so the call
// never hears the voice ever later. Also converts between sample rates.
class RateMatcher
{
public:
    RateMatcher(double inRate, double outRate, double targetMs = 30);
    // queued: output-rate samples still waiting to be played just before this
    // write, or -1 when the queue cannot be measured (plain conversion).
    // Replaces out with what to write.
    void process(const float *input, int count, int queued, std::vector<float> &out);
    double correction() const { return m_correction; }
    int underruns() const { return m_underruns; }
    int drops() const { return m_drops; }
    int targetSamples() const { return m_target; }
    // Beyond this many queued samples the queue is cut back to the target.
    int ceilingSamples() const { return m_ceiling; }

private:
    void pad(int samples, std::vector<float> &out);

    double m_inRate;
    double m_outRate;
    int m_target;
    int m_ceiling;
    std::vector<float> m_history;
    double m_position = 1;
    double m_level = 0;
    double m_integral = 0;
    double m_correction = 0;
    double m_gain = 1;
    double m_gainTarget = 1;
    double m_rampStep;
    bool m_primed = false;
    bool m_dropping = false;
    int m_underruns = 0;
    int m_drops = 0;
};

struct LearnedVoice {
    bool ok = false;
    VoiceParams params;
    int speakerPitchHz = 0;
    double fit = 0;
};

// Fit a voice from mono recordings of the speaker and of the target, both at
// sampleRate and a few seconds of ordinary speech each.
LearnedVoice learnVoice(const std::vector<float> &speaker,
                        const std::vector<float> &target, double sampleRate);

} // namespace forkmesh::voice
