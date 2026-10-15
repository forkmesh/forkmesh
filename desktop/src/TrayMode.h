#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::tray {

QStringList arguments(const QString &project);
bool launch();
int run(int argc, char *argv[]);

} // namespace forkmesh::tray
