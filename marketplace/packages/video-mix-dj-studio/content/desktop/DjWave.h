#pragma once

#include "DjCore.h"

#include <QByteArray>
#include <QVector>

namespace forkmesh::djstudio {

class DjWaveAccumulator
{
public:
    static constexpr int barCount = 1200;
    static constexpr double barsPerSecond = 20.0;
    static constexpr int mostBars = 12000;

    static int bars(double seconds);

    DjWaveAccumulator(double sampleRate, qsizetype totalSamples, int bars = barCount);

    void add(const float *samples, qsizetype count);
    void add(const QVector<float> &samples);
    QVector<DjWaveBar> finish(int count = barCount);

private:
    struct Row {
        double rms = 0;
        double low = 0;
        double mid = 0;
        double high = 0;
    };

    void close();

    float m_lowAlpha = 0;
    float m_highAlpha = 0;
    qsizetype m_samplesPerBar = 1;
    float m_low = 0;
    float m_lower = 0;
    float m_smooth = 0;
    float m_smoother = 0;
    qsizetype m_filled = 0;
    double m_lowEnergy = 0;
    double m_midEnergy = 0;
    double m_highEnergy = 0;
    QVector<Row> m_rows;
};

namespace waveCache {

QByteArray encode(const QVector<DjWaveBar> &bars);
QVector<DjWaveBar> decode(const QByteArray &data);

}

}
