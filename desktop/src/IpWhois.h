#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh {
namespace ipwhois {

struct Info {
    QString network;
    QString range;
    QString cidr;
    QString owner;
    QString about;
    QString country;
    QString asn;
    QString abuse;
    QString registered;
    QString updated;
    bool isEmpty() const;
};

Info parse(const QString &text);
bool worthLookingUp(const QString &address);
QString cacheKey(const QString &address);
QString ownerLabel(const Info &info);
QString describe(const QString &address, const Info &info);

} // namespace ipwhois
} // namespace forkmesh
