// Drives VoiceEngine on synthetic speech: a glottal pulse train at a pitch,
// shaped by three formant resonators, which is close enough to speech for the
// pitch tracker, the envelope split and the voice fit to be measured.
//
// Built as the package's desktop test target, and by
// tests/test_voice_changer.py with a bare g++ (no Qt needed). It checks
// itself and exits non-zero on a failure; with --json it also prints what it
// measured so the pytest can report the numbers.

#include "../VoiceEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace forkmesh::voice;

namespace {

constexpr double kRate = 48000;
int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<float> speech(double f0, double seconds, std::vector<double> formants)
{
    const size_t n = size_t(kRate * seconds);
    std::vector<float> out(n);
    struct Res { double a1, a2, y1 = 0, y2 = 0; };
    std::vector<Res> bank;
    for (size_t i = 0; i < formants.size(); ++i) {
        const double r = std::exp(-M_PI * (80 + 40 * double(i)) / kRate);
        bank.push_back({2 * r * std::cos(2 * M_PI * formants[i] / kRate), -r * r});
    }
    double phase = 0;
    double peak = 0;
    for (size_t i = 0; i < n; ++i) {
        phase += f0 * (1 + 0.03 * std::sin(2 * M_PI * 3 * double(i) / kRate)) / kRate;
        double pulse = 0;
        if (phase >= 1) {
            phase -= 1;
            pulse = 1;
        }
        double y = 0;
        for (Res &f : bank) {
            const double v = pulse + f.a1 * f.y1 + f.a2 * f.y2;
            f.y2 = f.y1;
            f.y1 = v;
            y += v;
        }
        out[i] = float(y);
        peak = std::max(peak, std::abs(y));
    }
    for (float &v : out)
        v = float(v * 0.3 / peak);
    return out;
}

std::vector<float> run(VoiceEngine &engine, const std::vector<float> &input)
{
    std::vector<float> out(input.size());
    for (size_t i = 0; i + 128 <= input.size(); i += 128)
        engine.process(input.data() + i, out.data() + i, 128);
    return out;
}

double medianPitch(const std::vector<float> &signal)
{
    const int step = 4;
    std::vector<double> dec(signal.size() / step);
    for (size_t i = 0; i < dec.size(); ++i) {
        double s = 0;
        for (int j = 0; j < step; ++j)
            s += signal[i * step + j];
        dec[i] = s / step;
    }
    PitchDetector detector(kRate / step, 1024);
    std::vector<double> found;
    for (size_t s = 6000; s + 1024 <= dec.size(); s += 256) {
        const double hz = detector.detect(dec.data() + s);
        if (hz > 0)
            found.push_back(hz);
    }
    if (found.empty())
        return 0;
    std::sort(found.begin(), found.end());
    return found[found.size() / 2];
}

// How cleanly periodic a voice is: the normalised autocorrelation at its
// best lag near periodSamples, averaged over 40 ms windows. A clean voice
// scores near 1; a smeared, phasey one far less.
double clarity(const std::vector<float> &signal, size_t from, double periodSamples)
{
    const int window = 1920;
    double total = 0;
    int count = 0;
    for (size_t s = from; s + window + 2 * size_t(periodSamples) < signal.size(); s += window) {
        double best = 0;
        for (int lag = int(periodSamples * 0.8); lag <= int(periodSamples * 1.25); ++lag) {
            double dot = 0, a = 0, b = 0;
            for (int i = 0; i < window; ++i) {
                const double x = signal[s + size_t(i)], y = signal[s + size_t(i + lag)];
                dot += x * y;
                a += x * x;
                b += y * y;
            }
            best = std::max(best, dot / std::sqrt(a * b + 1e-20));
        }
        total += best;
        ++count;
    }
    return count ? total / count : 0;
}

double rms(const std::vector<float> &signal, size_t from)
{
    double sum = 0;
    for (size_t i = from; i < signal.size(); ++i)
        sum += double(signal[i]) * signal[i];
    return std::sqrt(sum / double(signal.size() - from));
}

double rmsRange(const std::vector<float> &signal, size_t from, size_t to)
{
    double sum = 0;
    for (size_t i = from; i < to; ++i)
        sum += double(signal[i]) * signal[i];
    return std::sqrt(sum / double(to - from));
}

// A microphone: speech that pauses, over a hiss and a mains hum.
std::vector<float> noisyMicrophone(const std::vector<float> &speech, double noiseRms)
{
    std::vector<float> out(speech.size());
    uint32_t seed = 12345;
    for (size_t i = 0; i < out.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double white = (seed / 4294967296.0 - 0.5) * std::sqrt(12.0);
        const double hum = std::sqrt(2.0) * std::sin(2 * M_PI * 60 * double(i) / kRate);
        out[i] = float(speech[i] + noiseRms * (white + 0.5 * hum));
    }
    return out;
}

struct QueueRun {
    int gaps = 0;          // player cycles that found the queue short, after warm-up
    int underruns = 0;
    int drops = 0;
    double correction = 0;
    double maxQueueMs = 0;
};

// The microphone delivers 1024-sample blocks on its own clock (drift: 0.003 =
// 0.3% slow); the player takes 256-sample cycles on the other. A stall stops
// the microphone side for stallMs at 10 s, then everything it held arrives.
QueueRun simulateQueue(double drift, double stallMs)
{
    RateMatcher matcher(kRate, kRate);
    QueueRun run;
    const double seconds = 40;
    const double blockPeriod = 1024 / (kRate * (1 - drift));
    const double cyclePeriod = 256 / kRate;
    std::vector<float> block(1024, 0.1f), out;
    double nextBlock = 0.01;
    double nextCycle = 0;
    double queue = 0;
    while (nextCycle < seconds) {
        const double stallEnd = 10 + stallMs / 1000;
        // Blocks due during the stall wait, then all arrive at its end.
        const bool held = stallMs > 0 && nextBlock >= 10 && nextBlock < stallEnd &&
                          nextCycle < stallEnd;
        if (nextBlock <= nextCycle && !held) {
            matcher.process(block.data(), 1024, int(queue), out);
            queue += double(out.size());
            nextBlock += blockPeriod;
        } else {
            if (queue < 256 && nextCycle > 2 && !(nextCycle >= 10 && nextCycle < 10.5))
                ++run.gaps;
            queue = std::max(0.0, queue - 256);
            if (nextCycle > 2 && !(nextCycle >= 10 && nextCycle < 10.5))
                run.maxQueueMs = std::max(run.maxQueueMs, queue / kRate * 1000);
            nextCycle += cyclePeriod;
        }
    }
    run.underruns = matcher.underruns();
    run.drops = matcher.drops();
    run.correction = matcher.correction();
    return run;
}

VoiceParams with(const char *name, double value, VoiceParams p = {})
{
    setParamValue(p, name, value);
    return p;
}

} // namespace

