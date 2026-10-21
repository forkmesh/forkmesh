#pragma once

#include <QList>
#include <QString>

#include <utility>

namespace forkmesh::hiddenunicode {

inline constexpr const char *kPolicy = "forkmesh-hidden-unicode-v1";

QString clean(const QString &text);
bool hasHidden(const QString &text);
QList<std::pair<int, int>> droppedRanges(const QString &text);

}
