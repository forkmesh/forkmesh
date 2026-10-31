#include "DjWave.h"

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

int DjWaveAccumulator::bars(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0)
        return barCount;
    return std::min(mostBars, std::max(barCount, int(seconds * barsPerSecond)));
}

DjWaveAccumulator::DjWaveAccumulator(double sampleRate, qsizetype totalSamples, int bars)
{
    const double rate = std::max(1000.0, std::isfinite(sampleRate) ? sampleRate : 22050.0);
    m_lowAlpha = float(1 - std::exp(-2 * M_PI * 250 / rate));
    m_highAlpha = float(1 - std::exp(-2 * M_PI * 2500 / rate));
    m_samplesPerBar = std::max<qsizetype>(
        1, qsizetype(std::ceil(double(std::max<qsizetype>(1, totalSamples)) / double(std::max(1, bars)))));
    m_rows.reserve(std::max(0, bars) + 1);
}

void DjWaveAccumulator::add(const float *samples, qsizetype count)
{
    for (qsizetype i = 0; i < count; ++i) {
        const float value = std::isfinite(samples[i]) ? samples[i] : 0.0f;
        m_low += m_lowAlpha * (value - m_low);
        m_lower += m_lowAlpha * (m_low - m_lower);
        m_smooth += m_highAlpha * (value - m_smooth);
        m_smoother += m_highAlpha * (m_smooth - m_smoother);
        const float high = value - m_smoother;
        const float mid = m_smoother - m_lower;
        m_lowEnergy += double(m_lower * m_lower);
        m_midEnergy += double(mid * mid);
        m_highEnergy += double(high * high);
        m_filled += 1;
        if (m_filled >= m_samplesPerBar)
            close();
    }
}

void DjWaveAccumulator::add(const QVector<float> &samples) { add(samples.constData(), samples.size()); }

QVector<DjWaveBar> DjWaveAccumulator::finish(int count)
{
    if (m_filled > 0)
        close();
    if (m_rows.isEmpty() || count <= 0)
        return {};
    double peak = 0;
    for (const Row &row : m_rows)
        peak = std::max(peak, row.rms);
    if (!(peak > 1e-5))
        return {};
    QVector<DjWaveBar> out;
    out.reserve(count);
    const qsizetype rows = m_rows.size();
    for (int index = 0; index < count; ++index) {
        const Row &row = m_rows.at(std::min(rows - 1, qsizetype(index) * rows / count));
        DjWaveBar bar;
        bar.level = std::min(1.0, std::max(0.04, std::pow(row.rms / peak, 0.8)));
        const double low = row.low * row.low;
        const double mid = row.mid * row.mid;
        const double high = row.high * row.high;
        const double weight = low + mid + high;
        if (weight > 0) {
            bar.low = std::clamp(low / weight, 0.0, 1.0);
            bar.mid = std::clamp(mid / weight, 0.0, 1.0);
            bar.high = std::clamp(high / weight, 0.0, 1.0);
        }
        out.append(bar);
    }
    return out;
}

void DjWaveAccumulator::close()
{
    const double count = double(std::max<qsizetype>(1, m_filled));
    const double total = (m_lowEnergy + m_midEnergy + m_highEnergy) / count;
    m_rows.append(Row{std::sqrt(total), std::sqrt(m_lowEnergy / count), std::sqrt(m_midEnergy / count),
                      std::sqrt(m_highEnergy / count)});
    m_filled = 0;
    m_lowEnergy = 0;
    m_midEnergy = 0;
    m_highEnergy = 0;
}

namespace waveCache {

namespace {

char byte(double value)
{
    const double scaled = std::isfinite(value) ? std::round(value * 255) : 0;
    return char(quint8(std::clamp(scaled, 0.0, 255.0)));
}

double unit(char value) { return double(quint8(value)) / 255; }

}

QByteArray encode(const QVector<DjWaveBar> &bars)
{
    QByteArray bytes;
    bytes.reserve(bars.size() * 4);
    for (const DjWaveBar &bar : bars) {
        bytes.append(byte(bar.level));
        bytes.append(byte(bar.low));
        bytes.append(byte(bar.mid));
        bytes.append(byte(bar.high));
    }
    return bytes;
}

QVector<DjWaveBar> decode(const QByteArray &data)
{
    if (data.isEmpty() || data.size() % 4 != 0)
        return {};
    QVector<DjWaveBar> bars;
    bars.reserve(data.size() / 4);
    for (qsizetype index = 0; index < data.size(); index += 4) {
        DjWaveBar bar;
        bar.level = std::max(0.04, unit(data.at(index)));
        bar.low = unit(data.at(index + 1));
        bar.mid = unit(data.at(index + 2));
        bar.high = unit(data.at(index + 3));
        bars.append(bar);
    }
    return bars;
}

}

}
