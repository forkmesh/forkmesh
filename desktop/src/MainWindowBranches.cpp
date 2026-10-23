
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SecretDiffBanner.h"
#include "GitRefusal.h"
#include "AgentScratch.h"
#include "AgentWorktree.h"
#include "BackgroundFileWriter.h"
#include "FlowLayout.h"
#include "HeaderWrapLayout.h"
#include "KebabHeaderView.h"
#include "MergeSetAside.h"
#include "PrivateMirrorRuntime.h"
#include "RepoStatsStore.h"
#include "UiSounds.h"
#include "UpstreamCheckoutSync.h"
#include "UserActivityLog.h"
#include "WebBranchOps.h"

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
#include <QFutureWatcher>
#include <QItemSelectionModel>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <QTextTable>
#include <QTimer>

#include <algorithm>

using namespace forkmesh::ui;

static bool pushDeletedRef(const QString &dir, const QString &remote,
                           const QString &ref)
{
    QByteArray out;
    QString err;
    runGitCapture(dir,
                  {QStringLiteral("push"), QStringLiteral("--porcelain"), remote,
                   QStringLiteral("--delete"), ref},
                  &out, &err);
    const QString text = QString::fromUtf8(out) + '\n' + err;
    return text.contains(QStringLiteral(":refs/heads/%1\t[deleted]").arg(ref)) ||
           text.contains(QStringLiteral("unable to delete '%1': remote ref does "
                                        "not exist")
                             .arg(ref));
}

static bool deleteBranchAndCopies(const QString &dir, const QString &branch,
                                  QString *error)
{
    QByteArray listing;
    runGitCapture(dir,
                  {QStringLiteral("for-each-ref"),
                   QStringLiteral("--sort=-committerdate"),
                   QStringLiteral("--format=%(refname) %(objectname)"),
                   QStringLiteral("refs/heads/"), QStringLiteral("refs/remotes/")},
                  &listing, nullptr);
    QString localTip;
    QString newestCopyTip;
    QStringList copies;
    for (const QString &line :
         QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
        const QString ref = line.section(QLatin1Char(' '), 0, 0);
        const QString sha = line.section(QLatin1Char(' '), 1, 1);
        if (ref == QStringLiteral("refs/heads/") + branch) {
            localTip = sha;
            continue;
        }
        if (!ref.startsWith(QLatin1String("refs/remotes/")))
            continue;
        const QString tracking = ref.mid(int(qstrlen("refs/remotes/")));
        if (tracking.section(QLatin1Char('/'), 1) != branch)
            continue;
        copies << tracking;
        if (newestCopyTip.isEmpty())
            newestCopyTip = sha;
    }
    const QString tombstone = localTip.isEmpty() ? newestCopyTip : localTip;
    if (!tombstone.isEmpty())
        runGitCapture(dir,
                      {QStringLiteral("update-ref"),
                       forkmesh::upstream::deletedBranchTombstoneRef(branch),
                       tombstone},
                      nullptr, nullptr);
    if (!localTip.isEmpty() &&
        !runGitCapture(dir, {QStringLiteral("branch"), QStringLiteral("-D"), branch},
                       nullptr, error))
        return false;
    QByteArray remotesOut;
    runGitCapture(dir, {QStringLiteral("remote")}, &remotesOut, nullptr);
    const QStringList configured =
        QString::fromUtf8(remotesOut).split('\n', Qt::SkipEmptyParts);
    for (const QString &tracking : std::as_const(copies)) {
        const QString remote = tracking.section(QLatin1Char('/'), 0, 0);
        if (configured.contains(remote) && !pushDeletedRef(dir, remote, branch))
            continue;
        runGitCapture(dir, {QStringLiteral("branch"), QStringLiteral("-dr"), tracking},
                      nullptr, nullptr);
    }
    return true;
}


static QString branchMergeTree(const QString &dir, const QString &base,
                               const QString &branch);

// Whichever locks Git currently has beside a worktree's own HEAD. A killed
// merge strands index.lock, and a killed pull strands ORIG_HEAD.lock (or
// HEAD.lock, MERGE_HEAD.lock, FETCH_HEAD.lock...); any one of them blocks every
// later write in that checkout even though no MERGE_HEAD remains, so every
// top-level *.lock counts rather than a fixed list of names. Callers resolve
// the git dir off the worktree's .git link rather than asking "git rev-parse"
// for it, so the diff header can check for a lock on every refresh, and on
// every change its directory watcher reports, without spawning a process.
static QStringList gitLocksInDir(const QString &gitDir)
{
    if (gitDir.isEmpty())
        return {};
    const QDir dir(gitDir);
    QStringList locks;
    for (const QString &name :
         dir.entryList({QStringLiteral("*.lock")}, QDir::Files | QDir::Hidden,
                       QDir::Name))
        locks.append(dir.filePath(name));
    return locks;
}

static QStringList removeStaleGitLocks(const QString &worktree)
{
    QStringList removed;
    for (const QString &lockPath : gitLocksInDir(resolveGitDirPath(worktree))) {
        if (forkmesh::removeGitLockIfStale(lockPath))
            removed.append(lockPath);
    }
    return removed;
}

static bool worktreeRecordFromFiles(const QString &path, const QString &headFile,
                                    const QString &commonDir, QString *record)
{
    QFile head(headFile);
    if (!head.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString line = QString::fromUtf8(head.readLine(4096)).trimmed();
    *record = QStringLiteral("worktree %1\n").arg(path);
    if (line.startsWith(QLatin1String("ref:"))) {
        const QString ref = line.mid(4).trimmed();
        if (!ref.startsWith(QLatin1String("refs/heads/")) ||
            ref == QLatin1String("refs/heads/.invalid"))
            return false;
        const QString tip = refTipFromFiles(commonDir, ref);
        if (!tip.isEmpty())
            *record += QStringLiteral("HEAD %1\n").arg(tip);
        *record += QStringLiteral("branch %1\n").arg(ref);
    } else if (!line.isEmpty()) {
        *record += QStringLiteral("HEAD %1\ndetached\n").arg(line);
    } else {
        return false;
    }
    return true;
}

static bool gitDirIsBare(const QString &commonDir)
{
    QFile config(commonDir + QStringLiteral("/config"));
    if (!config.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    bool inCore = false;
    while (!config.atEnd()) {
        const QString line = QString::fromUtf8(config.readLine(4096)).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inCore = line.compare(QLatin1String("[core]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inCore)
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0 &&
            line.left(eq).trimmed().compare(QLatin1String("bare"),
                                            Qt::CaseInsensitive) == 0)
            return line.mid(eq + 1).trimmed().compare(QLatin1String("true"),
                                                      Qt::CaseInsensitive) == 0;
    }
    return false;
}

static bool worktreeListFromFiles(const QString &repoPath, QByteArray *out)
{
    const QString commonDir = gitCommonDirPath(repoPath);
    if (commonDir.isEmpty() || !QFileInfo(commonDir).isDir())
        return false;
    const QString commonAbs = QDir(commonDir).absolutePath();
    QString listing;
    QString record;
    if (gitDirIsBare(commonDir)) {
        listing += QStringLiteral("worktree %1\nbare\n\n").arg(commonAbs);
    } else {
        QString mainPath;
        if (QFileInfo(commonDir).fileName() == QLatin1String(".git"))
            mainPath = QFileInfo(commonAbs).absolutePath();
        else if (QDir(resolveGitDirPath(repoPath)).absolutePath() == commonAbs)
            mainPath = QDir(repoPath).absolutePath();
        if (mainPath.isEmpty() ||
            !worktreeRecordFromFiles(mainPath, commonDir + QStringLiteral("/HEAD"),
                                     commonDir, &record))
            return false;
        listing += record + QLatin1Char('\n');
    }
    const QString adminRoot = commonDir + QStringLiteral("/worktrees");
    QList<QPair<QString, QString>> linked;
    for (const QString &name :
         QDir(adminRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString admin = adminRoot + QLatin1Char('/') + name;
        QFile gitdir(admin + QStringLiteral("/gitdir"));
        QString path;
        bool prunable = true;
        if (gitdir.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString target = QString::fromUtf8(gitdir.readLine(4096)).trimmed();
            if (!target.isEmpty()) {
                if (QDir::isRelativePath(target))
                    target = QDir(admin).absoluteFilePath(target);
                target = QDir::cleanPath(target);
                path = QFileInfo(target).absolutePath();
                prunable = !QFileInfo::exists(target);
            }
        }
        if (path.isEmpty())
            continue;
        if (!worktreeRecordFromFiles(path, admin + QStringLiteral("/HEAD"), commonDir,
                                     &record)) {
            record = QStringLiteral("worktree %1\ndetached\n").arg(path);
            prunable = true;
        }
        if (prunable && !QFileInfo::exists(admin + QStringLiteral("/locked")))
            record += QStringLiteral("prunable gitdir file points to non-existent "
                                     "location\n");
        linked.append({path, record});
    }
    std::sort(linked.begin(), linked.end(),
              [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
                  return a.first < b.first;
              });
    for (const auto &entry : std::as_const(linked))
        listing += entry.second + QLatin1Char('\n');
    *out = listing.toUtf8();
    return true;
}

static bool worktreeListForCaller(const QString &repoPath, QByteArray *out)
{
    if (QThread::currentThread() == QCoreApplication::instance()->thread() &&
        worktreeListFromFiles(repoPath, out))
        return true;
    return cachedWorktreeListPorcelain(repoPath, out);
}

static QStringList unmergedFilesIn(const QString &dir)
{
    QByteArray unmerged;
    runGitCapture(dir, {"diff", "--name-only", "--diff-filter=U"}, &unmerged,
                  nullptr);
    return QString::fromUtf8(unmerged).split('\n', Qt::SkipEmptyParts);
}

static bool ensureBranchForMerge(const PullStore &store, const QString &dir,
                                 const QString &branch, const QString &base,
                                 QStringList *logs, QString *message)
{
    if (repoHasLocalBranch(dir, branch))
        return true;
    int pullNumber = 0;
    QString error;
    const PullStore::BranchSource source =
        store.ensureLocalBranch(branch, &pullNumber, &error);
    if (source == PullStore::BranchSource::Missing) {
        logs->append(QStringLiteral("Git: can't merge %1 into %2: %3")
                         .arg(branch, base, error));
        *message = QStringLiteral("Couldn't merge %1 into %2 — %3")
                       .arg(branch, base, error);
        return false;
    }
    if (source == PullStore::BranchSource::Mirror)
        logs->append(QStringLiteral("Git: fetched %1 from the mirror to merge it.")
                         .arg(branch));
    else if (source == PullStore::BranchSource::Pull)
        logs->append(QStringLiteral("Git: rebuilt %1 from pull request #%2's "
                                    "commits to merge it.")
                         .arg(branch)
                         .arg(pullNumber));
    return true;
}

static void readBehindAhead(const QString &dir, const QString &base,
                            const QString &branch, int *behind, int *ahead)
{
    *behind = 0;
    *ahead = 0;
    QByteArray counts;
    if (!runGitCapture(dir,
                       {"rev-list", "--left-right", "--count", base + "..." + branch},
                       &counts, nullptr))
        return;
    const QStringList parts = QString::fromUtf8(counts).trimmed().split(
        QRegularExpression(QStringLiteral("\\s+")));
    if (parts.size() >= 2) {
        *behind = parts.at(0).toInt();
        *ahead = parts.at(1).toInt();
    }
}

static QString gitCommonDirOf(const QString &dir)
{
    QByteArray out;
    if (!runGitCapture(dir, {"rev-parse", "--git-common-dir"}, &out, nullptr))
        return QString();
    QString common = QString::fromUtf8(out).trimmed();
    if (common.isEmpty())
        return QString();
    if (QDir::isRelativePath(common))
        common = QDir(dir).absoluteFilePath(common);
    return QDir::cleanPath(common);
}

static bool reflogEntryIsOwnWork(const QString &message, const QString &newSha,
                                 const QString &createdSha)
{
    if (message.contains(QLatin1String("fast-forward"), Qt::CaseInsensitive) ||
        message.startsWith(QLatin1String("branch: ")) ||
        message.startsWith(QLatin1String("Branch: ")) ||
        message.startsWith(QLatin1String("reset: moving to ")))
        return false;
    static const QStringList work = {
        QStringLiteral("commit"), QStringLiteral("merge "),
        QStringLiteral("cherry-pick"), QStringLiteral("rebase"),
        QStringLiteral("revert"), QStringLiteral("am: "), QStringLiteral("pull")};
    for (const QString &prefix : work)
        if (message.startsWith(prefix))
            return true;
    return newSha != createdSha;
}

static MainWindow::BranchRefHistory readBranchRefHistory(const QString &commonDir,
                                                         const QString &branch)
{
    MainWindow::BranchRefHistory history;
    if (commonDir.isEmpty() || branch.isEmpty())
        return history;
    QFile file(commonDir + QStringLiteral("/logs/refs/heads/") + branch);
    if (!file.open(QIODevice::ReadOnly))
        return history;
    QString createdSha;
    bool first = true;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const int tab = line.indexOf(QLatin1Char('\t'));
        const QString head = tab >= 0 ? line.left(tab) : line;
        const QString message = tab >= 0 ? line.mid(tab + 1).trimmed() : QString();
        const QStringList parts = head.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 4)
            continue;
        const QString oldSha = parts.at(0);
        const QString newSha = parts.at(1);
        const qint64 at = parts.at(parts.size() - 2).toLongLong();
        if (first) {
            first = false;
            if (oldSha.count(QLatin1Char('0')) != oldSha.size())
                return MainWindow::BranchRefHistory();
            createdSha = newSha;
            history.createdAt = at;
            const QString prefix = QStringLiteral("branch: Created from ");
            history.createdFrom = message.startsWith(prefix)
                                      ? message.mid(prefix.size()).trimmed()
                                      : QString();
            if (history.createdFrom.size() == 40 &&
                history.createdFrom == newSha)
                history.createdFrom = newSha.left(10);
            history.movedAt = at;
            continue;
        }
        ++history.moves;
        history.movedAt = qMax(history.movedAt, at);
        if (reflogEntryIsOwnWork(message, newSha, createdSha))
            ++history.ownUpdates;
    }
    return history;
}

static bool branchNeverCommitted(const MainWindow::BranchRefHistory &history)
{
    return history.createdAt > 0 && history.ownUpdates == 0;
}

static QString branchStampText(qint64 secs)
{
    if (secs <= 0)
        return QString();
    const QDateTime at = QDateTime::fromSecsSinceEpoch(secs);
    const QDate today = QDate::currentDate();
    QString when;
    if (at.date() == today)
        when = at.toString(QStringLiteral("h:mm AP"));
    else if (at.date().year() == today.year())
        when = at.toString(QStringLiteral("MMM d, h:mm AP"));
    else
        when = at.toString(QStringLiteral("MMM d yyyy"));
    return QString::fromUtf8("%1 \xC2\xB7 %2").arg(when, formatShortRelativeTime(secs));
}

static QString branchStampTip(qint64 secs)
{
    return secs > 0 ? QDateTime::fromSecsSinceEpoch(secs).toString(
                          QStringLiteral("dddd, MMMM d, yyyy 'at' h:mm:ss AP t"))
                    : QString();
}

static QString checkedOutBranch(const QString &dir)
{
    QByteArray headOut;
    if (runGitCapture(dir, {"rev-parse", "--abbrev-ref", "HEAD"}, &headOut, nullptr))
        return QString::fromUtf8(headOut).trimmed();
    return QString();
}

static const QString kBranchConflictsState = QStringLiteral("conflicts");

static constexpr int kBranchFilesRole = Qt::UserRole + 72;
static constexpr int kBranchAddedRole = Qt::UserRole + 73;
static constexpr int kBranchRemovedRole = Qt::UserRole + 74;
static constexpr int kBranchConflictRole = Qt::UserRole + 75;
static constexpr int kBranchBehindRole = Qt::UserRole + 77;
static constexpr int kBranchLinksRole = Qt::UserRole + 78;

static QString branchHealthIconName(bool conflicted, int behind)
{
    if (conflicted)
        return QStringLiteral("alert");
    if (behind > 0)
        return QStringLiteral("download");
    return QString();
}

class BranchSortPause
{
public:
    explicit BranchSortPause(QTableWidget *table)
        : m_table(table), m_was(table && table->isSortingEnabled())
    {
        if (m_was)
            m_table->setSortingEnabled(false);
    }
    ~BranchSortPause()
    {
        if (m_was && m_table)
            m_table->setSortingEnabled(true);
    }
    BranchSortPause(const BranchSortPause &) = delete;
    BranchSortPause &operator=(const BranchSortPause &) = delete;

private:
    QPointer<QTableWidget> m_table;
    bool m_was = false;
};

static QTableWidgetItem *branchSortItem(const QString &text, const QVariant &key)
{
    auto *item = new SortTableWidgetItem(text);
    item->setData(kTableSortRole, key);
    return item;
}

static QTableWidgetItem *branchCountItem(int count, const QColor &tone,
                                         const QString &tip)
{
    auto *item = branchSortItem(count >= 0 ? QString::number(count) : QString(),
                                count);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    item->setForeground(count > 0 ? tone : Theme::remapColor(QColor("#8b949e")));
    if (count >= 0)
        item->setToolTip(tip);
    return item;
}

static void setBranchChanges(QTableWidgetItem *item, int files, int added,
                             int removed)
{
    item->setData(kBranchFilesRole, files);
    item->setData(kBranchAddedRole, added);
    item->setData(kBranchRemovedRole, removed);
    item->setData(kTableSortRole, added + removed);
    item->setText(QString::fromUtf8("+%1 \xE2\x88\x92%2 %3 %4")
                      .arg(added)
                      .arg(removed)
                      .arg(files)
                      .arg(files == 1 ? QStringLiteral("file")
                                      : QStringLiteral("files")));
    item->setToolTip(QStringLiteral("%1 line(s) added, %2 removed across %3 file(s)")
                         .arg(added)
                         .arg(removed)
                         .arg(files));
}

static QTableWidgetItem *branchStateItem(const QString &base, bool isBase,
                                         bool known, int ahead, int behind,
                                         bool conflict,
                                         const MainWindow::BranchRefHistory &history)
{
    QString state;
    QString tip;
    QColor tone = Theme::remapColor(QColor("#8b949e"));
    if (isBase) {
        state = QStringLiteral("default");
        tip = QStringLiteral("The default branch");
        tone = QColor();
    } else if (conflict) {
        state = kBranchConflictsState;
        tip = QStringLiteral("Merging %1 into this branch conflicts").arg(base);
        tone = QColor("#f85149");
    } else if (!known) {
        tip = QStringLiteral("Not compared with %1").arg(base);
    } else if (ahead == 0 && branchNeverCommitted(history)) {
        state = QStringLiteral("new");
        tip = QStringLiteral("No commits yet. Created %1%2.")
                  .arg(branchStampTip(history.createdAt),
                       history.createdFrom.isEmpty()
                           ? QString()
                           : QStringLiteral(" from %1").arg(history.createdFrom));
        if (behind > 0)
            tip += QStringLiteral("\n%1 has %2 newer commit(s)").arg(base).arg(behind);
        tone = Theme::remapColor(QColor("#58a6ff"));
    } else if (ahead == 0) {
        state = QStringLiteral("merged");
        tip = history.ownUpdates > 0
                  ? QStringLiteral("All %1 update(s) made on this branch are in %2")
                        .arg(history.ownUpdates)
                        .arg(base)
                  : QStringLiteral("No commits of its own: %1 already has all of them")
                        .arg(base);
        tone = Theme::nextOr(Theme::Role::AccentFg, QColor("#a371f7"));
    } else if (behind == 0) {
        state = QStringLiteral("up to date");
        tip = QStringLiteral("%1 commit(s) not in %2, and nothing new in %2")
                  .arg(ahead)
                  .arg(base);
    } else {
        state = QStringLiteral("diverged");
        tip = QStringLiteral("%1 commit(s) not in %2, and %3 new in %2")
                  .arg(ahead)
                  .arg(base)
                  .arg(behind);
    }
    auto *item = new QTableWidgetItem(state);
    item->setToolTip(tip);
    if (tone.isValid())
        item->setForeground(tone);
    return item;
}

static void setBranchDetailCells(QTableWidget *table, int row,
                                 const QString &base, const QString &sha,
                                 const QString &subject, const QString &author,
                                 qint64 time, int ahead, int behind,
                                 const MainWindow::BranchRefHistory &history)
{
    table->setItem(row, MainWindow::kBranchesBehindColumn,
                   branchCountItem(behind, QColor("#d29922"),
                                   QStringLiteral("%1 commit(s) in %2 that this "
                                                  "branch does not have")
                                       .arg(behind)
                                       .arg(base)));
    table->setItem(row, MainWindow::kBranchesAheadColumn,
                   branchCountItem(ahead, QColor("#58a6ff"),
                                   QStringLiteral("%1 commit(s) on this branch "
                                                  "that %2 does not have")
                                       .arg(ahead)
                                       .arg(base)));

    auto *commit = new QTableWidgetItem(sha);
    commit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    commit->setForeground(Theme::remapColor(QColor("#8b949e")));
    if (!sha.isEmpty())
        commit->setToolTip(QStringLiteral("Tip commit %1").arg(sha));
    table->setItem(row, MainWindow::kBranchesCommitColumn, commit);

    auto *message = new QTableWidgetItem(subject);
    if (!subject.isEmpty())
        message->setToolTip(subject);
    table->setItem(row, MainWindow::kBranchesMessageColumn, message);

    auto *by = new QTableWidgetItem(author);
    by->setForeground(Theme::remapColor(QColor("#8b949e")));
    table->setItem(row, MainWindow::kBranchesAuthorColumn, by);

    auto *created = branchSortItem(branchStampText(history.createdAt),
                                   history.createdAt);
    created->setForeground(Theme::remapColor(QColor("#8b949e")));
    if (history.createdAt > 0)
        created->setToolTip(
            QStringLiteral("Created %1%2")
                .arg(branchStampTip(history.createdAt),
                     history.createdFrom.isEmpty()
                         ? QString()
                         : QStringLiteral("\nfrom %1").arg(history.createdFrom)));
    table->setItem(row, MainWindow::kBranchesCreatedColumn, created);

    const qint64 changed = qMax(time, history.movedAt);
    auto *updated = branchSortItem(branchStampText(changed), changed);
    updated->setForeground(Theme::remapColor(QColor("#8b949e")));
    QStringList updatedTip;
    if (time > 0)
        updatedTip << QStringLiteral("Tip commit: %1").arg(branchStampTip(time));
    if (history.movedAt > 0)
        updatedTip << QStringLiteral("Branch last moved: %1")
                          .arg(branchStampTip(history.movedAt));
    if (history.moves > 0)
        updatedTip << QStringLiteral("%1 move(s) since it was created, %2 of them "
                                     "its own work")
                          .arg(history.moves)
                          .arg(history.ownUpdates);
    updated->setToolTip(updatedTip.join(QLatin1Char('\n')));
    table->setItem(row, MainWindow::kBranchesUpdatedColumn, updated);
}

static QString diffHealthTone(int score)
{
    if (score >= 85)
        return QStringLiteral("green");
    if (score >= 60)
        return QStringLiteral("orange");
    return QStringLiteral("red");
}

static void applyDiffHealthMetric(QLabel *label,
                                  const DiffHealthMetric &metric)
{
    if (!label)
        return;
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString tone = diffHealthTone(metric.score);
    QString accent;
    QString border;
    QString background;
    if (tone == QLatin1String("green")) {
        accent = dark ? QStringLiteral("#3fb950") : QStringLiteral("#1a7f37");
        border = dark ? QStringLiteral("#238636") : QStringLiteral("#2da44e");
        background = dark ? QStringLiteral("#14261a") : QStringLiteral("#dafbe1");
    } else if (tone == QLatin1String("orange")) {
        accent = dark ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
        border = dark ? QStringLiteral("#9e6a03") : QStringLiteral("#bf8700");
        background = dark ? QStringLiteral("#2d240f") : QStringLiteral("#fff8c5");
    } else {
        accent = dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
        border = dark ? QStringLiteral("#da3633") : QStringLiteral("#cf222e");
        background = dark ? QStringLiteral("#351617") : QStringLiteral("#ffebe9");
    }
    const QString foreground =
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    label->setText(
        QString::fromUtf8("<span style='color:%1'>\xE2\x97\x8F</span>&nbsp;"
                          "<span style='color:%2;font-weight:600'>%3</span>"
                          "&nbsp;<span style='color:%1;font-weight:700'>%4</span>")
            .arg(accent, foreground, metric.label.toHtmlEscaped())
            .arg(metric.score));
    label->setStyleSheet(Theme::remapSheet(
        QStringLiteral("QLabel{background:%1;border:1px solid %2;"
                       "border-radius:8px;padding:1px 6px;}")
            .arg(background, border)));
    label->setProperty("metricKey", metric.key);
    label->setProperty("healthScore", metric.score);
    label->setProperty("healthTone", tone);
    label->setAccessibleName(
        QStringLiteral("%1 health score: %2 out of 100, %3")
            .arg(metric.label)
            .arg(metric.score)
            .arg(tone));
    label->setAccessibleDescription(metric.detail);
    label->setToolTip(
        QString::fromUtf8("%1: %2/100 \xC2\xB7 %3\n%4\n\n"
                          "Green 85\xE2\x80\x93" "100 \xC2\xB7 orange "
                          "60\xE2\x80\x93" "84 \xC2\xB7 red 0\xE2\x80\x93" "59")
            .arg(metric.label)
            .arg(metric.score)
            .arg(tone, metric.detail));
}

class BranchCellDelegate : public SelectionBorderRowDelegate
{
public:
    explicit BranchCellDelegate(QAbstractItemView *view)
        : SelectionBorderRowDelegate(view)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = SelectionBorderRowDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 29));
        return size;
    }

#ifdef FORKMESH_WINDOW_TESTS
    bool testStyleLayerHasNoContent(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
    {
        const QStyleOptionViewItem background =
            backgroundStyleOption(option, index);
        return background.text.isEmpty() && background.icon.isNull() &&
               !(background.features & QStyleOptionViewItem::HasDisplay) &&
               !(background.features & QStyleOptionViewItem::HasDecoration);
    }

    bool testSelectionKeepsNormalTextColor(const QStyleOptionViewItem &option,
                                           const QModelIndex &index) const
    {
        QStyleOptionViewItem selected(option);
        selected.state |= QStyle::State_Selected;
        QStyleOptionViewItem normal(option);
        normal.state &= ~QStyle::State_Selected;
        return primaryTextColor(selected, index) == primaryTextColor(normal, index);
    }
#endif

protected:
    void paintBackground(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
    {
        const QStyleOptionViewItem background =
            backgroundStyleOption(option, index);
        QStyle *style = option.widget ? option.widget->style()
                                      : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter,
                           option.widget);
        paintRowSelectionBorder(painter, option, index);
    }

    static QColor primaryTextColor(const QStyleOptionViewItem &option,
                                   const QModelIndex &index)
    {
        const QVariant foreground = index.data(Qt::ForegroundRole);
        return foreground.canConvert<QBrush>()
                   ? foreground.value<QBrush>().color()
                   : option.palette.color(QPalette::Text);
    }

private:
    QStyleOptionViewItem backgroundStyleOption(
        const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        background.features &= ~QStyleOptionViewItem::HasDisplay;
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        background.state &= ~QStyle::State_MouseOver;
        background.state &= ~QStyle::State_Selected;
        return background;
    }
};

class BranchOverviewDelegate : public BranchCellDelegate
{
public:
    using BranchCellDelegate::BranchCellDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintBackground(painter, option, index);

        painter->save();
        painter->setClipRect(option.rect);
        const int cy = option.rect.center().y();
        int x = option.rect.left() + 8;

        constexpr int iconSlot = 20;
        const QIcon statusIcon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!statusIcon.isNull())
            statusIcon.paint(painter, QRect(x, cy - 7, 14, 14));
        x += iconSlot;

        const QString healthIcon = branchHealthIconName(
            index.data(kBranchConflictRole).toBool(),
            index.data(kBranchBehindRole).isValid()
                ? index.data(kBranchBehindRole).toInt()
                : -1);
        if (!healthIcon.isEmpty()) {
            const QColor healthColor = Theme::nextOr(
                Theme::Role::Warning,
                QColor(currentThemeIsDark() ? "#e3742f" : "#bc4c00"));
            themedOcticon(healthIcon, healthColor, 13)
                .paint(painter, QRect(x, cy - 7, 14, 14));
        }
        x += iconSlot;

        painter->setPen(primaryTextColor(option, index));
        const QRect titleRect(x, option.rect.top(),
                              qMax(0, option.rect.right() - 5 - x),
                              option.rect.height());
        painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(painter->font())
                              .elidedText(index.data(Qt::DisplayRole).toString(),
                                          Qt::ElideRight, titleRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = BranchCellDelegate::sizeHint(option, index);
        size.setWidth(size.width() + 40);
        return size;
    }
};

class BranchLinksDelegate : public BranchCellDelegate
{
public:
    using BranchCellDelegate::BranchCellDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintBackground(painter, option, index);
        const QStringList links = index.data(kBranchLinksRole).toStringList();
        if (links.isEmpty())
            return;

        painter->save();
        painter->setClipRect(option.rect);
        const QFont font = pillFont(option.font);
        const QFontMetrics fm(font);
        const bool dark = currentThemeIsDark();
        const int cy = option.rect.center().y();
        const int right = option.rect.right() - kMargin;
        painter->setFont(font);
        painter->setRenderHint(QPainter::Antialiasing, true);
        int left = option.rect.left() + kMargin;
        for (const QString &link : links) {
            const int width = fm.horizontalAdvance(link) + 2 * kPad;
            if (left + width > right)
                break;
            painter->setPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")));
            painter->setBrush(Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa")));
            painter->drawRoundedRect(
                QRectF(left + 0.5, cy - kHeight / 2 + 0.5, width - 1, kHeight - 1),
                kHeight / 2.0, kHeight / 2.0);
            painter->setPen(Theme::remapColor(QColor(dark ? "#c9d1d9" : "#424a53")));
            painter->drawText(QRect(left, cy - kHeight / 2, width, kHeight),
                              Qt::AlignCenter, link);
            left += width + kGap;
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = BranchCellDelegate::sizeHint(option, index);
        const QFontMetrics fm(pillFont(option.font));
        int width = 2 * kMargin;
        for (const QString &link : index.data(kBranchLinksRole).toStringList())
            width += fm.horizontalAdvance(link) + 2 * kPad + kGap;
        size.setWidth(width);
        return size;
    }

private:
    static constexpr int kHeight = 17;
    static constexpr int kPad = 6;
    static constexpr int kGap = 4;
    static constexpr int kMargin = 6;

    static QFont pillFont(QFont font)
    {
        if (font.pointSizeF() > 0)
            font.setPointSizeF(font.pointSizeF() * 0.85);
        return font;
    }
};

class BranchChangesDelegate : public BranchCellDelegate
{
public:
    using BranchCellDelegate::BranchCellDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintBackground(painter, option, index);
        const QVariant filesValue = index.data(kBranchFilesRole);
        if (!filesValue.isValid())
            return;
        const int files = filesValue.toInt();
        const int added = index.data(kBranchAddedRole).toInt();
        const int removed = index.data(kBranchRemovedRole).toInt();

        painter->save();
        painter->setClipRect(option.rect);
        const QFontMetrics fm(painter->font());
        const QRect rect = option.rect.adjusted(6, 0, -6, 0);
        int x = rect.left();
        const auto segment = [&](const QString &text, const QColor &color) {
            painter->setPen(color);
            painter->drawText(QRect(x, rect.top(), qMax(0, rect.right() - x),
                                    rect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, text);
            x += fm.horizontalAdvance(text) + 6;
        };
        const bool dark = currentThemeIsDark();
        segment(QStringLiteral("+%1").arg(added),
                QColor(dark ? "#3fb950" : "#1a7f37"));
        segment(QString::fromUtf8("\xE2\x88\x92%1").arg(removed),
                QColor(dark ? "#f85149" : "#cf222e"));
        segment(files == 1 ? QStringLiteral("1 file")
                           : QStringLiteral("%1 files").arg(files),
                Theme::remapColor(QColor("#8b949e")));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = BranchCellDelegate::sizeHint(option, index);
        size.setWidth(size.width() + 12);
        return size;
    }
};

static QString branchConflictKey(const QString &dir, const QString &baseSha,
                                 const QString &branchSha)
{
    if (dir.isEmpty() || baseSha.isEmpty() || branchSha.isEmpty())
        return QString();
    return dir + QLatin1Char('\n') + baseSha + QLatin1Char('\n') + branchSha;
}


static const QString kMergeNotesSetting = QStringLiteral("branches/mergeNotes");

static constexpr qint64 kMergeNoteLifetimeMs = 48LL * 60 * 60 * 1000;

static constexpr int kMergeCelebrationRowLimit = 12;

struct MergeNote {
    QString mergeCommit; // ties the note back to the merge commit exactly
    QString branch;
    qint64 mergedAtMs = 0;
    bool byAgent = false;
    QString actor;    // "Opus 5" for an agent, the account name for a person
    QString detail;
    QString provider; // provider + model pick the agent's portrait
    QString model;
};

static QList<MergeNote> parseMergeNotes(const QByteArray &json)
{
    QList<MergeNote> notes;
    for (const QJsonValue &value : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = value.toObject();
        MergeNote note;
        note.mergeCommit = object.value(QStringLiteral("commit")).toString();
        note.branch = object.value(QStringLiteral("branch")).toString();
        note.mergedAtMs =
            static_cast<qint64>(object.value(QStringLiteral("at")).toDouble());
        note.byAgent = object.value(QStringLiteral("agent")).toBool();
        note.actor = object.value(QStringLiteral("actor")).toString();
        note.detail = object.value(QStringLiteral("detail")).toString();
        note.provider = object.value(QStringLiteral("provider")).toString();
        note.model = object.value(QStringLiteral("model")).toString();
        if (note.mergeCommit.isEmpty())
            continue;
        notes.append(note);
    }
    return notes;
}

static QByteArray serializeMergeNotes(const QList<MergeNote> &notes)
{
    QJsonArray stored;
    for (const MergeNote &note : notes)
        stored.append(QJsonObject{
            {QStringLiteral("commit"), note.mergeCommit},
            {QStringLiteral("branch"), note.branch},
            {QStringLiteral("at"), double(note.mergedAtMs)},
            {QStringLiteral("agent"), note.byAgent},
            {QStringLiteral("actor"), note.actor},
            {QStringLiteral("detail"), note.detail},
            {QStringLiteral("provider"), note.provider},
            {QStringLiteral("model"), note.model}});
    return QJsonDocument(stored).toJson(QJsonDocument::Compact);
}

struct MergeNotesFile {
    QString path;
    QList<MergeNote> notes;
};

static MergeNotesFile &mergeNotesFile()
{
    static MergeNotesFile file;
    QSettings settings;
    const QString path = QFileInfo(settings.fileName()).absolutePath() +
                         QStringLiteral("/merge-notes.json");
    if (file.path == path)
        return file;
    file.path = path;
    file.notes.clear();
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        file.notes = parseMergeNotes(existing.readAll());
        return file;
    }
    if (!settings.contains(kMergeNotesSetting))
        return file;
    file.notes =
        parseMergeNotes(settings.value(kMergeNotesSetting).toString().toUtf8());
    if (QString error; !forkmesh::diskwrite::writeNow(
            path, serializeMergeNotes(file.notes), &error)) {
        qWarning() << "merge notes:" << error;
        return file;
    }
    settings.remove(kMergeNotesSetting);
    return file;
}

static QList<MergeNote> loadMergeNotes()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<MergeNote> notes;
    for (const MergeNote &note : std::as_const(mergeNotesFile().notes)) {
        if (now - note.mergedAtMs > kMergeNoteLifetimeMs)
            continue;
        notes.append(note);
    }
    return notes;
}

static void saveMergeNotes(const QList<MergeNote> &notes)
{
    MergeNotesFile &file = mergeNotesFile();
    file.notes = notes;
    forkmesh::diskwrite::writeLater(file.path, serializeMergeNotes(notes));
}

static const AgentSession *sessionForMergedBranch(
    const QList<AgentSession> &sessions, const QString &branch,
    const QString &repoOwner, const QString &repoName)
{
    const AgentSession *best = nullptr;
    for (const AgentSession &session : sessions) {
        if (session.branchName != branch)
            continue;
        if (!repoOwner.isEmpty() &&
            (session.owner != repoOwner || session.name != repoName))
            continue;
        if (!best) {
            best = &session;
            continue;
        }
        const bool live = session.status != AgentStatus::Cleared;
        const bool bestLive = best->status != AgentStatus::Cleared;
        if ((live && !bestLive) || (live == bestLive && session.id > best->id))
            best = &session;
    }
    return best;
}

static void creditFromSession(const AgentSession &session, QString *actor,
                              QString *detail)
{
    const QString model = agentModelLabel(session.model);
    const QString provider = cliProviderLabel(session.provider);
    *actor = !model.isEmpty() ? model
                              : (!provider.isEmpty() ? provider
                                                     : QStringLiteral("Agent"));
    QStringList parts;
    parts << QStringLiteral("Agent #%1").arg(session.id);
    if (!provider.isEmpty() && provider != *actor)
        parts << provider;
    if (session.issueNumber > 0)
        parts << QStringLiteral("issue #%1").arg(session.issueNumber);
    *detail = parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

static void rememberMergedBranch(const QString &mergeCommit, const QString &branch,
                                 const QList<AgentSession> &sessions,
                                 const QString &repoOwner, const QString &repoName,
                                 const QString &personName)
{
    if (mergeCommit.isEmpty() || branch.isEmpty())
        return;
    MergeNote note;
    note.mergeCommit = mergeCommit;
    note.branch = branch;
    note.mergedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (const AgentSession *session =
            sessionForMergedBranch(sessions, branch, repoOwner, repoName)) {
        note.byAgent = true;
        note.provider = session->provider;
        note.model = session->model;
        creditFromSession(*session, &note.actor, &note.detail);
    } else {
        note.actor = personName.trimmed().isEmpty() ? QStringLiteral("You")
                                                    : personName.trimmed();
        note.detail = QStringLiteral("merged by hand");
    }
    QList<MergeNote> notes = loadMergeNotes();
    notes.removeIf([&note](const MergeNote &existing) {
        return existing.mergeCommit == note.mergeCommit;
    });
    notes.append(note);
    // A fleet that lands hundreds of branches a day would otherwise grow the
    // settings entry unbounded within the lifetime window; only the newest
    // notes can still be shown.
    constexpr int kMergeNoteLimit = 200;
    if (notes.size() > kMergeNoteLimit)
        notes = notes.mid(notes.size() - kMergeNoteLimit);
    saveMergeNotes(notes);
}

struct MergeLanding {
    QString sha;
    QString branch; // from the subject; empty when it named none
    QString author;
    qint64 whenSecs = 0;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};

// The churn pass diffs every merge it prints — a quarter of a minute over six
// months of this repository, where the log without it is instant at any width.
// So the window is read in full and its churn only for the newest landings.
// The limit sits well above a day's landings (the window this opens on, and the
// only one the pane used to have) and far above the dozen rows it lists, so
// what it costs is what the day already cost; a window wide enough to pass it
// reports its totals as partial rather than showing a fraction as the whole.
static constexpr int kMergeChurnScanLimit = 60;

struct MergeChurn {
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};

static QList<MergeLanding> mergesLandedIn(const QString &dir, const QString &base,
                                          int range, bool *churnComplete = nullptr)
{
    if (churnComplete)
        *churnComplete = true;
    QList<MergeLanding> landings;
    if (dir.isEmpty() || base.isEmpty())
        return landings;
    const QString since = QDateTime::currentDateTime()
                              .addDays(-mergeRangeSpec(range).days)
                              .toString(Qt::ISODate);
    QByteArray out;
    if (!runGitCapture(dir,
                       {QStringLiteral("log"), QStringLiteral("--merges"),
                        QStringLiteral("--first-parent"),
                        QStringLiteral("--since=%1").arg(since),
                        QStringLiteral("--pretty=format:%H%x1f%ct%x1f%an%x1f%s"),
                        base},
                       &out, nullptr))
        return landings;
    const QStringList records =
        QString::fromUtf8(out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &record : records) {
        const QStringList fields = record.split(QChar(0x1f));
        if (fields.size() < 4)
            continue;
        MergeLanding landing;
        landing.sha = fields.at(0);
        landing.whenSecs = fields.at(1).toLongLong();
        landing.author = fields.at(2);
        landing.branch = mergedBranchFromMergeSubject(fields.at(3));
        landings.append(landing);
    }
    if (landings.isEmpty())
        return landings;
    if (churnComplete)
        *churnComplete = landings.size() <= kMergeChurnScanLimit;

    QByteArray stats;
    if (!runGitCapture(dir,
                       {QStringLiteral("log"), QStringLiteral("--merges"),
                        QStringLiteral("--first-parent"),
                        QStringLiteral("--diff-merges=first-parent"),
                        QStringLiteral("--shortstat"), QStringLiteral("-n"),
                        QString::number(kMergeChurnScanLimit),
                        QStringLiteral("--since=%1").arg(since),
                        QStringLiteral("--pretty=format:%x1e%H"), base},
                       &stats, nullptr))
        return landings;
    static const QRegularExpression shortstat(
        QStringLiteral("(\\d+) files? changed"
                       "(?:, (\\d+) insertions?\\(\\+\\))?"
                       "(?:, (\\d+) deletions?\\(-\\))?"));
    QHash<QString, MergeChurn> churn;
    const QStringList statRecords =
        QString::fromUtf8(stats).split(QChar(0x1e), Qt::SkipEmptyParts);
    for (const QString &record : statRecords) {
        const QStringList lines = record.split(QLatin1Char('\n'));
        const QString sha = lines.value(0).trimmed();
        if (sha.isEmpty())
            continue;
        for (int i = 1; i < lines.size(); ++i) {
            const QRegularExpressionMatch match = shortstat.match(lines.at(i));
            if (!match.hasMatch())
                continue;
            MergeChurn entry;
            entry.files = match.captured(1).toInt();
            entry.insertions = match.captured(2).toInt();
            entry.deletions = match.captured(3).toInt();
            churn.insert(sha, entry);
            break;
        }
    }
    for (MergeLanding &landing : landings) {
        const auto entry = churn.constFind(landing.sha);
        if (entry == churn.constEnd())
            continue;
        landing.files = entry->files;
        landing.insertions = entry->insertions;
        landing.deletions = entry->deletions;
    }
    return landings;
}

// Velocity does not need subjects, attribution, or churn. The cards' top/avg
// figures span the whole first-parent merge history (the busiest 24 hours
// ever, not just the busiest hour of today), so this timestamp-only walk reads
// every merge without paying for a shortstat pass over any of them.
static MergeVelocity readMergeVelocity(const QString &dir, const QString &base,
                                       qint64 nowSecs)
{
    if (dir.isEmpty() || base.isEmpty())
        return {};
    QByteArray out;
    if (!runGitCapture(dir,
                       {QStringLiteral("log"), QStringLiteral("--merges"),
                        QStringLiteral("--first-parent"),
                        QStringLiteral("--pretty=format:%ct"), base},
                       &out, nullptr))
        return {};
    QList<qint64> timestamps;
    for (const QByteArray &line : out.split('\n')) {
        bool ok = false;
        const qint64 timestamp = line.trimmed().toLongLong(&ok);
        if (ok)
            timestamps.append(timestamp);
    }
    return mergeVelocityForTimestamps(timestamps, nowSecs);
}

// Everything expensive behind the post-merge celebration is plain repository
// data. Read it together on a worker so deleting a branch never makes the GUI
// thread walk and diff dozens of historical merges before it can finish the
// click. QPixmap/avatar creation deliberately remains in the GUI-thread render
// half below.
struct MergeCelebrationRead {
    QList<MergeLanding> landings;
    QSet<QString> liveBranches;
    MergeVelocity velocity;
    bool churnComplete = true;
    int commits = -1;
    QString celebrated;
};

static MergeCelebrationRead readMergeCelebration(const QString &dir,
                                                  const QString &base,
                                                  const QString &requested,
                                                  int range,
                                                  bool newestIfNone = false)
{
    MergeCelebrationRead read;
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    read.landings =
        mergesLandedIn(dir, base, range, &read.churnComplete);
    read.velocity = readMergeVelocity(dir, base, nowSecs);

    QByteArray refs;
    if (runGitCapture(dir,
                      {QStringLiteral("for-each-ref"),
                       QStringLiteral("--format=%(refname:short)"),
                       QStringLiteral("refs/heads")},
                      &refs, nullptr)) {
        for (const QString &ref : QString::fromUtf8(refs).split(
                 QLatin1Char('\n'), Qt::SkipEmptyParts))
            read.liveBranches.insert(ref.trimmed());
    }

    QString celebrated = requested;
    if (celebrated.isEmpty() && newestIfNone) {
        const auto newest = std::find_if(
            read.landings.cbegin(), read.landings.cend(),
            [](const MergeLanding &landing) { return !landing.branch.isEmpty(); });
        if (newest != read.landings.cend())
            celebrated = newest->branch;
    }
    read.celebrated = celebrated;
    const auto current = std::find_if(
        read.landings.cbegin(), read.landings.cend(),
        [&celebrated](const MergeLanding &landing) {
            return !celebrated.isEmpty() && landing.branch == celebrated;
        });
    if (current != read.landings.cend() && !current->sha.isEmpty()) {
        QByteArray out;
        if (runGitCapture(dir,
                          {QStringLiteral("rev-list"), QStringLiteral("--count"),
                           QStringLiteral("%1^1..%1").arg(current->sha)},
                          &out, nullptr))
            read.commits = QString::fromUtf8(out).trimmed().toInt();
    }
    return read;
}

// A total summed over the part of a window the churn pass reached would read as
// the whole of it, so a partial one is not shown at all.
static void stripMergeTallyChurn(QList<MergeActorTally> *tallies)
{
    for (MergeActorTally &tally : *tallies) {
        tally.files = 0;
        tally.insertions = 0;
        tally.deletions = 0;
    }
}

static QPixmap mergeCreditAvatar(bool byAgent, const QString &provider,
                                 const QString &model, const QString &person,
                                 const QString &me, const QByteArray &myAvatarPng,
                                 int size)
{
    if (byAgent) {
        const QIcon icon =
            model.trimmed().compare(kClaudeAutoModelId, Qt::CaseInsensitive) == 0
                ? agentControlIcon(7)
                : agentModelIcon(provider, model);
        return icon.pixmap(QSize(size, size), iconDevicePixelRatio());
    }
    const QString seed = person.trimmed().toLower();
    if (seed.isEmpty())
        return QPixmap();
    if (!me.trimmed().isEmpty() && !myAvatarPng.isEmpty() &&
        person.trimmed().compare(me.trimmed(), Qt::CaseInsensitive) == 0)
        return roundedAvatar(myAvatarPng, size, 0.5);
    return roundedAvatar(forkMeshAvatarPng(seed), size, 0.5);
}

static QList<MergeCelebrationRow> mergeCelebrationRows(
    const QString &dir, const QString &base, const QList<MergeLanding> &landings,
    const QList<AgentSession> &sessions, const QString &repoOwner,
    const QString &repoName, const QString &me, const QByteArray &myAvatarPng,
    const QString &celebrated, int avatarSize, int heroAvatarSize,
    const QSet<QString> *knownLiveBranches = nullptr)
{
    QHash<QString, MergeNote> notesByCommit;
    QHash<QString, MergeNote> notesByBranch;
    for (const MergeNote &note : loadMergeNotes()) {
        notesByCommit.insert(note.mergeCommit, note);
        if (!note.branch.isEmpty())
            notesByBranch.insert(note.branch, note);
    }
    QSet<QString> liveBranches = knownLiveBranches ? *knownLiveBranches
                                                   : QSet<QString>();
    if (!knownLiveBranches) {
        QByteArray refs;
        if (runGitCapture(dir,
                          {QStringLiteral("for-each-ref"),
                           QStringLiteral("--format=%(refname:short)"),
                           QStringLiteral("refs/heads")},
                          &refs, nullptr)) {
            for (const QString &ref : QString::fromUtf8(refs).split(
                     QLatin1Char('\n'), Qt::SkipEmptyParts))
                liveBranches.insert(ref.trimmed());
        }
    }

    // A six-month window names the same handful of faces over and over, and
    // each one is a rendered pixmap: build each size of each face once.
    QHash<QString, QPixmap> avatars;
    QList<MergeCelebrationRow> rows;
    for (const MergeLanding &landing : landings) {
        MergeCelebrationRow row;
        row.branch = landing.branch;
        row.mergeCommit = landing.sha;
        row.branchStillExists = !landing.branch.isEmpty() &&
                                landing.branch != base &&
                                liveBranches.contains(landing.branch);
        row.whenSecs = landing.whenSecs;
        row.files = landing.files;
        row.insertions = landing.insertions;
        row.deletions = landing.deletions;
        row.current = !celebrated.isEmpty() && landing.branch == celebrated;
        // A caller that is merely pointing at an older landing clears this.
        row.justNow = row.current;

        QString provider;
        QString model;
        const auto note = notesByCommit.constFind(landing.sha);
        const auto branchNote = landing.branch.isEmpty()
                                    ? notesByBranch.constEnd()
                                    : notesByBranch.constFind(landing.branch);
        if (note != notesByCommit.constEnd() || branchNote != notesByBranch.constEnd()) {
            const MergeNote &credit =
                note != notesByCommit.constEnd() ? note.value() : branchNote.value();
            row.byAgent = credit.byAgent;
            row.actor = credit.actor;
            row.detail = credit.detail;
            provider = credit.provider;
            model = credit.model;
        } else if (const AgentSession *session = sessionForMergedBranch(
                       sessions, landing.branch, repoOwner, repoName)) {
            row.byAgent = true;
            provider = session->provider;
            model = session->model;
            creditFromSession(*session, &row.actor, &row.detail);
        } else {
            row.actor = landing.author;
        }
        const int avatarPx = row.current ? heroAvatarSize : avatarSize;
        const QString avatarKey =
            (row.byAgent ? QStringLiteral("1|") : QStringLiteral("0|")) + provider +
            QLatin1Char('|') + model + QLatin1Char('|') + row.actor +
            QLatin1Char('|') + QString::number(avatarPx);
        const auto cached = avatars.constFind(avatarKey);
        if (cached != avatars.constEnd()) {
            row.avatar = cached.value();
        } else {
            row.avatar = mergeCreditAvatar(row.byAgent, provider, model, row.actor,
                                           me, myAvatarPng, avatarPx);
            avatars.insert(avatarKey, row.avatar);
        }
        rows.append(row);
    }
    return rows;
}

QWidget *MainWindow::buildWorktreesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel("Worktrees");
    heading->setObjectName("channelTitle");
    m_worktreesSummary = new QLabel;
    m_worktreesSummary->setObjectName("statusLine");
    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::loadWorktreesPanel);
    auto *pruneButton = new QPushButton("Prune");
    pruneButton->setObjectName("ghostButton");
    pruneButton->setCursor(Qt::PointingHandCursor);
    setOcticon(pruneButton, "trash", 16);
    pruneButton->setToolTip("Drop registrations for worktrees whose folders are gone");
    connect(pruneButton, &QPushButton::clicked, this, [this] {
        QString repoPath;
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
            repoPath = m_repositories.at(m_repoDetailIndex).localPath;
        if (repoPath.isEmpty()) {
            loadWorktreesPanel();
            return;
        }
        runGitDetached(repoPath, {QStringLiteral("worktree"), QStringLiteral("prune")},
                       [this, repoPath](bool, const QByteArray &) {
                           if (repoDetailShows(repoPath))
                               loadWorktreesPanel();
                       });
    });
    headerRow->addWidget(heading);
    headerRow->addWidget(m_worktreesSummary);
    headerRow->addStretch();
    headerRow->addWidget(pruneButton);
    headerRow->addWidget(refreshButton);
    layout->addLayout(headerRow);

    auto *info = new QLabel("Each agent works in its own worktree + branch, so "
                            "concurrent agents never share a working tree.");
    info->setObjectName("statusLine");
    info->setWordWrap(true);
    layout->addWidget(info);

    m_worktreesTable = new QTableWidget(0, 5);
    installColumnHeaderMenu(m_worktreesTable);
    m_worktreesTable->setObjectName("issueTable");
    enableHoverRowHighlight(m_worktreesTable);
    m_worktreesTable->setHorizontalHeaderLabels(
        {"Branch", "Path", "Status", "Ahead/Behind", ""});
    m_worktreesTable->verticalHeader()->setVisible(false);
    m_worktreesTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_worktreesTable->verticalHeader()->setDefaultSectionSize(36);
    m_worktreesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_worktreesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_worktreesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_worktreesTable->setShowGrid(false);
    m_worktreesTable->setWordWrap(false);
    QHeaderView *wh = m_worktreesTable->horizontalHeader();
    wh->setHighlightSections(false);
    wh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    wh->setSectionResizeMode(1, QHeaderView::Stretch);
    wh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    wh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    wh->setSectionResizeMode(4, QHeaderView::Fixed);
    makeColumnsResizable(m_worktreesTable);
    connect(m_worktreesTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                if (QTableWidgetItem *it = m_worktreesTable->item(row, 1))
                    QDesktopServices::openUrl(QUrl::fromLocalFile(it->text()));
            });
    connect(m_worktreesTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                QTableWidgetItem *b = m_worktreesTable->item(row, 0);
                QTableWidgetItem *p = m_worktreesTable->item(row, 1);
                updateWorktreeSelection(
                    b ? b->data(Qt::UserRole).toString() : QString(),
                    p ? p->text() : QString());
            });
    connect(m_worktreesTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column >= 4)
                    return; // action column: its cell widgets own their clicks
                QTableWidgetItem *b = m_worktreesTable->item(row, 0);
                const QString branch =
                    b ? b->data(Qt::UserRole).toString() : QString();
                if (!branch.isEmpty())
                    switchToBranch(branch);
            });

    m_worktreeMergeButton = new QPushButton("Merge into main");
    m_worktreeMergeButton->setObjectName("primaryButton");
    m_worktreeMergeButton->setProperty("buttonSize", "sm");
    m_worktreeMergeButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeMergeButton, "check-circle", 14);
    m_worktreeMergeButton->setToolTip(
        "Merge the selected worktree's branch into the default branch, then delete "
        "the worktree and its branch");
    m_worktreeMergeButton->setEnabled(false);
    connect(m_worktreeMergeButton, &QPushButton::clicked, this, [this] {
        if (!m_worktreeSelectedBranch.isEmpty())
            requestMergeWorktreeIntoMain(m_worktreeSelectedBranch,
                                         m_worktreeSelectedPath);
    });
    m_worktreeMergeDeleteAgentButton = new QPushButton("Merge & clean up");
    m_worktreeMergeDeleteAgentButton->setObjectName("ghostButton");
    m_worktreeMergeDeleteAgentButton->setProperty("buttonSize", "sm");
    m_worktreeMergeDeleteAgentButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeMergeDeleteAgentButton, "check-circle", 14);
    m_worktreeMergeDeleteAgentButton->setToolTip(
        "Merge the selected worktree's branch into the default branch, then delete "
        "the worktree, source branch, pull request and Agent record");
    m_worktreeMergeDeleteAgentButton->setEnabled(false);
    connect(m_worktreeMergeDeleteAgentButton, &QPushButton::clicked, this, [this] {
        if (m_worktreeSelectedBranch.isEmpty())
            return;
        forkmesh::sonify::UiSounds::play(forkmesh::sonify::Cue::Merge);
        requestMergeWorktreeIntoMain(m_worktreeSelectedBranch,
                                     m_worktreeSelectedPath,
                                     /*cleanUp=*/true);
    });
    m_worktreeUpdateButton = new QPushButton("Update from main");
    m_worktreeUpdateButton->setObjectName("ghostButton");
    m_worktreeUpdateButton->setProperty("buttonSize", "sm");
    m_worktreeUpdateButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeUpdateButton, "sync", 14);
    m_worktreeUpdateButton->setToolTip(
        "Merge the default branch into the selected worktree's branch");
    m_worktreeUpdateButton->setEnabled(false);
    connect(m_worktreeUpdateButton, &QPushButton::clicked, this, [this] {
        if (!m_worktreeSelectedPath.isEmpty() && !m_worktreeSelectedBranch.isEmpty())
            updateWorktreeFromMain(m_worktreeSelectedPath, m_worktreeSelectedBranch);
    });
    m_worktreeResolveButton = new QPushButton("Resolve conflicts\xE2\x80\xA6");
    m_worktreeResolveButton->setObjectName("primaryButton");
    m_worktreeResolveButton->setProperty("buttonSize", "sm");
    m_worktreeResolveButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeResolveButton, "alert", 14);
    m_worktreeResolveButton->setToolTip(
        "Open the merge editor to resolve this worktree's conflicts and commit them");
    m_worktreeResolveButton->hide();
    connect(m_worktreeResolveButton, &QPushButton::clicked, this,
            &MainWindow::resolveWorktreeConflicts);
    m_worktreeCommitButton = new QPushButton("Commit changes");
    m_worktreeCommitButton->setObjectName("ghostButton");
    m_worktreeCommitButton->setProperty("buttonSize", "sm");
    m_worktreeCommitButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeCommitButton, "git-branch", 14);
    m_worktreeCommitButton->setToolTip(
        "Stage and commit the selected worktree's uncommitted changes");
    m_worktreeCommitButton->setEnabled(false);
    connect(m_worktreeCommitButton, &QPushButton::clicked, this, [this] {
        if (!m_worktreeSelectedPath.isEmpty() && !m_worktreeSelectedBranch.isEmpty())
            commitWorktreeChanges(m_worktreeSelectedPath, m_worktreeSelectedBranch);
    });
    m_worktreeRemoveButton = new QPushButton("Delete");
    m_worktreeRemoveButton->setObjectName("ghostButton");
    m_worktreeRemoveButton->setProperty("buttonSize", "sm");
    m_worktreeRemoveButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_worktreeRemoveButton, "trash", 14);
    m_worktreeRemoveButton->setToolTip(
        "Remove the selected worktree, delete its branch and its agent session");
    m_worktreeRemoveButton->setEnabled(false);
    connect(m_worktreeRemoveButton, &QPushButton::clicked, this, [this] {
        if (!m_worktreeSelectedPath.isEmpty())
            deleteWorktreeBranchAndAgent(m_worktreeSelectedPath,
                                         m_worktreeSelectedBranch);
    });
    m_worktreeBranchLabel = new QLabel;
    m_worktreeBranchLabel->setObjectName("sectionLabel");
    m_worktreeBranchLabel->setTextFormat(Qt::RichText);
    m_worktreeBranchLabel->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                   Qt::LinksAccessibleByMouse);
    m_worktreeBranchLabel->setOpenExternalLinks(true);
    auto *detailBar = new QHBoxLayout;
    detailBar->setContentsMargins(0, 0, 0, 0);
    detailBar->addWidget(m_worktreeBranchLabel);
    detailBar->addStretch();
    detailBar->addWidget(m_worktreeResolveButton);
    detailBar->addWidget(m_worktreeCommitButton);
    detailBar->addWidget(m_worktreeUpdateButton);
    detailBar->addWidget(m_worktreeMergeButton);
    detailBar->addWidget(m_worktreeMergeDeleteAgentButton);
    detailBar->addWidget(m_worktreeRemoveButton);

    layout->addWidget(m_worktreesTable, 1);
    layout->addLayout(detailBar);
    return page;
}

namespace {

constexpr int kMaxDetachedGitInFlight = 12;

constexpr int kDetachedGitTimeoutMs = 60000;

struct DetachedGitRequest {
    QPointer<MainWindow> window;
    QString dir;
    QStringList args;
    std::function<void(bool, const QByteArray &)> onDone;
    // Rides along to the worker so the footer's outcome line names the caller
    // that queued the read rather than this one shared runner.
    forkmesh::LogSource source;
    // The routine that asked (BackgroundRoutine on the GUI thread at enqueue
    // time) rides along the same way, and is re-installed around onDone so
    // work the callback asks for in turn is attributed to it too. queuedAtMs
    // lets the ticket say how long the read waited for an admission slot.
    QString routine;
    qint64 queuedAtMs = 0;
    bool mergeStderr = false;
};

int g_detachedGitInFlight = 0;
QQueue<DetachedGitRequest> g_detachedGitQueue;

void startDetachedGit(DetachedGitRequest request);

void pumpDetachedGitQueue()
{
    while (g_detachedGitInFlight < kMaxDetachedGitInFlight &&
           !g_detachedGitQueue.isEmpty()) {
        DetachedGitRequest request = g_detachedGitQueue.dequeue();
        if (!request.window)
            continue; // the window closed while this read waited its turn
        startDetachedGit(std::move(request));
    }
}

void startDetachedGit(DetachedGitRequest request)
{
    const QPointer<MainWindow> window = request.window;
    ++g_detachedGitInFlight;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto *watcher = new QFutureWatcher<WorkerProcessResult>(
        QCoreApplication::instance());
    // The watcher outlives the window so a cancelled worker still releases its
    // global admission slot.  Its QPointer below prevents a stale UI callback.
    QObject::connect(window.data(), &QObject::destroyed, watcher,
                     [cancelled](QObject *) {
                         cancelled->store(true, std::memory_order_relaxed);
                     });
    QObject::connect(watcher, &QObject::destroyed,
                     [cancelled](QObject *) {
                         cancelled->store(true, std::memory_order_relaxed);
                     });
    QObject::connect(
        watcher, &QFutureWatcher<WorkerProcessResult>::finished, watcher,
        [watcher, window, onDone = std::move(request.onDone),
         routine = request.routine]() mutable {
        const WorkerProcessResult result = watcher->result();
        --g_detachedGitInFlight;
        watcher->deleteLater();
        if (window && onDone) {
            if (forkmesh::ui::inKeepAlivePump() && window)
                QTimer::singleShot(0, window, [onDone, result, routine] {
                    const forkmesh::BackgroundRoutine scope(routine);
                    onDone(result.ok, result.output);
                });
            else {
                const forkmesh::BackgroundRoutine scope(routine);
                onDone(result.ok, result.output);
            }
        }
        pumpDetachedGitQueue();
    });

    WorkerProcessRequest processRequest;
    processRequest.program = QStringLiteral("git");
    processRequest.arguments = request.args;
    processRequest.workingDirectory = request.dir;
    processRequest.environment = QProcessEnvironment::systemEnvironment();
    processRequest.environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    processRequest.timeoutMs = kDetachedGitTimeoutMs;
    processRequest.activityKind = QStringLiteral("git");
    processRequest.activityDetail =
        gitArgsLine(QStringLiteral("git"), request.args);
    processRequest.activityRoutine = request.routine;
    if (request.mergeStderr)
        processRequest.channelMode = QProcess::MergedChannels;
    processRequest.activityQueuedMs =
        request.queuedAtMs > 0
            ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() -
                                  request.queuedAtMs)
            : -1;
    watcher->setFuture(QtConcurrent::run(
        [processRequest = std::move(processRequest), cancelled,
         source = request.source] {
            return runWorkerProcess(processRequest, cancelled, source);
        }));
}

} // namespace

void MainWindow::runGitDetached(const QString &dir, const QStringList &args,
                                std::function<void(bool, const QByteArray &)> onDone,
                                const forkmesh::LogSource &source,
                                bool mergeStderr)
{
    g_detachedGitQueue.enqueue(DetachedGitRequest{
        this, dir, args, std::move(onDone), source,
        forkmesh::BackgroundRoutine::current(),
        QDateTime::currentMSecsSinceEpoch(), mergeStderr});
    pumpDetachedGitQueue();
}

void MainWindow::focusRepoDetailTable(int id)
{
    // The list table at the heart of each repo-detail tab. Tabs that aren't a
    // single scrollable list (Code, Insights, Security, Settings, …) map to
    // nullptr and are left alone.
    QTableWidget *table = nullptr;
    if (id == 1)
        table = m_commitsTable;
    else if (id == 2)
        table = m_issueTable;
    else if (id == 3)
        table = m_agentTable;
    else if (id == 4)
        table = m_pullTable;
    else if (id == 5)
        table = m_discussionTable;
    else if (id == 6)
        table = m_actionsTable;
    else if (id == m_branchesTabIndex)
        table = m_branchesTable;
    else if (id == m_worktreesTabIndex)
        table = m_worktreesTable;
    else if (id == m_releasesTabIndex)
        table = m_releasesTable;
    else if (id == m_artifactsTabIndex)
        table = m_artifactsTable;
    else if (id == m_projectsTabIndex)
        table = m_projectTable;
    if (table && table->isVisible() && table->isEnabled())
        table->setFocus(Qt::OtherFocusReason);
}

bool MainWindow::deferUiRefresh(unsigned kind)
{
    if (m_uiRefreshBatchDepth <= 0)
        return false;
    m_uiRefreshPending |= kind;
    return true;
}

void MainWindow::runPendingUiRefreshes()
{
    const unsigned pending = m_uiRefreshPending;
    m_uiRefreshPending = 0;
    if (!pending)
        return;
    if (pending & UiRefreshSourceControlForce)
        refreshSourceControl(true);
    else if (pending & UiRefreshSourceControl)
        refreshSourceControl(false);
    if (pending & UiRefreshAgents)
        // Whether the deferred reload wanted a rescan is carried in
        // m_agentRescanDeferred, which reloadAgents() folds in itself.
        reloadAgents(AgentReloadScan::FromCache);
    else if (pending & UiRefreshAgentState) {
        refreshAgentTable();
        refreshAgentModelSelectors();
        if (m_selectedAgentSessionId > 0)
            showAgentSession(m_selectedAgentSessionId);
    }
    if (pending & UiRefreshWorktrees)
        loadWorktreesPanel();
    if (pending & UiRefreshBranches)
        loadBranchesAndTags();
    if (pending & UiRefreshIssueReload) {
        reloadIssues();
    } else if (pending & UiRefreshIssues) {
        refreshIssueList();
        updateIssueActionState();
    }
}

void MainWindow::loadWorktreesPanel()
{
    if (deferUiRefresh(UiRefreshWorktrees))
        return;
    if (!m_worktreesTable)
        return;
    const QString keepPath = m_worktreeSelectedPath;
    TableRepaintGuard repaintGuard(m_worktreesTable);
    m_worktreesTable->setRowCount(0);
    QString repoPath, repoOwner, repoName;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        repoPath = m_repositories.at(m_repoDetailIndex).localPath;
        repoOwner = m_repositories.at(m_repoDetailIndex).owner;
        repoName = m_repositories.at(m_repoDetailIndex).name;
    }
    if (repoPath.isEmpty() || !QDir(repoPath).exists(QStringLiteral(".git"))) {
        if (m_worktreesSummary)
            m_worktreesSummary->setText(QStringLiteral("· no local checkout"));
        if (auto *b = dynamic_cast<VerticalIconButton *>(m_worktreesButton))
            b->setBadgeCount(0);
        return;
    }

    struct WT { QString path, branch, head; bool detached = false; };
    QList<WT> wts;
    WT cur;
    bool have = false;
    QByteArray out;
    if (worktreeListForCaller(repoPath, &out)) {
        for (const QString &raw : QString::fromUtf8(out).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty()) {
                if (have) wts.append(cur);
                cur = WT();
                have = false;
                continue;
            }
            have = true;
            if (line.startsWith(QLatin1String("worktree ")))
                cur.path = line.mid(9);
            else if (line.startsWith(QLatin1String("HEAD ")))
                cur.head = line.mid(5);
            else if (line.startsWith(QLatin1String("branch ")))
                cur.branch = line.mid(7).replace(QLatin1String("refs/heads/"), QString());
            else if (line == QLatin1String("detached"))
                cur.detached = true;
        }
        if (have) wts.append(cur);
    }

    wts.erase(std::remove_if(wts.begin(), wts.end(), [](const WT &wt) {
                  return wt.branch == QLatin1String("forkmesh/pulls") ||
                         wt.branch.startsWith(
                             QLatin1String("forkmesh/pull/"));
              }),
              wts.end());

    const QString mainPath = QDir(repoPath).absolutePath();
    const QString baseBranch = repoDefaultBranch(repoBranches());
    const int statusGen = ++m_worktreeStatusGen;
    int actionWidth = 0;
    for (const WT &wt : wts) {
        const int row = m_worktreesTable->rowCount();
        m_worktreesTable->insertRow(row);
        const bool isMain = QDir(wt.path).absolutePath() == mainPath;

        const QString branchLabel =
            wt.detached ? QStringLiteral("(detached %1)").arg(wt.head.left(8))
                        : (wt.branch.isEmpty() ? QStringLiteral("(none)") : wt.branch);
        auto *bItem = new QTableWidgetItem(branchLabel);
        bItem->setIcon(themedOcticon(isMain ? "check-circle" : "git-branch",
                                     Theme::remapColor(QColor(isMain ? "#3fb950" : "#8b949e")), 14));
        bItem->setData(Qt::UserRole, wt.branch); // real branch for diff/merge
        m_worktreesTable->setItem(row, 0, bItem);
        m_worktreesTable->setItem(row, 1, new QTableWidgetItem(wt.path));

        m_worktreesTable->setItem(
            row, 2,
            new QTableWidgetItem(isMain ? QStringLiteral("main checkout")
                                        : QString::fromUtf8("checking\xE2\x80\xA6")));
        const QString wtPath = wt.path;
        const bool mainRow = isMain;
        runGitDetached(
            wt.path, {QStringLiteral("status"), QStringLiteral("--porcelain")},
            [this, statusGen, wtPath, mainRow](bool ok, const QByteArray &st) {
                if (statusGen != m_worktreeStatusGen || !m_worktreesTable)
                    return; // the table was rebuilt while git ran — drop this result
                const bool dirty =
                    ok && !QString::fromUtf8(st).trimmed().isEmpty();
                QString status = mainRow ? QStringLiteral("main checkout") : QString();
                if (dirty)
                    status = status.isEmpty() ? QStringLiteral("uncommitted changes")
                                              : status + QStringLiteral(" · dirty");
                if (status.isEmpty())
                    status = QStringLiteral("clean");
                const QString want = QDir(wtPath).absolutePath();
                for (int r = 0; r < m_worktreesTable->rowCount(); ++r) {
                    QTableWidgetItem *p = m_worktreesTable->item(r, 1);
                    if (p && QDir(p->text()).absolutePath() == want) {
                        if (QTableWidgetItem *c = m_worktreesTable->item(r, 2))
                            c->setText(status);
                        break;
                    }
                }
            });

        auto *abItem = new QTableWidgetItem(
            baseBranch.isEmpty() ? QStringLiteral("—")
                                 : QString::fromUtf8("checking\xE2\x80\xA6"));
        abItem->setTextAlignment(Qt::AlignCenter);
        m_worktreesTable->setItem(row, 3, abItem);
        if (!baseBranch.isEmpty()) {
            runGitDetached(
                wt.path,
                {QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                 QStringLiteral("--count"),
                 baseBranch + QStringLiteral("...HEAD")},
                [this, statusGen, wtPath, baseBranch](bool ok, const QByteArray &c) {
                    if (statusGen != m_worktreeStatusGen || !m_worktreesTable)
                        return; // table rebuilt while git ran — drop this result
                    const QStringList n =
                        QString::fromUtf8(c).trimmed().split(QRegularExpression(
                            QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                    const int behind = ok && n.size() > 0 ? n.at(0).toInt() : 0;
                    const int ahead = ok && n.size() > 1 ? n.at(1).toInt() : 0;
                    QString text, tip;
                    if (ahead > 0 && behind > 0) {
                        text = QString::fromUtf8("\xE2\x86\x91%1 \xE2\x86\x93%2")
                                   .arg(ahead).arg(behind);
                        tip = QStringLiteral("%1 commit(s) ahead of and %2 behind %3")
                                  .arg(ahead).arg(behind).arg(baseBranch);
                    } else if (ahead > 0) {
                        text = QString::fromUtf8("\xE2\x86\x91%1").arg(ahead);
                        tip = QStringLiteral("%1 commit(s) ahead of %2")
                                  .arg(ahead).arg(baseBranch);
                    } else if (behind > 0) {
                        text = QString::fromUtf8("\xE2\x86\x93%1").arg(behind);
                        tip = QStringLiteral("%1 commit(s) behind %2")
                                  .arg(behind).arg(baseBranch);
                    } else {
                        text = QStringLiteral("—");
                        tip = QStringLiteral("Up to date with %1").arg(baseBranch);
                    }
                    const QString want = QDir(wtPath).absolutePath();
                    for (int r = 0; r < m_worktreesTable->rowCount(); ++r) {
                        QTableWidgetItem *p = m_worktreesTable->item(r, 1);
                        if (p && QDir(p->text()).absolutePath() == want) {
                            if (QTableWidgetItem *cc = m_worktreesTable->item(r, 3)) {
                                cc->setText(text);
                                cc->setToolTip(tip);
                            }
                            break;
                        }
                    }
                });
        }

        auto *cell = new QWidget;
        auto *h = new QHBoxLayout(cell);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(4);
        auto *openBtn = new QPushButton("Open");
        openBtn->setObjectName("ghostButton");
        openBtn->setCursor(Qt::PointingHandCursor);
        const QString p = wt.path;
        connect(openBtn, &QPushButton::clicked, this,
                [p] { QDesktopServices::openUrl(QUrl::fromLocalFile(p)); });
        h->addWidget(openBtn);
        if (!wt.branch.isEmpty()) {
            const QString branch = wt.branch;
            auto *reviewBtn = new QPushButton("Review changes");
            reviewBtn->setObjectName("ghostButton");
            reviewBtn->setCursor(Qt::PointingHandCursor);
            setOcticon(reviewBtn, "git-compare", 14);
            reviewBtn->setToolTip(
                "Open this worktree in the shared Git branch and diff viewer");
            connect(reviewBtn, &QPushButton::clicked, this,
                    [this, branch, isMain] {
                        if (isMain) {
                            showOverviewCommits();
                            setCommitWorkspacePage(kCommitWorkspaceChangesPage);
                            refreshSourceControl();
                            loadCommits();
                        } else {
                            switchToBranch(branch);
                        }
                    });
            h->addWidget(reviewBtn);
        }
        const AgentSession *agent = nullptr;
        if (!wt.branch.isEmpty()) {
            for (const AgentSession &s : std::as_const(m_agentSessions)) {
                if (s.owner != repoOwner || s.name != repoName
                    || s.branchName != wt.branch)
                    continue;
                if (!agent) {
                    agent = &s;
                    continue;
                }
                const bool sLive = s.status != AgentStatus::Cleared;
                const bool curLive = agent->status != AgentStatus::Cleared;
                if ((sLive && !curLive) || (sLive == curLive && s.id > agent->id))
                    agent = &s;
            }
        }
        if (agent) {
            const int agentId = agent->id;
            auto *agentBtn = new QPushButton(
                QString::fromUtf8("Agent \xC2\xB7 %1")
                    .arg(agentStatusText(agent->status)));
            agentBtn->setObjectName("ghostButton");
            agentBtn->setCursor(Qt::PointingHandCursor);
            agentBtn->setIcon(themedOcticon(
                "rocket",
                agent->merged
                    ? Theme::nextOr(Theme::Role::AccentFg, QColor("#a371f7"))
                    : agentStatusColor(agent->status),
                14));
            agentBtn->setIconSize(QSize(14, 14));
            QString tip = agent->issueNumber > 0
                              ? QString::fromUtf8("Agent #%1 \xC2\xB7 issue #%2 %3")
                                    .arg(agent->id)
                                    .arg(agent->issueNumber)
                                    .arg(agent->issueTitle)
                              : QString::fromUtf8("Agent #%1 \xC2\xB7 %2")
                                    .arg(agent->id)
                                    .arg(agent->issueTitle);
            tip += QString::fromUtf8(" \xC2\xB7 %1").arg(agentStatusText(agent->status));
            if (agent->merged)
                tip += QString::fromUtf8(" \xC2\xB7 merged");
            agentBtn->setToolTip(tip);
            connect(agentBtn, &QPushButton::clicked, this,
                    [this, agentId] { switchToAgentsTab(agentId); });
            h->addWidget(agentBtn);
        }
        if (!isMain && !wt.branch.isEmpty()) {
            const QString branch = wt.branch;
            auto *mergeBtn = new QPushButton("Merge into main");
            mergeBtn->setObjectName("ghostButton");
            mergeBtn->setCursor(Qt::PointingHandCursor);
            setOcticon(mergeBtn, "check-circle", 14);
            connect(mergeBtn, &QPushButton::clicked, this,
                    [this, branch, p] { requestMergeWorktreeIntoMain(branch, p); });
            h->addWidget(mergeBtn);
            auto *prBtn = new QPushButton("Create PR");
            prBtn->setObjectName("ghostButton");
            prBtn->setCursor(Qt::PointingHandCursor);
            setOcticon(prBtn, "git-pull-request", 14);
            connect(prBtn, &QPushButton::clicked, this,
                    [this, branch] { createPullFromBranch(branch); });
            h->addWidget(prBtn);
        }
        if (!isMain) {
            auto *rmBtn = new QPushButton("Delete");
            rmBtn->setObjectName("ghostButton");
            rmBtn->setCursor(Qt::PointingHandCursor);
            setOcticon(rmBtn, "trash", 14);
            rmBtn->setToolTip(
                agent ? "Remove this worktree, delete its branch and its agent session"
                      : "Remove this worktree and delete its branch");
            const QString branch = wt.branch;
            connect(rmBtn, &QPushButton::clicked, this,
                    [this, p, branch] { deleteWorktreeBranchAndAgent(p, branch); });
            h->addWidget(rmBtn);
        }
        h->addStretch();
        m_worktreesTable->setCellWidget(row, 4, cell);
        actionWidth = qMax(actionWidth, cell->sizeHint().width());
    }
    if (actionWidth > 0)
        m_worktreesTable->setColumnWidth(4, actionWidth + 12);
    if (m_worktreesSummary)
        m_worktreesSummary->setText(
            QString::fromUtf8("\xC2\xB7 %1 worktree(s)").arg(wts.size()));
    if (auto *b = dynamic_cast<VerticalIconButton *>(m_worktreesButton))
        b->setBadgeCount(wts.size());

    if (!keepPath.isEmpty()) {
        const QString keep = QDir(keepPath).absolutePath();
        for (int row = 0; row < m_worktreesTable->rowCount(); ++row) {
            QTableWidgetItem *p = m_worktreesTable->item(row, 1);
            if (p && QDir(p->text()).absolutePath() == keep) {
                m_worktreesTable->selectRow(row); // fires currentCellChanged -> diff
                break;
            }
        }
    }
}

void MainWindow::switchToWorktree(const QString &branch)
{
    if (!branch.trimmed().isEmpty()) {
        switchToBranch(branch);
        return;
    }
    showOverviewWorktrees();
    loadWorktreesPanel();
}

void MainWindow::switchToBranch(const QString &branch, int agentSessionId)
{
    QString target = branch.trimmed();
    const int nodeSeparator = target.indexOf(QLatin1Char(':'));
    if (nodeSeparator > 0) {
        const QString local = target.mid(nodeSeparator + 1).trimmed();
        const QString dir = repoGitDir();
        if (!local.isEmpty() && localBranchExists(dir, local)) {
            target = local;
        } else {
            setRepoDetailNotice(
                QStringLiteral("%1 is a cross-node pull-request label; its branch "
                               "is not available on this mirror. Open the pull "
                               "request to review its stored changes.")
                    .arg(target),
                true);
            return;
        }
    }
    m_branchDiffPullNumber = -1; // plain branch mode
    showOverviewCommits();
    const QString base = branchCompareBase();
    const bool branchChanged = !target.isEmpty() && target != m_repoBranch;
    if (branchChanged)
        setRepoBranch(target, /*loadContent=*/false);
    if (target.isEmpty() || target == base) {
        m_branchDiffAgentSessionId = -1;
        setCommitWorkspacePage(kCommitWorkspaceChangesPage);
        showSourceControlLoading(target.isEmpty() ? base : target);
    }
    const int readGen = m_branchScopeDiffGen;
    if (!target.isEmpty() && target != base) {
        setCommitWorkspacePage(kCommitWorkspaceRangePage);
        showBranchDiff(target, agentSessionId);
        if (m_branchDiffView)
            m_branchDiffView->setFocus();
    }
    const bool diffReading = m_branchScopeDiffGen != readGen;
    const quint64 diffReadSeq = m_branchDiffSeq;
    const auto refreshRepo = [this, target, branchChanged] {
        if (branchChanged) {
            if (m_repoBranch == target) {
                loadRepoOverview(QString());
                loadCommits();
            }
        } else {
            refreshCommitsUnlessCurrent();
        }
    };
    scheduleNavRecord();
    if (m_branchesTable && !target.isEmpty()) {
        const bool fresh = !m_branchesPanelDir.isEmpty() &&
                           m_branchesPanelDir == repoGitDir();
        if (fresh && selectBranchRow(target))
            m_branchesPanelPendingSelect.clear();
        else
            m_branchesPanelPendingSelect = target;
    }
    if (diffReading) {
        deferUntilBranchDiffRead([this, refreshRepo, diffReadSeq] {
            refreshRepo();
            loadBranchesPanelAsOf(diffReadSeq);
        });
        return;
    }
    flushBranchDiffAfterRead();
    QTimer::singleShot(0, this, refreshRepo);
    loadBranchesPanelAsOf(diffReadSeq);
}

QString MainWindow::branchCompareBase() const
{
    return m_branchCompareBase.isEmpty() ? repoDefaultBranchFast()
                                         : m_branchCompareBase;
}

void MainWindow::setBranchCompareBase(const QString &base)
{
    const QString target = base.trimmed();
    m_branchCompareBase =
        target == repoDefaultBranchFast() ? QString() : target;
    const QString branch = m_repoBranch.isEmpty() ? repoHeadBranch() : m_repoBranch;
    if (branch.isEmpty() || branch == branchCompareBase()) {
        setCommitWorkspacePage(kCommitWorkspaceChangesPage);
        return;
    }
    setCommitWorkspacePage(kCommitWorkspaceRangePage);
    m_branchDiffPullNumber = -1; // a base change leaves PR mode
    showBranchDiff(branch, m_branchDiffAgentSessionId);
}

void MainWindow::closeBranchCompareView()
{
    m_branchCompareBase.clear();
    setCommitWorkspacePage(kCommitWorkspaceChangesPage);
    const QString base = repoDefaultBranchFast();
    if (!base.isEmpty() && m_repoBranch != base)
        setRepoBranch(base);
}

bool MainWindow::selectBranchRow(const QString &branch)
{
    if (!m_branchesTable || branch.isEmpty())
        return false;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        QTableWidgetItem *it =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (it && it->text() == branch) {
            m_branchesTable->selectRow(row); // fires currentCellChanged -> diff
            return true;
        }
    }
    return false;
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testBranchAgentButtonText() const
{
    return m_branchOpenAgentButton && !m_branchOpenAgentButton->isHidden()
               ? m_branchOpenAgentButton->text()
               : QString();
}

bool MainWindow::testBranchAgentButtonUsesStatusIcon() const
{
    if (!m_branchOpenAgentButton || m_branchOpenAgentButton->isHidden())
        return false;
    const int sessionId = branchDiffAgentSessionId();
    const AgentSession *session = nullptr;
    for (const AgentSession &candidate : m_agentSessions) {
        if (candidate.id == sessionId) {
            session = &candidate;
            break;
        }
    }
    if (!session)
        return false;
    const QSize size(14, 14);
    const QImage actual = m_branchOpenAgentButton->icon()
                              .pixmap(size)
                              .toImage()
                              .convertToFormat(QImage::Format_RGBA8888);
    const QImage expected = agentStatusOcticon(*session, size.width())
                                .pixmap(size)
                                .toImage()
                                .convertToFormat(QImage::Format_RGBA8888);
    return !actual.isNull() && actual == expected;
}

bool MainWindow::testClickBranchAgentButton()
{
    if (!m_branchOpenAgentButton || m_branchOpenAgentButton->isHidden())
        return false;
    m_branchOpenAgentButton->click();
    return true;
}

void MainWindow::testReloadBranchesPanel()
{
    ensureRepoOverviewBodyBuilt(2);
    loadBranchesPanel();
    QDeadlineTimer deadline(10000);
    while ((m_branchesPanelLoading || m_branchChangeStatsLoading) &&
           !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

QString MainWindow::testBranchVisualBadges(const QString &branch) const
{
    if (!m_branchesTable)
        return QString();
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        const QTableWidgetItem *item =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (!item || item->text() != branch)
            continue;
        const auto cell = [this, row](int column) {
            return m_branchesTable->item(row, column);
        };
        const auto value = [](const QTableWidgetItem *cell, int role) {
            const QVariant data = cell ? cell->data(role) : QVariant();
            return data.isValid() && data.toInt() >= 0 ? data.toString()
                                                       : QStringLiteral("-");
        };
        const QTableWidgetItem *changes = cell(kBranchesChangesColumn);
        const QTableWidgetItem *worktree = cell(kBranchesWorktreeColumn);
        const QTableWidgetItem *updated = cell(kBranchesUpdatedColumn);
        return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
            .arg(value(changes, kBranchFilesRole), value(changes, kBranchAddedRole),
                 value(changes, kBranchRemovedRole),
                 worktree && !worktree->text().isEmpty() ? QStringLiteral("1")
                                                         : QStringLiteral("0"),
                 item->data(kBranchConflictRole).toBool() ? QStringLiteral("1")
                                                          : QStringLiteral("0"),
                 updated && !updated->text().isEmpty() ? updated->text()
                                                       : QStringLiteral("-"),
                 value(cell(kBranchesBehindColumn), kTableSortRole),
                 value(cell(kBranchesAheadColumn), kTableSortRole));
    }
    return QString();
}

QString MainWindow::testBranchHealthIcon(const QString &branch) const
{
    if (!m_branchesTable)
        return QString();
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        const QTableWidgetItem *item =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (!item || item->text() != branch)
            continue;
        const QVariant behind = item->data(kBranchBehindRole);
        return branchHealthIconName(
            item->data(kBranchConflictRole).toBool(),
            behind.isValid() ? behind.toInt() : -1);
    }
    return QString();
}

bool MainWindow::testBranchesShowSortableColumns() const
{
    if (!m_branchesTable ||
        m_branchesTable->columnCount() != kBranchesColumnCount ||
        !m_branchesTable->isSortingEnabled() ||
        m_branchesTable->horizontalHeader()->isHidden())
        return false;
    for (int column = 0; column < kBranchesColumnCount; ++column) {
        if (m_branchesTable->isColumnHidden(column))
            return false;
    }
    const QHeaderView *header = m_branchesTable->horizontalHeader();
    return header->visualIndex(kBranchesLinksColumn) <
           header->visualIndex(kBranchesNameColumn);
}

bool MainWindow::testBranchesKeepFlexibleNameColumn() const
{
    return m_branchesTable &&
           m_branchesTable->horizontalHeader()->sectionResizeMode(
               kBranchesNameColumn) ==
               QHeaderView::Stretch;
}

bool MainWindow::testBranchDelegatePaintsSingleTextLayer(
    const QString &branch) const
{
    if (!m_branchesTable)
        return false;
    auto *delegate = dynamic_cast<BranchOverviewDelegate *>(
        m_branchesTable->itemDelegateForColumn(kBranchesNameColumn));
    if (!delegate)
        return false;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        const QModelIndex index =
            m_branchesTable->model()->index(row, kBranchesNameColumn);
        if (index.data(Qt::DisplayRole).toString() != branch)
            continue;
        QStyleOptionViewItem option;
        option.initFrom(m_branchesTable->viewport());
        option.rect = m_branchesTable->visualRect(index);
        return delegate->testStyleLayerHasNoContent(option, index);
    }
    return false;
}

bool MainWindow::testBranchSelectedTextColorIsReadable(const QString &branch) const
{
    if (!m_branchesTable)
        return false;
    auto *delegate = dynamic_cast<BranchOverviewDelegate *>(
        m_branchesTable->itemDelegateForColumn(kBranchesNameColumn));
    if (!delegate)
        return false;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        const QModelIndex index =
            m_branchesTable->model()->index(row, kBranchesNameColumn);
        if (index.data(Qt::DisplayRole).toString() != branch)
            continue;
        QStyleOptionViewItem option;
        option.initFrom(m_branchesTable->viewport());
        option.rect = m_branchesTable->visualRect(index);
        return delegate->testSelectionKeepsNormalTextColor(option, index);
    }
    return false;
}

QString MainWindow::testSwitchToBranchImmediateSelection(const QString &branch)
{
    switchToBranch(branch);
    if (!m_branchesTable)
        return QString();
    const QTableWidgetItem *it = m_branchesTable->item(
        m_branchesTable->currentRow(), kBranchesNameColumn);
    return it ? it->text() : QString();
}

QString MainWindow::testBranchesCurrentRowName() const
{
    if (!m_branchesTable)
        return QString();
    const QTableWidgetItem *it = m_branchesTable->item(
        m_branchesTable->currentRow(), kBranchesNameColumn);
    return it ? it->text() : QString();
}

bool MainWindow::testClickBranchRowInOverview(const QString &branch)
{
    if (!m_branchesTable)
        return false;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        QTableWidgetItem *item =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (!item || item->text() != branch)
            continue;
        emit m_branchesTable->cellClicked(row, kBranchesNameColumn);
        return true;
    }
    return false;
}

bool MainWindow::testBranchesPanelOwnsDiffView() const
{
    return m_branchesTable && m_branchDiffView &&
           m_branchesTable->parentWidget() &&
           m_branchesTable->parentWidget()->isAncestorOf(m_branchDiffView);
}

bool MainWindow::testGitWorkspaceIsExclusive() const
{
    if (!m_railGitButton || !m_railCodeButton || !m_repoDetailChrome ||
        !m_repoFilesModeBar || !m_repoOverviewChrome || !m_footerDock ||
        !m_commitsStack)
        return false;
    if (!m_railGitButton->isChecked() || m_railCodeButton->isChecked() ||
        !m_repoDetailChrome->isHidden() || !m_repoFilesModeBar->isHidden() ||
        !m_repoOverviewChrome->isHidden() || m_footerDock->isHidden())
        return false;
    for (QTextEdit *view : m_diffViews) {
        if (view && view->isVisibleTo(this) &&
            !m_commitsStack->isAncestorOf(view))
            return false;
    }
    return true;
}

bool MainWindow::testGitPromptFloatsBottomLeft() const
{
    return m_footerDock && m_globalOverlayHost && m_promptOverlayHost &&
           m_promptWrapper && m_footerDock->parentWidget() == m_globalOverlayHost &&
           m_footerDock->isVisible() && m_promptOverlayHost->isVisible() &&
           m_promptOverlayHost->width() <= 560 &&
           m_promptOverlayHost->geometry().left() == 8 &&
           m_promptOverlayHost->geometry().bottom() ==
               m_footerDock->rect().bottom() &&
           m_footerDock->y() + m_footerDock->height() <=
               m_globalOverlayHost->height();
}

int MainWindow::testCommitWorkspacePage() const
{
    return m_commitsStack ? m_commitsStack->currentIndex() : -1;
}

int MainWindow::testGitFilesSlotPage() const
{
    return m_gitFilesSlot ? m_gitFilesSlot->currentIndex() : -1;
}

int MainWindow::testGitHistorySlotPage() const
{
    return m_gitHistorySlot ? m_gitHistorySlot->currentIndex() : -1;
}

QString MainWindow::testGitMergedCoverText() const
{
    return m_gitMergedLabel ? m_gitMergedLabel->text() : QString();
}

QString MainWindow::testGitMergedStatsText() const
{
    return m_gitMergedStats ? m_gitMergedStats->toPlainText() : QString();
}

bool MainWindow::testClickGitMergedCoverButton(bool branches)
{
    if (!gitMergedCoverShown())
        return false;
    const QString text = branches ? QStringLiteral("Back to branches")
                                  : QStringLiteral("Back to agents");
    for (QPushButton *button : m_gitMergedPage->findChildren<QPushButton *>()) {
        if (button->text() == text) {
            button->click();
            return true;
        }
    }
    return false;
}

bool MainWindow::testClickGitLeaderboardButton()
{
    if (!m_commitsLeaderboardButton)
        return false;
    m_commitsLeaderboardButton->click();
    return true;
}

void MainWindow::testCloseBranchRange()
{
    if (m_branchCloseButton)
        m_branchCloseButton->click();
}

void MainWindow::testClickRailGitButton()
{
    if (m_railGitButton)
        m_railGitButton->click();
}

int MainWindow::testGitChangeBadgeCount() const
{
    return m_railGitButton ? m_railGitButton->badgeCount() : 0;
}

int MainWindow::testGitPendingSyncCount() const
{
    return m_railGitButton ? m_railGitButton->pendingSyncCount() : 0;
}

QString MainWindow::testCompareIndicatorText() const
{
    if (!m_commitsCompareBaseButton || m_commitsCompareBaseButton->isHidden())
        return QString();
    if (auto *elider =
            dynamic_cast<ElidingPushButton *>(m_commitsCompareBaseButton))
        return elider->fullText();
    return m_commitsCompareBaseButton->text();
}

QString MainWindow::testComparedBranchText() const
{
    if (!m_commitsBranchButton)
        return QString();
    if (auto *elider = dynamic_cast<ElidingPushButton *>(m_commitsBranchButton))
        return elider->fullText();
    return m_commitsBranchButton->text();
}

void MainWindow::testSetCompareBase(const QString &base)
{
    setBranchCompareBase(base);
}

bool MainWindow::testClickBranchReviewMerge(bool deleteAll)
{
    QPushButton *button = deleteAll ? m_branchMergeDeleteButton : m_branchMergeButton;
    if (!button)
        return false;
    QDeadlineTimer deadline(10000);
    while (!button->isEnabled() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!button->isEnabled())
        return false;
    button->click();
    return true;
}

bool MainWindow::testBranchPullEnabled() const
{
    return m_branchPullButton && m_branchPullButton->isEnabled();
}

bool MainWindow::testClickBranchPull()
{
    testSpinWhile([this] { return m_branchMergeFlowBusy; });
    if (!testBranchPullEnabled())
        return false;
    m_branchPullButton->click();
    testSpinWhile([this] { return m_branchMergeFlowBusy; });
    return true;
}

bool MainWindow::testBranchClearLockVisible() const
{
    return m_branchClearLockButton && !m_branchClearLockButton->isHidden();
}

QString MainWindow::testBranchClearLockTooltip() const
{
    return m_branchClearLockButton ? m_branchClearLockButton->toolTip() : QString();
}

bool MainWindow::testClickBranchClearLock()
{
    if (!testBranchClearLockVisible())
        return false;
    m_branchClearLockButton->click();
    return true;
}
#endif

bool MainWindow::selectWorktreeRow(const QString &branch)
{
    if (!m_worktreesTable || branch.isEmpty())
        return false;
    for (int row = 0; row < m_worktreesTable->rowCount(); ++row) {
        QTableWidgetItem *b = m_worktreesTable->item(row, 0);
        if (b && b->data(Qt::UserRole).toString() == branch) {
            m_worktreesTable->selectRow(row); // fires currentCellChanged -> selection
            return true;
        }
    }
    return false;
}

void MainWindow::updateWorktreeSelection(const QString &branch,
                                         const QString &worktreePath)
{
    const QString base = repoDefaultBranch(repoBranches());
    m_worktreeSelectedBranch = branch;
    m_worktreeSelectedPath = worktreePath;
    if (m_worktreeBranchLabel) {
        if (branch.isEmpty()) {
            m_worktreeBranchLabel->setText(QString());
        } else {
            const bool onDisk = !worktreePath.isEmpty() && QDir(worktreePath).exists();
            const QString url =
                onDisk ? QUrl::fromLocalFile(worktreePath).toString() : QString();
            const auto link = [&url, onDisk](const QString &html) {
                return onDisk ? QStringLiteral("<a href=\"%1\">%2</a>").arg(url, html)
                              : html;
            };
            QString text = QStringLiteral("On branch %1")
                               .arg(link(QStringLiteral("<b>%1</b>")
                                             .arg(branch.toHtmlEscaped())));
            if (!worktreePath.isEmpty())
                text += QStringLiteral(" · %1").arg(link(worktreePath.toHtmlEscaped()));
            m_worktreeBranchLabel->setText(text);
        }
    }
    QString repoLocal;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
        repoLocal = m_repositories.at(m_repoDetailIndex).localPath;
    const bool feature = !branch.isEmpty() && branch != base;
    const bool mergeAvailable = !m_worktreeMergeInProgress &&
                                !m_branchCleanupInProgress.contains(branch);
    const bool isMain = !worktreePath.isEmpty() && !repoLocal.isEmpty() &&
                        QDir(worktreePath).absolutePath() == QDir(repoLocal).absolutePath();
    if (m_worktreeMergeButton)
        m_worktreeMergeButton->setEnabled(feature && mergeAvailable &&
                                          repoHasWorkingTree());
    if (m_worktreeMergeDeleteAgentButton)
        m_worktreeMergeDeleteAgentButton->setEnabled(
            feature && mergeAvailable && repoHasWorkingTree());
    if (m_worktreeUpdateButton)
        m_worktreeUpdateButton->setEnabled(feature && !worktreePath.isEmpty() &&
                                           QDir(worktreePath).exists());
    if (m_worktreeCommitButton)
        m_worktreeCommitButton->setEnabled(!worktreePath.isEmpty() &&
                                           QDir(worktreePath).exists());
    if (m_worktreeRemoveButton)
        m_worktreeRemoveButton->setEnabled(!isMain && !worktreePath.isEmpty());
    if (m_worktreeResolveButton) {
        const quint64 probe = ++m_worktreeConflictProbeGen;
        if (worktreePath.isEmpty() || !QDir(worktreePath).exists()) {
            m_worktreeResolveButton->setVisible(false);
            return;
        }
        runOffThread<bool>(
            [worktreePath] { return !unmergedFilesIn(worktreePath).isEmpty(); },
            [this, probe, worktreePath](bool conflicted) {
                if (probe != m_worktreeConflictProbeGen ||
                    m_worktreeSelectedPath != worktreePath ||
                    !m_worktreeResolveButton)
                    return;
                m_worktreeResolveButton->setVisible(conflicted);
            });
    }
}

void MainWindow::requestMergeWorktreeIntoMain(const QString &branchArg,
                                              const QString &worktreePathArg,
                                              bool cleanUp,
                                              bool findWorktree)
{
    const QString branch = branchArg;
    const QString worktreePath = worktreePathArg;
    const QString dir = repoGitDir();
    if (branch.trimmed().isEmpty() || dir.isEmpty() ||
        m_worktreeMergeInProgress ||
        m_branchCleanupInProgress.contains(branch))
        return;
    if (m_branchMergeFlowBusy) {
        const PingThreadScope pingThread(this, mergePingThread(dir, branch));
        flashMessage(QStringLiteral("A branch update is running; merge again when it finishes."));
        return;
    }

    // Let the pressed button and progress copy reach the screen before the
    // merge starts its ordered Git checks. Those checks keep painting while
    // they wait, but entering them directly from clicked() made the button feel
    // stuck and admitted no frame that acknowledged the action first.
    m_worktreeMergeInProgress = true;
    if (m_worktreeMergeButton)
        m_worktreeMergeButton->setEnabled(false);
    if (m_worktreeMergeDeleteAgentButton)
        m_worktreeMergeDeleteAgentButton->setEnabled(false);
    if (m_branchMergeButton)
        m_branchMergeButton->setEnabled(false);
    if (m_branchMergeDeleteButton)
        m_branchMergeDeleteButton->setEnabled(false);
    if (m_branchDeleteCleanupButton)
        m_branchDeleteCleanupButton->setEnabled(false);
    m_pingThreadIds.remove(mergePingThread(dir, branch));
    {
        const PingThreadScope pingThread(this, mergePingThread(dir, branch));
        setRepoDetailNotice(
            cleanUp
                ? QString::fromUtf8("Merging %1, then cleaning up in the background\xE2\x80\xA6")
                      .arg(branch)
                : QString::fromUtf8("Merging %1\xE2\x80\xA6").arg(branch));
    }

    QTimer::singleShot(0, this,
                       [this, branch, worktreePath, dir, cleanUp,
                        findWorktree] {
        // A queued navigation can replace the open repository in the one turn
        // deliberately yielded above. Never apply the captured branch to that
        // different checkout.
        if (repoGitDir() != dir) {
            m_worktreeMergeInProgress = false;
            return;
        }
        const QString resolvedWorktree =
            findWorktree
                ? worktreePathForBranch(dir, branch, WorktreeLookup::Any)
                : worktreePath;
        if (repoGitDir() != dir) {
            m_worktreeMergeInProgress = false;
            updateAgentFilesTabState(m_selectedAgentSessionId);
            return;
        }
        mergeWorktreeIntoMain(
            branch, resolvedWorktree, cleanUp, [this, branch](bool merged) {
                m_worktreeMergeInProgress = false;

                // The batched refresh inside mergeWorktreeIntoMain ran while the busy
                // guard was still set, so a failed merge needs one cheap action-state
                // pass to make its buttons available again. A successful cleanup keeps
                // them disabled through m_branchCleanupInProgress until its callback.
                if (!merged && m_branchDiffBranch == branch)
                    updateBranchDetailActions(branch);
                if (m_worktreeSelectedBranch == branch)
                    updateWorktreeSelection(branch, m_worktreeSelectedPath);
                updateAgentFilesTabState(m_selectedAgentSessionId);
            });
    });
}

namespace {

struct CheckoutDirt {
    bool read = false;
    QStringList changed;
    QStringList staged;
};

CheckoutDirt readCheckoutDirt(const QString &dir)
{
    CheckoutDirt dirt;
    QByteArray out;
    if (!runGitCapture(dir, {"status", "--porcelain", "-z", "--untracked-files=all"},
                       &out, nullptr))
        return dirt;
    dirt.read = true;
    const QList<QByteArray> entries = out.split('\0');
    for (int i = 0; i < entries.size(); ++i) {
        const QByteArray &entry = entries.at(i);
        if (entry.size() < 4)
            continue;
        if (RepoStatsStore::onlyGeneratedStatsDirty(entry))
            continue;
        const QString path = QString::fromUtf8(entry.mid(3));
        const char index = entry.at(0);
        dirt.changed << path;
        if (index != ' ' && index != '?')
            dirt.staged << path;
        if ((index == 'R' || index == 'C') && i + 1 < entries.size()) {
            dirt.changed << QString::fromUtf8(entries.at(++i));
        }
    }
    return dirt;
}

QString describeDirtyPaths(const QStringList &paths)
{
    constexpr int kShown = 5;
    QString text = paths.mid(0, kShown).join(QStringLiteral(", "));
    if (paths.size() > kShown)
        text += QStringLiteral(" (+%1 more)").arg(paths.size() - kShown);
    return text;
}

QString dirtyWorktreeRefusal(const QString &branch, const QStringList &paths,
                             bool cleanUp, bool agentWorking)
{
    const QString files =
        paths.isEmpty() ? QString()
                        : QStringLiteral(" (%1)").arg(describeDirtyPaths(paths));
    if (agentWorking)
        return QStringLiteral("Worktree %1 has uncommitted changes%2 and its agent "
                              "is still working — merge it once the agent finishes.")
            .arg(branch, files);
    if (cleanUp)
        return QStringLiteral("Worktree %1 has uncommitted changes%2 — commit or "
                              "stash them first (they'd be lost when it's deleted).")
            .arg(branch, files);
    return QStringLiteral("Worktree %1 has uncommitted changes%2 — commit or stash "
                          "them first (the merge would leave them out).")
        .arg(branch, files);
}

} // namespace

void MainWindow::mergeWorktreeIntoMain(const QString &branchArg,
                                       const QString &worktreePathArg,
                                       bool cleanUp,
                                       std::function<void(bool)> callerDone,
                                       bool unattended,
                                       bool setAside)
{
    const QString branch = branchArg;
    const QString worktreePath = worktreePathArg;
    const QString dir = repoGitDir();
    const QString base = repoDefaultBranch(repoBranches());
    if (branch.isEmpty() || branch == base || dir.isEmpty()) {
        if (callerDone)
            callerDone(false);
        return;
    }
    if (m_worktreeMergeRunning) {
        if (unattended && !m_worktreeMergeAwaiting.isEmpty()) {
            const QString message =
                QStringLiteral("Couldn't merge %1: the merge before it is waiting "
                               "for an answer on the desktop (%2).")
                    .arg(branch, m_worktreeMergeAwaiting);
            logSystem(QStringLiteral("Git: ") + message);
            {
                const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                flashMessage(message, true, QString(), forkmesh::kLocalOperationNoticeKind);
            }
            if (callerDone)
                callerDone(false);
            return;
        }
        QueuedWorktreeMerge queued;
        queued.dir = dir;
        queued.branch = branch;
        queued.worktreePath = worktreePath;
        queued.cleanUp = cleanUp;
        queued.unattended = unattended;
        queued.setAside = setAside;
        queued.done = std::move(callerDone);
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            queued.owner = m_repositories.at(m_repoDetailIndex).owner;
            queued.name = m_repositories.at(m_repoDetailIndex).name;
        }
        m_queuedWorktreeMerges.append(std::move(queued));
        logSystem(QStringLiteral("Git: queued the merge of %1 behind the one "
                                 "already running.")
                      .arg(branch));
        return;
    }
    m_worktreeMergeRunning = true;
    m_worktreeMergeConflicted = false;
    m_worktreeMergeCanSetAside = false;
    m_worktreeMergeSetAsideNote.clear();
    std::function<void(bool)> done = [this, callerDone](bool ok) {
        m_worktreeMergeRunning = false;
        m_worktreeMergeAwaiting.clear();
        if (callerDone)
            callerDone(ok);
        m_worktreeMergeConflicted = false;
        m_worktreeMergeCanSetAside = false;
        m_worktreeMergeSetAsideNote.clear();
        QTimer::singleShot(0, this, [this] { runNextQueuedWorktreeMerge(); });
    };
    const auto refuse = [&done] { done(false); };
    QString detailOwner;
    QString detailName;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        detailOwner = m_repositories.at(m_repoDetailIndex).owner;
        detailName = m_repositories.at(m_repoDetailIndex).name;
    }
    QList<int> liveSessionIds;
    bool anyExternal = false;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.branchName != branch || session.merged)
            continue;
        if (session.owner != detailOwner || session.name != detailName)
            continue;
        if (session.status != AgentStatus::Running &&
            session.status != AgentStatus::Waiting &&
            session.status != AgentStatus::Queued)
            continue;
        if (!agentSessionWorkInFlight(session.id)) {
            logSystem(QStringLiteral("Git: Agent #%1's status still reads \"%2\" but "
                                     "nothing is executing it — merging %3 without "
                                     "stopping it.")
                          .arg(session.id)
                          .arg(session.status, branch));
            continue;
        }
        liveSessionIds << session.id;
        anyExternal = anyExternal || isExternalSession(session.id);
    }
    if (!liveSessionIds.isEmpty()) {
        const QStringList names = [&liveSessionIds] {
            QStringList out;
            for (const int id : std::as_const(liveSessionIds))
                out << QStringLiteral("#%1").arg(id);
            return out;
        }();
        const QString who = liveSessionIds.size() == 1
                                ? QStringLiteral("Agent %1 is").arg(names.first())
                                : QStringLiteral("Agents %1 are")
                                      .arg(names.join(QStringLiteral(", ")));
        if (anyExternal || unattended) {
            {
                const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                setRepoDetailNotice(
                    QStringLiteral("%1 still working on %2. %3, then merge so %4 "
                                   "committed work is included.")
                        .arg(who, branch,
                             liveSessionIds.size() == 1 ? QStringLiteral("Stop it")
                                                        : QStringLiteral("Stop them"),
                             liveSessionIds.size() == 1 ? QStringLiteral("its")
                                                        : QStringLiteral("their")),
                    true);
            }
            refuse();
            return;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Agent is still working"));
        box.setText(QStringLiteral("%1 still working on %2.").arg(who, branch));
        box.setInformativeText(
            liveSessionIds.size() == 1
                ? QStringLiteral("Stop it and merge its committed work now? Any "
                                 "uncommitted work or later commits will not be "
                                 "included.")
                : QStringLiteral("Stop them and merge their committed work now? Any "
                                 "uncommitted work or later commits will not be "
                                 "included."));
        QPushButton *stopAndMerge = box.addButton(
            liveSessionIds.size() == 1 ? QStringLiteral("Stop agent && merge")
                                       : QStringLiteral("Stop agents && merge"),
            QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        awaitDesktopDuringMerge(box.windowTitle());
        box.exec();
        m_worktreeMergeAwaiting.clear();
        if (box.clickedButton() != stopAndMerge) {
            refuse();
            return;
        }
        for (const int sessionId : std::as_const(liveSessionIds)) {
            if (!stopAgentSessionById(sessionId)) {
                {
                    const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                    setRepoDetailNotice(
                        QStringLiteral("Couldn't stop Agent #%1; its branch was left "
                                       "unchanged.")
                            .arg(sessionId),
                        true);
                }
                refuse();
                return;
            }
        }
        m_worktreeMergeRunning = false;
        mergeWorktreeIntoMain(branch, worktreePath, cleanUp, callerDone, unattended);
        return;
    }

    // A session configured to create a PR must get its durable record before
    // this path mutates, merges, or deletes the branch. Previously the normal
    // "Merge & clean up" action bypassed the finish-time PR hook entirely,
    // which is how completed agent work could land with no PR record.
    QList<int> requestedPullSessions;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (!session.merged && session.createPr && session.prNumber <= 0 &&
            session.owner == detailOwner && session.name == detailName &&
            session.branchName == branch && !isExternalSession(session.id)) {
            if (m_agentPullSkipOnce.remove(session.id))
                continue;
            requestedPullSessions << session.id;
        }
    }
    if (!requestedPullSessions.isEmpty()) {
        auto pending = std::make_shared<int>(int(requestedPullSessions.size()));
        const auto resume = [this, pending, requestedPullSessions, branch,
                             worktreePath, cleanUp, callerDone, dir, done,
                             unattended] {
            if (--*pending > 0)
                return;
            QList<int> nothingToLand;
            for (const int sessionId : requestedPullSessions) {
                const AgentSession *after = findAgentSession(sessionId);
                if (after && after->prNumber <= 0 &&
                    m_agentPullNotNeeded.contains(sessionId)) {
                    m_agentPullFailures.remove(sessionId);
                    nothingToLand << sessionId;
                    continue;
                }
                if (!after || after->prNumber <= 0) {
                    const QString reason = m_agentPullFailures.take(sessionId);
                    {
                        const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                        setRepoDetailNotice(
                            QStringLiteral("Couldn't create the requested PR for "
                                           "Agent #%1 (%2); %3 was not merged or "
                                           "cleaned up.")
                                .arg(sessionId)
                                .arg(reason.isEmpty()
                                         ? QStringLiteral(
                                               "see the agent log for details")
                                         : reason,
                                     branch),
                            true);
                    }
                    done(false);
                    return;
                }
            }
            if (repoGitDir() != dir) {
                done(false);
                return;
            }
            for (const int sessionId : std::as_const(nothingToLand)) {
                logSystem(QStringLiteral("Git: Agent #%1's branch %2 has no changes "
                                         "that are not already in its base, so it "
                                         "is merged and cleaned up without the "
                                         "requested pull request.")
                              .arg(sessionId)
                              .arg(branch));
                m_agentPullSkipOnce.insert(sessionId);
            }
            m_worktreeMergeRunning = false;
            mergeWorktreeIntoMain(branch, worktreePath, cleanUp, callerDone, unattended);
            for (const int sessionId : std::as_const(nothingToLand))
                m_agentPullSkipOnce.remove(sessionId);
        };
        for (const int sessionId : std::as_const(requestedPullSessions))
            maybeCreatePullForStreamSession(sessionId, resume);
        return;
    }
    if (!repoHasWorkingTree()) {
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            setRepoDetailNotice("Read-only mirror — nothing to merge into here.", true);
        }
        refuse();
        return;
    }
    if (repoGitDir() != dir) {
        refuse();
        return;
    }

    WorktreeMergeFlow flow;
    flow.dir = dir;
    flow.branch = branch;
    flow.base = base;
    flow.worktreePath = worktreePath;
    flow.cleanUp = cleanUp;
    flow.unattended = unattended;
    flow.setAside = setAside;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        flow.owner = repo.owner;
        flow.name = repo.name;
        flow.localPath = repo.localPath;
        flow.mirrorPath = repo.mirrorPath;
    }
    flow.identity = &m_profileIdentity;
    flow.userName = m_userName;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || session.branchName != branch
            || session.owner != flow.owner || session.name != flow.name)
            continue;
        if (agentSessionWorkInFlight(session.id))
            flow.agentWorking = true;
        else if (flow.idleAgentId <= 0)
            flow.idleAgentId = session.id;
    }
    runOffThread<WorktreeMergeStep>(
        [flow] { return prepareWorktreeMerge(flow); },
        [this, flow, done](WorktreeMergeStep step) {
            applyWorktreeMergeStep(flow, std::move(step), done);
        });
}

void MainWindow::runNextQueuedWorktreeMerge()
{
    if (m_worktreeMergeRunning || m_queuedWorktreeMerges.isEmpty())
        return;
    QueuedWorktreeMerge next = m_queuedWorktreeMerges.takeFirst();
    const auto dropped = [this, &next](const QString &why) {
        const QString message =
            QStringLiteral("Couldn't merge %1: %2").arg(next.branch, why);
        logSystem(QStringLiteral("Git: ") + message);
        {
            const PingThreadScope pingThread(this, mergePingThread(next.dir, next.branch));
            flashMessage(message, true, QString(), forkmesh::kLocalOperationNoticeKind);
        }
        if (next.done)
            next.done(false);
        QTimer::singleShot(0, this, [this] { runNextQueuedWorktreeMerge(); });
    };
    if (repoGitDir() != next.dir) {
        const int index = repoIndexFor(next.owner, next.name);
        if (index < 0 || !bindRepoDetailToRepo(index) || repoGitDir() != next.dir) {
            dropped(QStringLiteral("its repository is no longer available."));
            return;
        }
    }
    mergeWorktreeIntoMain(next.branch, next.worktreePath, next.cleanUp,
                          std::move(next.done), next.unattended, next.setAside);
}

void MainWindow::awaitDesktopDuringMerge(const QString &what)
{
    m_worktreeMergeAwaiting = what;
    QList<QueuedWorktreeMerge> waiting;
    for (auto it = m_queuedWorktreeMerges.begin(); it != m_queuedWorktreeMerges.end();) {
        if (it->unattended) {
            waiting.append(std::move(*it));
            it = m_queuedWorktreeMerges.erase(it);
        } else {
            ++it;
        }
    }
    for (QueuedWorktreeMerge &next : waiting) {
        const QString message =
            QStringLiteral("Couldn't merge %1: the merge before it is waiting for "
                           "an answer on the desktop (%2).")
                .arg(next.branch, what);
        logSystem(QStringLiteral("Git: ") + message);
        {
            const PingThreadScope pingThread(this, mergePingThread(next.dir, next.branch));
            flashMessage(message, true, QString(), forkmesh::kLocalOperationNoticeKind);
        }
        if (next.done)
            next.done(false);
    }
}

MainWindow::WorktreeMergeStep
MainWindow::prepareWorktreeMerge(const WorktreeMergeFlow &flow)
{
    WorktreeMergeStep step;
    const QString &dir = flow.dir;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    const QString &worktreePath = flow.worktreePath;
    if (const QStringList staleLocks = removeStaleGitLocks(dir);
        !staleLocks.isEmpty())
        step.logs << QStringLiteral("Git: removed orphaned lock(s) %1 before merging %2.")
                         .arg(staleLocks.join(QStringLiteral(", ")), branch);
    if (const QStringList patterns =
            forkmesh::gitrefusal::mergeSourceRefPatterns(branch);
        !patterns.isEmpty()) {
        QByteArray refsOut;
        if (runGitCapture(dir,
                          QStringList{QStringLiteral("for-each-ref"),
                                      QStringLiteral("--format=%(refname) %(objectname)")}
                              + patterns,
                          &refsOut, nullptr)) {
            using forkmesh::gitrefusal::MergeSourceKind;
            const forkmesh::gitrefusal::MergeSource source =
                forkmesh::gitrefusal::mergeSourceFromRefs(
                    branch, QString::fromUtf8(refsOut));
            if (source.kind == MergeSourceKind::Missing
                && !ensureBranchForMerge(
                    PullStore(flow.localPath.isEmpty() ? dir : flow.localPath,
                              flow.mirrorPath, flow.identity, flow.userName),
                    dir, branch, base, &step.logs, &step.message)) {
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                step.reloadPanels = true;
                return step;
            }
            if (source.kind == MergeSourceKind::Ambiguous) {
                step.message =
                    QStringLiteral("Can't merge %1: it exists only on remotes, and "
                                   "they point at different commits (%2). Check "
                                   "out the one you want, then merge again.")
                        .arg(branch, source.candidates.join(QStringLiteral(", ")));
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                return step;
            }
            if (source.kind == MergeSourceKind::RemoteTracking
                && runGitCapture(dir,
                                 {QStringLiteral("merge-base"),
                                  QStringLiteral("--is-ancestor"), source.ref,
                                  base},
                                 nullptr, nullptr)) {
                step.message =
                    QStringLiteral("%1 is already merged into %2; only its copy "
                                   "on %3 is left.")
                        .arg(branch, base,
                             source.ref.mid(13).chopped(branch.size() + 1));
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                step.error = false;
                step.reloadPanels = true;
                return step;
            }
            if (source.kind == MergeSourceKind::RemoteTracking) {
                QString branchError;
                if (!runGitCapture(dir,
                                   {QStringLiteral("branch"), QStringLiteral("--track"),
                                    branch, source.ref},
                                   nullptr, &branchError)) {
                    step.message =
                        QStringLiteral("Couldn't create a local %1 from %2 to merge "
                                       "it: %3")
                            .arg(branch, source.ref.mid(13),
                                 branchError.trimmed().left(200));
                    return step;
                }
                step.logs << QStringLiteral("Git: created local branch %1 from %2 "
                                            "to merge it.")
                                 .arg(branch, source.ref.mid(13));
            }
        }
    }
    QString head = headBranchFromFile(dir);
    if (head.isEmpty()) {
        head = checkedOutBranch(dir);
        if (head == QLatin1String("HEAD"))
            head.clear();
    }
    QByteArray st;
    if (runGitCapture(dir, {"status", "--porcelain"}, &st, nullptr)
        && !QString::fromUtf8(st).trimmed().isEmpty()
        && !RepoStatsStore::onlyGeneratedStatsDirty(st)) {
        const CheckoutDirt dirt = readCheckoutDirt(dir);
        QByteArray touchedOut;
        const bool touchedRead = runGitCapture(
            dir, {"diff", "--name-only", "-z", base + QStringLiteral("...") + branch},
            &touchedOut, nullptr);
        QStringList overlap;
        for (const QByteArray &path : touchedOut.split('\0')) {
            const QString name = QString::fromUtf8(path);
            if (!name.isEmpty() && dirt.changed.contains(name))
                overlap << name;
        }
        QString why;
        QStringList listed = dirt.changed;
        const bool overlapOnly = dirt.read && touchedRead && head == base
                                 && dirt.staged.isEmpty() && !overlap.isEmpty();
        if (overlapOnly && flow.setAside) {
            const forkmesh::mergesetaside::Git git = forkmesh::mergesetaside::processGit(dir);
            const forkmesh::mergesetaside::Aside aside = forkmesh::mergesetaside::setAside(
                git, base, branch, overlap, QDateTime::currentDateTime());
            if (!aside.ok) {
                step.message =
                    QStringLiteral("Couldn't set aside the uncommitted changes to %1 in %2, so "
                                   "%3 was not merged: %4%5")
                        .arg(describeDirtyPaths(overlap), QDir::toNativeSeparators(dir), branch,
                             aside.error,
                             aside.backupBranch.isEmpty()
                                 ? QString()
                                 : QStringLiteral(". A copy is on branch %1.").arg(aside.backupBranch));
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                return step;
            }
            WorktreeMergeFlow inner = flow;
            inner.setAside = false;
            WorktreeMergeStep next = prepareWorktreeMerge(inner);
            const forkmesh::mergesetaside::PutBack back = forkmesh::mergesetaside::putBack(git, aside);
            next.setAsideNote = forkmesh::mergesetaside::putBackNote(aside, back);
            QStringList logs = step.logs;
            logs << QStringLiteral("Git: saved the uncommitted work in %1 on branch %2 and set "
                                   "aside %3 to merge %4.")
                        .arg(QDir::toNativeSeparators(dir), aside.backupBranch,
                             describeDirtyPaths(overlap), branch);
            logs << next.logs;
            logs << QStringLiteral("Git: ") + next.setAsideNote;
            next.logs = logs;
            return next;
        }
        if (!dirt.read || !touchedRead) {
            why = QStringLiteral("and Git couldn't tell which files this merge touches");
        } else if (head != base) {
            why = QStringLiteral("and is on %1, not %2")
                      .arg(head.isEmpty() ? QStringLiteral("a detached HEAD") : head,
                           base);
        } else if (!dirt.staged.isEmpty()) {
            why = QStringLiteral("staged in its index");
            listed = dirt.staged;
        } else if (!overlap.isEmpty()) {
            why = QStringLiteral("in files this merge touches");
            listed = overlap;
        }
        if (!why.isEmpty()) {
            step.message =
                QStringLiteral("Can't merge %1 yet: the checkout has uncommitted "
                               "changes %2 — commit/stash them in %3 or use Create "
                               "PR: %4")
                    .arg(branch, why, QDir::toNativeSeparators(dir),
                         describeDirtyPaths(listed));
            step.noticeKind = forkmesh::kLocalOperationNoticeKind;
            step.error = false;
            step.canSetAside =
                overlapOnly
                && forkmesh::mergesetaside::canSetAside(
                    forkmesh::mergesetaside::processGit(dir), overlap);
            return step;
        }
        step.logs << QStringLiteral("Git: merging %1 into %2 around %3 uncommitted "
                                    "change(s) in %4 that it doesn't touch: %5")
                         .arg(branch, base)
                         .arg(dirt.changed.size())
                         .arg(QDir::toNativeSeparators(dir),
                              describeDirtyPaths(dirt.changed));
    }
    if (RepoStatsStore::onlyGeneratedStatsDirty(st))
        step.logs << QStringLiteral(
            "Git: preserving ForkMesh's generated repository statistics while merging.");
    if (head != base) {
        QString cerr;
        if (!runGitCapture(dir, {"checkout", base}, nullptr, &cerr)) {
            step.message =
                QStringLiteral("Couldn't switch the checkout to %1 (it's on %2): %3")
                    .arg(base, head.isEmpty() ? QStringLiteral("a detached HEAD") : head,
                         cerr.left(200));
            return step;
        }
        step.logs << QStringLiteral("Git: switched the checkout to %1 to merge %2 into it.")
                         .arg(base, branch);
    }

    if (!worktreePath.isEmpty() && QDir(worktreePath).exists()
        && QDir(worktreePath).absolutePath() != QDir(dir).absolutePath()) {
        QByteArray wst;
        QString wstError;
        if (!runGitCapture(worktreePath, {"status", "--porcelain"}, &wst,
                           &wstError)) {
            step.message =
                QStringLiteral("Couldn't check worktree %1 for uncommitted "
                               "changes, so it was left as it is: %2")
                    .arg(branch, wstError.trimmed().isEmpty()
                                     ? QStringLiteral("git status failed")
                                     : wstError.trimmed().left(200));
            return step;
        }
        if (!QString::fromUtf8(wst).trimmed().isEmpty()) {
            const CheckoutDirt left = readCheckoutDirt(worktreePath);
            if (!flow.cleanUp || flow.agentWorking || flow.idleAgentId <= 0) {
                step.message = dirtyWorktreeRefusal(branch, left.changed, flow.cleanUp,
                                                    flow.agentWorking);
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                return step;
            }
            const QString message =
                QStringLiteral("Agent #%1: commit the changes it left uncommitted")
                    .arg(flow.idleAgentId);
            const GitCaptureResult add =
                runGitCaptureBackgrounded(worktreePath, {"add", "-A"});
            const GitCaptureResult commit =
                add.ok ? runGitCaptureBackgrounded(worktreePath,
                                                   {"commit", "-m", message})
                       : add;
            if (!commit.ok) {
                step.message =
                    QStringLiteral("Couldn't commit the changes Agent #%1 left in "
                                   "worktree %2 (%3), so it was not merged: %4")
                        .arg(QString::number(flow.idleAgentId), branch,
                             describeDirtyPaths(left.changed),
                             gitFailureDetail(commit, add.ok
                                                          ? QStringLiteral("git commit")
                                                          : QStringLiteral("git add -A")));
                step.reloadPanels = true;
                return step;
            }
            step.logs << QStringLiteral("Git: committed the %1 change(s) Agent #%2 left "
                                        "uncommitted in %3 before merging it: %4")
                             .arg(QString::number(left.changed.size()),
                                  QString::number(flow.idleAgentId), branch,
                                  describeDirtyPaths(left.changed));
        }
        const GitCaptureResult update = runGitCaptureBackgrounded(
            worktreePath,
            {"merge", base, "-m", QStringLiteral("Merge %1 into %2").arg(base, branch)});
        const bool updated = update.ok;
        const bool updateConflicts = !unmergedFilesIn(worktreePath).isEmpty();
        if (!updated || updateConflicts) {
            runGitCapture(worktreePath, {"merge", "--abort"}, nullptr, nullptr);
            if (updateConflicts) {
                step.kind = WorktreeMergeStep::NeedsEditor;
                return step;
            }
            const QString detail =
                gitFailureDetail(update, QStringLiteral("git merge"));
            if (forkmesh::gitrefusal::overLocalChanges(update.error + QLatin1Char('\n')
                                           + update.failureOutput)) {
                step.message = dirtyWorktreeRefusal(
                    branch, readCheckoutDirt(worktreePath).changed, flow.cleanUp,
                    flow.agentWorking);
                step.noticeKind = forkmesh::kLocalOperationNoticeKind;
                step.reloadPanels = true;
                return step;
            }
            step.message =
                QStringLiteral("Couldn't update %1 from %2 in its worktree: %3")
                    .arg(branch, base, detail);
            step.reloadPanels = true;
            return step;
        }
    }
    return mergeBranchIntoBase(flow, std::move(step));
}

MainWindow::WorktreeMergeStep
MainWindow::mergeBranchIntoBase(const WorktreeMergeFlow &flow,
                                WorktreeMergeStep step)
{
    const QString &dir = flow.dir;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    step.kind = WorktreeMergeStep::Attempted;
    const GitCaptureResult merge = runGitCaptureBackgrounded(
        dir, {"merge", "--no-ff", branch, "-m",
              QStringLiteral("Merge %1 into %2").arg(branch, base)});
    step.merged = merge.ok;
    if (!merge.ok)
        step.mergeError = gitFailureDetail(merge, QStringLiteral("git merge"));
    step.hasConflicts = !unmergedFilesIn(dir).isEmpty();
    step.branchInBase =
        runGitCapture(dir, {"merge-base", "--is-ancestor", branch, "HEAD"}, nullptr,
                      nullptr);
    if (step.merged && !step.hasConflicts && step.branchInBase) {
        QByteArray head;
        if (runGitCapture(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                          &head, nullptr))
            step.mergeCommit = QString::fromUtf8(head).trimmed();
        if (!flow.localPath.isEmpty()) {
            PullStore pulls(flow.localPath, flow.mirrorPath, flow.identity,
                            flow.userName);
            QString reconcileError;
            pulls.reconcileMergedPulls(&reconcileError);
            if (!reconcileError.isEmpty())
                step.logs << QStringLiteral("PR: merged %1, but could not record "
                                            "its PR status yet: %2")
                                 .arg(branch, reconcileError);
        }
    } else {
        runGitCapture(dir, {"merge", "--abort"}, nullptr, nullptr);
    }
    return step;
}

void MainWindow::applyWorktreeMergeStep(const WorktreeMergeFlow &flow,
                                        WorktreeMergeStep step,
                                        std::function<void(bool)> done)
{
    for (const QString &line : std::as_const(step.logs))
        logSystem(line);
    step.logs.clear();
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    const QString &dir = flow.dir;
    const bool here = repoGitDir() == dir;
    if (!step.setAsideNote.isEmpty()) {
        m_worktreeMergeSetAsideNote = step.setAsideNote;
        const PingThreadScope pingThread(this, mergePingThread(dir, branch));
        noticeForRepo(here, step.setAsideNote, false, forkmesh::kLocalOperationNoticeKind);
    }
    if (step.kind == WorktreeMergeStep::Refused) {
        m_worktreeMergeCanSetAside = step.canSetAside;
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            noticeForRepo(here, step.message, step.error, step.noticeKind);
        }
        if (here && step.reloadPanels) {
            loadWorktreesPanel();
            loadBranchesAndTags();
        }
        if (done)
            done(false);
        return;
    }
    if (step.kind == WorktreeMergeStep::EditorUnresolved) {
        if (here) {
            loadWorktreesPanel();
            loadBranchesAndTags();
        }
        if (done)
            done(false);
        return;
    }
    if (step.kind == WorktreeMergeStep::NeedsEditor) {
        if (flow.unattended) {
            m_worktreeMergeConflicted = true;
            noticeForRepo(here,
                          QStringLiteral("Couldn't merge %1 into %2: updating it from "
                                         "%2 conflicts. Kept its worktree and branch; "
                                         "use \"Fix with agent\" or resolve the "
                                         "conflicts on the desktop.")
                              .arg(branch, base),
                          true, forkmesh::kLocalOperationNoticeKind);
            if (here) {
                loadWorktreesPanel();
                loadBranchesAndTags();
            }
            if (done)
                done(false);
            return;
        }
        if (!here) {
            {
                const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                flashMessage(QStringLiteral("Left %1 unmerged: its repository was closed "
                                            "before its conflicts could be resolved.")
                                 .arg(branch),
                             true);
            }
            if (done)
                done(false);
            return;
        }
        // A content conflict is an input to the existing merge editor,
        // not the end of the merge click. Resolve it in the branch's
        // own worktree, then carry on only after Git proves that the
        // branch contains the base. Cancelling leaves both refs
        // untouched and the editor's neutral cancellation notice in
        // place.
        awaitDesktopDuringMerge(
            QString::fromUtf8("Merge editor \xE2\x80\x94 %1").arg(branch));
        openBranchMergeEditor(branch, [this, flow, done] {
            m_worktreeMergeAwaiting.clear();
            runOffThread<WorktreeMergeStep>(
                [flow] {
                    WorktreeMergeStep next;
                    const bool resolved = runGitCapture(
                        flow.dir, {"merge-base", "--is-ancestor", flow.base, flow.branch},
                        nullptr, nullptr);
                    if (!resolved) {
                        next.kind = WorktreeMergeStep::EditorUnresolved;
                        return next;
                    }
                    return mergeBranchIntoBase(flow, std::move(next));
                },
                [this, flow, done](WorktreeMergeStep next) {
                    applyWorktreeMergeStep(flow, std::move(next), done);
                });
        });
        return;
    }

    UiRefreshBatch uiBatch(this);
    const bool merged = step.merged;
    const bool hasConflicts = step.hasConflicts;
    const bool branchInBase = step.branchInBase;
    const bool landed = merged && !hasConflicts && branchInBase;
    if (landed) {
        danceGenie();
        {
            const QList<AgentSession> sessions = m_agentSessions;
            rememberMergedBranch(step.mergeCommit, branch, sessions, flow.owner,
                                 flow.name, topBarUserName());
        }
    }
    if (landed && here && gitWorkspaceShowsBranchRange(branch))
        showGitMergedCover(branch, base);
    if (!here) {
        const QString message =
            landed ? QStringLiteral("Merged %1 into %2, but its repository was closed "
                                    "before cleanup; open it to remove the worktree, "
                                    "branch and Agent record.")
                         .arg(branch, base)
                   : QStringLiteral("Couldn't merge %1 into %2 — kept its worktree "
                                    "and branch.")
                         .arg(branch, base);
        logSystem(QStringLiteral("Git: ") + message);
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            flashMessage(message, true, QString(),
                         forkmesh::kLocalOperationNoticeKind);
        }
        if (done)
            done(false);
        return;
    }
    if (landed) {
        if (!branch.isEmpty()
            && m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
            QList<int> attachedIssues;
            for (const AgentSession &s : std::as_const(m_agentSessions)) {
                if (s.owner == repo.owner && s.name == repo.name
                    && s.branchName == branch && s.issueNumber > 0
                    && !attachedIssues.contains(s.issueNumber))
                    attachedIssues.append(s.issueNumber);
            }
            closeIssuesForMerge(
                attachedIssues,
                QStringLiteral("Closed by merged branch \"%1\".").arg(branch),
                QStringLiteral("merged branch \"%1\"").arg(branch));
        }
        const QString worktreePath = flow.worktreePath;
        if (flow.cleanUp) {
            markAgentSessionsMerged(0, branch, branchInBase);
            if (m_repoDetailIndex >= 0 &&
                m_repoDetailIndex < m_repositories.size()) {
                const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
                retireMergedCloudAgentSessions(repo.owner, repo.name, branch);
            }
            // The merge is durable now; worktree traversal and ref deletion are
            // cleanup, not part of the click's critical path. Retire the compare
            // header immediately, paint a lightweight success page, and let the
            // existing background cleanup pipeline do the destructive tail.
            // Only this branch's Agent sessions go; every other agent is left
            // as it is.
            QList<int> sessionPulls;
            for (const AgentSession &s : std::as_const(m_agentSessions)) {
                if (s.owner == flow.owner && s.name == flow.name &&
                    s.branchName == branch && s.prNumber > 0 &&
                    !sessionPulls.contains(s.prNumber))
                    sessionPulls.append(s.prNumber);
            }
            flashMergedBranchRow(branch);
            startMergedBranchCelebration(branch, base, dir);
            m_branchCleanupInProgress.insert(branch);
            {
                const PingThreadScope pingThread(this, mergePingThread(dir, branch));
                setRepoDetailNotice(
                    QStringLiteral("Merged %1 into %2. Cleaning up its worktree, "
                                   "branch and Agent record in the background.")
                        .arg(branch, base),
                    false);
            }
            deleteWorktreeBranchAndAgentInBackground(
                worktreePath, branch,
                [this, flow, sessionPulls](bool success) {
                    deletePullsOfMergedBranch(
                        flow, sessionPulls,
                        [this, flow, success](const QList<int> &deletedPulls,
                                              const QString &pullError) {
                            finishMergedBranchCleanup(flow, success, deletedPulls,
                                                      pullError);
                        });
                },
                /*announce=*/false);
        } else if (!worktreePath.isEmpty() &&
            QDir(worktreePath).absolutePath() != QDir(dir).absolutePath()) {
            removeWorktree(worktreePath, branch, /*confirm=*/false,
                           /*alsoDeleteBranch=*/true,
                           [this, flow, step, done] {
                               const bool removed =
                                   !QDir(flow.worktreePath).exists();
                               const bool branchDeleted =
                                   removed && !localBranchExists(flow.dir, flow.branch);
                               finishWorktreeMerge(flow, step, removed,
                                                   branchDeleted, done);
                           });
            return;
        }
    } else if (hasConflicts) {
        m_worktreeMergeConflicted = true;
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            setRepoDetailNotice(
                QStringLiteral("Couldn't merge %1 into %2 cleanly — kept its worktree and "
                               "branch. Update it from %2 to resolve the conflicts (the "
                               "\"Update from %2\" button), or use \"Fix with agent\".")
                    .arg(branch, base),
                true, forkmesh::kLocalOperationNoticeKind);
        }
    } else {
        // Git refused the merge outright rather than reporting a conflicted
        // file: a stale .git/index.lock left by a Git run that was killed
        // mid-merge, a full disk, a ref another worktree holds. None of those
        // are conflicts, and neither "Update from main" nor "Fix with agent"
        // can clear any of them, so naming a conflict here sends people to two
        // buttons that cannot help and throws away the one line that says what
        // actually happened. Repeat what Git said instead (adhoc #203).
        QString detail = step.mergeError.trimmed();
        if (detail.isEmpty())
            detail = QStringLiteral("Git refused the merge.");
        detail = detail.left(240);
        // "fatal: Unable to write index." is how a leftover lock file reads,
        // and it never mentions the lock, so say where to look.
        if (detail.contains(QStringLiteral("index.lock")) ||
            detail.contains(QStringLiteral("Unable to write index")))
            detail += QStringLiteral(" A Git process that was killed mid-merge "
                                     "can leave .git/index.lock behind; remove "
                                     "it once no Git process is running.");
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            setRepoDetailNotice(
                QStringLiteral("Couldn't merge %1 into %2 — kept its worktree and "
                               "branch. %3")
                    .arg(branch, base, detail),
                true);
        }
    }
    finishWorktreeMerge(flow, step, false, false, std::move(done));
}

void MainWindow::deletePullsOfMergedBranch(
    const WorktreeMergeFlow &flow, const QList<int> &sessionPulls,
    std::function<void(QList<int>, QString)> onDone)
{
    struct PullCleanup {
        QList<int> deleted;
        QString error;
    };
    if (flow.localPath.isEmpty()) {
        onDone({}, QString());
        return;
    }
    PullStore store(flow.localPath, flow.mirrorPath, flow.identity, flow.userName);
    store.setSigningRepository(flow.owner, flow.name);
    const QString branch = flow.branch;
    m_pullDeleteInProgress = true;
    setPullDeleteButtonsEnabled(false);
    runOffThread<PullCleanup>(
        [store, branch, sessionPulls]() mutable {
            PullCleanup result;
            QList<int> numbers = sessionPulls;
            const QRegularExpressionMatch canonical =
                QRegularExpression(QStringLiteral("^pr/(\\d+)$")).match(branch);
            if (canonical.hasMatch())
                numbers << canonical.captured(1).toInt();
            for (const PullRequest &pr : store.loadAll()) {
                QString head = pr.head.trimmed();
                const int separator = head.indexOf(QLatin1Char(':'));
                if (separator > 0)
                    head = head.mid(separator + 1).trimmed();
                if (head == branch)
                    numbers << pr.number;
            }
            QList<int> unique;
            for (const int number : std::as_const(numbers))
                if (number > 0 && !unique.contains(number))
                    unique << number;
            if (unique.isEmpty())
                return result;
            QList<int> missing;
            store.deletePulls(unique, /*rewriteHistory=*/false, &result.deleted,
                              &missing, &result.error);
            return result;
        },
        [this, branch, dir = flow.dir,
         onDone = std::move(onDone)](PullCleanup result) {
            m_pullDeleteInProgress = false;
            setPullDeleteButtonsEnabled(true);
            if (!result.error.isEmpty())
                logSystem(QStringLiteral("PR: could not delete the pull request "
                                         "of merged branch %1: %2")
                              .arg(branch, result.error));
            if (!result.deleted.isEmpty()) {
                QStringList labels;
                for (const int number : std::as_const(result.deleted))
                    labels << QStringLiteral("#%1").arg(number);
                logSystem(QStringLiteral("PR: deleted %1 with merged branch %2.")
                              .arg(labels.join(QStringLiteral(", ")), branch));
                if (repoGitDir() == dir) {
                    if (result.deleted.contains(m_currentPullNumber))
                        m_currentPullNumber = -1;
                    reloadPullsInBackground();
                }
            }
            onDone(result.deleted, result.error);
        });
}

void MainWindow::finishMergedBranchCleanup(const WorktreeMergeFlow &flow,
                                           bool success,
                                           const QList<int> &deletedPulls,
                                           const QString &pullError)
{
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    m_branchCleanupInProgress.remove(branch);
    if (repoGitDir() != flow.dir) {
        updateAgentFilesTabState(m_selectedAgentSessionId);
        return;
    }
    QStringList pullLabels;
    for (const int number : deletedPulls)
        pullLabels << QStringLiteral("#%1").arg(number);
    if (success && !pullError.isEmpty()) {
        {
            const PingThreadScope pingThread(this, mergePingThread(flow.dir, flow.branch));
            setRepoDetailNotice(
                QStringLiteral("Merged %1 into %2 and cleaned up its worktree, branch "
                               "and Agent record, but could not delete its pull "
                               "request: %3")
                    .arg(branch, base, pullError),
                true, forkmesh::kLocalOperationNoticeKind);
        }
    } else if (success) {
        {
            const PingThreadScope pingThread(this, mergePingThread(flow.dir, flow.branch));
            setRepoDetailNotice(
                pullLabels.isEmpty()
                    ? QStringLiteral("Merged %1 into %2 and cleaned up its worktree, "
                                     "branch and Agent record.")
                          .arg(branch, base)
                    : QStringLiteral("Merged %1 into %2 and cleaned up its worktree, "
                                     "branch, pull request %3 and Agent record.")
                          .arg(branch, base, pullLabels.join(QStringLiteral(", "))),
                false);
        }
    } else {
        if (m_branchMergedFlashBranch == branch)
            clearMergedBranchFlash();
        {
            const PingThreadScope pingThread(this, mergePingThread(flow.dir, flow.branch));
            setRepoDetailNotice(
                QStringLiteral("Merged %1 into %2, but cleanup could not finish.")
                    .arg(branch, base),
                true, forkmesh::kLocalOperationNoticeKind);
        }
    }
    if (success && m_repoBranch == branch)
        setRepoBranch(base);
    if (m_branchDiffBranch == branch)
        updateBranchDetailActions(branch);
    if (m_worktreeSelectedBranch == branch) {
        if (success)
            updateWorktreeSelection(QString(), QString());
        else
            updateWorktreeSelection(branch, m_worktreeSelectedPath);
    }
    updateAgentFilesTabState(m_selectedAgentSessionId);
}

void MainWindow::finishWorktreeMerge(const WorktreeMergeFlow &flow,
                                     const WorktreeMergeStep &step, bool removed,
                                     bool branchDeleted,
                                     std::function<void(bool)> done)
{
    UiRefreshBatch uiBatch(this);
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    const QString &dir = flow.dir;
    const bool landed = step.merged && !step.hasConflicts && step.branchInBase;
    if (repoGitDir() != dir) {
        if (landed) {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            flashMessage(QStringLiteral("Merged %1 into %2, but its repository was "
                                        "closed before its Agent sessions were "
                                        "marked merged.")
                             .arg(branch, base),
                         true, QString(), forkmesh::kLocalOperationNoticeKind);
        }
        if (done)
            done(false);
        return;
    }
    if (landed && !flow.cleanUp) {
        {
            const PingThreadScope pingThread(this, mergePingThread(dir, branch));
            setRepoDetailNotice(
                (removed
                     ? QStringLiteral("Merged %1 into %2, removed its worktree and "
                                      "deleted its branch")
                           .arg(branch, base)
                     : branchDeleted
                           ? QStringLiteral("Merged %1 into %2 and deleted its branch")
                                 .arg(branch, base)
                           : QStringLiteral("Merged %1 into %2").arg(branch, base))
                    + QStringLiteral("."),
                false);
        }
        markAgentSessionsMerged(0, branch, step.branchInBase);
    } else if (landed) {
        reloadIssues();
        refreshIssueList();
        updateIssueActionState();
    }
    if (branchDeleted) {
        flashMergedBranchRow(branch);
        if (!m_branchMergedFlashBranch.isEmpty())
            startMergedBranchCelebration(branch, base, dir);
        showBranchDiffBranchGone(branch, base);
    } else if (!flow.cleanUp && landed && flow.worktreePath.isEmpty() &&
               !gitMergedCoverShown()) {
        const QString next = neighbourBranchInList(branch);
        if (!next.isEmpty())
            m_branchDiffBranch = next;
    }
    clearBranchDiffCache();
    refreshSourceControl(true);
    loadWorktreesPanel();
    loadBranchesAndTags();
    if (done)
        done(landed);
}

void MainWindow::removeWorktree(const QString &worktreePath, const QString &branch,
                                bool confirm, bool alsoDeleteBranch,
                                std::function<void()> onDone)
{
    const auto settle = [&onDone] {
        if (onDone)
            onDone();
    };
    if (worktreePath.isEmpty()) {
        settle();
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        settle();
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = repo.localPath;
    if (repoPath.isEmpty()) {
        settle();
        return;
    }
    if (QDir(worktreePath).absolutePath() == QDir(repoPath).absolutePath()) {
        setRepoDetailNotice("That's the main checkout — it can't be removed here.",
                            true);
        settle();
        return;
    }
    const QString base = repoDefaultBranch(repoBranches());
    const bool deleteBranch =
        alsoDeleteBranch && !branch.isEmpty() && branch != base;
    if (confirm &&
        QMessageBox::question(
            this, QStringLiteral("Remove worktree"),
            (deleteBranch
                 ? QStringLiteral("Remove the worktree at\n%1\nand delete its branch "
                                  "%2?\n\nUncommitted changes there will be lost.")
                 : QStringLiteral("Remove the worktree at\n%1\n(branch %2)?\n\n"
                                  "Uncommitted changes there will be lost."))
                .arg(worktreePath, branch.isEmpty() ? QStringLiteral("-") : branch))
            != QMessageBox::Yes) {
        settle();
        return;
    }

    // From here this call owns the directory. Any session still holding it as
    // its live worktree lets go now, so a later cleanupStreamWorktree() doesn't
    // start a second `git worktree remove` for the same path on a background
    // thread — two removers is how one of them ends up reporting a failure for a
    // worktree the other had just removed (adhoc #1680).
    releaseTrackedWorktree(worktreePath);

    runOffThread<WorktreeRemoval>(
        [repoPath, worktreePath, branch, deleteBranch] {
            return removeWorktreeAndBranch(repoPath, worktreePath, branch,
                                           deleteBranch);
        },
        [this, repo, repoPath, worktreePath, branch,
         onDone = std::move(onDone)](WorktreeRemoval removal) {
            UiRefreshBatch uiBatch(this);
            for (const QString &line : std::as_const(removal.logs))
                logSystem(line);
            const bool here = repoDetailShows(repoPath);
            if (removal.refused) {
                noticeForRepo(here,
                              QStringLiteral("Could not remove the worktree at %1.")
                                  .arg(worktreePath),
                              true, forkmesh::kLocalOperationNoticeKind);
            } else if (removal.branchAttempted) {
                if (removal.branchDeleted) {
                    logSystem(
                        QStringLiteral("Git: removed worktree %1 and deleted branch %2.")
                            .arg(worktreePath, branch));
                    noticeForRepo(here, QStringLiteral("Removed worktree and deleted "
                                                       "branch %1.")
                                            .arg(branch));
                } else {
                    noticeForRepo(
                        here,
                        QStringLiteral(
                            "Removed the worktree, but could not delete branch %1: %2")
                            .arg(branch, removal.branchError.isEmpty()
                                             ? QStringLiteral("unknown error")
                                             : removal.branchError),
                        true, forkmesh::kLocalOperationNoticeKind);
                }
            }
            if (removal.branchDeleted)
                propagateBranchDeletion(repo.owner, repo.name);
            if (here) {
                loadWorktreesPanel();
                if (!removal.refused && m_branchesTable)
                    loadBranchesPanel();
                if (removal.branchDeleted) {
                    m_branchesCache.clear();
                    loadBranchesAndTags();
                }
            }
            if (onDone)
                onDone();
        });
}

MainWindow::WorktreeRemoval MainWindow::removeWorktreeAndBranch(
    const QString &repoPath, const QString &worktreePath, const QString &branch,
    bool deleteBranch)
{
    WorktreeRemoval removal;
    // git refuses a path it no longer knows about ("is not a working tree"),
    // and a directory that is already gone is the outcome this was asked for
    // however it got there — an earlier pass, a wiped worktree root, a stale
    // panel row clicked twice. Prune the admin entry left behind (it would keep
    // the branch hostage) and carry on so the branch is still deleted, instead
    // of reporting a failure for work that is done.
    if (!runGitCapture(repoPath,
                       {QStringLiteral("worktree"), QStringLiteral("remove"),
                        QStringLiteral("--force"), worktreePath},
                       nullptr, nullptr)) {
        if (forkmesh::agentwt::refusalWorthReporting(/*gitSucceeded=*/false,
                                                     QDir(worktreePath).exists())) {
            removal.refused = true;
            return removal;
        }
        runGitCapture(repoPath, {QStringLiteral("worktree"), QStringLiteral("prune")},
                      nullptr, nullptr);
        removal.logs << QStringLiteral("Git: worktree %1 was already removed; pruned "
                                       "its entry and finished the cleanup.")
                            .arg(worktreePath);
    }
    const QString scratch = forkmesh::agentscratch::folderFor(worktreePath);
    if (!scratch.isEmpty() && QFileInfo::exists(scratch)) {
        if (forkmesh::agentscratch::remove(scratch)) {
            removal.logs << QStringLiteral("Agents: removed scratch folder %1.").arg(scratch);
            runGitCapture(repoPath, {QStringLiteral("worktree"), QStringLiteral("prune")},
                          nullptr, nullptr);
        } else {
            removal.logs << QStringLiteral("Agents: could not remove scratch folder %1.")
                                .arg(scratch);
        }
    }
    forkmesh::agentscratch::sweepOrphans(QFileInfo(worktreePath).absolutePath());
    if (deleteBranch && repoHasLocalBranch(repoPath, branch)) {
        removal.branchAttempted = true;
        removal.branchDeleted =
            deleteBranchAndCopies(repoPath, branch, &removal.branchError);
    }
    return removal;
}

QString MainWindow::worktreePathForBranch(QString repoPath, QString branch,
                                          WorktreeLookup lookup) const
{
    if (repoPath.isEmpty() || branch.trimmed().isEmpty())
        return QString();
    // Keep both values alive across runGitCapture(): its nested event loop can
    // reload agent sessions and repositories while Git runs. In particular,
    // sessionWorkdir() commonly calls us with branchName owned by a session that
    // reloadIssues() may replace, so const-reference parameters become dangling
    // before the porcelain output is parsed below.
    QByteArray out;
    // Served from the stamp-keyed memo while no worktree was added, removed
    // or switched; one git process otherwise.
    const bool listed = worktreeListForCaller(repoPath, &out);
#ifdef FORKMESH_WINDOW_TESTS
    if (m_testWorktreeListCompletedHook) {
        auto hook = std::move(m_testWorktreeListCompletedHook);
        m_testWorktreeListCompletedHook = {};
        hook();
    }
#endif
    if (!listed)
        return QString();
    return worktreePathInListing(out, repoPath, branch, lookup);
}

QString MainWindow::readWorktreePathForBranch(const QString &repoPath,
                                              const QString &branch,
                                              WorktreeLookup lookup)
{
    if (repoPath.isEmpty() || branch.trimmed().isEmpty())
        return QString();
    QByteArray out;
    if (!cachedWorktreeListPorcelain(repoPath, &out))
        return QString();
    return worktreePathInListing(out, repoPath, branch, lookup);
}

QString MainWindow::worktreePathInListing(const QByteArray &listing,
                                          const QString &repoPath,
                                          const QString &branch,
                                          WorktreeLookup lookup)
{
    const QString want = QStringLiteral("refs/heads/%1").arg(branch);
    const QString mainPath = QDir(repoPath).absolutePath();
    // git prints "prunable <reason>" *after* the branch line of a record whose
    // directory has gone (a merge cleanup, or someone wiping the worktree root),
    // so a match is only decided once the whole record has been read.
    QString currentPath;
    bool wanted = false;
    bool gone = false;
    const auto matched = [&]() -> QString {
        if (!wanted || currentPath.isEmpty()
            || QDir(currentPath).absolutePath() == mainPath)
            return QString();
        if (lookup == WorktreeLookup::Live && (gone || !QDir(currentPath).exists()))
            return QString();
        return currentPath;
    };
    for (const QString &raw : QString::fromUtf8(listing).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("worktree "))) {
            const QString hit = matched();
            if (!hit.isEmpty())
                return hit;
            currentPath = line.mid(9).trimmed();
            wanted = false;
            gone = false;
        } else if (line.startsWith(QLatin1String("branch "))) {
            wanted = line.mid(7).trimmed() == want;
        } else if (line == QLatin1String("prunable")
                   || line.startsWith(QLatin1String("prunable "))) {
            gone = true;
        }
    }
    return matched();
}

bool MainWindow::localBranchExists(const QString &repoPath,
                                   const QString &branch) const
{
    // Answered from the ref files where the layout allows it; repoHasLocalBranch
    // falls back to git only when it cannot recognise the ref backend.
    return repoHasLocalBranch(repoPath, branch);
}

void MainWindow::deleteWorktreeBranchAndAgent(const QString &worktreePath,
                                              const QString &branch, bool confirm,
                                              bool deferRefresh,
                                              std::function<void()> onDone)
{
    const auto settle = [&onDone] {
        if (onDone)
            onDone();
    };
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        settle();
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = repo.localPath;
    if (repoPath.isEmpty()) {
        settle();
        return;
    }
    if (!worktreePath.isEmpty()
        && QDir(worktreePath).absolutePath() == QDir(repoPath).absolutePath()) {
        setRepoDetailNotice("That's the main checkout — it can't be removed here.",
                            true);
        settle();
        return;
    }

    QList<int> agentIds;
    QSet<int> issueNumbers;
    if (!branch.isEmpty()) {
        for (const AgentSession &s : std::as_const(m_agentSessions)) {
            if (s.owner == repo.owner && s.name == repo.name
                && s.branchName == branch && !isExternalSession(s.id)) {
                agentIds.append(s.id);
                if (s.issueNumber > 0)
                    issueNumbers.insert(s.issueNumber);
            }
        }
    }

    const QString base = repoDefaultBranch(repoBranches());
    const bool willDeleteBranch = !branch.isEmpty() && branch != base;

    QStringList parts;
    if (!worktreePath.isEmpty())
        parts << QStringLiteral("the worktree at\n%1").arg(worktreePath);
    if (willDeleteBranch)
        parts << QStringLiteral("branch %1").arg(branch);
    if (!agentIds.isEmpty())
        parts << (agentIds.size() == 1
                      ? QStringLiteral("its agent session")
                      : QStringLiteral("its %1 agent sessions").arg(agentIds.size()));
    if (parts.isEmpty()) {
        settle();
        return;
    }
    const QString what =
        parts.size() == 1
            ? parts.first()
            : QStringLiteral("%1 and %2").arg(
                  QStringList(parts.mid(0, parts.size() - 1)).join(QStringLiteral(", ")),
                  parts.last());
    QString prompt = QStringLiteral("Delete %1?\n\nUncommitted changes there will be "
                                    "lost. This cannot be undone.")
                         .arg(what);
    if (!issueNumbers.isEmpty())
        prompt += issueNumbers.size() == 1
                      ? QStringLiteral("\n\nThe linked issue #%1 will be closed.")
                            .arg(*issueNumbers.cbegin())
                      : QStringLiteral("\n\nThe %1 linked issues will be closed.")
                            .arg(issueNumbers.size());
    if (confirm
        && QMessageBox::question(this,
                                 QStringLiteral("Delete worktree, branch & agent"),
                                 prompt)
               != QMessageBox::Yes) {
        settle();
        return;
    }

    GitKeepAlive keepAlive;

    // The worktree below is removed by this call, so claim it before deleting the
    // sessions: session deletion tears its own worktree down from a background
    // thread, and with both running for one path the loser reported "Could not
    // remove the worktree at <path>" for a directory the winner had just removed
    // (adhoc #1680). Claiming first is what puts them in order — releasing it
    // afterwards would already be too late.
    const QList<int> heldWorktree = releaseTrackedWorktree(worktreePath);

    // Delete the agent session(s) next — that stops any runner still holding the
    // worktree open. If one is mid-stop or we lack permission, bail (it flashed
    // why) before touching the worktree so nothing is half-deleted, handing the
    // claim back since nothing was removed. force: this action deletes the branch
    // a provenance-only record documents, so keeping that record would strand it
    // (and would block the rest of this cleanup outright).
    for (int id : std::as_const(agentIds)) {
        if (!deleteStoredAgentSession(id, /*cleanupWorktree=*/true,
                                      /*allowAssociationOnly=*/true)) {
            retrackWorktree(heldWorktree, worktreePath);
            if (!deferRefresh)
                refreshAgentListAfterRemoval();
            settle();
            return;
        }
    }

    if (!worktreePath.isEmpty()) {
        removeWorktree(worktreePath, branch, /*confirm=*/false,
                       /*alsoDeleteBranch=*/willDeleteBranch, onDone);
    } else if (willDeleteBranch && localBranchExists(repoPath, branch)) {
        runOffThread<QPair<bool, QString>>(
            [repoPath, branch] {
                QString err;
                const bool ok = deleteBranchAndCopies(repoPath, branch, &err);
                return qMakePair(ok, err);
            },
            [this, repo, repoPath, branch, onDone](QPair<bool, QString> result) {
                const auto finished = [&onDone] {
                    if (onDone)
                        onDone();
                };
                const bool here = repoDetailShows(repoPath);
                if (result.first) {
                    logSystem(QStringLiteral("Git: deleted branch %1.").arg(branch));
                    noticeForRepo(here,
                                  QStringLiteral("Deleted branch %1.").arg(branch));
                    propagateBranchDeletion(repo.owner, repo.name);
                } else {
                    noticeForRepo(here,
                                  result.second.isEmpty()
                                      ? QStringLiteral("Could not delete branch %1.")
                                            .arg(branch)
                                      : result.second,
                                  true);
                }
                if (here) {
                    loadWorktreesPanel();
                    if (m_branchesTable)
                        loadBranchesPanel();
                    if (result.first) {
                        m_branchesCache.clear();
                        loadBranchesAndTags();
                    }
                }
                finished();
            });
    } else {
        settle();
    }

    int closedIssues = 0;
    if (!issueNumbers.isEmpty()) {
        const RepositoryRecord &writable = writableRecordFor(repo);
        IssueStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                         m_userName);
        if (store.canWrite()) {
            QHash<int, QString> statusByNumber;
            for (const Issue &issue : store.loadAll())
                statusByNumber.insert(issue.number, issue.status);
            for (const int number : std::as_const(issueNumbers)) {
                if (!statusByNumber.contains(number)
                    || statusByNumber.value(number) == QLatin1String("closed"))
                    continue;
                QString err;
                if (store.setStatus(number, QStringLiteral("closed"), &err)) {
                    logSystem(
                        QStringLiteral("Closed issue #%1 (agent worktree deleted).")
                            .arg(number));
                    ++closedIssues;
                } else {
                    logSystem(QStringLiteral("Issue #%1: could not close on delete: %2")
                                  .arg(number)
                                  .arg(err));
                }
            }
        }
    }

    if (!deferRefresh) {
        if (!agentIds.isEmpty()) {
            // The deleted rows are already gone from the list; a rescan would
            // re-read every other agent's worktree for nothing.
            refreshAgentListAfterRemoval();
            reloadIssues();
            refreshIssueList();
            updateIssueActionState();
        }
        if (closedIssues > 0) {
            updateRepoIssueCount();
            flashMessage(
                closedIssues == 1
                    ? QStringLiteral("Closed the linked issue.")
                    : QStringLiteral("Closed %1 linked issues.").arg(closedIssues));
        }
    }
}

void MainWindow::deleteWorktreeBranchAndAgentInBackground(
    const QString &worktreePath, const QString &branch,
    std::function<void(bool)> onDone, bool announce)
{
    const auto rejected = [&onDone] {
        if (onDone)
            onDone(false);
    };
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        rejected();
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = repo.localPath;
    if (repoPath.isEmpty() || branch.trimmed().isEmpty()) {
        rejected();
        return;
    }
    if (!worktreePath.isEmpty() &&
        QDir(worktreePath).absolutePath() == QDir(repoPath).absolutePath()) {
        setRepoDetailNotice("That's the main checkout — it can't be removed here.",
                            true);
        rejected();
        return;
    }

    QList<int> agentIds;
    QSet<int> issueNumbers;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.owner != repo.owner || session.name != repo.name ||
            session.branchName != branch || isExternalSession(session.id))
            continue;
        agentIds.append(session.id);
        if (session.issueNumber > 0)
            issueNumbers.insert(session.issueNumber);
    }
    // This call removes the worktree itself, so nothing else may (adhoc #1680) —
    // a teardown a finished yolo run left tracked would otherwise still be able
    // to start one.
    const QList<int> heldWorktree = releaseTrackedWorktree(worktreePath);

    QElapsedTimer storeTimer;
    storeTimer.start();
    for (const int id : std::as_const(agentIds)) {
        if (!deleteStoredAgentSession(id, /*cleanupWorktree=*/false, /*force=*/true)) {
            logSystem(QStringLiteral("Agents: delete of session #%1 (%2) stopped "
                                     "before cleanup started.")
                          .arg(id)
                          .arg(branch));
            retrackWorktree(heldWorktree, worktreePath);
            refreshAgentListAfterRemoval();
            rejected();
            return;
        }
    }
    if (!agentIds.isEmpty()) {
        logSystem(
            QStringLiteral("Agents: dropped %1 stored session%2 on %3 in %4 ms; "
                           "worktree and branch cleanup runs in the background.")
                .arg(agentIds.size())
                .arg(agentIds.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(branch)
                .arg(storeTimer.elapsed()));
        if (announce)
            flashMessage(
                agentIds.size() == 1
                    ? QStringLiteral("Agent session deleted. Cleanup is running in the background.")
                    : QStringLiteral("%1 agent sessions deleted. Cleanup is running in the background.")
                          .arg(agentIds.size()));
    }
    // Only the rows deleted above changed: repaint the list without reloading
    // and rescanning every other agent.
    if (!agentIds.isEmpty()) {
        if (m_uiRefreshBatchDepth > 0) {
            refreshAgentListAfterRemoval();
            reloadIssues();
            refreshIssueList();
            updateIssueActionState();
        } else {
            QTimer::singleShot(0, this, [this] {
                refreshAgentListAfterRemoval();
                reloadIssuesInBackground();
                updateIssueActionState();
            });
        }
    }

    const quint64 taskId = beginBackgroundTask(
        QStringLiteral("cleanup"),
        QStringLiteral("Deleting %1 worktree and branch").arg(branch));
    const QString base = repoDefaultBranch(repoBranches());
    const bool deleteBranch = !branch.isEmpty() && branch != base;
    auto completed = std::make_shared<bool>(false);
    auto cleanupTimer = std::make_shared<QElapsedTimer>();
    cleanupTimer->start();
    logSystem(QStringLiteral("Agents: cleaning up worktree %1 and branch %2 in the "
                             "background.")
                  .arg(worktreePath.isEmpty() ? QStringLiteral("(none)") : worktreePath)
                  .arg(branch));

    auto finish = [this, repo, issueNumbers, branch, deleteBranch, taskId,
                   completed, cleanupTimer, onDone = std::move(onDone)](
                      bool success, const QString &error) {
        if (*completed)
            return;
        *completed = true;
        int closedIssues = 0;
        if (success && !issueNumbers.isEmpty()) {
            const RepositoryRecord &writable = writableRecordFor(repo);
            IssueStore store(writable.localPath, writable.mirrorPath,
                             &m_profileIdentity, m_userName);
            if (store.canWrite()) {
                QHash<int, QString> statusByNumber;
                for (const Issue &issue : store.loadAll())
                    statusByNumber.insert(issue.number, issue.status);
                for (const int number : issueNumbers) {
                    if (!statusByNumber.contains(number) ||
                        statusByNumber.value(number) == QLatin1String("closed"))
                        continue;
                    QString issueError;
                    if (store.setStatus(number, QStringLiteral("closed"),
                                        &issueError)) {
                        ++closedIssues;
                        logSystem(
                            QStringLiteral(
                                "Closed issue #%1 (agent worktree deleted).")
                                .arg(number));
                    } else {
                        logSystem(
                            QStringLiteral(
                                "Issue #%1: could not close on delete: %2")
                                .arg(number)
                                .arg(issueError));
                    }
                }
            }
        }
        loadWorktreesPanel();
        if (m_branchesTable)
            loadBranchesPanel();
        if (repoDetailShows(repo.localPath)) {
            m_branchesCache.clear();
            loadBranchesAndTags();
        }
        reloadIssuesInBackground();
        updateIssueActionState();
        updateRepoIssueCount();
        const QString detail =
            success
                ? QStringLiteral("Deleted worktree and branch %1%2 (%3 ms).")
                      .arg(branch,
                           closedIssues > 0
                               ? QStringLiteral("; closed %1 linked issue%2")
                                     .arg(closedIssues)
                                     .arg(closedIssues == 1 ? QString()
                                                           : QStringLiteral("s"))
                               : QString())
                      .arg(cleanupTimer->elapsed())
                : (error.trimmed().isEmpty()
                       ? QStringLiteral("Could not finish deleting %1.").arg(branch)
                       : error.trimmed());
        finishBackgroundTask(taskId, success, detail,
                             forkmesh::kLocalOperationNoticeKind);
        if (success && deleteBranch)
            propagateBranchDeletion(repo.owner, repo.name);
        if (onDone)
            onDone(success);
    };

    auto deleteBranchNext =
        std::make_shared<std::function<void()>>();
    *deleteBranchNext = [this, repoPath, branch, deleteBranch, finish] {
        if (!deleteBranch) {
            finish(true, QString());
            return;
        }
        runOffThread<GitCaptureResult>(
            [repoPath, branch] {
                GitCaptureResult result;
                result.ok = deleteBranchAndCopies(repoPath, branch, &result.error);
                return result;
            },
            [branch, finish](GitCaptureResult result) {
                if (result.ok) {
                    finish(true, QString());
                    return;
                }
                finish(false,
                       result.error.isEmpty()
                           ? QStringLiteral("Could not delete branch %1.")
                                 .arg(branch)
                           : result.error);
            });
    };

    // A worktree whose directory has gone still holds its branch hostage
    // ("cannot delete branch 'x' used by worktree at ...") until the admin entry
    // goes too, so prune before handing over to the branch step.
    auto prunedAndSettled = [this, repoPath, worktreePath, deleteBranchNext] {
        runGitDetached(
            repoPath, {QStringLiteral("worktree"), QStringLiteral("prune")},
            [this, worktreePath, deleteBranchNext](bool, const QByteArray &) {
                logSystem(QStringLiteral("Git: worktree %1 was already removed; "
                                         "pruned its entry and carried on with "
                                         "the branch.")
                              .arg(worktreePath));
                (*deleteBranchNext)();
            });
    };

    if (worktreePath.isEmpty()) {
        QTimer::singleShot(0, this, [deleteBranchNext] { (*deleteBranchNext)(); });
        return;
    }
    if (!QDir(worktreePath).exists()) {
        QTimer::singleShot(0, this, prunedAndSettled);
        return;
    }

    auto *git = new QProcess(this);
    auto handled = std::make_shared<bool>(false);
    connect(git, &QProcess::errorOccurred, this,
            [git, worktreePath, finish,
             handled](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *handled)
                    return;
                *handled = true;
                git->deleteLater();
                finish(false,
                       QStringLiteral("Could not start worktree cleanup for %1.")
                           .arg(worktreePath));
            });
    connect(git, &QProcess::finished, this,
            [git, worktreePath, deleteBranchNext, finish, prunedAndSettled,
             handled](int code, QProcess::ExitStatus status) {
                if (*handled)
                    return;
                *handled = true;
                const QString error =
                    QString::fromUtf8(git->readAllStandardError()).trimmed();
                git->deleteLater();
                if (status == QProcess::NormalExit && code == 0) {
                    (*deleteBranchNext)();
                    return;
                }
                if (!forkmesh::agentwt::refusalWorthReporting(
                        /*gitSucceeded=*/false, QDir(worktreePath).exists())) {
                    prunedAndSettled();
                    return;
                }
                finish(false,
                       error.isEmpty()
                           ? QStringLiteral("Could not remove worktree %1.")
                                 .arg(worktreePath)
                           : error);
            });
    git->start(
        QStringLiteral("git"),
        {QStringLiteral("-C"), repoPath, QStringLiteral("worktree"),
         QStringLiteral("remove"), QStringLiteral("--force"), worktreePath});
}

void MainWindow::deleteMergedAgentBranchesInTurn(const QString &repoPath,
                                                 QStringList branches,
                                                 int sessionCount)
{
    if (!repoDetailShows(repoPath)) {
        flashMessage(QStringLiteral("Stopped deleting merged agent sessions: %1 "
                                    "branch%2 left because the repository was "
                                    "closed. Run it again from that repository.")
                         .arg(branches.size())
                         .arg(branches.size() == 1 ? QString()
                                                   : QStringLiteral("es")),
                     true);
        return;
    }
    if (branches.isEmpty()) {
        refreshAgentListAfterRemoval();
        reloadIssues();
        refreshIssueList();
        updateIssueActionState();
        updateRepoIssueCount();
        flashMessage(QStringLiteral("Deleted %1 merged agent session%2.")
                         .arg(sessionCount)
                         .arg(sessionCount == 1 ? QString() : QStringLiteral("s")));
        return;
    }
    const QString branch = branches.takeFirst();
    // Any: a worktree whose directory is already gone still holds the branch
    // hostage until its admin entry is removed too.
    const QString wt = worktreePathForBranch(repoPath, branch, WorktreeLookup::Any);
    deleteWorktreeBranchAndAgent(wt, branch, /*confirm=*/false,
                                 /*deferRefresh=*/true,
                                 [this, repoPath, branches, sessionCount] {
                                     deleteMergedAgentBranchesInTurn(
                                         repoPath, branches, sessionCount);
                                 });
}

void MainWindow::deleteAllMergedAgentSessions()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = repo.localPath;
    if (repoPath.isEmpty())
        return;

    QStringList branches;
    QSet<QString> seen;
    int sessionCount = 0;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner != repo.owner || s.name != repo.name)
            continue;
        if (!s.merged || s.branchName.isEmpty() || isExternalSession(s.id))
            continue;
        ++sessionCount;
        if (!seen.contains(s.branchName)) {
            seen.insert(s.branchName);
            branches << s.branchName;
        }
    }
    if (branches.isEmpty()) {
        flashMessage(QStringLiteral("No merged agent sessions to delete."));
        return;
    }

    deleteMergedAgentBranchesInTurn(repoPath, branches, sessionCount);
}

void MainWindow::updateWorktreeFromMain(const QString &worktreePath,
                                        const QString &branch,
                                        const QString &baseArg,
                                        std::function<void()> done)
{
    const auto finished = [&done] {
        if (done)
            done();
    };
    const QString base = baseArg.isEmpty() ? repoDefaultBranch(repoBranches()) : baseArg;
    if (worktreePath.isEmpty() || branch.isEmpty() || base.isEmpty() || branch == base) {
        finished();
        return;
    }
    if (!QDir(worktreePath).exists()) {
        setRepoDetailNotice("That worktree's folder is gone.", true);
        loadWorktreesPanel();
        finished();
        return;
    }
    if (m_worktreeGitBusy.contains(worktreePath)) {
        flashMessage(QStringLiteral("%1 is already being updated.").arg(branch));
        finished();
        return;
    }
    m_worktreeGitBusy.insert(worktreePath);
    const QString dir = repoGitDir();
    runOffThread<WorktreeUpdateRead>(
        [worktreePath, branch, base] {
            WorktreeUpdateRead read;
            QByteArray st;
            if (runGitCapture(worktreePath, {"status", "--porcelain"}, &st, nullptr) &&
                !QString::fromUtf8(st).trimmed().isEmpty()) {
                read.dirty = true;
                return read;
            }
            read.merged = runGitCapture(
                worktreePath,
                {"merge", base, "-m", QStringLiteral("Merge %1 into %2").arg(base, branch)},
                nullptr, nullptr);
            return read;
        },
        [this, dir, worktreePath, branch, base, done](WorktreeUpdateRead read) {
            const bool here = repoGitDir() == dir;
            if (read.dirty) {
                m_worktreeGitBusy.remove(worktreePath);
                noticeForRepo(here,
                              QStringLiteral("Worktree %1 has uncommitted changes — "
                                             "commit or stash them before updating "
                                             "from %2.")
                                  .arg(branch, base),
                              true);
                if (done)
                    done();
                return;
            }
            if (read.merged) {
                m_worktreeGitBusy.remove(worktreePath);
                noticeForRepo(here,
                              QStringLiteral("Updated %1 from %2.").arg(branch, base),
                              false);
                if (here)
                    loadWorktreesPanel();
                if (done)
                    done();
                return;
            }
            editWorktreeConflicts(
                worktreePath, branch, base,
                [this, dir, worktreePath, branch, base, done](bool resolved) {
                    m_worktreeGitBusy.remove(worktreePath);
                    const bool stillHere = repoGitDir() == dir;
                    if (resolved)
                        noticeForRepo(stillHere,
                                      QStringLiteral("Updated %1 from %2 (conflicts "
                                                     "resolved).")
                                          .arg(branch, base),
                                      false);
                    else
                        noticeForRepo(stillHere,
                                      QStringLiteral("Couldn't update %1 from %2 "
                                                     "cleanly; the merge was left "
                                                     "unchanged.")
                                          .arg(branch, base),
                                      true);
                    if (stillHere)
                        loadWorktreesPanel();
                    if (done)
                        done();
                });
        });
}

void MainWindow::editWorktreeConflicts(const QString &worktreePath,
                                       const QString &branch, const QString &base,
                                       std::function<void(bool)> done)
{
    runOffThread<QStringList>(
        [worktreePath] {
            const QStringList conflicted = unmergedFilesIn(worktreePath);
            if (conflicted.isEmpty())
                runGitCapture(worktreePath, {"merge", "--abort"}, nullptr, nullptr);
            return conflicted;
        },
        [this, worktreePath, branch, base, done](QStringList conflicted) {
            if (conflicted.isEmpty()) {
                if (done)
                    done(false);
                return;
            }
            const QString intro =
                QString::fromUtf8(
                    "Resolve each conflict, then commit the merge into <b>%1</b>. "
                    "<b>Ours</b> is %1; <b>theirs</b> is %2. You can also edit the "
                    "text directly.")
                    .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
            const bool committed = runMergeConflictEditor(
                QString::fromUtf8("Resolve conflicts \xE2\x80\x94 %1").arg(branch),
                intro, worktreePath, conflicted, QStringLiteral("Commit merge"),
                [worktreePath](QString *e) {
                    return commitResolvedMerge(worktreePath, e);
                },
                true);
            if (committed) {
                if (done)
                    done(true);
                return;
            }
            runOffThread<bool>(
                [worktreePath] {
                    return runGitCapture(worktreePath, {"merge", "--abort"}, nullptr,
                                         nullptr);
                },
                [done](bool) {
                    if (done)
                        done(false);
                });
        });
}

void MainWindow::resolveWorktreeConflicts()
{
    const QString worktreePath = m_worktreeSelectedPath;
    const QString branch = m_worktreeSelectedBranch;
    if (worktreePath.isEmpty() || !QDir(worktreePath).exists()) {
        setRepoDetailNotice("That worktree's folder is gone.", true);
        loadWorktreesPanel();
        return;
    }
    if (m_worktreeGitBusy.contains(worktreePath)) {
        flashMessage(QStringLiteral("%1 is already being updated.").arg(branch));
        return;
    }
    m_worktreeGitBusy.insert(worktreePath);
    const QString base = repoDefaultBranch(repoBranches());
    const QString dir = repoGitDir();
    editWorktreeConflicts(
        worktreePath, branch, base, [this, dir, worktreePath, branch](bool resolved) {
            m_worktreeGitBusy.remove(worktreePath);
            const bool here = repoGitDir() == dir;
            if (resolved)
                noticeForRepo(here,
                              QStringLiteral("Resolved conflicts in %1.").arg(branch),
                              false);
            else
                noticeForRepo(here,
                              QStringLiteral("Cancelled conflict resolution; %1 was "
                                             "left unchanged.")
                                  .arg(branch));
            if (here)
                loadWorktreesPanel();
        });
}

void MainWindow::commitWorktreeChanges(const QString &worktreePath,
                                       const QString &branch)
{
    if (worktreePath.isEmpty() || branch.isEmpty())
        return;
    if (!QDir(worktreePath).exists()) {
        setRepoDetailNotice("That worktree's folder is gone.", true);
        loadWorktreesPanel();
        return;
    }
    if (m_worktreeGitBusy.contains(worktreePath)) {
        flashMessage(QStringLiteral("%1 is already being updated.").arg(branch));
        return;
    }
    m_worktreeGitBusy.insert(worktreePath);
    const QString dir = repoGitDir();
    runOffThread<bool>(
        [worktreePath] {
            QByteArray st;
            return runGitCapture(worktreePath, {"status", "--porcelain"}, &st,
                                 nullptr) &&
                   QString::fromUtf8(st).trimmed().isEmpty();
        },
        [this, dir, worktreePath, branch](bool clean) {
            if (clean) {
                m_worktreeGitBusy.remove(worktreePath);
                noticeForRepo(repoGitDir() == dir,
                              QStringLiteral("Worktree %1 has no changes to commit.")
                                  .arg(branch),
                              false);
                return;
            }
            if (repoGitDir() != dir) {
                m_worktreeGitBusy.remove(worktreePath);
                return;
            }
            bool ok = false;
            const QString message =
                QInputDialog::getMultiLineText(this, "Commit changes",
                                               "Commit message:", QString(), &ok)
                    .trimmed();
            if (!ok) {
                m_worktreeGitBusy.remove(worktreePath);
                return;
            }
            if (message.isEmpty()) {
                m_worktreeGitBusy.remove(worktreePath);
                setRepoDetailNotice("A commit message is required.", true);
                return;
            }
            runOffThread<QPair<bool, QString>>(
                [worktreePath, message] {
                    QString err;
                    const bool committed =
                        runGitCapture(worktreePath, {"add", "-A"}, nullptr, &err) &&
                        runGitCapture(worktreePath, {"commit", "-m", message}, nullptr,
                                      &err);
                    return qMakePair(committed, err);
                },
                [this, dir, worktreePath, branch](QPair<bool, QString> result) {
                    m_worktreeGitBusy.remove(worktreePath);
                    const bool here = repoGitDir() == dir;
                    if (result.first)
                        noticeForRepo(here,
                                      QStringLiteral("Committed changes in %1.")
                                          .arg(branch),
                                      false);
                    else
                        noticeForRepo(here,
                                      QStringLiteral("Couldn't commit changes in %1: %2")
                                          .arg(branch, result.second.trimmed()),
                                      true);
                    if (here)
                        loadWorktreesPanel();
                });
        });
}

QWidget *MainWindow::buildBranchesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(10);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel("Branches");
    heading->setObjectName("channelTitle");
    m_branchesSummary = new QLabel;
    m_branchesSummary->setObjectName("statusLine");
    auto *newBranchButton = new QPushButton("New branch");
    newBranchButton->setObjectName("primaryButton");
    newBranchButton->setCursor(Qt::PointingHandCursor);
    setOcticon(newBranchButton, "git-branch", 16);
    connect(newBranchButton, &QPushButton::clicked, this, &MainWindow::promptNewBranch);
    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    refreshButton->setToolTip("Fetch and prune remote-tracking branches, then refresh");
    connect(refreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshBranchesFromRemotes);
    addRefreshSpin(refreshButton);
    m_branchPullAllButton = new QPushButton("Fast-forward idle");
    m_branchPullAllButton->setObjectName("ghostButton");
    m_branchPullAllButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchPullAllButton, "download", 16);
    connect(m_branchPullAllButton, &QPushButton::clicked, this,
            &MainWindow::pullBaseIntoAllBranches);
    m_branchDeleteMergedButton = new QPushButton("Delete merged");
    m_branchDeleteMergedButton->setObjectName("ghostButton");
    m_branchDeleteMergedButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchDeleteMergedButton, "trash", 16);
    connect(m_branchDeleteMergedButton, &QPushButton::clicked, this,
            &MainWindow::deleteMergedBranches);
    m_branchDeleteSelectedButton = new QPushButton("Delete selected");
    m_branchDeleteSelectedButton->setObjectName("ghostButton");
    m_branchDeleteSelectedButton->setCursor(Qt::PointingHandCursor);
    m_branchDeleteSelectedButton->setEnabled(false);
    setOcticon(m_branchDeleteSelectedButton, "trash", 16);
    m_branchDeleteSelectedButton->setToolTip(
        "Delete selected branches that are not checked out or attached to a worktree");
    connect(m_branchDeleteSelectedButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedBranches);
    headerRow->addWidget(heading);
    headerRow->addWidget(m_branchesSummary);
    headerRow->addStretch();
    headerRow->addWidget(m_branchPullAllButton);
    headerRow->addWidget(refreshButton);
    headerRow->addWidget(m_branchDeleteMergedButton);
    headerRow->addWidget(m_branchDeleteSelectedButton);
    headerRow->addWidget(newBranchButton);
    layout->addLayout(headerRow);

    m_branchesTable = new QTableWidget(0, kBranchesColumnCount);
    m_branchesTable->setObjectName("issueTable");
    // The per-row action cells used to carry their own
    // "#branchActions { background: transparent; }" sheet; one rule on the
    // table cascades to every cell widget without re-running the stylesheet
    // machinery per row on each rebuild.
    m_branchesTable->setStyleSheet(Theme::remapSheet(
        "#issueTable { border: none; } #branchActions { background: transparent; }"));
    m_branchesTable->setFrameShape(QFrame::NoFrame);
    enableHoverRowHighlight(m_branchesTable);
    installColumnHeaderMenu(m_branchesTable, /*showSortIndicator=*/true);
    m_branchesTable->setHorizontalHeaderLabels(
        {"", "Agent / task", "Branch", "Status", "Behind", "Ahead", "Commit",
         "Message", "Author", "Changes", "Created", "Updated", "Worktree"});
    m_branchesTable->verticalHeader()->setVisible(false);
    m_branchesTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_branchesTable->verticalHeader()->setDefaultSectionSize(30);
    m_branchesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_branchesTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_branchesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_branchesTable->setShowGrid(false);
    m_branchesTable->setWordWrap(false);
    m_branchesTable->setTextElideMode(Qt::ElideRight);
    m_branchesTable->setItemDelegateForColumn(
        kBranchesNameColumn, new BranchOverviewDelegate(m_branchesTable));
    m_branchesTable->setItemDelegateForColumn(
        kBranchesLinksColumn, new BranchLinksDelegate(m_branchesTable));
    m_branchesTable->setItemDelegateForColumn(
        kBranchesChangesColumn, new BranchChangesDelegate(m_branchesTable));
    QHeaderView *bh = m_branchesTable->horizontalHeader();
    bh->setHighlightSections(false);
    bh->setMinimumSectionSize(24);
    for (int column = 0; column < kBranchesColumnCount; ++column)
        bh->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    bh->setSectionResizeMode(kBranchesDeleteColumn, QHeaderView::Fixed);
    bh->resizeSection(kBranchesDeleteColumn, 36);
    bh->setSectionResizeMode(kBranchesNameColumn, QHeaderView::Stretch);
    bh->setSectionResizeMode(kBranchesMessageColumn, QHeaderView::Stretch);
    makeColumnsResizable(m_branchesTable,
                         QSet<int>{kBranchesNameColumn, kBranchesMessageColumn});
    bh->setSortIndicatorClearable(true);
    bh->setSortIndicator(-1, Qt::AscendingOrder);
    m_branchesTable->setSortingEnabled(true);
    connect(bh, &QHeaderView::sortIndicatorChanged, this,
            [this, bh](int section, Qt::SortOrder order) {
                if (section == kBranchesDeleteColumn)
                    bh->setSortIndicator(-1, order);
                else if (section < 0)
                    loadBranchesPanel();
            });
    connect(m_branchesTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                QTableWidgetItem *it =
                    m_branchesTable->item(row, kBranchesNameColumn);
                if (it && !it->text().isEmpty())
                    clearMergedBranchFlash();
            });
    connect(m_branchesTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] {
                if (!m_branchDeleteSelectedButton)
                    return;
                m_branchDeleteSelectedButton->setEnabled(
                    repoHasWorkingTree() && m_branchesTable->selectionModel()
                    && m_branchesTable->selectionModel()->hasSelection());
            });
    connect(m_branchesTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column == kBranchesDeleteColumn)
                    return;
                QTableWidgetItem *it =
                    m_branchesTable->item(row, kBranchesNameColumn);
                if (it && !it->text().isEmpty())
                    switchToBranch(it->text());
            });
    m_branchesTable->setMouseTracking(true);
    auto *prefetchDwell = new QTimer(m_branchesTable);
    prefetchDwell->setSingleShot(true);
    prefetchDwell->setInterval(kBranchDiffPrefetchDwellMs);
    connect(prefetchDwell, &QTimer::timeout, this, [this, prefetchDwell] {
        prefetchBranchDiff(prefetchDwell->property("branch").toString());
    });
    connect(m_branchesTable, &QTableWidget::cellEntered, this,
            [this, prefetchDwell](int row, int) {
                QTableWidgetItem *it =
                    m_branchesTable->item(row, kBranchesNameColumn);
                const QString branch = it ? it->text() : QString();
                if (branch == prefetchDwell->property("branch").toString())
                    return;
                prefetchDwell->setProperty("branch", branch);
                if (branch.isEmpty())
                    prefetchDwell->stop();
                else
                    prefetchDwell->start();
            });

    layout->addWidget(m_branchesTable, 1);
    return page;
}

QWidget *MainWindow::buildBranchRangePane()
{
    m_branchDiffView = new QTextBrowser;
    m_branchDiffView->setObjectName("diffView");
    m_branchDiffView->setOpenExternalLinks(false);
    m_branchDiffView->setOpenLinks(false); // we handle "viewed:" anchors ourselves
    connect(m_branchDiffView, &QTextBrowser::anchorClicked, this,
            &MainWindow::onBranchDiffAnchorClicked);
    registerDiffView(m_branchDiffView);
    m_branchDiffActiveOutline = new QFrame(diffOverlayParent(m_branchDiffView));
    m_branchDiffActiveOutline->setObjectName("diffActiveOutline");
    m_branchDiffActiveOutline->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_branchDiffActiveOutline->setStyleSheet(Theme::remapSheet(
        QStringLiteral("#diffActiveOutline{background:transparent;"
                       "border:2px solid %1;border-radius:6px;}")
            .arg(Theme::nextOrHex(Theme::Role::AccentFg,
                                  QStringLiteral("#2da44e")))));
    m_branchDiffActiveOutline->hide();
    m_branchDiffView->viewport()->installEventFilter(this);
    m_branchDiffSticky = new QFrame(diffOverlayParent(m_branchDiffView));
    m_branchDiffSticky->setObjectName("diffStickyHeader");
    {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        m_branchDiffSticky->setStyleSheet(Theme::remapSheet(
            QStringLiteral("#diffStickyHeader{background:%1;border:none;}"
                           "#diffStickyHeader QLabel{background:transparent;}")
                .arg(dark ? "#161b22" : "#f6f8fa")));
        auto *sl = new QHBoxLayout(m_branchDiffSticky);
        sl->setContentsMargins(12, 9, 12, 9);
        sl->setSpacing(0);
        m_branchStickyPath = new QLabel(m_branchDiffSticky);
        m_branchStickyPath->setTextFormat(Qt::RichText);
        m_branchStickyPath->setTextInteractionFlags(Qt::NoTextInteraction);
        configureDiffStickyPathLabel(m_branchStickyPath);
        sl->addWidget(m_branchStickyPath, 1);
        m_branchStickyControls = new QLabel(m_branchDiffSticky);
        m_branchStickyControls->setTextFormat(Qt::RichText);
        m_branchStickyControls->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_branchStickyControls->setCursor(Qt::PointingHandCursor);
        connect(m_branchStickyControls, &QLabel::linkActivated, this,
                [this](const QString &link) {
                    onBranchDiffAnchorClicked(QUrl(link));
                });
        sl->addWidget(m_branchStickyControls, 0);
        m_branchDiffSticky->hide();
    }
    m_branchAutoViewedDebounce = new QTimer(this);
    m_branchAutoViewedDebounce->setSingleShot(true);
    m_branchAutoViewedDebounce->setInterval(400);
    connect(m_branchAutoViewedDebounce, &QTimer::timeout, this,
            &MainWindow::applyBranchAutoMarkViewedOnScroll);
    connect(m_branchDiffView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] {
                updateBranchDiffSticky();
                updateBranchDiffActiveOutline();
                positionPullInlineComposer();
                if (m_branchAutoViewedDebounce)
                    m_branchAutoViewedDebounce->start();
            });

    auto *diffZoomOut = new StackedIconButton(QStringLiteral("Smaller"));
    diffZoomOut->setGlyph(QString::fromUtf8("\xE2\x88\x92")); // −
    diffZoomOut->setToolTip("Smaller diff text");
    connect(diffZoomOut, &QPushButton::clicked, this, [this] { adjustDiffFont(-1); });
    auto *diffZoomIn = new StackedIconButton(QStringLiteral("Larger"));
    diffZoomIn->setGlyph(QStringLiteral("+"));
    diffZoomIn->setToolTip("Larger diff text");
    connect(diffZoomIn, &QPushButton::clicked, this, [this] { adjustDiffFont(1); });
    m_branchSplitButton = new StackedIconButton;
    m_branchSplitButton->setCheckable(true);
    m_branchSplitButton->setChecked(diffSplitPref());
    setOcticon(m_branchSplitButton, "diff", 14);
    updateDiffSplitButton(m_branchSplitButton);
    connect(m_branchSplitButton, &QPushButton::clicked, this, [this](bool on) {
        setDiffSplitPref(on);
        updateDiffSplitButton(m_branchSplitButton);
        for (QPushButton *b : {m_commitSplitButton, m_pullSplitButton}) {
            if (b) {
                b->setChecked(on);
                updateDiffSplitButton(b);
            }
        }
        if (m_branchDiffLastValid) {
            beginBranchDiffLog(QStringLiteral("Switching %1 to the %2 view")
                                   .arg(m_branchDiffBranch,
                                        on ? QStringLiteral("side-by-side")
                                           : QStringLiteral("unified")));
            renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                                  m_branchDiffLastEmpty, m_branchDiffViewedContext);
        } else {
            renderBranchScopeDiff();
        }
    });
    auto *prevChange = new StackedIconButton(QStringLiteral("Prev"));
    prevChange->setToolTip("Previous change");
    setOcticon(prevChange, "chevron-up", 14);
    connect(prevChange, &QPushButton::clicked, this,
            [this] { branchScrollToAdjacentHunk(-1); });
    auto *nextChange = new StackedIconButton(QStringLiteral("Next"));
    nextChange->setToolTip("Next change");
    setOcticon(nextChange, "chevron-down", 14);
    connect(nextChange, &QPushButton::clicked, this,
            [this] { branchScrollToAdjacentHunk(1); });
    for (QPushButton *b : std::initializer_list<QPushButton *>{
             diffZoomOut, diffZoomIn, m_branchSplitButton, prevChange, nextChange}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    m_branchDetailLabel = new QLabel;
    m_branchDetailLabel->setObjectName("sectionLabel");
    m_branchDetailLabel->setTextFormat(Qt::RichText);

    m_branchCloseButton = new QPushButton;
    m_branchCloseButton->setObjectName("ghostButton");
    m_branchCloseButton->setProperty("buttonSize", "sm");
    m_branchCloseButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchCloseButton, "x", 14);
    m_branchCloseButton->setToolTip(
        QStringLiteral("Close this comparison and go back to the working tree"));
    connect(m_branchCloseButton, &QPushButton::clicked, this,
            [this] { closeBranchCompareView(); });

    m_branchOpenPullButton = new StackedIconButton;
    m_branchOpenPullButton->setObjectName("ghostButton");
    m_branchOpenPullButton->setProperty("buttonSize", "sm");
    m_branchOpenPullButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchOpenPullButton, "git-pull-request", 14);
    m_branchOpenPullButton->hide();
    connect(m_branchOpenPullButton, &QPushButton::clicked, this, [this] {
        if (m_branchDiffPullNumber >= 0)
            switchToPullTab(m_branchDiffPullNumber);
    });

    // Reviewing an agent's branch and reading what the agent said are two halves
    // of the same question, so the diff carries a link back to the conversation
    // that produced it. It appears only for branches an agent owns.
    m_branchOpenAgentButton = new StackedIconButton;
    m_branchOpenAgentButton->setObjectName("ghostButton");
    m_branchOpenAgentButton->setProperty("buttonSize", "sm");
    m_branchOpenAgentButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchOpenAgentButton, "rocket", 14); // replaced with session status when shown
    m_branchOpenAgentButton->hide();
    connect(m_branchOpenAgentButton, &QPushButton::clicked, this, [this] {
        const int sessionId = branchDiffAgentSessionId();
        if (sessionId != -1)
            switchToAgentsTab(sessionId);
    });

    m_branchConflictButton = new StackedIconButton("Conflict");
    m_branchConflictButton->setObjectName("ghostButton");
    m_branchConflictButton->setProperty("buttonSize", "sm");
    m_branchConflictButton->setCursor(Qt::PointingHandCursor);
    m_branchConflictButton->hide();
    connect(m_branchConflictButton, &QPushButton::clicked, this, [this] {
        const int sessionId = branchDiffAgentSessionId();
        if (sessionId != -1)
            fixAgentConflictsWithAgent(sessionId);
    });

    m_branchPackageActions = new QWidget;
    auto *packageActionsRow = new QHBoxLayout(m_branchPackageActions);
    packageActionsRow->setContentsMargins(0, 0, 0, 0);
    packageActionsRow->setSpacing(6);
    m_branchPackageActions->hide();
    rebuildBranchPackageActions();

    // A Git run that was killed leaves index.lock (or HEAD.lock) behind in the
    // branch's own git dir, and from then on every write in that checkout fails
    // with "Unable to create ... index.lock: File exists" — staging, committing,
    // merging, all of it. The failure names a path and nothing else, so the
    // header carries both the diagnosis and the one repair, beside the branch it
    // belongs to. It is hidden whenever that checkout has no lock.
    m_branchClearLockButton = new StackedIconButton("Clear lock");
    m_branchClearLockButton->setObjectName("ghostButton");
    m_branchClearLockButton->setProperty("buttonSize", "sm");
    m_branchClearLockButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchClearLockButton, "unlock", 14);
    m_branchClearLockButton->hide();
    connect(m_branchClearLockButton, &QPushButton::clicked, this,
            [this] { clearBranchGitLock(); });

    m_branchMergeEditorButton = new StackedIconButton("Merge editor");
    m_branchMergeEditorButton->setObjectName("ghostButton");
    m_branchMergeEditorButton->setProperty("buttonSize", "sm");
    m_branchMergeEditorButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchMergeEditorButton, "git-merge", 14);
    m_branchMergeEditorButton->setEnabled(false);
    connect(m_branchMergeEditorButton, &QPushButton::clicked, this, [this] {
        if (!m_branchDiffBranch.isEmpty())
            openBranchMergeEditor(m_branchDiffBranch);
    });

    m_branchPullButton = new StackedIconButton("Pull main");
    m_branchPullButton->setObjectName("ghostButton");
    m_branchPullButton->setProperty("buttonSize", "sm");
    m_branchPullButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchPullButton, "download", 14);
    m_branchPullButton->setEnabled(false);
    connect(m_branchPullButton, &QPushButton::clicked, this, [this] {
        const QString branch = m_branchDiffBranch;
        if (!branch.isEmpty())
            updateBranchFromBase(branch);
    });

    m_branchFixButton = new StackedIconButton("Fix with agent");
    m_branchFixButton->setObjectName("ghostButton");
    m_branchFixButton->setProperty("buttonSize", "sm");
    m_branchFixButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchFixButton, "rocket", 14);
    m_branchFixButton->hide();
    connect(m_branchFixButton, &QPushButton::clicked, this, [this] {
        if (m_branchDiffBranch.isEmpty())
            return;
        const QString provider = m_branchFixAgentCombo
                                     ? m_branchFixAgentCombo->currentData().toString()
                                     : QStringLiteral("claude");
        const QString model = m_branchFixModelCombo
                                  ? m_branchFixModelCombo->currentData().toString()
                                  : QString();
        fixBranchConflictsWithAgent(m_branchDiffBranch, provider, model);
    });

    m_branchFixAgentCombo = new QComboBox;
    m_branchFixAgentCombo->setObjectName("issueControlSm");
    m_branchFixAgentCombo->setCursor(Qt::PointingHandCursor);
    m_branchFixAgentCombo->setToolTip("Which agent resolves the conflicts");
    m_branchFixAgentCombo->addItem(QStringLiteral("Claude"), QStringLiteral("claude"));
    m_branchFixAgentCombo->addItem(QStringLiteral("OpenAI"), QStringLiteral("openai"));
    m_branchFixAgentCombo->addItem(QStringLiteral("CC"),
                                   QStringLiteral("claude-code"));
    m_branchFixAgentCombo->hide();
    {
        const QString def = defaultAgentProvider();
        const QString want = def == QLatin1String("claude-api")
                                 ? QStringLiteral("claude")
                                 : def;
        const int idx = m_branchFixAgentCombo->findData(want);
        m_branchFixAgentCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    m_branchFixModelCombo = new QComboBox;
    m_branchFixModelCombo->setObjectName("issueControlSm");
    m_branchFixModelCombo->setCursor(Qt::PointingHandCursor);
    m_branchFixModelCombo->setToolTip("Which model the agent uses");
    m_branchFixModelCombo->setProperty("claudeModelCombo", true);
    m_branchFixModelCombo->view()->installEventFilter(this);
    m_branchFixModelCombo->hide();
    fillAgentFixModelCombo(m_branchFixModelCombo,
                           m_branchFixAgentCombo->currentData().toString());
    applyLiveClaudeModelsToCombos();
    connect(m_branchFixAgentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_branchFixAgentCombo && m_branchFixModelCombo) {
                    fillAgentFixModelCombo(
                        m_branchFixModelCombo,
                        m_branchFixAgentCombo->currentData().toString());
                    applyLiveClaudeModelsToCombos();
                }
            });

    m_branchPrButton = new StackedIconButton("Create PR");
    m_branchPrButton->setObjectName("ghostButton");
    m_branchPrButton->setProperty("buttonSize", "sm");
    m_branchPrButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchPrButton, "git-pull-request", 14);
    m_branchPrButton->setEnabled(false);
    connect(m_branchPrButton, &QPushButton::clicked, this, [this] {
        if (!m_branchDiffBranch.isEmpty())
            createPullFromBranch(m_branchDiffBranch);
    });

    m_branchQueueButton = new StackedIconButton("Queue merge");
    m_branchQueueButton->setObjectName("ghostButton");
    m_branchQueueButton->setProperty("buttonSize", "sm");
    m_branchQueueButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchQueueButton, "list-unordered", 14);
    m_branchQueueButton->setEnabled(false);
    m_branchQueueButton->hide();
    connect(m_branchQueueButton, &QPushButton::clicked, this, [this] {
        if (!m_branchDiffBranch.isEmpty())
            queueBranchForMerge(m_branchDiffBranch);
    });

    m_branchMergeButton = new StackedIconButton("Merge to main");
    m_branchMergeButton->setObjectName("ghostButton");
    m_branchMergeButton->setProperty("buttonSize", "sm");
    m_branchMergeButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchMergeButton, "check-circle", 14);
    m_branchMergeButton->setEnabled(false);
    connect(m_branchMergeButton, &QPushButton::clicked, this, [this] {
        if (!m_branchDiffBranch.isEmpty()) {
            requestMergeWorktreeIntoMain(m_branchDiffBranch, QString());
        }
    });

    m_branchMergeDeleteButton = new StackedIconButton("Merge & clean up");
    m_branchMergeDeleteButton->setObjectName("primaryButton");
    m_branchMergeDeleteButton->setProperty("buttonSize", "sm");
    m_branchMergeDeleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchMergeDeleteButton, "check-circle", 14);
    m_branchMergeDeleteButton->setEnabled(false);
    connect(m_branchMergeDeleteButton, &QPushButton::clicked, this, [this] {
        if (m_branchDiffBranch.isEmpty())
            return;
        forkmesh::sonify::UiSounds::play(forkmesh::sonify::Cue::Merge);
        requestMergeWorktreeIntoMain(
            m_branchDiffBranch, QString(), /*cleanUp=*/true,
            /*findWorktree=*/true);
    });

    m_branchDeleteCleanupButton = new StackedIconButton("Delete & clean up");
    m_branchDeleteCleanupButton->setObjectName("dangerButton");
    m_branchDeleteCleanupButton->setProperty("buttonSize", "sm");
    m_branchDeleteCleanupButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_branchDeleteCleanupButton, "trash", 14);
    m_branchDeleteCleanupButton->setEnabled(false);
    connect(m_branchDeleteCleanupButton, &QPushButton::clicked, this, [this] {
        if (!m_branchDiffBranch.isEmpty())
            deleteReviewedBranchAndCleanUp(m_branchDiffBranch);
    });

    // The whole point of the Git splitter is that either side can be dragged
    // to whatever width the work needs, and a one-line header of a dozen
    // actions plus a full branch pair used to be the thing standing in the way:
    // its width became the pane's minimum. So the row wraps instead. The
    // actions keep the top line and give way among themselves, and the branch
    // title drops onto the next line as soon as it no longer fits beside them.
    auto *detailHeader = new QWidget;
    detailHeader->setObjectName(QStringLiteral("branchRangeHeader"));
    detailHeader->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *detailBar = new HeaderWrapLayout(detailHeader);
    detailBar->setContentsMargins(0, 0, 12, 0);
    detailBar->setLead(m_branchCloseButton);
    detailBar->setTitle(m_branchDetailLabel);
    detailBar->addAction(diffZoomOut);
    detailBar->addAction(diffZoomIn);
    detailBar->addAction(m_branchSplitButton);
    detailBar->addAction(prevChange);
    detailBar->addAction(nextChange);
    detailBar->addActionSpacing(12);
    // First of the branch actions: while it is showing, none of the ones after
    // it can succeed.
    detailBar->addAction(m_branchClearLockButton);
    detailBar->addAction(m_branchOpenPullButton);
    detailBar->addAction(m_branchOpenAgentButton);
    detailBar->addAction(m_branchConflictButton);
    detailBar->addAction(m_branchPackageActions);
    detailBar->addAction(m_branchMergeEditorButton);
    detailBar->addAction(m_branchPullButton);
    detailBar->addAction(m_branchFixButton);
    detailBar->addAction(m_branchFixAgentCombo);
    detailBar->addAction(m_branchFixModelCombo);
    detailBar->addAction(m_branchPrButton);
    detailBar->addAction(m_branchQueueButton);
    detailBar->addAction(m_branchMergeButton);
    detailBar->addAction(m_branchDeleteCleanupButton);
    detailBar->addAction(m_branchMergeDeleteButton);

    buildBranchDiffSummaryBar();
    m_branchSecretBanner = new forkmesh::ui::SecretDiffBanner;
    m_branchSecretBanner->setOuterMargins(0, 0, 12, 0);
    m_branchSecretBanner->onIgnore =
        [this](const QList<RepoSecurityFinding> &findings) {
            ignoreBranchDiffSecrets(findings);
        };

    buildBranchDiffSearchBar();

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(detailHeader);
    layout->addWidget(m_branchDiffSummaryBar);
    layout->addWidget(m_branchSecretBanner);
    layout->addWidget(buildBranchDiffLogPanel());
    layout->addWidget(m_branchDiffSearchBar);
    layout->addWidget(m_branchDiffView, 1);

    auto *closeSearchShortcut =
        new QShortcut(QKeySequence(Qt::Key_Escape), m_branchDiffSearchInput);
    closeSearchShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeSearchShortcut, &QShortcut::activated, this,
            [this] { toggleBranchDiffSearch(false); });
    return page;
}

void MainWindow::buildBranchDiffSummaryBar()
{
    // Review progress and merge health, above the diff rather than inside it:
    // the left side says how much has been read, while six evidence-backed
    // scores keep the main merge risks visible at a glance. The strip is one
    // line tall and disappears with the diff it summarises.
    m_branchDiffSummaryBar = new QWidget;
    {
        // The strip is hidden whenever there is no diff to summarise, so it
        // lives in its own container: a hidden widget takes its layout spacing
        // with it, a hidden child of a nested layout does not.
        auto *summaryRow = new QHBoxLayout(m_branchDiffSummaryBar);
        summaryRow->setContentsMargins(0, 0, 12, 0);
        summaryRow->setSpacing(0);
        auto *summaryStrip = new QFrame(m_branchDiffSummaryBar);
        summaryStrip->setObjectName(QStringLiteral("diffSummaryBar"));
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        summaryStrip->setStyleSheet(Theme::remapSheet(
            QStringLiteral("#diffSummaryBar{background:%1;border:1px solid %2;"
                           "border-radius:6px;}"
                           "#diffSummaryBar QLabel{background:transparent;}")
                .arg(dark ? QStringLiteral("#161b22") : QStringLiteral("#f6f8fa"),
                     dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de"))));
        // Seven fixed captions in a row would have set the floor for how far
        // the Git splitter can be dragged, so they flow instead: the health
        // scores wrap onto a second and third line as the pane narrows.
        auto *summaryLayout = new FlowLayout(summaryStrip, 0, 6, 4);
        summaryLayout->setContentsMargins(12, 4, 12, 4);
        QSizePolicy stripPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        stripPolicy.setHeightForWidth(true);
        summaryStrip->setSizePolicy(stripPolicy);
        m_branchDiffSummaryLabel = new QLabel(summaryStrip);
        m_branchDiffSummaryLabel->setObjectName(
            QStringLiteral("branchDiffReviewSummary"));
        m_branchDiffSummaryLabel->setTextFormat(Qt::RichText);
        m_branchDiffSummaryLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        m_branchDiffSummaryLabel->setSizePolicy(QSizePolicy::Maximum,
                                                QSizePolicy::Preferred);
        // The review sentence is the widest thing in the strip, and its own
        // width must not become the pane's minimum: it keeps that width while
        // there is room to flow it, and gives up its tail once there is not.
        m_branchDiffSummaryLabel->setMinimumWidth(1);
        summaryLayout->addWidget(m_branchDiffSummaryLabel);
        m_branchDiffHealthLabels.clear();
        const QStringList healthKeys{
            QStringLiteral("merge"),      QStringLiteral("complexity"),
            QStringLiteral("quality"),    QStringLiteral("security"),
            QStringLiteral("network"),    QStringLiteral("practices"),
        };
        for (const QString &key : healthKeys) {
            auto *metric = new QLabel(summaryStrip);
            metric->setObjectName(QStringLiteral("branchDiffHealth-") + key);
            metric->setTextFormat(Qt::RichText);
            metric->setTextInteractionFlags(Qt::NoTextInteraction);
            metric->setAlignment(Qt::AlignCenter);
            metric->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            metric->setCursor(Qt::WhatsThisCursor);
            summaryLayout->addWidget(metric);
            m_branchDiffHealthLabels.append(metric);
        }
        m_branchDiffLogButton = new QPushButton(summaryStrip);
        m_branchDiffLogButton->setObjectName(QStringLiteral("branchDiffLogButton"));
        m_branchDiffLogButton->setCheckable(true);
        m_branchDiffLogButton->setCursor(Qt::PointingHandCursor);
        m_branchDiffLogButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_branchDiffLogButton->setAccessibleName(QStringLiteral("Activity log"));
        m_branchDiffLogButton->setStyleSheet(
            QStringLiteral("QPushButton#branchDiffLogButton{background:%1;color:%2;"
                           "border:1px solid %3;border-radius:8px;"
                           "padding:1px 8px;font-weight:600;min-height:0;}"
                           "QPushButton#branchDiffLogButton:hover{border-color:%4;}"
                           "QPushButton#branchDiffLogButton:checked{background:%5;}")
                .arg(dark ? QStringLiteral("#21262d") : QStringLiteral("#ffffff"),
                     dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"),
                     dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de"),
                     dark ? QStringLiteral("#8b949e") : QStringLiteral("#8c959f"),
                     dark ? QStringLiteral("#30363d") : QStringLiteral("#eaeef2")));
        connect(m_branchDiffLogButton, &QPushButton::clicked, this,
                [this] { setBranchDiffLogCollapsed(!m_branchDiffLogCollapsed); });
        m_branchDiffLogButton->hide();
        summaryLayout->addWidget(m_branchDiffLogButton);
        summaryRow->addWidget(summaryStrip, 1);
    }
    m_branchDiffSummaryBar->hide();
}

void MainWindow::buildBranchDiffSearchBar()
{
    m_branchDiffSearchInput = new QLineEdit;
    m_branchDiffSearchInput->setObjectName("issueSearch");
    m_branchDiffSearchInput->setPlaceholderText("Find in diff\xE2\x80\xA6");
    m_branchDiffSearchInput->setClearButtonEnabled(true);
    connect(m_branchDiffSearchInput, &QLineEdit::textChanged, this,
            [this] { branchDiffSearchRecompute(); });
    connect(m_branchDiffSearchInput, &QLineEdit::returnPressed, this, [this] {
        branchDiffSearchGoTo(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier
                                 ? -1
                                 : 1);
    });
    m_branchDiffSearchCount = new QLabel;
    m_branchDiffSearchCount->setObjectName("hintLabel");
    auto *searchPrev = new QPushButton;
    searchPrev->setToolTip("Previous match");
    setOcticon(searchPrev, "chevron-up", 14);
    connect(searchPrev, &QPushButton::clicked, this,
            [this] { branchDiffSearchGoTo(-1); });
    auto *searchNext = new QPushButton;
    searchNext->setToolTip("Next match");
    setOcticon(searchNext, "chevron-down", 14);
    connect(searchNext, &QPushButton::clicked, this,
            [this] { branchDiffSearchGoTo(1); });
    auto *searchClose = new QPushButton;
    searchClose->setToolTip("Close find bar");
    setOcticon(searchClose, "x", 14);
    connect(searchClose, &QPushButton::clicked, this,
            [this] { toggleBranchDiffSearch(false); });
    for (QPushButton *b : {searchPrev, searchNext, searchClose}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    m_branchDiffSearchBar = new QWidget;
    auto *searchBarLayout = new QHBoxLayout(m_branchDiffSearchBar);
    searchBarLayout->setContentsMargins(0, 0, 12, 6);
    searchBarLayout->addWidget(m_branchDiffSearchInput, 1);
    searchBarLayout->addWidget(m_branchDiffSearchCount);
    searchBarLayout->addWidget(searchPrev);
    searchBarLayout->addWidget(searchNext);
    searchBarLayout->addWidget(searchClose);
    m_branchDiffSearchBar->setVisible(false);
}

MainWindow::BranchesPanelData
MainWindow::readBranchesPanelGit(BranchesPanelData data)
{
    const QString &dir = data.dir;
    if (dir.isEmpty())
        return data;

    data.branches = listRepoBranches(dir);
    data.base = chooseDefaultBranch(data.branches, data.configuredDefault, dir,
                                    data.checkedOut);
    const QString &base = data.base;
    if (!base.isEmpty() && data.branches.removeOne(base))
        data.branches.prepend(base);
    data.selected = data.checkedOut.isEmpty() ? base : data.checkedOut;

    QByteArray rout;
    if (runGitCapture(dir,
                      {"for-each-ref", "--sort=-committerdate",
                       "--format=%(refname:short)", "refs/remotes/"},
                      &rout, nullptr)) {
        for (const QString &line :
             QString::fromUtf8(rout).split('\n', Qt::SkipEmptyParts)) {
            const QString ref = line.trimmed();
            if (ref.isEmpty() || !ref.contains(u'/') ||
                ref.endsWith(QLatin1String("/HEAD")))
                continue;
            const QString branch = ref.section(QLatin1Char('/'), 1);
            if (branch == QLatin1String("forkmesh/pulls") ||
                branch.startsWith(QLatin1String("forkmesh/pull/")))
                continue;
            if (!data.remoteBranches.contains(ref))
                data.remoteBranches.append(ref);
        }
    }

    QByteArray times;
    if (runGitCapture(
            dir,
            {"for-each-ref",
             "--format=%(refname:short)%09%(committerdate:unix)%09"
             "%(objectname:short)%09%(subject)%09%(authorname)",
             "refs/heads/", "refs/remotes/"},
            &times, nullptr)) {
        for (const QString &line :
             QString::fromUtf8(times).split('\n', Qt::SkipEmptyParts)) {
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 2)
                continue;
            const QString ref = parts.at(0);
            data.times.insert(ref, parts.at(1).toLongLong());
            if (parts.size() >= 3)
                data.shortShas.insert(ref, parts.at(2));
            if (parts.size() >= 4)
                data.subjects.insert(ref, parts.at(3));
            if (parts.size() >= 5)
                data.authors.insert(ref, parts.at(4));
        }
    }

    {
        const QSet<QString> localBranches(data.branches.cbegin(), data.branches.cend());
        QStringList shown;
        for (const QString &ref : std::as_const(data.remoteBranches)) {
            const QString remote = ref.section(QLatin1Char('/'), 0, 0);
            const QString branch = ref.section(QLatin1Char('/'), 1);
            if (remote == QLatin1String("forkmesh-cores"))
                continue;
            if (localBranches.contains(branch)) {
                const QString remoteSha = data.shortShas.value(ref);
                if (!remoteSha.isEmpty() && remoteSha == data.shortShas.value(branch))
                    continue;
                if (runGitCapture(dir,
                                  {"merge-base", "--is-ancestor",
                                   QStringLiteral("refs/remotes/") + ref,
                                   QStringLiteral("refs/heads/") + branch},
                                  nullptr, nullptr))
                    continue;
            }
            shown.append(ref);
        }
        data.remoteBranches = shown;
    }

    if (!base.isEmpty() && !data.remoteBranches.isEmpty()) {
        QByteArray ab;
        if (runGitCapture(
                dir,
                {"for-each-ref",
                 QStringLiteral("--format=%(refname:short) %(ahead-behind:%1)").arg(base),
                 "refs/remotes/"},
                &ab, nullptr)) {
            for (const QString &line :
                 QString::fromUtf8(ab).split('\n', Qt::SkipEmptyParts)) {
                const QStringList parts = line.split(
                    QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (parts.size() >= 3)
                    data.remoteAheadBehind.insert(
                        parts.first(),
                        qMakePair(parts.at(1).toInt(), parts.at(2).toInt()));
            }
        }
    }

    if (!base.isEmpty()) {
        QByteArray ab;
        if (runGitCapture(
                dir,
                {"for-each-ref",
                 QStringLiteral("--format=%(refname:short) %(ahead-behind:%1)").arg(base),
                 "refs/heads/"},
                &ab, nullptr)) {
            for (const QString &line :
                 QString::fromUtf8(ab).split('\n', Qt::SkipEmptyParts)) {
                const QStringList parts = line.split(
                    QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (parts.size() >= 3)
                    data.localAheadBehind.insert(
                        parts.first(),
                        qMakePair(parts.at(1).toInt(), parts.at(2).toInt()));
            }
        }
        for (const QString &branch : std::as_const(data.branches)) {
            if (branch == base || data.localAheadBehind.contains(branch))
                continue;
            QByteArray counts;
            if (!runGitCapture(dir,
                               {"rev-list", "--left-right", "--count",
                                base + "..." + branch},
                               &counts, nullptr))
                continue;
            const QStringList parts = QString::fromUtf8(counts).trimmed().split(
                QRegularExpression(QStringLiteral("\\s+")));
            if (parts.size() >= 2)
                data.localAheadBehind.insert(
                    branch, qMakePair(parts.at(1).toInt(), parts.at(0).toInt()));
        }
    }

    const QString commonDir = gitCommonDirOf(dir);
    for (const QString &branch : std::as_const(data.branches))
        data.histories.insert(branch, readBranchRefHistory(commonDir, branch));

    QByteArray wtOut;
    if (cachedWorktreeListPorcelain(dir, &wtOut)) {
        // Compare canonical paths so a symlinked checkout root (e.g. /tmp on
        // some platforms) doesn't make the main worktree look like a separate
        // branch worktree; fall back to the absolute path if it can't resolve.
        QString mainPath = QFileInfo(dir).canonicalFilePath();
        if (mainPath.isEmpty())
            mainPath = QDir(dir).absolutePath();
        QString currentPath;
        for (const QString &raw : QString::fromUtf8(wtOut).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("worktree ")))
                currentPath = line.mid(9).trimmed();
            else if (line.startsWith(QLatin1String("branch "))) {
                const QString br =
                    line.mid(7).trimmed().replace(QLatin1String("refs/heads/"),
                                                  QString());
                QString canonicalPath = QFileInfo(currentPath).canonicalFilePath();
                if (canonicalPath.isEmpty())
                    canonicalPath = QDir(currentPath).absolutePath();
                if (!br.isEmpty() && !currentPath.isEmpty() && canonicalPath != mainPath)
                    data.worktrees.insert(br, currentPath);
            }
        }
    }
    return data;
}

void MainWindow::loadBranchesPanel()
{
    loadBranchesPanelAsOf(m_branchDiffSeq);
}

void MainWindow::loadBranchesPanelAsOf(quint64 diffReadSeq)
{
    if (!m_branchesTable)
        return;
    if (m_branchesPanelLoading) {
        m_branchesPanelReloadQueued = true;
        m_branchesPanelQueuedSeq = diffReadSeq; // the latest ask wins
        return;
    }
    startBranchesPanelLoad(diffReadSeq);
}

void MainWindow::deferUntilBranchDiffRead(std::function<void()> work)
{
    std::function<void()> earlier = std::move(m_branchDiffAfterRead);
    m_branchDiffAfterRead = [earlier = std::move(earlier), work = std::move(work)] {
        if (earlier)
            earlier();
        work();
    };
    const int gen = ++m_branchDiffAfterReadGen;
    QTimer::singleShot(kBranchDiffAfterReadMaxMs, this, [this, gen] {
        if (gen == m_branchDiffAfterReadGen)
            flushBranchDiffAfterRead();
    });
}

void MainWindow::flushBranchDiffAfterRead()
{
    if (!m_branchDiffAfterRead)
        return;
    std::function<void()> work = std::move(m_branchDiffAfterRead);
    m_branchDiffAfterRead = nullptr;
    ++m_branchDiffAfterReadGen;
    work();
}

void MainWindow::startBranchesPanelLoad(quint64 diffReadSeq)
{
    BranchesPanelData data;
    data.dir = repoGitDir();
    data.configuredDefault = m_repoInfo.defaultBranch.trimmed();
    data.checkedOut = m_repoBranch;
    data.writable = repoHasWorkingTree();
    data.diffReadSeq = diffReadSeq;

    m_branchesPanelLoading = true;
    const int gen = ++m_branchesPanelGen;
    runOffThread<BranchesPanelData>(
        [data] { return readBranchesPanelGit(data); },
        [this, gen](BranchesPanelData loaded) {
            if (gen != m_branchesPanelGen)
                return; // a newer load already owns the table
            m_branchesPanelLoading = false;
            if (m_branchesTable) {
                renderBranchesPanel(loaded);
                refreshBranchLinks();
            }
            if (m_branchesPanelReloadQueued) {
                m_branchesPanelReloadQueued = false;
                if (m_branchesTable)
                    startBranchesPanelLoad(m_branchesPanelQueuedSeq);
            } else if (m_branchesTable) {
                startBranchChangeStats(loaded);
            }
        });
}

void MainWindow::startBranchChangeStats(const BranchesPanelData &data)
{
    if (data.dir.isEmpty() || data.base.isEmpty() || data.branches.isEmpty()) {
        m_branchChangeStatsLoading = false;
        return;
    }

    const int gen = ++m_branchChangeStatsGen;
    m_branchChangeStatsLoading = true;
    const QString dir = data.dir;
    const QString base = data.base;
    const QStringList branches = data.branches;
    const QHash<QString, QString> shas = data.shortShas;
    const QHash<QString, QPair<int, int>> aheadBehind = data.localAheadBehind;
    const QHash<QString, BranchChangeStat> cache = m_branchChangeStatsCache;

    runOffThread<QHash<QString, BranchChangeStat>>(
        [dir, base, branches, shas, aheadBehind, cache] {
            ensureGitCommitGraph(dir);
            QHash<QString, BranchChangeStat> result;
            for (const QString &branch : branches) {
                const QString key = branchConflictKey(
                    dir, shas.value(base), shas.value(branch));
                const auto cached = cache.constFind(key);
                if (!key.isEmpty() && cached != cache.constEnd()) {
                    result.insert(branch, *cached);
                    continue;
                }

                BranchChangeStat stat;
                const auto counts = aheadBehind.constFind(branch);
                if (branch == base ||
                    (counts != aheadBehind.constEnd() && counts->first == 0)) {
                    stat.files = stat.added = stat.removed = 0;
                    result.insert(branch, stat);
                    continue;
                }

                QByteArray numstat;
                if (runGitCapture(dir,
                                  {QStringLiteral("diff"), QStringLiteral("--numstat"),
                                   base + QStringLiteral("...") + branch},
                                  &numstat, nullptr)) {
                    stat.files = stat.added = stat.removed = 0;
                    for (const QByteArray &line : numstat.split('\n')) {
                        if (line.trimmed().isEmpty())
                            continue;
                        const QList<QByteArray> fields = line.split('\t');
                        if (fields.size() < 3)
                            continue;
                        ++stat.files;
                        bool addOk = false;
                        bool removeOk = false;
                        const int add = fields.at(0).toInt(&addOk);
                        const int remove = fields.at(1).toInt(&removeOk);
                        if (addOk)
                            stat.added += add;
                        if (removeOk)
                            stat.removed += remove;
                    }
                }
                result.insert(branch, stat);
            }
            return result;
        },
        [this, gen, data](QHash<QString, BranchChangeStat> stats) {
            if (gen != m_branchChangeStatsGen)
                return;
            m_branchChangeStatsLoading = false;
            if (m_branchChangeStatsCache.size() > 4096)
                m_branchChangeStatsCache.clear();
            for (auto it = stats.constBegin(); it != stats.constEnd(); ++it) {
                const QString key = branchConflictKey(
                    data.dir, data.shortShas.value(data.base),
                    data.shortShas.value(it.key()));
                if (!key.isEmpty())
                    m_branchChangeStatsCache.insert(key, it.value());
            }

            if (!m_branchesTable || repoGitDir() != data.dir)
                return;
            {
                BranchSortPause sortPause(m_branchesTable);
                for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
                    QTableWidgetItem *name =
                        m_branchesTable->item(row, kBranchesNameColumn);
                    QTableWidgetItem *changes =
                        m_branchesTable->item(row, kBranchesChangesColumn);
                    if (!name || !changes)
                        continue;
                    const auto stat = stats.constFind(name->text());
                    if (stat == stats.constEnd() || stat->files < 0)
                        continue;
                    setBranchChanges(changes, stat->files, stat->added,
                                     stat->removed);
                }
            }
            growColumnsToContents(m_branchesTable, 260);
            m_branchesTable->viewport()->update();
            warmBranchDiffs(data);
        });
}

void MainWindow::refreshBranchLinks()
{
    if (!m_branchesTable)
        return;
    QString owner;
    QString repoName;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        repoName = m_repositories.at(m_repoDetailIndex).name;
    }
    const QString repository = owner + QLatin1Char('/') + repoName;

    QHash<QString, AgentSession> branchSessions;
    for (const AgentSession &session : m_agentSessions) {
        if (session.branchName.isEmpty() || owner.isEmpty() ||
            session.owner != owner || session.name != repoName)
            continue;
        const auto existing = branchSessions.constFind(session.branchName);
        if (existing == branchSessions.constEnd() ||
            (session.issueNumber > 0 && existing->issueNumber <= 0) ||
            ((session.issueNumber > 0) == (existing->issueNumber > 0) &&
             session.id > existing->id))
            branchSessions.insert(session.branchName, session);
    }

    QHash<QString, PullRequest> branchPulls;
    for (const PullRequest &pull : std::as_const(m_currentPulls)) {
        if (pull.head.isEmpty() || pull.number <= 0)
            continue;
        const bool open = pull.status == QLatin1String("open");
        const auto existing = branchPulls.constFind(pull.head);
        if (existing == branchPulls.constEnd() ||
            (open && existing->status != QLatin1String("open")) ||
            (open == (existing->status == QLatin1String("open")) &&
             pull.number > existing->number))
            branchPulls.insert(pull.head, pull);
    }

    QHash<QString, QString> taskTitles;
    QHash<QString, QStringList> branchTasks;
    const auto addTaskItems = [&](const QString &id, const QJsonArray &items) {
        for (const forkmesh::taskwork::WorkItem &item :
             forkmesh::taskwork::fromJsonArray(items)) {
            if (item.branch.isEmpty() ||
                item.repository.compare(repository, Qt::CaseInsensitive) != 0)
                continue;
            QStringList &ids = branchTasks[item.branch];
            if (!ids.contains(id))
                ids.append(id);
        }
    };
    for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
        const QJsonObject task = value.toObject();
        const QString id = task.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        taskTitles.insert(id, task.value(QStringLiteral("title")).toString());
        addTaskItems(id, task.value(QStringLiteral("workItems")).toArray());
    }
    const QJsonObject cachedTasks = taskWorkItemCache();
    for (auto it = cachedTasks.constBegin(); it != cachedTasks.constEnd(); ++it)
        addTaskItems(it.key(),
                     it.value().toObject().value(QStringLiteral("items")).toArray());

    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    BranchSortPause sortPause(m_branchesTable);
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        QTableWidgetItem *name = m_branchesTable->item(row, kBranchesNameColumn);
        QTableWidgetItem *linksItem = m_branchesTable->item(row, kBranchesLinksColumn);
        if (!name || !linksItem)
            continue;
        const QString branch = name->text();
        QString sortKey;
        QStringList labels;
        QStringList tips;
        int issueNumber = 0;
        QString issueTitle;
        int prNumber = 0;
        QStringList taskIds = branchTasks.value(branch);

        const auto sessionIt = branchSessions.constFind(branch);
        if (sessionIt != branchSessions.constEnd()) {
            const AgentSession &session = sessionIt.value();
            name->setIcon(agentStatusOcticon(session));
            const QString who = !session.teammateName.isEmpty()
                                    ? session.teammateName
                                    : session.provider;
            QString agentTip = session.cloud
                                   ? QStringLiteral("Cloud run")
                                   : QStringLiteral("Agent #%1").arg(session.id);
            agentTip += dot + (session.merged ? QStringLiteral("merged")
                                              : agentStatusText(session.status));
            if (!who.isEmpty())
                agentTip += dot + who;
            if (session.issueNumber <= 0 && !session.issueTitle.isEmpty())
                agentTip += QStringLiteral(": ") + session.issueTitle;
            labels.append(session.cloud ? QStringLiteral("Cloud run")
                                        : QStringLiteral("Agent #%1").arg(session.id));
            sortKey = QStringLiteral("0 %1").arg(session.id, 10, 10, QLatin1Char('0'));
            tips.append(agentTip);
            issueNumber = session.issueNumber;
            if (issueNumber > 0)
                issueTitle = session.issueTitle;
            prNumber = session.prNumber;
            if (session.orgTask && !session.orgTaskId.isEmpty() &&
                !taskIds.contains(session.orgTaskId))
                taskIds.prepend(session.orgTaskId);
        } else {
            name->setIcon(QIcon());
        }

        if (issueNumber > 0) {
            labels.append(QStringLiteral("#%1").arg(issueNumber));
            tips.append(issueTitle.isEmpty()
                            ? QStringLiteral("Issue #%1").arg(issueNumber)
                            : QStringLiteral("Issue #%1: %2").arg(issueNumber).arg(issueTitle));
        }

        const auto pullIt = branchPulls.constFind(branch);
        if (pullIt != branchPulls.constEnd() &&
            (prNumber <= 0 || prNumber == pullIt->number)) {
            prNumber = pullIt->number;
            labels.append(QStringLiteral("PR #%1").arg(prNumber));
            tips.append(QStringLiteral("Pull request #%1: %2 (%3)")
                            .arg(prNumber)
                            .arg(pullIt->title, pullIt->status));
        } else if (prNumber > 0) {
            labels.append(QStringLiteral("PR #%1").arg(prNumber));
            tips.append(QStringLiteral("Pull request #%1").arg(prNumber));
        }

        if (!taskIds.isEmpty()) {
            labels.append(taskIds.size() == 1
                              ? QStringLiteral("Task")
                              : QStringLiteral("%1 tasks").arg(taskIds.size()));
            for (const QString &id : std::as_const(taskIds)) {
                const QString title = taskTitles.value(id);
                tips.append(title.isEmpty() ? QStringLiteral("Task")
                                            : QStringLiteral("Task: %1").arg(title));
            }
        }

        if (sortKey.isEmpty() && prNumber > 0)
            sortKey = QStringLiteral("1 %1").arg(prNumber, 10, 10, QLatin1Char('0'));
        if (sortKey.isEmpty() && !taskIds.isEmpty())
            sortKey = QStringLiteral("2 ") + taskTitles.value(taskIds.first());
        linksItem->setData(kBranchLinksRole, labels);
        linksItem->setData(kTableSortRole,
                           sortKey.isEmpty() ? QStringLiteral("~") : sortKey);
        linksItem->setToolTip(tips.join(QLatin1Char('\n')));
    }
    growColumnsToContents(m_branchesTable, 260);
    m_branchesTable->viewport()->update();
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testBranchLinks(const QString &branch) const
{
    if (!m_branchesTable)
        return QString();
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_branchesTable->item(row, kBranchesNameColumn);
        const QTableWidgetItem *links = m_branchesTable->item(row, kBranchesLinksColumn);
        if (item && links && item->text() == branch)
            return links->data(kBranchLinksRole).toStringList().join(QLatin1Char('|'));
    }
    return QString();
}
#endif

void MainWindow::renderBranchesPanel(const BranchesPanelData &data)
{
    TableRepaintGuard repaintGuard(m_branchesTable);
    QSignalBlocker branchesTableBlock(m_branchesTable);
    BranchSortPause sortPause(m_branchesTable);
    m_branchesTable->setRowCount(data.branches.size() +
                                 data.remoteBranches.size());
    for (int r = 0, rows = m_branchesTable->rowCount(); r < rows; ++r) {
        if (m_branchesTable->cellWidget(r, kBranchesNameColumn))
            m_branchesTable->removeCellWidget(r, kBranchesNameColumn);
    }
    int nextRow = 0;
    const QString &dir = data.dir;
    const QStringList &branches = data.branches;
    const QString &base = data.base;
    const QString &selected = data.selected;
    // Read now, not when the load began: a click while the load ran moved the
    // pane on, and reselecting the branch it left would light up the wrong row.
    const QString previouslyViewed = m_branchDiffBranch;
    const bool writable = data.writable;
    const QStringList &remoteBranches = data.remoteBranches;
    const QHash<QString, qint64> &branchTimes = data.times;
    const QHash<QString, QString> &branchShortShas = data.shortShas;
    const QHash<QString, QString> &branchSubjects = data.subjects;
    const QHash<QString, QString> &branchAuthors = data.authors;
    const QHash<QString, QPair<int, int>> &localAheadBehind = data.localAheadBehind;
    const QHash<QString, QPair<int, int>> &remoteAheadBehind = data.remoteAheadBehind;
    const QHash<QString, QString> &branchWorktrees = data.worktrees;
    m_branchesPanelDir = dir;

    if (m_branchesSummary) {
        const QString def = base.isEmpty() ? QStringLiteral("none") : base;
        const QString total =
            remoteBranches.isEmpty()
                ? QString::number(branches.size())
                : QString::fromUtf8("%1 local \xC2\xB7 %2 remote")
                      .arg(branches.size())
                      .arg(remoteBranches.size());
        m_branchesSummary->setText(
            QString::fromUtf8("\xC2\xB7 %1 \xC2\xB7 default: %2").arg(total, def));
    }

    int actionWidth = 0;
    int actionButtonMinWidth = 0; // measured from the first row's delete button
    bool anyFastForwardable = false;
    bool anyMerged = false; // fully-merged branches the "Delete merged" action can remove
    QList<QPair<QString, QString>> conflictProbes;
    for (const QString &branch : branches) {
        const int row = nextRow++;
        const QString sha = branchShortShas.value(branch);
        const QString subject = branchSubjects.value(branch);
        const QString author = branchAuthors.value(branch);
        const BranchRefHistory history = data.histories.value(branch);

        auto *name = new QTableWidgetItem(branch);
        {
            QString tip;
            if (!sha.isEmpty())
                tip = sha;
            if (!subject.isEmpty())
                tip += (tip.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ")) + subject;
            if (!author.isEmpty())
                tip += (tip.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 by ")) + author;
            if (history.createdAt > 0)
                tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) +
                       QStringLiteral("Created %1").arg(branchStampTip(history.createdAt));
            if (!tip.isEmpty())
                name->setToolTip(tip);
        }
        m_branchesTable->setItem(row, kBranchesNameColumn, name);
        m_branchesTable->setItem(row, kBranchesLinksColumn,
                                 branchSortItem(QString(), QStringLiteral("~")));

        auto *changes = branchSortItem(QString(), -1);
        {
            BranchChangeStat stat;
            bool known = false;
            if (branch == base) {
                stat.files = stat.added = stat.removed = 0;
                known = true;
            } else {
                const QString key = branchConflictKey(
                    dir, branchShortShas.value(base), sha);
                const auto cached = m_branchChangeStatsCache.constFind(key);
                if (!key.isEmpty() && cached != m_branchChangeStatsCache.constEnd()) {
                    stat = *cached;
                    known = true;
                }
            }
            if (known && stat.files >= 0)
                setBranchChanges(changes, stat.files, stat.added, stat.removed);
        }
        m_branchesTable->setItem(row, kBranchesChangesColumn, changes);

        int behind = 0;
        int ahead = 0;
        bool divergenceKnown = branch == base;
        bool hasConflict = false;
        if (!dir.isEmpty() && branch != base) {
            const auto abIt = localAheadBehind.constFind(branch);
            if (abIt != localAheadBehind.constEnd()) {
                divergenceKnown = true;
                ahead = abIt->first;
                behind = abIt->second;
            }
            if (behind > 0 && ahead > 0) {
                const QString key = branchConflictKey(
                    dir, branchShortShas.value(base), sha);
                const auto cached = m_branchConflictCache.constFind(key);
                if (!key.isEmpty() && cached != m_branchConflictCache.constEnd())
                    hasConflict = *cached;
                else if (!key.isEmpty())
                    conflictProbes.append(qMakePair(branch, key));
            }
        }
        if (divergenceKnown)
            name->setData(kBranchBehindRole, behind);
        name->setData(kBranchConflictRole, hasConflict);
        QString healthTip;
        if (branch != base && divergenceKnown) {
            healthTip = QStringLiteral("%1 commit(s) behind · %2 ahead of %3")
                            .arg(behind)
                            .arg(ahead)
                            .arg(base);
            if (behind > 0)
                healthTip += QStringLiteral("\nMerge %1 into this branch to update it")
                                 .arg(base);
        }
        if (hasConflict)
            healthTip += (healthTip.isEmpty() ? QString() : QStringLiteral("\n")) +
                         QStringLiteral("Conflicts with %1").arg(base);
        const QString existingTip = name->toolTip();
        if (!healthTip.isEmpty())
            name->setToolTip(existingTip.isEmpty()
                                 ? healthTip
                                 : existingTip + QLatin1Char('\n') + healthTip);
        m_branchesTable->setItem(
            row, kBranchesStatusColumn,
            branchStateItem(base, branch == base, divergenceKnown, ahead, behind,
                            hasConflict, history));
        setBranchDetailCells(m_branchesTable, row, base, sha, subject, author,
                             branchTimes.value(branch, 0),
                             divergenceKnown ? ahead : -1,
                             divergenceKnown ? behind : -1, history);

        const QString worktreePath = branchWorktrees.value(branch);
        auto *worktree = new QTableWidgetItem(
            worktreePath.isEmpty() ? QString() : QFileInfo(worktreePath).fileName());
        if (!worktreePath.isEmpty()) {
            worktree->setIcon(themedOcticon("worktree", QColor("#58a6ff"), 13));
            worktree->setToolTip(worktreePath);
            worktree->setForeground(Theme::remapColor(QColor("#8b949e")));
        }
        m_branchesTable->setItem(row, kBranchesWorktreeColumn, worktree);

        if (writable && branch != base && behind > 0 && ahead == 0 &&
            worktreePath.isEmpty())
            anyFastForwardable = true;
        auto *actions = new QWidget;
        actions->setObjectName("branchActions");
        auto *actionRow = new QHBoxLayout(actions);
        actionRow->setContentsMargins(2, 0, 2, 0);
        actionRow->setSpacing(0);

        auto *del = new QPushButton;
        del->setObjectName("issueIconButton");
        del->setFlat(true);
        del->setCursor(Qt::PointingHandCursor);
        del->setIcon(themedOcticon("trash", QColor("#f85149"), 16));
        del->setIconSize(QSize(16, 16));
        del->setToolTip(QStringLiteral("Delete branch %1").arg(branch));
        const bool canDelete = writable && branch != base && branch != selected;
        del->setEnabled(canDelete);
        if (canDelete && behind == 0 && ahead == 0 && !branchNeverCommitted(history))
            anyMerged = true;
        if (!canDelete)
            del->setToolTip(writable
                                ? "Can't delete the default or current branch"
                                : "Read-only mirror — no working tree to delete from");
        connect(del, &QPushButton::clicked, this,
                [this, branch] { deleteBranch(branch); });
        actionRow->addWidget(del);

        m_branchesTable->setCellWidget(row, kBranchesDeleteColumn, actions);
        // Every row's cell is the same one flat icon button, so its polished
        // size is measured once (two forced style resolutions and two layout
        // passes per row used to be the bulk of this rebuild) and reused.
        if (actionButtonMinWidth <= 0) {
            actions->ensurePolished();
            del->ensurePolished();
            actionButtonMinWidth = del->sizeHint().width();
            del->setMinimumWidth(actionButtonMinWidth);
            actionRow->invalidate();
            actionWidth = qMax(actionWidth, actions->sizeHint().width());
        } else {
            del->setMinimumWidth(actionButtonMinWidth);
        }
    }
    if (actionWidth > 0)
        m_branchesTable->horizontalHeader()->resizeSection(
            kBranchesDeleteColumn, actionWidth + 4);

    renderRemoteBranchRows(data, nextRow, actionWidth, actionButtonMinWidth);
    m_branchesTable->horizontalHeader()->resizeSection(
        kBranchesDeleteColumn, qMax(actionWidth + 4, 30));

    if (m_branchPullAllButton) {
        m_branchPullAllButton->setText(
            base.isEmpty() ? QStringLiteral("Fast-forward idle")
                           : QStringLiteral("Fast-forward idle to %1").arg(base));
        m_branchPullAllButton->setEnabled(writable && anyFastForwardable);
        m_branchPullAllButton->setToolTip(
            !writable
                ? QString::fromUtf8("Read-only mirror \xE2\x80\x94 nothing to update")
                : anyFastForwardable
                      ? QStringLiteral("Fast-forward inactive branches with no unique "
                                       "commits to %1")
                            .arg(base)
                      : QStringLiteral("No inactive branch can fast-forward to %1")
                            .arg(base));
    }

    for (const QString &remoteBranch : remoteBranches) {
        const auto ab = data.remoteAheadBehind.constFind(remoteBranch);
        if (ab != data.remoteAheadBehind.constEnd() && ab->first == 0 &&
            remoteBranch.section('/', 1) != base) {
            anyMerged = true;
            break;
        }
    }
    if (m_branchDeleteMergedButton) {
        m_branchDeleteMergedButton->setEnabled(writable && anyMerged);
        m_branchDeleteMergedButton->setToolTip(
            !writable
                ? QString::fromUtf8("Read-only mirror \xE2\x80\x94 nothing to delete")
                : anyMerged
                      ? QStringLiteral("Delete every local and remote branch "
                                       "fully merged into %1 (0 ahead)")
                            .arg(base)
                      : QStringLiteral("No branches are fully merged into %1")
                            .arg(base));
    }
    if (m_branchDeleteSelectedButton) {
        const bool selected = m_branchesTable->selectionModel()
                              && m_branchesTable->selectionModel()->hasSelection();
        m_branchDeleteSelectedButton->setEnabled(writable && selected);
    }

    startBranchConflictProbes(dir, base, conflictProbes);

    const QString pendingSelect = m_branchesPanelPendingSelect;
    m_branchesPanelPendingSelect.clear();

    if (m_branchesTable->rowCount() == 0) {
        m_branchesTable->insertRow(0);
        auto *empty = new QTableWidgetItem("No branches in this repository.");
        empty->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_branchesTable->setItem(0, kBranchesNameColumn, empty);
        showBranchDiff(QString());
        return;
    }

    if (!m_branchMergedFlashBranch.isEmpty() && m_branchMergedFlashRow >= 0 &&
        pendingSelect.isEmpty() && m_branchMergedFlashDir == dir &&
        !branches.contains(m_branchMergedFlashBranch)) {
        const int row = qMin(m_branchMergedFlashRow, m_branchesTable->rowCount());
        m_branchesTable->insertRow(row);
        auto *cell = new QWidget;
        cell->setObjectName("branchMergedFlash");
        cell->setStyleSheet(Theme::remapSheet("#branchMergedFlash { background: transparent; }"));
        auto *cellRow = new QHBoxLayout(cell);
        cellRow->setContentsMargins(4, 0, 8, 0);
        cellRow->setSpacing(8);
        cellRow->addWidget(new DoneCheckMark(cell));
        auto *label = new QLabel(
            QStringLiteral("Merged & deleted %1").arg(m_branchMergedFlashBranch));
        label->setStyleSheet(Theme::remapSheet("color: #8b949e; background: transparent;"));
        cellRow->addWidget(label);
        cellRow->addStretch();
        m_branchesTable->setItem(row, kBranchesNameColumn, new QTableWidgetItem);
        m_branchesTable->setCellWidget(row, kBranchesNameColumn, cell);
        for (int c = 0; c < m_branchesTable->columnCount(); ++c) {
            if (c == kBranchesNameColumn)
                continue;
            m_branchesTable->setItem(row, c, new QTableWidgetItem);
        }
        m_branchesTable->setCurrentCell(row, kBranchesNameColumn);
        showBranchDiff(QString());
        if (m_branchDiffView) {
            QString page = m_branchMergedFlashHtml;
            if (page.isEmpty()) {
                // Without the celebration this pane was one grey line. It stands
                // for a branch that just landed, so it carries the same stats the
                // empty-range pane does: the day's landings, per agent.
                const QString flashed = m_branchMergedFlashBranch;
                const int paneGen = m_branchDiffPaneGen;
                const QString message =
                    QStringLiteral("Merged %1 into %2 and deleted it. Pick a "
                                   "branch to see its changes.")
                        .arg(flashed, base);
                // The stats read git on a worker: this paints the plain
                // message now and the page when the read lands, unless the pane
                // has been torn down or moved on to another branch by then.
                page = branchNoChangesPaneHtml(
                    flashed, base, dir, message, /*remember=*/true,
                    [this, flashed, paneGen](QString ready) {
                        if (!m_branchDiffView ||
                            m_branchMergedFlashBranch != flashed ||
                            m_branchDiffPaneGen != paneGen)
                            return;
                        setBranchDiffHtml(ready, /*endCap=*/false);
                    });
                if (page.isEmpty())
                    page = QStringLiteral("<p style='color:#8b949e'>%1</p>")
                               .arg(message.toHtmlEscaped());
            }
            setBranchDiffHtml(page, /*endCap=*/false);
        }
        return;
    }

    QString target = previouslyViewed;
    if (!pendingSelect.isEmpty() &&
        (branches.contains(pendingSelect) || remoteBranches.contains(pendingSelect)))
        target = pendingSelect;
    if (target.isEmpty() ||
        (!branches.contains(target) && !remoteBranches.contains(target)))
        target = selected;
    for (int r = 0; r < m_branchesTable->rowCount(); ++r) {
        QTableWidgetItem *it =
            m_branchesTable->item(r, kBranchesNameColumn);
        if (it && it->text() == target) {
            m_branchesTable->setCurrentCell(r, kBranchesNameColumn);
            break;
        }
    }
    // The refs may have moved since the pane last read this branch, so it is
    // read again, unless a read that started after this load was asked for
    // has already been put in hand. A click starts both at once; re-reading
    // then would orphan the first read and make the new branch wait for two.
    const bool readSinceAsked = m_branchScopeDiffStartGen == m_branchScopeDiffGen &&
                                m_branchScopeDiffStartSeq >= data.diffReadSeq;
    if (!target.isEmpty() && target == m_branchDiffBranch && !readSinceAsked)
        showBranchDiff(target, m_branchDiffAgentSessionId);
}

// Appends one row per remote-tracking branch below the local rows, sharing
// the delete-column width measurement with the local loop.
void MainWindow::renderRemoteBranchRows(const BranchesPanelData &data, int &nextRow,
                                        int &actionWidth, int &actionButtonMinWidth)
{
    const QString &base = data.base;
    const QStringList &remoteBranches = data.remoteBranches;
    const QHash<QString, qint64> &branchTimes = data.times;
    const QHash<QString, QString> &branchShortShas = data.shortShas;
    const QHash<QString, QString> &branchSubjects = data.subjects;
    const QHash<QString, QString> &branchAuthors = data.authors;
    const QHash<QString, QPair<int, int>> &remoteAheadBehind = data.remoteAheadBehind;
    for (const QString &branch : remoteBranches) {
        const int row = nextRow++;
        const QString sha = branchShortShas.value(branch);
        const QString subject = branchSubjects.value(branch);
        const QString author = branchAuthors.value(branch);

        auto *name = new QTableWidgetItem(branch);
        name->setForeground(Theme::remapColor(QColor("#8b949e")));
        {
            QString tip = QStringLiteral("Remote-tracking branch %1").arg(branch);
            if (!sha.isEmpty())
                tip += QStringLiteral("\n%1").arg(sha);
            if (!subject.isEmpty())
                tip += QString::fromUtf8(" \xC2\xB7 %1").arg(subject);
            if (!author.isEmpty())
                tip += QString::fromUtf8(" \xC2\xB7 by %1").arg(author);
            name->setToolTip(tip);
        }
        m_branchesTable->setItem(row, kBranchesNameColumn, name);
        m_branchesTable->setItem(row, kBranchesLinksColumn,
                                 branchSortItem(QString(), QStringLiteral("~")));
        m_branchesTable->setItem(row, kBranchesChangesColumn,
                                 branchSortItem(QString(), -1));

        int ahead = -1;
        int behind = -1;
        const auto abIt = remoteAheadBehind.constFind(branch);
        if (abIt != remoteAheadBehind.constEnd()) {
            ahead = abIt->first;
            behind = abIt->second;
            name->setData(kBranchBehindRole, behind);
        }
        auto *statusItem = new QTableWidgetItem(QStringLiteral("remote"));
        statusItem->setForeground(Theme::remapColor(QColor("#8b949e")));
        statusItem->setToolTip(
            ahead < 0 ? QStringLiteral("Remote-tracking branch")
            : ahead == 0 && behind == 0
                ? QStringLiteral("Remote-tracking branch, up to date with %1").arg(base)
                : QStringLiteral("Remote-tracking branch, %1 commit(s) behind and "
                                 "%2 ahead of %3")
                      .arg(behind)
                      .arg(ahead)
                      .arg(base));
        m_branchesTable->setItem(row, kBranchesStatusColumn, statusItem);
        setBranchDetailCells(m_branchesTable, row, base, sha, subject, author,
                             branchTimes.value(branch, 0), ahead, behind,
                             BranchRefHistory());
        m_branchesTable->setItem(row, kBranchesWorktreeColumn,
                                 new QTableWidgetItem);

        const QString remoteName = branch.section('/', 0, 0);
        const QString remoteRef = branch.section('/', 1);
        const bool canDeleteRemote =
            !remoteName.isEmpty() && !remoteRef.isEmpty() && remoteRef != base;
        auto *ractions = new QWidget;
        ractions->setObjectName("branchActions");
        auto *ractionRow = new QHBoxLayout(ractions);
        ractionRow->setContentsMargins(2, 0, 2, 0);
        ractionRow->setSpacing(0);
        auto *rdel = new QPushButton;
        rdel->setObjectName("issueIconButton");
        rdel->setFlat(true);
        rdel->setCursor(Qt::PointingHandCursor);
        rdel->setIcon(themedOcticon("trash", QColor("#f85149"), 16));
        rdel->setIconSize(QSize(16, 16));
        rdel->setEnabled(canDeleteRemote);
        rdel->setToolTip(canDeleteRemote
                             ? QStringLiteral("Delete branch %1 on %2")
                                   .arg(remoteRef, remoteName)
                             : QStringLiteral("Can't delete the remote's default "
                                              "branch"));
        connect(rdel, &QPushButton::clicked, this,
                [this, branch] { deleteRemoteBranch(branch); });
        ractionRow->addWidget(rdel);
        m_branchesTable->setCellWidget(row, kBranchesDeleteColumn, ractions);
        if (actionButtonMinWidth <= 0) {
            ractions->ensurePolished();
            rdel->ensurePolished();
            actionButtonMinWidth = rdel->sizeHint().width();
            rdel->setMinimumWidth(actionButtonMinWidth);
            ractionRow->invalidate();
            actionWidth = qMax(actionWidth, ractions->sizeHint().width());
        } else {
            rdel->setMinimumWidth(actionButtonMinWidth);
        }
    }
}

void MainWindow::promptNewBranch()
{
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice("This is a read-only mirror; branches can't be created here.",
                            true, forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString dir = repoGitDir();
    const QStringList branches = repoBranches();
    const QString base = m_repoBranch.isEmpty() ? repoDefaultBranch(branches)
                                                : m_repoBranch;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, "New branch",
                              QStringLiteral("Create a branch from %1:").arg(base),
                              QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    runOffThread<QPair<bool, QString>>(
        [dir, name, base] {
            QString err;
            const bool created =
                runGitCapture(dir, {"branch", name, base}, nullptr, &err);
            return qMakePair(created, err);
        },
        [this, dir, name, base](QPair<bool, QString> result) {
            const bool here = repoGitDir() == dir;
            if (!result.first) {
                noticeForRepo(here,
                              result.second.isEmpty()
                                  ? QStringLiteral("Could not create the branch.")
                                  : result.second,
                              true);
                return;
            }
            logSystem(QStringLiteral("Git: created branch %1 from %2.").arg(name, base));
            noticeForRepo(here, QStringLiteral("Created branch %1.").arg(name));
            if (!here)
                return;
            loadBranchesAndTags();
            setRepoBranch(name);
        });
}

void MainWindow::deleteBranch(const QString &branch)
{
    if (branch.isEmpty() || !repoHasWorkingTree())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = repoGitDir();
    const QString worktreePath =
        dir == repo.localPath ? worktreePathForBranch(dir, branch, WorktreeLookup::Any)
                              : QString();
    const QString question =
        worktreePath.isEmpty()
            ? QStringLiteral("Delete branch \"%1\"? This cannot be undone.").arg(branch)
            : QStringLiteral("Delete branch \"%1\" and remove its worktree at\n%2?\n\n"
                             "Uncommitted changes there will be lost. This cannot be "
                             "undone.")
                  .arg(branch, worktreePath);
    if (QMessageBox::question(this, "Delete branch", question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString nextSelection = neighbourBranchInList(branch);

    if (!worktreePath.isEmpty()) {
        removeWorktree(worktreePath, branch, /*confirm=*/false,
                       /*alsoDeleteBranch=*/true, [this, dir, branch, nextSelection] {
                           if (repoGitDir() != dir)
                               return;
                           m_branchesCache.clear();
                           if (!repoHasLocalBranch(dir, branch))
                               m_branchDiffBranch = nextSelection;
                           loadBranchesAndTags();
                       });
        return;
    }

    runOffThread<QPair<bool, QString>>(
        [dir, branch] {
            QString err;
            const bool deleted = deleteBranchAndCopies(dir, branch, &err);
            return qMakePair(deleted, err);
        },
        [this, repo, dir, branch, nextSelection](QPair<bool, QString> result) {
            const bool here = repoGitDir() == dir;
            if (!result.first) {
                noticeForRepo(here,
                              result.second.isEmpty()
                                  ? QStringLiteral("Could not delete the branch.")
                                  : result.second,
                              true);
                return;
            }
            logSystem(QStringLiteral("Git: deleted branch %1.").arg(branch));
            noticeForRepo(here, QStringLiteral("Deleted branch %1.").arg(branch));
            propagateBranchDeletion(repo.owner, repo.name);
            if (!here)
                return;
            m_branchesCache.clear(); // branch removed — bust the cache
            m_branchDiffBranch = nextSelection;
            loadBranchesAndTags();
        });
}

void MainWindow::deleteReviewedBranchAndCleanUp(const QString &branch)
{
    if (branch.isEmpty() || !repoHasWorkingTree() || m_worktreeMergeInProgress ||
        m_branchMergeFlowBusy || m_branchCleanupInProgress.contains(branch))
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const QString repoPath = m_repositories.at(m_repoDetailIndex).localPath;
    const QString dir = repoGitDir();
    if (repoPath.isEmpty() || branch == repoDefaultBranch(repoBranches()))
        return;
    const QString nextSelection = neighbourBranchInList(branch);
    const QString worktreePath =
        worktreePathForBranch(repoPath, branch, WorktreeLookup::Any);
    if (repoGitDir() != dir || m_branchDiffBranch != branch)
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    WorktreeMergeFlow flow;
    flow.dir = dir;
    flow.branch = branch;
    flow.owner = repo.owner;
    flow.name = repo.name;
    flow.localPath = repo.localPath;
    flow.mirrorPath = repo.mirrorPath;
    flow.identity = &m_profileIdentity;
    flow.userName = m_userName;
    QList<int> branchPulls;
    if (m_branchDiffPullNumber > 0)
        branchPulls.append(m_branchDiffPullNumber);
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner == repo.owner && s.name == repo.name &&
            s.branchName == branch && s.prNumber > 0 &&
            !branchPulls.contains(s.prNumber))
            branchPulls.append(s.prNumber);
    }
    const bool hadBranch = repoHasLocalBranch(dir, branch);
    m_branchCleanupInProgress.insert(branch);
    if (m_branchDeleteCleanupButton)
        m_branchDeleteCleanupButton->setEnabled(false);
    if (m_branchMergeDeleteButton)
        m_branchMergeDeleteButton->setEnabled(false);
    if (m_branchMergeButton)
        m_branchMergeButton->setEnabled(false);
    deleteWorktreeBranchAndAgent(
        worktreePath, branch, /*confirm=*/true, /*deferRefresh=*/false,
        [this, flow, branchPulls, hadBranch, nextSelection] {
            const QString dir = flow.dir;
            const QString branch = flow.branch;
            const auto refresh = [this, dir, branch, nextSelection] {
                m_branchCleanupInProgress.remove(branch);
                if (repoGitDir() != dir)
                    return;
                m_branchesCache.clear();
                if (!repoHasLocalBranch(dir, branch)) {
                    forgetBranchDiff(branch);
                    if (m_branchDiffBranch == branch)
                        m_branchDiffBranch = nextSelection;
                    if (m_worktreeSelectedBranch == branch)
                        updateWorktreeSelection(QString(), QString());
                } else if (m_branchDiffBranch == branch) {
                    updateBranchDetailActions(branch);
                }
                loadBranchesAndTags();
                updateAgentFilesTabState(m_selectedAgentSessionId);
            };
            if (!hadBranch || repoHasLocalBranch(dir, branch)) {
                refresh();
                return;
            }
            deletePullsOfMergedBranch(
                flow, branchPulls,
                [this, dir, branch, refresh](const QList<int> &,
                                             const QString &pullError) {
                    if (!pullError.isEmpty() && repoGitDir() == dir)
                        setRepoDetailNotice(
                            QStringLiteral("Deleted %1, but could not delete "
                                           "its pull request: %2")
                                .arg(branch, pullError),
                            true);
                    refresh();
                });
        });
}

void MainWindow::applyWebBranchOps(const RepositoryRecord &repo,
                                   const QJsonArray &ops)
{
    if (ops.isEmpty())
        return;
    // Only the node that owns the repository applies these. A mirror's
    // checkout is a copy: moving refs there would be overwritten by the next
    // converge and would never reach anybody else.
    if (!isSourceOfTruthRepo(repo))
        return;
    const QString path = repo.localPath.trimmed();
    if (path.isEmpty() || !QDir(path).exists())
        return;
    // The branch the checkout has checked out is the repository's default
    // branch for every checkout this app creates. WebBranchOps refuses to
    // touch it either way, so this is a second guard rather than the only one.
    const QString base = headBranchFromFile(path);
    const QString owner = repo.owner;
    const QString name = repo.name;
    QList<forkmesh::branchops::Outcome> blocked;
    QJsonArray runnable;
    for (const QJsonValue &value : ops) {
        const QJsonObject op = value.toObject();
        const QString branch = op.value(QStringLiteral("branch")).toString().trimmed();
        if (op.value(QStringLiteral("op")).toString() == QLatin1String("merge")
            && webMergeBlockedByAgent(owner, name, branch)) {
            forkmesh::branchops::Outcome outcome;
            outcome.op = QStringLiteral("merge");
            outcome.branch = branch;
            outcome.base = op.value(QStringLiteral("base")).toString().trimmed();
            outcome.requestId =
                static_cast<qint64>(op.value(QStringLiteral("id")).toDouble());
            outcome.message =
                QStringLiteral("refused: an agent is still working on %1")
                    .arg(branch);
            blocked.append(outcome);
            continue;
        }
        runnable.append(value);
    }
    // Off the GUI thread: one bulk request from the website's Branches page
    // ("update every branch behind main") is dozens of git invocations, and
    // this runs inside the periodic relay sync. WebBranchOps touches nothing
    // but the repository on disk, so it is safe to run in a worker; every
    // effect of it lands back here.
    auto *watcher = new QFutureWatcher<QList<forkmesh::branchops::Outcome>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, owner, name, path, base, blocked] {
                watcher->deleteLater();
                QList<forkmesh::branchops::Outcome> outcomes = blocked;
                outcomes.append(watcher->result());
                recordWebBranchMerges(owner, name, path, outcomes);
                finishWebBranchOps(owner, name, path, base, outcomes);
            });
    watcher->setFuture(QtConcurrent::run([path, base, runnable] {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("branches"),
            QStringLiteral("Applying branch changes requested on the website"));
        return forkmesh::branchops::applyBranchOps(path, base, runnable);
    }));
}

bool MainWindow::webMergeBlockedByAgent(const QString &owner,
                                        const QString &name,
                                        const QString &branch) const
{
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.branchName != branch || session.merged
            || session.owner != owner || session.name != name)
            continue;
        if (session.status != AgentStatus::Running
            && session.status != AgentStatus::Waiting
            && session.status != AgentStatus::Queued)
            continue;
        if (agentSessionWorkInFlight(session.id))
            return true;
    }
    return false;
}

void MainWindow::recordWebBranchMerges(
    const QString &owner, const QString &name, const QString &path,
    const QList<forkmesh::branchops::Outcome> &outcomes)
{
    QStringList mergedBranches;
    for (const forkmesh::branchops::Outcome &outcome : outcomes) {
        if (outcome.op == QLatin1String("merge")
            && (outcome.applied || outcome.alreadyDone))
            mergedBranches.append(outcome.branch);
    }
    if (mergedBranches.isEmpty())
        return;
    const int index = repoIndexFor(owner, name);
    if (index < 0)
        return;
    danceGenie();
    const RepositoryRecord repo = m_repositories.at(index);
    PullStore pulls(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                    m_userName);
    QString reconcileError;
    pulls.reconcileMergedPulls(&reconcileError);
    if (!reconcileError.isEmpty())
        logSystem(QStringLiteral("PR: web merge in %1/%2 could not record its "
                                 "PR status yet: %3")
                      .arg(owner, name, reconcileError));
    const RepositoryRecord &writable = writableRecordFor(repo);
    for (const forkmesh::branchops::Outcome &outcome : outcomes) {
        if (outcome.op != QLatin1String("merge") || !outcome.applied)
            continue;
        rememberMergedBranch(outcome.mergeOid, outcome.branch, m_agentSessions, owner,
                             name, topBarUserName());
    }
    for (const QString &branch : std::as_const(mergedBranches)) {
        QList<int> attachedIssues;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.owner == owner && session.name == name
                && session.branchName == branch && session.issueNumber > 0
                && !attachedIssues.contains(session.issueNumber))
                attachedIssues.append(session.issueNumber);
        }
        closeIssuesForMerge(
            IssueStore(writable.localPath, writable.mirrorPath,
                       &m_profileIdentity, m_userName),
            attachedIssues,
            QStringLiteral("Closed by merged branch \"%1\".").arg(branch),
            QStringLiteral("merged branch \"%1\"").arg(branch));
        markRepoAgentSessionsMerged(owner, name, 0, branch, true);
    }
}

void MainWindow::finishWebBranchOps(
    const QString &owner, const QString &name, const QString &path,
    const QString &base, const QList<forkmesh::branchops::Outcome> &outcomes)
{
    QHash<int, QPair<QList<int>, QString>> held;
    bool cleanup = false;
    for (int i = 0; i < outcomes.size(); ++i) {
        const forkmesh::branchops::Outcome &outcome = outcomes.at(i);
        if (!outcome.cleanupRequested)
            continue;
        cleanup = true;
        if (!outcome.worktreePath.isEmpty())
            held.insert(i, {releaseTrackedWorktree(outcome.worktreePath),
                            outcome.worktreePath});
    }
    const auto report = [this, owner, name](
                            const QList<forkmesh::branchops::Outcome> &done) {
        int changed = 0;
        QJsonArray results;
        for (const forkmesh::branchops::Outcome &outcome : done) {
            if (outcome.applied || outcome.branchDeleted)
                ++changed;
            // Every outcome is logged, including the refusals: the
            // relay hands its queue over exactly once, so this log and the
            // report below are how an owner finds out why a request they
            // made did not happen.
            logSystem(QStringLiteral("Branches (web request for %1/%2): %3")
                          .arg(owner, name, outcome.message));
            results.append(forkmesh::branchops::resultJson(outcome));
        }
        reportWebBranchOpResults(owner, name, results);
        if (changed == 0)
            return;
        m_branchConflictCache.clear();
        m_branchesCache.clear();
        if (m_repoDetailIndex >= 0 &&
            m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord shown = m_repositories.at(m_repoDetailIndex);
            if (shown.owner == owner && shown.name == name)
                loadBranchesAndTags();
        }
        // A ref change no commit hook can see (the same blind spot
        // propagateBranchDeletion exists for): republish so the
        // mirrors — and so the branch listing the website reads back —
        // stop advertising a branch that is gone and start
        // advertising one that is new.
        const int index = repoIndexFor(owner, name);
        if (index >= 0) {
            logSystem(QStringLiteral(
                          "Mirror: propagating web branch changes for %1/%2.")
                          .arg(owner, name));
            propagateRepoUpdate(index);
        }
    };
    if (!cleanup) {
        report(outcomes);
        return;
    }
    auto *watcher =
        new QFutureWatcher<QList<forkmesh::branchops::Outcome>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, held, report] {
                watcher->deleteLater();
                const QList<forkmesh::branchops::Outcome> done =
                    watcher->result();
                for (auto it = held.cbegin(); it != held.cend(); ++it) {
                    if (it.key() < done.size() && !done.at(it.key()).branchDeleted)
                        retrackWorktree(it.value().first, it.value().second);
                }
                report(done);
            });
    watcher->setFuture(QtConcurrent::run([path, base, outcomes] {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("branches"),
            QStringLiteral("Cleaning up branches merged from the website"));
        QList<forkmesh::branchops::Outcome> done;
        for (const forkmesh::branchops::Outcome &outcome : outcomes)
            done.append(
                forkmesh::branchops::cleanUpMergedBranch(path, base, outcome));
        return done;
    }));
}

void MainWindow::reportWebBranchOpResults(const QString &owner,
                                          const QString &name,
                                          const QJsonArray &results)
{
    if (results.isEmpty() || !m_networkAccess || !hasOwnerSigningCapability())
        return;
    const QUrlQuery query = signedInboxQuery(owner);
    if (query.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(
        QStringLiteral("/api/repo/%1/%2/branches/ops/results").arg(owner, name));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("results"), results}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void MainWindow::drainWebBranchOpsFor(const RepositoryRecord &repo)
{
    if (!m_networkAccess || !isSourceOfTruthRepo(repo) ||
        !PrivateMirrorRuntime::repositoryRelayInboxAllowed(
            repo.publishToNetwork))
        return;
    const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
    const QString name = repoSegment(repo.name, QStringLiteral("repository"));
    if (owner.isEmpty() || name.isEmpty())
        return;
    const QUrlQuery query = signedInboxQuery(owner);
    if (query.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/branches/ops").arg(owner, name));
    const QString backoffKey = url.toString();
    if (!m_pollBackoff.ready(backoffKey, QDateTime::currentMSecsSinceEpoch()))
        return;
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, backoffKey] {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    noteInboxDrainFailure(
                        backoffKey,
                        reply->attribute(
                                 QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt());
                    return;
                }
                noteInboxDrainSuccess(backoffKey);
                applyWebBranchOps(repo, QJsonDocument::fromJson(reply->readAll())
                                            .object()
                                            .value(QStringLiteral("ops"))
                                            .toArray());
            });
}

void MainWindow::deleteRemoteBranch(const QString &branch)
{
    const QString remote = branch.section('/', 0, 0);
    const QString ref = branch.section('/', 1);
    if (remote.isEmpty() || ref.isEmpty())
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    runOffThread<QStringList>(
        [dir] {
            QByteArray configured;
            runGitCapture(dir, {"remote"}, &configured, nullptr);
            return QString::fromUtf8(configured).split('\n', Qt::SkipEmptyParts);
        },
        [this, dir, branch, remote, ref](QStringList configured) {
            if (repoGitDir() != dir)
                return;
            if (!configured.contains(remote)) {
                deleteMeshTrackingBranch(remote, ref);
                return;
            }
            confirmDeleteRemoteBranch(dir, branch, remote, ref);
        });
}

void MainWindow::confirmDeleteRemoteBranch(const QString &dir, const QString &branch,
                                           const QString &remote, const QString &ref)
{
    if (QMessageBox::question(
            this, "Delete remote branch",
            QStringLiteral("Delete branch \"%1\" on the remote \"%2\"? This "
                           "removes it for everyone and cannot be undone.")
                .arg(ref, remote),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString nextSelection = neighbourBranchInList(branch);
    setRepoDetailNotice(
        QString::fromUtf8("Deleting %1 on %2\xE2\x80\xA6").arg(ref, remote));
    runGitDetached(
        dir, {"push", remote, "--delete", ref},
        [this, branch, remote, ref, dir, nextSelection](bool ok,
                                                         const QByteArray &out) {
            if (!ok) {
                const QString detail = QString::fromUtf8(out).trimmed();
                setRepoDetailNotice(
                    detail.isEmpty()
                        ? QStringLiteral("Could not delete %1 on %2.")
                              .arg(ref, remote)
                        : QStringLiteral("Could not delete %1 on %2: %3")
                              .arg(ref, remote, detail.left(200)),
                    true);
                return;
            }
            runGitDetached(
                dir, {"branch", "-dr", branch},
                [this, remote, ref, dir, nextSelection](bool, const QByteArray &) {
                    logSystem(
                        QStringLiteral("Git: deleted branch %1 on remote %2.")
                            .arg(ref, remote));
                    const bool here = repoGitDir() == dir;
                    noticeForRepo(here,
                                  QStringLiteral("Deleted %1 on %2.").arg(ref, remote));
                    if (!here)
                        return;
                    m_branchesCache.clear();
                    m_branchDiffBranch = nextSelection;
                    loadBranchesAndTags();
                });
        },
        forkmesh::logSourceHere(), /*mergeStderr=*/true);
}

void MainWindow::deleteMeshTrackingBranch(const QString &remote,
                                          const QString &ref)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = repoGitDir();
    const QString branch = remote + '/' + ref;
    const bool mesh = remote == QLatin1String("forkmesh-mesh");
    if (mesh && repoHasLocalBranch(dir, ref)) {
        setRepoDetailNotice(
            QStringLiteral("%1 is your mirrors' copy of the local branch %2, "
                           "so it can't be deleted on its own. Delete the "
                           "local branch %2 and the mirrors drop it too.")
                .arg(branch, ref),
            true);
        return;
    }
    const QString question =
        mesh ? QStringLiteral(
                   "\"forkmesh-mesh\" is not a Git remote: it lists the "
                   "branches your ForkMesh mirrors still hold. Delete \"%1\" "
                   "from the mirrors? They drop it on the next full mesh "
                   "sync. Its tip is kept under "
                   "refs/forkmesh/trash/deleted/%1.")
                   .arg(ref)
             : QStringLiteral(
                   "\"%1\" is no longer a configured remote, so there is "
                   "nothing to push to. Remove the leftover branch \"%2\" "
                   "from this checkout?")
                   .arg(remote, branch);
    if (QMessageBox::question(this, "Delete remote branch", question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString nextSelection = neighbourBranchInList(branch);
    runOffThread<QPair<bool, QString>>(
        [dir, branch, ref] {
            QByteArray tip;
            if (runGitCapture(dir,
                              {"rev-parse", "--verify", "--quiet",
                               QStringLiteral("refs/remotes/") + branch},
                              &tip, nullptr))
                runGitCapture(dir,
                              {"update-ref",
                               forkmesh::upstream::deletedBranchTombstoneRef(ref),
                               QString::fromUtf8(tip).trimmed()},
                              nullptr, nullptr);
            QString err;
            const bool removed =
                runGitCapture(dir, {"branch", "-dr", branch}, nullptr, &err);
            return qMakePair(removed, err);
        },
        [this, repo, dir, branch, mesh, nextSelection](QPair<bool, QString> result) {
            const bool here = repoGitDir() == dir;
            if (!result.first) {
                noticeForRepo(here,
                              result.second.isEmpty()
                                  ? QStringLiteral("Could not remove %1.").arg(branch)
                                  : QStringLiteral("Could not remove %1: %2")
                                        .arg(branch, result.second.left(200)),
                              true);
                return;
            }
            logSystem(QStringLiteral("Git: removed tracking branch %1.").arg(branch));
            const int repoIndex = repoIndexFor(repo.owner, repo.name);
            if (mesh && repoIndex >= 0 && isSourceOfTruthRepo(repo)) {
                noticeForRepo(
                    here,
                    QString::fromUtf8("Removed %1. Syncing the mirrors so they drop "
                                      "it\xE2\x80\xA6")
                        .arg(branch));
                convergeSourceRepoFromMesh(repoIndex, /*force=*/true);
            } else {
                noticeForRepo(here, QStringLiteral("Removed %1.").arg(branch));
            }
            if (!here)
                return;
            m_branchesCache.clear();
            m_branchDiffBranch = nextSelection;
            loadBranchesAndTags();
        });
}

QString MainWindow::neighbourBranchInList(const QString &branch) const
{
    const int row = branchRowInList(branch);
    if (row < 0)
        return QString();
    for (int r = row + 1; r < m_branchesTable->rowCount(); ++r) {
        QTableWidgetItem *it =
            m_branchesTable->item(r, kBranchesNameColumn);
        if (it && !it->text().isEmpty())
            return it->text();
    }
    for (int r = row - 1; r >= 0; --r) {
        QTableWidgetItem *it =
            m_branchesTable->item(r, kBranchesNameColumn);
        if (it && !it->text().isEmpty())
            return it->text();
    }
    return QString();
}

int MainWindow::branchRowInList(const QString &branch) const
{
    if (!m_branchesTable || branch.isEmpty())
        return -1;
    for (int r = 0; r < m_branchesTable->rowCount(); ++r) {
        QTableWidgetItem *it =
            m_branchesTable->item(r, kBranchesNameColumn);
        if (it && it->text() == branch)
            return r;
    }
    return -1;
}

void MainWindow::flashMergedBranchRow(const QString &branch)
{
    const int row = branchRowInList(branch);
    if (branch.isEmpty() || row < 0)
        return;
    m_branchMergedFlashBranch = branch;
    m_branchMergedFlashDir = m_branchesPanelDir;
    m_branchMergedFlashRow = row;
    m_branchDiffBranch.clear();
    // The compare header names the branch being reviewed; with the branch
    // gone it retires now rather than whenever the next page change happens
    // to refresh it (showBranchDiffBranchGone() returns early once the
    // selection is cleared here, so nothing else would).
    updateCommitsCompareIndicator();
    QTimer::singleShot(kBranchMergedFlashMs, this, [this, branch] {
        if (m_branchMergedFlashBranch == branch)
            clearMergedBranchFlash();
    });
}

static QString renderMergedBranchCelebration(
    const QString &branch, const QString &base, const QString &dir, int range,
    const MergeCelebrationRead &read, const QList<AgentSession> &sessions,
    const QString &owner, const QString &name, const QString &me,
    const QByteArray &myAvatar, bool lookBack = false)
{
    constexpr int kRowAvatarPx = 22;
    constexpr int kHeroAvatarPx = 56;
    QList<MergeCelebrationRow> rows = mergeCelebrationRows(
        dir, base, read.landings, sessions, owner, name, me, myAvatar, branch,
        kRowAvatarPx, kHeroAvatarPx, &read.liveBranches);

    if (lookBack) {
        MergeCelebrationRow landed;
        for (MergeCelebrationRow &row : rows) {
            if (!row.current)
                continue;
            row.justNow = false;
            landed = row;
        }
        QList<MergeActorTally> tallies =
            mergeCelebrationTallies(rows, /*minMerges=*/1);
        if (!read.churnComplete)
            stripMergeTallyChurn(&tallies);
        int truncated = 0;
        if (rows.size() > kMergeCelebrationRowLimit) {
            truncated = rows.size() - kMergeCelebrationRowLimit;
            rows = rows.mid(0, kMergeCelebrationRowLimit);
        }
        if (landed.mergeCommit.isEmpty())
            return branchNoChangesHtml(
                QStringLiteral("No merge has landed in %1 in this window. Pick "
                               "a longer window to see older merges.")
                    .arg(base),
                landed, base, rows, truncated, tallies, range, read.velocity);
        QVector<int> activity;
        if (const auto *log = forkmesh::activity::UserActivityLog::instance())
            activity = log->today();
        return mergeCelebrationHtml(
            landed, base, rows, read.commits, truncated, tallies, range,
            read.velocity, activity,
            forkmesh::activity::activitySlotFor(QTime::currentTime()),
            /*lookBack=*/true);
    }

    // This renderer is used for the "merged & deleted" flash. The worker can
    // finish its ref read a moment before asynchronous cleanup removes the
    // source ref, but the resulting row must already link to the durable merge
    // commit rather than back to a branch whose deletion is in flight.
    for (MergeCelebrationRow &row : rows)
        if (row.current)
            row.branchStillExists = false;

    MergeCelebrationRow landed;
    const auto current = std::find_if(
        rows.cbegin(), rows.cend(),
        [](const MergeCelebrationRow &row) { return row.current; });
    if (current != rows.cend()) {
        landed = *current;
    } else {
        MergeLanding synthetic;
        synthetic.branch = branch;
        synthetic.whenSecs = QDateTime::currentSecsSinceEpoch();
        const QList<MergeCelebrationRow> heroOnly = mergeCelebrationRows(
            dir, base, {synthetic}, sessions, owner, name, me, myAvatar, branch,
            kRowAvatarPx, kHeroAvatarPx, &read.liveBranches);
        if (!heroOnly.isEmpty())
            landed = heroOnly.first();
    }
    landed.branchStillExists = false;

    QList<MergeActorTally> tallies = mergeCelebrationTallies(rows);
    if (!read.churnComplete)
        stripMergeTallyChurn(&tallies);

    int truncated = 0;
    if (rows.size() > kMergeCelebrationRowLimit) {
        truncated = rows.size() - kMergeCelebrationRowLimit;
        rows = rows.mid(0, kMergeCelebrationRowLimit);
    }
    QVector<int> activity;
    if (const auto *log = forkmesh::activity::UserActivityLog::instance())
        activity = log->today();
    return mergeCelebrationHtml(
        landed, base, rows, read.commits, truncated, tallies, range,
        read.velocity, activity,
        forkmesh::activity::activitySlotFor(QTime::currentTime()));
}

static QString pendingMergeCelebrationHtml(const QString &branch,
                                           const QString &base)
{
    return QStringLiteral(
               "<div style='padding:8px 10px;'><p style='color:#3fb950;"
               "font-size:18px;font-weight:700;margin-bottom:6px;'>"
               "&#10003; Merged!</p><p style='color:#8b949e;'>"
               "<b style='font-family:monospace;'>%1</b> is part of "
               "<b style='font-family:monospace;'>%2</b> now. Cleaning up "
               "the worktree and loading merge details&#8230;</p></div>")
        .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
}

void MainWindow::startMergedBranchCelebration(const QString &branch,
                                              const QString &base,
                                              const QString &dir)
{
    if (branch.isEmpty() || base.isEmpty() || dir.isEmpty() ||
        m_branchMergedFlashBranch != branch)
        return;
    QString owner;
    QString name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    const QList<AgentSession> sessions = m_agentSessions;
    const QString me = topBarUserName();
    const QByteArray myAvatar = effectiveUserAvatar();
    const int range = m_mergeRange;
    const int generation = ++m_mergeCelebrationGen;
    m_mergeRangePane = {true, branch, base, dir, QString()};
    m_branchMergedFlashHtml = pendingMergeCelebrationHtml(branch, base);
    syncUserActivity(true);

    runOffThread<MergeCelebrationRead>(
        [dir, base, branch, range] {
            return readMergeCelebration(dir, base, branch, range);
        },
        [this, generation, branch, base, dir, range, sessions, owner, name, me,
         myAvatar](MergeCelebrationRead read) {
            if (generation != m_mergeCelebrationGen ||
                m_branchMergedFlashBranch != branch ||
                m_branchMergedFlashDir != dir)
                return;
            const QString page = renderMergedBranchCelebration(
                branch, base, dir, range, read, sessions, owner, name, me,
                myAvatar);
            if (page.isEmpty())
                return;
            m_branchMergedFlashHtml = page;

            // Cleanup may already have rebuilt the branch table around its
            // temporary completed row. Replace that row's lightweight page in
            // place, but never steal the diff pane after the user picked
            // another branch while the history read was running.
            if (!m_branchesTable || !m_branchDiffView)
                return;
            const int row = m_branchesTable->currentRow();
            QWidget *cell = row >= 0
                                ? m_branchesTable->cellWidget(
                                      row, kBranchesNameColumn)
                                : nullptr;
            if (cell && cell->objectName() == QLatin1String("branchMergedFlash"))
                setBranchDiffHtml(page, /*endCap=*/false);
        });
}

// adhoc #2104: the same walk-and-diff of every merge in the window that the
// no-changes pane pays for, reached from a range button. It is read on a worker
// and `onReady` paints the finished page.
void MainWindow::requestMergedBranchCelebrationHtml(
    const QString &branch, const QString &base, const QString &dir,
    std::function<void(QString)> onReady)
{
    if (!onReady || dir.isEmpty() || base.isEmpty())
        return;
    QString owner;
    QString name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    const int range = m_mergeRange;
    m_mergeRangePane = {true, branch, base, dir, QString()};
    const QList<AgentSession> sessions = m_agentSessions;
    const QString me = topBarUserName();
    const QByteArray myAvatar = effectiveUserAvatar();
    const int generation = ++m_mergeCelebrationGen;
    runOffThread<MergeCelebrationRead>(
        [dir, base, branch, range] {
            return readMergeCelebration(dir, base, branch, range);
        },
        [this, generation, branch, base, dir, range, sessions, owner, name, me,
         myAvatar, onReady](MergeCelebrationRead read) {
            if (generation != m_mergeCelebrationGen)
                return; // another celebration owns the pane now
            onReady(renderMergedBranchCelebration(branch, base, dir, range, read,
                                                  sessions, owner, name, me,
                                                  myAvatar));
        });
}

void MainWindow::showGitMergedCover(const QString &branch, const QString &base)
{
    if (!m_gitWorkspaceStack || !m_gitMergedPage || !m_gitMergedLabel)
        return;
    m_gitMergedLabel->setText(
        QStringLiteral("Merged <b>%1</b> into <b>%2</b>.")
            .arg(branch.toHtmlEscaped(), base.toHtmlEscaped()));
    m_gitMergedBranch = branch;
    m_gitMergedBase = base;
    m_gitMergedDir = repoGitDir();
    m_gitMergedSessions = m_agentSessions;
    m_gitMergedLookBack = false;
    m_gitWorkspaceStack->setCurrentWidget(m_gitMergedPage);
    if (m_gitMergedStats)
        setDiffHtml(m_gitMergedStats,
                    QString::fromUtf8("<p style='color:#8b949e'>Loading merge "
                                      "stats\xE2\x80\xA6</p>"),
                    /*endCap=*/false);
    loadGitMergedStats();
}

void MainWindow::showGitLeaderboard()
{
    if (!m_gitWorkspaceStack || !m_gitMergedPage || !m_gitMergedLabel)
        return;
    const QString base = repoDefaultBranchFast();
    const QString dir = repoGitDir();
    if (base.isEmpty() || dir.isEmpty()) {
        flashMessage(QStringLiteral("Open a repository with a local checkout to see "
                                 "its merge leaderboard."),
                  true);
        return;
    }
    m_gitMergedLabel->setText(
        QStringLiteral("Merge leaderboard for <b>%1</b>.").arg(base.toHtmlEscaped()));
    m_gitMergedBranch.clear();
    m_gitMergedBase = base;
    m_gitMergedDir = dir;
    m_gitMergedSessions = m_agentSessions;
    m_gitMergedLookBack = true;
    m_gitWorkspaceStack->setCurrentWidget(m_gitMergedPage);
    if (m_gitMergedStats)
        setDiffHtml(m_gitMergedStats,
                    QString::fromUtf8("<p style='color:#8b949e'>Loading merge "
                                      "stats\xE2\x80\xA6</p>"),
                    /*endCap=*/false);
    loadGitMergedStats();
}

void MainWindow::loadGitMergedStats()
{
    if (!m_gitMergedStats || m_gitMergedDir.isEmpty() || m_gitMergedBase.isEmpty())
        return;
    const QString branch = m_gitMergedBranch;
    const QString base = m_gitMergedBase;
    const QString dir = m_gitMergedDir;
    QString owner;
    QString name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    const QList<AgentSession> sessions = m_gitMergedSessions;
    const QString me = topBarUserName();
    const QByteArray myAvatar = effectiveUserAvatar();
    const int range = m_mergeRange;
    const bool lookBack = m_gitMergedLookBack;
    const int generation = ++m_gitMergedStatsGen;
    syncUserActivity(true);
    runOffThread<MergeCelebrationRead>(
        [dir, base, branch, range, lookBack] {
            return readMergeCelebration(dir, base, branch, range, lookBack);
        },
        [this, generation, branch, base, dir, range, sessions, owner, name, me,
         myAvatar, lookBack](MergeCelebrationRead read) {
            if (generation != m_gitMergedStatsGen || !m_gitMergedStats ||
                m_gitMergedBranch != branch || m_gitMergedDir != dir)
                return;
            const QString page = renderMergedBranchCelebration(
                lookBack ? read.celebrated : branch, base, dir, range, read,
                sessions, owner, name, me, myAvatar, lookBack);
            if (page.isEmpty())
                return;
            const int scroll = m_gitMergedStats->verticalScrollBar()->value();
            setDiffHtml(m_gitMergedStats, page, /*endCap=*/false);
            m_gitMergedStats->verticalScrollBar()->setValue(scroll);
        });
}

// The render half of the "no changes" pane: rows, portraits and HTML, built on
// the GUI thread from a snapshot its worker read.
QString MainWindow::renderBranchNoChangesPane(const QString &branch,
                                              const QString &base,
                                              const QString &dir,
                                              const QString &emptyMessage,
                                              int range,
                                              const MergeCelebrationRead &read)
{
    constexpr int kRowAvatarPx = 22;
    // The certificate paints this branch's portrait at 56px; building it at
    // row size stretched a 22px bitmap into a blur.
    constexpr int kHeroAvatarPx = 56;
    QString owner;
    QString name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    QList<MergeCelebrationRow> rows = mergeCelebrationRows(
        dir, base, read.landings, m_agentSessions, owner, name,
        topBarUserName(), effectiveUserAvatar(), branch, kRowAvatarPx,
        kHeroAvatarPx, &read.liveBranches);

    MergeCelebrationRow landed;
    for (MergeCelebrationRow &row : rows) {
        if (!row.current)
            continue;
        row.justNow = false; // this landing is being looked back at, not made
        landed = row;
    }

    // Everyone who merged in the window, one landing included: this pane is the
    // window's stats, not a footnote under a list that names every landing.
    QList<MergeActorTally> tallies = mergeCelebrationTallies(rows, /*minMerges=*/1);
    if (!read.churnComplete)
        stripMergeTallyChurn(&tallies);

    int truncated = 0;
    if (rows.size() > kMergeCelebrationRowLimit) {
        truncated = rows.size() - kMergeCelebrationRowLimit;
        rows = rows.mid(0, kMergeCelebrationRowLimit);
    }
    return branchNoChangesHtml(emptyMessage, landed, base, rows, truncated,
                               tallies, range, read.velocity);
}

// adhoc #1651: a pane with no changes — commonly an agent branch whose work is
// already in the base, and also the clean working-tree page — used to be a
// single grey line. The same day the celebration shows belongs here too: who
// merged what today, and this branch's own landing when one is being inspected.
//
// adhoc #2104: this pane used to walk and diff every merge in the window on the
// GUI thread. One of those walks blocked it for 13.3 s and sat under seven of
// the stalls reported on 2026-09-11, because `git log --shortstat` over a wide
// window is slow and the pane is rebuilt on every switch between the branch and
// clean-tree pages. Now it only ever *serves* a page: a fresh memo, the
// previous one while a refresh runs, or nothing at all the first time (callers
// paint their plain message for that). The reads run on a worker and `onReady`
// paints the finished page into whatever view the caller owns.
QString MainWindow::branchNoChangesPaneHtml(const QString &branch,
                                            const QString &base,
                                            const QString &dir,
                                            const QString &emptyMessage,
                                            bool remember,
                                            std::function<void(QString)> onReady)
{
    if (dir.isEmpty() || base.isEmpty())
        return QString();
    const int range = m_mergeRange;
    // The branch pane's range buttons repaint whatever page they were clicked
    // on; the clean-tree pane repaints itself, so it leaves this alone.
    if (remember)
        m_mergeRangePane = {false, branch, base, dir, emptyMessage};
    const QString key = dir + QLatin1Char('\n') + base + QLatin1Char('\n') + branch +
                        QLatin1Char('\n') + emptyMessage + QLatin1Char('\n') +
                        QString::number(range);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cached = m_branchNoChangesPanes.constFind(key);
    const bool known = cached != m_branchNoChangesPanes.cend();
    // An entry inside its window is served as it stands, empty included: a
    // window that produced no page must not put a worker on every repaint.
    if (known && now - cached->atMs < kBranchNoChangesStatsMs)
        return cached->html;
    const QString stale = known ? cached->html : QString();

    // The newest caller owns the repaint: an older pane's continuation would
    // paint yesterday's page into a view that has moved on.
    if (onReady)
        m_branchNoChangesReady.insert(key, std::move(onReady));
    else
        m_branchNoChangesReady.remove(key);
    if (m_branchNoChangesLoading.contains(key))
        return stale; // a worker is already reading this exact pane
    m_branchNoChangesLoading.insert(key);

    runOffThread<MergeCelebrationRead>(
        [dir, base, branch, range] {
            return readMergeCelebration(dir, base, branch, range);
        },
        [this, key, branch, base, dir, emptyMessage, range](MergeCelebrationRead read) {
            m_branchNoChangesLoading.remove(key);
            const QString html = renderBranchNoChangesPane(
                branch, base, dir, emptyMessage, range, read);
            if (m_branchNoChangesPanes.size() > 16)
                m_branchNoChangesPanes.clear();
            m_branchNoChangesPanes.insert(
                key, {html, QDateTime::currentMSecsSinceEpoch()});
            const auto ready = m_branchNoChangesReady.find(key);
            if (ready == m_branchNoChangesReady.end())
                return;
            const std::function<void(QString)> paint = ready.value();
            m_branchNoChangesReady.erase(ready);
            if (!html.isEmpty())
                paint(html);
        });
    return stale;
}

// A window button repaints the page it was clicked on over the range it names.
// Both pages read git, which pumps the event loop, so what is painted into is
// re-checked afterwards and the reader is left where they were scrolled to.
void MainWindow::showMergeRange(const QString &key)
{
    m_mergeRange = mergeRangeIndexForKey(key);
    if (gitMergedCoverShown()) {
        loadGitMergedStats();
        return;
    }
    // (The pane memo is keyed by range, so the window just left stays cached
    // for a quick return and the new one is computed if it is not there.)
    if (!m_branchDiffView || m_mergeRangePane.dir.isEmpty())
        return;
    const MergeRangePane pane = m_mergeRangePane; // copied: the reads are async
    const int scroll = m_branchDiffView->verticalScrollBar()->value();
    const int paneGen = m_branchDiffPaneGen;
    auto paint = [this, pane, scroll, paneGen](QString page) {
        if (!m_branchDiffView || page.isEmpty() || m_branchDiffPaneGen != paneGen)
            return; // the pane moved on to another range while this was read
        // Keep the choice on the page the flash repaints from, or the next
        // reload of the branch list would put the old window back.
        if (pane.celebration && m_branchMergedFlashBranch == pane.branch)
            m_branchMergedFlashHtml = page;
        setBranchDiffHtml(page, /*endCap=*/false);
        m_branchDiffView->verticalScrollBar()->setValue(scroll);
    };
    if (pane.celebration) {
        requestMergedBranchCelebrationHtml(pane.branch, pane.base, pane.dir,
                                           paint);
        return;
    }
    // A memo hit paints straight away; otherwise the worker's page does.
    paint(branchNoChangesPaneHtml(pane.branch, pane.base, pane.dir, pane.message,
                                  /*remember=*/true, paint));
}

void MainWindow::clearMergedBranchFlash()
{
    ++m_mergeCelebrationGen;
    m_branchMergedFlashBranch.clear();
    m_branchMergedFlashDir.clear();
    m_branchMergedFlashRow = -1;
    m_branchMergedFlashHtml.clear();
}

void MainWindow::refreshBranchesFromRemotes()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || m_branchRefreshRunning)
        return;
    m_branchRefreshRunning = true;
    setRepoDetailNotice(QStringLiteral("Refreshing branches and pruning remote refs…"));
    runOffThread<QStringList>(
        [dir] {
            QByteArray remotes;
            if (!runGitCapture(dir, {QStringLiteral("remote")}, &remotes, nullptr))
                return QStringList{QStringLiteral("could not list remotes")};
            QStringList failed;
            for (const QString &remote :
                 QString::fromUtf8(remotes).split('\n', Qt::SkipEmptyParts)) {
                if (!runGitCapture(dir, {QStringLiteral("fetch"),
                                         QStringLiteral("--prune"), remote},
                                   nullptr, nullptr))
                    failed << remote;
            }
            return failed;
        },
        [this, dir](QStringList failed) {
            m_branchRefreshRunning = false;
            const bool here = repoGitDir() == dir;
            if (here) {
                m_branchesCache.clear();
                loadBranchesAndTags();
                loadBranchesPanel();
            }
            noticeForRepo(
                here,
                failed.isEmpty()
                    ? QStringLiteral("Refreshed branches and pruned remote refs.")
                    : QStringLiteral("Refreshed branches; could not prune %1.")
                          .arg(failed.join(QStringLiteral(", "))),
                !failed.isEmpty());
        });
}

void MainWindow::deleteMergedBranches()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "This is a read-only mirror; branches can't be deleted here.", true);
        return;
    }
    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (base.isEmpty())
        return;

    QSet<QString> agentBranches;
    for (const AgentSession &session : std::as_const(m_agentSessions))
        if (!session.merged && session.owner == repo.owner
            && session.name == repo.name && !session.branchName.isEmpty())
            agentBranches.insert(session.branchName);
    runOffThread<QPair<QStringList, QStringList>>(
        [dir, branches, base, agentBranches] {
            const QString currentBranch = checkedOutBranch(dir);
            const QString commonDir = gitCommonDirOf(dir);
            QByteArray worktreeList;
            runGitCapture(dir, {"worktree", "list", "--porcelain"}, &worktreeList,
                          nullptr);
            const QSet<QString> checkedOut =
                forkmesh::gitrefusal::branchesCheckedOutInWorktrees(
                    QString::fromUtf8(worktreeList));
            QSet<QString> kept;
            QStringList merged;
            for (const QString &branch : branches) {
                if (branch == base || branch == currentBranch
                    || checkedOut.contains(branch) || agentBranches.contains(branch)) {
                    kept.insert(branch);
                    continue;
                }
                int behind = -1, ahead = -1;
                QByteArray counts;
                if (!runGitCapture(dir,
                                   {"rev-list", "--left-right", "--count",
                                    base + "..." + branch},
                                   &counts, nullptr)) {
                    kept.insert(branch);
                    continue;
                }
                const QStringList parts = QString::fromUtf8(counts).trimmed().split(
                    QRegularExpression(QStringLiteral("\\s+")));
                if (parts.size() >= 2) {
                    behind = parts.at(0).toInt();
                    ahead = parts.at(1).toInt();
                }
                if (behind == 0 && ahead == 0 &&
                    !branchNeverCommitted(readBranchRefHistory(commonDir, branch)))
                    merged << branch;
                else
                    kept.insert(branch);
            }
            QStringList mergedRemote;
            QByteArray remoteList;
            if (runGitCapture(dir,
                              {"for-each-ref", "--merged", base,
                               "--format=%(refname:short)", "refs/remotes/"},
                              &remoteList, nullptr)) {
                for (const QString &line :
                     QString::fromUtf8(remoteList).split('\n', Qt::SkipEmptyParts)) {
                    const QString ref = line.trimmed();
                    const QString branch = ref.section(QLatin1Char('/'), 1);
                    if (branch.isEmpty() || branch == QLatin1String("HEAD") ||
                        branch == base || kept.contains(branch) ||
                        agentBranches.contains(branch) ||
                        branch == QLatin1String("forkmesh/pulls") ||
                        branch.startsWith(QLatin1String("forkmesh/pull/")))
                        continue;
                    mergedRemote << ref;
                }
            }
            return qMakePair(merged, mergedRemote);
        },
        [this, repo, dir, base](QPair<QStringList, QStringList> found) {
            if (repoGitDir() != dir)
                return;
            confirmDeleteMergedBranches(repo, dir, base, found.first, found.second);
        });
}

void MainWindow::confirmDeleteMergedBranches(const RepositoryRecord &repo,
                                             const QString &dir, const QString &base,
                                             const QStringList &merged,
                                             const QStringList &mergedRemote)
{
    if (merged.isEmpty() && mergedRemote.isEmpty()) {
        setRepoDetailNotice(
            QStringLiteral("No branches are fully merged into %1.").arg(base));
        return;
    }
    QStringList listed = merged;
    listed += mergedRemote;
    constexpr int kListedMax = 40;
    QString list = QStringList(listed.mid(0, kListedMax)).join(QStringLiteral("\n"));
    if (listed.size() > kListedMax)
        list += QString::fromUtf8("\n\xE2\x80\xA6 and %1 more")
                    .arg(listed.size() - kListedMax);
    if (QMessageBox::question(
            this, "Delete merged branches",
            QStringLiteral("Delete the %1 local branch(es) and %2 remote "
                           "branch(es) fully merged into %3? Remote branches are "
                           "deleted on their remote; mirror copies "
                           "(forkmesh-mesh) drop on the next mesh sync. This "
                           "cannot be undone.\n\n%4")
                .arg(merged.size())
                .arg(mergedRemote.size())
                .arg(base, list),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    struct MergedCleanup {
        QStringList deleted;
        QStringList remoteDeleted;
        QStringList failed;
        bool meshDropped = false;
    };
    runOffThread<MergedCleanup>(
        [dir, merged, mergedRemote] {
            MergedCleanup result;
            for (const QString &branch : merged) {
                QByteArray tip;
                QString error;
                const bool found = runGitCapture(
                    dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                          QStringLiteral("refs/heads/") + branch},
                    &tip, &error);
                const bool marked = found && runGitCapture(
                    dir, {QStringLiteral("update-ref"),
                          forkmesh::upstream::deletedBranchTombstoneRef(branch),
                          QString::fromUtf8(tip).trimmed()},
                    nullptr, &error);
                if (marked && runGitCapture(dir, {"branch", "-D", branch}, nullptr, &error))
                    result.deleted << branch;
                else
                    result.failed << QStringLiteral("%1: %2").arg(
                        branch, forkmesh::gitrefusal::firstFailureLine(error)
                                    .left(160));
            }
            QByteArray remotesOut;
            runGitCapture(dir, {"remote"}, &remotesOut, nullptr);
            const QStringList configured =
                QString::fromUtf8(remotesOut).split('\n', Qt::SkipEmptyParts);
            QMap<QString, QStringList> byRemote;
            for (const QString &ref : mergedRemote)
                byRemote[ref.section('/', 0, 0)] << ref.section('/', 1);
            constexpr int kPushChunk = 100;
            for (auto it = byRemote.constBegin(); it != byRemote.constEnd(); ++it) {
                const QString &remote = it.key();
                QStringList droppable;
                if (!configured.contains(remote)) {
                    droppable = it.value();
                    result.meshDropped = true;
                } else {
                    for (int i = 0; i < it.value().size(); i += kPushChunk) {
                        const QStringList chunk = it.value().mid(i, kPushChunk);
                        QStringList args{QStringLiteral("push"),
                                         QStringLiteral("--porcelain"), remote,
                                         QStringLiteral("--delete")};
                        args += chunk;
                        QByteArray out;
                        QString err;
                        runGitCapture(dir, args, &out, &err);
                        const QString text = QString::fromUtf8(out) + '\n' + err;
                        for (const QString &ref : chunk) {
                            const bool deleted = text.contains(
                                QStringLiteral(":refs/heads/%1\t[deleted]").arg(ref));
                            const bool gone = text.contains(
                                QStringLiteral("unable to delete '%1': remote ref "
                                               "does not exist")
                                    .arg(ref));
                            if (deleted || gone)
                                droppable << ref;
                            else
                                result.failed << QStringLiteral("%1/%2: not deleted on %1")
                                                     .arg(remote, ref);
                        }
                    }
                }
                for (const QString &ref : std::as_const(droppable)) {
                    const QString tracking = remote + '/' + ref;
                    QString error;
                    if (runGitCapture(dir, {"branch", "-dr", tracking}, nullptr,
                                      &error))
                        result.remoteDeleted << tracking;
                    else
                        result.failed << QStringLiteral("%1: %2").arg(
                            tracking,
                            forkmesh::gitrefusal::firstFailureLine(error).left(160));
                }
            }
            return result;
        },
        [this, repo, dir](MergedCleanup result) {
            const bool here = repoGitDir() == dir;
            const int removed = result.deleted.size() + result.remoteDeleted.size();
            if (!result.deleted.isEmpty())
                logSystem(QStringLiteral("Git: deleted %1 merged branch(es): %2.")
                              .arg(result.deleted.size())
                              .arg(result.deleted.join(QStringLiteral(", "))));
            if (!result.remoteDeleted.isEmpty())
                logSystem(QStringLiteral("Git: deleted %1 merged remote branch(es): %2.")
                              .arg(result.remoteDeleted.size())
                              .arg(result.remoteDeleted.join(QStringLiteral(", "))));
            const int repoIndex = repoIndexFor(repo.owner, repo.name);
            if (result.meshDropped && repoIndex >= 0 && isSourceOfTruthRepo(repo))
                convergeSourceRepoFromMesh(repoIndex, /*force=*/true);
            else if (!result.deleted.isEmpty())
                propagateBranchDeletion(repo.owner, repo.name);
            if (result.failed.isEmpty())
                noticeForRepo(here, QStringLiteral("Deleted %1 merged branch(es).")
                                        .arg(removed));
            else
                noticeForRepo(here,
                              QStringLiteral("Deleted %1 merged branch(es); %2 could "
                                             "not be deleted (%3).")
                                  .arg(removed)
                                  .arg(result.failed.size())
                                  .arg(QStringList(result.failed.mid(0, 5))
                                           .join(QStringLiteral("; "))),
                              true);
            if (here) {
                m_branchesCache.clear();
                loadBranchesAndTags();
            }
        });
}

void MainWindow::deleteSelectedBranches()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree() || !m_branchesTable
        || m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString base = repoDefaultBranch(repoBranches());
    const QString current = headBranchFromFile(dir);
    QSet<QString> activeAgentBranches;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (!session.merged && session.owner == repo.owner && session.name == repo.name
            && !session.branchName.isEmpty())
            activeAgentBranches.insert(session.branchName);
    }
    QStringList local;
    QStringList remote;
    QStringList skipped;
    QSet<QString> seen;
    for (const QModelIndex &index : m_branchesTable->selectionModel()->selectedRows()) {
        QTableWidgetItem *name =
            m_branchesTable->item(index.row(), kBranchesNameColumn);
        QTableWidgetItem *status =
            m_branchesTable->item(index.row(), kBranchesStatusColumn);
        QTableWidgetItem *worktree =
            m_branchesTable->item(index.row(), kBranchesWorktreeColumn);
        if (!name || name->text().isEmpty() || seen.contains(name->text()))
            continue;
        const QString branch = name->text();
        seen.insert(branch);
        if (status && status->text() == QLatin1String("remote")) {
            const QString ref = branch.section(QLatin1Char('/'), 1);
            if (ref.isEmpty() || ref == base || ref == QLatin1String("forkmesh/pulls")
                || ref.startsWith(QLatin1String("forkmesh/pull/")))
                skipped << branch;
            else
                remote << branch;
            continue;
        }
        if (branch == base || branch == current || activeAgentBranches.contains(branch)
            || (worktree && !worktree->text().isEmpty()))
            skipped << branch;
        else
            local << branch;
    }
    if (local.isEmpty() && remote.isEmpty()) {
        setRepoDetailNotice("No selected branch can be deleted. Default, checked-out, "
                            "active-agent and worktree branches are protected.", true);
        return;
    }
    QStringList listed = local;
    listed += remote;
    constexpr int kListedMax = 40;
    QString list = QStringList(listed.mid(0, kListedMax)).join(QStringLiteral("\n"));
    if (listed.size() > kListedMax)
        list += QString::fromUtf8("\n\xE2\x80\xA6 and %1 more")
                    .arg(listed.size() - kListedMax);
    QString prompt = QStringLiteral("Delete the %1 selected local branch(es) and %2 "
                                    "remote branch(es)? This cannot be undone.\n\n%3")
                         .arg(local.size())
                         .arg(remote.size())
                         .arg(list);
    if (!skipped.isEmpty())
        prompt += QStringLiteral("\n\nProtected and kept: %1")
                      .arg(skipped.join(QStringLiteral(", ")));
    if (QMessageBox::question(this, "Delete selected branches", prompt,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    struct SelectedCleanup {
        QStringList deleted;
        QStringList remoteDeleted;
        QStringList failed;
        bool meshDropped = false;
    };
    runOffThread<SelectedCleanup>(
        [dir, local, remote] {
            SelectedCleanup result;
            for (const QString &branch : local) {
                QString error;
                if (deleteBranchAndCopies(dir, branch, &error))
                    result.deleted << branch;
                else
                    result.failed << QStringLiteral("%1: %2").arg(
                        branch, forkmesh::gitrefusal::firstFailureLine(error).left(160));
            }
            QByteArray remotesOut;
            runGitCapture(dir, {QStringLiteral("remote")}, &remotesOut, nullptr);
            const QStringList configured =
                QString::fromUtf8(remotesOut).split('\n', Qt::SkipEmptyParts);
            for (const QString &tracking : remote) {
                const QString remoteName = tracking.section(QLatin1Char('/'), 0, 0);
                const QString ref = tracking.section(QLatin1Char('/'), 1);
                if (!configured.contains(remoteName)) {
                    result.meshDropped = true;
                } else if (!pushDeletedRef(dir, remoteName, ref)) {
                    result.failed << QStringLiteral("%1: not deleted on %2")
                                         .arg(ref, remoteName);
                    continue;
                }
                QString error;
                if (runGitCapture(dir, {QStringLiteral("branch"), QStringLiteral("-dr"),
                                        tracking}, nullptr, &error))
                    result.remoteDeleted << tracking;
                else
                    result.failed << QStringLiteral("%1: %2").arg(
                        tracking, forkmesh::gitrefusal::firstFailureLine(error).left(160));
            }
            return result;
        },
        [this, repo, dir](SelectedCleanup result) {
            const bool here = repoGitDir() == dir;
            const int removed = result.deleted.size() + result.remoteDeleted.size();
            const int repoIndex = repoIndexFor(repo.owner, repo.name);
            if (result.meshDropped && repoIndex >= 0 && isSourceOfTruthRepo(repo))
                convergeSourceRepoFromMesh(repoIndex, /*force=*/true);
            else if (!result.deleted.isEmpty() || !result.remoteDeleted.isEmpty())
                propagateBranchDeletion(repo.owner, repo.name);
            if (result.failed.isEmpty())
                noticeForRepo(here, QStringLiteral("Deleted %1 selected branch(es).")
                                        .arg(removed));
            else
                noticeForRepo(here,
                              QStringLiteral("Deleted %1 selected branch(es); %2 could "
                                             "not be deleted (%3).")
                                  .arg(removed)
                                  .arg(result.failed.size())
                                  .arg(QStringList(result.failed.mid(0, 5))
                                           .join(QStringLiteral("; "))),
                              true);
            if (here) {
                m_branchesCache.clear();
                loadBranchesAndTags();
                loadBranchesPanel();
            }
        });
}

void MainWindow::updateBranchDetailActions(const QString &branch)
{
    if (!m_branchMergeButton)
        return;
    const QString dir = repoGitDir();
    const QString base = repoDefaultBranchFast();
    const bool isBase = branch.isEmpty() || branch == base;
    if (dir.isEmpty() || isBase) {
        applyBranchDetailActions(branch, base, 0, 0, false);
        return;
    }

    const int gen = ++m_branchDetailActionsGen;
    applyBranchDetailActions(branch, base, -1, -1, false);
    const QString branchWorktree = worktreePathForBranch(dir, branch);
    struct BranchDetailStats {
        int behind = 0;
        int ahead = 0;
        QString conflictKey;
        bool worktreeConflict = false;
    };
    runOffThread<BranchDetailStats>(
        [dir, base, branch, branchWorktree] {
            BranchDetailStats s;
            QByteArray counts;
            if (runGitCapture(dir,
                              {"rev-list", "--left-right", "--count",
                               base + "..." + branch},
                              &counts, nullptr)) {
                const QStringList parts = QString::fromUtf8(counts).trimmed().split(
                    QRegularExpression(QStringLiteral("\\s+")));
                if (parts.size() >= 2) {
                    s.behind = parts.at(0).toInt();
                    s.ahead = parts.at(1).toInt();
                }
            }
            if (s.behind > 0 && s.ahead > 0) {
                QByteArray baseSha;
                QByteArray branchSha;
                if (runGitCapture(dir, {"rev-parse", "--short", base}, &baseSha,
                                  nullptr) &&
                    runGitCapture(dir, {"rev-parse", "--short", branch}, &branchSha,
                                  nullptr))
                    s.conflictKey = branchConflictKey(
                        dir, QString::fromUtf8(baseSha).trimmed(),
                        QString::fromUtf8(branchSha).trimmed());
            }
            if (!branchWorktree.isEmpty()) {
                QByteArray unmerged;
                s.worktreeConflict =
                    runGitCapture(branchWorktree,
                                  {"diff", "--name-only", "--diff-filter=U"},
                                  &unmerged, nullptr) &&
                    !unmerged.trimmed().isEmpty();
            }
            return s;
        },
        [this, gen, branch, base, dir](BranchDetailStats s) {
            if (gen != m_branchDetailActionsGen || m_branchDiffBranch != branch)
                return;
            bool hasConflict = false;
            bool conflictCheckPending = false;
            if (!s.conflictKey.isEmpty()) {
                const auto cached = m_branchConflictCache.constFind(s.conflictKey);
                if (cached != m_branchConflictCache.constEnd())
                    hasConflict = *cached;
                else {
                    conflictCheckPending = true;
                    startBranchConflictProbes(dir, base, {{branch, s.conflictKey}});
                }
            }
            applyBranchDetailActions(branch, base, s.behind, s.ahead, hasConflict,
                                     s.worktreeConflict, conflictCheckPending);
        });
}

void MainWindow::applyBranchDetailActions(const QString &branch, const QString &base,
                                          int behind, int ahead, bool hasConflict,
                                          bool worktreeConflict,
                                          bool conflictCheckPending)
{
    if (!m_branchMergeButton)
        return;
    const QString dir = repoGitDir();
    const bool writable = repoHasWorkingTree();
    const bool isBase = branch.isEmpty() || branch == base;
    const bool counted = behind >= 0 && ahead >= 0;

    // The detail read and merge-tree probe finish independently of the range
    // diff. Keep the health strip on the same generation of branch state, then
    // re-grade an already-painted patch as soon as Git's answer arrives.
    if (branch == m_branchDiffBranch) {
        m_branchDiffHealthBehind = behind;
        m_branchDiffHealthAhead = ahead;
        m_branchDiffHealthConflict = hasConflict;
        m_branchDiffHealthWorktreeConflict = worktreeConflict;
        m_branchDiffHealthConflictPending =
            !isBase && (!counted || conflictCheckPending);
        if (m_branchDiffSummaryBar && m_branchDiffSummaryBar->isVisible() &&
            !m_branchDiffHealthPatch.isEmpty()) {
            updateBranchDiffHealth(diffRenderedFiles(m_branchDiffView),
                                   m_branchDiffHealthPatch);
        }
    }

    if (m_branchDetailLabel) {
        const QString pair =
            base.isEmpty() || branch == base
                ? QStringLiteral("<b>%1</b>").arg(branch.toHtmlEscaped())
                : QString::fromUtf8("<b>%1</b> \xE2\x86\x92 <b>%2</b>")
                      .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
        QString text;
        if (branch.isEmpty())
            text.clear();
        else if (branch == base)
            text = QString::fromUtf8("%1 \xC2\xB7 default branch").arg(pair);
        else if (!counted) {
            text = QString::fromUtf8("%1 \xC2\xB7 checking\xE2\x80\xA6").arg(pair);
        } else {
            text = QString::fromUtf8("%1 \xC2\xB7 %2 behind \xC2\xB7 %3 ahead")
                       .arg(pair)
                       .arg(behind)
                       .arg(ahead);
            if (hasConflict)
                text += QString::fromUtf8(
                    " \xC2\xB7 <span style='color:#f85149'>conflicts</span>");
            if (worktreeConflict)
                text += QString::fromUtf8(
                    " \xC2\xB7 <span style='color:#d29922'>local edits need "
                    "resolution</span>");
        }
        m_branchDetailLabel->setToolTip(
            branch.isEmpty() || base.isEmpty() || branch == base
                ? QString()
                : QStringLiteral("Showing %1's complete checkout compared with %2")
                      .arg(branch, base));
        if (m_branchDiffPullNumber >= 0 && !text.isEmpty()) {
            QString state;
            for (const PullRequest &p : std::as_const(m_currentPulls)) {
                if (p.number == m_branchDiffPullNumber) {
                    state = p.status;
                    break;
                }
            }
            const QString color = state == QLatin1String("merged")
                                      ? Theme::nextOrHex(
                                            Theme::Role::AccentFg,
                                            QStringLiteral("#a371f7"))
                                      : state == QLatin1String("closed")
                                            ? QStringLiteral("#f85149")
                                            : QStringLiteral("#3fb950");
            text = QString::fromUtf8("<b>PR #%1</b>%2 \xC2\xB7 %3")
                       .arg(m_branchDiffPullNumber)
                       .arg(state.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                      " <span style='color:%1'>%2</span>")
                                      .arg(color, state.toHtmlEscaped()))
                       .arg(text);
        }
        m_branchDetailLabel->setText(text);
    }

    if (m_branchOpenPullButton) {
        const bool prMode = m_branchDiffPullNumber >= 0;
        m_branchOpenPullButton->setVisible(prMode);
        if (prMode) {
            m_branchOpenPullButton->setText(
                QStringLiteral("PR #%1").arg(m_branchDiffPullNumber));
            m_branchOpenPullButton->setToolTip(
                QString::fromUtf8("Open pull request #%1 \xE2\x80\x94 conversation, "
                               "checks and merge controls")
                    .arg(m_branchDiffPullNumber));
        }
    }

    refreshBranchAgentButton();

    refreshBranchPackageActions(branch, dir);

    m_branchPullButton->setText(base.isEmpty() ? QStringLiteral("Pull main")
                                               : QStringLiteral("Pull %1").arg(base));
    const bool canPull = writable && !isBase && behind > 0 && !worktreeConflict &&
                         !m_worktreeMergeInProgress && !m_branchMergeFlowBusy;
    m_branchPullButton->setEnabled(canPull);
    if (m_branchPullButton->property("needsPull").toBool() != canPull) {
        m_branchPullButton->setProperty("needsPull", canPull);
        m_branchPullButton->style()->unpolish(m_branchPullButton);
        m_branchPullButton->style()->polish(m_branchPullButton);
        m_branchPullButton->update();
    }
    if (worktreeConflict)
        m_branchPullButton->setToolTip(
            "The branch is updated; resolve its restored local edits next");
    else if (isBase)
        m_branchPullButton->setToolTip(
            QStringLiteral("Select a branch other than %1").arg(base));
    else if (!writable)
        m_branchPullButton->setToolTip(
            "Read-only mirror \xE2\x80\x94 no working tree to update");
    else if (!counted)
        m_branchPullButton->setToolTip(
            QString::fromUtf8("Checking how far %1 is behind %2\xE2\x80\xA6")
                .arg(branch, base));
    else if (behind == 0)
        m_branchPullButton->setToolTip(
            QStringLiteral("%1 is already up to date with %2").arg(branch, base));
    else
        m_branchPullButton->setToolTip(
            QStringLiteral("Merge %1 into %2 (%3 commit(s) behind)")
                .arg(base, branch)
                .arg(behind));

    if (m_branchMergeEditorButton) {
        const bool canMergeEditor =
            writable && !isBase && (behind > 0 || worktreeConflict);
        m_branchMergeEditorButton->setText(worktreeConflict
                                               ? QStringLiteral("Resolve local edits")
                                               : QStringLiteral("Merge editor"));
        m_branchMergeEditorButton->setEnabled(canMergeEditor);
        if (worktreeConflict)
            m_branchMergeEditorButton->setToolTip(
                QStringLiteral("Resolve the agent's local edits after updating %1 "
                               "from %2; Git kept an autostash backup")
                    .arg(branch, base));
        else if (isBase)
            m_branchMergeEditorButton->setToolTip(
                QStringLiteral("Select a branch other than %1").arg(base));
        else if (!writable)
            m_branchMergeEditorButton->setToolTip(
                "Read-only mirror \xE2\x80\x94 no working tree to update");
        else if (!counted)
            m_branchMergeEditorButton->setToolTip(
                QString::fromUtf8("Checking how far %1 is behind %2\xE2\x80\xA6")
                    .arg(branch, base));
        else if (behind == 0)
            m_branchMergeEditorButton->setToolTip(
                QStringLiteral("%1 is already up to date with %2").arg(branch, base));
        else
            m_branchMergeEditorButton->setToolTip(
                QStringLiteral("Merge %1 into %2 and resolve any conflicts in an "
                               "editor")
                    .arg(base, branch));
    }

    m_branchFixButton->setToolTip(
        QString::fromUtf8("Let the chosen agent merge %1 into %2 and resolve the "
                       "conflicts \xE2\x80\x94 watch it on the Agents tab")
            .arg(base, branch));

    const bool canPr = writable && !isBase;
    m_branchPrButton->setEnabled(canPr);
    m_branchPrButton->setToolTip(
        canPr ? QStringLiteral("Open a pull request from %1 into %2").arg(branch, base)
              : (isBase ? "Select a branch other than the default to open a pull request"
                        : "Read-only mirror \xE2\x80\x94 no working tree to open a pull "
                          "request from"));

    if (m_branchQueueButton) {
        const bool queueOn = mergeQueueEnabledForOpenRepo();
        const bool canQueue = queueOn && writable && !isBase;
        m_branchQueueButton->setVisible(queueOn);
        m_branchQueueButton->setEnabled(canQueue);
        m_branchQueueButton->setToolTip(
            canQueue
                ? QStringLiteral("Send %1 to the merge queue — its pull request "
                                 "(opened now if it has none) is updated from %2 "
                                 "and merged in turn")
                      .arg(branch, base)
                : (isBase ? QStringLiteral("Select a branch other than %1").arg(base)
                          : QString::fromUtf8("Read-only mirror \xE2\x80\x94 nothing "
                                           "to merge into here")));
    }

    const bool canMerge = writable && !isBase && !m_worktreeMergeInProgress &&
                          !m_branchMergeFlowBusy &&
                          !m_branchCleanupInProgress.contains(branch);
    m_branchMergeButton->setEnabled(canMerge);
    m_branchMergeButton->setToolTip(
        canMerge ? QStringLiteral("Merge %1 into %2").arg(branch, base)
                 : (isBase ? QStringLiteral("Select a branch other than %1").arg(base)
                           : "Read-only mirror \xE2\x80\x94 nothing to merge into here"));

    if (m_branchMergeDeleteButton) {
        m_branchMergeDeleteButton->setEnabled(canMerge);
        m_branchMergeDeleteButton->setToolTip(
            canMerge
                ? QStringLiteral("Merge %1 into %2, then delete its agent session, "
                                 "pull request, branch and worktree")
                      .arg(branch, base)
                : (isBase ? QStringLiteral("Select a branch other than %1").arg(base)
                          : "Read-only mirror \xE2\x80\x94 nothing to merge into here"));
    }

    if (m_branchDeleteCleanupButton) {
        m_branchDeleteCleanupButton->setEnabled(canMerge);
        m_branchDeleteCleanupButton->setToolTip(
            canMerge
                ? QStringLiteral("Delete %1 without merging it: its worktree, "
                                 "branch, agent sessions and pull request")
                      .arg(branch)
                : (isBase ? QStringLiteral("Select a branch other than %1").arg(base)
                          : "Read-only mirror \xE2\x80\x94 nothing to delete here"));
    }

    // Last, because it resolves the branch's checkout and so may pump the event
    // loop: every button above is already in its final state by then.
    refreshBranchLockState();
}

void MainWindow::refreshBranchLockState()
{
    if (!m_branchClearLockButton)
        return;
    // Only the branch's own checkout is asked about. A lock in another
    // worktree's git dir is not this branch's to clear, and a branch nobody has
    // checked out has no git dir of its own to be stuck in.
    const QString gitDir = resolveGitDirPath(
        repoHasWorkingTree() ? branchWorkDir(m_branchDiffBranch) : QString());
    if (gitDir != m_branchLockDir) {
        m_branchLockDir = gitDir;
        if (m_branchLockWatcher) {
            const QStringList watched = m_branchLockWatcher->directories();
            if (!watched.isEmpty())
                m_branchLockWatcher->removePaths(watched);
        }
        if (!gitDir.isEmpty()) {
            // The lock normally appears while the diff is already open — the
            // reviewer stages a file, Git refuses — so the header cannot wait
            // for the next branch refresh to find out about it.
            if (!m_branchLockWatcher) {
                m_branchLockWatcher = new QFileSystemWatcher(this);
                connect(m_branchLockWatcher, &QFileSystemWatcher::directoryChanged,
                        this, [this](const QString &) { updateBranchLockButton(); });
            }
            m_branchLockWatcher->addPath(gitDir);
        }
    }
    updateBranchLockButton();
}

void MainWindow::updateBranchLockButton()
{
    if (!m_branchClearLockButton)
        return;
    m_branchLockPaths = gitLocksInDir(m_branchLockDir);
    m_branchClearLockButton->setVisible(!m_branchLockPaths.isEmpty());
    if (m_branchLockPaths.isEmpty())
        return;
    QStringList lines;
    for (const QString &path : std::as_const(m_branchLockPaths))
        lines << QStringLiteral("%1 (left %2)")
                     .arg(path,
                          formatIssueRelativeTime(
                              QFileInfo(path).lastModified().toMSecsSinceEpoch()));
    m_branchClearLockButton->setText(m_branchLockPaths.size() > 1
                                         ? QStringLiteral("Clear locks")
                                         : QStringLiteral("Clear lock"));
    m_branchClearLockButton->setToolTip(
        QStringLiteral("Git left a lock in %1's checkout, so nothing there can be "
                       "staged, committed or merged until it goes:\n%2\n\nRemove "
                       "it once no Git process is running.")
            .arg(m_branchDiffBranch.isEmpty() ? QStringLiteral("this branch")
                                              : m_branchDiffBranch,
                 lines.join(QLatin1Char('\n'))));
}

void MainWindow::clearBranchGitLock()
{
    // The lock can go away between the button appearing and the click: the Git
    // process that held it finishes, or another window clears it.
    updateBranchLockButton();
    if (m_branchLockPaths.isEmpty()) {
        setRepoDetailNotice(QStringLiteral("That Git lock is already gone."), false,
                            forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QStringList locks = m_branchLockPaths;
    const QDateTime now = QDateTime::currentDateTime();
    qint64 youngestMs = QFileInfo(locks.first()).lastModified().msecsTo(now);
    for (const QString &path : locks)
        youngestMs = qMin(youngestMs, QFileInfo(path).lastModified().msecsTo(now));
    // A lock this new is more likely held than abandoned, and deleting one out
    // from under a running Git leaves its write half-done. Everywhere else in
    // ForkMesh that window simply means "don't touch it"; here the reviewer
    // asked, so it becomes a question instead of a silent refusal.
    bool held = false;
    for (const QString &path : locks)
        held = held || forkmesh::gitLockHeldByLiveProcess(path);
    if ((held || youngestMs < forkmesh::kGitLockStaleMs) &&
        QMessageBox::question(
            this, QStringLiteral("Clear Git lock"),
            QStringLiteral("%1\n\n%2, and removing a live lock leaves that "
                           "write half-finished.\n\nRemove it anyway?")
                .arg(locks.join(QLatin1Char('\n')),
                     held ? QStringLiteral("is held open by a running Git process")
                          : QStringLiteral("was written in the last minute, so a "
                                           "Git process may still be holding it")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QStringList removed;
    QStringList failed;
    for (const QString &path : locks) {
        if (QFile::remove(path))
            removed.append(QFileInfo(path).fileName());
        else
            failed.append(QFileInfo(path).fileName());
    }
    updateBranchLockButton();
    const QString branch = m_branchDiffBranch.isEmpty()
                               ? QStringLiteral("the branch")
                               : m_branchDiffBranch;
    if (!removed.isEmpty())
        logSystem(QStringLiteral("Git: cleared lock(s) %1 from %2's checkout (%3).")
                      .arg(removed.join(QStringLiteral(", ")), branch,
                           m_branchLockDir));
    if (failed.isEmpty())
        setRepoDetailNotice(
            QStringLiteral("Cleared %1 in %2's checkout — Git can write there "
                           "again.")
                .arg(removed.join(QStringLiteral(" and ")), branch));
    else
        setRepoDetailNotice(
            QStringLiteral("Could not remove %1 from %2 — check who owns the "
                           "file.")
                .arg(failed.join(QStringLiteral(", ")), m_branchLockDir),
            true, forkmesh::kLocalOperationNoticeKind);
}

QString MainWindow::branchWorkDir(const QString &branch) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const QString localPath = m_repositories.at(m_repoDetailIndex).localPath;
    if (localPath.isEmpty() || branch.isEmpty())
        return QString();
    const QString wt = worktreePathForBranch(localPath, branch);
    if (!wt.isEmpty())
        return wt;
    if (!repoHasWorkingTree())
        return QString();
    if (headBranchFromFile(localPath) == branch)
        return localPath;
    return QString();
}

// The agent behind the branch currently being diffed, or -1 when the branch is
// nobody's. A diff opened from the agent already carries its session id; one
// reached from the branch list, a commit link or a search hit does not, so the
// branch name is matched back to the session that claims it, by the same rule
// the branch rows use. Both routes land on the same conversation, which is what
// lets the toolbar link and the prompt bar agree about who they are talking to.
int MainWindow::branchDiffAgentSessionId() const
{
    if (m_branchDiffBranch.isEmpty() || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return -1;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    for (const AgentSession &s : m_agentSessions) {
        // The session the diff was opened from wins outright.
        if (s.id == m_branchDiffAgentSessionId &&
            s.branchName == m_branchDiffBranch)
            return s.id;
    }
    const AgentSession *owner = sessionForMergedBranch(
        m_agentSessions, m_branchDiffBranch, repo.owner, repo.name);
    return owner ? owner->id : -1;
}

void MainWindow::refreshBranchConflictButton()
{
    if (!m_branchConflictButton || !m_branchFixButton)
        return;
    const bool conflict =
        !m_branchDiffBranch.isEmpty() && m_branchDiffHealthConflict;
    const bool writable = repoHasWorkingTree();
    const int sessionId = conflict ? branchDiffAgentSessionId() : -1;
    const bool viaAgent = sessionId != -1;
    const bool viaNewAgent = conflict && !viaAgent;

    m_branchConflictButton->setVisible(viaAgent);
    m_branchConflictButton->setEnabled(viaAgent && writable);
    if (viaAgent) {
        m_branchConflictButton->setProperty("forkmeshOcticon", QVariant());
        m_branchConflictButton->setIcon(themedOcticon(
            QStringLiteral("alert"),
            Theme::nextOr(Theme::Role::Warning,
                          QColor(currentThemeIsDark() ? "#e3742f" : "#bc4c00")),
            14));
        m_branchConflictButton->setIconSize(QSize(14, 14));
        m_branchConflictButton->setToolTip(
            QString::fromUtf8("%1 conflicts with %2 â ask agent #%3 to "
                              "merge %2 in, resolve the conflicts and commit")
                .arg(m_branchDiffBranch, repoDefaultBranchFast())
                .arg(sessionId));
    }

    m_branchFixButton->setVisible(viaNewAgent);
    m_branchFixButton->setEnabled(viaNewAgent && writable);
    if (m_branchFixAgentCombo) {
        m_branchFixAgentCombo->setVisible(viaNewAgent);
        m_branchFixAgentCombo->setEnabled(viaNewAgent && writable);
    }
    if (m_branchFixModelCombo) {
        m_branchFixModelCombo->setVisible(viaNewAgent);
        m_branchFixModelCombo->setEnabled(viaNewAgent && writable);
    }
}

void MainWindow::refreshBranchAgentButton()
{
    refreshBranchConflictButton();
    if (!m_branchOpenAgentButton)
        return;
    const int sessionId = branchDiffAgentSessionId();
    m_branchOpenAgentButton->setVisible(sessionId != -1);
    if (sessionId == -1)
        return;

    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;

    // This button used to retain its generic rocket. It is a link to a
    // particular session, so use the same status glyph and tint as its branch
    // row. Clear the stored static octicon too, otherwise a later theme refresh
    // would restore that rocket.
    m_branchOpenAgentButton->setProperty("forkmeshOcticon", QVariant());
    m_branchOpenAgentButton->setIcon(agentStatusOcticon(*session, 14));
    m_branchOpenAgentButton->setIconSize(QSize(14, 14));
    m_branchOpenAgentButton->setText(QStringLiteral("Agent #%1").arg(sessionId));

    const QString status = session->merged ? QStringLiteral("Merged")
                                            : agentStatusText(session->status);
    m_branchOpenAgentButton->setToolTip(
        QString::fromUtf8("Open agent #%1 \xE2\x80\x94 %2. The conversation that wrote "
                          "%3; its prompt bar below adds to that conversation.")
            .arg(sessionId)
            .arg(status)
            .arg(m_branchDiffBranch));
    m_branchOpenAgentButton->setAccessibleName(
        QString::fromUtf8("Agent #%1 \xE2\x80\x94 %2")
            .arg(sessionId)
            .arg(status));
}

namespace {

// Mirrors MainWindow::BranchDiffLogKind for the worker-side helpers below,
// which cannot name a private nested enum. Kept in step by the static_assert
// in buildBranchDiffLogPanel().
enum BranchDiffLogKindInt : int {
    kLogStep = 0,
    kLogInfo,
    kLogGit,
    kLogProgress,
    kLogOk,
    kLogWarn,
    kLogFail,
    kLogProgressDone,
};
using BranchDiffLogFn = std::function<void(const QString &, int)>;

QString humanBytes(qint64 n)
{
    if (n < 1024)
        return QStringLiteral("%1 B").arg(n);
    if (n < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(n / 1024.0, 0, 'f', 1);
    if (n < qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(n / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(n / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString firstLineOf(const QString &text)
{
    const QString clean = text.trimmed();
    const int nl = clean.indexOf(QLatin1Char('\n'));
    return nl < 0 ? clean : clean.left(nl);
}

// "git diff --cached abc" out of the "git -C <dir> diff --cached abc" a
// ticket carries: the log already said which directory the load runs in.
QString gitTicketShortName(const QString &detail)
{
    static const QRegularExpression dirPrefix(
        QStringLiteral("^git -C (\"[^\"]*\"|\\S+) "));
    static const QRegularExpression dirSuffix(QStringLiteral(" \\([^()]*\\)$"));
    QString name = detail.simplified();
    name.replace(dirPrefix, QStringLiteral("git "));
    name.remove(dirSuffix); // gitArgsLine() appends the checkout as " (<dir>)"
    return name;
}

constexpr int kBranchDiffShortGitMs = 60 * 1000;
constexpr int kBranchDiffLongGitMs = 15 * 60 * 1000;
constexpr int kBranchDiffProgressEveryMs = 250;
constexpr int kBranchDiffHeartbeatMs = 2000;

struct StreamedGit {
    bool ok = false;
    bool timedOut = false;
    QByteArray out;
    QString err;
    qint64 elapsedMs = 0;
    int files = 0; // "diff --git" headers seen in the output so far
};

// Runs one git command on the calling (worker) thread and narrates it: the
// output is read as it arrives, so a diff that takes a minute to stream
// reports bytes and files every quarter second instead of going quiet, and a
// command that produces nothing for a while (`git add -A` over a big tree)
// says it is still working. Opens the same "git" ticket waitForGit does so
// the footer's background strip and the pane's log agree.
// code-health: allow(blocking-wait) sync git primitive; every caller is inside the runOffThread<ScopeDiff> branch-diff read
StreamedGit runGitStreaming(const QString &dir, const QStringList &args,
                            const QProcessEnvironment *env,
                            const BranchDiffLogFn &log,
                            const QString &progressLabel, int timeoutMs)
{
    StreamedGit r;
    QProcess process;
    if (env)
        process.setProcessEnvironment(*env);
    process.setProgram(QStringLiteral("git"));
    process.setArguments(QStringList{QStringLiteral("-C"), dir} + args);

    const QString commandLine = gitProcessCommandLine(process);
    forkmesh::BackgroundWork work;
    work.kind = QStringLiteral("git");
    work.detail = commandLine;
    work.execution = forkmesh::ActionTelemetry::Execution::Worker;
    work.source = forkmesh::logSourceHere();
    work.workingDirectory = dir;
    const quint64 ticket = forkmesh::BackgroundActivity::begin(std::move(work));

    QElapsedTimer clock;
    clock.start();
    process.start();
    if (!process.waitForStarted(5000)) {
        r.err = QStringLiteral("git failed to start: %1").arg(process.errorString());
        r.elapsedMs = clock.elapsed();
        forkmesh::BackgroundActivity::end(ticket, QStringLiteral("failed to start"));
        return r;
    }
    process.closeWriteChannel();

    qsizetype scanned = 0; // r.out bytes already searched for diff headers
    QByteArray stderrText;
    // Both channels are drained every pass. Stdout alone is not enough: a
    // command that writes more than a pipe buffer of warnings (`git add -A`
    // over a tree full of line-ending or filemode notices) blocks on stderr
    // and never finishes, and the pane would sit on its last progress line.
    const auto drain = [&] {
        stderrText += process.readAllStandardError();
        const QByteArray chunk = process.readAllStandardOutput();
        if (chunk.isEmpty())
            return false;
        r.out += chunk;
        static const QByteArray header("diff --git ");
        const qsizetype previouslyScanned = scanned;
        qsizetype at = r.out.indexOf(
            header, qMax<qsizetype>(0, previouslyScanned - header.size()));
        while (at >= 0) {
            // A header whose end was already inside the scanned prefix was
            // counted last time round; one straddling it was not.
            if (at + header.size() > previouslyScanned &&
                (at == 0 || r.out.at(at - 1) == '\n'))
                ++r.files;
            at = r.out.indexOf(header, at + header.size());
        }
        scanned = r.out.size();
        return true;
    };
    const auto progress = [&](bool final) {
        if (progressLabel.isEmpty())
            return;
        log(QStringLiteral("%1: %2, %3 files, %4%5")
                .arg(progressLabel, humanBytes(r.out.size()))
                .arg(r.files)
                .arg(forkmesh::backgroundElapsedText(clock.elapsed()),
                     final ? QString() : QString::fromUtf8(" \xE2\x80\xA6")),
            final ? kLogProgressDone : kLogProgress);
    };

    qint64 lastReportMs = 0;
    qint64 lastOutputMs = 0;
    qint64 lastHeartbeatMs = 0;
    bool reportedAnything = false;
    while (true) {
        process.waitForReadyRead(200);
        const bool drained = drain(); // both channels, every pass
        const qint64 now = clock.elapsed();
        if (drained) {
            lastOutputMs = now;
            if (now - lastReportMs >= kBranchDiffProgressEveryMs) {
                progress(/*final=*/false);
                lastReportMs = now;
                reportedAnything = true;
            }
        }
        if (process.state() == QProcess::NotRunning) {
            process.waitForFinished(1000); // reap; flushes the child's pipes
            drain();
            break;
        }
        if (now >= timeoutMs) {
            r.timedOut = true;
            r.err = gitTimeoutError(process, timeoutMs);
            break;
        }
        if (!drained && now - lastOutputMs >= kBranchDiffHeartbeatMs &&
            now - lastHeartbeatMs >= kBranchDiffHeartbeatMs) {
            lastHeartbeatMs = now;
            log(QStringLiteral("%1 still working: %2 elapsed, %3 received so far")
                    .arg(gitTicketShortName(commandLine),
                         forkmesh::backgroundElapsedText(now),
                         humanBytes(r.out.size())),
                kLogProgress);
        }
    }
    r.elapsedMs = clock.elapsed();
    if (r.timedOut) {
        forkmesh::BackgroundActivity::end(ticket, QStringLiteral("timed out"));
        return r;
    }
    if (reportedAnything || (!progressLabel.isEmpty() && !r.out.isEmpty()))
        progress(/*final=*/true);
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
        r.ok = true;
    } else {
        stderrText += process.readAllStandardError();
        r.err = QString::fromUtf8(stderrText).trimmed();
        if (r.err.isEmpty())
            r.err = process.exitStatus() != QProcess::NormalExit
                        ? QStringLiteral("git crashed")
                        : QStringLiteral("git exited with code %1")
                              .arg(process.exitCode());
    }
    forkmesh::BackgroundActivity::end(ticket, processExitOutcome(process));
    return r;
}

// buildWorkingTreeDiff() with every step narrated: the temporary index, the
// base tree load, the whole-tree add, and the diff streamed as it arrives.
bool buildWorkingTreeDiffStreamed(const QString &dir, const QString &base,
                                  QByteArray *out, QString *err, int *files,
                                  const BranchDiffLogFn &log)
{
    if (!QDir(dir).exists()) {
        if (err)
            *err = QStringLiteral("The worktree at %1 is gone.").arg(dir);
        return false;
    }
    QTemporaryDir temp;
    if (!temp.isValid()) {
        if (err)
            *err = QStringLiteral("Could not create a temporary Git index.");
        return false;
    }
    const QString indexPath = temp.path() + QStringLiteral("/index");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_INDEX_FILE"), indexPath);
    log(QStringLiteral("Snapshotting the worktree into a temporary index (%1) so "
                       "uncommitted and untracked files count as changes")
            .arg(indexPath),
        kLogStep);

    // Seeded from the live index when there is one, so the add below only
    // hashes files whose stat data changed; the base tree is the cold
    // fallback, which re-hashes the whole tree.
    bool lastSeeded = false; // whether the latest attempt used the live index
    const auto stageSnapshot = [&](bool seed) {
        QString livePath;
        bool seeded = false;
        lastSeeded = false;
        if (seed) {
            QElapsedTimer copyClock;
            copyClock.start();
            seeded = seedSnapshotIndexFromWorktree(dir, indexPath, &livePath);
            lastSeeded = seeded;
            if (seeded)
                log(QStringLiteral("Seeded the snapshot from the live index %1 "
                                   "in %2: its stat cache lets git skip every "
                                   "file that has not changed since it was "
                                   "last written")
                        .arg(livePath, forkmesh::backgroundElapsedText(
                                           copyClock.elapsed())),
                    kLogInfo);
            else
                log(QStringLiteral("No live index to copy (%1); loading the "
                                   "base tree instead")
                        .arg(livePath.isEmpty()
                                 ? QStringLiteral("could not resolve the gitdir")
                                 : livePath),
                    kLogInfo);
        }
        if (!seeded) {
            log(QStringLiteral("Loading the worktree's HEAD tree into the "
                               "temporary index (a large tree spends most of "
                               "the read re-hashing every file after this)"),
                kLogInfo);
            const StreamedGit readTree = runGitStreaming(
                dir, {QStringLiteral("read-tree"), QStringLiteral("HEAD")}, &env,
                log, QString(), kBranchDiffShortGitMs);
            if (!readTree.ok) {
                if (err)
                    *err = readTree.err;
                return false;
            }
        }
        log(QStringLiteral("Adding every tracked, modified and untracked file "
                           "under the worktree to the snapshot"),
            kLogInfo);
        const StreamedGit add = runGitStreaming(
            dir,
            {QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--"),
             QStringLiteral(".")},
            &env, log, QString(), kBranchDiffLongGitMs);
        if (!add.ok) {
            if (err)
                *err = add.err;
            return false;
        }
        return true;
    };
    if (!stageSnapshot(/*seed=*/true)) {
        const QString firstError = err ? *err : QString();
        const bool racedDeletion =
            firstError.contains(QStringLiteral("unable to stat"),
                                Qt::CaseInsensitive) ||
            firstError.contains(QStringLiteral("No such file"),
                                Qt::CaseInsensitive);
        if (racedDeletion)
            log(QStringLiteral("A file vanished while the snapshot was being "
                               "taken (%1); taking it again")
                    .arg(firstLineOf(firstError)),
                kLogWarn);
        else if (lastSeeded)
            // An index git would not read (a format this git does not know,
            // a truncated copy) is worth one cold rebuild before giving up.
            log(QStringLiteral("The seeded snapshot failed (%1); rebuilding it "
                               "from the base tree")
                    .arg(firstLineOf(firstError)),
                kLogWarn);
        else
            return false;
        if (!stageSnapshot(/*seed=*/racedDeletion))
            return false;
    }
    log(QStringLiteral("Diffing the snapshot against the merge base of %1 and "
                       "the branch, so commits %1 has that the branch never "
                       "merged do not show as its changes")
            .arg(base),
        kLogStep);
    StreamedGit diff = runGitStreaming(
        dir,
        {QStringLiteral("diff"), QStringLiteral("--binary"),
         QStringLiteral("--cached"), QStringLiteral("--merge-base"), base},
        &env, log, QStringLiteral("Receiving diff"), kBranchDiffLongGitMs);
    if (!diff.ok && !diff.timedOut &&
        diff.err.contains(QStringLiteral("no merge base"), Qt::CaseInsensitive)) {
        log(QStringLiteral("No merge base with %1; diffing against it directly")
                .arg(base),
            kLogWarn);
        diff = runGitStreaming(
            dir,
            {QStringLiteral("diff"), QStringLiteral("--binary"),
             QStringLiteral("--cached"), base},
            &env, log, QStringLiteral("Receiving diff"), kBranchDiffLongGitMs);
    }
    if (out)
        *out = diff.out;
    if (files)
        *files = diff.files;
    if (!diff.ok) {
        if (err)
            *err = diff.err;
        return false;
    }
    return true;
}

struct ScopeDiff {
    bool ok = true;
    bool branchGone = false;
    bool workGone = false;
    QByteArray out;
    QString text;
    QHash<QString, QString> fingerprints;
    QString err;
    QString emptyMessage;
    QString viewedContext;
    int attempts = 1;
    int files = 0;
    qint64 readMs = 0;
};

ScopeDiff readBranchScopeDiff(const QString &dir, const QString &base,
                              const QString &branch, const QString &work,
                              int pullNumber, const BranchDiffLogFn &log)
{
    ScopeDiff r;
    r.viewedContext = pullNumber >= 0
                          ? QStringLiteral("pull/") +
                                QString::number(pullNumber)
                          : QStringLiteral("branch/") + branch;
    r.emptyMessage =
        QStringLiteral("No changes between %1 and %2.").arg(branch, base);
    QElapsedTimer total;
    total.start();
    auto readDiff = [&](ScopeDiff &attempt) {
        // Re-checked per attempt: the worktree can be removed between the
        // click and this read, or between two retries of it.
        if (!work.isEmpty()) {
            log(QStringLiteral("Checking that the worktree is still a "
                               "checkout root"),
                kLogInfo);
            if (!QDir(work).exists()) {
                log(QStringLiteral("The worktree directory is gone; "
                                   "falling back to the committed range"),
                    kLogWarn);
                attempt.workGone = true;
            } else if (!isGitWorkingTreeRoot(work)) {
                log(QStringLiteral("%1 is not a worktree root (or git "
                                   "could not read it); falling back to "
                                   "the committed range")
                        .arg(work),
                    kLogWarn);
                attempt.workGone = true;
            }
        }
        if (!work.isEmpty() && !attempt.workGone) {
            if (!buildWorkingTreeDiffStreamed(work, base, &attempt.out,
                                              &attempt.err,
                                              &attempt.files, log))
                attempt.ok = false;
        } else {
            log(QStringLiteral("Reading the committed range %1...%2 "
                               "(commits on %2 since it left %1)")
                    .arg(base, branch),
                kLogStep);
            const StreamedGit diff = runGitStreaming(
                dir,
                {QStringLiteral("diff"), QStringLiteral("--binary"),
                 base + "..." + branch},
                nullptr, log, QStringLiteral("Receiving diff"),
                kBranchDiffLongGitMs);
            attempt.out = diff.out;
            attempt.files = diff.files;
            if (!diff.ok) {
                attempt.err = diff.err;
                attempt.ok = false;
            }
        }
    };
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDeadlineMs = 12000;
    QElapsedTimer spent;
    spent.start();
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        r.ok = true;
        r.out.clear();
        r.err.clear();
        r.files = 0;
        r.attempts = attempt;
        if (attempt > 1)
            log(QStringLiteral("Attempt %1 of %2").arg(attempt).arg(kMaxAttempts),
                kLogStep);
        readDiff(r);
        if (r.ok)
            break;
        if (!isTransientGitError(r.err)) {
            log(QStringLiteral("Read failed: %1").arg(firstLineOf(r.err)),
                kLogFail);
            break;
        }
        if (attempt == kMaxAttempts || spent.hasExpired(kRetryDeadlineMs)) {
            log(QStringLiteral("Read failed after %1 attempts: %2")
                    .arg(attempt)
                    .arg(firstLineOf(r.err)),
                kLogFail);
            break;
        }
        log(QStringLiteral("Transient git error, retrying in %1 ms: %2")
                .arg(250 * attempt)
                .arg(firstLineOf(r.err)),
            kLogWarn);
        QThread::msleep(250 * attempt); // let the contention clear
    }
    r.readMs = total.elapsed();
    if (r.ok) {
        log(QStringLiteral("Read complete: %1, %2 files, %3 in %4")
                .arg(humanBytes(r.out.size()))
                .arg(r.files)
                .arg(r.workGone || work.isEmpty()
                         ? QStringLiteral("committed range")
                         : QStringLiteral("worktree snapshot"),
                     forkmesh::backgroundElapsedText(r.readMs)),
            kLogOk);
        r.text = QString::fromUtf8(r.out);
        r.fingerprints = forkmesh::ui::diffFileFingerprints(r.text);
    }
    const auto resolves = [&dir](const QString &rev) {
        return runGitCapture(
            dir,
            {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
             QStringLiteral("--quiet"), QStringLiteral("--end-of-options"),
             rev + QStringLiteral("^{commit}")},
            nullptr, nullptr);
    };
    if (!r.ok) {
        log(QStringLiteral("Checking whether %1 still exists").arg(branch),
            kLogInfo);
        if (!resolves(branch) && resolves(base)) {
            r.branchGone = true;
            log(QStringLiteral("%1 no longer names a commit; it was "
                               "probably merged and deleted")
                    .arg(branch),
                kLogWarn);
        }
    }
    return r;
}

} // namespace

void MainWindow::showBranchDiff(const QString &branch, int agentSessionId)
{
    if (!m_branchDiffView)
        return;
    // Another repository, base or pull counts as leaving too: the same branch
    // name there is a different diff, and the old one must not stay up.
    const bool leavingAnotherRange = enterBranchDiffRange(branch);
    m_branchReviewEventsKey.clear();
    if (leavingAnotherRange && !m_pullInlineBranch.isEmpty()) {
        m_pullInlineSelecting = false;
        m_pullInlineBranch.clear();
        m_pullInlinePath.clear();
        m_pullInlineSide.clear();
        m_pullInlineStartLine = 0;
        m_pullInlineEndLine = 0;
        if (m_pullInlineComposerFrame)
            m_pullInlineComposerFrame->hide();
        if (m_pullInlineCommentEdit)
            m_pullInlineCommentEdit->clear();
        m_branchDiffView->setExtraSelections({});
    }
    m_branchDiffBranch = branch;
    m_branchDiffAgentSessionId = agentSessionId;
    m_branchDiffHealthPatch.clear();
    m_branchDiffHealthBehind = -1;
    m_branchDiffHealthAhead = -1;
    m_branchDiffHealthConflict = false;
    m_branchDiffHealthWorktreeConflict = false;
    m_branchDiffHealthConflictPending = !branch.isEmpty();
    if (m_branchDiffSummaryBar)
        m_branchDiffSummaryBar->hide(); // stale scores belong to the old branch
    if (m_branchSecretBanner)
        m_branchSecretBanner->clearFindings();
    updateCommitsCompareIndicator(); // "<branch> -> <base>" on the branch row
    updateBranchDetailActions(branch);
    updateQuickAddEnterTarget(); // the prompt bar follows the branch's agent

    m_branchDiffFileSpans.clear();
    m_branchFileTops.clear();
    m_branchStickyFile.clear();
    if (m_branchDiffSticky)
        m_branchDiffSticky->hide(); // no spans yet; reappears on scroll
    m_branchDiffViewedContext.clear();
    m_branchDiffWorkDir = branchWorkDir(branch);
    m_branchDiffLastValid = false;
    cancelBranchDiffRender();
    const QString dir = repoGitDir();
    if (branch.isEmpty() || dir.isEmpty()) {
        m_branchDiffWorkDir.clear();
        m_branchDiffPendingFade = false;
        m_branchDiffView->clear();
        if (m_branchDiffLogPanel)
            m_branchDiffLogPanel->hide();
        return;
    }
    const QString base = branchCompareBase();
    if (branch == base) {
        m_branchDiffPendingFade = false;
        setBranchDiffHtml(
            QStringLiteral("<p style='color:#8b949e'>%1 is the branch being "
                           "compared against.</p>")
                .arg(branch.toHtmlEscaped()),
            /*endCap=*/false);
        return;
    }

    if (leavingAnotherRange)
        beginBranchDiffTransition(branch);

    renderBranchScopeDiff();
}

void MainWindow::showBranchDiffBranchGone(QString branch, QString base)
{
    if (branch.isEmpty())
        return;
    forgetBranchDiff(branch); // its patch describes a range that no longer exists
    if (m_branchDiffBranch != branch)
        return;
    ++m_branchScopeDiffGen; // drop a read already in flight for the dead branch
    m_branchDiffPendingFade = false;
    m_branchDiffAgentSessionId = -1;
    clearRangeFilesInSourceControl();
    showBranchDiff(QString());
    if (!m_branchDiffView)
        return;
    if (m_branchMergedFlashBranch == branch && !m_branchMergedFlashHtml.isEmpty()) {
        setBranchDiffHtml(m_branchMergedFlashHtml, /*endCap=*/false);
        return;
    }
    const QString landed =
        base.isEmpty() ? QStringLiteral("merged and deleted")
                       : QStringLiteral("merged into %1 and deleted").arg(base);
    const QString message =
        QStringLiteral("%1 was not found in this repository. It may have been "
                       "%2. Pick a branch to see its changes.")
            .arg(branch, landed);
    // The same stats the empty-range pane carries: the day's landings, per
    // agent. They are read on a worker, so the page arrives after this paint —
    // and only if nothing has been selected into the pane meanwhile.
    const int paneGen = m_branchDiffPaneGen;
    QString page = branchNoChangesPaneHtml(
        branch, base, repoGitDir(), message, /*remember=*/true,
        [this, paneGen](QString ready) {
            if (!m_branchDiffView || !m_branchDiffBranch.isEmpty() ||
                m_branchDiffPaneGen != paneGen)
                return;
            setBranchDiffHtml(ready, /*endCap=*/false);
        });
    if (!m_branchDiffView || !m_branchDiffBranch.isEmpty())
        return;
    if (page.isEmpty())
        page = QStringLiteral("<p style='color:#8b949e'>%1</p>")
                   .arg(message.toHtmlEscaped());
    setBranchDiffHtml(page, /*endCap=*/false);
}

QString MainWindow::branchDiffRangeKey(const QString &branch) const
{
    return repoGitDir() + QLatin1Char('\n') + branch + QLatin1Char('\n') +
           branchCompareBase() + QLatin1Char('\n') +
           QString::number(m_branchDiffPullNumber);
}

bool MainWindow::enterBranchDiffRange(const QString &branch)
{
    const QString key = branchDiffRangeKey(branch);
    // A pane the merged-branch flash took over has no branch but kept the key.
    const bool changed = key != m_branchDiffRangeKey || m_branchDiffBranch != branch;
    m_branchDiffRangeKey = key;
    if (changed)
        ++m_branchDiffPaneGen;
    return changed;
}

QString MainWindow::branchDiffViewedContext(const QString &branch) const
{
    return m_branchDiffPullNumber >= 0
               ? QStringLiteral("pull/") + QString::number(m_branchDiffPullNumber)
               : QStringLiteral("branch/") + branch;
}

void MainWindow::beginBranchDiffTransition(QString branch)
{
    if (!m_branchDiffView)
        return;
    m_branchDiffFilePaths.clear();
    m_branchDiffFileAnchors.clear();
    m_branchStickyLabelHtml.clear();
    m_branchActiveFile.clear();
    if (m_branchDiffActiveOutline)
        m_branchDiffActiveOutline->hide();
    if (m_branchDiffSummaryBar)
        m_branchDiffSummaryBar->hide(); // its scores belong to the range just left
    if (m_branchSecretBanner)
        m_branchSecretBanner->clearFindings();
    clearRangeFilesInSourceControl();
    setBranchDiffHtml(
        QString::fromUtf8("<p style='color:#8b949e'>Reading changes on "
                          "%1\xE2\x80\xA6</p>")
            .arg(branch.toHtmlEscaped()),
        /*endCap=*/false);
    if (QScrollBar *bar = m_branchDiffView->verticalScrollBar())
        bar->setValue(0);

    const QString key = branchDiffCacheKey(branch, m_branchDiffWorkDir);
    auto cached = m_branchDiffCache.constFind(key);
    if (!key.isEmpty() && cached == m_branchDiffCache.constEnd() &&
        !m_branchDiffWorkDir.isEmpty())
        cached = m_branchDiffCache.constFind(branchDiffCacheKey(branch, QString()));
    if (key.isEmpty() || cached == m_branchDiffCache.constEnd()) {
        m_branchDiffPaintedFromCache = false;
        m_branchDiffPendingFade = true; // real content fades in over the line above
        return;
    }
    const QByteArray patch = cached->patch;
    const QString emptyMessage = cached->emptyMessage;
    m_branchDiffPaintedFromCache = true;
    m_branchDiffPendingFade = false;
    QTimer::singleShot(0, this, [this, branch, patch, emptyMessage] {
        if (m_branchDiffBranch != branch || m_branchDiffLastValid || !m_commitsStack ||
            m_commitsStack->currentIndex() != kCommitWorkspaceRangePage)
            return;
        m_branchDiffLastPatch = patch;
        m_branchDiffLastEmpty = emptyMessage;
        m_branchDiffLastValid = true;
        appendBranchDiffLog(QStringLiteral("Painting the cached copy of this diff "
                                          "(%1) while the fresh read runs")
                                .arg(humanBytes(patch.size())));
        renderBranchDiffPatchNow(QString::fromUtf8(patch), emptyMessage,
                                 branchDiffViewedContext(branch));
    });
}

void MainWindow::finishBranchDiffTransition()
{
    if (!m_branchDiffPendingFade)
        return;
    m_branchDiffPendingFade = false;
    if (!m_branchDiffView || !m_branchDiffView->isVisible())
        return;
    auto *effect = new QGraphicsOpacityEffect(m_branchDiffView);
    m_branchDiffView->setGraphicsEffect(effect);
    auto *fade = new QPropertyAnimation(effect, "opacity", m_branchDiffView);
    fade->setDuration(140);
    fade->setStartValue(0.4);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QTextBrowser> view = m_branchDiffView;
    connect(fade, &QPropertyAnimation::finished, m_branchDiffView, [view] {
        if (view)
            view->setGraphicsEffect(nullptr);
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::prefetchBranchDiff(const QString &branch)
{
    if (branch.isEmpty() || branch == m_branchDiffBranch ||
        m_branchDiffPullNumber >= 0 || !m_branchDiffView ||
        m_branchDiffPrefetching.size() >= kBranchDiffPrefetchMaxInFlight)
        return;
    const QString dir = repoGitDir();
    const QString base = branchCompareBase();
    if (dir.isEmpty() || base.isEmpty() || branch == base)
        return;
    QString work = branchWorkDir(branch);
    if (!work.isEmpty() && !QDir(work).exists())
        work.clear();
    const QString key = branchDiffCacheKey(branch, work);
    if (key.isEmpty() || m_branchDiffCache.contains(key) ||
        m_branchDiffPrefetching.contains(key))
        return;
    m_branchDiffPrefetching.insert(key);
    runOffThread<ScopeDiff>(
        [dir, base, branch, work] {
            return readBranchScopeDiff(dir, base, branch, work, -1,
                                       [](const QString &, int) {});
        },
        [this, key, branch](ScopeDiff r) {
            m_branchDiffPrefetching.remove(key);
            if (!r.ok || r.workGone || r.branchGone ||
                branchDiffCacheKey(branch, branchWorkDir(branch)) != key)
                return;
            rememberBranchDiff(key, r.out, r.emptyMessage);
            if (m_branchDiffBranch != branch || m_branchDiffLastValid ||
                branchDiffCacheKey(branch, m_branchDiffWorkDir) != key ||
                !m_commitsStack ||
                m_commitsStack->currentIndex() != kCommitWorkspaceRangePage)
                return;
            m_branchDiffLastPatch = r.out;
            m_branchDiffLastEmpty = r.emptyMessage;
            m_branchDiffLastValid = true;
            m_branchDiffPendingFade = false;
            appendBranchDiffLog(QStringLiteral("Painting the copy read when the "
                                              "pointer rested on this branch "
                                              "(%1) while the fresh read runs")
                                    .arg(humanBytes(r.out.size())));
            renderBranchDiffPatchNow(r.text, r.emptyMessage, r.viewedContext);
        });
}

void MainWindow::warmBranchDiffs(const BranchesPanelData &data)
{
    if (m_branchDiffWarming || !m_branchesTable || !m_branchDiffView ||
        m_branchDiffPullNumber >= 0 || data.dir.isEmpty() ||
        data.base.isEmpty() || data.dir != repoGitDir() ||
        data.base != branchCompareBase())
        return;
    const QSet<QString> local(data.branches.cbegin(), data.branches.cend());
    QList<QPair<QString, QString>> wanted;
    for (int row = 0; row < m_branchesTable->rowCount() &&
                      wanted.size() < kBranchDiffWarmRows;
         ++row) {
        const QTableWidgetItem *name = m_branchesTable->item(row, kBranchesNameColumn);
        const QString branch = name ? name->text() : QString();
        if (branch.isEmpty() || branch == data.base || !local.contains(branch))
            continue;
        const auto counts = data.localAheadBehind.constFind(branch);
        if (counts != data.localAheadBehind.constEnd() && counts->first == 0)
            continue;
        const QString key = branchDiffCacheKey(branch, QString());
        const QString tip = data.shortShas.value(branch);
        if (key.isEmpty() ||
            (m_branchDiffCache.contains(key) && m_branchDiffWarmTips.value(key) == tip))
            continue;
        wanted.append({branch, tip});
    }
    if (wanted.isEmpty())
        return;
    m_branchDiffWarming = true;
    const QString dir = data.dir;
    const QString base = data.base;
    struct Warmed {
        QString branch;
        QString tip;
        QByteArray patch;
        QString emptyMessage;
    };
    runOffThread<QList<Warmed>>(
        [dir, base, wanted] {
            QList<Warmed> out;
            for (const auto &[branch, tip] : wanted) {
                const ScopeDiff r = readBranchScopeDiff(
                    dir, base, branch, QString(), -1, [](const QString &, int) {});
                if (r.ok)
                    out.append({branch, tip, r.out, r.emptyMessage});
            }
            return out;
        },
        [this, dir, base](QList<Warmed> warmed) {
            m_branchDiffWarming = false;
            if (dir != repoGitDir() || base != branchCompareBase() ||
                m_branchDiffPullNumber >= 0)
                return;
            if (m_branchDiffWarmTips.size() > 4 * kBranchDiffCacheEntries)
                m_branchDiffWarmTips.clear();
            for (const Warmed &w : warmed) {
                const QString key = branchDiffCacheKey(w.branch, QString());
                if (w.branch == m_branchDiffBranch && m_branchDiffCache.contains(key))
                    continue;
                rememberBranchDiff(key, w.patch, w.emptyMessage);
                m_branchDiffWarmTips.insert(key, w.tip);
            }
        });
}

QString MainWindow::branchDiffCacheKey(const QString &branch,
                                       const QString &workDir) const
{
    if (branch.isEmpty() || m_branchDiffPullNumber >= 0)
        return QString();
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return QString();
    return dir + QLatin1Char('\n') + branch + QLatin1Char('\n') +
           branchCompareBase() + QLatin1Char('\n') + workDir;
}

void MainWindow::rememberBranchDiff(const QString &key, const QByteArray &patch,
                                    const QString &emptyMessage)
{
    if (key.isEmpty())
        return;
    const auto drop = [this](QString dead) {
        const auto it = m_branchDiffCache.constFind(dead);
        if (it == m_branchDiffCache.constEnd())
            return;
        m_branchDiffCacheBytes -= it->patch.size();
        m_branchDiffCache.erase(it);
        m_branchDiffCacheOrder.removeAll(dead);
    };
    drop(key); // re-cached: replace the old copy rather than count it twice
    if (patch.size() > kBranchDiffCacheBytes)
        return;
    m_branchDiffCache.insert(key, {patch, emptyMessage});
    m_branchDiffCacheOrder.append(key);
    m_branchDiffCacheBytes += patch.size();
    while (!m_branchDiffCacheOrder.isEmpty() &&
           (m_branchDiffCacheOrder.size() > kBranchDiffCacheEntries ||
            m_branchDiffCacheBytes > kBranchDiffCacheBytes))
        drop(m_branchDiffCacheOrder.first());
}

void MainWindow::forgetBranchDiff(const QString &branch)
{
    if (branch.isEmpty())
        return;
    const QString prefix =
        repoGitDir() + QLatin1Char('\n') + branch + QLatin1Char('\n');
    const QStringList keys = m_branchDiffCacheOrder;
    for (const QString &key : keys) {
        if (!key.startsWith(prefix))
            continue;
        const auto it = m_branchDiffCache.constFind(key);
        if (it != m_branchDiffCache.constEnd()) {
            m_branchDiffCacheBytes -= it->patch.size();
            m_branchDiffCache.erase(it);
        }
        m_branchDiffCacheOrder.removeAll(key);
    }
}

void MainWindow::clearBranchDiffCache()
{
    m_branchDiffCache.clear();
    m_branchDiffCacheOrder.clear();
    m_branchDiffCacheBytes = 0;
    m_branchDiffWarmTips.clear();
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testBranchDiffText() const
{
    return m_branchDiffView ? m_branchDiffView->toPlainText() : QString();
}

QString MainWindow::testBranchDiffLogText() const
{
    return m_branchDiffLog ? m_branchDiffLog->toPlainText() : QString();
}

QString MainWindow::testBranchDiffLogButtonText() const
{
    return m_branchDiffLogButton && !m_branchDiffLogButton->isHidden()
               ? m_branchDiffLogButton->text()
               : QString();
}

bool MainWindow::testBranchDiffLogPanelShown() const
{
    return m_branchDiffLogPanel && !m_branchDiffLogPanel->isHidden();
}

void MainWindow::testToggleBranchDiffLog()
{
    if (m_branchDiffLogButton)
        m_branchDiffLogButton->click();
}

bool MainWindow::testScrollBranchDiffViewportToFile(const QString &path)
{
    const int index = m_branchDiffFilePaths.indexOf(path);
    if (!m_branchDiffView || index < 0 ||
        index >= m_branchDiffFileAnchors.size())
        return false;
    scrollDiffToAnchor(m_branchDiffView, m_branchDiffFileAnchors.at(index));
    updateBranchDiffSticky();
    updateBranchDiffActiveOutline();
    return m_branchActiveFile == path;
}

bool MainWindow::testBranchDiffCached(const QString &branch) const
{
    const QString key = branchDiffCacheKey(branch, branchWorkDir(branch));
    return !key.isEmpty() && m_branchDiffCache.contains(key);
}
#endif

void MainWindow::renderBranchScopeDiff()
{
    if (!m_branchDiffView)
        return;
    const QString branch = m_branchDiffBranch;
    const QString dir = repoGitDir();
    if (branch.isEmpty() || dir.isEmpty())
        return;
    const QString base = branchCompareBase();
    // The path is normally captured when the branch is selected, but an agent
    // can recreate or relocate its worktree while this pane remains open.
    // Re-resolve it before every read so a repair/retry uses the live worktree.
    const QString liveWork = branchWorkDir(branch);
    if (!liveWork.isEmpty())
        m_branchDiffWorkDir = liveWork;
    // The worktree can go away under a range that is already on screen: merging
    // the branch removes it, and so does a wipe of the worktree root. Reading in
    // a directory that is gone only yields git's "cannot change to '<dir>'", so
    // fall back to the committed range instead of failing the pane.
    if (!m_branchDiffWorkDir.isEmpty() && !QDir(m_branchDiffWorkDir).exists())
        m_branchDiffWorkDir.clear();
    const QString work = m_branchDiffWorkDir;

    const int pullNumber = m_branchDiffPullNumber;
    const int agentSessionId = m_branchDiffAgentSessionId;
    const QString cacheKey = branchDiffCacheKey(branch, work);
    const int gen = ++m_branchScopeDiffGen;
    m_branchScopeDiffStartGen = gen;
    m_branchScopeDiffStartSeq = ++m_branchDiffSeq;

    // Every step of this read narrates itself into the pane's activity log:
    // the git commands as they start and finish, the diff as it streams in,
    // retries, fallbacks, and then the render and layout that follow.
    beginBranchDiffLog(QStringLiteral("Reading changes on %1").arg(branch));
    const int logGen = m_branchDiffLogGen;
    const BranchDiffLogFn log = branchDiffLogSink(logGen);
    appendBranchDiffLog(QStringLiteral("Comparing %1 against %2").arg(branch, base),
                        BranchDiffLogKind::Step);
    appendBranchDiffLog(QStringLiteral("Repository: %1").arg(dir));
    if (!work.isEmpty())
        appendBranchDiffLog(
            QStringLiteral("Worktree: %1 (its uncommitted and untracked files "
                           "count as changes)")
                .arg(work));
    else
        appendBranchDiffLog(QStringLiteral("No live worktree for this branch: "
                                          "reading the committed range only"));
    if (m_branchDiffPaintedFromCache)
        appendBranchDiffLog(QStringLiteral("A cached copy of this diff stays on "
                                          "screen while the fresh read runs"));

    // Named so every git ticket the worker opens carries this load's routine,
    // which is how noteBranchDiffTicket() knows to echo it into the log.
    const forkmesh::BackgroundRoutine routineScope(m_branchDiffLogRoutine);
    if (!work.isEmpty() && pullNumber < 0 && !m_branchDiffLastValid &&
        !m_branchDiffPaintedFromCache) {
        appendBranchDiffLog(QStringLiteral("Reading the committed changes too, "
                                          "so they paint before the worktree "
                                          "snapshot lands"));
        runOffThread<ScopeDiff>(
            [dir, base, branch, log] {
                return readBranchScopeDiff(dir, base, branch, QString(), -1, log);
            },
            [this, gen, branch](ScopeDiff r) {
                if (!r.ok || r.out.isEmpty() || gen != m_branchScopeDiffGen ||
                    !m_branchDiffView || m_branchDiffBranch != branch ||
                    m_branchDiffLastValid)
                    return;
                m_branchDiffLastPatch = r.out;
                m_branchDiffLastEmpty = r.emptyMessage;
                m_branchDiffLastValid = true;
                appendBranchDiffLog(
                    QStringLiteral("Painting the committed changes (%1 files) "
                                   "while the worktree snapshot is read; "
                                   "uncommitted files join when it lands")
                        .arg(r.files),
                    BranchDiffLogKind::Step);
                renderBranchDiffPatch(
                    r.text, r.emptyMessage, r.viewedContext,
                    [this, gen] {
                        if (gen != m_branchScopeDiffGen)
                            return;
                        finishBranchDiffTransition();
                        flushBranchDiffAfterRead();
                    },
                    std::move(r.fingerprints));
            });
    }
    runOffThread<ScopeDiff>(
        [dir, base, branch, work, pullNumber, log]() {
            return readBranchScopeDiff(dir, base, branch, work, pullNumber, log);
        },
        [this, gen, branch, base, agentSessionId, cacheKey](ScopeDiff r) {
            if (gen != m_branchScopeDiffGen || !m_branchDiffView ||
                m_branchDiffBranch != branch)
                return;
            ++m_branchDiffSeq; // a branch-list load asked for after this re-reads
            if (r.branchGone) {
                appendBranchDiffLog(QStringLiteral("Showing the branch-gone page"),
                                    BranchDiffLogKind::Info);
                showBranchDiffBranchGone(branch, base);
                flushBranchDiffAfterRead();
                return;
            }
            if (r.workGone) // this read fell back; later ones start there
                m_branchDiffWorkDir.clear();
            if (!r.ok) {
                m_branchDiffPendingFade = false;
                appendBranchDiffLog(QStringLiteral("Showing the error page"),
                                    BranchDiffLogKind::Fail);
                setBranchDiffHtml(
                    branchDiffErrorHtml(branch, r.err, r.attempts),
                    /*endCap=*/false);
                flushBranchDiffAfterRead();
                return;
            }
            const bool alreadyShown = m_branchDiffLastValid &&
                                      m_branchDiffLastPatch == r.out &&
                                      m_branchDiffViewedContext == r.viewedContext;
            m_branchDiffLastPatch = r.out;
            m_branchDiffLastEmpty = r.emptyMessage;
            m_branchDiffLastValid = true;
            if (!r.workGone) { // the key names a worktree this patch didn't come from
                rememberBranchDiff(cacheKey, r.out, r.emptyMessage);
                if (!cacheKey.isEmpty())
                    appendBranchDiffLog(
                        r.out.size() > kBranchDiffCacheBytes
                            ? QStringLiteral("Patch is larger than the %1 cache "
                                             "limit; the next visit re-reads it")
                                  .arg(humanBytes(kBranchDiffCacheBytes))
                            : QStringLiteral("Patch cached for the next visit to "
                                             "this branch"));
            }
            const auto finished = [this, gen, agentSessionId] {
                if (gen != m_branchScopeDiffGen)
                    return;
                finishBranchDiffTransition();
                flushBranchDiffAfterRead();
                syncBranchDiffAgentStat(agentSessionId);
            };
            if (alreadyShown) {
                appendBranchDiffLog(QStringLiteral("The diff on screen already "
                                                  "matches this read; nothing to "
                                                  "redraw"),
                                    BranchDiffLogKind::Ok);
                finished();
                return;
            }
            renderBranchDiffPatch(r.text, r.emptyMessage, r.viewedContext,
                                  finished, std::move(r.fingerprints));
        });
}

namespace {

constexpr qsizetype kBranchDiffSyncRenderChars = 192 * 1024;

bool branchDiffRenderNeedsWorker(const QString &patch)
{
    return patch.size() > kBranchDiffSyncRenderChars ||
           patch.contains(QLatin1String("GIT binary patch")) ||
           patch.contains(QLatin1String("\nBinary files "));
}

QString branchDiffPatchIdentity(const QString &patch)
{
    constexpr qsizetype kEdge = 64 * 1024;
    const QStringView all(patch);
    return QString::number(all.size()) + QLatin1Char(':') +
           QString::number(qHash(all.left(kEdge))) + QLatin1Char(':') +
           QString::number(qHash(all.right(kEdge)));
}

int countDiffFiles(const QString &patch)
{
    int n = patch.startsWith(QLatin1String("diff --git ")) ? 1 : 0;
    static const QLatin1String marker("\ndiff --git ");
    for (qsizetype at = 0; (at = patch.indexOf(marker, at)) >= 0; ++at)
        ++n;
    return n;
}

QString sortedJoined(const QStringList &list)
{
    QStringList sorted = list;
    sorted.sort();
    return sorted.join(QLatin1Char('\x1e'));
}

} // namespace

void MainWindow::renderBranchDiffPatch(
    const QString &patch, const QString &emptyMessage,
    const QString &viewedContext, std::function<void()> done,
    std::optional<QHash<QString, QString>> fingerprints)
{
    renderBranchDiffPatchOn(patch, emptyMessage, viewedContext, std::move(done),
                            /*guiThread=*/false, std::move(fingerprints));
}

void MainWindow::renderBranchDiffPatchNow(const QString &patch,
                                          const QString &emptyMessage,
                                          const QString &viewedContext)
{
    constexpr qsizetype kInlineRenderChars = 256 * 1024;
    renderBranchDiffPatchOn(patch, emptyMessage, viewedContext, {},
                            /*guiThread=*/patch.size() <= kInlineRenderChars);
}

void MainWindow::renderBranchDiffPatchOn(
    const QString &patch, const QString &emptyMessage,
    const QString &viewedContext, std::function<void()> done, bool guiThread,
    std::optional<QHash<QString, QString>> fingerprints)
{
    if (!m_branchDiffView)
        return;
    const QString message = emptyMessage;
    m_branchDiffViewedContext = viewedContext;
    const QString dir = m_branchDiffWorkDir.isEmpty() ? repoGitDir()
                                                       : m_branchDiffWorkDir;
    const QString base = branchCompareBase();
    const QString head = m_branchDiffBranch;
    QHash<QString, QString> notes;
    QString anchorFile;
    if (m_branchDiffPullNumber >= 0) {
        for (const PullRequest &p : std::as_const(m_currentPulls)) {
            if (p.number == m_branchDiffPullNumber) {
                notes = buildPullLineNotes(p);
                anchorFile = QStringLiteral("*");
                break;
            }
        }
    } else if (branchDiffTakesBranchComments() && !patch.isEmpty()) {
        notes = buildPullLineNotes(branchReviewPull(patch));
        anchorFile = QStringLiteral("*");
    }
    const bool comments = !anchorFile.isEmpty();
    const QString scope = diffViewedScope(viewedContext);
    const QHash<QString, QString> stamps =
        scope.isEmpty() ? QHash<QString, QString>()
                        : loadDiffViewedStamps(viewedContext);
    const QSet<QString> expanded = diffExpandedViewed(viewedContext);
    const bool split = diffSplitPref();
    const DiffRenderAppearance appearance = currentDiffRenderAppearance();

    QStringList noteKeys = notes.keys();
    noteKeys.sort();
    QString notesKey;
    for (const QString &k : std::as_const(noteKeys))
        notesKey += k + QLatin1Char('\x1e') + notes.value(k) + QLatin1Char('\x1d');
    const QString identity = branchDiffPatchIdentity(patch);
    const QString renderKey =
        identity + QLatin1Char('\x1f') + viewedContext + QLatin1Char('\x1f') +
        anchorFile + QLatin1Char('\x1f') + (split ? QLatin1Char('1') : QLatin1Char('0')) +
        (appearance.dark ? QLatin1Char('1') : QLatin1Char('0')) +
        QString::number(appearance.dpr) + QLatin1Char('\x1f') +
        sortedJoined(stamps.keys()) + QLatin1Char('\x1f') +
        sortedJoined(QStringList(expanded.cbegin(), expanded.cend())) +
        QLatin1Char('\x1f') + notesKey;
    if (!m_branchDiffRenderKey.isEmpty() && renderKey == m_branchDiffRenderKey) {
        appendBranchDiffLog(QStringLiteral("The diff on screen was drawn from these "
                                          "same inputs; nothing to redraw"),
                            BranchDiffLogKind::Ok);
        if (done)
            done();
        return;
    }

    const int gen = ++m_branchDiffRenderGen;
    if (m_branchDiffRenderCancel)
        m_branchDiffRenderCancel->store(true, std::memory_order_release);
    m_branchDiffRenderCancel.reset();
    if (patch.isEmpty())
        appendBranchDiffLog(QStringLiteral("Nothing to render: the patch is empty"));

    const QString healthKey = branchDiffHealthInputKey();
    const int behind = m_branchDiffHealthBehind;
    const int ahead = m_branchDiffHealthAhead;
    const bool conflict = m_branchDiffHealthConflict;
    const bool worktreeConflict = m_branchDiffHealthWorktreeConflict;
    const bool conflictPending = m_branchDiffHealthConflictPending;
    const auto build = [patch, dir, base, head, anchorFile, notes, scope, stamps,
                        expanded, split, appearance, healthKey, behind, ahead,
                        conflict, worktreeConflict, conflictPending,
                        fingerprints](DiffRenderSink *sink, bool wantHealth) {
        BranchDiffRender out;
        if (!scope.isEmpty()) {
            out.fingerprints =
                fingerprints ? *fingerprints : diffFileFingerprints(patch);
            out.prune = pruneDiffViewedStamps(stamps, out.fingerprints);
            out.viewed = QSet<QString>(out.prune.stamps.keyBegin(),
                                       out.prune.stamps.keyEnd());
        }
        out.expanded = expanded;
        for (const QString &path : std::as_const(out.prune.dropped))
            out.expanded.remove(path);
        out.html = renderDiffHtmlSplit(split, patch, out.files, dir, base, head,
                                       anchorFile, notes, out.viewed, out.expanded,
                                       appearance, sink);
        out.aborted = sink && sink->aborted;
        if (wantHealth && !out.aborted) {
            out.health = diffHealthReport(patch, out.files, behind, ahead, conflict,
                                          worktreeConflict, conflictPending);
            out.hasHealth = true;
            out.healthKey = healthKey;
        }
        return out;
    };

    if (guiThread || !branchDiffRenderNeedsWorker(patch)) {
        QElapsedTimer clock;
        clock.start();
        const BranchDiffRender out = build(nullptr, /*wantHealth=*/false);
        if (!patch.isEmpty())
            appendBranchDiffLog(QStringLiteral("Rendered %1 files from %2 of patch "
                                              "on the GUI thread in %3")
                                    .arg(out.files.size())
                                    .arg(humanBytes(patch.size()))
                                    .arg(forkmesh::backgroundElapsedText(
                                        clock.elapsed())),
                                BranchDiffLogKind::Ok);
        applyBranchDiffRender(out, patch, message, viewedContext, comments,
                              renderKey, /*progressive=*/false);
        if (done)
            done();
        return;
    }

    appendBranchDiffLog(QStringLiteral("Rendering %1 of patch as %2 HTML on a "
                                      "worker; files paint as they are built")
                            .arg(humanBytes(patch.size()))
                            .arg(split ? QStringLiteral("side-by-side")
                                       : QStringLiteral("unified")),
                        BranchDiffLogKind::Step);
    m_branchDiffFileSpans.clear();
    m_branchDiffRenderKey.clear();
    const QString placeholder =
        m_branchDiffShownIdentity == identity
            ? QString()
            : QString::fromUtf8(
                  "<p style='color:#8b949e'>Rendering changes\xE2\x80\xA6</p>");
    m_branchDiffShownIdentity.clear();
    beginDiffStreamIncremental(m_branchDiffView, diffStyleSheet(m_diffFontPt),
                               placeholder, /*endCap=*/true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_branchDiffRenderCancel = cancel;
    const BranchDiffLogFn log = branchDiffLogSink(m_branchDiffLogGen);
    const forkmesh::BackgroundRoutine routineScope(m_branchDiffLogRoutine);
    runOffThread<BranchDiffRender>(
        [this, build, cancel, gen, patch, comments, appearance, log] {
            QElapsedTimer clock;
            clock.start();
            const int filesTotal = countDiffFiles(patch);
            QStringList blocks;
            DiffRenderSink sink;
            sink.deliver = [this, cancel, gen, filesTotal,
                            &blocks](const QString &chunk, int filesDone) {
                if (cancel->load(std::memory_order_acquire) ||
                    !m_acceptOffThreadResults.load(std::memory_order_acquire))
                    return false;
                blocks += splitDiffFileBlocks(chunk);
                QList<QStringList> pages = paginateDiffStreamChunk(chunk);
                QMetaObject::invokeMethod(
                    this,
                    [this, gen, pages = std::move(pages), filesDone, filesTotal] {
                        deliverOffThreadResult<int>(
                            [this, gen, pages, filesDone, filesTotal](int) {
                                applyBranchDiffChunk(gen, pages, filesDone,
                                                     filesTotal);
                            },
                            0);
                    },
                    Qt::QueuedConnection);
                return true;
            };
            BranchDiffRender out = build(&sink, /*wantHealth=*/true);
            if (out.aborted)
                return out;
            out.blocks = buildDiffBlockCache(blocks, out.files, out.viewed,
                                             out.expanded, comments, appearance);
            log(QStringLiteral("Rendered and scored %1 files on the worker in %2")
                    .arg(out.files.size())
                    .arg(forkmesh::backgroundElapsedText(clock.elapsed())),
                kLogOk);
            return out;
        },
        [this, gen, patch, message, viewedContext, comments, renderKey,
         done](BranchDiffRender out) {
            if (gen != m_branchDiffRenderGen || out.aborted || !m_branchDiffView)
                return;
            m_branchDiffRenderCancel.reset();
            applyBranchDiffRender(out, patch, message, viewedContext, comments,
                                  renderKey, /*progressive=*/true);
            if (done)
                done();
        });
}

void MainWindow::applyBranchDiffChunk(int gen, const QList<QStringList> &pages,
                                      int filesDone, int filesTotal)
{
    if (gen != m_branchDiffRenderGen || !m_branchDiffView)
        return;
    appendDiffStreamPages(m_branchDiffView, pages);
    appendBranchDiffLog(QStringLiteral("Streamed %1 of %2 files into the pane")
                            .arg(QLocale().toString(filesDone),
                                 QLocale().toString(filesTotal)),
                        BranchDiffLogKind::Progress);
    finishBranchDiffTransition();
}

void MainWindow::applyBranchDiffRender(const BranchDiffRender &render,
                                       const QString &patch,
                                       const QString &message,
                                       const QString &viewedContext,
                                       bool comments, const QString &renderKey,
                                       bool progressive)
{
    if (!m_branchDiffView)
        return;
    if (!render.fingerprints.isEmpty())
        refreshDiffViewedForFingerprints(viewedContext, render.fingerprints);
    const QList<DiffFileEntry> &files = render.files;
    const QSet<QString> &viewed = render.viewed;
    const bool rendered = progressive ? !files.isEmpty() : !render.html.isEmpty();
    const QString base = branchCompareBase();
    m_branchDiffFilePaths.clear();
    m_branchDiffFileAnchors.clear();
    m_branchStickyLabelHtml.clear();
    QStringList rangeStatuses;
    for (const DiffFileEntry &f : files) {
        m_branchDiffFilePaths.append(f.path);
        m_branchDiffFileAnchors.append(f.anchor);
        rangeStatuses.append(f.status);
        m_branchStickyLabelHtml.insert(f.path,
                                       diffFileLabelHtml(f, viewed.contains(f.path)));
    }
    showRangeFilesInSourceControl(m_branchDiffFilePaths, rangeStatuses);
    if (render.hasHealth) {
        m_branchDiffHealthReport = std::make_shared<DiffHealthReport>(render.health);
        m_branchDiffHealthReportKey = render.healthKey;
        m_branchDiffHealthReportPatch = patch;
    }
    updateBranchDiffSummary(files, viewed, patch, viewedContext);

    QElapsedTimer clock;
    clock.start();
    if (!rendered) {
        appendBranchDiffLog(QStringLiteral("No changes to show; composing the "
                                          "empty-range page"));
        const QString branch = m_branchDiffBranch;
        const int paneGen = m_branchDiffPaneGen;
        QString page = branchNoChangesPaneHtml(
            branch, base, repoGitDir(), message, /*remember=*/true,
            [this, branch, paneGen](QString ready) {
                if (!m_branchDiffView || m_branchDiffBranch != branch ||
                    m_branchDiffPaneGen != paneGen)
                    return;
                setBranchDiffHtml(ready, /*endCap=*/false);
            });
        if (page.isEmpty())
            page = QStringLiteral("<p style='color:#8b949e'>%1</p>")
                       .arg(message.toHtmlEscaped());
        setBranchDiffHtml(page, /*endCap=*/false);
    } else {
        m_branchDiffLogAwaitingLayout = true;
        if (progressive) {
            appendBranchDiffLog(QStringLiteral("Streamed all %1 files")
                                    .arg(QLocale().toString(files.size())),
                                BranchDiffLogKind::ProgressDone);
            finishDiffStreamIncremental(m_branchDiffView);
            noteDiffFileBlocks(m_branchDiffView, render.blocks);
        } else {
            appendBranchDiffLog(
                QStringLiteral("Laying out %1 characters of HTML: the first page "
                               "paints now and the rest streams in bounded pages "
                               "as you scroll")
                    .arg(render.html.size()),
                BranchDiffLogKind::Step);
            setDiffHtml(m_branchDiffView, render.html, /*endCap=*/true);
            noteDiffFileBlocks(m_branchDiffView, render.html, files, viewed,
                               render.expanded, comments);
        }
        m_branchDiffRenderKey = renderKey;
        m_branchDiffShownIdentity = renderKey.section(QLatin1Char('\x1f'), 0, 0);
    }

    m_branchFileTops.clear();
    m_branchStickyFile.clear();
    if (!m_branchDiffFilePaths.contains(m_branchActiveFile))
        m_branchActiveFile.clear();
    rebuildBranchDiffSpans();
    updateBranchDiffActiveOutline();
    if (m_branchDiffSearchBar && m_branchDiffSearchBar->isVisible())
        branchDiffSearchRecompute();
    if (rendered) {
        appendBranchDiffLog(QStringLiteral("File outline built for %1 files in %2")
                                .arg(files.size())
                                .arg(forkmesh::backgroundElapsedText(clock.elapsed())));
        appendBranchDiffLog(
            QStringLiteral("First page on screen: %1 files, %2 since the load began")
                .arg(files.size())
                .arg(forkmesh::backgroundElapsedText(
                    m_branchDiffLogClock.isValid() ? m_branchDiffLogClock.elapsed()
                                                   : 0)),
            BranchDiffLogKind::Ok);
    }
}

void MainWindow::cancelBranchDiffRender()
{
    ++m_branchDiffRenderGen;
    if (m_branchDiffRenderCancel) {
        m_branchDiffRenderCancel->store(true, std::memory_order_release);
        m_branchDiffRenderCancel.reset();
    }
    m_branchDiffRenderKey.clear();
    m_branchDiffShownIdentity.clear();
}

void MainWindow::setBranchDiffHtml(const QString &html, bool endCap)
{
    if (!m_branchDiffView)
        return;
    cancelBranchDiffRender();
    setDiffHtml(m_branchDiffView, html, endCap);
}

void MainWindow::syncBranchDiffAgentStat(int agentSessionId)
{
    if (agentSessionId <= 0 || m_branchDiffAgentSessionId != agentSessionId)
        return;
    AgentDiffStat stat = m_agentDiffStats.value(agentSessionId);
    if (stat.files == m_branchDiffFilePaths.size())
        return;
    stat.files = m_branchDiffFilePaths.size();
    m_agentDiffStats.insert(agentSessionId, stat);
    m_agentDiffSig.remove(agentSessionId);
    refreshAgentTable();
}

QString MainWindow::branchDiffHealthInputKey() const
{
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    return QStringLiteral("%1/%2/%3/%4/%5/%6")
        .arg(m_branchDiffHealthBehind)
        .arg(m_branchDiffHealthAhead)
        .arg(m_branchDiffHealthConflict ? 1 : 0)
        .arg(m_branchDiffHealthWorktreeConflict ? 1 : 0)
        .arg(m_branchDiffHealthConflictPending ? 1 : 0)
        .arg(dark ? 1 : 0);
}

QWidget *MainWindow::buildBranchDiffLogPanel()
{
    static_assert(int(BranchDiffLogKind::Step) == kLogStep &&
                      int(BranchDiffLogKind::Info) == kLogInfo &&
                      int(BranchDiffLogKind::Git) == kLogGit &&
                      int(BranchDiffLogKind::Progress) == kLogProgress &&
                      int(BranchDiffLogKind::Ok) == kLogOk &&
                      int(BranchDiffLogKind::Warn) == kLogWarn &&
                      int(BranchDiffLogKind::Fail) == kLogFail &&
                      int(BranchDiffLogKind::ProgressDone) == kLogProgressDone,
                  "BranchDiffLogKind and the worker-side kinds must agree");
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString bg = dark ? QStringLiteral("#0d1117") : QStringLiteral("#f6f8fa");
    const QString border = dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de");
    const QString fg = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    const QString handle = dark ? QStringLiteral("#484f58") : QStringLiteral("#d0d7de");

    m_branchDiffLogPanel = new QWidget;
    m_branchDiffLogPanel->setObjectName(QStringLiteral("branchDiffLogPanel"));
    auto *outer = new QVBoxLayout(m_branchDiffLogPanel);
    outer->setContentsMargins(0, 0, 12, 0);
    outer->setSpacing(0);
    auto *frame = new QFrame(m_branchDiffLogPanel);
    frame->setObjectName(QStringLiteral("branchDiffLogFrame"));
    frame->setStyleSheet(Theme::remapSheet(
        QStringLiteral("#branchDiffLogFrame{background:%1;border:1px solid %2;"
                       "border-radius:6px;}"
                       "#branchDiffLogFrame QLabel{background:transparent;}")
            .arg(bg, border)));
    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(10, 4, 6, 4);
    frameLayout->setSpacing(2);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(8);
    m_branchDiffLogTitle = new QLabel(frame);
    m_branchDiffLogTitle->setObjectName(QStringLiteral("branchDiffLogTitle"));
    m_branchDiffLogTitle->setTextFormat(Qt::RichText);
    m_branchDiffLogTitle->setTextInteractionFlags(Qt::NoTextInteraction);
    m_branchDiffLogTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_branchDiffLogTitle->setToolTip(
        QStringLiteral("Everything the pane is doing to read, render and lay out "
                       "this branch's changes, as it happens: each git command, "
                       "the diff streaming in, the render and the layout."));
    head->addWidget(m_branchDiffLogTitle, 1);
    m_branchDiffLogToggle = new QPushButton(frame);
    m_branchDiffLogToggle->setObjectName(QStringLiteral("ghostButton"));
    m_branchDiffLogToggle->setProperty("buttonSize", "sm");
    m_branchDiffLogToggle->setCursor(Qt::PointingHandCursor);
    m_branchDiffLogToggle->setAccessibleName(QStringLiteral("Toggle activity log"));
    connect(m_branchDiffLogToggle, &QPushButton::clicked, this,
            [this] { setBranchDiffLogCollapsed(!m_branchDiffLogCollapsed); });
    head->addWidget(m_branchDiffLogToggle, 0);
    frameLayout->addLayout(head);

    m_branchDiffLog = new QTextEdit(frame);
    m_branchDiffLog->setObjectName(QStringLiteral("branchDiffLog"));
    m_branchDiffLog->setAccessibleName(QStringLiteral("Branch changes activity log"));
    m_branchDiffLog->setReadOnly(true);
    m_branchDiffLog->setFrameShape(QFrame::NoFrame);
    m_branchDiffLog->document()->setDocumentMargin(2);
    m_branchDiffLog->setLineWrapMode(QTextEdit::NoWrap);
    m_branchDiffLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_branchDiffLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_branchDiffLog->setFixedHeight(150);
    m_branchDiffLog->setStyleSheet(Theme::remapSheet(
        QStringLiteral("QTextEdit#branchDiffLog{color:%1;border:none;background:"
                       "transparent;font-family:monospace;font-size:11px;}"
                       "QTextEdit#branchDiffLog QScrollBar:vertical{background:"
                       "transparent;width:9px;margin:0;}"
                       "QTextEdit#branchDiffLog QScrollBar::handle:vertical{"
                       "background:%2;border-radius:4px;min-height:24px;}"
                       "QTextEdit#branchDiffLog QScrollBar::add-line:vertical,"
                       "QTextEdit#branchDiffLog QScrollBar::sub-line:vertical{"
                       "height:0;}"
                       "QTextEdit#branchDiffLog QScrollBar::add-page:vertical,"
                       "QTextEdit#branchDiffLog QScrollBar::sub-page:vertical{"
                       "background:transparent;}"
                       "QTextEdit#branchDiffLog QScrollBar:horizontal{background:"
                       "transparent;height:9px;margin:0;}"
                       "QTextEdit#branchDiffLog QScrollBar::handle:horizontal{"
                       "background:%2;border-radius:4px;min-width:24px;}"
                       "QTextEdit#branchDiffLog QScrollBar::add-line:horizontal,"
                       "QTextEdit#branchDiffLog QScrollBar::sub-line:horizontal{"
                       "width:0;}"
                       "QTextEdit#branchDiffLog QScrollBar::add-page:horizontal,"
                       "QTextEdit#branchDiffLog QScrollBar::sub-page:horizontal{"
                       "background:transparent;}")
            .arg(fg, handle)));
    frameLayout->addWidget(m_branchDiffLog);
    outer->addWidget(frame);

    m_branchDiffLogCollapsed = true;
    m_branchDiffLogToggle->setText(QStringLiteral("Hide log"));
    m_branchDiffLogPanel->hide();
    return m_branchDiffLogPanel;
}

void MainWindow::setBranchDiffLogCollapsed(bool collapsed)
{
    m_branchDiffLogCollapsed = collapsed;
    if (m_branchDiffLogPanel)
        m_branchDiffLogPanel->setVisible(
            !collapsed && m_branchDiffLog && !m_branchDiffLog->document()->isEmpty());
    if (!collapsed && m_branchDiffLog) {
        if (QScrollBar *bar = m_branchDiffLog->verticalScrollBar())
            bar->setValue(bar->maximum());
    }
    updateBranchDiffLogButton();
}

void MainWindow::updateBranchDiffLogButton(const QString &line)
{
    if (!m_branchDiffLogButton)
        return;
    const qint64 ms = m_branchDiffLogTookMs >= 0 ? m_branchDiffLogTookMs
                      : m_branchDiffLogClock.isValid()
                          ? m_branchDiffLogClock.elapsed()
                          : 0;
    const QString chevron = m_branchDiffLogCollapsed
                                ? QString::fromUtf8("\xE2\x96\xB8")
                                : QString::fromUtf8("\xE2\x96\xBE");
    m_branchDiffLogButton->setText(QStringLiteral("%1 Activity %2")
                                       .arg(chevron,
                                            forkmesh::backgroundElapsedText(ms)));
    m_branchDiffLogButton->setChecked(!m_branchDiffLogCollapsed);
    if (!line.isEmpty())
        m_branchDiffLogButton->setProperty("lastLine", line);
    const QString last = m_branchDiffLogButton->property("lastLine").toString();
    m_branchDiffLogButton->setToolTip(
        QStringLiteral("%1 %2\n%3")
            .arg(m_branchDiffLogTookMs >= 0 ? QStringLiteral("Loaded in")
                                            : QStringLiteral("Loading for"),
                 forkmesh::backgroundElapsedText(ms),
                 m_branchDiffLogCollapsed
                     ? QStringLiteral("Click to show the activity log")
                     : QStringLiteral("Click to hide the activity log")) +
        (last.isEmpty() ? QString() : QStringLiteral("\n") + last));
    m_branchDiffLogButton->setVisible(m_branchDiffLog &&
                                      !m_branchDiffLog->document()->isEmpty());
}

void MainWindow::beginBranchDiffLog(const QString &title)
{
    ++m_branchDiffLogGen;
    m_branchDiffLogRoutine =
        QStringLiteral("%1 #%2").arg(title).arg(m_branchDiffLogGen);
    m_branchDiffLogClock.start();
    m_branchDiffLogLastWasProgress = false;
    m_branchDiffLogAwaitingLayout = false;
    m_branchDiffLogTookMs = -1;
    // The same range read again (after a commit, a merge, a repair) continues
    // the log under a rule; only a different load starts it afresh.
    const bool sameLoad =
        m_branchDiffLogTitle &&
        m_branchDiffLogTitle->property("logTitle").toString() == title &&
        m_branchDiffLog && !m_branchDiffLog->document()->isEmpty();
    if (m_branchDiffLog && !sameLoad)
        m_branchDiffLog->clear();
    if (m_branchDiffLogTitle)
        m_branchDiffLogTitle->setProperty("logTitle", title);
    if (sameLoad)
        appendBranchDiffLog(QString::fromUtf8("\xE2\x94\x80\xE2\x94\x80 again "
                                              "\xE2\x94\x80\xE2\x94\x80"));
    appendBranchDiffLog(title, BranchDiffLogKind::Step);
}

void MainWindow::appendBranchDiffLog(const QString &line, BranchDiffLogKind kind)
{
    if (!m_branchDiffLog)
        return;
    if (!m_branchDiffLogClock.isValid())
        m_branchDiffLogClock.start();
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString muted = QStringLiteral("#8b949e");
    const QString fg = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    QString glyph;
    QString glyphColor = muted;
    bool bold = false;
    switch (kind) {
    case BranchDiffLogKind::Step:
        glyph = QString::fromUtf8("\xE2\x96\xB6"); // ▶
        glyphColor = dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
        bold = true;
        break;
    case BranchDiffLogKind::Info:
        glyph = QString::fromUtf8("\xC2\xB7"); // ·
        break;
    case BranchDiffLogKind::Git:
        glyph = QStringLiteral("$");
        glyphColor = Theme::nextOrHex(
            Theme::Role::FgSecondary,
            dark ? QStringLiteral("#d2a8ff") : QStringLiteral("#8250df"));
        break;
    case BranchDiffLogKind::Progress:
        glyph = QString::fromUtf8("\xE2\x86\x93"); // ↓
        glyphColor = dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
        break;
    case BranchDiffLogKind::Ok:
        glyph = forkmesh::backgroundOkGlyph();
        glyphColor = QStringLiteral("#3fb950");
        break;
    case BranchDiffLogKind::Warn:
        glyph = QStringLiteral("!");
        glyphColor = QStringLiteral("#d29922");
        break;
    case BranchDiffLogKind::Fail:
        glyph = forkmesh::backgroundNotGlyph();
        glyphColor = QStringLiteral("#f85149");
        break;
    case BranchDiffLogKind::ProgressDone:
        glyph = forkmesh::backgroundOkGlyph();
        glyphColor = QStringLiteral("#3fb950");
        break;
    }
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString since =
        forkmesh::backgroundElapsedText(m_branchDiffLogClock.elapsed());
    const QString html =
        QStringLiteral("<span style='color:%1'>%2</span>&nbsp;"
                       "<span style='color:%1'>%3</span>&nbsp;&nbsp;"
                       "<span style='color:%4;font-weight:700'>%5</span>&nbsp;"
                       "<span style='color:%6%7'>%8</span>")
            .arg(muted, stamp, QStringLiteral("+") + since.rightJustified(7),
                 glyphColor, glyph.toHtmlEscaped(), fg,
                 bold ? QStringLiteral(";font-weight:700") : QString(),
                 line.toHtmlEscaped());

    QTextDocument *doc = m_branchDiffLog->document();
    QScrollBar *bar = m_branchDiffLog->verticalScrollBar();
    m_branchDiffLogFollow = !bar || bar->value() >= bar->maximum() - 4;
    if ((kind == BranchDiffLogKind::Progress ||
         kind == BranchDiffLogKind::ProgressDone) &&
        m_branchDiffLogLastWasProgress) {
        // A byte counter ticking every quarter second replaces itself in
        // place; the finished figure stays as an ordinary line.
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::End);
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
    }
    m_branchDiffLog->append(html);
    m_branchDiffLogLastWasProgress = kind == BranchDiffLogKind::Progress;
    constexpr int kMaxLines = 2500;
    constexpr int kTrimLines = 500;
    if (doc->blockCount() > kMaxLines) {
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor,
                            kTrimLines);
        cursor.removeSelectedText();
    }
    if (m_branchDiffLogFollow && bar)
        bar->setValue(bar->maximum());
    if (m_branchDiffLogTitle) {
        const QString title =
            m_branchDiffLogTitle->property("logTitle").toString();
        m_branchDiffLogTitle->setText(
            QStringLiteral("<b>Activity</b>%1 &nbsp;<span style='color:%2'>%3 · "
                           "%4</span>")
                .arg(title.isEmpty() ? QString()
                                     : QStringLiteral(" · ") + title.toHtmlEscaped(),
                     muted, since, line.toHtmlEscaped()));
    }
    if (m_branchDiffLogPanel && !m_branchDiffLogCollapsed &&
        !m_branchDiffLogPanel->isVisible())
        m_branchDiffLogPanel->show();
    updateBranchDiffLogButton(line);
}

void MainWindow::postBranchDiffLog(int logGen, const QString &line,
                                   BranchDiffLogKind kind)
{
    if (!m_acceptOffThreadResults.load(std::memory_order_acquire))
        return;
    QMetaObject::invokeMethod(
        this,
        [this, logGen, line, kind] {
            if (logGen != m_branchDiffLogGen)
                return; // the pane moved on to another load
            appendBranchDiffLog(line, kind);
        },
        Qt::QueuedConnection);
}

std::function<void(const QString &, int)> MainWindow::branchDiffLogSink(int logGen)
{
    return [this, logGen](const QString &line, int kind) {
        postBranchDiffLog(logGen, line, static_cast<BranchDiffLogKind>(kind));
    };
}

void MainWindow::noteBranchDiffTicket(const forkmesh::BackgroundTicket &ticket,
                                      bool started)
{
    if (!m_branchDiffLog || m_branchDiffLogRoutine.isEmpty() ||
        ticket.routine != m_branchDiffLogRoutine)
        return;
    const QString detail = forkmesh::backgroundDetailNote(ticket.detail);
    if (started) {
        appendBranchDiffLog(ticket.kind == QLatin1String("git") || detail.isEmpty()
                                ? (detail.isEmpty() ? ticket.kind : detail)
                                : ticket.kind + QStringLiteral(": ") + detail,
                            BranchDiffLogKind::Git);
        return;
    }
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - ticket.startedAtMs;
    const QString failure = forkmesh::backgroundFailureText(ticket.outcome);
    const QString name = ticket.kind == QLatin1String("git")
                             ? gitTicketShortName(detail)
                             : ticket.kind;
    if (failure.isEmpty())
        appendBranchDiffLog(QStringLiteral("%1 finished in %2")
                                .arg(name, forkmesh::backgroundElapsedText(elapsed)),
                            BranchDiffLogKind::Ok);
    else
        appendBranchDiffLog(QStringLiteral("%1 %2 after %3")
                                .arg(name, failure,
                                     forkmesh::backgroundElapsedText(elapsed)),
                            BranchDiffLogKind::Fail);
}

void MainWindow::updateBranchDiffSummary(const QList<DiffFileEntry> &files,
                                         const QSet<QString> &viewed,
                                         const QString &patch,
                                         const QString &viewedContext)
{
    if (!m_branchDiffSummaryBar || !m_branchDiffSummaryLabel)
        return;
    QStringList paths;
    for (const DiffFileEntry &f : files)
        paths.append(f.path);
    const QString html = diffReviewSummaryHtml(
        files, viewed, diffViewedMarksCurrent(viewedContext, paths));
    m_branchDiffSummaryLabel->setText(html);
    updateBranchDiffHealth(files, patch);
    m_branchDiffSummaryBar->setVisible(!html.isEmpty());
}

void MainWindow::updateBranchDiffHealth(const QList<DiffFileEntry> &files,
                                        const QString &patch)
{
    if (!m_branchDiffSummaryBar)
        return;
    const QString inputKey = branchDiffHealthInputKey();
    // Folding/reopening or marking a file viewed refreshes the summary text,
    // but does not change the patch or Git verdict. Avoid repeatedly running
    // every regex (especially the credential scanner) over a large patch.
    if (patch == m_branchDiffHealthPatch && !m_branchDiffHealthLabels.isEmpty() &&
        m_branchDiffHealthLabels.first()
                ->property("healthInputKey")
                .toString() == inputKey) {
        for (QLabel *label : std::as_const(m_branchDiffHealthLabels))
            label->setVisible(!files.isEmpty() &&
                              label->property("healthScore").isValid());
        if (files.isEmpty() && m_branchSecretBanner)
            m_branchSecretBanner->clearFindings();
        return;
    }
    DiffHealthReport health;
    if (m_branchDiffHealthReport && m_branchDiffHealthReportKey == inputKey &&
        patch == m_branchDiffHealthReportPatch) {
        health = *m_branchDiffHealthReport; // scored on the render worker
    } else if (!branchDiffRenderNeedsWorker(patch)) {
        if (m_branchDiffHealthReport && patch == m_branchDiffHealthReportPatch)
            appendBranchDiffLog(QStringLiteral("Merge-health inputs changed since "
                                              "the worker scored them; re-scoring"));
        m_branchDiffHealthScoringKey.clear();
        m_branchDiffHealthScoringPatch.clear();
        health = diffHealthReport(patch, files, m_branchDiffHealthBehind,
                                  m_branchDiffHealthAhead, m_branchDiffHealthConflict,
                                  m_branchDiffHealthWorktreeConflict,
                                  m_branchDiffHealthConflictPending);
        m_branchDiffHealthReport = std::make_shared<DiffHealthReport>(health);
        m_branchDiffHealthReportKey = inputKey;
        m_branchDiffHealthReportPatch = patch;
    } else {
        const bool alreadyScoring = m_branchDiffHealthScoringKey == inputKey &&
                                    m_branchDiffHealthScoringPatch == patch;
        if (m_branchDiffHealthReport && patch == m_branchDiffHealthReportPatch &&
            !alreadyScoring)
            appendBranchDiffLog(QStringLiteral("Merge-health inputs changed since "
                                              "the worker scored them; re-scoring"));
        if (patch != m_branchDiffHealthPatch) {
            for (QLabel *label : std::as_const(m_branchDiffHealthLabels))
                label->setVisible(false);
            if (m_branchSecretBanner)
                m_branchSecretBanner->clearFindings();
        }
        m_branchDiffHealthPatch = patch;
        if (alreadyScoring)
            return;
        m_branchDiffHealthScoringKey = inputKey;
        m_branchDiffHealthScoringPatch = patch;
        const int behind = m_branchDiffHealthBehind;
        const int ahead = m_branchDiffHealthAhead;
        const bool conflict = m_branchDiffHealthConflict;
        const bool worktreeConflict = m_branchDiffHealthWorktreeConflict;
        const bool conflictPending = m_branchDiffHealthConflictPending;
        runOffThread<DiffHealthReport>(
            [patch, files, behind, ahead, conflict, worktreeConflict,
             conflictPending] {
                return diffHealthReport(patch, files, behind, ahead, conflict,
                                        worktreeConflict, conflictPending);
            },
            [this, files, patch, inputKey](DiffHealthReport scored) {
                if (m_branchDiffHealthScoringKey != inputKey ||
                    m_branchDiffHealthScoringPatch != patch)
                    return;
                m_branchDiffHealthScoringKey.clear();
                m_branchDiffHealthScoringPatch.clear();
                m_branchDiffHealthReport =
                    std::make_shared<DiffHealthReport>(std::move(scored));
                m_branchDiffHealthReportKey = inputKey;
                m_branchDiffHealthReportPatch = patch;
                if (m_branchDiffHealthPatch == patch &&
                    branchDiffHealthInputKey() == inputKey)
                    updateBranchDiffHealth(files, patch);
            });
        return;
    }
    m_branchDiffHealthPatch = patch;
    if (m_branchSecretBanner)
        m_branchSecretBanner->setFindings(
            files.isEmpty() ? QList<RepoSecurityFinding>() : health.secrets,
            !m_branchDiffWorkDir.isEmpty());
    for (int i = 0; i < m_branchDiffHealthLabels.size(); ++i) {
        QLabel *label = m_branchDiffHealthLabels.at(i);
        const bool available = i < health.metrics.size() && !files.isEmpty();
        label->setVisible(available);
        label->setProperty("healthInputKey", inputKey);
        if (available)
            applyDiffHealthMetric(label, health.metrics.at(i));
    }
}

QList<RepoSecurityFinding> MainWindow::addSecretIgnoreMarkers(
    const QString &workDir, const QList<RepoSecurityFinding> &findings)
{
    QList<RepoSecurityFinding> marked;
    QStringList failed;
    if (workDir.isEmpty())
        return marked;
    for (const RepoSecurityFinding &finding :
         forkmesh::ui::secretFindingsByLine(findings)) {
        if (RepoSecurity::markFindingSafe(workDir, finding))
            marked.append(finding);
        else
            failed << QStringLiteral("%1:%2").arg(finding.path).arg(finding.line);
    }
    if (!failed.isEmpty()) {
        flashMessage(
            QStringLiteral("Could not add the secret-scan ignore marker to %1. "
                           "The line may have changed since the diff was read, "
                           "or the file type has no comment syntax; add \"%2\" "
                           "by hand.")
                .arg(failed.join(QStringLiteral(", ")),
                     forkmesh::ui::secretScanIgnoreMarker()),
            true);
    } else if (!marked.isEmpty()) {
        flashMessage(
            QStringLiteral("Added the secret-scan ignore marker to %1 line%2. "
                           "Commit the change so the push scan skips %3 too.")
                .arg(marked.size())
                .arg(marked.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(marked.size() == 1 ? QStringLiteral("it")
                                        : QStringLiteral("them")));
    }
    return marked;
}

void MainWindow::ignoreBranchDiffSecrets(const QList<RepoSecurityFinding> &findings)
{
    const QString branch = m_branchDiffBranch;
    const QString work = m_branchDiffWorkDir;
    if (branch.isEmpty() || work.isEmpty())
        return;
    const QList<RepoSecurityFinding> marked = addSecretIgnoreMarkers(work, findings);
    if (marked.isEmpty())
        return;
    if (m_branchSecretBanner)
        m_branchSecretBanner->removeFindings(marked);
    if (m_branchDiffHealthReport)
        m_branchDiffHealthReport->secrets = forkmesh::ui::secretFindingsWithout(
            m_branchDiffHealthReport->secrets, marked);
    forgetBranchDiff(branch);
    renderBranchScopeDiff();
}

// code-health: allow(deep-nested-loop) one pass over the document's fragments; the innermost loop is a fragment's anchor names with a hash lookup
void MainWindow::rebuildBranchDiffSpans()
{
    if (!m_branchDiffView)
        return;
    m_branchDiffFileSpans.clear();
    QTextDocument *spanDoc = m_branchDiffView->document();
    // Match files by their anchors in one document pass. Scanning for path
    // text in file order finds nothing on later diff pages (the earlier
    // files' paths never appear, so the ordered walk never advances) and can
    // stick to a path quoted inside an unrelated hunk.
    QHash<QString, int> anchorIndex;
    for (int i = 0; i < m_branchDiffFileAnchors.size() &&
                    i < m_branchDiffFilePaths.size();
         ++i) {
        if (!m_branchDiffFileAnchors.at(i).isEmpty())
            anchorIndex.insert(m_branchDiffFileAnchors.at(i), i);
    }
    for (QTextBlock block = spanDoc->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isAnchor())
                continue;
            const QStringList names = frag.charFormat().anchorNames();
            for (const QString &name : names) {
                const auto ai = anchorIndex.constFind(name);
                if (ai != anchorIndex.constEnd()) {
                    m_branchDiffFileSpans.append(qMakePair(
                        frag.position(), m_branchDiffFilePaths.at(ai.value())));
                    anchorIndex.erase(ai);
                    break;
                }
            }
        }
    }
    // Path text remains the fallback for files without a surviving anchor,
    // but never on a paged diff, where absence means "on another page".
    if (!anchorIndex.isEmpty() && !diffIsPaginated(m_branchDiffView)) {
        for (auto it = anchorIndex.constBegin(); it != anchorIndex.constEnd();
             ++it) {
            const QString &path = m_branchDiffFilePaths.at(it.value());
            const QTextCursor found = spanDoc->find(path);
            if (!found.isNull())
                m_branchDiffFileSpans.append(
                    qMakePair(found.selectionStart(), path));
        }
    }
    std::sort(m_branchDiffFileSpans.begin(), m_branchDiffFileSpans.end(),
              [](const QPair<int, QString> &left, const QPair<int, QString> &right) {
                  return left.first < right.first;
              });
    updateBranchDiffSticky();
}

void MainWindow::createPullFromBranch(const QString &branch,
                                      std::function<void(int)> done)
{
    const auto fail = [&done] {
        if (done)
            done(-1);
    };
    const QString dir = repoGitDir();
    const QString base = repoDefaultBranch(repoBranches());
    if (branch.isEmpty() || branch == base) {
        fail();
        return;
    }
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "This is a read-only mirror; pull requests can't be opened here.", true);
        fail();
        return;
    }
    runOffThread<QPair<QByteArray, QStringList>>(
        [dir, base, branch] {
            QByteArray diff;
            QString err;
            if (!runGitCapture(dir, {"diff", "--binary", base + ".." + branch}, &diff,
                               &err) ||
                QString::fromUtf8(diff).trimmed().isEmpty())
                return qMakePair(QByteArray(), QStringList());
            QByteArray subjectOut;
            runGitCapture(dir, {"log", "--format=%s", "--reverse", base + ".." + branch},
                          &subjectOut, nullptr);
            return qMakePair(diff,
                             QString::fromUtf8(subjectOut).split('\n', Qt::SkipEmptyParts));
        },
        [this, dir, base, branch, done](QPair<QByteArray, QStringList> read) {
            const auto failed = [&done] {
                if (done)
                    done(-1);
            };
            if (repoGitDir() != dir) {
                failed();
                return;
            }
            const QByteArray diff = read.first;
            const QStringList subjects = read.second;
            if (diff.isEmpty()) {
                setRepoDetailNotice(
                    QStringLiteral("%1 has no changes to open as a pull request.")
                        .arg(branch),
                    true);
                failed();
                return;
            }
            bool ok = false;
            const QString title =
                QInputDialog::getText(
                    this, "Create pull request",
                    QStringLiteral("Title for the pull request from %1 into %2:")
                        .arg(branch, base),
                    QLineEdit::Normal, subjects.isEmpty() ? branch : subjects.first(),
                    &ok)
                    .trimmed();
            if (!ok || title.isEmpty() || repoGitDir() != dir) {
                failed();
                return;
            }
            QString description;
            for (const QString &s : subjects)
                description += "- " + s + "\n";
            runOffThread<QByteArray>(
                [dir, base, branch] {
                    QByteArray mbox;
                    runGitCapture(dir, {"format-patch", "--stdout", base + ".." + branch},
                                  &mbox, nullptr);
                    return mbox;
                },
                [this, dir, base, branch, title, description, diff,
                 done](QByteArray mbox) {
                    if (repoGitDir() != dir) {
                        if (done)
                            done(-1);
                        return;
                    }
                    PullStore store = pullStoreForCurrentRepo();
                    QString error;
                    const int number = store.createPull(
                        title, description, base, branch, QString::fromUtf8(diff),
                        QString::fromUtf8(mbox),
                        /*branchBacked=*/true, &error);
                    if (number < 0) {
                        setRepoDetailNotice(
                            error.isEmpty() ? "Could not create the pull request."
                                            : error,
                            true);
                        if (done)
                            done(-1);
                        return;
                    }
                    bindAgentSessionsToPull(number, branch);
                    logSystem(QStringLiteral("Opened pull #%1 from %2 into %3.")
                                  .arg(number)
                                  .arg(branch, base));
                    setRepoDetailNotice(
                        QStringLiteral("Opened pull request #%1 from %2.")
                            .arg(number)
                            .arg(branch));
                    m_currentPullNumber = number;
                    switchToPullTab(number);
                    if (done)
                        done(number);
                });
        });
}

MainWindow::WorktreeMergeReport
MainWindow::mergeBaseIntoLinkedWorktree(const QString &worktree,
                                        const QString &branch,
                                        const QString &base)
{
    WorktreeMergeReport report;
    if (const QStringList staleLocks = removeStaleGitLocks(worktree);
        !staleLocks.isEmpty())
        report.logs << QStringLiteral("Git: removed orphaned lock(s) %1 before updating %2.")
                           .arg(staleLocks.join(QStringLiteral(", ")), branch);
    QByteArray existingMerge;
    if (runGitCapture(worktree, {"rev-parse", "-q", "--verify", "MERGE_HEAD"},
                      &existingMerge, nullptr) &&
        !existingMerge.trimmed().isEmpty()) {
        report.status = WorktreeMergeReport::Busy;
        report.message =
            QStringLiteral("%1 already has a merge in progress in its worktree; "
                           "finish or abort it before updating from %2.")
                .arg(branch, base);
        return report;
    }

    QByteArray existingUnmerged;
    if (runGitCapture(worktree, {"diff", "--name-only", "--diff-filter=U"},
                      &existingUnmerged, nullptr) &&
        !existingUnmerged.trimmed().isEmpty()) {
        report.status = WorktreeMergeReport::Busy;
        report.message = QStringLiteral("%1 already has unresolved local files; "
                                        "resolve them before updating from %2.")
                             .arg(branch, base);
        return report;
    }

    QByteArray beforeStatus;
    runGitCapture(worktree, {"status", "--porcelain"}, &beforeStatus, nullptr);
    const bool protectedLocalEdits = !beforeStatus.trimmed().isEmpty();
    QByteArray beforeHeadOut;
    if (!runGitCapture(worktree, {"rev-parse", "HEAD"}, &beforeHeadOut, nullptr) ||
        beforeHeadOut.trimmed().isEmpty()) {
        report.status = WorktreeMergeReport::Failed;
        report.message = QStringLiteral("Couldn't identify %1's current commit "
                                        "before updating it from %2.")
                             .arg(branch, base);
        return report;
    }
    const QString beforeHead = QString::fromUtf8(beforeHeadOut).trimmed();
    QByteArray beforeStashOut;
    runGitCapture(worktree, {"rev-parse", "-q", "--verify", "refs/stash"},
                  &beforeStashOut, nullptr);
    QString worktreeError;
    if (!runGitCapture(worktree, {"merge", "--autostash", "--no-edit", base},
                       nullptr, &worktreeError)) {
        // Git reports a failed command when the branch merge succeeds but its
        // automatic stash cannot be reapplied. At that point MERGE_HEAD is
        // already gone and HEAD has advanced, so treating it as an ordinary
        // merge failure strands the updated branch and the user's edits in
        // refs/stash. Keep Pull-main transactional by recognizing that exact
        // state, rolling the branch back, and applying the new autostash to the
        // commit it came from.
        QByteArray failedHeadOut;
        QByteArray retainedStashOut;
        runGitCapture(worktree, {"rev-parse", "HEAD"}, &failedHeadOut, nullptr);
        runGitCapture(worktree,
                      {"rev-parse", "-q", "--verify", "refs/stash"},
                      &retainedStashOut, nullptr);
        const QString failedHead = QString::fromUtf8(failedHeadOut).trimmed();
        const QString retainedStash =
            QString::fromUtf8(retainedStashOut).trimmed();
        const bool haveNewAutostash =
            !retainedStash.isEmpty() &&
            retainedStashOut.trimmed() != beforeStashOut.trimmed();
        if (protectedLocalEdits && failedHead != beforeHead &&
            haveNewAutostash) {
            QString rollbackError;
            const bool rolledBack = runGitCapture(
                worktree, {"reset", "--hard", beforeHead}, nullptr,
                &rollbackError);
            QString restoreError;
            const bool restored =
                rolledBack &&
                runGitCapture(worktree,
                              {"stash", "apply", "--index", retainedStash},
                              nullptr, &restoreError);
            if (!restored && rolledBack)
                runGitCapture(worktree, {"reset", "--hard", beforeHead},
                              nullptr, nullptr);
            if (restored) {
                QByteArray topStashOut;
                runGitCapture(worktree,
                              {"rev-parse", "-q", "--verify", "stash@{0}"},
                              &topStashOut, nullptr);
                if (QString::fromUtf8(topStashOut).trimmed() == retainedStash)
                    runGitCapture(worktree,
                                  {"stash", "drop", "stash@{0}"}, nullptr,
                                  nullptr);
            }
            report.status = WorktreeMergeReport::Conflicted;
            report.message =
                restored
                    ? QStringLiteral("Couldn't update %1 from %2 because its "
                                     "local edits overlap. The branch was "
                                     "rolled back and its original edits were "
                                     "restored cleanly.")
                          .arg(branch, base)
                    : QStringLiteral("Couldn't update %1 from %2 because its "
                                     "local edits overlap. The branch was "
                                     "rolled back; the original edits remain "
                                     "safe in Git's autostash (%3).")
                          .arg(branch, base,
                               (rollbackError + QLatin1Char(' ') + restoreError)
                                   .trimmed()
                                   .left(180));
            return report;
        }
        QByteArray mergeConflicts;
        const bool contentConflict =
            runGitCapture(worktree,
                          {"diff", "--name-only", "--diff-filter=U"},
                          &mergeConflicts, nullptr) &&
            !mergeConflicts.trimmed().isEmpty();
        QByteArray startedMerge;
        if (runGitCapture(worktree, {"rev-parse", "-q", "--verify", "MERGE_HEAD"},
                          &startedMerge, nullptr) &&
            !startedMerge.trimmed().isEmpty())
            runGitCapture(worktree, {"merge", "--abort"}, nullptr, nullptr);
        report.status = contentConflict ? WorktreeMergeReport::Conflicted
                                        : WorktreeMergeReport::Failed;
        report.message = contentConflict
            ? QStringLiteral("%1 and %2 overlap in %3 file(s). Its local "
                             "changes are safe; open the merge editor to "
                             "resolve them.")
                  .arg(branch, base)
                  .arg(QString::fromUtf8(mergeConflicts)
                           .split('\n', Qt::SkipEmptyParts)
                           .size())
            : QStringLiteral("Couldn't update %1 from %2 in its worktree: %3. "
                             "Its local changes are safe.")
                  .arg(branch, base,
                       worktreeError.trimmed().isEmpty()
                           ? QStringLiteral("Git refused the merge")
                           : worktreeError.trimmed()
                                 .left(240)
                                 .remove(QRegularExpression(QStringLiteral("\\.+$"))));
        return report;
    }

    QByteArray unmerged;
    const bool restoreConflict =
        runGitCapture(worktree, {"diff", "--name-only", "--diff-filter=U"},
                      &unmerged, nullptr) &&
        !unmerged.trimmed().isEmpty();
    if (restoreConflict) {
        const int count =
            QString::fromUtf8(unmerged).split('\n', Qt::SkipEmptyParts).size();
        QByteArray retainedStashOut;
        runGitCapture(worktree, {"rev-parse", "-q", "--verify", "refs/stash"},
                      &retainedStashOut, nullptr);
        const QString retainedStash = QString::fromUtf8(retainedStashOut).trimmed();
        const bool haveNewAutostash =
            !retainedStash.isEmpty() &&
            retainedStashOut.trimmed() != beforeStashOut.trimmed();
        QString rollbackError;
        const bool rolledBack =
            (!protectedLocalEdits || haveNewAutostash) &&
            runGitCapture(worktree, {"reset", "--hard", beforeHead}, nullptr,
                          &rollbackError);
        QString restoreError;
        const bool restored =
            rolledBack &&
            (!protectedLocalEdits ||
             (haveNewAutostash &&
              runGitCapture(worktree,
                            {"stash", "apply", "--index", retainedStash}, nullptr,
                            &restoreError)));
        // A failed stash application can itself leave conflict markers.
        // Clear those generated files once more; the retained stash is the
        // recoverable copy of the original edits in this rare fallback.
        if (!restored && rolledBack)
            runGitCapture(worktree, {"reset", "--hard", beforeHead}, nullptr,
                          nullptr);
        if (restored && haveNewAutostash) {
            QByteArray topStashOut;
            runGitCapture(worktree, {"rev-parse", "-q", "--verify", "stash@{0}"},
                          &topStashOut, nullptr);
            if (QString::fromUtf8(topStashOut).trimmed() == retainedStash)
                runGitCapture(worktree, {"stash", "drop", "stash@{0}"}, nullptr,
                              nullptr);
        }
        report.logs << (QStringLiteral(
                      "Git: rolled %1 back after %2 local file(s) conflicted "
                      "with %3; original edits %4.")
                      .arg(branch)
                      .arg(count)
                      .arg(base)
                      .arg(restored ? QStringLiteral("restored")
                                    : QStringLiteral("kept in the autostash")));
        report.status = WorktreeMergeReport::Conflicted;
        report.message =
            restored
                ? QStringLiteral("Couldn't update %1 from %2 because %3 local "
                                 "file(s) overlap. The branch was rolled back "
                                 "and its original edits were restored cleanly.")
                      .arg(branch, base)
                      .arg(count)
                : QStringLiteral("Couldn't update %1 from %2 because %3 local "
                                 "file(s) overlap. The branch was rolled back; "
                                 "the original edits remain safe in Git's "
                                 "autostash (%4).")
                      .arg(branch, base)
                      .arg(count)
                      .arg((rollbackError + QLatin1Char(' ') + restoreError)
                               .trimmed()
                               .left(180));
        return report;
    }

    report.logs << QStringLiteral("Git: merged %1 into %2 in its linked worktree.")
                       .arg(base, branch);
    report.status = WorktreeMergeReport::Merged;
    report.message =
        protectedLocalEdits
            ? QStringLiteral("Updated %1 with %2 and safely restored its local "
                             "changes.")
                  .arg(branch, base)
            : QStringLiteral("Updated %1 with %2 in its worktree.").arg(branch, base);
    return report;
}

bool MainWindow::repoDetailShows(const QString &localPath) const
{
    return !localPath.isEmpty() && m_repoDetailIndex >= 0 &&
           m_repoDetailIndex < m_repositories.size() &&
           (m_repositories.at(m_repoDetailIndex).localPath == localPath ||
            repoGitDir() == localPath);
}

void MainWindow::noticeForRepo(bool here, const QString &message, bool error,
                               const QString &kind)
{
    if (message.isEmpty())
        return;
    if (here)
        setRepoDetailNotice(message, error, kind);
    else
        flashMessage(message, error, QString(), kind);
}

QString MainWindow::putCheckoutBack(const BranchGitFlow &flow, QStringList *logs)
{
    // Putting the checkout back is not bookkeeping that can be skipped. This
    // flow borrows the shared checkout to update a branch that has no worktree
    // of its own, so if the return trip fails the checkout is simply left
    // sitting on that branch — which is how "main" quietly becomes an agent
    // branch (adhoc #203). Git refuses here for the same reasons it refuses
    // anywhere: a leftover index.lock from a Git run that was killed, a file it
    // would clobber. Dropping the result hides all of it, so report it.
    if (flow.isCurrent || flow.currentBranch.isEmpty())
        return QString();
    QString cerr;
    if (runGitCapture(flow.dir, {"checkout", flow.currentBranch}, nullptr, &cerr))
        return QString();
    logs->append(QStringLiteral("Git: could not put the checkout back on %1: %2")
                     .arg(flow.currentBranch, cerr.trimmed().isEmpty()
                                                  ? QStringLiteral("Git refused")
                                                  : cerr.trimmed()));
    return QStringLiteral(" The checkout could not be put back on %1 and is "
                          "still on %2 — switch it back before working in it.")
        .arg(flow.currentBranch, flow.branch);
}

QString MainWindow::popBranchUpdateStash(BranchGitFlow *flow)
{
    if (!flow->stashed)
        return QString();
    flow->stashed = false;
    if (runGitCapture(flow->dir, {"stash", "pop"}, nullptr, nullptr))
        return QStringLiteral(" Your stashed changes were restored.");
    return QStringLiteral(" Your local changes couldn't be cleanly restored and "
                          "remain stashed — run \"git stash pop\" to reapply them.");
}

void MainWindow::showBranchGitStep(const BranchGitStep &step)
{
    for (const QString &line : step.logs)
        logSystem(line);
    const bool here = repoGitDir() == step.flow.dir;
    noticeForRepo(here, step.message, step.error, step.noticeKind);
    if (!here)
        return;
    if (step.clearBranchesCache)
        m_branchesCache.clear();
    if (step.showBranch && m_branchDiffBranch == step.flow.branch)
        showBranchDiff(step.flow.branch, m_branchDiffAgentSessionId);
    if (step.reloadWorktrees)
        loadWorktreesPanel();
    if (step.reloadBranchesAndTags)
        loadBranchesAndTags();
}

void MainWindow::updateBranchFromBase(const QString &branch)
{
    const QString dir = repoGitDir();
    const QString base = repoDefaultBranchFast();
    if (branch.isEmpty() || branch == base || dir.isEmpty())
        return;
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "This is a read-only mirror; branches can't be updated here.", true,
            forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (m_branchMergeFlowBusy || m_worktreeMergeInProgress) {
        flashMessage(m_worktreeMergeInProgress
                         ? QStringLiteral("A merge is running; pull again when it finishes.")
                         : QStringLiteral("A branch update is already running."));
        return;
    }
    m_branchMergeFlowBusy = true;
    BranchGitFlow flow;
    flow.dir = dir;
    flow.branch = branch;
    flow.base = base;
    runOffThread<BranchGitStep>(
        [flow] { return readBranchUpdate(flow); },
        [this](BranchGitStep step) { applyBranchUpdateStep(std::move(step)); });
}

MainWindow::BranchGitStep MainWindow::readBranchUpdate(BranchGitFlow flow)
{
    BranchGitStep step;
    const QString &dir = flow.dir;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    int behind = 0, ahead = 0;
    readBehindAhead(dir, base, branch, &behind, &ahead);
    if (behind == 0) {
        step.flow = flow;
        step.message =
            QStringLiteral("%1 is already up to date with %2.").arg(branch, base);
        return step;
    }

    const QString linkedWorktree = readWorktreePathForBranch(dir, branch);
    if (!linkedWorktree.isEmpty() &&
        QDir(linkedWorktree).absolutePath() != QDir(dir).absolutePath()) {
        const WorktreeMergeReport report =
            mergeBaseIntoLinkedWorktree(linkedWorktree, branch, base);
        // Pull is deliberately transactional. A protected-edit overlap has
        // already been rolled back and restored by the helper; opening the
        // merge editor here would immediately repeat the merge and leave that
        // same worktree conflicted. Report the safe rollback and let the
        // explicit Merge editor action opt into conflict resolution.
        step.flow = flow;
        step.logs = report.logs;
        step.message = report.message;
        step.error = report.status != WorktreeMergeReport::Merged;
        step.noticeKind = forkmesh::kLocalOperationNoticeKind;
        step.clearBranchesCache = true;
        step.showBranch = true;
        step.reloadWorktrees = true;
        return step;
    }

    flow.currentBranch = checkedOutBranch(dir);
    flow.isCurrent = !flow.currentBranch.isEmpty() && branch == flow.currentBranch;
    step.flow = flow;

    if (ahead == 0 && !flow.isCurrent) {
        // `isCurrent` only reflects *this* checkout's HEAD. The branch can still be
        // checked out in a separate agent worktree (e.g. an issue session under
        // <checkout>/.worktrees/...), and git flatly refuses to fetch into a ref
        // that's live in another worktree — surfacing a cryptic
        // "fatal: refusing to fetch into branch '...' checked out at '...'".
        // Explain what's actually happening instead of dumping the raw error, so
        // it's obvious the branch is busy in an active session. Any: git refuses
        // just as flatly when the worktree's directory is already gone.
        const QString otherWorktree =
            readWorktreePathForBranch(dir, branch, WorktreeLookup::Any);
        if (!otherWorktree.isEmpty()) {
            step.message = QStringLiteral(
                               "%1 is behind %2 but is checked out by an active agent session at "
                               "%3, so it can't be updated from here — git won't fetch into a "
                               "branch that's live in another worktree. Stop or finish that agent "
                               "first, or let it update from %2 itself.")
                               .arg(branch, base, otherWorktree);
            step.error = true;
            return step;
        }
        QString err;
        if (!runGitCapture(dir, {"fetch", ".", base + ":" + branch}, nullptr, &err)) {
            QString msg = err.trimmed();
            if (msg.contains(QLatin1String("refusing to fetch into branch"))) {
                msg = QStringLiteral(
                          "%1 can't be updated from here because it's currently checked "
                          "out in another worktree (an active agent session). Stop or "
                          "finish that agent first, or let it update from %2 itself.")
                          .arg(branch, base);
            } else if (msg.isEmpty()) {
                msg = QStringLiteral("Could not fast-forward the branch.");
            } else {
                msg = msg.left(240);
            }
            step.message = msg;
            step.error = true;
            return step;
        }
        step.logs << QStringLiteral("Git: fast-forwarded %1 to %2.").arg(branch, base);
        step.message = QStringLiteral("Updated %1 with %2.").arg(branch, base);
        step.clearBranchesCache = true;
        step.showBranch = true;
        return step;
    }

    QByteArray status;
    QString err;
    if (!runGitCapture(dir, {"status", "--porcelain"}, &status, &err)) {
        step.message =
            err.isEmpty() ? QStringLiteral("Could not read the repository status.")
                          : err.left(240);
        step.error = true;
        return step;
    }
    if (!status.trimmed().isEmpty()) {
        for (const QString &l :
             QString::fromUtf8(status).split('\n', Qt::SkipEmptyParts))
            step.dirtyFiles << l.mid(3); // strip the two-char XY status + space
        step.kind = BranchGitStep::NeedsStashChoice;
        return step;
    }
    return mergeBaseIntoBranchCheckout(flow, false);
}

MainWindow::BranchGitStep MainWindow::mergeBaseIntoBranchCheckout(BranchGitFlow flow,
                                                                  bool stash)
{
    BranchGitStep step;
    const QString &dir = flow.dir;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    QString err;
    if (stash) {
        if (!runGitCapture(
                dir,
                {"stash", "push", "-u", "-m",
                 QStringLiteral("forkmesh: auto-stash before pulling %1 into %2")
                     .arg(base, branch)},
                nullptr, &err)) {
            step.flow = flow;
            step.message = err.isEmpty()
                               ? QStringLiteral("Could not stash the local changes.")
                               : err.left(240);
            step.error = true;
            return step;
        }
        flow.stashed = true;
        step.logs << QStringLiteral("Git: auto-stashed local changes before pulling "
                                    "%1 into %2.")
                         .arg(base, branch);
    }

    if (!flow.isCurrent && !checkoutReleasingWorktree(dir, branch, &err)) {
        const QString note = popBranchUpdateStash(&flow);
        step.flow = flow;
        step.message = QStringLiteral("Could not check out %1: %2%3")
                           .arg(branch, err.left(200), note);
        step.error = true;
        return step;
    }

    if (const GitCaptureResult merge =
            runGitCaptureBackgrounded(dir, {"merge", "--no-edit", base});
        !merge.ok) {
        const QStringList conflicted = unmergedFilesIn(dir);
        if (conflicted.isEmpty()) {
            runGitCapture(dir, {"merge", "--abort"}, nullptr, nullptr);
            const QString back = putCheckoutBack(flow, &step.logs);
            const QString note = back + popBranchUpdateStash(&flow);
            step.flow = flow;
            step.message =
                QStringLiteral("Could not merge %1 into %2: %3%4")
                    .arg(base, branch,
                         gitFailureDetail(merge, QStringLiteral("git merge"), 200),
                         note);
            step.error = true;
            return step;
        }
        step.flow = flow;
        step.kind = BranchGitStep::NeedsEditor;
        step.conflicted = conflicted;
        return step;
    }

    const QString back = putCheckoutBack(flow, &step.logs);
    const QString note = back + popBranchUpdateStash(&flow);
    step.flow = flow;
    step.logs << QStringLiteral("Git: merged %1 into %2.").arg(base, branch);
    step.message = QStringLiteral("Updated %1 with %2.%3").arg(branch, base, note);
    step.error = !back.isEmpty();
    step.clearBranchesCache = true;
    step.showBranch = true;
    return step;
}

MainWindow::BranchGitStep MainWindow::finishBranchUpdateEditor(BranchGitFlow flow,
                                                               bool committed)
{
    BranchGitStep step;
    if (!committed) {
        runGitCapture(flow.dir, {"merge", "--abort"}, nullptr, nullptr);
        const QString back = putCheckoutBack(flow, &step.logs);
        const QString note = back + popBranchUpdateStash(&flow);
        step.flow = flow;
        step.message = QStringLiteral("Cancelled the merge of %1 into %2; %2 was left "
                                      "unchanged.%3")
                           .arg(flow.base, flow.branch, note);
        step.error = !back.isEmpty();
        return step;
    }
    const QString back = putCheckoutBack(flow, &step.logs);
    const QString note = back + popBranchUpdateStash(&flow);
    step.flow = flow;
    step.logs << QStringLiteral("Git: merged %1 into %2 (conflicts resolved).")
                     .arg(flow.base, flow.branch);
    step.message = QStringLiteral("Updated %1 with %2 (conflicts resolved).%3")
                       .arg(flow.branch, flow.base, note);
    step.error = !back.isEmpty();
    step.clearBranchesCache = true;
    step.showBranch = true;
    return step;
}

void MainWindow::applyBranchUpdateStep(BranchGitStep step)
{
    const BranchGitFlow flow = step.flow;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    if (step.kind == BranchGitStep::Done) {
        m_branchMergeFlowBusy = false;
        showBranchGitStep(step);
        return;
    }
    for (const QString &line : std::as_const(step.logs))
        logSystem(line);
    if (step.kind == BranchGitStep::NeedsStashChoice) {
        if (repoGitDir() != flow.dir) {
            m_branchMergeFlowBusy = false;
            return;
        }
        const QStringList &files = step.dirtyFiles;
        const QString n = QString::number(files.size());
        const QString plural = files.size() == 1 ? QString() : QStringLiteral("s");
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("Update %1 from %2").arg(branch, base));
        box.setText(QStringLiteral("The working tree has uncommitted changes in %1 "
                                   "file%2, so %3 can't be merged in directly.")
                        .arg(n, plural, base));
        box.setInformativeText(
            QStringLiteral("Stash those changes, update %1 from %2, then restore them "
                           "on top? Nothing is discarded.")
                .arg(branch, base));
        box.setDetailedText(files.join('\n'));
        QPushButton *stashBtn = box.addButton(
            QStringLiteral("Stash, update & restore"), QMessageBox::AcceptRole);
        QPushButton *reviewBtn = box.addButton(QStringLiteral("Review changes"),
                                               QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(stashBtn);
        box.exec();
        if (box.clickedButton() == reviewBtn) {
            m_branchMergeFlowBusy = false;
            setRepoDetailNotice(
                QStringLiteral("Left %1 unchanged — review its %2 uncommitted file%3 "
                               "below, then update from %4.")
                    .arg(branch, n, plural, base));
            showOverviewCommits(); // the universal Git workspace
            loadCommits();         // refresh history + the working-changes panel
            return;
        }
        if (box.clickedButton() != stashBtn || repoGitDir() != flow.dir) {
            m_branchMergeFlowBusy = false;
            setRepoDetailNotice(
                QStringLiteral("Left %1 unchanged; commit or stash its %2 uncommitted "
                               "file%3 to update from %4.")
                    .arg(branch, n, plural, base));
            return;
        }
        runOffThread<BranchGitStep>(
            [flow] { return mergeBaseIntoBranchCheckout(flow, true); },
            [this](BranchGitStep next) { applyBranchUpdateStep(std::move(next)); });
        return;
    }
    const QString intro =
        QString::fromUtf8(
            "Resolve each conflict, then commit the merge into <b>%1</b>. "
            "<b>Ours</b> is %1; <b>theirs</b> is %2. You can also edit the "
            "text directly.")
            .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
    const QString dir = flow.dir;
    const bool committed = runMergeConflictEditor(
        QString::fromUtf8("Resolve conflicts \xE2\x80\x94 %1").arg(branch),
        intro, dir, step.conflicted, QStringLiteral("Commit merge"),
        [dir](QString *e) { return commitResolvedMerge(dir, e); },
        true);
    runOffThread<BranchGitStep>(
        [flow, committed] { return finishBranchUpdateEditor(flow, committed); },
        [this](BranchGitStep next) { applyBranchUpdateStep(std::move(next)); });
}

void MainWindow::openBranchMergeEditor(const QString &branch,
                                       std::function<void()> done)
{
    const QString dir = repoGitDir();
    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (branch.isEmpty() || branch == base || dir.isEmpty()) {
        if (done)
            done();
        return;
    }
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "This is a read-only mirror; branches can't be updated here.", true);
        if (done)
            done();
        return;
    }
    if (m_branchMergeFlowBusy || m_worktreeMergeInProgress) {
        flashMessage(m_worktreeMergeInProgress
                         ? QStringLiteral("A merge is running; try again when it finishes.")
                         : QStringLiteral("A branch update is already running."));
        if (done)
            done();
        return;
    }
    m_branchMergeFlowBusy = true;
    BranchGitFlow flow;
    flow.dir = dir;
    flow.branch = branch;
    flow.base = base;
    const PullStore store = pullStoreForCurrentRepo();
    runOffThread<BranchGitStep>(
        [flow, store] {
            BranchGitStep refused;
            refused.flow = flow;
            refused.error = true;
            if (!ensureBranchForMerge(store, flow.dir, flow.branch, flow.base,
                                      &refused.logs, &refused.message))
                return refused;
            BranchGitStep step = startBranchMergeEditor(flow);
            step.logs = refused.logs + step.logs;
            step.clearBranchesCache = step.clearBranchesCache || !refused.logs.isEmpty();
            return step;
        },
        [this, done](BranchGitStep step) {
            applyBranchEditorStep(std::move(step), done);
        });
}

MainWindow::BranchGitStep MainWindow::startBranchMergeEditor(BranchGitFlow flow)
{
    BranchGitStep step;
    const QString &dir = flow.dir;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    const QString linkedWorktree = readWorktreePathForBranch(dir, branch);
    flow.linkedWorktree = linkedWorktree;
    step.flow = flow;
    if (!linkedWorktree.isEmpty()) {
        const QStringList conflicted = unmergedFilesIn(linkedWorktree);
        if (!conflicted.isEmpty()) {
            step.kind = BranchGitStep::LocalConflicts;
            step.conflicted = conflicted;
            return step;
        }
    }

    int behind = 0, ahead = 0;
    readBehindAhead(dir, base, branch, &behind, &ahead);
    if (behind == 0) {
        step.message =
            QStringLiteral("%1 is already up to date with %2.").arg(branch, base);
        return step;
    }

    const bool inLinkedWorktree =
        !linkedWorktree.isEmpty() &&
        QDir(linkedWorktree).absolutePath() != QDir(dir).absolutePath();
    flow.mergeDir = inLinkedWorktree ? linkedWorktree : dir;
    const QString &mergeDir = flow.mergeDir;

    QByteArray status;
    QString err;
    if (!runGitCapture(mergeDir, {"status", "--porcelain"}, &status, &err) ||
        (!inLinkedWorktree && !status.trimmed().isEmpty())) {
        step.flow = flow;
        step.message =
            err.isEmpty()
                ? QStringLiteral("Commit or stash local changes before merging into "
                                 "this branch.")
                : err.left(240);
        step.error = true;
        return step;
    }

    flow.currentBranch = checkedOutBranch(dir);
    // Same trap as updateBranchFromBase (adhoc #203): this editor borrows the
    // shared checkout, and a return trip that quietly fails leaves it parked on
    // the branch being edited. Always record it, and hand back a sentence for
    // the callers that put a notice up straight afterwards.
    flow.isCurrent = inLinkedWorktree ||
                     (!flow.currentBranch.isEmpty() && branch == flow.currentBranch);
    step.flow = flow;

    if (!flow.isCurrent && !checkoutReleasingWorktree(dir, branch, &err)) {
        step.message =
            QStringLiteral("Could not check out %1: %2").arg(branch, err.left(200));
        step.error = true;
        return step;
    }

    QStringList mergeArgs{"merge"};
    if (inLinkedWorktree)
        mergeArgs << "--autostash";
    mergeArgs << "--no-commit" << "--no-ff" << base;
    const GitCaptureResult editorMerge =
        runGitCaptureBackgrounded(mergeDir, mergeArgs);

    const QStringList conflicted = unmergedFilesIn(mergeDir);
    if (!conflicted.isEmpty()) {
        step.kind = BranchGitStep::NeedsEditor;
        step.conflicted = conflicted;
        return step;
    }

    const GitCaptureResult editorCommit =
        editorMerge.ok
            ? runGitCaptureBackgrounded(mergeDir, {"commit", "--no-edit"})
            : editorMerge;
    if (!editorCommit.ok) {
        runGitCapture(mergeDir, {"merge", "--abort"}, nullptr, nullptr);
        const QString back = putCheckoutBack(flow, &step.logs);
        step.message =
            QStringLiteral("Could not merge %1 into %2: %3%4")
                .arg(base, branch,
                     gitFailureDetail(editorCommit,
                                      editorMerge.ok ? QStringLiteral("git commit")
                                                     : QStringLiteral("git merge"),
                                      200),
                     back);
        step.error = true;
        return step;
    }
    putCheckoutBack(flow, &step.logs);
    if (!unmergedFilesIn(mergeDir).isEmpty()) {
        step.message = QStringLiteral("Updated %1 from %2. Its protected local edits "
                                      "now need resolution — click Resolve local "
                                      "edits.")
                           .arg(branch, base);
        step.error = true;
        step.showBranch = true;
        return step;
    }
    step.logs << QStringLiteral("Git: merged %1 into %2.").arg(base, branch);
    step.message = QString::fromUtf8("Updated %1 with %2 \xE2\x80\x94 no conflicts.")
                       .arg(branch, base);
    step.reloadBranchesAndTags = true;
    return step;
}

MainWindow::BranchGitStep MainWindow::finishBranchMergeEditor(BranchGitFlow flow,
                                                              bool committed)
{
    BranchGitStep step;
    step.flow = flow;
    const QString &mergeDir = flow.mergeDir;
    if (!committed) {
        runGitCapture(mergeDir, {"merge", "--abort"}, nullptr, nullptr);
        const QString back = putCheckoutBack(flow, &step.logs);
        step.message = QStringLiteral("Cancelled the merge of %1 into %2; %2 was left "
                                      "unchanged.%3")
                           .arg(flow.base, flow.branch, back);
        step.error = !back.isEmpty();
        return step;
    }
    putCheckoutBack(flow, &step.logs);
    if (!unmergedFilesIn(mergeDir).isEmpty()) {
        step.message = QStringLiteral("Updated %1 from %2. Its protected local edits "
                                      "now need resolution — click Resolve local "
                                      "edits.")
                           .arg(flow.branch, flow.base);
        step.error = true;
        step.showBranch = true;
        return step;
    }
    step.logs << QStringLiteral("Git: merged %1 into %2 (conflicts resolved).")
                     .arg(flow.base, flow.branch);
    step.message = QStringLiteral("Updated %1 with %2.").arg(flow.branch, flow.base);
    step.reloadBranchesAndTags = true;
    return step;
}

MainWindow::BranchGitStep MainWindow::keepResolvedLocalEdits(BranchGitFlow flow,
                                                             int count)
{
    BranchGitStep step;
    step.flow = flow;
    step.logs << QStringLiteral("Git: resolved and restored %1 local file(s) "
                                "in %2 without committing them.")
                     .arg(count)
                     .arg(flow.branch);
    step.message = QStringLiteral("Resolved %1 local file(s) in %2. The edits remain "
                                  "uncommitted for the agent.")
                       .arg(count)
                       .arg(flow.branch);
    step.clearBranchesCache = true;
    step.showBranch = true;
    step.reloadWorktrees = true;
    return step;
}

void MainWindow::applyBranchEditorStep(BranchGitStep step,
                                       std::function<void()> done)
{
    const BranchGitFlow flow = step.flow;
    const QString &branch = flow.branch;
    const QString &base = flow.base;
    if (step.kind == BranchGitStep::Done) {
        m_branchMergeFlowBusy = false;
        showBranchGitStep(step);
        if (done)
            done();
        return;
    }
    if (step.kind == BranchGitStep::LocalConflicts) {
        const QString linkedWorktree = flow.linkedWorktree;
        const QStringList conflicted = step.conflicted;
        const QString intro =
            QString::fromUtf8(
                "Git updated <b>%1</b> from <b>%2</b>, then found overlaps "
                "while restoring the agent's uncommitted edits. Choose the "
                "right result for each conflict. The resolved files stay "
                "uncommitted so the agent can continue normally.")
                .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
        const bool resolved = runMergeConflictEditor(
            QString::fromUtf8("Resolve local edits \xE2\x80\x94 %1").arg(branch),
            intro, linkedWorktree, conflicted,
            QStringLiteral("Keep resolved edits"),
            [linkedWorktree, conflicted](QString *e) {
                QStringList addArgs{"add", "--"};
                addArgs.append(conflicted);
                QStringList resetArgs{"reset", "--"};
                resetArgs.append(conflicted);
                return runGitCapture(linkedWorktree, addArgs, nullptr, e) &&
                       runGitCapture(linkedWorktree, resetArgs, nullptr, e);
            },
            true);
        if (!resolved) {
            m_branchMergeFlowBusy = false;
            noticeForRepo(repoGitDir() == flow.dir,
                          QStringLiteral("Left %1's local-edit conflicts open; its "
                                         "autostash backup is still safe.")
                              .arg(branch),
                          true);
            if (done)
                done();
            return;
        }
        const int count = conflicted.size();
        runOffThread<BranchGitStep>(
            [flow, count] { return keepResolvedLocalEdits(flow, count); },
            [this, done](BranchGitStep next) {
                applyBranchEditorStep(std::move(next), done);
            });
        return;
    }
    for (const QString &line : std::as_const(step.logs))
        logSystem(line);
    const QString intro =
        QString::fromUtf8(
            "Resolve each conflict, then commit the merge into <b>%1</b>. "
            "<b>Ours</b> is %1; <b>theirs</b> is %2. You can also edit the "
            "text directly.")
            .arg(branch.toHtmlEscaped(), base.toHtmlEscaped());
    const QString mergeDir = flow.mergeDir;
    const bool committed = runMergeConflictEditor(
        QString::fromUtf8("Merge editor \xE2\x80\x94 %1").arg(branch), intro,
        mergeDir, step.conflicted, QStringLiteral("Commit merge"),
        [mergeDir](QString *e) { return commitResolvedMerge(mergeDir, e); },
        true);
    runOffThread<BranchGitStep>(
        [flow, committed] { return finishBranchMergeEditor(flow, committed); },
        [this, done](BranchGitStep next) {
            applyBranchEditorStep(std::move(next), done);
        });
}

static QString branchMergeTree(const QString &dir, const QString &base,
                               const QString &branch)
{
    QByteArray out;
    if (!runGitCapture(dir, {"merge-tree", "--write-tree", branch, base}, &out,
                       nullptr))
        return QString(); // non-zero exit == conflicts (or error)
    return QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts).value(0);
}

void MainWindow::startBranchConflictProbes(
    const QString &dir, const QString &base,
    const QList<QPair<QString, QString>> &probes)
{
    if (dir.isEmpty() || base.isEmpty() || probes.isEmpty())
        return;
    QList<QPair<QString, QString>> pending;
    for (const QPair<QString, QString> &probe : probes) {
        if (probe.second.isEmpty() || m_branchConflictProbes.contains(probe.first))
            continue;
        m_branchConflictProbes.insert(probe.first);
        pending.append(probe);
    }
    if (pending.isEmpty())
        return;

    auto verdicts = std::make_shared<QList<bool>>(); // parallel to `pending`
    QThread *worker = QThread::create([dir, base, pending, verdicts] {
        for (const QPair<QString, QString> &probe : pending)
            verdicts->append(branchMergeTree(dir, base, probe.first).isEmpty());
    });
    connect(worker, &QThread::finished, this,
            [this, worker, pending, verdicts, dir, base] {
                worker->deleteLater();
                for (const QPair<QString, QString> &probe : pending)
                    m_branchConflictProbes.remove(probe.first);
                if (m_branchConflictCache.size() > 512)
                    m_branchConflictCache.clear();
                const int answered = qMin(pending.size(), verdicts->size());
                for (int i = 0; i < answered; ++i)
                    m_branchConflictCache.insert(pending.at(i).second,
                                                 verdicts->at(i));
                if (repoGitDir() != dir)
                    return;
                bool refreshDetail = false;
                for (int i = 0; i < answered; ++i) {
                    const QString branch = pending.at(i).first;
                    if (branch == m_branchDiffBranch)
                        refreshDetail = true;
                    if (!verdicts->at(i) || !m_branchesTable)
                        continue;
                    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
                        QTableWidgetItem *name = m_branchesTable->item(
                            row, kBranchesNameColumn);
                        QTableWidgetItem *status = m_branchesTable->item(
                            row, kBranchesStatusColumn);
                        if (!name || !status || name->text() != branch)
                            continue;
                        if (status->text() != kBranchConflictsState) {
                            status->setText(kBranchConflictsState);
                            status->setToolTip(
                                QStringLiteral("Merging %1 into this branch conflicts")
                                    .arg(base));
                            status->setForeground(QColor("#f85149"));
                            name->setData(kBranchConflictRole, true);
                        }
                        break;
                    }
                }
                if (refreshDetail && !m_branchDiffBranch.isEmpty())
                    updateBranchDetailActions(m_branchDiffBranch);
            });
    worker->start();
}

void MainWindow::pullBaseIntoAllBranches()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "This is a read-only mirror; branches can't be updated here.", true);
        return;
    }
    if (m_pullAllBranchesRunning) {
        flashMessage(QStringLiteral("Branches are already being fast-forwarded."));
        return;
    }
    m_branchesCache.clear();
    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (base.isEmpty())
        return;

    m_pullAllBranchesRunning = true;
    startButtonSpin(m_branchPullAllButton);
    QPointer<QPushButton> spinButton = m_branchPullAllButton;
    runOffThread<PullAllBranchesResult>(
        [dir, branches, base] { return fastForwardInactiveBranches(dir, branches, base); },
        [this, dir, base, spinButton](PullAllBranchesResult result) {
            m_pullAllBranchesRunning = false;
            if (spinButton)
                stopButtonSpin(spinButton);
            const bool here = repoGitDir() == dir;
            if (!result.attempted) {
                noticeForRepo(here,
                              QStringLiteral("No inactive branch can be "
                                             "fast-forwarded to %1.")
                                  .arg(base));
                return;
            }
            logSystem(QStringLiteral("Git: fast-forwarded %1 branch(es) to %2; %3 failed.")
                          .arg(result.updated)
                          .arg(base)
                          .arg(result.failed.size()));
            QString summary =
                QStringLiteral("Fast-forwarded %1 inactive branch(es) to %2.")
                    .arg(result.updated)
                    .arg(base);
            if (!result.failed.isEmpty())
                summary += QStringLiteral(" %1 failed (%2).")
                               .arg(result.failed.size())
                               .arg(result.failed.join(QStringLiteral(", ")));
            if (!result.skipped.isEmpty())
                summary += QStringLiteral(" Left active or divergent branches "
                                          "unchanged (%1).")
                               .arg(result.skipped.join(QStringLiteral(", ")));
            if (here) {
                clearBranchDiffCache();
                loadBranchesAndTags();
                loadBranchesPanel();
            }
            noticeForRepo(here, summary);
        });
}

MainWindow::PullAllBranchesResult MainWindow::fastForwardInactiveBranches(
    const QString &dir, const QStringList &branches, const QString &base)
{
    PullAllBranchesResult result;
    const QString currentBranch = checkedOutBranch(dir);
    const auto behindOf = [&](const QString &branch, int *ahead) -> int {
        int behind = 0;
        readBehindAhead(dir, base, branch, &behind, ahead);
        return behind;
    };

    int behindCount = 0;
    for (const QString &branch : branches) {
        if (branch == base || branch == currentBranch)
            continue;
        int ahead = 0;
        if (behindOf(branch, &ahead) > 0 && ahead == 0 &&
            readWorktreePathForBranch(dir, branch, WorktreeLookup::Any).isEmpty())
            ++behindCount;
    }
    if (behindCount == 0)
        return result;
    result.attempted = true;

    for (const QString &branch : branches) {
        if (branch == base)
            continue;
        int ahead = 0;
        const int behind = behindOf(branch, &ahead);
        if (behind == 0)
            continue;
        if (branch == currentBranch ||
            !readWorktreePathForBranch(dir, branch, WorktreeLookup::Any).isEmpty() ||
            ahead > 0) {
            result.skipped << branch;
            continue;
        }
        if (runGitCapture(dir, {"fetch", ".", base + ":" + branch}, nullptr,
                          nullptr))
            ++result.updated;
        else
            result.failed << branch;
    }
    return result;
}

void MainWindow::fixBranchConflictsWithAgent(const QString &branch,
                                             const QString &provider,
                                             const QString &modelArg)
{
    if (m_aiFix) {
        flashMessage("An AI conflict fix is already running; wait for it to finish.",
                     true);
        return;
    }
    if (!m_agentStore || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = writableRecordFor(repo).localPath;
    if (dir.isEmpty()) {
        QMessageBox::warning(this, "Fix with agent",
                             "This repository is read-only on this node.");
        return;
    }
    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (branch.isEmpty() || branch == base || base.isEmpty())
        return;

    // "claude-code" drives the real `claude` CLI (no API key, authenticates via the
    // local login); the two API providers POST each conflicted file to their endpoint.
    const bool claudeCode = provider == QLatin1String("claude-code");
    const bool claude = !claudeCode && agentIsClaudeProvider(provider);
    QString model = modelArg.trimmed();
    if (model.isEmpty() && !claudeCode)
        model = claude ? QStringLiteral("claude-haiku-4-5")
                       : QStringLiteral("gpt-4.1-nano");
    QString apiKey;
    if (!claudeCode) {
        apiKey = (claude ? QSettings().value(kClaudeApiKeySetting)
                         : QSettings().value(kCodexApiKeySetting))
                     .toString()
                     .trimmed();
        if (apiKey.isEmpty()) {
            flashMessage(claude ? "Add a Claude API key in Settings first."
                                : "Add an OpenAI API key in Settings first.",
                         true, QString(),
                         forkmesh::kLocalOperationNoticeKind);
            return;
        }
    }

    if (m_aiFixStarting) {
        flashMessage("An AI conflict fix is already running; wait for it to finish.",
                     true);
        return;
    }
    m_aiFixStarting = true;
    const int repoIndex = m_repoDetailIndex;
    const QString owner = repo.owner;
    const QString name = repo.name;
    runOffThread<BranchFixPrep>(
        [dir, branch, base] { return prepareBranchConflictFix(dir, branch, base); },
        [this, repoIndex, owner, name, branch, base, provider, model, apiKey,
         claudeCode](BranchFixPrep prep) {
            m_aiFixStarting = false;
            const bool here = m_repoDetailIndex == repoIndex &&
                              repoIndexFor(owner, name) == repoIndex;
            for (const QString &line : std::as_const(prep.logs))
                logSystem(line);
            if (prep.kind != BranchFixPrep::Conflicted) {
                noticeForRepo(here, prep.message, prep.error);
                if (here && prep.kind == BranchFixPrep::Merged) {
                    loadBranchesAndTags();
                    loadBranchesPanel();
                }
                return;
            }
            if (!here || m_aiFix || !m_agentStore) {
                const QString mergeDir = prep.mergeDir;
                const QString restoreBranch = prep.restoreBranch;
                runGitDetached(
                    mergeDir, {"merge", "--abort"},
                    [this, mergeDir, restoreBranch, branch](bool, const QByteArray &) {
                        if (!restoreBranch.isEmpty() && restoreBranch != branch)
                            runGitDetached(mergeDir, {"checkout", restoreBranch},
                                           [](bool, const QByteArray &) {});
                    });
                flashMessage(QStringLiteral("Left %1 unchanged: the conflict fix "
                                            "could not start.")
                                 .arg(branch),
                             true);
                return;
            }
            startBranchConflictFix(repoIndex, owner, name, branch, base, provider,
                                   model, apiKey, claudeCode, prep);
        });
}

MainWindow::BranchFixPrep MainWindow::prepareBranchConflictFix(const QString &dir,
                                                               const QString &branch,
                                                               const QString &base)
{
    BranchFixPrep prep;
    prep.error = true;
    int behind = 0, ahead = 0;
    readBehindAhead(dir, base, branch, &behind, &ahead);
    if (behind == 0) {
        prep.message =
            QStringLiteral("%1 is already up to date with %2.").arg(branch, base);
        prep.error = false;
        return prep;
    }

    const QString branchWorktree = readWorktreePathForBranch(dir, branch);
    const bool inBranchWorktree = !branchWorktree.isEmpty();
    const QString mergeDir = inBranchWorktree ? branchWorktree : dir;
    prep.mergeDir = mergeDir;

    QByteArray status;
    if (!runGitCapture(mergeDir, {"status", "--porcelain"}, &status, nullptr) ||
        !status.trimmed().isEmpty()) {
        prep.message = QStringLiteral(
            "Commit or stash local changes before fixing this branch.");
        return prep;
    }

    QString err;
    if (!inBranchWorktree) {
        prep.restoreBranch = checkedOutBranch(dir);
        if (prep.restoreBranch != branch &&
            !checkoutReleasingWorktree(dir, branch, &err)) {
            prep.message =
                QStringLiteral("Could not check out %1: %2").arg(branch, err.left(240));
            return prep;
        }
    }

    const GitCaptureResult fixMerge =
        runGitCaptureBackgrounded(mergeDir, {"merge", "--no-edit", base});
    if (fixMerge.ok) {
        if (!prep.restoreBranch.isEmpty() && prep.restoreBranch != branch)
            runGitCapture(mergeDir, {"checkout", prep.restoreBranch}, nullptr, nullptr);
        prep.logs << QStringLiteral("Git: merged %1 into %2 (no conflicts).")
                         .arg(base, branch);
        prep.message =
            QStringLiteral("Updated %1 with %2 (no conflicts).").arg(branch, base);
        prep.error = false;
        prep.kind = BranchFixPrep::Merged;
        return prep;
    }

    prep.conflicted = unmergedFilesIn(mergeDir);
    if (prep.conflicted.isEmpty()) {
        runGitCapture(mergeDir, {"merge", "--abort"}, nullptr, nullptr);
        if (!prep.restoreBranch.isEmpty() && prep.restoreBranch != branch)
            runGitCapture(mergeDir, {"checkout", prep.restoreBranch}, nullptr, nullptr);
        prep.message =
            QStringLiteral("Could not merge %1 into %2: %3")
                .arg(base, branch,
                     gitFailureDetail(fixMerge, QStringLiteral("git merge"), 160));
        return prep;
    }
    prep.kind = BranchFixPrep::Conflicted;
    return prep;
}

void MainWindow::startBranchConflictFix(int repoIndex, const QString &owner,
                                        const QString &name, const QString &branch,
                                        const QString &base, const QString &provider,
                                        const QString &model, const QString &apiKey,
                                        bool claudeCode, const BranchFixPrep &prep)
{
    const QStringList &conflicted = prep.conflicted;
    const QString &mergeDir = prep.mergeDir;
    const QString &restoreBranch = prep.restoreBranch;
    AgentSession session;
    session.owner = owner;
    session.name = name;
    session.issueNumber = 0; // branch-scoped, not issue- or PR-scoped
    session.issueTitle =
        QStringLiteral("Resolve conflicts merging %1 into %2").arg(base, branch);
    session.provider = provider;
    session.originInstanceId = activeServerInstanceId();
    session.originInstanceUrl = activeServerInstanceUrl();
    session.branchName = branch;
    session.status = AgentStatus::Running;
    session = m_agentStore->createSession(session);
    session.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_agentStore->saveSession(session);
    danceGenie();
    m_agentStore->appendLog(
        session,
        model.isEmpty()
            ? QStringLiteral("==> %1 resolving merge conflicts: %2 into %3.\n")
                  .arg(agentProviderName(provider), base, branch)
            : QStringLiteral("==> %1 (%2) resolving merge conflicts: %3 into %4.\n")
                  .arg(agentProviderName(provider), model, base, branch));

    m_aiFix = new AiConflictFix;
    m_aiFix->branchMerge = true;
    m_aiFix->repoIndex = repoIndex;
    m_aiFix->sessionId = session.id;
    m_aiFix->provider = provider;
    m_aiFix->model = model;
    m_aiFix->apiKey = apiKey;
    m_aiFix->workTree = mergeDir;
    m_aiFix->files = conflicted;
    m_aiFix->branch = branch;
    m_aiFix->baseBranch = base;
    m_aiFix->restoreBranch = restoreBranch;
    m_aiFix->claudeCode = claudeCode;

    switchToAgentsTab(session.id);
    aiFixLog(QStringLiteral("==> %1 file(s) to resolve: %2\n")
                 .arg(conflicted.size())
                 .arg(conflicted.join(QStringLiteral(", "))));
    if (m_aiFix->claudeCode)
        aiFixRunClaudeCode();
    else
        aiFixResolveNextFile();
}

void MainWindow::onBranchDiffAnchorClicked(const QUrl &url)
{
    const QString href = QUrl::fromPercentEncoding(url.toEncoded());
    if (m_branchDiffPullNumber < 0 && branchDiffTakesBranchComments() &&
        (href.startsWith(QLatin1String("thread:")) ||
         url.scheme() == QLatin1String("cmt") ||
         url.scheme() == QLatin1String("threadtoggle") ||
         url.scheme() == QLatin1String("filecomment"))) {
        onBranchReviewAnchorClicked(url);
        return;
    }
    if (m_branchDiffPullNumber >= 0 &&
        (href.startsWith(QLatin1String("thread:")) ||
         url.scheme() == QLatin1String("cmt") ||
         url.scheme() == QLatin1String("threadtoggle") ||
         url.scheme() == QLatin1String("filecomment"))) {
        if (m_currentPullNumber != m_branchDiffPullNumber)
            showPull(m_branchDiffPullNumber);
        const int scroll =
            m_branchDiffView ? m_branchDiffView->verticalScrollBar()->value() : 0;
        const bool inlineAnchor = url.scheme() == QLatin1String("cmt") ||
                                  url.scheme() == QLatin1String("threadtoggle");
        if (inlineAnchor)
            m_pullInlineView = m_branchDiffView;
        onPullDiffAnchorClicked(url); // dialogs pump the event loop; members only
        if (inlineAnchor)
            return; // the inline handler re-renders its own reserved code row
        if (m_branchDiffLastValid)
            renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                                  m_branchDiffLastEmpty, m_branchDiffViewedContext);
        else
            renderBranchScopeDiff();
        if (m_branchDiffView)
            m_branchDiffView->verticalScrollBar()->setValue(scroll);
        return;
    }
    if (url.scheme() == QLatin1String("fmbranch")) {
        const QString target = url.path();
        if (!target.isEmpty()) {
            clearMergedBranchFlash();
            switchToBranch(target);
        }
        return;
    }
    if (url.scheme() == QLatin1String("fmcommit")) {
        const QString sha = url.path();
        if (!sha.isEmpty()) {
            showOverviewCommits();
            showCommit(sha);
        }
        return;
    }
    if (url.scheme() == QLatin1String("fmmergeshare")) {
        MergeCertificateData certificate;
        const QString platform = url.path().toLower();
        const QUrl composer =
            mergeCertificateDataFromUrl(url, &certificate)
                ? mergeCertificateComposerUrl(platform, certificate)
                : QUrl();
        if (composer.isEmpty()) {
            flashMessage(QStringLiteral("That certificate share link is no "
                                        "longer valid."),
                         true);
            return;
        }
        const QImage image = mergeCertificateImage(certificate);
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (image.isNull() || !clipboard) {
            flashMessage(QStringLiteral("Could not create the certificate "
                                        "image."),
                         true);
            return;
        }
        clipboard->setImage(image);
        const bool opened = QDesktopServices::openUrl(composer);
        const QString service =
            platform == QLatin1String("reddit")
                ? QStringLiteral("Reddit")
                : (platform == QLatin1String("twitter")
                       ? QStringLiteral("Twitter")
                       : QStringLiteral("Mastodon"));
        flashMessage(
            opened
                ? QStringLiteral("Certificate image copied. Paste it into the "
                                 "%1 composer to attach it.")
                      .arg(service)
                : QStringLiteral("Certificate image copied, but the %1 "
                                 "composer could not be opened.")
                      .arg(service),
            !opened);
        return;
    }
    if (url.scheme() == QLatin1String("fmmergerange")) {
        showMergeRange(url.path());
        return;
    }
    if (url.scheme() == QLatin1String("retry") ||
        url.scheme() == QLatin1String("repairdiff")) {
        if (url.scheme() == QLatin1String("repairdiff")) {
            // Re-query worktree porcelain rather than trusting the path that
            // produced this failure. An ignored container is not a worktree;
            // an empty result intentionally makes the next read use the
            // committed branch range instead.
            m_branchDiffWorkDir = branchWorkDir(m_branchDiffBranch);
        }
        setBranchDiffHtml(
            QStringLiteral("<p style='color:#8b949e'>%1 %2…</p>")
                .arg(url.scheme() == QLatin1String("repairdiff")
                         ? QStringLiteral("Repairing workspace and retrying")
                         : QStringLiteral("Retrying"))
                .arg(m_branchDiffBranch.toHtmlEscaped()),
            /*endCap=*/false);
        renderBranchScopeDiff();
        return;
    }
    if (url.scheme() != QLatin1String("viewed") &&
        url.scheme() != QLatin1String("expand"))
        return;
    const QString path = url.path();
    const QString context = m_branchDiffViewedContext.isEmpty()
                                ? QStringLiteral("branch/") + m_branchDiffBranch
                                : m_branchDiffViewedContext;
    const bool folded = toggleDiffExpandedFromAnchor(url, context);
    QString next;
    if (!folded) {
        const bool nowViewed = !loadDiffViewed(context).contains(path);
        const int at = m_branchDiffFilePaths.indexOf(path);
        if (nowViewed && at >= 0 && at + 1 < m_branchDiffFilePaths.size())
            next = m_branchDiffFilePaths.at(at + 1);
        setDiffViewed(context, path, nowViewed);
    }
    // The whole diff is one QTextDocument, so re-rendering it to fold a single
    // file re-lays out every other file and visibly redraws the pane. Swap the
    // one block instead whenever the document can carry it.
    if (swapBranchDiffFileInPlace(path, context)) {
        if (!next.isEmpty())
            scrollBranchDiffToFile(next, /*moveFocus=*/false);
        return;
    }
    const int scroll =
        m_branchDiffView ? m_branchDiffView->verticalScrollBar()->value() : 0;
    if (m_branchDiffLastValid)
        renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                              m_branchDiffLastEmpty, context);
    else
        renderBranchScopeDiff();
    if (!m_branchDiffView)
        return;
    // Folding moves every row below the toggled file, so the old scroll value
    // no longer points at what the reviewer was looking at: land on the file
    // they just opened or closed instead.
    if (!next.isEmpty())
        scrollBranchDiffToFile(next, /*moveFocus=*/false);
    else if (folded)
        scrollBranchDiffToFile(path, /*moveFocus=*/false);
    else
        m_branchDiffView->verticalScrollBar()->setValue(scroll);
}

// Folds or reopens one file of the branch diff without touching the rest of the
// document. False when the swap is not available and the caller must re-render.
bool MainWindow::swapBranchDiffFileInPlace(const QString &path,
                                           const QString &context)
{
    if (!m_branchDiffView || m_branchDiffRenderCancel)
        return false;
    const int at = m_branchDiffFilePaths.indexOf(path);
    if (at < 0 || at >= m_branchDiffFileAnchors.size())
        return false;
    const QSet<QString> viewed = loadDiffViewed(context);
    const QSet<QString> expanded = diffExpandedViewed(context);
    if (!swapDiffFileViewedState(m_branchDiffView, m_branchDiffFileAnchors.at(at),
                                 viewed.contains(path), expanded.contains(path))
             .applied)
        return false;
    m_branchDiffRenderKey.clear();
    // Everything above the swapped file kept its geometry, so the scroll value
    // still points at what the reviewer was reading. Only the marks measured
    // off the old layout have to be rebuilt.
    updateBranchDiffSummary(diffRenderedFiles(m_branchDiffView), viewed,
                            m_branchDiffHealthPatch, context);
    updateRangeViewedLabel(m_branchDiffFilePaths);
    m_branchFileTops.clear();
    m_branchStickyFile.clear();
    rebuildBranchDiffSpans();
    updateBranchDiffActiveOutline();
    if (m_branchDiffSearchBar && m_branchDiffSearchBar->isVisible())
        branchDiffSearchRecompute();
    return true;
}

void MainWindow::computeBranchFileTops()
{
    m_branchFileTops = QList<int>(m_branchDiffFileSpans.size(), -1);
    if (!m_branchDiffView || m_branchDiffFileSpans.isEmpty())
        return;
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    const int viewTop = vbar ? vbar->value() : 0;
    QTextDocument *doc = m_branchDiffView->document();
    for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
        QTextCursor cur(doc);
        cur.setPosition(
            qMin(m_branchDiffFileSpans.at(i).first, doc->characterCount() - 1));
        // The anchor sits on a character inside the header cell, so its cursor
        // rect starts at the text line — a cell padding below the row's own top
        // edge, which is what the outline has to line up with.
        const QRectF header =
            diffFileRowRect(m_branchDiffView, m_branchDiffFileSpans.at(i).first);
        m_branchFileTops[i] = header.isNull()
                                  ? m_branchDiffView->cursorRect(cur).top() + viewTop
                                  : qRound(header.top());
    }
}

void MainWindow::updateBranchDiffSticky()
{
    if (!m_branchDiffView || !m_branchDiffSticky)
        return;
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    if (!vbar)
        return;
    if (vbar->value() <= vbar->minimum()) {
        m_branchDiffSticky->hide();
        return;
    }
    if (m_branchDiffFileSpans.isEmpty()) {
        m_branchDiffSticky->hide();
        return;
    }
    const int viewTop = vbar->value();
    const int viewBottom = viewTop + m_branchDiffView->viewport()->height();
    const int docHeight =
        m_branchDiffView->document()->documentLayout()->documentSize().height();
    if (m_branchFileTops.size() != m_branchDiffFileSpans.size())
        computeBranchFileTops();

    int idx = -1, fileTop = 0, fileBottom = 0;
    for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
        if (m_branchFileTops.at(i) < 0)
            continue;
        const int bottom =
            (i + 1 < m_branchFileTops.size() && m_branchFileTops.at(i + 1) >= 0)
                ? m_branchFileTops.at(i + 1)
                : docHeight;
        if (bottom > viewTop) {
            idx = i;
            fileTop = m_branchFileTops.at(i);
            fileBottom = bottom;
            break;
        }
    }
    if (idx < 0) {
        for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
            if (m_branchDiffFileSpans.at(i).second == m_branchStickyFile) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            idx = 0;
        fileTop = 0;
        fileBottom = qMax(1, docHeight);
    }
    const QString cur = m_branchDiffFileSpans.at(idx).second;

    // The file at the top of the diff is the file currently being reviewed.
    // Keep the CHANGES row and green section outline attached to it when the
    // user scrolls, not to whichever row was clicked before scrolling began.
    if (!m_scmSuppressFileScroll && cur != m_branchActiveFile) {
        m_branchActiveFile = cur;
        if (QTreeWidgetItem *item = scmFindItem(cur, /*staged=*/false)) {
            setScmCurrentItemSilently(item);
            m_scmTree->scrollToItem(item);
        }
    }

    double progress = 1.0;
    if (fileBottom > fileTop)
        progress = double(viewBottom - fileTop) / double(fileBottom - fileTop);
    progress = qBound(0.0, progress, 1.0);

    const QString viewedContext = m_branchDiffViewedContext.isEmpty()
                                      ? QStringLiteral("branch/") + m_branchDiffBranch
                                      : m_branchDiffViewedContext;
    const bool isViewed = loadDiffViewed(viewedContext).contains(cur);
    if (cur != m_branchStickyFile) {
        m_branchStickyFile = cur;
        if (m_branchStickyPath) {
            const QString label = m_branchStickyLabelHtml.value(cur);
            m_branchStickyPath->setText(label.isEmpty() ? diffStickyPathHtml(cur)
                                                        : label);
            m_branchStickyPath->setToolTip(cur); // the bar may clip a long path
        }
    }
    if (m_branchStickyControls)
        m_branchStickyControls->setText(diffRowControlsHtml(
            cur, progress, isViewed, m_branchDiffPullNumber >= 0, QString(),
            diffExpandedViewed(viewedContext).contains(cur)));

    const bool wasHidden = m_branchDiffSticky->isHidden();
    const int stickyHeight = m_branchDiffSticky->sizeHint().height();
    placeDiffOverlay(m_branchDiffSticky, m_branchDiffView,
                     QRect(0, diffStickyTopOffset(stickyHeight, viewTop, fileBottom),
                           m_branchDiffView->viewport()->width(), stickyHeight));
    if (wasHidden) {
        // Only when it comes back: raise() dirties the widget, and doing that
        // on every scroll tick is what makes the bar flicker.
        m_branchDiffSticky->show();
        m_branchDiffSticky->raise();
        if (m_branchDiffActiveOutline && m_branchDiffActiveOutline->isVisible())
            m_branchDiffActiveOutline->raise();
    }
}

void MainWindow::updateBranchDiffActiveOutline()
{
    if (!m_branchDiffView || !m_branchDiffActiveOutline)
        return;
    // m_branchFileTops runs parallel to the spans, not to the file list: on a
    // paged diff the spans only cover the files of the current page.
    int index = -1;
    if (!m_branchActiveFile.isEmpty()) {
        for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
            if (m_branchDiffFileSpans.at(i).second == m_branchActiveFile) {
                index = i;
                break;
            }
        }
    }
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    if (index < 0 || !vbar || m_branchDiffFileSpans.isEmpty()) {
        m_branchDiffActiveOutline->hide();
        return;
    }
    if (m_branchFileTops.size() != m_branchDiffFileSpans.size())
        computeBranchFileTops();
    if (index >= m_branchFileTops.size() || m_branchFileTops.at(index) < 0) {
        m_branchDiffActiveOutline->hide();
        return;
    }
    const int docHeight =
        m_branchDiffView->document()->documentLayout()->documentSize().height();
    const int top = m_branchFileTops.at(index);
    const int bottom = (index + 1 < m_branchFileTops.size() &&
                        m_branchFileTops.at(index + 1) >= 0)
                           ? m_branchFileTops.at(index + 1)
                           : docHeight;

    const int viewTop = vbar->value();
    const int height = m_branchDiffView->viewport()->height();
    if (bottom - viewTop <= 0 || top - viewTop >= height) {
        m_branchDiffActiveOutline->hide();
        return;
    }
    // Sit on the row's own edges. A diff row runs the full width of the
    // document, so the outline starts at the document's left edge and is as
    // wide as the row: anything narrower leaves the row's border showing
    // alongside it, anything offset pushes its right edge off the viewport.
    QScrollBar *hbar = m_branchDiffView->horizontalScrollBar();
    int left = -(hbar ? hbar->value() : 0);
    int width = m_branchDiffView->viewport()->width();
    const QRectF headerRect = diffFileRowRect(
        m_branchDiffView, m_branchDiffFileSpans.at(index).first);
    if (!headerRect.isNull() && headerRect.width() > 0)
        width = qRound(headerRect.width());
    int outlineTop = top - viewTop;
    if (outlineTop < 0 && m_branchDiffSticky)
        outlineTop = qMax(outlineTop,
                          diffStickyTopOffset(
                              m_branchDiffSticky->sizeHint().height(), viewTop,
                              bottom));
    const bool wasHidden = m_branchDiffActiveOutline->isHidden();
    placeDiffOverlay(
        m_branchDiffActiveOutline, m_branchDiffView,
        QRect(left, outlineTop, width, qMax(1, bottom - viewTop - outlineTop)));
    if (wasHidden) {
        m_branchDiffActiveOutline->show();
        m_branchDiffActiveOutline->raise();
    }
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testBranchOutlinedFile() const
{
    return m_branchDiffActiveOutline && m_branchDiffActiveOutline->isVisible()
               ? m_branchActiveFile
               : QString();
}

QRect MainWindow::testBranchOutlineRect() const
{
    return m_branchDiffActiveOutline ? m_branchDiffActiveOutline->geometry()
                                     : QRect();
}

// Viewport-space rectangle of the active file's header row, so a test can hold
// the outline against the thing it is supposed to be drawn around.
QRect MainWindow::testBranchActiveRowRect() const
{
    if (!m_branchDiffView || m_branchActiveFile.isEmpty())
        return {};
    int index = -1;
    for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
        if (m_branchDiffFileSpans.at(i).second == m_branchActiveFile) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return {};
    const QRectF header =
        diffFileRowRect(m_branchDiffView, m_branchDiffFileSpans.at(index).first);
    if (header.isNull())
        return {};
    QScrollBar *hbar = m_branchDiffView->horizontalScrollBar();
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    return QRect(qRound(header.left()) - (hbar ? hbar->value() : 0),
                 qRound(header.top()) - (vbar ? vbar->value() : 0),
                 qRound(header.width()), qRound(header.height()));
}

QString MainWindow::testBranchDiffSummaryText() const
{
    if (!m_branchDiffSummaryBar || !m_branchDiffSummaryBar->isVisible() ||
        !m_branchDiffSummaryLabel)
        return {};
    QTextDocument doc;
    doc.setHtml(m_branchDiffSummaryLabel->text());
    return doc.toPlainText().simplified();
}
#endif

void MainWindow::applyBranchAutoMarkViewedOnScroll()
{
    if (!m_branchDiffView || m_branchDiffFileSpans.isEmpty() ||
        !m_branchDiffLastValid || !autoMarkViewedOnScrollPref() ||
        diffIsPaginated(m_branchDiffView))
        return;
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    if (!vbar)
        return;
    const int viewBottom = vbar->value() + m_branchDiffView->viewport()->height();
    const int docHeight =
        m_branchDiffView->document()->documentLayout()->documentSize().height();
    if (m_branchFileTops.size() != m_branchDiffFileSpans.size())
        computeBranchFileTops();

    const QString context = m_branchDiffViewedContext.isEmpty()
                                ? QStringLiteral("branch/") + m_branchDiffBranch
                                : m_branchDiffViewedContext;
    const QSet<QString> viewed = loadDiffViewed(context);
    QString currentFile; // first file the reviewer hasn't fully scrolled through
    QStringList newlyViewed;
    for (int i = 0; i < m_branchDiffFileSpans.size(); ++i) {
        if (m_branchFileTops.at(i) < 0)
            continue;
        const int bottom =
            (i + 1 < m_branchFileTops.size() && m_branchFileTops.at(i + 1) >= 0)
                ? m_branchFileTops.at(i + 1)
                : docHeight;
        const QString &path = m_branchDiffFileSpans.at(i).second;
        if (bottom <= viewBottom) {
            if (!viewed.contains(path))
                newlyViewed << path;
        } else if (currentFile.isEmpty()) {
            currentFile = path;
        }
    }
    if (newlyViewed.isEmpty())
        return;
    // Folding under the reviewer's own scroll is the worst place to re-render
    // the whole diff, so take the in-place route file by file and only fall
    // back once — for whichever file the document could not carry.
    bool swappedEvery = true;
    for (const QString &path : std::as_const(newlyViewed)) {
        setDiffViewed(context, path, true);
        swappedEvery = swapBranchDiffFileInPlace(path, context) && swappedEvery;
    }
    if (!swappedEvery)
        renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                              m_branchDiffLastEmpty, context);
    // Either way the files that folded were above the viewport, so the rows the
    // reviewer is reading rose by their height: land on the first file they
    // have not finished instead of leaving them mid-scroll somewhere else.
    if (!currentFile.isEmpty()) {
        const int idx = m_branchDiffFilePaths.indexOf(currentFile);
        if (idx >= 0 && idx < m_branchDiffFileAnchors.size()) {
            scrollDiffToAnchor(m_branchDiffView,
                               m_branchDiffFileAnchors.at(idx));
        }
    }
}

bool MainWindow::branchScrollToAdjacentHunk(int delta)
{
    if (!m_branchDiffView)
        return false;
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    if (!vbar)
        return false;
    flushDiffStream(m_branchDiffView);
    const int curTop = vbar->value();
    int target = delta > 0 ? std::numeric_limits<int>::max()
                           : std::numeric_limits<int>::min();
    QTextCursor cur(m_branchDiffView->document());
    while (true) {
        cur = m_branchDiffView->document()->find(QStringLiteral("@@ -"), cur);
        if (cur.isNull())
            break;
        QTextCursor lineCur(cur);
        lineCur.setPosition(cur.selectionStart());
        lineCur.movePosition(QTextCursor::StartOfLine);
        const int y = m_branchDiffView->cursorRect(lineCur).top() + curTop;
        if (delta > 0) {
            if (y > curTop + 4)
                target = std::min(target, y);
        } else if (y < curTop - 4) {
            target = std::max(target, y);
        }
    }
    if (delta > 0 ? target == std::numeric_limits<int>::max()
                  : target == std::numeric_limits<int>::min()) {
        return showDiffPageContainingText(
            m_branchDiffView, QStringLiteral("@@ -"), delta,
            /*wrap=*/false);
    }
    vbar->setValue(std::clamp(target - 4, vbar->minimum(), vbar->maximum()));
    return true;
}

void MainWindow::toggleBranchDiffSearch(bool show)
{
    if (!m_branchDiffSearchBar || !m_branchDiffSearchInput)
        return;
    m_branchDiffSearchBar->setVisible(show);
    if (show) {
        m_branchDiffSearchInput->setFocus();
        m_branchDiffSearchInput->selectAll();
    } else {
        m_branchDiffSearchInput->clear(); // triggers recompute; clears highlights
        if (m_branchDiffView)
            m_branchDiffView->setFocus();
    }
}

void MainWindow::branchDiffSearchRecompute()
{
    if (!m_branchDiffView)
        return;
    m_branchDiffSearchMatches.clear();
    m_branchDiffSearchIndex = -1;

    const QString term =
        m_branchDiffSearchInput ? m_branchDiffSearchInput->text() : QString();
    if (!term.isEmpty()) {
        flushDiffStream(m_branchDiffView);
        if (diffStreamInProgress(m_branchDiffView)) {
            applyDiffSearchHighlights(
                m_branchDiffView, m_branchDiffSearchMatches,
                m_branchDiffSearchIndex, m_branchDiffSearchCount, false);
            return; // the finished hook retries against the complete page
        }
        QTextCursor cur = m_branchDiffView->document()->find(term);
        while (!cur.isNull()) {
            m_branchDiffSearchMatches.append(cur);
            if (m_branchDiffSearchMatches.size() >= 5000)
                break; // safety cap on pathological match counts
            cur = m_branchDiffView->document()->find(term, cur);
        }
        if (!m_branchDiffSearchMatches.isEmpty()) {
            const int arrival =
                m_branchDiffView->property("forkmeshDiffSearchArrival").toInt();
            m_branchDiffView->setProperty("forkmeshDiffSearchArrival", QVariant());
            m_branchDiffSearchIndex =
                arrival < 0 ? m_branchDiffSearchMatches.size() - 1 : 0;
        } else {
            m_branchDiffView->setProperty("forkmeshDiffSearchArrival", 1);
            if (showDiffPageContainingText(m_branchDiffView, term, 1,
                                           /*wrap=*/true)) {
                return;
            }
            m_branchDiffView->setProperty("forkmeshDiffSearchArrival", QVariant());
        }
    }

    applyDiffSearchHighlights(m_branchDiffView, m_branchDiffSearchMatches,
                              m_branchDiffSearchIndex, m_branchDiffSearchCount,
                              term.isEmpty());
    if (m_branchDiffSearchIndex >= 0)
        branchDiffSearchGoTo(0);
}

void MainWindow::branchDiffSearchGoTo(int delta)
{
    if (!m_branchDiffView || m_branchDiffSearchMatches.isEmpty())
        return;
    QScrollBar *vbar = m_branchDiffView->verticalScrollBar();
    if (!vbar)
        return;
    const int count = m_branchDiffSearchMatches.size();
    if (delta != 0 &&
        ((delta > 0 && m_branchDiffSearchIndex == count - 1) ||
         (delta < 0 && m_branchDiffSearchIndex == 0))) {
        const QString term = m_branchDiffSearchInput
                                 ? m_branchDiffSearchInput->text()
                                 : QString();
        if (!term.isEmpty()) {
            m_branchDiffView->setProperty("forkmeshDiffSearchArrival", delta);
            if (showDiffPageContainingText(m_branchDiffView, term, delta,
                                           /*wrap=*/true)) {
                return;
            }
            m_branchDiffView->setProperty("forkmeshDiffSearchArrival", QVariant());
        }
    }
    m_branchDiffSearchIndex =
        ((m_branchDiffSearchIndex + delta) % count + count) % count;
    applyDiffSearchHighlights(m_branchDiffView, m_branchDiffSearchMatches,
                              m_branchDiffSearchIndex, m_branchDiffSearchCount,
                              false);
    const QTextCursor &target =
        m_branchDiffSearchMatches.at(m_branchDiffSearchIndex);
    QTextCursor lineCur(target);
    lineCur.setPosition(target.selectionStart());
    const int y = m_branchDiffView->cursorRect(lineCur).top() + vbar->value();
    const int centered = y - m_branchDiffView->viewport()->height() / 3;
    vbar->setValue(std::clamp(centered, vbar->minimum(), vbar->maximum()));
}

QString MainWindow::diffViewedScope(const QString &context) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const RepositoryRecord &r = m_repositories.at(m_repoDetailIndex);
    return r.owner + QStringLiteral("/") + r.name + QStringLiteral("/") + context;
}

namespace {
// A stored Viewed mark is "<fingerprint> <path>", the fingerprint being what
// that file's diff hashed to when the mark was made. Marks written before
// fingerprints existed are the bare path: they carry no fingerprint, so they
// are left alone until a render can stamp them. The fingerprint leads because
// it is fixed-width hex, which a path is free to contain but never to end at a
// space in exactly that many characters.
QString encodeDiffViewedMark(const QString &path, const QString &stamp)
{
    return stamp.size() == forkmesh::ui::kDiffFingerprintChars
               ? stamp + QLatin1Char(' ') + path
               : path;
}

// -> (path, stamp)
QPair<QString, QString> decodeDiffViewedMark(const QString &mark)
{
    constexpr int n = forkmesh::ui::kDiffFingerprintChars;
    if (mark.size() <= n || mark.at(n) != QLatin1Char(' '))
        return {mark, QString()};
    for (int i = 0; i < n; ++i) {
        const QChar c = mark.at(i);
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return {mark, QString()};
    }
    return {mark.mid(n + 1), mark.left(n)};
}

struct DiffViewedFile {
    QString path;
    QHash<QString, QStringList> scopes;
};

QByteArray serializeDiffViewed(const QHash<QString, QStringList> &scopes)
{
    QJsonObject object;
    for (auto it = scopes.cbegin(); it != scopes.cend(); ++it)
        object.insert(it.key(), QJsonArray::fromStringList(it.value()));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

DiffViewedFile &diffViewedFile()
{
    static DiffViewedFile file;
    QSettings settings;
    const QString path = QFileInfo(settings.fileName()).absolutePath() +
                         QStringLiteral("/diff-viewed.json");
    if (file.path == path)
        return file;
    file.path = path;
    file.scopes.clear();
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        const QJsonObject object =
            QJsonDocument::fromJson(existing.readAll()).object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            QStringList marks;
            for (const QJsonValue &mark : it.value().toArray())
                marks.append(mark.toString());
            if (!marks.isEmpty())
                file.scopes.insert(it.key(), marks);
        }
        return file;
    }
    settings.beginGroup(QStringLiteral("diffViewed"));
    const QStringList keys = settings.allKeys();
    for (const QString &key : keys) {
        const QStringList marks = settings.value(key).toStringList();
        if (!marks.isEmpty())
            file.scopes.insert(key, marks);
    }
    settings.endGroup();
    if (keys.isEmpty())
        return file;
    if (QString error; !forkmesh::diskwrite::writeNow(
            path, serializeDiffViewed(file.scopes), &error)) {
        qWarning() << "diff viewed marks:" << error;
        return file;
    }
    settings.remove(QStringLiteral("diffViewed"));
    return file;
}
} // namespace

QHash<QString, QString> MainWindow::loadDiffViewedStamps(const QString &context) const
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return {};
    const QStringList list = diffViewedFile().scopes.value(scope);
    QHash<QString, QString> stamps;
    stamps.reserve(list.size());
    for (const QString &mark : list) {
        const QPair<QString, QString> parsed = decodeDiffViewedMark(mark);
        stamps.insert(parsed.first, parsed.second);
    }
    return stamps;
}

void MainWindow::saveDiffViewedStamps(const QString &context,
                                      const QHash<QString, QString> &stamps)
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return;
    QStringList marks;
    marks.reserve(stamps.size());
    for (auto it = stamps.cbegin(); it != stamps.cend(); ++it)
        marks.append(encodeDiffViewedMark(it.key(), it.value()));
    DiffViewedFile &file = diffViewedFile();
    if (marks.isEmpty())
        file.scopes.remove(scope);
    else
        file.scopes.insert(scope, marks);
    forkmesh::diskwrite::writeLater(file.path, serializeDiffViewed(file.scopes));
    if (context.startsWith(QLatin1String("pull/")))
        setPullViewedCount(context.mid(5).toInt(), marks.size());
}

QSet<QString> MainWindow::loadDiffViewed(const QString &context) const
{
    const QHash<QString, QString> stamps = loadDiffViewedStamps(context);
    return QSet<QString>(stamps.keyBegin(), stamps.keyEnd());
}

bool MainWindow::diffViewedMarksCurrent(const QString &context,
                                        const QStringList &paths) const
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return false;
    const QHash<QString, QString> stamps = loadDiffViewedStamps(context);
    const QHash<QString, QString> fingerprints = m_diffFileFingerprints.value(scope);
    for (const QString &path : paths) {
        const auto stamp = stamps.constFind(path);
        if (stamp == stamps.cend())
            continue;
        if (stamp->isEmpty() || *stamp != fingerprints.value(path))
            return false;
    }
    return true;
}

QSet<QString> MainWindow::refreshDiffViewedForPatch(const QString &context,
                                                    const QString &patch)
{
    if (diffViewedScope(context).isEmpty())
        return {};
    return refreshDiffViewedForFingerprints(
        context, forkmesh::ui::diffFileFingerprints(patch));
}

QSet<QString> MainWindow::refreshDiffViewedForFingerprints(
    const QString &context, const QHash<QString, QString> &fingerprints)
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return {};
    // Held for setDiffViewed(): a tick lands after the render that drew the
    // file, so this is the fingerprint the reviewer was looking at.
    m_diffFileFingerprints.insert(scope, fingerprints);
    const DiffViewedPrune prune =
        pruneDiffViewedStamps(loadDiffViewedStamps(context), fingerprints);
    // Losing the mark settles the fold with it: a file the reviewer had
    // reopened while it counted as viewed opens on its own terms now, and
    // leaving the override behind would make their next tick on it look
    // like a no-op (see setDiffViewed).
    for (const QString &path : prune.dropped)
        setDiffExpandedViewed(context, path, false);
    if (prune.changed)
        saveDiffViewedStamps(context, prune.stamps);
    return QSet<QString>(prune.stamps.keyBegin(), prune.stamps.keyEnd());
}

void MainWindow::setDiffViewed(const QString &context, const QString &path, bool viewed)
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return;
    QHash<QString, QString> stamps = loadDiffViewedStamps(context);
    if (viewed)
        stamps.insert(path, m_diffFileFingerprints.value(scope).value(path));
    else
        stamps.remove(path);
    saveDiffViewedStamps(context, stamps);
    // Either direction settles the fold: ticking the pill closes the file the
    // reviewer had reopened, and unticking it opens the file on its own terms.
    // Leaving the override behind would make the next tick look like a no-op.
    auto it = m_diffExpandedViewed.find(scope);
    if (it != m_diffExpandedViewed.end() && it->remove(path) && it->isEmpty())
        m_diffExpandedViewed.erase(it);
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testDiffViewedSurvivesPatch(const QString &context,
                                             const QString &path,
                                             const QString &patch,
                                             const QString &next)
{
    refreshDiffViewedForPatch(context, patch); // what the reviewer looked at
    setDiffViewed(context, path, true);
    return refreshDiffViewedForPatch(context, next).contains(path);
}
#endif

QSet<QString> MainWindow::diffExpandedViewed(const QString &context) const
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty())
        return {};
    return m_diffExpandedViewed.value(scope);
}

void MainWindow::setDiffExpandedViewed(const QString &context,
                                       const QString &path, bool expanded)
{
    const QString scope = diffViewedScope(context);
    if (scope.isEmpty() || path.isEmpty())
        return;
    if (expanded) {
        m_diffExpandedViewed[scope].insert(path);
        return;
    }
    auto it = m_diffExpandedViewed.find(scope);
    if (it != m_diffExpandedViewed.end() && it->remove(path) && it->isEmpty())
        m_diffExpandedViewed.erase(it);
}

bool MainWindow::toggleDiffExpandedFromAnchor(const QUrl &url,
                                              const QString &context)
{
    if (url.scheme() != QLatin1String("expand"))
        return false;
    const QString path = url.path();
    if (!path.isEmpty())
        setDiffExpandedViewed(context, path,
                              !diffExpandedViewed(context).contains(path));
    return true;
}
