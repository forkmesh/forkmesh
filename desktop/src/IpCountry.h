#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace forkmesh {
namespace ipcountry {

struct Range4 {
    quint32 first = 0;
    quint32 last = 0;
    char code[2] = {0, 0};
};

struct Range6 {
    quint64 first = 0;
    quint64 last = 0;
    char code[2] = {0, 0};
};

struct Table {
    QVector<Range4> v4;
    QVector<Range6> v6;
    bool isEmpty() const { return v4.isEmpty() && v6.isEmpty(); }
};

int appendCsv(Table &table, const QByteArray &csv);
void finish(Table &table);
QString lookup(const Table &table, const QString &address);

QString csvUrl(bool ipv6);
QString csvFileName(bool ipv6);
bool csvLooksValid(const QByteArray &csv);

} // namespace ipcountry
} // namespace forkmesh
