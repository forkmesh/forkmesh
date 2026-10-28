#include "PokerSynth.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace forkmesh::poker {
namespace {

constexpr double kPi = 3.14159265358979323846;

class Noise
{
public:
    explicit Noise(std::uint32_t seed) : m_state(seed ? seed : 0x9e3779b9u) {}

    float next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return float(double(m_state) / 2147483648.0 - 1.0);
    }

    double uniform() { return (double(next()) + 1.0) / 2.0; }

private:
    std::uint32_t m_state;
};

class Biquad
{
public:
    void bandPass(double frequency, double q, int rate)
    {
        const double w0 = 2 * kPi * frequency / rate;
        const double alpha = std::sin(w0) / (2 * q);
        const double a0 = 1 + alpha;
        m_b0 = alpha / a0;
        m_b1 = 0;
        m_b2 = -alpha / a0;
        m_a1 = -2 * std::cos(w0) / a0;
        m_a2 = (1 - alpha) / a0;
    }

    void lowPass(double frequency, double q, int rate)
    {
        const double w0 = 2 * kPi * frequency / rate;
        const double alpha = std::sin(w0) / (2 * q);
        const double cosine = std::cos(w0);
        const double a0 = 1 + alpha;
        m_b0 = (1 - cosine) / 2 / a0;
        m_b1 = (1 - cosine) / a0;
        m_b2 = (1 - cosine) / 2 / a0;
        m_a1 = -2 * cosine / a0;
        m_a2 = (1 - alpha) / a0;
    }

    float process(float input)
    {
        const double output = m_b0 * input + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;
        m_x2 = m_x1;
        m_x1 = input;
        m_y2 = m_y1;
        m_y1 = output;
        return float(output);
    }

private:
    double m_b0 = 1, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
    double m_x1 = 0, m_x2 = 0, m_y1 = 0, m_y2 = 0;
};

int samplesFor(double seconds, int rate)
{
    return std::max(1, int(std::lround(seconds * rate)));
}

void ensure(std::vector<float> &out, std::size_t size)
{
    if (out.size() < size)
        out.resize(size, 0.0f);
}

void addNoiseBurst(std::vector<float> &out, int rate, double at, double length, double centre, double q,
                   double gain, std::uint32_t seed)
{
    const int start = samplesFor(at, rate);
    const int count = samplesFor(length, rate);
    ensure(out, std::size_t(start + count + rate / 50));
    Noise noise(seed);
    Biquad filter;
    filter.bandPass(centre, q, rate);
    for (int index = 0; index < count + rate / 50; ++index) {
        const double t = double(index) / rate;
        const double envelope = index < count ? std::exp(-t / (length * 0.35)) : 0.0;
        const float input = index < count ? float(noise.next() * envelope) : 0.0f;
        out[std::size_t(start + index)] += float(filter.process(input) * gain * 3.0);
    }
}

void addSine(std::vector<float> &out, int rate, double at, double length, double frequency, double gain,
             double decay, double attack = 0.002)
{
    const int start = samplesFor(at, rate);
    const int count = samplesFor(length, rate);
    ensure(out, std::size_t(start + count));
    for (int index = 0; index < count; ++index) {
        const double t = double(index) / rate;
        double envelope = decay > 0 ? std::exp(-t / decay) : 1.0;
        envelope *= std::min(1.0, t / attack);
        const double tail = double(count - index) / rate;
        envelope *= std::min(1.0, tail / 0.004);
        out[std::size_t(start + index)] += float(std::sin(2 * kPi * frequency * t) * envelope * gain);
    }
}

void addClack(std::vector<float> &out, int rate, double at, double gain, std::uint32_t seed)
{
    addSine(out, rate, at, 0.09, 3200, gain * 0.55, 0.025, 0.0005);
    addSine(out, rate, at, 0.09, 5100, gain * 0.35, 0.025, 0.0005);
    const int start = samplesFor(at, rate);
    const int count = samplesFor(0.005, rate);
    ensure(out, std::size_t(start + count));
    Noise noise(seed);
    for (int index = 0; index < count; ++index) {
        const double envelope = 1.0 - double(index) / count;
        out[std::size_t(start + index)] += float(noise.next() * envelope * gain * 0.6);
    }
}

