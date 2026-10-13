#include "RepoStatsStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>

static bool run(const QString &dir, const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    return p.waitForFinished() && p.exitCode() == 0;
}

static void write(QFile &file, const QByteArray &data)
{
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(data);
    file.close();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!RepoStatsStore::onlyGeneratedStatsDirty(
            QByteArrayLiteral(" M .forkmesh/stats/repository.json\n")) ||
        !RepoStatsStore::onlyGeneratedStatsDirty(
            QByteArrayLiteral("?? .forkmesh/stats/repository.json\r\n")) ||
        RepoStatsStore::onlyGeneratedStatsDirty(QByteArray()) ||
        RepoStatsStore::onlyGeneratedStatsDirty(
            QByteArrayLiteral(" M .forkmesh/stats/repository.json\n"
                              " M source.cpp\n")) ||
        RepoStatsStore::onlyGeneratedStatsDirty(
            QByteArrayLiteral("R  old.json -> .forkmesh/stats/repository.json\n")))
        return 1;
    QTemporaryDir tmp;
    if (!tmp.isValid() || !run(tmp.path(), {QStringLiteral("init"),
                                            QStringLiteral("-q")})) return 1;
    run(tmp.path(), {QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("test@example.invalid")});
    run(tmp.path(), {QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Test")});
    QFile source(QDir(tmp.path()).filePath(QStringLiteral("source.cpp")));
    write(source, "one\ntwo\n");
    if (!run(tmp.path(), {QStringLiteral("add"), QStringLiteral("source.cpp")}) ||
        !run(tmp.path(), {QStringLiteral("commit"), QStringLiteral("-qm"),
                          QStringLiteral("seed")})) return 1;

    QString error;
    const QVector<RepoStatsSample> first = RepoStatsStore::captureDaily(tmp.path(), &error);
    if (first.isEmpty() || first.last().files != 1 || first.last().lines != 2 ||
        RepoStatsStore::repositoryJsonEnabled(tmp.path()) ||
        QFileInfo::exists(QDir(tmp.path()).filePath(
            QStringLiteral(".forkmesh/stats/repository.json")))) return 1;
    if (!RepoStatsStore::setRepositoryJsonEnabled(tmp.path(), true, &error) ||
        !RepoStatsStore::repositoryJsonEnabled(tmp.path()) ||
        RepoStatsStore::captureDaily(tmp.path(), &error).isEmpty() ||
        !QFileInfo::exists(QDir(tmp.path()).filePath(
            QStringLiteral(".forkmesh/stats/repository.json")))) return 1;
    QFile stats(QDir(tmp.path()).filePath(
        QStringLiteral(".forkmesh/stats/repository.json")));
    if (!stats.open(QIODevice::ReadOnly)) return 1;
    QJsonObject legacyStats = QJsonDocument::fromJson(stats.readAll()).object();
    stats.close();
    legacyStats.insert(QStringLiteral("ratchet"), true);
    if (!stats.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    stats.write(QJsonDocument(legacyStats).toJson(QJsonDocument::Indented));
    stats.close();
    if (!RepoStatsStore::ratchetEnabled(tmp.path()) ||
        !stats.open(QIODevice::ReadOnly)) return 1;
    const QByteArray statsBeforeToggle = stats.readAll();
    stats.close();
    if (!RepoStatsStore::setRatchetEnabled(tmp.path(), true, &error) ||
        !RepoStatsStore::ratchetEnabled(tmp.path()) ||
        RepoStatsStore::agentGuidance(tmp.path()).isEmpty()) return 1;
    if (!stats.open(QIODevice::ReadOnly) || stats.readAll() != statsBeforeToggle)
        return 1;
    stats.close();

    // Growing the repository is refused, and the refusal talks about size.
    QString reason;
    write(source, "one\ntwo\nthree and then some more text\n");
    run(tmp.path(), {QStringLiteral("add"), QStringLiteral("source.cpp")});
    if (RepoStatsStore::stagedCommitAllowed(tmp.path(), &reason) ||
        !reason.contains(QStringLiteral("ceiling"))) return 1;

    // Disabling is immediate and local: it overrides any legacy committed state,
    // clears that same refusal, and must not create a repository change that
    // itself needs a commit.
    if (!RepoStatsStore::setRatchetEnabled(tmp.path(), false, &error) ||
        RepoStatsStore::ratchetEnabled(tmp.path()) ||
        !RepoStatsStore::agentGuidance(tmp.path()).isEmpty() ||
        !RepoStatsStore::stagedCommitAllowed(tmp.path(), &reason)) return 1;
    if (!stats.open(QIODevice::ReadOnly) || stats.readAll() != statsBeforeToggle)
        return 1;
    stats.close();

    // Re-arm on the original content, so the ceiling is that tree's size again.
    write(source, "one\ntwo\n");
    run(tmp.path(), {QStringLiteral("add"), QStringLiteral("source.cpp")});
    if (!RepoStatsStore::setRatchetEnabled(tmp.path(), true, &error)) return 1;

    // More lines than before but fewer bytes: allowed, because the ratchet
    // measures repository size rather than lines of code.
    write(source, "a\nb\nc\n");
    run(tmp.path(), {QStringLiteral("add"), QStringLiteral("source.cpp")});
    if (!RepoStatsStore::stagedCommitAllowed(tmp.path(), &reason)) return 1;

    // The switch is shared by every worktree, not baked into a tracked file.
    if (!run(tmp.path(), {QStringLiteral("worktree"), QStringLiteral("add"),
                          QStringLiteral("-q"), QStringLiteral("--detach"),
                          QStringLiteral("wt")})) return 1;
    const QString worktree = QDir(tmp.path()).filePath(QStringLiteral("wt"));
    if (!RepoStatsStore::ratchetEnabled(worktree)) return 1;

    // Turning it off stops enforcement everywhere, even for a commit that grows.
    write(source, "one\ntwo\nthree and then some more text\n");
    run(tmp.path(), {QStringLiteral("add"), QStringLiteral("source.cpp")});
    if (!RepoStatsStore::setRatchetEnabled(tmp.path(), false, &error) ||
        RepoStatsStore::ratchetEnabled(worktree) ||
        !RepoStatsStore::agentGuidance(tmp.path()).isEmpty() ||
        !RepoStatsStore::stagedCommitAllowed(tmp.path(), &reason) ||
        !RepoStatsStore::stagedCommitAllowed(worktree, &reason)) return 1;

    std::cout << "repo stats tests passed\n";
    return 0;
}
