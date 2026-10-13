#include "RepoStatsStore.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QProcess>
#include <QSaveFile>

namespace {
const int kHistoryDays = 30;
const char kStatsSkip[] = ".forkmesh/stats/";
const char kGeneratedStatsPath[] = ".forkmesh/stats/repository.json";
const QString kRatchetEnabled = QStringLiteral("forkmesh.ratchet.enabled");
const QString kRepositoryJsonEnabled =
    QStringLiteral("forkmesh.stats.repositoryJson");
const QString kRatchetCeilingDay = QStringLiteral("forkmesh.ratchet.ceilingDay");
const QString kRatchetCeilingBytes = QStringLiteral("forkmesh.ratchet.ceilingBytes");

QJsonObject readDocument(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeDocument(const QString &path, const QJsonObject &object, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QByteArray git(const QString &repo, const QStringList &args, bool *ok = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(repo);
    process.start(QStringLiteral("git"), args);
    const bool finished = process.waitForFinished(30000);
    const bool success = finished && process.exitStatus() == QProcess::NormalExit &&
                         process.exitCode() == 0;
    if (ok) *ok = success;
    return success ? process.readAllStandardOutput() : QByteArray();
}

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes);
}

RepoStatsSample treeSample(const QString &repoDir, const QString &sha)
{
    RepoStatsSample sample;
    bool ok = false;
    const QByteArray listing = git(repoDir, {QStringLiteral("ls-tree"),
                                             QStringLiteral("-r"),
                                             QStringLiteral("-l"), sha}, &ok);
    if (!ok) return sample;
    for (const QByteArray &line : listing.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab < 0 || line.mid(tab + 1).startsWith(kStatsSkip)) continue;
        const QList<QByteArray> fields = line.left(tab).simplified().split(' ');
        if (fields.size() < 4 || fields.at(1) != "blob") continue;
        sample.bytes += fields.at(3).toLongLong();
        ++sample.files;
    }
    return sample;
}

bool readLocalConfig(const QString &repo, const QString &key, QString *value)
{
    bool ok = false;
    const QByteArray output = git(repo, {QStringLiteral("config"),
                                         QStringLiteral("--local"),
                                         QStringLiteral("--get"), key}, &ok);
    if (!ok) return false;
    if (value) *value = QString::fromUtf8(output).trimmed();
    return true;
}

bool writeLocalConfig(const QString &repo, const QString &key,
                      const QString &value, QString *error)
{
    bool ok = false;
    git(repo, {QStringLiteral("config"), QStringLiteral("--local"), key, value}, &ok);
    if (!ok && error)
        *error = QStringLiteral("Could not write the repository's local Git config.");
    return ok;
}

bool localConfigBool(const QString &repo, const QString &key, bool *found)
{
    QString value;
    const bool present = readLocalConfig(repo, key, &value);
    if (found) *found = present;
    if (!present) return false;
    value = value.toLower();
    return value == QStringLiteral("true") || value == QStringLiteral("yes") ||
           value == QStringLiteral("on") || value == QStringLiteral("1");
}

qint64 localConfigInteger(const QString &repo, const QString &key,
                          qint64 fallback)
{
    QString value;
    if (!readLocalConfig(repo, key, &value)) return fallback;
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok);
    return ok ? parsed : fallback;
}

bool writeRatchetCeiling(const QString &repo, const RepoStatsSample &sample,
                         QString *error)
{
    return writeLocalConfig(repo, kRatchetCeilingDay, sample.day, error) &&
           writeLocalConfig(repo, kRatchetCeilingBytes,
                            QString::number(sample.bytes), error);
}
}

QString RepoStatsStore::statsPath(const QString &repoDir)
{
    return QDir(repoDir).filePath(QString::fromLatin1(kGeneratedStatsPath));
}

bool RepoStatsStore::onlyGeneratedStatsDirty(
    const QByteArray &porcelainStatus)
{
    bool found = false;
    for (QByteArray line : porcelainStatus.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if (line.size() < 4 || line.mid(3) != kGeneratedStatsPath)
            return false;
        found = true;
    }
    return found;
}

RepoStatsSample RepoStatsStore::measure(const QString &repoDir, QString *error)
{
    RepoStatsSample sample;
    sample.day = QDate::currentDate().toString(Qt::ISODate);
    bool ok = false;
    const QList<QByteArray> paths = git(repoDir, {QStringLiteral("ls-files"),
                                                 QStringLiteral("-z")}, &ok)
                                      .split('\0');
    if (!ok) {
        if (error) *error = QStringLiteral("git ls-files failed");
        return {};
    }
    for (const QByteArray &encoded : paths) {
        if (encoded.isEmpty()) continue;
        if (encoded.startsWith(kStatsSkip)) continue;
        QFile file(QDir(repoDir).filePath(QString::fromUtf8(encoded)));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = file.readAll();
        sample.bytes += data.size();
        ++sample.files;
        if (!data.contains('\0')) {
            sample.lines += data.count('\n');
            if (!data.isEmpty() && !data.endsWith('\n')) ++sample.lines;
        }
    }
    return sample;
}

