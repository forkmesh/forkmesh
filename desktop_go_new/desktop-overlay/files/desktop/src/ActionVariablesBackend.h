#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::actionvars {

enum class WriteResult { Unhandled, Written, Refused };

using Reader = std::function<bool(QMap<QString, QString> *variables)>;
using Writer = std::function<WriteResult(const QMap<QString, QString> &variables)>;

void setBackend(Reader reader, Writer writer);
QString marker();
QMap<QString, QString> storedPlaintext();
void writeMarkers(const QStringList &names);
bool write(const QMap<QString, QString> &variables);

}
