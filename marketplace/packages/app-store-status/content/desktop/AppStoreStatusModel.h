#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace forkmesh::appstorestatus {

enum class Tone { Neutral, Good, Pending, Action };

struct Figures {
    QString label;
    qint64 downloads = 0;
    qint64 redownloads = 0;
    qint64 updates = 0;
};

Tone toneOf(const QJsonObject &status);
QString toneName(Tone tone);
QString statusLabel(const QJsonObject &status);
QString humanizeState(const QString &state);

QDateTime parseTimestamp(const QJsonValue &value);
QString relativeTime(const QDateTime &then, const QDateTime &now);
QString publishedText(const QJsonValue &publishedAt, const QDateTime &now);
QString dateText(const QJsonValue &value);

QJsonObject snapshotOf(const QJsonObject &payload);
bool exportHasData(const QJsonObject &payload);

QString versionTitle(const QString &version, const QString &build);
QString releaseTypeLabel(const QString &releaseType);
QString versionDetail(const QJsonObject &version);
QString buildDetail(const QJsonObject &build);
QString submissionTitle(const QJsonObject &submission);

QString countText(qint64 count, const QString &singular, const QString &plural);
QString spanLabel(int days);
QList<Figures> downloadFigures(const QJsonObject &downloads);

QJsonArray sortedGroups(const QJsonArray &groups);
QString groupDetail(const QJsonObject &group);

QString feedbackTitle(const QJsonObject &feedback);
QString feedbackDetail(const QJsonObject &feedback);

QString stars(int rating);
QString reviewTitle(const QJsonObject &review);
QString reviewDetail(const QJsonObject &review);

QString sectionTitle(const QString &key);
QString unavailableText(const QJsonObject &snapshot, const QString &key);
QStringList attentionLines(const QJsonObject &snapshot);

}
