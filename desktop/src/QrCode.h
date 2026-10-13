#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <vector>

namespace QrCode {

enum class Ecl { Low, Medium, Quartile, High };

std::vector<std::vector<bool>> encode(const QByteArray &data,
                                      Ecl ecl = Ecl::Medium);

QImage encodeToImage(const QString &text, int scale = 4, int margin = 4,
                     Ecl ecl = Ecl::Medium);

} // namespace QrCode
