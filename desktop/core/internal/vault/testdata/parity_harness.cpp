#include "VaultFile.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 4)
        return 2;
    if (args.size() == 5) {
        const QByteArray name = forkmesh::vaultfile::entryName(args.at(3), args.at(4)).toUtf8();
        std::fwrite(name.constData(), 1, size_t(name.size()), stdout);
        return 0;
    }
    QMap<QString, forkmesh::vaultfile::Entry> entries;
    QString error;
    QJsonObject out;
    if (!forkmesh::vaultfile::readNamespace(args.at(1), args.at(2), args.at(3), &entries, &error)) {
        out.insert(QStringLiteral("error"), error);
    } else {
        QJsonObject values;
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
            values.insert(it.key(), QJsonObject{{QStringLiteral("value"), it.value().value},
                                                {QStringLiteral("at"), double(it.value().at)}});
        out.insert(QStringLiteral("values"), values);
    }
    const QByteArray line = QJsonDocument(out).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    return 0;
}
