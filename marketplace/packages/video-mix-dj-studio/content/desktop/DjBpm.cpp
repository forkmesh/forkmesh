#include "DjBpm.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace forkmesh::djstudio::bpm {

namespace {

constexpr int multipleCount = int(std::size(multiples));

double value(const QVector<double> &acf, double lag)
{
    const int base = int(lag);
    if (!(lag >= 0) || base + 1 >= acf.size())
        return 0;
    const double fraction = lag - double(base);
    return acf.at(base) * (1 - fraction) + acf.at(base + 1) * fraction;
}

double score(const QVector<double> &acf, double period)
{
    double total = 0;
    for (const double multiple : multiples)
        total += value(acf, period * multiple);
    return total / double(multipleCount);
}

double prior(double tempo)
{
    const double octaves = std::log2(tempo / favourite) / spread;
    return std::exp(-0.5 * octaves * octaves);
}

double refine(const QVector<double> &acf, double period)
{
    double multiple = 1;
    while (period * multiple * 2 + 3 < double(acf.size()))
        multiple *= 2;
    const int centre = int(std::round(period * multiple));
    if (centre < 3 || centre + 3 >= acf.size())
        return period;
    int peak = centre;
    for (int lag = centre - 2; lag <= centre + 2; ++lag) {
        if (acf.at(lag) > acf.at(peak))
            peak = lag;
    }
    const double left = acf.at(peak - 1);
    const double middle = acf.at(peak);
    const double right = acf.at(peak + 1);
    const double curve = left - 2 * middle + right;
    const double shift = curve < 0 ? std::min(0.5, std::max(-0.5, 0.5 * (left - right) / curve)) : 0;
    const double refined = (double(peak) + shift) / multiple;
    return std::abs(refined - period) < period * 0.02 ? refined : period;
}

QVector<double> onsets(const QVector<qint16> &samples, double rate, int step)
{
    const qsizetype span = std::min<qsizetype>(samples.size(), qsizetype(longest * rate));
    const qsizetype start = (samples.size() - span) / 2;
    const int frames = int(span / step);
    if (frames <= 2)
        return {};
    const double smoothing = 1 - std::exp(-2 * M_PI * 180 / rate);
    double low = 0;
    QVector<double> full(frames, 0);
    QVector<double> bass(frames, 0);
    const qint16 *pcm = samples.constData();
    for (int frame = 0; frame < frames; ++frame) {
        double fullEnergy = 0;
        double bassEnergy = 0;
        const qsizetype offset = start + qsizetype(frame) * step;
        for (qsizetype index = offset; index < offset + step; ++index) {
            const double sample = double(pcm[index]) / 32768;
            low += smoothing * (sample - low);
            fullEnergy += sample * sample;
            bassEnergy += low * low;
        }
        full[frame] = std::log(1 + 1000 * fullEnergy / double(step));
        bass[frame] = std::log(1 + 1000 * bassEnergy / double(step));
    }
    QVector<double> flux(frames, 0);
    for (int frame = 1; frame < frames; ++frame)
        flux[frame] = std::max(0.0, full.at(frame) - full.at(frame - 1)) + std::max(0.0, bass.at(frame) - bass.at(frame - 1));
    const int reach = std::max(1, int(0.2 * rate / double(step)));
    QVector<double> running(frames + 1, 0);
    for (int frame = 0; frame < frames; ++frame)
        running[frame + 1] = running.at(frame) + flux.at(frame);
    QVector<double> result(frames, 0);
    double total = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const int from = std::max(0, frame - reach);
        const int to = std::min(frames, frame + reach + 1);
        result[frame] = std::max(0.0, flux.at(frame) - (running.at(to) - running.at(from)) / double(to - from));
        total += result.at(frame);
    }
    const double mean = total / double(frames);
    for (double &item : result)
        item -= mean;
    return result;
}

QVector<double> autocorrelation(const QVector<double> &values, int lags)
{
    const int count = int(values.size());
    QVector<double> acf(std::min(lags, count), 0);
    const double *x = values.constData();
    for (int lag = 0; lag < acf.size(); ++lag) {
        double sum = 0;
        for (int index = 0; index < count - lag; ++index)
            sum += x[index] * x[index + lag];
        acf[lag] = sum / double(count - lag);
    }
    if (acf.isEmpty() || !(acf.first() > 0))
        return QVector<double>(acf.size(), 0);
    const double zero = acf.first();
    for (double &item : acf)
        item /= zero;
    return acf;
}

}

int hopFor(double rate)
{
    if (!std::isfinite(rate) || rate <= 0)
        return hop;
    return std::max(16, int(std::round(hop * rate / hopRate)));
}

std::optional<double> detect(const QVector<qint16> &samples, double rate)
{
    if (!(rate >= 2000) || !std::isfinite(rate))
        return std::nullopt;
    const int step = hopFor(rate);
    const double fps = rate / double(step);
    const QVector<double> found = onsets(samples, rate, step);
    const int lags = int(std::ceil(60 / slowest * fps * multiples[multipleCount - 1])) + 2;
    if (found.size() <= qsizetype(lags) * 2)
        return std::nullopt;
    const QVector<double> acf = autocorrelation(found, lags);
    double bestBpm = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    double bestRaw = 0;
    bool any = false;
    for (int tenth = int(std::round(slowest * 10)); tenth <= int(std::round(fastest * 10)); ++tenth) {
        const double tempo = double(tenth) / 10;
        const double raw = score(acf, 60 / tempo * fps);
        const double weighted = raw * prior(tempo);
        if (weighted > bestScore) {
            bestBpm = tempo;
            bestScore = weighted;
            bestRaw = raw;
            any = true;
        }
    }
    if (!any || !(bestRaw >= confidence))
        return std::nullopt;
    double chosen = bestBpm;
    std::optional<double> doubled;
    for (const double nudge : {-0.1, 0.0, 0.1}) {
        const double candidate = bestBpm * 2 + nudge;
        if (candidate > fastest)
            continue;
        if (!doubled || score(acf, 60 / *doubled * fps) < score(acf, 60 / candidate * fps))
            doubled = candidate;
    }
    if (doubled && score(acf, 60 / *doubled * fps) >= bestRaw * octave)
        chosen = *doubled;
    const double period = refine(acf, 60 / chosen * fps);
    return std::min(fastest, std::max(slowest, std::round(60 * fps / period * 10) / 10));
}

}