int main(int argc, char **argv)
{
    const bool json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
    const std::vector<double> vowel = {700, 1220, 2600};
    const std::vector<float> input = speech(120, 4, vowel);

    // With nothing to change the engine is a pure delay.
    VoiceEngine plain(kRate);
    const std::vector<float> same = run(plain, input);
    const int latency = plain.latencySamples();
    double dot = 0, a = 0, b = 0;
    for (size_t i = size_t(kRate); i < input.size() - 4000; ++i) {
        dot += double(same[i]) * input[i - latency];
        a += double(input[i - latency]) * input[i - latency];
        b += double(same[i]) * same[i];
    }
    const double identityCorrelation = dot / std::sqrt(a * b);
    const double identityGain = dot / a;

    VoiceEngine down(kRate);
    down.setParams(with("semitones", -5));
    const std::vector<float> lowered = run(down, input);
    VoiceEngine target(kRate);
    target.setParams(with("formant", 1.2, with("targetPitchHz", 220)));
    const std::vector<float> reached = run(target, input);

    std::string presetJson;
    bool presetsFinite = true;
    double baritone = 0, soprano = 0, whisper = -1;
    for (const VoicePreset &preset : presets()) {
        VoiceEngine engine(kRate);
        engine.setParams(preset.params);
        const std::vector<float> out = run(engine, input);
        bool finite = true;
        for (float v : out)
            finite = finite && std::isfinite(v) && std::abs(v) < 1.5f;
        presetsFinite = presetsFinite && finite;
        const double pitch = medianPitch(out);
        if (!std::strcmp(preset.id, "deep-baritone")) baritone = pitch;
        if (!std::strcmp(preset.id, "clear-soprano")) soprano = pitch;
        if (!std::strcmp(preset.id, "whisper")) whisper = pitch;
        char row[160];
        std::snprintf(row, sizeof row, "%s\"%s\":{\"finite\":%s,\"pitch\":%d}",
                      presetJson.empty() ? "" : ",", preset.id,
                      finite ? "true" : "false", int(std::lround(pitch)));
        presetJson += row;
    }

    VoiceEngine busy(kRate);
    VoiceParams heavy = with("targetPitchHz", 200);
    heavy.formant = 1.15;
    heavy.breath = 0.2;
    heavy.roughness = 0.3;
    busy.setParams(heavy);
    const auto started = std::chrono::steady_clock::now();
    run(busy, input);
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    std::vector<double> theirs;
    for (double hz : vowel)
        theirs.push_back(hz * 1.18);
    const LearnedVoice fitted =
        learnVoice(speech(120, 3, vowel), speech(230, 3, theirs), kRate);

    // Background noise: speech with a two-second pause, over hiss and hum.
    std::vector<float> talk = speech(120, 6, vowel);
    for (size_t i = size_t(kRate * 2); i < size_t(kRate * 4); ++i)
        talk[i] = 0;
    const std::vector<float> mic = noisyMicrophone(talk, 0.003);
    VoiceParams shifted = with("semitones", -4);
    VoiceEngine noisy(kRate);
    noisy.setParams(shifted);
    const std::vector<float> kept = run(noisy, mic);
    VoiceEngine cleaned(kRate);
    cleaned.setParams(shifted);
    cleaned.setNoiseReduction(1);
    const std::vector<float> clean = run(cleaned, mic);
    // The pause's last second (after the engine's latency), and speech after it.
    const size_t pauseFrom = size_t(kRate * 3) + size_t(latency), pauseTo = size_t(kRate * 4);
    const size_t talkFrom = size_t(kRate * 4.5) + size_t(latency), talkTo = size_t(kRate * 5.8);
    const double noiseCutDb = 20 * std::log10(rmsRange(kept, pauseFrom, pauseTo) /
                                              rmsRange(clean, pauseFrom, pauseTo));
    const double cleanSpeechLevel = rmsRange(clean, talkFrom, talkTo) /
                                    rmsRange(kept, talkFrom, talkTo);
    const std::vector<float> cleanTail(clean.begin() + long(talkFrom), clean.end());
    const double cleanPitch = medianPitch(cleanTail);

    // Someone already talking (300 ms syllables, 100 ms gaps) in a quiet room
    // when noise reduction starts: their voice must not be learned as the
    // noise and cut out.
    std::vector<float> syllables = speech(120, 4, vowel);
    for (size_t i = 0; i < syllables.size(); ++i) {
        if (i % size_t(kRate * 0.4) >= size_t(kRate * 0.3))
            syllables[i] = 0;
    }
    VoiceEngine talking(kRate);
    talking.setNoiseReduction(1);
    const std::vector<float> heard = run(talking, syllables);
    const double talkingLevel =
        rmsRange(heard, size_t(kRate * 0.2) + size_t(latency), size_t(kRate * 3.9)) /
        rmsRange(syllables, size_t(kRate * 0.2), size_t(kRate * 3.9) - size_t(latency));

    const QueueRun drifting = simulateQueue(0.003, 0);
    const QueueRun steady = simulateQueue(0, 0);
    const QueueRun stalled = simulateQueue(0.003, 300);
    RateMatcher convert(44100, 48000);
    std::vector<float> converted, piece;
    const std::vector<float> ramp(44100, 0.25f);
    for (size_t i = 0; i < ramp.size(); i += 441) {
        convert.process(ramp.data() + i, 441, -1, piece);
        converted.insert(converted.end(), piece.begin(), piece.end());
    }

    VoiceParams wild;
    wild.formant = 9;
    wild.semitones = -40;
    wild.targetPitchHz = 20;
    wild.gainDb = NAN;
    const VoiceParams cleanedParams = wild.clean();

    const double inputPitch = medianPitch(input);
    const double downPitch = medianPitch(lowered);
    const double downLevel = rms(lowered, size_t(kRate)) / rms(input, size_t(kRate));
    const double targetPitch = medianPitch(reached);
    const double expectedDown = 120 * std::pow(2, -5.0 / 12);
    const double downClarity = clarity(lowered, size_t(kRate), kRate / expectedDown);
    const double inputClarity = clarity(input, size_t(kRate), kRate / 120);

    check(latency > 0 && latency <= 2400, "under 50 ms of latency at 48 kHz");
    check(identityCorrelation > 0.9999, "identity is a clean delay");
    check(std::abs(identityGain - 1) < 0.01, "identity keeps the level");
    check(std::abs(inputPitch - 120) < 2, "input pitch reads 120 Hz");
    check(std::abs(downPitch - expectedDown) < 0.03 * expectedDown, "-5 semitones");
    check(downLevel > 0.8 && downLevel < 1.25, "shifted voice keeps its level");
    check(downClarity > 0.9, "the lowered voice is as clean a tone as the input");
    check(std::abs(target.stats().userPitchHz - 120) <= 3, "speaker pitch learned");
    check(std::abs(targetPitch - 220) < 0.06 * 220, "target pitch reached");
    check(presetsFinite, "every preset stays finite and in range");
    check(baritone > 0 && baritone < 100, "deep baritone is low");
    check(soprano > 220, "soprano is high");
    check(whisper == 0, "whisper has no voiced tone");
    check(elapsedMs / 4000 < 0.25, "runs well inside real time");
    check(fitted.ok && std::abs(fitted.params.targetPitchHz - 230) <= 6, "fitted pitch");
    check(std::abs(fitted.params.formant - 1.18) <= 0.03, "fitted vocal tract");
    check(std::abs(fitted.speakerPitchHz - 120) <= 3, "fitted speaker pitch");
    check(cleanedParams.formant == 1.45 && cleanedParams.semitones == -12 &&
          cleanedParams.targetPitchHz == 60 && cleanedParams.gainDb == 0, "clean() clamps");
    check(noiseCutDb > 15, "noise reduction takes the room down");
    check(talkingLevel > 0.9, "noise reduction never learns a voice as the noise");
    check(cleanSpeechLevel > 0.8 && cleanSpeechLevel < 1.25, "and keeps the voice's level");
    const double shiftedPitch = 120 * std::pow(2, -4.0 / 12);
    check(std::abs(cleanPitch - shiftedPitch) < 0.03 * shiftedPitch, "and its pitch");
    check(drifting.gaps == 0 && drifting.underruns == 0, "a drifting clock never runs dry");
    check(std::abs(drifting.correction - 0.003) < 0.0015, "the correction finds the drift");
    check(drifting.maxQueueMs < 90 && steady.gaps == 0, "the queue stays short");
    check(stalled.underruns <= 1 && stalled.drops == 1 && stalled.gaps == 0 &&
          stalled.maxQueueMs < 200,
          "a stall costs one gap, then the queue is cut back");
    check(std::abs(double(converted.size()) - 48000) < 16 &&
          std::abs(converted[24000] - 0.25f) < 1e-4, "rate conversion is exact");

    if (json) {
        std::string bounds;
        for (const ParamBounds &p : paramBounds()) {
            char row[160];
            std::snprintf(row, sizeof row, "%s\"%s\":[%g,%g,%g]",
                          bounds.empty() ? "" : ",", p.name, p.fallback, p.low, p.high);
            bounds += row;
        }
        std::printf(
            "{\"params\":{%s},\"latencySamples\":%d,\"identityCorrelation\":%.9f,"
            "\"identityGain\":%.6f,\"inputPitch\":%.2f,\"downPitch\":%.2f,"
            "\"downLevelRatio\":%.4f,\"downClarity\":%.4f,\"inputClarity\":%.4f,\"targetPitch\":%.2f,\"learnedSpeakerPitch\":%d,"
            "\"presets\":{%s},\"realtimeFactor\":%.4f,\"fitted\":{\"targetPitchHz\":%g,"
            "\"formant\":%g,\"brightness\":%g,\"speakerPitchHz\":%d},"
            "\"noiseCutDb\":%.2f,\"talkingLevel\":%.4f,\"cleanSpeechLevel\":%.4f,\"cleanPitch\":%.2f,"
            "\"queue\":{\"driftGaps\":%d,\"driftUnderruns\":%d,\"driftCorrection\":%.5f,"
            "\"driftMaxMs\":%.1f,\"steadyGaps\":%d,\"stallUnderruns\":%d,"
            "\"stallGaps\":%d,\"stallDrops\":%d,\"stallMaxMs\":%.1f},\"failures\":%d}\n",
            bounds.c_str(), latency, identityCorrelation, identityGain, inputPitch,
            downPitch, downLevel, downClarity, inputClarity, targetPitch, target.stats().userPitchHz,
            presetJson.c_str(), elapsedMs / 4000, fitted.params.targetPitchHz,
            fitted.params.formant, fitted.params.brightness, fitted.speakerPitchHz,
            noiseCutDb, talkingLevel, cleanSpeechLevel, cleanPitch, drifting.gaps, drifting.underruns,
            drifting.correction, drifting.maxQueueMs, steady.gaps, stalled.underruns,
            stalled.gaps, stalled.drops, stalled.maxQueueMs, failures);
    }
    return failures ? 1 : 0;
}
