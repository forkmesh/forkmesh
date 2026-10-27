#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace forkmesh::git {

bool capture(const QString &dir, const QStringList &args, QByteArray *out,
             QString *err);

}
