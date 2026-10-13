#pragma once

#include <QByteArray>
#include <QString>

namespace forkmesh::diskwrite {

void writeLater(const QString &path, const QByteArray &bytes);
bool writeNow(const QString &path, const QByteArray &bytes,
              QString *error = nullptr);
bool flush(int timeoutMs = -1);

} // namespace forkmesh::diskwrite
