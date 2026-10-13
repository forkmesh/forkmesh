#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

struct ActionStep {
    QString name;
    QString run;
};

// One workflow parsed from a.forkmesh/*.yml|*.yaml file. The trust unit for
// the approval gate is `content` (the raw file text) — its SHA-256 is what the
// ActionStore records as approved.
struct ActionWorkflow {
    QString path;               // repo-relative path, e.g. ".forkmesh/deploy.yml"
    QString content;            // raw file text (hashed for approval, shown in diffs)
    QString name;               // display name (defaults to the file name)
    QStringList on;             // trigger events, e.g. {"push", "workflow_dispatch"}
    QStringList pushChangedPaths; // optional filters from on.push.paths
    QMap<QString, QString> env; // workflow-level environment
    QStringList runsOn;
    QStringList needs;
    QList<ActionStep> steps;    // steps flattened across all jobs, in order
    bool checkout = true;
    int fetchDepth = 2;
    bool valid = false;
    QString error;

    bool triggersOnPush() const { return on.contains(QStringLiteral("push")); }
    bool triggersOnRelease() const
    {
        return on.contains(QStringLiteral("release"));
    }
    // Pull requests are a separate event on purpose: a repository's deploy or
    // release workflow declares `on: push` and must never run against a
    // contributor's proposed head, so a workflow is queued for a pull request
    // only where this opt-in is present (the caller reads it from the base
    // branch's copy of the file as well as from the proposed head's).
    bool triggersOnPullRequest() const
    {
        return on.contains(QStringLiteral("pull_request"));
    }
    bool allowsManualRun() const
    {
        return on.contains(QStringLiteral("workflow_dispatch"));
    }
    bool matchesPushChangedPaths(const QStringList &changedPaths) const;
    bool runsOnNode(const QStringList &nodeLabels) const;
};

class ActionFile
{
public:
    static ActionWorkflow parse(const QString &relPath, const QString &content);
    static bool defaultBranchRetiresPush(const QString &relPath,
                                         const QString &defaultBranchContent);

    static QList<ActionWorkflow> parseWorkflowsInDir(const QString &checkoutDir);

    static QStringList nodeLabels(const QString &machineNode,
                                  const QString &mirrorNode,
                                  const QString &configured);

    static QStringList parseLabelList(const QString &configured);

    static QString pinnedNode(const QStringList &pins, const QString &path);
    static QStringList setPinnedNode(const QStringList &pins,
                                     const QStringList &paths,
                                     const QString &node);

    static QString substitute(const QString &input,
                              const QMap<QString, QString> &vars);
};
