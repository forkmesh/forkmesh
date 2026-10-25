#pragma once

#include <QVector>

#include <optional>

namespace forkmesh::djstudio::bpm {

inline constexpr double slowest = 70.0;
inline constexpr double fastest = 180.0;
inline constexpr double favourite = 120.0;
inline constexpr double spread = 0.8;
inline constexpr int hop = 256;
inline constexpr double hopRate = 22050.0;
inline constexpr double longest = 120;
inline constexpr double multiples[] = {1, 2, 3, 4, 5, 6, 7, 8};
inline constexpr double confidence = 0.04;
inline constexpr double octave = 0.8;

int hopFor(double rate);
std::optional<double> detect(const QVector<qint16> &samples, double rate);

}
