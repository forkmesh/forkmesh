#pragma once

#include "MergeQueue.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// The "mergequeue." local control family (WS11; docs/native/DESKTOP_PARITY.md
// section 19): the merge queue and code review actions that run on this desktop. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlMergeQueue.cpp. Gate 2 creates it empty; WS11 owns it.
namespace forkmesh::localcontrol::mergequeue {

QString familyPrefix();

constexpr int kMaxRepos = 100;
constexpr int kMaxEntries = 100;
constexpr int kMaxTitleChars = 200;

QStringList actions();
bool isKnownAction(const QString &action);
bool needsNumber(const QString &action);
bool validRepoSegment(const QString &segment);
bool validRequest(const QString &owner, const QString &name, const QString &action,
                  int number, int delta);

QString confirmationTarget(const QString &owner, const QString &name,
                           const QString &action, int number, int delta);

QString actionSummary(const QString &owner, const QString &name,
                      const QString &action, int number, int delta);

struct RepoState {
    QString owner;
    QString name;
    bool enabled = false;
    bool paused = false;
    bool open = false;
    bool writable = false;
    MergeQueue queue;
};

QString precheck(const RepoState &repo, const QString &action, int number,
                 const QString &pullStatus);

QString applyToQueue(MergeQueue &queue, const QString &action, int number,
                     int delta, qint64 nowMs);

QString resultMessage(const QString &action, int number, const MergeQueue &queue);

QJsonObject repoRow(const RepoState &repo, const QHash<int, QString> &titles);

} // namespace forkmesh::localcontrol::mergequeue
