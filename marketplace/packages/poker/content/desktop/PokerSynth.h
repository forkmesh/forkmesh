#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace forkmesh::poker {

QStringList soundNames();
std::vector<float> synthesize(const QString &name, int sampleRate);
double soundSeconds(const std::vector<float> &samples, int sampleRate);
float peakLevel(const std::vector<float> &samples);
QByteArray pcm16(const std::vector<float> &samples, int channels = 1);
QByteArray pcmFloat(const std::vector<float> &samples, int channels = 1);

}
