#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace forkmesh::binarysize {

enum Category {
    AppCode = 0,
    Templates,
    Libraries,
    Constants,
    Unwind,
    Dynamic,
    Debug,
    SymbolNames,
    Other,
    CategoryCount
};

struct Node {
    QString name;
    QString detail;
    qint64 bytes = 0;
    int parent = -1;
    int category = Other;
    QVector<int> children;
};

struct Report {
    QString binaryPath;
    qint64 fileSize = 0;
    QVector<Node> nodes;
    int objectCount = 0;
    int objectsListed = 0;
    int symbolCount = 0;
    bool fromSymtab = false;
    QString error;
    qint64 scanMs = 0;

    bool isEmpty() const { return nodes.isEmpty(); }
};

struct Options {
    QString binaryPath;
    QString buildDir;
    QString targetName;
    QString sourceRoot;
    int symbolsPerGroup = 150;
    std::function<QString(const QString &)> componentOf;
};

Report analyse(const Options &options,
               const std::function<bool(int, int)> &progress = {});

QStringList linkedObjects(const QString &buildDir, const QString &targetName);
QString sourcePathOf(const QString &objectPath, const QString &sourceRoot);
QString demangle(const QByteArray &name);
QString familyOf(const QString &demangled);
QString shortNameOf(const QString &demangled);
QString categoryName(int category);
QString formatBytes(qint64 bytes);

}