QVector<RepoStatsSample> RepoStatsStore::backfill(const QString &repoDir,
                                                  const RepoStatsSample &today)
{
    const QDate last = QDate::fromString(today.day, Qt::ISODate);
    const QDate first = last.addDays(-(kHistoryDays - 1));
    if (!last.isValid()) return {today};

    bool ok = false;
    QVector<QPair<QDate, QString>> commits; // newest first
    for (const QByteArray &line : git(repoDir, {QStringLiteral("log"),
                                                QStringLiteral("--first-parent"),
                                                QStringLiteral("--format=%H %cs")},
                                      &ok).split('\n')) {
        const int space = line.indexOf(' ');
        if (space <= 0) continue;
        const QDate when =
            QDate::fromString(QString::fromLatin1(line.mid(space + 1, 10)), Qt::ISODate);
        if (when.isValid())
            commits.append({when, QString::fromLatin1(line.left(space))});
    }
    if (commits.isEmpty()) return {today};

    QHash<QString, qint64> lineDelta;
    QString cursor;
    for (const QByteArray &line : git(repoDir, {QStringLiteral("log"),
                                                QStringLiteral("--first-parent"),
                                                QStringLiteral("-m"),
                                                QStringLiteral("--numstat"),
                                                QStringLiteral("--format=%x01%cs"),
                                                QStringLiteral("--since"),
                                                first.toString(Qt::ISODate)},
                                      &ok).split('\n')) {
        if (line.startsWith('\x01')) {
            cursor = QString::fromLatin1(line.mid(1, 10));
            continue;
        }
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() < 3 || fields.at(0) == "-") continue;
        if (fields.at(2).startsWith(kStatsSkip)) continue;
        lineDelta[cursor] += fields.at(0).toLongLong() - fields.at(1).toLongLong();
    }

    QHash<QString, RepoStatsSample> treeCache;
    QVector<RepoStatsSample> days;
    qint64 lines = today.lines;
    for (QDate day = last; day >= first; day = day.addDays(-1)) {
        const QString key = day.toString(Qt::ISODate);
        if (key == today.day) {
            days.prepend(today);
        } else {
            QString sha;
            for (const QPair<QDate, QString> &commit : commits) {
                if (commit.first <= day) { sha = commit.second; break; }
            }
            if (sha.isEmpty()) break; // before the repository's first commit
            if (!treeCache.contains(sha)) treeCache.insert(sha, treeSample(repoDir, sha));
            RepoStatsSample sample = treeCache.value(sha);
            sample.day = key;
            sample.lines = qMax<qint64>(0, lines);
            days.prepend(sample);
        }
        lines -= lineDelta.value(key);
    }
    return days;
}

QVector<RepoStatsSample> RepoStatsStore::load(const QString &repoDir, QString *error)
{
    QVector<RepoStatsSample> result;
    const QJsonObject root = readDocument(statsPath(repoDir));
    for (const QJsonValue &value : root.value(QStringLiteral("days")).toArray()) {
        const QJsonObject item = value.toObject();
        RepoStatsSample sample;
        sample.day = item.value(QStringLiteral("day")).toString();
        sample.bytes = qint64(item.value(QStringLiteral("bytes")).toDouble());
        sample.lines = qint64(item.value(QStringLiteral("lines")).toDouble());
        sample.files = qint64(item.value(QStringLiteral("files")).toDouble());
        if (!sample.day.isEmpty()) result.append(sample);
    }
    if (result.size() > kHistoryDays) result = result.mid(result.size() - kHistoryDays);
    Q_UNUSED(error);
    return result;
}

QVector<RepoStatsSample> RepoStatsStore::captureDaily(const QString &repoDir,
                                                       QString *error)
{
    QVector<RepoStatsSample> days = load(repoDir, error);
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (!days.isEmpty() && days.last().day == today) {
        if (ratchetEnabled(repoDir)) ceilingBytes(repoDir, days.last(), true);
        return days;
    }
    const RepoStatsSample current = measure(repoDir, error);
    if (current.day.isEmpty()) return days;
    if (days.isEmpty()) days = backfill(repoDir, current);
    else days.append(current);
    while (days.size() > kHistoryDays) days.removeFirst();
    QJsonObject root = readDocument(statsPath(repoDir));
    QJsonArray encoded;
    for (const RepoStatsSample &sample : days)
        encoded.append(QJsonObject{{QStringLiteral("day"), sample.day},
                                   {QStringLiteral("bytes"), double(sample.bytes)},
                                   {QStringLiteral("lines"), double(sample.lines)},
                                   {QStringLiteral("files"), double(sample.files)}});
    root.insert(QStringLiteral("days"), encoded);
    if (ratchetEnabled(repoDir) && !writeRatchetCeiling(repoDir, current, error))
        return {};
    if (repositoryJsonEnabled(repoDir) &&
        !writeDocument(statsPath(repoDir), root, error))
        return {};
    return days;
}

