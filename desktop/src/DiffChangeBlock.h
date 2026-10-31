#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::diffblock {

QStringList splitFilePatches(const QString &patch);

QString filePatchPath(const QString &filePatch);

QString changeBlockPatch(const QString &filePatch, int oldStart, int newStart);

} // namespace forkmesh::diffblock
