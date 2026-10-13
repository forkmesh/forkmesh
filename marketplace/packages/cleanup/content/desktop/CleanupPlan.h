#pragma once

#include "CleanupScanner.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>

namespace forkmesh::cleanup {

struct Choice {
    bool confirmed = false;
    bool discardChanges = false;
    bool retireBranch = false;
};

struct Step {
    Item item;
    Choice choice;
};

struct Skip {
    Item item;
    QString why;
};

struct Plan {
    QList<Step> files;
    QList<Step> sandboxes;
    QList<Step> worktrees;
    QList<Skip> skipped;
    qint64 bytes = 0;
    int count() const { return int(files.size() + sandboxes.size() + worktrees.size()); }
};

QString planRefusal(const Item &item, const Choice &choice);
Plan buildPlan(const QList<Item> &selected, const QHash<QString, Choice> &choices);
QString describe(const Plan &plan);

struct Recheck {
    ProcessSnapshot processes;
    QSet<QString> live;
    qint64 nowMs = 0;
    int liveMinutes = 30;
};

struct Outcome {
    QString path;
    qint64 bytes = 0;
    QString reason;
    bool removed = false;
    QString error;
};

using SandboxRemover = std::function<QString(const QString &, const QString &)>;

QString memberRefusal(const Item &item, const Member &member, const Recheck &recheck);
QString worktreeRefusal(const Item &item, const Recheck &recheck);
QList<Outcome> deleteFiles(const QList<Step> &steps, const Recheck &recheck,
                           const CancelFlag &cancel = {});
QList<Outcome> deleteSandboxes(const QList<Step> &steps, const Recheck &recheck,
                               const SandboxRemover &remove);
qint64 freedBytes(const QList<Outcome> &outcomes);
QString logLine(const Outcome &outcome, qint64 nowMs);
bool appendLog(const QString &file, const QList<Outcome> &outcomes, qint64 nowMs);

} // namespace forkmesh::cleanup
