#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::pullmergeall {

constexpr int kMaxTitleChars = 160;
constexpr int kMaxTargets = 200;

struct Target
{
    int number = 0;
    QString title;
    QString authorName;
    QString branch;
    QString base;
};

struct Run
{
    QString owner;
    QString name;
    QList<Target> targets;
};

QString promptTitle(const QString &title);
bool safeRefName(const QString &ref);
QString agentPrompt(const QString &owner, const QString &name,
                    const QString &checkout, const QList<Target> &targets);
QString runTitle(int count);

QJsonObject runToJson(const Run &run);
bool runFromJson(const QJsonObject &object, Run *run);
QByteArray encodeRuns(const QHash<int, Run> &runs);
QHash<int, Run> decodeRuns(const QByteArray &bytes);

}