bool RepoStatsStore::repositoryJsonEnabled(const QString &repoDir)
{
    return localConfigBool(repoDir, kRepositoryJsonEnabled, nullptr);
}

bool RepoStatsStore::setRepositoryJsonEnabled(const QString &repoDir,
                                               bool enabled, QString *error)
{
    return writeLocalConfig(repoDir, kRepositoryJsonEnabled,
                            enabled ? QStringLiteral("true")
                                    : QStringLiteral("false"),
                            error);
}

bool RepoStatsStore::ratchetEnabled(const QString &repoDir)
{
    bool found = false;
    const bool enabled = localConfigBool(repoDir, kRatchetEnabled, &found);
    if (found) return enabled;
    return readDocument(statsPath(repoDir)).value(QStringLiteral("ratchet")).toBool();
}

RepoRatchetReading RepoStatsStore::ratchetReading(const QString &repoDir)
{
    RepoRatchetReading reading;
    reading.enabled = ratchetEnabled(repoDir);
    if (!reading.enabled) return reading;
    reading.ceilingBytes = localConfigInteger(repoDir, kRatchetCeilingBytes, 0);
    readLocalConfig(repoDir, kRatchetCeilingDay, &reading.ceilingDay);
    return reading;
}

bool RepoStatsStore::setRatchetEnabled(const QString &repoDir, bool enabled,
                                       QString *error)
{
    if (enabled) {
        const RepoStatsSample now = measure(repoDir, error);
        if (now.day.isEmpty()) return false;
        if (!writeRatchetCeiling(repoDir, now, error)) return false;
    } else {
        git(repoDir, {QStringLiteral("config"), QStringLiteral("--local"),
                      QStringLiteral("--unset"), kRatchetCeilingDay});
        git(repoDir, {QStringLiteral("config"), QStringLiteral("--local"),
                      QStringLiteral("--unset"), kRatchetCeilingBytes});
    }
    return writeLocalConfig(repoDir, kRatchetEnabled,
                            enabled ? QStringLiteral("true")
                                    : QStringLiteral("false"),
                            error);
}

qint64 RepoStatsStore::ceilingBytes(const QString &repoDir, const RepoStatsSample &now,
                                    bool refreshStaleDay)
{
    QString day;
    const qint64 stored = localConfigInteger(repoDir, kRatchetCeilingBytes, 0);
    if (readLocalConfig(repoDir, kRatchetCeilingDay, &day) && day == now.day &&
        stored > 0)
        return stored;
    if (!refreshStaleDay) return stored;
    writeRatchetCeiling(repoDir, now, nullptr);
    return now.bytes;
}

bool RepoStatsStore::stagedCommitAllowed(const QString &repoDir, QString *reason)
{
    if (!ratchetEnabled(repoDir)) return true;
    const RepoStatsSample now = measure(repoDir, nullptr);
    if (now.day.isEmpty()) {
        if (reason) *reason = QStringLiteral("Ratchet could not measure the repository.");
        return false;
    }
    const qint64 ceiling = ceilingBytes(repoDir, now, true);
    if (ceiling > 0 && now.bytes > ceiling) {
        if (reason)
            *reason = QStringLiteral("Ratchet Mode blocked this commit: it would leave the "
                                     "repository at %1, which is %2 over today's ceiling of "
                                     "%3. Delete or shrink tracked files first.")
                          .arg(humanSize(now.bytes), humanSize(now.bytes - ceiling),
                               humanSize(ceiling));
        return false;
    }
    return true;
}

QString RepoStatsStore::agentGuidance(const QString &repoDir)
{
    if (!ratchetEnabled(repoDir)) return {};
    const RepoStatsSample now = measure(repoDir, nullptr);
    const qint64 ceiling = ceilingBytes(repoDir, now, false);
    if (ceiling <= 0) return {};
    return QStringLiteral("Ratchet Mode is enabled for this repository and measures size, "
                          "not line count. Tracked files total %1 against today's ceiling "
                          "of %2 (%3 of headroom); commits that go over are rejected, so "
                          "delete dead code or unused assets to pay for anything you add.")
        .arg(humanSize(now.bytes), humanSize(ceiling),
             humanSize(qMax<qint64>(0, ceiling - now.bytes)));
}