void addCascade(std::vector<float> &out, int rate, double at, int clacks, double spread, double gain,
                std::uint32_t seed)
{
    Noise jitter(seed);
    double time = at;
    for (int index = 0; index < clacks; ++index) {
        const double fade = 1.0 - 0.5 * double(index) / std::max(1, clacks - 1);
        addClack(out, rate, time, gain * fade * (0.7 + 0.3 * jitter.uniform()), seed + 31u * std::uint32_t(index + 1));
        time += spread * (0.6 + 0.8 * jitter.uniform());
    }
}

std::vector<float> shuffle(int rate)
{
    std::vector<float> out;
    Noise jitter(7);
    for (int tick = 0; tick < 10; ++tick) {
        const double offset = (jitter.uniform() * 2 - 1) * 0.006;
        const double at = 0.01 + tick * 0.032 + offset;
        const double shape = std::sin(kPi * (tick + 0.5) / 10.0);
        addNoiseBurst(out, rate, std::max(0.0, at), 0.012, 3000, 1.4, 0.25 + 0.55 * shape, 101u + std::uint32_t(tick));
    }
    return out;
}

std::vector<float> deal(int rate)
{
    const int count = samplesFor(0.045, rate);
    std::vector<float> out(std::size_t(count + rate / 40), 0.0f);
    Noise noise(23);
    Biquad filter;
    for (int index = 0; index < count; ++index) {
        const double t = double(index) / rate;
        const double progress = t / 0.045;
        const double frequency = 5000.0 * std::pow(1500.0 / 5000.0, progress);
        filter.bandPass(frequency, 1.1, rate);
        const double envelope = std::exp(-t / 0.011);
        out[std::size_t(index)] = float(filter.process(float(noise.next() * envelope)) * 2.4);
    }
    for (std::size_t index = std::size_t(count); index < out.size(); ++index)
        out[index] = filter.process(0.0f) * 2.4f;
    return out;
}

std::vector<float> chip(int rate)
{
    std::vector<float> out;
    for (int clack = 0; clack < 3; ++clack)
        addClack(out, rate, clack * 0.045, 0.75 - clack * 0.12, 51u + std::uint32_t(clack));
    return out;
}

std::vector<float> check(int rate)
{
    std::vector<float> out;
    for (int knock = 0; knock < 2; ++knock) {
        const double at = knock * 0.12;
        addSine(out, rate, at, 0.2, 150, 0.85, 0.06, 0.001);
        addNoiseBurst(out, rate, at, 0.02, 700, 0.8, 0.18, 71u + std::uint32_t(knock));
    }
    return out;
}

std::vector<float> fold(int rate)
{
    const int count = samplesFor(0.18, rate);
    std::vector<float> out(std::size_t(count), 0.0f);
    Noise noise(41);
    Biquad filter;
    filter.lowPass(1200, 0.7, rate);
    for (int index = 0; index < count; ++index) {
        const double progress = double(index) / count;
        const double envelope = std::pow(std::sin(kPi * progress), 1.6);
        out[std::size_t(index)] = float(filter.process(noise.next()) * envelope * 1.3);
    }
    return out;
}

std::vector<float> turn(int rate)
{
    std::vector<float> out;
    addSine(out, rate, 0.0, 0.14, 880, 0.32, 0.0, 0.012);
    addSine(out, rate, 0.14, 0.14, 1320, 0.28, 0.0, 0.012);
    for (std::size_t index = 0; index < out.size(); ++index) {
        const double t = double(index) / rate;
        const double local = std::fmod(t, 0.14) / 0.14;
        out[index] *= float(std::pow(std::sin(kPi * local), 0.6));
    }
    return out;
}

std::vector<float> clockTick(int rate)
{
    std::vector<float> out;
    addSine(out, rate, 0.0, 0.02, 1000, 0.45, 0.006, 0.0005);
    return out;
}

