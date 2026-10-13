#pragma once

#include <QString>

class CoreClient;

namespace forkmesh::corestats {

void noteLogLine(CoreClient *client, const QString &storedLine, const QString &badge);
void flushLogLines();

} // namespace forkmesh::corestats
