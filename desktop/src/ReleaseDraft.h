#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace forkmesh::releasedraft {

constexpr int kMaxTitleChars = 200;
constexpr int kMaxNotesChars = 20000;
constexpr int kMaxLogCommits = 250;
constexpr int kMaxTagChars = 128;

struct Latest {
    QString tag;
    QString title;
};

struct Cut {
    QString owner;
    QString name;
    QString dir;
    QString tag;
    QString targetRef;
    QString title;
    QString notes;
    bool canRollOut = false;
    bool rollout = false;
    bool pruneArtifacts = false;
    bool pruneTags = false;
    bool postToMastodon = false;
};

Latest parseLatest(const QByteArray &forEachRefLine);
QString nextTag(const QString &previous);
QString nextTitle(const QString &prevTag, const QString &prevTitle, const QString &tag);
bool tagNameValid(const QString &tag);
bool deployableTag(const QString &tag);
QStringList logArgs(const QString &prevTag, const QString &targetRef);
QString notesFromLog(const QByteArray &log, const QString &prevTag,
                     const QString &tag, const QString &targetRef);
QString tagMessage(const QString &title, const QString &notes, const QString &tag);

}