std::vector<float> allIn(int rate)
{
    const int count = samplesFor(0.4, rate);
    std::vector<float> out(std::size_t(count), 0.0f);
    double phase = 0;
    for (int index = 0; index < count; ++index) {
        const double progress = double(index) / count;
        const double frequency = 110.0 * std::pow(2.0, progress);
        phase += 2 * kPi * frequency / rate;
        const double envelope = std::min(1.0, progress / 0.06) * std::min(1.0, (1.0 - progress) / 0.12);
        out[std::size_t(index)] = float(std::sin(phase) * envelope * 0.55);
    }
    addCascade(out, rate, 0.02, 9, 0.042, 0.45, 211);
    return out;
}

std::vector<float> win(int rate)
{
    const double notes[] = {523.25, 659.25, 783.99, 1046.50};
    std::vector<float> out;
    for (int note = 0; note < 4; ++note) {
        const double at = note * 0.11;
        const int start = samplesFor(at, rate);
        const double length = note == 3 ? 0.32 : 0.13;
        const int count = samplesFor(length, rate);
        ensure(out, std::size_t(start + count));
        for (int index = 0; index < count; ++index) {
            const double t = double(index) / rate;
            const double cycle = std::fmod(t * notes[note], 1.0);
            const double triangle = 4.0 * std::fabs(cycle - 0.5) - 1.0;
            const double envelope = std::min(1.0, t / 0.006) * std::exp(-t / (note == 3 ? 0.16 : 0.09));
            out[std::size_t(start + index)] += float(triangle * envelope * 0.34);
        }
    }
    addCascade(out, rate, 0.5, 10, 0.05, 0.42, 307);
    return out;
}

void normalize(std::vector<float> &samples)
{
    const float peak = peakLevel(samples);
    if (peak > 0.92f) {
        const float scale = 0.92f / peak;
        for (float &sample : samples)
            sample *= scale;
    }
}

}

QStringList soundNames()
{
    return {QStringLiteral("shuffle"), QStringLiteral("deal"), QStringLiteral("chip"), QStringLiteral("check"),
            QStringLiteral("fold"), QStringLiteral("turn"), QStringLiteral("clock"), QStringLiteral("allin"),
            QStringLiteral("win")};
}

std::vector<float> synthesize(const QString &name, int sampleRate)
{
    const int rate = std::clamp(sampleRate, 8000, 192000);
    std::vector<float> out;
    if (name == QLatin1String("shuffle"))
        out = shuffle(rate);
    else if (name == QLatin1String("deal"))
        out = deal(rate);
    else if (name == QLatin1String("chip"))
        out = chip(rate);
    else if (name == QLatin1String("check"))
        out = check(rate);
    else if (name == QLatin1String("fold"))
        out = fold(rate);
    else if (name == QLatin1String("turn"))
        out = turn(rate);
    else if (name == QLatin1String("clock"))
        out = clockTick(rate);
    else if (name == QLatin1String("allin"))
        out = allIn(rate);
    else if (name == QLatin1String("win"))
        out = win(rate);
    normalize(out);
    return out;
}

double soundSeconds(const std::vector<float> &samples, int sampleRate)
{
    return sampleRate > 0 ? double(samples.size()) / sampleRate : 0.0;
}

float peakLevel(const std::vector<float> &samples)
{
    float peak = 0;
    for (const float sample : samples)
        peak = std::max(peak, std::fabs(sample));
    return peak;
}

QByteArray pcm16(const std::vector<float> &samples, int channels)
{
    const int width = std::max(1, channels);
    QByteArray out(qsizetype(samples.size() * std::size_t(width) * sizeof(qint16)), Qt::Uninitialized);
    auto *data = reinterpret_cast<qint16 *>(out.data());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const qint16 value = qint16(std::lround(std::clamp(samples[index], -1.0f, 1.0f) * 32767.0f));
        for (int channel = 0; channel < width; ++channel)
            data[index * std::size_t(width) + std::size_t(channel)] = value;
    }
    return out;
}

QByteArray pcmFloat(const std::vector<float> &samples, int channels)
{
    const int width = std::max(1, channels);
    QByteArray out(qsizetype(samples.size() * std::size_t(width) * sizeof(float)), Qt::Uninitialized);
    auto *data = reinterpret_cast<float *>(out.data());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float value = std::clamp(samples[index], -1.0f, 1.0f);
        for (int channel = 0; channel < width; ++channel)
            data[index * std::size_t(width) + std::size_t(channel)] = value;
    }
    return out;
}

}
