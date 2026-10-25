
#include "MainWindowInternal.h"

#include "ActionStore.h"
#include "AgentStore.h"
#include "DiffIntraLine.h"
#include "MarkdownRender.h"
#include "StartupSplash.h"

#include <QTextFrame>
#include <QTextTable>
#include <QThreadPool>

// Qt's built-in image plugins cover PNG/JPEG/GIF/BMP/ICO, but WebP only arrives
// with the optional qtimageformats package, which plenty of Linux desktops
// never install — so a .webp before/after preview would tell the reviewer their
// image is "unrecognized". libwebp's decoder is a far smaller and much more
// commonly present dependency, so decode those frames ourselves where it exists
// (see FORKMESH_HAVE_WEBP in CMakeLists.txt).
#ifdef FORKMESH_HAVE_WEBP
#include <webp/decode.h>
#endif

namespace forkmesh::ui {

QString openAiAuthHeader(const QString &apiKey)
{
    return QStringLiteral("Bearer ") + apiKey.trimmed();
}

QNetworkRequest openAiRequest(const QUrl &url, const QString &apiKey)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", openAiAuthHeader(apiKey).toUtf8());
    request.setRawHeader("Accept", "application/json");
    return request;
}

QString apiErrorSummary(QNetworkReply *reply, const QByteArray &body)
{
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString message = reply->errorString();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QString apiMessage =
        doc.object().value("error").toObject().value("message").toString();
    if (!apiMessage.isEmpty())
        message = apiMessage;
    if (status > 0)
        return QStringLiteral("HTTP %1: %2").arg(status).arg(message);
    return message;
}

QString openAiResponseText(const QJsonObject &obj)
{
    const QString direct = obj.value(QStringLiteral("output_text")).toString().trimmed();
    if (!direct.isEmpty())
        return direct;

    QStringList parts;
    const QJsonArray output = obj.value(QStringLiteral("output")).toArray();
    for (const QJsonValue &outputValue : output) {
        const QJsonArray content =
            outputValue.toObject().value(QStringLiteral("content")).toArray();
        for (const QJsonValue &contentValue : content) {
            const QJsonObject contentObj = contentValue.toObject();
            const QString text = contentObj.value(QStringLiteral("text")).toString();
            if (!text.trimmed().isEmpty())
                parts << text.trimmed();
        }
    }
    return parts.join(QStringLiteral("\n\n")).trimmed();
}

double openAiAskCostUsd(const QJsonObject &response, qint64 *inTokens,
                        qint64 *outTokens)
{
    const QJsonObject usage = response.value(QStringLiteral("usage")).toObject();
    const qint64 input = static_cast<qint64>(
        usage.value(QStringLiteral("input_tokens")).toDouble());
    const qint64 output = static_cast<qint64>(
        usage.value(QStringLiteral("output_tokens")).toDouble());
    if (inTokens)
        *inTokens = input;
    if (outTokens)
        *outTokens = output;
    constexpr double kInputPerMillion = 0.10;
    constexpr double kOutputPerMillion = 0.40;
    return (input * kInputPerMillion + output * kOutputPerMillion) / 1000000.0;
}

QString displayMirrorBranchNameForRef(const QString &ref)
{
    if (ref.startsWith(QLatin1String("refs/heads/")))
        return ref.mid(QStringLiteral("refs/heads/").size());
    if (!ref.startsWith(QLatin1String("refs/remotes/")))
        return QString();
    const QString remotePath = ref.mid(QStringLiteral("refs/remotes/").size());
    const int slash = remotePath.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString();
    const QString name = remotePath.mid(slash + 1);
    if (name.isEmpty() || name == QLatin1String("HEAD"))
        return QString();
    return name;
}

// code-health: allow(blocking-wait) sync primitive called only by mirrorHeadBranch/mirrorBranchCommit below, whose frequent callers (SCM outgoing probe, commit-marker probe, sync-fetch prep) run on QtConcurrent/runOffThread/QThread workers; branch refs resolve from the loose/packed ref files, so only tags, HEAD and refs with no loose or packed entry reach the 5 s-capped git rev-parse
QString mirrorCommitForRef(const QString &mirrorPath, const QString &ref)
{
    if (!QDir(mirrorPath).exists() || ref.trimmed().isEmpty() ||
        ref.contains(QChar('\0')))
        return QString();
    // Branches and remote-tracking refs point straight at commits, so the
    // loose/packed ref file *is* the answer — no process. Tags may need
    // peeling and HEAD may be symbolic, so those still ask git below.
    const QString trimmed = ref.trimmed();
    if (trimmed.startsWith(QLatin1String("refs/heads/")) ||
        trimmed.startsWith(QLatin1String("refs/remotes/"))) {
        const QString tip = refTipFromFiles(mirrorPath, trimmed);
        if (!tip.isEmpty())
            return tip;
    }
    QProcess p;
    p.start("git", {"-C", mirrorPath, "rev-parse", "--verify", "-q",
                    ref + QStringLiteral("^{commit}")});
    if (!p.waitForFinished(5000) || p.exitCode() != 0)
        return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// code-health: allow(blocking-wait) sync primitive with callers on workers (SCM outgoing probe, commit-marker probe, sync-fetch prep) and a few UI-thread reads; HEAD comes from the HEAD file and the branch tip from the ref files, so git (5 s cap each) runs only for a detached/unreadable HEAD or a branch with no ref file
QString mirrorHeadBranch(const QString &mirrorPath)
{
    if (!QDir(mirrorPath).exists())
        return QString();
    QString head = headBranchFromFile(mirrorPath);
    if (head.isEmpty()) {
        QProcess p;
        p.start("git", {"-C", mirrorPath, "symbolic-ref", "--short", "HEAD"});
        if (p.waitForFinished(5000) && p.exitCode() == 0)
            head = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    }
    // The branch ref is asked first: it resolves from the ref files, while
    // "HEAD" always costs a `git rev-parse`. Either one answering is enough.
    if (!head.isEmpty() &&
        (!mirrorCommitForRef(mirrorPath,
                             QStringLiteral("refs/heads/") + head).isEmpty() ||
         !mirrorCommitForRef(mirrorPath, QStringLiteral("HEAD")).isEmpty()))
        return head;
    QProcess refs;
    refs.start("git", {"-C", mirrorPath, "for-each-ref",
                       "--format=%(refname)", "refs/heads/", "refs/remotes/"});
    if (!refs.waitForFinished(5000) || refs.exitCode() != 0)
        return QString();
    QStringList branches;
    for (const QString &ref : QString::fromUtf8(refs.readAllStandardOutput())
                                  .split('\n', Qt::SkipEmptyParts)) {
        const QString display = displayMirrorBranchNameForRef(ref.trimmed());
        if (!display.isEmpty() && !branches.contains(display))
            branches.append(display);
    }
    for (const QString &preferred : {QStringLiteral("main"), QStringLiteral("master")})
        if (branches.contains(preferred))
            return preferred;
    if (!branches.isEmpty())
        return branches.first().trimmed();
    return mirrorCommitForRef(mirrorPath, QStringLiteral("HEAD")).isEmpty()
               ? QString()
               : QStringLiteral("HEAD");
}

// code-health: allow(blocking-wait) sync primitive with callers on workers (SCM outgoing probe, commit-marker probe, sync-fetch prep) and a few UI-thread reads; an existing local branch returns from refTipFromFiles before any process, and the for-each-ref/rev-parse fallbacks are 5 s-capped
QString mirrorBranchCommit(const QString &mirrorPath, const QString &branch)
{
    if (!QDir(mirrorPath).exists())
        return QString();
    const QString raw = branch.trimmed();
    QStringList candidates;
    auto add = [&candidates](const QString &ref) {
        if (!ref.isEmpty() && !candidates.contains(ref))
            candidates.append(ref);
    };
    if (raw.isEmpty() || raw == QLatin1String("HEAD")) {
        add(QStringLiteral("HEAD"));
    } else if (raw.startsWith(QLatin1String("refs/"))) {
        add(raw);
    } else {
        // The common case — a local branch that exists — resolves from the
        // ref files without listing the remotes first.
        if (const QString local = refTipFromFiles(
                mirrorPath, QStringLiteral("refs/heads/") + raw);
            !local.isEmpty())
            return local;
        add(QStringLiteral("refs/heads/") + raw);
        add(QStringLiteral("refs/remotes/") + raw);
        QProcess refs;
        refs.start("git", {"-C", mirrorPath, "for-each-ref",
                           "--format=%(refname)", "refs/remotes/"});
        if (refs.waitForFinished(5000) && refs.exitCode() == 0) {
            for (const QString &ref : QString::fromUtf8(refs.readAllStandardOutput())
                                          .split('\n', Qt::SkipEmptyParts)) {
                const QString trimmed = ref.trimmed();
                if (displayMirrorBranchNameForRef(trimmed) == raw)
                    add(trimmed);
            }
        }
        add(raw);
    }
    for (const QString &candidate : std::as_const(candidates)) {
        const QString commit = mirrorCommitForRef(mirrorPath, candidate);
        if (!commit.isEmpty())
            return commit;
    }
    return QString();
}

QString cachedRemoteFetchUrl(const QString &repoPath, const QString &remote)
{
    QByteArray listing;
    if (remote.isEmpty() || !cachedRepoRemotesRaw(repoPath, &listing))
        return QString();
    for (const QByteArray &raw : listing.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.endsWith(QLatin1String("(fetch)")))
            continue;
        const qsizetype tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0 || line.left(tab) != remote)
            continue;
        return line.mid(tab + 1)
            .chopped(int(QStringLiteral("(fetch)").size()))
            .trimmed();
    }
    return QString();
}

bool upstreamPublishesThroughMirror(const QString &localPath,
                                    const QString &mirrorPath,
                                    const QString &relayHost)
{
    const QString branch = headBranchFromFile(localPath);
    if (branch.isEmpty())
        return false;
    const QString section = QStringLiteral("branch.%1.").arg(branch);
    QByteArray remoteOut;
    QByteArray mergeOut;
    if (!cachedGitConfigLocal(localPath, section + QStringLiteral("remote"),
                              &remoteOut) ||
        !cachedGitConfigLocal(localPath, section + QStringLiteral("merge"),
                              &mergeOut) ||
        QString::fromUtf8(mergeOut).trimmed().isEmpty())
        return false;
    const QString remote = QString::fromUtf8(remoteOut).trimmed();
    if (remote.isEmpty() || remote == QLatin1String("."))
        return false;
    const QString remoteUrl = cachedRemoteFetchUrl(localPath, remote);
    if (remoteUrl.isEmpty())
        return false;
    const QString remoteHost = QUrl(remoteUrl).host();
    if (!relayHost.isEmpty() && remoteHost == relayHost)
        return true;
    return remoteHost.isEmpty() && !mirrorPath.isEmpty() &&
           QDir(remoteUrl).absolutePath() == QDir(mirrorPath).absolutePath();
}

namespace {
QMutex g_configuredRelayHostMutex;
QString g_configuredRelayHost;
}

void forgetConfiguredRelayHost()
{
    QMutexLocker lock(&g_configuredRelayHostMutex);
    g_configuredRelayHost.clear();
}

QString configuredRelayHost()
{
    {
        QMutexLocker lock(&g_configuredRelayHostMutex);
        if (!g_configuredRelayHost.isEmpty())
            return g_configuredRelayHost;
    }
    QString host =
        serverHost(QSettings().value(kServerUrlSetting).toString().trimmed());
    if (host.isEmpty())
        host = serverHost(kDefaultServerUrl);
    QMutexLocker lock(&g_configuredRelayHostMutex);
    g_configuredRelayHost = host;
    return host;
}

MirrorBranchTip mirrorPrimaryBranchTip(const QString &mirrorPath,
                                       const QString &workTree)
{
    auto commitForBranch = [&](const QString &branch) -> QString {
        if (branch.trimmed().isEmpty())
            return QString();
        QString commit = worktreeBranchCommit(workTree, branch);
        if (commit.isEmpty())
            commit = mirrorBranchCommit(mirrorPath, branch);
        return commit;
    };

    for (const QString &preferred : {QStringLiteral("main"),
                                     QStringLiteral("master")}) {
        const QString commit = commitForBranch(preferred);
        if (!commit.isEmpty())
            return {preferred, commit};
    }

    const QString servedBranch = mirrorHeadBranch(mirrorPath);
    QString servedCommit = mirrorBranchCommit(mirrorPath, servedBranch);
    if (!servedCommit.isEmpty()) {
        const QString sourceCommit = worktreeBranchCommit(workTree, servedBranch);
        return {servedBranch, sourceCommit.isEmpty() ? servedCommit : sourceCommit};
    }

    const QString headBranch = worktreeHeadBranch(workTree);
    QString headCommit = worktreeBranchCommit(workTree, headBranch);
    if (headCommit.isEmpty())
        headCommit = worktreeHeadCommit(workTree);
    if (!headCommit.isEmpty())
        return {headBranch, headCommit};

    return {};
}

QString actionStatusText(const QString &status)
{
    if (status == ActionStatus::AwaitingApproval) return QStringLiteral("Awaiting approval");
    if (status == ActionStatus::Queued) return QStringLiteral("Queued");
    if (status == ActionStatus::Running) return QStringLiteral("Running");
    if (status == ActionStatus::Success) return QStringLiteral("Success");
    if (status == ActionStatus::Failed) return QStringLiteral("Failed");
    if (status == ActionStatus::Rejected) return QStringLiteral("Rejected");
    if (status == ActionStatus::Cancelled) return QStringLiteral("Cancelled");
    if (status == ActionStatus::Skipped) return QStringLiteral("Skipped");
    return status;
}

QColor actionStatusColor(const QString &status)
{
    // Parsed once: QColor(const char *) runs the CSS colour parser, and the
    // actions table asks for two of these per row on every rebuild — a stall was
    // sampled inside QColor::fromString() here.
    static const QColor green(QStringLiteral("#3fb950"));
    static const QColor red(QStringLiteral("#f85149"));
    static const QColor blue(QStringLiteral("#58a6ff"));
    static const QColor amber(QStringLiteral("#d29922"));
    static const QColor grey(QStringLiteral("#8b949e"));
    if (status == ActionStatus::Success) return green;
    if (status == ActionStatus::Failed) return red;
    if (status == ActionStatus::Running) return blue;
    if (status == ActionStatus::AwaitingApproval) return amber;
    return grey;
}

void logStartup(const QString &phase)
{
    forkmesh::logStartupTrace(phase);
    if (phase.startsWith(QLatin1String("  ")))
        startupDetail(phase.trimmed());
    else if (auto *splash = activeStartupSplash())
        splash->completeCurrentStep();
}

QElapsedTimer &restartClock()
{
    static QElapsedTimer t;
    return t;
}
void beginRestartLog()
{
    restartClock().start();
}
void logRestart(const QString &phase)
{
    if (!restartClock().isValid())
        restartClock().start();
    qInfo().noquote() << QStringLiteral("[restart +%1ms] %2")
                             .arg(restartClock().elapsed(), 5)
                             .arg(phase);
}



namespace {

// Suffix-first, because a queued attachment is often named before anything has
// read it, and sniffing a file the user has since moved answers "unknown"
// rather than "video".
const QStringList &videoAttachmentSuffixes()
{
    static const QStringList suffixes{
        QStringLiteral("mp4"),  QStringLiteral("m4v"),  QStringLiteral("mov"),
        QStringLiteral("webm"), QStringLiteral("mkv"),  QStringLiteral("avi"),
        QStringLiteral("ogv"),  QStringLiteral("mpg"),  QStringLiteral("mpeg"),
        QStringLiteral("wmv"),  QStringLiteral("3gp"),  QStringLiteral("flv")};
    return suffixes;
}

} // namespace

bool isVideoAttachmentPath(const QString &path)
{
    if (path.isEmpty())
        return false;
    if (videoAttachmentSuffixes().contains(QFileInfo(path).suffix().toLower()))
        return true;
    return QMimeDatabase()
        .mimeTypeForFile(path)
        .name()
        .startsWith(QLatin1String("video/"));
}

QString promptAttachmentFileFilter()
{
    QStringList video;
    for (const QString &suffix : videoAttachmentSuffixes())
        video << QStringLiteral("*.") + suffix;
    const QString images =
        QStringLiteral("*.png *.jpg *.jpeg *.gif *.webp *.bmp");
    const QString videos = video.join(QLatin1Char(' '));
    return QStringLiteral("Images and video (%1 %2);;Images (%1);;Video (%2);;"
                          "All files (*)")
        .arg(images, videos);
}

QString promptAttachmentLine(const QString &path)
{
    // Two prefixes rather than one neutral word: "Attached image:" is what the
    // CLIs, the transcript and the session scanner already key on, and a video
    // must not be handed to any of them as an image.
    return (isVideoAttachmentPath(path) ? QStringLiteral("Attached video: ")
                                        : QStringLiteral("Attached image: ")) +
           path;
}

namespace {

const QStringList &promptAttachmentPrefixes()
{
    static const QStringList prefixes{QStringLiteral("Attached image:"),
                                      QStringLiteral("Attached video:")};
    return prefixes;
}

} // namespace

QStringList promptAttachmentPaths(const QString &prompt)
{
    QStringList paths;
    for (const QString &line : prompt.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        for (const QString &prefix : promptAttachmentPrefixes()) {
            if (!trimmed.startsWith(prefix, Qt::CaseInsensitive))
                continue;
            const QString path = trimmed.mid(prefix.size()).trimmed();
            if (!path.isEmpty() && !paths.contains(path))
                paths << path;
            break;
        }
    }
    return paths;
}

QString promptTextWithoutAttachments(const QString &prompt)
{
    QStringList lines;
    for (const QString &line : prompt.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        bool attachment = false;
        for (const QString &prefix : promptAttachmentPrefixes())
            attachment = attachment || trimmed.startsWith(prefix, Qt::CaseInsensitive);
        if (attachment)
            continue;
        lines << line;
    }
    while (!lines.isEmpty() && lines.constFirst().trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && lines.constLast().trimmed().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n')).trimmed();
}

namespace {

constexpr qsizetype kDiffFirstPaintChars = 12'000;
// Follow-up batches can run right up to the apply cap: fewer event-loop
// round trips per page without exceeding one bounded GUI turn.
constexpr qsizetype kDiffStreamBatchChars = 16'000;
constexpr qsizetype kDiffFragmentChars = 12'000;
// QTextDocument has no row virtualization. Keep every individual setHtml /
// insertHtml payload bounded, and keep pathological documents in bounded
// pages so Qt never later decides to lay out several megabytes on a timer.
constexpr qsizetype kDiffMaxUiApplyChars = 16 * 1024;
constexpr qsizetype kDiffOversizeCellChunkChars = 4 * 1024;
constexpr qsizetype kDiffMaxRowHtmlChars = 6 * 1024;
constexpr qsizetype kDiffIntraLineMaxHtmlChars = 4 * 1024;
// What an inline before/after image preview may cost, base64 included. Two
// display-sized frames fit inside this; past it the preview is dropped rather
// than handed to QTextDocument (see splitDiffFileFragments).
constexpr qsizetype kDiffMaxImageBlockChars = 640 * 1024;
constexpr qsizetype kDiffContinuousDocumentChars = 192 * 1024;
constexpr qsizetype kDiffPageChars = 96 * 1024;
// Splitting a production-sized diff into file blocks, rows, fragments and
// pages is linear work, but a generated/minified file can still make that pass
// take several seconds.  The stall watchdog caught splitOversizeDiffRow() on
// the GUI thread repeatedly.  Plan large documents on a worker; only the
// already-bounded fragments ever come back to QTextDocument on the GUI thread.
constexpr qsizetype kDiffAsyncPlanChars = 512 * 1024;
constexpr int kDiffWindowPages = 2;

QString boundedDiffNotice(qsizetype chars, const QString &detail)
{
    return QStringLiteral(
               "<div class='diffresponsive'>%1 (%2 characters). Open the file "
               "from the Files tab to inspect the original.</div>")
        .arg(detail.toHtmlEscaped(), QString::number(chars));
}

// The most one inlined image may cost a *page*. Pagination bounds the layout
// and scroll work one resident document carries, and a data URI is neither: it
// is one decode of one image, already scaled into its box. Charging its base64
// length would give every changed screenshot a page of its own; capping the
// charge here still paginates an image-heavy diff before the decoded frames add
// up. An image smaller than the cap is charged what it actually occupies, so a
// pane of thumbnails — a merge celebration's portraits — is not paginated into
// pieces by a flat per-image weight it never costs.
constexpr qsizetype kDiffImagePageWeight = 8 * 1024;

struct DiffImageBlockStats {
    qsizetype chars = 0;     // characters the inline image markup occupies
    qsizetype pageChars = 0; // what those images cost one resident page
};

// What the inline images occupy in a slice of diff HTML: the before/after
// preview tables a changed asset renders, and the base64 portraits a summary
// pane carries (merge celebrations sit one avatar per row). The char budgets
// below bound how much *layout* one GUI turn may take on, and a base64 data URI
// is not that: it is one decode of one <img>. Callers discount this so an image
// is measured against its own allowance instead of the row budget, which every
// real screenshot — and a day's worth of merge portraits — would blow past on
// its own.
DiffImageBlockStats diffImageBlockStats(const QString &html)
{
    static const QString tableMarker = QStringLiteral("<table class='imagetable'");
    static const QString imgMarker = QStringLiteral("<img");
    DiffImageBlockStats stats;
    const auto charge = [&stats](qsizetype span) {
        stats.chars += span;
        stats.pageChars += qMin(span, kDiffImagePageWeight);
    };
    int at = 0;
    while (at < html.size()) {
        const int table = html.indexOf(tableMarker, at);
        const int img = html.indexOf(imgMarker, at);
        if (table < 0 && img < 0)
            break;
        // A preview table is charged whole — its own <img> tags are inside it,
        // so consuming it here is also what keeps them from being counted twice.
        if (table >= 0 && (img < 0 || table <= img)) {
            const int close = html.indexOf(QStringLiteral("</table>"), table);
            if (close < 0)
                break;
            charge(close + 8 - table);
            at = close + 8;
            continue;
        }
        const int close = html.indexOf(QLatin1Char('>'), img);
        if (close < 0)
            break;
        // Only an inlined image is base64 weight the document never lays out;
        // a linked <img> is already just its URL.
        if (QStringView(html).mid(img, close + 1 - img)
                .contains(QLatin1String("data:")))
            charge(close + 1 - img);
        at = close + 1;
    }
    return stats;
}

// The most one bounded GUI apply of this html may span. Every gate that hands
// diff markup to QTextDocument uses it, so an inlined image is credited its own
// allowance at each of them: a gate that measured the raw size instead would
// replace a real screenshot's file block — anchor, header and all — with a
// "too large to render safely" notice, which is the whole preview vanishing.
// The same gate ate the merged-branch celebration (adhoc #1726), whose bulk is
// one portrait per merge of the day rather than any layout at all.
qsizetype diffApplyCapFor(const QString &html)
{
    return kDiffMaxUiApplyChars +
           qMin(diffImageBlockStats(html).chars, kDiffMaxImageBlockChars);
}

qsizetype diffLayoutChars(const QString &html)
{
    const DiffImageBlockStats stats = diffImageBlockStats(html);
    return html.size() - qMin(stats.chars, kDiffMaxImageBlockChars) +
           stats.pageChars;
}

QStringList splitEscapedDiffText(const QString &text)
{
    if (text.size() <= kDiffOversizeCellChunkChars)
        return {text};
    QStringList chunks;
    qsizetype pos = 0;
    while (pos < text.size()) {
        qsizetype end = qMin(pos + kDiffOversizeCellChunkChars, text.size());
        if (end < text.size()) {
            // Never cut an HTML entity (the diff renderer escapes source text
            // before it reaches this point) or a UTF-16 surrogate pair.
            const qsizetype amp = text.lastIndexOf(QLatin1Char('&'), end - 1);
            const qsizetype semi = text.lastIndexOf(QLatin1Char(';'), end - 1);
            if (amp >= pos && amp > semi)
                end = amp;
            if (end > pos && end < text.size() &&
                text.at(end - 1).isHighSurrogate() &&
                text.at(end).isLowSurrogate())
                --end;
        }
        if (end <= pos) {
            const qsizetype semi = text.indexOf(QLatin1Char(';'), pos);
            end = semi >= pos ? semi + 1
                              : qMin(pos + kDiffOversizeCellChunkChars, text.size());
        }
        chunks.append(text.mid(pos, end - pos));
        pos = end;
    }
    return chunks;
}

QStringList splitOversizeDiffRow(const QString &row)
{
    if (row.size() <= kDiffMaxRowHtmlChars)
        return {row};

    const qsizetype rowOpenEnd = row.indexOf(QLatin1Char('>'));
    const qsizetype rowClose = row.lastIndexOf(QStringLiteral("</tr>"));
    if (rowOpenEnd < 0 || rowClose <= rowOpenEnd)
        return {QStringLiteral("<tr><td class='code ctx' colspan='4'>%1</td></tr>")
                    .arg(boundedDiffNotice(
                        row.size(), QStringLiteral("One malformed diff row was too large to render")))};

    struct Cell {
        QString open;
        QString inner;
        bool code = false;
        QStringList chunks;
    };
    QList<Cell> cells;
    qsizetype cursor = rowOpenEnd + 1;
    while (cursor < rowClose) {
        const qsizetype cellStart = row.indexOf(QStringLiteral("<td"), cursor);
        if (cellStart < 0 || cellStart >= rowClose ||
            !row.mid(cursor, cellStart - cursor).trimmed().isEmpty()) {
            cells.clear();
            break;
        }
        const qsizetype openEnd = row.indexOf(QLatin1Char('>'), cellStart);
        const qsizetype close =
            openEnd < 0 ? -1 : row.indexOf(QStringLiteral("</td>"), openEnd + 1);
        if (openEnd < 0 || close < 0 || close > rowClose) {
            cells.clear();
            break;
        }
        Cell cell;
        cell.open = row.mid(cellStart, openEnd - cellStart + 1);
        cell.inner = row.mid(openEnd + 1, close - openEnd - 1);
        cell.code = cell.open.contains(QStringLiteral("class='code")) ||
                    cell.open.contains(QStringLiteral("class=\"code"));
        if (cell.code && !cell.inner.contains(QLatin1Char('<')))
            cell.chunks = splitEscapedDiffText(cell.inner);
        cells.append(std::move(cell));
        cursor = close + 5;
    }
    if (cells.isEmpty() || !row.mid(cursor, rowClose - cursor).trimmed().isEmpty())
        return {QStringLiteral("<tr><td class='code ctx' colspan='4'>%1</td></tr>")
                    .arg(boundedDiffNotice(
                        row.size(), QStringLiteral("One complex diff row was too large to render")))};

    qsizetype pieces = 0;
    for (const Cell &cell : std::as_const(cells)) {
        if (cell.code)
            pieces = qMax(pieces, cell.chunks.size());
    }
    if (pieces <= 1)
        return {QStringLiteral("<tr><td class='code ctx' colspan='4'>%1</td></tr>")
                    .arg(boundedDiffNotice(
                        row.size(), QStringLiteral("One complex diff row was too large to render")))};

    const QString rowOpen = row.left(rowOpenEnd + 1);
    QStringList rows;
    rows.reserve(pieces);
    for (qsizetype piece = 0; piece < pieces; ++piece) {
        QString split = rowOpen;
        for (const Cell &cell : std::as_const(cells)) {
            split += cell.open;
            if (cell.code) {
                split += piece < cell.chunks.size() ? cell.chunks.at(piece)
                                                    : QStringLiteral("&nbsp;");
            } else {
                // A continuation is the same logical source line, so only the
                // first visual row keeps its line-number/link gutter.
                split += piece == 0 ? cell.inner : QStringLiteral("&nbsp;");
            }
            split += QStringLiteral("</td>");
        }
        split += QStringLiteral("</tr>");
        if (split.size() > kDiffMaxRowHtmlChars) {
            return {QStringLiteral(
                        "<tr><td class='code ctx' colspan='4'>%1</td></tr>")
                        .arg(boundedDiffNotice(
                            row.size(), QStringLiteral("One diff row was too large to render safely")))};
        }
        rows.append(std::move(split));
    }
    return rows;
}

// Only the *bare* anchor that opens a file block starts a new block. The file
// header carries a second anchor of the same name on a real character, and
// splitting there would cut the header table in half: the two halves would then
// be parsed by separate insertHtml calls whenever a batch or page edge fell
// between them, leaving a dangling cell and a header that never renders.
int nextDiffFileBlockStart(const QString &html, int from)
{
    static const QString marker = QStringLiteral("<a name=\"file-");
    static const QString bare = QStringLiteral("\"></a>");
    for (int at = html.indexOf(marker, from); at >= 0;
         at = html.indexOf(marker, at + marker.size())) {
        const int quote = html.indexOf(QLatin1Char('"'), at + marker.size());
        if (quote >= 0 && QStringView(html).mid(quote, bare.size()) == bare)
            return at;
    }
    return -1;
}

} // namespace

QStringList splitDiffFileBlocks(const QString &html)
{
    int pos = nextDiffFileBlockStart(html, 0);
    if (pos < 0)
        return {html}; // no per-file anchors (e.g. an empty/notice body)
    QStringList blocks;
    if (pos > 0)
        blocks.append(html.left(pos)); // preamble before the first file (if any)
    while (pos >= 0) {
        const int next = nextDiffFileBlockStart(html, pos + 1);
        blocks.append(html.mid(pos, next < 0 ? -1 : next - pos));
        pos = next;
    }
    return blocks;
}

namespace {

// A bounded slice of one file's diff. Continuation slices rejoin the file's
// table through a margin-less div, so a split inside a page is invisible. A
// slice opening a page repeats only the file anchor: sticky navigation keeps
// its context without inserting a second file header into the code rows.
struct DiffHtmlFragment {
    QString html;
    QString pageStartAnchor;
};

QList<DiffHtmlFragment> splitDiffFileFragments(const QString &block)
{
    if (block.size() <= kDiffFragmentChars)
        return {{block, QString()}};

    const QString tableMarker = QStringLiteral("<table class='difftable'");
    const int table = block.indexOf(tableMarker);
    const int tableOpenEnd = table < 0 ? -1 : block.indexOf(QLatin1Char('>'), table);
    const int tableClose = block.lastIndexOf(QStringLiteral("</table>"));
    if (table < 0 || tableOpenEnd < 0 || tableClose <= tableOpenEnd)
        return {{block, QString()}}; // binary/collapsed/image-only block: already atomic

    const int anchorEnd = block.indexOf(QStringLiteral("</a>"));
    const QString fileBlockMarker = QStringLiteral("<div class='fileblock");
    const int fileBlock =
        anchorEnd < 0 ? -1 : block.indexOf(fileBlockMarker, anchorEnd + 4);
    const QString headerMarker = QStringLiteral("<table class='fileheader'");
    const int header =
        fileBlock < 0 ? -1 : block.indexOf(headerMarker, fileBlock);
    const int headerClose =
        header < 0 ? -1 : block.indexOf(QStringLiteral("</table>"), header);
    if (anchorEnd < 0 || fileBlock < 0 || fileBlock >= table || header < 0 ||
        headerClose < 0 || headerClose >= table) {
        return {{boundedDiffNotice(
                     block.size(),
                     QStringLiteral("This rich diff block was too large to render safely")),
                 QString()}};
    }

    const QString anchor = block.left(anchorEnd + 4);
    const QString tableOpen = block.mid(table, tableOpenEnd - table + 1);
    QString firstPrefix = block.left(tableOpenEnd + 1);
    const QString continuationPrefix =
        QStringLiteral("<div class='fileblockcont'>") + tableOpen;
    const QString pageStartAnchor = anchor;
    const QString closing = QStringLiteral("</table></div>");
    const QString finalSuffix = block.mid(tableClose);

    // An inline before/after preview is a single <img> data URI: it costs the
    // document one base64 decode, not the per-row layout work this char budget
    // exists to bound. So measure the markup around it and let the preview carry
    // its own, much larger allowance — charging it against the row budget would
    // strip every preview bigger than a favicon. Anything past that allowance
    // still gives way to an explicit Files-tab route.
    const qsizetype maxPrefix =
        kDiffMaxUiApplyChars - kDiffMaxRowHtmlChars -
        qMax(closing.size(), finalSuffix.size());
    const int image =
        firstPrefix.indexOf(QStringLiteral("<table class='imagetable'"));
    const int imageClose =
        image < 0 ? -1 : firstPrefix.indexOf(QStringLiteral("</table>"), image);
    qsizetype imageChars =
        (image >= 0 && imageClose >= image) ? imageClose + 8 - image : 0;
    if (imageChars > kDiffMaxImageBlockChars ||
        (imageChars > 0 && firstPrefix.size() - imageChars > maxPrefix)) {
        firstPrefix.replace(
            image, imageChars,
            boundedDiffNotice(imageChars,
                              QStringLiteral("Image preview skipped to keep this diff responsive")));
        imageChars = 0;
    }
    if (firstPrefix.size() - imageChars > maxPrefix ||
        pageStartAnchor.size() + continuationPrefix.size() > maxPrefix) {
        firstPrefix =
            anchor + QStringLiteral("<div class='fileblock'>") +
            boundedDiffNotice(
                block.left(tableOpenEnd + 1).size(),
                QStringLiteral("File header simplified to keep this diff responsive")) +
            tableOpen;
    }

    QStringList rows;
    int cursor = tableOpenEnd + 1;
    while (cursor < tableClose) {
        const int rowEnd = block.indexOf(QStringLiteral("</tr>"), cursor);
        if (rowEnd < 0 || rowEnd >= tableClose)
            break;
        const int after = rowEnd + 5;
        rows.append(splitOversizeDiffRow(block.mid(cursor, after - cursor)));
        cursor = after;
    }
    // An image-only file block has no rows to slice: its diff table is empty
    // because the before/after preview stood in for the "binary file changed"
    // row. Hand back the prefix — which carries the file's anchor, its header
    // and the preview allowance applied above — rather than the raw block,
    // which a later gate would have to replace with a notice wholesale.
    if (rows.isEmpty() && cursor == tableClose)
        return {{firstPrefix + finalSuffix, QString()}};
    // Unexpected table markup is safer left atomic than accidentally losing
    // bytes. Normal unified/split renderers always produce complete <tr> rows.
    if (rows.isEmpty() || cursor != tableClose)
        return {{block, QString()}};

    QList<DiffHtmlFragment> fragments;
    QString current = firstPrefix;
    bool continuation = false;
    bool hasRows = false;
    // Continuation slices reserve room for the anchor a page start prepends,
    // so the assembled page-start slice still fits one bounded GUI apply.
    const qsizetype rowBudget = kDiffFragmentChars - pageStartAnchor.size();
    for (const QString &row : rows) {
        if (hasRows &&
            current.size() + row.size() + closing.size() > rowBudget) {
            current += closing;
            fragments.append({std::move(current),
                              continuation ? pageStartAnchor : QString()});
            current = continuationPrefix;
            continuation = true;
            hasRows = false;
        }
        current += row;
        hasRows = true;
    }
    current += finalSuffix;
    if (continuation) {
        // Only the file's closing slice keeps the fileblock class, so exactly
        // one bottom margin separates this file from the next. The opening
        // slice has to give its margin up too, or a split file shows the
        // between-files gap in its own middle.
        const QString openMargin = QStringLiteral("<div class='fileblock'>");
        const int openAt = fragments.first().html.indexOf(openMargin);
        if (openAt >= 0)
            fragments.first().html.replace(
                openAt, openMargin.size(),
                QStringLiteral("<div class='fileblockcont'>"));
        current.replace(0, QStringLiteral("<div class='fileblockcont'>").size(),
                        QStringLiteral("<div class='fileblock'>"));
    }
    fragments.append({std::move(current),
                      continuation ? pageStartAnchor : QString()});
    for (DiffHtmlFragment &fragment : fragments) {
        if (fragment.html.size() + fragment.pageStartAnchor.size() >
            diffApplyCapFor(fragment.html)) {
            fragment.html = boundedDiffNotice(
                fragment.html.size(),
                QStringLiteral("One rich diff fragment was too large to render safely"));
            fragment.pageStartAnchor.clear();
        }
    }
    return fragments;
}

QList<DiffHtmlFragment> fragmentDiffHtml(const QString &html)
{
    QList<DiffHtmlFragment> fragments;
    const QString sectionBreak = diffSectionBreakHtml();
    const QStringList sections =
        html.contains(sectionBreak) ? html.split(sectionBreak, Qt::SkipEmptyParts)
                                    : QStringList{html};
    for (const QString &section : sections) {
        for (const QString &block : splitDiffFileBlocks(section)) {
            // A previewed image block is atomic — its table holds no diff rows to
            // slice — so it has to clear the cap here on its image allowance alone.
            if (block.size() <= diffApplyCapFor(block)) {
                fragments.append({block, QString()});
            } else {
                fragments.append(splitDiffFileFragments(block));
            }
        }
    }
    if (fragments.isEmpty())
        fragments.append({QString(), QString()});
    for (DiffHtmlFragment &fragment : fragments) {
        if (fragment.html.size() + fragment.pageStartAnchor.size() >
            diffApplyCapFor(fragment.html)) {
            fragment.html = boundedDiffNotice(
                fragment.html.size(),
                QStringLiteral("This unstructured diff content was too large to render safely"));
            fragment.pageStartAnchor.clear();
        }
    }
    return fragments;
}

QList<QStringList> paginateDiffHtml(const QString &html, bool forcePaged = false)
{
    QList<DiffHtmlFragment> fragments = fragmentDiffHtml(html);
    QList<qsizetype> costs;
    costs.reserve(fragments.size());
    qsizetype residentChars = 0;
    for (const DiffHtmlFragment &fragment : std::as_const(fragments)) {
        costs.append(diffLayoutChars(fragment.html));
        residentChars += costs.last();
    }

    QList<QStringList> pages;
    QStringList page;
    qsizetype pageChars = 0;
    const bool paged = forcePaged || residentChars > kDiffContinuousDocumentChars;
    for (qsizetype i = 0; i < fragments.size(); ++i) {
        DiffHtmlFragment &fragment = fragments[i];
        const qsizetype cost = costs.at(i);
        if (paged && !page.isEmpty() && pageChars + cost > kDiffPageChars) {
            pages.append(std::move(page));
            page.clear();
            pageChars = 0;
        }
        // A page opening in the middle of a file restates its invisible anchor;
        // inside a page, continuation slices join up the same way.
        const bool restatesAnchor =
            page.isEmpty() && !fragment.pageStartAnchor.isEmpty();
        QString html = restatesAnchor
                           ? fragment.pageStartAnchor + fragment.html
                           : std::move(fragment.html);
        pageChars += cost + (restatesAnchor ? fragment.pageStartAnchor.size() : 0);
        page.append(std::move(html));
    }
    if (!page.isEmpty())
        pages.append(std::move(page));
    if (pages.isEmpty())
        pages.append(QStringList{QString()});
    return pages;
}

struct DiffPendingPage {
    int page = -1;
    QStringList fragments;
    bool started = false;
};

struct DiffStreamState {
    QList<QStringList> pages;
    QList<DiffPendingPage> pendingPages;
    int gen = 0;
    bool planning = false;
    // The first logical page in the resident rolling window. Pathological
    // diffs keep this page and its successor together, which gives the reader
    // one real, continuous boundary to cross before the window has to move.
    int page = 0;
    int windowLastPage = 0;
    QHash<int, int> windowPagePositions;
    QString styleSheet;
    QString pendingAnchor;
    QString pendingText;
    int pendingTextDirection = 1;
    int pendingViewportPosition = 0; // -1 bottom, +1 top, 0 unchanged
    // A rolling-window move reparses the shared page. Pin its document position
    // to the same viewport Y and apply the wheel/key distance afterwards, so
    // the replacement is visually indistinguishable from ordinary scrolling.
    int pendingViewportPage = -1;
    int pendingViewportY = 0;
    int pendingScrollPixels = 0;
    bool viewportUpdatesSuspended = false;
    bool edgeMoveScheduled = false;
    int endCapPosition = -1;
    // An in-place fold near the bottom shortens the document. Hold just enough
    // runway below the END OF DIFF bar to keep the current scroll value valid
    // instead of letting QScrollBar clamp the viewport upward.
    int endCapExtraHeight = 0;
    bool endCapResizePending = false;
    bool endCapResizeFilterInstalled = false;
    bool pageWindowFilterInstalled = false;
    bool endCap = true;
    bool incremental = false;
    bool incrementalFinished = false;
    bool incrementalEndCap = true;
    QList<std::function<void()>> finishedHooks;
};

void renderDiffPage(QTextEdit *view, int page);
void showDiffPage(QTextEdit *view, int page, bool preservePending = false,
                  int viewportPosition = 0);
bool scheduleDiffWindowMove(QTextEdit *view, int direction,
                            int scrollPixels = 0);
void scheduleDiffWindowFill(QTextEdit *view);

QHash<QTextEdit *, DiffStreamState> &diffStreams()
{
    static QHash<QTextEdit *, DiffStreamState> streams;
    return streams;
}

DiffStreamState &diffStreamState(QTextEdit *view)
{
    auto &streams = diffStreams();
    auto it = streams.find(view);
    if (it != streams.end())
        return *it;
    QObject::connect(view, &QObject::destroyed, qApp,
                     [view] { diffStreams().remove(view); });
    return streams[view];
}

const QUrl &diffEndCapResource()
{
    static const QUrl resource(QStringLiteral("forkmesh-diff-end-cap"));
    return resource;
}

QImage diffEndCapImage(QTextEdit *view, int capHeight, int barHeight)
{
    QWidget *page = view->viewport();
    QColor background = page->palette().color(page->backgroundRole());
    if (!background.isValid())
        background = qApp->palette().color(QPalette::Base);
    QImage cap(1, qMax(1, capHeight), QImage::Format_ARGB32);
    cap.fill(background);
    QPainter painter(&cap);
    painter.fillRect(0, 0, 1, qBound(1, barHeight, cap.height()), Qt::black);
    painter.end();
    return cap;
}

int diffEndCapBarHeight(QTextEdit *view)
{
    return qMax(1, view->fontMetrics().lineSpacing());
}

void resizeDiffEndCap(QTextEdit *view, DiffStreamState &state)
{
    if (!view || state.endCapPosition < 0)
        return;
    QTextCursor cursor(view->document());
    cursor.setPosition(state.endCapPosition);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QTextImageFormat image = cursor.charFormat().toImageFormat();
    if (!image.isValid())
        return;
    const int barHeight = diffEndCapBarHeight(view);
    const int capHeight = qMax(
        barHeight, view->viewport()->height() + state.endCapExtraHeight);
    view->document()->addResource(QTextDocument::ImageResource,
                                  diffEndCapResource(),
                                  diffEndCapImage(view, capHeight, barHeight));
    image.setWidth(qMax(1, view->viewport()->width()));
    image.setHeight(capHeight);
    cursor.setCharFormat(image);
}

class DiffEndCapResizeFilter final : public QObject
{
public:
    explicit DiffEndCapResizeFilter(QTextEdit *view) : QObject(view), m_view(view) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_view || event->type() != QEvent::Resize)
            return false;
        auto it = diffStreams().find(m_view);
        if (it == diffStreams().end())
            return false;
        if (it->endCapPosition >= 0 && !it->endCapResizePending) {
            it->endCapResizePending = true;
            QPointer<QTextEdit> guard(m_view);
            QTimer::singleShot(0, m_view, [guard] {
                if (!guard)
                    return;
                auto it = diffStreams().find(guard.data());
                if (it == diffStreams().end())
                    return;
                it->endCapResizePending = false;
                resizeDiffEndCap(guard, *it);
            });
        }
        // A taller viewport can expose a rolling-page boundary that used to sit
        // below the fold. Pull the next page in after Qt has applied the resize
        // instead of leaving blank space that only a wheel event can replace.
        scheduleDiffWindowFill(m_view);
        return false;
    }

private:
    QTextEdit *m_view = nullptr;
};

void installDiffEndCapResizeFilter(QTextEdit *view)
{
    view->installEventFilter(new DiffEndCapResizeFilter(view));
}

bool diffStreamPending(const DiffStreamState &state)
{
    return state.planning || !state.pendingPages.isEmpty();
}

int diffWheelScrollPixels(QTextEdit *view, const QWheelEvent *wheel)
{
    if (!view || !wheel)
        return 0;
    if (wheel->pixelDelta().y() != 0)
        return -wheel->pixelDelta().y();
    const int angle = wheel->angleDelta().y();
    if (angle == 0)
        return 0;
    const int linePixels = qMax(1, view->fontMetrics().lineSpacing());
    const int lines = qMax(1, QApplication::wheelScrollLines());
    const int pixels = qRound(-angle / 120.0 * lines * linePixels);
    return pixels != 0 ? pixels : (angle < 0 ? 1 : -1);
}

int diffKeyScrollPixels(QTextEdit *view, const QKeyEvent *key)
{
    if (!view || !key)
        return 0;
    const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
    const int linePixels = qMax(1, view->fontMetrics().lineSpacing());
    const int pagePixels = qMax(linePixels, view->viewport()->height() - linePixels);
    if (key->key() == Qt::Key_Down)
        return linePixels;
    if (key->key() == Qt::Key_Up)
        return -linePixels;
    if (key->key() == Qt::Key_PageDown ||
        (key->key() == Qt::Key_Space && !shifted)) {
        return pagePixels;
    }
    if (key->key() == Qt::Key_PageUp ||
        (key->key() == Qt::Key_Space && shifted)) {
        return -pagePixels;
    }
    return 0;
}

class DiffPageWindowFilter final : public QObject
{
public:
    explicit DiffPageWindowFilter(QTextEdit *view) : QObject(view), m_view(view) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        int direction = 0;
        int scrollPixels = 0;
        if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
            const auto *wheel = static_cast<QWheelEvent *>(event);
            scrollPixels = diffWheelScrollPixels(m_view, wheel);
            direction = scrollPixels > 0 ? 1 : scrollPixels < 0 ? -1 : 0;
        } else if (watched == m_view && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            scrollPixels = diffKeyScrollPixels(m_view, key);
            direction = scrollPixels > 0 ? 1 : scrollPixels < 0 ? -1 : 0;
        } else if (watched == m_view->verticalScrollBar() &&
                   event->type() == QEvent::MouseButtonRelease) {
            QScrollBar *bar = m_view->verticalScrollBar();
            if (bar->value() >= bar->maximum())
                direction = 1;
            else if (bar->value() <= bar->minimum())
                direction = -1;
        }
        if (direction == 0)
            return false;
        auto state = diffStreams().find(m_view);
        if (state != diffStreams().end() && state->pendingViewportPage >= 0) {
            state->pendingScrollPixels += scrollPixels;
            event->accept();
            return true;
        }
        if (!scheduleDiffWindowMove(m_view, direction, scrollPixels))
            return false;
        event->accept();
        return watched != m_view->verticalScrollBar();
    }

private:
    QTextEdit *m_view = nullptr;
};

void installDiffPageWindowFilter(QTextEdit *view)
{
    auto *filter = new DiffPageWindowFilter(view);
    view->installEventFilter(filter);
    view->viewport()->installEventFilter(filter);
    QScrollBar *bar = view->verticalScrollBar();
    bar->installEventFilter(filter);
    QObject::connect(
        bar, &QAbstractSlider::actionTriggered, view,
        [view](int action) {
            int direction = 0;
            if (action == QAbstractSlider::SliderSingleStepAdd ||
                action == QAbstractSlider::SliderPageStepAdd ||
                action == QAbstractSlider::SliderToMaximum) {
                direction = 1;
            } else if (action == QAbstractSlider::SliderSingleStepSub ||
                       action == QAbstractSlider::SliderPageStepSub ||
                       action == QAbstractSlider::SliderToMinimum) {
                direction = -1;
            }
            if (direction != 0)
                scheduleDiffWindowMove(view, direction);
        });
}

void appendDiffEndCap(QTextEdit *view, DiffStreamState &state)
{
    if (!view || !state.endCap || state.endCapPosition >= 0 ||
        state.windowLastPage + 1 < state.pages.size())
        return;
    QTextCursor cursor(view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock();
    state.endCapExtraHeight = 0;
    const int barHeight = diffEndCapBarHeight(view);
    view->document()->addResource(
        QTextDocument::ImageResource, diffEndCapResource(),
        diffEndCapImage(view, qMax(barHeight, view->viewport()->height()),
                        barHeight));
    QTextImageFormat image;
    image.setName(diffEndCapResource().toString());
    state.endCapPosition = cursor.position();
    cursor.insertImage(image);
    resizeDiffEndCap(view, state);
}

// A viewed tick replaces a file body with its header. Above the fold that is
// naturally position-stable, but near the bottom the shorter document can make
// the scroll bar's maximum smaller than its current value. Keep only enough of
// the end-cap runway to retain that value; reopening or the next in-place swap
// remeasures from the normal cap, so no spare blank space sticks around.
void preserveDiffSwapScrollPosition(QTextEdit *view, DiffStreamState &state,
                                    int scrollValue)
{
    QScrollBar *bar = view ? view->verticalScrollBar() : nullptr;
    if (!bar || !state.endCap || state.endCapPosition < 0)
        return;

    state.endCapExtraHeight = 0;
    resizeDiffEndCap(view, state);
    // The image's formatted height updates the document and scroll bar
    // synchronously. A second pass covers a one-pixel layout rounding edge.
    for (int pass = 0; pass < 2 && bar->maximum() < scrollValue; ++pass) {
        state.endCapExtraHeight += scrollValue - bar->maximum();
        resizeDiffEndCap(view, state);
    }
    bar->setValue(qBound(bar->minimum(), scrollValue, bar->maximum()));
}

void resumeDiffViewportUpdates(QTextEdit *view, DiffStreamState &state)
{
    if (!view || !state.viewportUpdatesSuspended)
        return;
    state.viewportUpdatesSuspended = false;
    view->setUpdatesEnabled(true);
    view->update();
}

bool restoreDiffWindowViewport(QTextEdit *view, DiffStreamState &state)
{
    if (!view || state.pendingViewportPage < 0)
        return false;
    const int position = state.windowPagePositions.value(
        state.pendingViewportPage, -1);
    QScrollBar *bar = view->verticalScrollBar();
    if (position < 0 || !bar)
        return false;
    QTextCursor cursor(view->document());
    cursor.setPosition(qBound(0, position,
                              view->document()->characterCount() - 1));
    const int documentY = view->cursorRect(cursor).top() + bar->value();
    bar->setValue(qBound(bar->minimum(),
                         documentY - state.pendingViewportY +
                             state.pendingScrollPixels,
                         bar->maximum()));
    return true;
}

void finishDiffStream(QTextEdit *view)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end())
        return;
    if (!it->pendingText.isEmpty()) {
        const QString text = it->pendingText;
        it->pendingText.clear();
        const int direction = it->pendingTextDirection;
        QTextCursor from(view->document());
        QTextDocument::FindFlags flags;
        if (direction < 0) {
            from.movePosition(QTextCursor::End);
            flags |= QTextDocument::FindBackward;
        }
        const QTextCursor found = view->document()->find(text, from, flags);
        if (!found.isNull()) {
            view->setTextCursor(found);
            view->ensureCursorVisible();
        }
    }
    const bool restoredWindow = restoreDiffWindowViewport(view, *it);
    it->pendingViewportPage = -1;
    it->pendingViewportY = 0;
    it->pendingScrollPixels = 0;
    const int viewportPosition = it->pendingViewportPosition;
    it->pendingViewportPosition = 0;
    if (!restoredWindow) {
        if (QScrollBar *bar = view->verticalScrollBar()) {
            if (viewportPosition < 0)
                bar->setValue(bar->maximum());
            else if (viewportPosition > 0)
                bar->setValue(bar->minimum());
        }
    }
    resumeDiffViewportUpdates(view, *it);
    if (it->incremental && !it->incrementalFinished)
        return;
    const QList<std::function<void()>> hooks = it->finishedHooks;
    for (const std::function<void()> &hook : hooks)
        hook();
}

void appendDiffStreamBatch(QTextEdit *view, const QString &batch)
{
    if (batch.isEmpty())
        return;
    BlockingCallScope crumb(QStringLiteral("diff html append (%1 chars, %2)")
                                .arg(batch.size())
                                .arg(view->objectName().isEmpty()
                                         ? QStringLiteral("unnamed view")
                                         : view->objectName()));
    const forkmesh::BackgroundScope action(
        QStringLiteral("diff"),
        QStringLiteral("append %1 chars to %2")
            .arg(batch.size())
            .arg(view->objectName().isEmpty() ? QStringLiteral("unnamed view")
                                               : view->objectName()),
        forkmesh::ActionTelemetry::Execution::UiDeferred);
    view->setUpdatesEnabled(false);
    QTextCursor cur(view->document());
    cur.movePosition(QTextCursor::End);
    cur.insertHtml(batch);
    view->setUpdatesEnabled(true);
}

void scrollLoadedDiffAnchor(QTextEdit *view, DiffStreamState &state,
                            const QString &batch)
{
    if (state.pendingAnchor.isEmpty())
        return;
    const QString marker = QStringLiteral("name=\"") + state.pendingAnchor +
                           QStringLiteral("\"");
    if (!batch.contains(marker))
        return;
    if (auto *browser = qobject_cast<QTextBrowser *>(view))
        browser->scrollToAnchor(state.pendingAnchor);
    state.pendingAnchor.clear();
}

QString takeDiffStreamBatch(QStringList &pending, qsizetype targetChars)
{
    QString batch;
    while (!pending.isEmpty() &&
           (batch.isEmpty() ||
            batch.size() + pending.first().size() <= targetChars))
        batch += pending.takeFirst();
    if (batch.size() > diffApplyCapFor(batch)) {
        qWarning().noquote()
            << QStringLiteral("Refusing oversized diff UI batch (%1 chars)")
                   .arg(batch.size());
        return boundedDiffNotice(
            batch.size(),
            QStringLiteral("A diff batch was too large to render safely"));
    }
    return batch;
}

void scheduleDiffStreamBatch(QTextEdit *view, int gen)
{
    QPointer<QTextEdit> guard(view);
    QTimer::singleShot(0, view, [guard, gen] {
        if (!guard)
            return;
        auto it = diffStreams().find(guard.data());
        if (it == diffStreams().end() || it->gen != gen ||
            it->pendingPages.isEmpty()) {
            return;
        }
        DiffPendingPage &pendingPage = it->pendingPages.first();
        if (!pendingPage.started) {
            QTextCursor end(guard->document());
            end.movePosition(QTextCursor::End);
            it->windowPagePositions.insert(pendingPage.page, end.position());
            pendingPage.started = true;
        }
        const QString batch = takeDiffStreamBatch(
            pendingPage.fragments, kDiffStreamBatchChars);
        if (pendingPage.fragments.isEmpty())
            it->pendingPages.removeFirst();
        bool done = it->pendingPages.isEmpty();
        appendDiffStreamBatch(guard, batch);
        if (!guard)
            return;
        scrollLoadedDiffAnchor(guard, *it, batch);
        // Arriving from the page below reads bottom-up: ride the growing tail
        // instead of showing the page top and jumping down on completion.
        if (it->pendingViewportPosition < 0 &&
            it->pendingViewportPage < 0) {
            if (QScrollBar *bar = guard->verticalScrollBar())
                bar->setValue(bar->maximum());
        }
        // Pagination is measured in layout work, not pixels. A run of compact
        // Viewed headers can therefore exhaust the normal two-page window while
        // occupying only half a tall viewport. Add another bounded page until
        // the document becomes naturally scrollable, so the next file rows are
        // already visible in that otherwise blank space.
        if (done && it->windowLastPage + 1 < it->pages.size()) {
            QScrollBar *bar = guard->verticalScrollBar();
            if (bar && bar->maximum() <= bar->minimum() &&
                guard->viewport()->height() > 0) {
                const int nextPage = ++it->windowLastPage;
                it->pendingPages.append(
                    {nextPage, it->pages.at(nextPage), false});
                done = false;
            }
        }
        if (done) {
            appendDiffEndCap(guard, *it);
            finishDiffStream(guard);
        } else {
            scheduleDiffStreamBatch(guard, gen);
        }
    });
}

void scheduleDiffWindowFill(QTextEdit *view)
{
    if (!view)
        return;
    const auto current = diffStreams().constFind(view);
    if (current == diffStreams().cend() || diffStreamPending(*current) ||
        current->windowLastPage + 1 >= current->pages.size()) {
        return;
    }
    const int gen = current->gen;
    QPointer<QTextEdit> guard(view);
    QTimer::singleShot(0, view, [guard, gen] {
        if (!guard)
            return;
        auto state = diffStreams().find(guard.data());
        if (state == diffStreams().end() || state->gen != gen ||
            diffStreamPending(*state) ||
            state->windowLastPage + 1 >= state->pages.size()) {
            return;
        }
        QScrollBar *bar = guard->verticalScrollBar();
        if (!bar || bar->maximum() > bar->minimum() ||
            guard->viewport()->height() <= 0) {
            return;
        }
        const int nextPage = ++state->windowLastPage;
        state->pendingPages.append(
            {nextPage, state->pages.at(nextPage), false});
        scheduleDiffStreamBatch(guard, gen);
    });
}

} // namespace

QString diffSectionBreakHtml()
{
    return QStringLiteral("<!--fm-diff-section-->");
}

namespace {

DiffStreamState &resetDiffStreamForRender(QTextEdit *view,
                                          const QString &styleSheet, bool endCap)
{
    DiffStreamState &state = diffStreamState(view);
    resumeDiffViewportUpdates(view, state);
    state.endCap = endCap;
    if (!state.endCapResizeFilterInstalled) {
        installDiffEndCapResizeFilter(view);
        state.endCapResizeFilterInstalled = true;
    }
    if (!state.pageWindowFilterInstalled) {
        installDiffPageWindowFilter(view);
        state.pageWindowFilterInstalled = true;
    }
    ++state.gen; // supersede any batches still queued from a previous render
    state.planning = false;
    state.pendingPages.clear();
    state.pages.clear();
    state.page = 0;
    state.windowLastPage = 0;
    state.windowPagePositions.clear();
    state.styleSheet = styleSheet;
    state.pendingAnchor.clear();
    state.pendingText.clear();
    state.pendingTextDirection = 1;
    state.pendingViewportPosition = 1;
    state.pendingViewportPage = -1;
    state.pendingViewportY = 0;
    state.pendingScrollPixels = 0;
    state.edgeMoveScheduled = false;
    state.endCapPosition = -1;
    state.endCapExtraHeight = 0;
    state.endCapResizePending = false;
    state.incremental = false;
    state.incrementalFinished = false;
    state.incrementalEndCap = endCap;
    return state;
}

} // namespace

void beginDiffStreamIncremental(QTextEdit *view, const QString &styleSheet,
                                const QString &placeholder, bool endCap)
{
    if (!view)
        return;
    DiffStreamState &state = resetDiffStreamForRender(view, styleSheet, endCap);
    state.incremental = true;
    state.endCap = false;
    view->document()->setDefaultStyleSheet(styleSheet);
    if (!placeholder.isEmpty())
        view->setHtml(placeholder);
}

QList<QStringList> paginateDiffStreamChunk(const QString &chunk)
{
    if (chunk.isEmpty())
        return {};
    return paginateDiffHtml(chunk, /*forcePaged=*/true);
}

void appendDiffStreamChunk(QTextEdit *view, const QString &chunk)
{
    if (!view || chunk.isEmpty())
        return;
    appendDiffStreamPages(view, paginateDiffStreamChunk(chunk));
}

void appendDiffStreamPages(QTextEdit *view, const QList<QStringList> &pages)
{
    if (!view || pages.isEmpty())
        return;
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || !it->incremental || it->incrementalFinished)
        return;
    const bool first = it->pages.isEmpty();
    it->pages += pages;
    if (first) {
        renderDiffPage(view, 0);
        return;
    }
    scheduleDiffWindowFill(view);
}

void finishDiffStreamIncremental(QTextEdit *view)
{
    if (!view)
        return;
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || !it->incremental || it->incrementalFinished)
        return;
    it->incrementalFinished = true;
    it->endCap = it->incrementalEndCap;
    if (it->pages.isEmpty()) {
        it->pages.append(QStringList{QString()});
        renderDiffPage(view, 0);
        return;
    }
    if (!diffStreamPending(*it)) {
        appendDiffEndCap(view, *it);
        finishDiffStream(view);
    }
}

void renderDiffStreamed(QTextEdit *view, const QString &html,
                        const QString &styleSheet, bool endCap)
{
    if (!view)
        return;
    DiffStreamState &state = resetDiffStreamForRender(view, styleSheet, endCap);
    const int planningGen = state.gen;

    if (html.size() > kDiffAsyncPlanChars) {
        state.planning = true;
        view->document()->setDefaultStyleSheet(styleSheet);
        view->setHtml(QStringLiteral(
            "<div class='diffresponsive'>Preparing large diff…</div>"));

        QPointer<QTextEdit> guard(view);
        auto *watcher = new QFutureWatcher<QList<QStringList>>(view);
        QObject::connect(
            watcher, &QFutureWatcher<QList<QStringList>>::finished, view,
            [guard, watcher, planningGen]() mutable {
                QList<QStringList> pages = watcher->result();
                watcher->deleteLater();
                auto apply = [guard, planningGen,
                              pages = std::move(pages)]() mutable {
                    if (!guard)
                        return;
                    auto it = diffStreams().find(guard.data());
                    if (it == diffStreams().end() || it->gen != planningGen ||
                        !it->planning) {
                        return;
                    }
                    it->planning = false;
                    it->pages = std::move(pages);
                    renderDiffPage(guard, 0);
                };
                if (!forkmesh::ui::deferUntilAfterKeepAlivePump(apply))
                    apply();
            });
        watcher->setFuture(QtConcurrent::run([html] {
            const forkmesh::BackgroundScope action(
                QStringLiteral("diff"),
                QStringLiteral("plan %1 chars into bounded pages")
                    .arg(html.size()),
                forkmesh::ActionTelemetry::Execution::Worker);
            return paginateDiffHtml(html);
        }));
        return;
    }

    state.pages = paginateDiffHtml(html);
    renderDiffPage(view, 0);
}

namespace {

void renderDiffPage(QTextEdit *view, int page)
{
    if (!view)
        return;
    DiffStreamState &state = diffStreamState(view);
    if (state.pages.isEmpty())
        return;
    state.planning = false;
    state.page = qBound(0, page, int(state.pages.size()) - 1);
    state.windowLastPage = qMin(state.page + kDiffWindowPages - 1,
                                int(state.pages.size()) - 1);
    state.edgeMoveScheduled = false;
    ++state.gen;
    state.pendingPages.clear();
    state.windowPagePositions.clear();
    const int gen = state.gen;

    QStringList blocks = state.pages.at(state.page);
    const QString first = takeDiffStreamBatch(blocks, kDiffFirstPaintChars);
    if (!blocks.isEmpty())
        state.pendingPages.append({state.page, std::move(blocks), true});
    for (int pendingPage = state.page + 1;
         pendingPage <= state.windowLastPage; ++pendingPage) {
        state.pendingPages.append(
            {pendingPage, state.pages.at(pendingPage), false});
    }
    const bool streaming = diffStreamPending(state);
    {
        BlockingCallScope crumb(QStringLiteral("diff html layout (%1 chars, %2)")
                                    .arg(first.size())
                                    .arg(view->objectName().isEmpty()
                                             ? QStringLiteral("unnamed view")
                                             : view->objectName()));
        const forkmesh::BackgroundScope action(
            QStringLiteral("diff"),
            QStringLiteral("layout %1 chars in %2")
                .arg(first.size())
                .arg(view->objectName().isEmpty()
                         ? QStringLiteral("unnamed view")
                         : view->objectName()),
            forkmesh::ActionTelemetry::Execution::UiDeferred);
        view->document()->setDefaultStyleSheet(state.styleSheet);
        view->setHtml(first);
    }
    state.windowPagePositions.insert(state.page, 0);
    scrollLoadedDiffAnchor(view, state, first);
    if (streaming) {
        if (state.pendingViewportPosition < 0 &&
            state.pendingViewportPage < 0) {
            if (QScrollBar *bar = view->verticalScrollBar())
                bar->setValue(bar->maximum());
        }
        scheduleDiffStreamBatch(view, gen);
    } else {
        appendDiffEndCap(view, state);
        finishDiffStream(view);
    }
}

} // namespace

bool restyleDiffStreamed(QTextEdit *view, const QString &styleSheet)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end())
        return false;
    if (it->planning) {
        it->styleSheet = styleSheet;
        view->document()->setDefaultStyleSheet(styleSheet);
        return true;
    }
    if (it->pages.isEmpty())
        return false;
    resumeDiffViewportUpdates(view, *it);
    it->styleSheet = styleSheet;
    it->pendingViewportPage = -1;
    it->pendingViewportY = 0;
    it->pendingScrollPixels = 0;
    it->endCapPosition = -1;
    it->endCapExtraHeight = 0;
    it->endCapResizePending = false;
    renderDiffPage(view, it->page);
    return true;
}

namespace {

void showDiffPage(QTextEdit *view, int page, bool preservePending,
                  int viewportPosition)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || page < 0 || page >= it->pages.size() ||
        page == it->page)
        return;
    resumeDiffViewportUpdates(view, *it);
    if (!preservePending) {
        it->pendingAnchor.clear();
        it->pendingText.clear();
        it->pendingTextDirection = 1;
    }
    it->pendingViewportPage = -1;
    it->pendingViewportY = 0;
    it->pendingScrollPixels = 0;
    it->pendingViewportPosition = viewportPosition;
    it->endCapPosition = -1;
    it->endCapExtraHeight = 0;
    it->endCapResizePending = false;
    if (QScrollBar *bar = view->verticalScrollBar())
        bar->setValue(0);
    renderDiffPage(view, page);
}

int diffPageViewportY(QTextEdit *view, const DiffStreamState &state, int page)
{
    if (!view)
        return 0;
    const int position = state.windowPagePositions.value(page, -1);
    if (position < 0)
        return 0;
    QTextCursor cursor(view->document());
    cursor.setPosition(qBound(0, position,
                              view->document()->characterCount() - 1));
    return view->cursorRect(cursor).top();
}

bool moveDiffWindowAtEdge(QTextEdit *view, int direction, int scrollPixels)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || direction == 0 ||
        diffStreamPending(*it)) {
        return false;
    }
    QScrollBar *bar = view->verticalScrollBar();
    if (!bar)
        return false;
    const int step = direction < 0 ? -1 : 1;
    if ((step > 0 && bar->value() < bar->maximum()) ||
        (step < 0 && bar->value() > bar->minimum())) {
        return false;
    }
    const bool canMove = step > 0
                             ? it->windowLastPage + 1 < it->pages.size()
                             : it->page > 0;
    if (!canMove)
        return false;
    const int overlapPage = step > 0 ? it->page + 1 : it->page;
    if (!it->windowPagePositions.contains(overlapPage))
        return false;
    const int overlapViewportY = diffPageViewportY(view, *it, overlapPage);
    const int target = it->page + step;

    it->pendingAnchor.clear();
    it->pendingText.clear();
    it->pendingTextDirection = 1;
    it->pendingViewportPosition = 0;
    it->pendingViewportPage = overlapPage;
    it->pendingViewportY = overlapViewportY;
    it->pendingScrollPixels += scrollPixels;
    it->endCapPosition = -1;
    it->endCapExtraHeight = 0;
    it->endCapResizePending = false;
    if (!it->viewportUpdatesSuspended && view->updatesEnabled()) {
        it->viewportUpdatesSuspended = true;
        view->setUpdatesEnabled(false);
    }
    bar->setValue(0);
    renderDiffPage(view, target);
    return true;
}

bool scheduleDiffWindowMove(QTextEdit *view, int direction, int scrollPixels)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || diffStreamPending(*it)) {
        return false;
    }
    if (it->edgeMoveScheduled) {
        it->pendingScrollPixels += scrollPixels;
        return true;
    }
    QScrollBar *bar = view->verticalScrollBar();
    const int step = direction < 0 ? -1 : 1;
    const bool canMove = step > 0
                             ? it->windowLastPage + 1 < it->pages.size()
                             : it->page > 0;
    if (!bar || (step > 0 && bar->value() < bar->maximum()) ||
        (step < 0 && bar->value() > bar->minimum()) ||
        !canMove) {
        return false;
    }
    it->edgeMoveScheduled = true;
    it->pendingScrollPixels += scrollPixels;
    const int gen = it->gen;
    QPointer<QTextEdit> guard(view);
    QTimer::singleShot(0, view, [guard, step, gen] {
        if (!guard)
            return;
        auto state = diffStreams().find(guard.data());
        if (state == diffStreams().end() || state->gen != gen ||
            !state->edgeMoveScheduled) {
            return;
        }
        state->edgeMoveScheduled = false;
        const int scrollPixels = state->pendingScrollPixels;
        state->pendingScrollPixels = 0;
        moveDiffWindowAtEdge(guard, step, scrollPixels);
    });
    return true;
}

QString visibleDiffPageText(const QStringList &fragments)
{
    QString visible;
    qsizetype chars = 0;
    for (const QString &fragment : fragments)
        chars += fragment.size();
    visible.reserve(chars);
    bool inTag = false;
    for (const QString &fragment : fragments) {
        for (const QChar ch : fragment) {
            if (inTag) {
                if (ch == QLatin1Char('>')) {
                    inTag = false;
                    visible += QLatin1Char(' ');
                }
                continue;
            }
            if (ch == QLatin1Char('<')) {
                inTag = true;
                visible += QLatin1Char(' ');
                continue;
            }
            visible += ch;
        }
        visible += QLatin1Char(' ');
    }
    return visible;
}

bool diffPageContainsText(const QStringList &fragments, const QString &text)
{
    if (text.isEmpty())
        return false;
    return visibleDiffPageText(fragments).contains(
        text.toHtmlEscaped(), Qt::CaseInsensitive);
}

} // namespace

bool diffStreamInProgress(QTextEdit *view)
{
    const auto it = diffStreams().constFind(view);
    return it != diffStreams().cend() && diffStreamPending(*it);
}

bool diffIsPaginated(QTextEdit *view)
{
    const auto it = diffStreams().constFind(view);
    return it != diffStreams().cend() && it->pages.size() > 1;
}

bool showDiffPageContainingText(QTextEdit *view, const QString &text,
                                int direction, bool wrap)
{
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || text.isEmpty() ||
        it->pages.size() <= 1 || direction == 0) {
        return false;
    }
    const int step = direction < 0 ? -1 : 1;
    const int count = it->pages.size();
    int page = it->page;
    for (int checked = 1; checked < count; ++checked) {
        page += step;
        if (page < 0 || page >= count) {
            if (!wrap)
                break;
            page = page < 0 ? count - 1 : 0;
        }
        if (!diffPageContainsText(it->pages.at(page), text))
            continue;
        it->pendingAnchor.clear();
        it->pendingText = text;
        it->pendingTextDirection = step;
        showDiffPage(view, page, /*preservePending=*/true);
        return true;
    }
    return false;
}

void scrollDiffToAnchor(QTextEdit *view, const QString &anchor)
{
    if (!view || anchor.isEmpty())
        return;
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || it->pages.isEmpty()) {
        if (it != diffStreams().end() && it->planning)
            it->pendingAnchor = anchor;
        if (auto *browser = qobject_cast<QTextBrowser *>(view))
            browser->scrollToAnchor(anchor);
        return;
    }
    const QString marker = QStringLiteral("name=\"") + anchor +
                           QStringLiteral("\"");
    int targetPage = -1;
    for (int page = 0; page < it->pages.size() && targetPage < 0; ++page) {
        for (const QString &fragment : it->pages.at(page)) {
            if (fragment.contains(marker)) {
                targetPage = page;
                break;
            }
        }
    }
    if (targetPage < 0) {
        if (auto *browser = qobject_cast<QTextBrowser *>(view))
            browser->scrollToAnchor(anchor);
        return;
    }
    it->pendingAnchor = anchor;
    if (targetPage < it->page || targetPage > it->windowLastPage)
        showDiffPage(view, targetPage, /*preservePending=*/true);
    else if (auto *browser = qobject_cast<QTextBrowser *>(view))
        browser->scrollToAnchor(anchor);
}

void flushDiffStream(QTextEdit *view)
{
    if (!view)
        return;
    auto it = diffStreams().find(view);
    if (it == diffStreams().end() || !diffStreamPending(*it))
        return;
}

#ifdef FORKMESH_WINDOW_TESTS
QList<qsizetype> diffPlannedUiApplySizesForTest(const QString &html)
{
    const QList<QStringList> pages = paginateDiffHtml(html);
    QList<qsizetype> sizes;
    for (int page = 0; page < pages.size(); ++page) {
        QStringList pending = pages.at(page);
        if (pending.isEmpty())
            continue;
        sizes.append(takeDiffStreamBatch(pending, kDiffFirstPaintChars).size());
        while (!pending.isEmpty())
            sizes.append(takeDiffStreamBatch(pending, kDiffStreamBatchChars).size());
    }
    return sizes;
}

qsizetype diffMaxUiApplyCharsForTest()
{
    return kDiffMaxUiApplyChars;
}

QStringList diffFileBlocksForTest(const QString &html)
{
    return splitDiffFileBlocks(html);
}

QList<QStringList> diffPlannedPagesForTest(const QString &html)
{
    return paginateDiffHtml(html);
}

int diffPageCountForTest(QTextEdit *view)
{
    const auto it = diffStreams().constFind(view);
    return it == diffStreams().cend() ? 0 : it->pages.size();
}

void showDiffPageForTest(QTextEdit *view, int page)
{
    showDiffPage(view, page);
}

int diffWindowFirstPageForTest(QTextEdit *view)
{
    const auto it = diffStreams().constFind(view);
    return it == diffStreams().cend() ? -1 : it->page;
}

int diffLoadedPageViewportYForTest(QTextEdit *view, int page)
{
    const auto it = diffStreams().constFind(view);
    if (it == diffStreams().cend() ||
        !it->windowPagePositions.contains(page)) {
        return -0x3fffffff;
    }
    return diffPageViewportY(view, *it, page);
}
#endif

void addDiffStreamFinishedHook(QTextEdit *view, std::function<void()> hook)
{
    if (!view || !hook)
        return;
    diffStreamState(view).finishedHooks.append(std::move(hook));
}

QString agentCostText(double usd)
{
    return QStringLiteral("$%1").arg(usd, 0, 'f', 2);
}

QString linkifyIssueRefs(const QString &escaped)
{
    static const QRegularExpression re(QStringLiteral(
        "(?<![\\w/#])(?<pull>(?:pull[-\\s]+request|pull|pr)[-\\s]*#(?<pullNumber>\\d+))\\b"
        "|(?<![\\w/#])(?<issue>issue[-\\s]*#(?<issueNumber>\\d+))\\b"
        "|(?<![\\w/#])#(?<bareNumber>\\d+)\\b"
        "|\\b(?=[0-9a-f]*[a-f])(?<commit>[0-9a-f]{7,40})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    int last = 0;
    auto it = re.globalMatch(escaped);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += escaped.mid(last, m.capturedStart() - last);
        if (!m.captured(QStringLiteral("pull")).isEmpty()) {
            out += QStringLiteral(
                       "<a href=\"pull:%1\" style=\"color:#58a6ff;text-decoration:none\">"
                       "%2</a>")
                       .arg(m.captured(QStringLiteral("pullNumber")),
                            m.captured(QStringLiteral("pull")));
        } else if (!m.captured(QStringLiteral("issue")).isEmpty()) {
            out += QStringLiteral(
                       "<a href=\"issue:%1\" style=\"color:#58a6ff;text-decoration:none\">"
                       "%2</a>")
                       .arg(m.captured(QStringLiteral("issueNumber")),
                            m.captured(QStringLiteral("issue")));
        } else if (!m.captured(QStringLiteral("bareNumber")).isEmpty()) {
            const QString num = m.captured(QStringLiteral("bareNumber"));
            out += QStringLiteral(
                       "<a href=\"ref:%1\" style=\"color:#58a6ff;text-decoration:none\">"
                       "#%1</a>")
                       .arg(num);
        } else {
            const QString sha = m.captured(QStringLiteral("commit"));
            out += QStringLiteral(
                       "<a href=\"commit:%1\" style=\"color:#58a6ff;text-decoration:none\">"
                       "%1</a>")
                       .arg(sha);
        }
        last = m.capturedEnd();
    }
    out += escaped.mid(last);
    return out;
}

const QRegularExpression &bodyReferenceRegex()
{
    static const QRegularExpression re(QStringLiteral(
        "(?<permalink>forkmesh://(?:issue|pull|commit|cove)/[^\\s<>()\\[\\]]*[^\\s<>()\\[\\].,;:!?'\"])"
        "|(?<![\\w/#])(?<pull>(?:pull[-\\s]+request|pull|pr)[-\\s]*#(?<pullNumber>\\d+))\\b"
        "|(?<![\\w/#])(?<issue>issue[-\\s]*#(?<issueNumber>\\d+))\\b"
        "|(?<![\\w/#])#(?<bareNumber>\\d+)\\b"
        "|(?<![\\w@/.+-])@(?<mention>[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)(?![\\w-]|[./][a-z0-9]|@|\\[bot\\])"
        "|\\b(?=[0-9a-f]*[a-f])(?<commit>[0-9a-f]{7,40})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString userProfileAccount(const QString &name)
{
    QString account = name.trimmed();
    if (account.startsWith(QLatin1Char('@')))
        account.remove(0, 1);
    account = account.toLower();
    return isValidNodeName(account) ? account : QString();
}

QString userProfileLinkHtml(const QString &account, const QString &shown)
{
    const QString text = shown.isEmpty() ? account.trimmed() : shown;
    const QString key = userProfileAccount(account);
    if (key.isEmpty())
        return text.toHtmlEscaped();
    return QStringLiteral("<a href=\"%1%2\" style=\"text-decoration:none\">%3</a>")
        .arg(kUserLinkScheme, key, text.toHtmlEscaped());
}

QString linkifyReferenceText(const QString &text,
                             const ReferenceStatusGlyph &statusGlyph)
{
    auto glyph = [&](const QString &kind, const QString &number) {
        return statusGlyph ? statusGlyph(kind, number.toInt()) : QString();
    };
    QString out;
    int last = 0;
    auto it = bodyReferenceRegex().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        if (!m.captured(QStringLiteral("permalink")).isEmpty()) {
            out += QStringLiteral("<%1>")
                       .arg(m.captured(QStringLiteral("permalink")));
        } else if (!m.captured(QStringLiteral("pull")).isEmpty()) {
            out += glyph(QStringLiteral("pull"),
                         m.captured(QStringLiteral("pullNumber")));
            out += QStringLiteral("[%1](forkmesh-pull:%2)")
                       .arg(m.captured(QStringLiteral("pull")),
                            m.captured(QStringLiteral("pullNumber")));
        } else if (!m.captured(QStringLiteral("issue")).isEmpty()) {
            out += glyph(QStringLiteral("issue"),
                         m.captured(QStringLiteral("issueNumber")));
            out += QStringLiteral("[%1](forkmesh-issue:%2)")
                       .arg(m.captured(QStringLiteral("issue")),
                            m.captured(QStringLiteral("issueNumber")));
        } else if (!m.captured(QStringLiteral("bareNumber")).isEmpty()) {
            const QString num = m.captured(QStringLiteral("bareNumber"));
            out += glyph(QStringLiteral("ref"), num);
            out += QStringLiteral("[#%1](forkmesh-ref:%1)").arg(num);
        } else if (!m.captured(QStringLiteral("mention")).isEmpty()) {
            const QString login = m.captured(QStringLiteral("mention"));
            out += QStringLiteral("[@%1](%2%3)")
                       .arg(login, kUserLinkScheme, login.toLower());
        } else {
            const QString sha = m.captured(QStringLiteral("commit"));
            out += QStringLiteral("[%1](forkmesh-commit:%1)").arg(sha);
        }
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

int markdownLinkSpanEnd(const QString &line, int start)
{
    const int close = line.indexOf(QLatin1Char(']'), start + 1);
    if (close < 0)
        return -1;
    const int paren = close + 1;
    if (paren >= line.size() || line.at(paren) != QLatin1Char('('))
        return -1;
    const int end = line.indexOf(QLatin1Char(')'), paren + 1);
    return end < 0 ? -1 : end + 1;
}

int autolinkSpanEnd(const QString &line, int start)
{
    const int close = line.indexOf(QLatin1Char('>'), start + 1);
    if (close < 0)
        return -1;
    if (!line.mid(start + 1, close - start - 1).contains(QStringLiteral("://")))
        return -1;
    return close + 1;
}

QString linkifyReferenceLine(const QString &line,
                             const ReferenceStatusGlyph &statusGlyph)
{
    QString out;
    const int n = line.size();
    int i = 0;
    while (i < n) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('`')) {
            int run = 1;
            while (i + run < n && line.at(i + run) == QLatin1Char('`'))
                ++run;
            const QString ticks = line.mid(i, run);
            int close = i + run;
            int found = -1;
            while (close < n) {
                const int idx = line.indexOf(ticks, close);
                if (idx < 0)
                    break;
                const int after = idx + run;
                if (after < n && line.at(after) == QLatin1Char('`')) {
                    close = after; // part of a longer run; keep looking
                    continue;
                }
                found = idx;
                break;
            }
            if (found >= 0) {
                out += line.mid(i, found + run - i);
                i = found + run;
            } else {
                out += ticks; // unterminated: emit literally
                i += run;
            }
            continue;
        }
        if (c == QLatin1Char('[')) {
            const int end = markdownLinkSpanEnd(line, i);
            if (end > i) {
                out += line.mid(i, end - i);
                i = end;
                continue;
            }
            out += c;
            ++i;
            continue;
        }
        if (c == QLatin1Char('<')) {
            const int end = autolinkSpanEnd(line, i);
            if (end > i) {
                out += line.mid(i, end - i);
                i = end;
                continue;
            }
            out += c;
            ++i;
            continue;
        }
        int j = i;
        while (j < n) {
            const QChar d = line.at(j);
            if (d == QLatin1Char('`') || d == QLatin1Char('[') || d == QLatin1Char('<'))
                break;
            ++j;
        }
        out += linkifyReferenceText(line.mid(i, j - i), statusGlyph);
        i = j;
    }
    return out;
}


namespace {

// An overlay parented to the viewport handed its wheel events to the viewport
// by plain propagation, which is where the diff's Ctrl+wheel zoom and its
// page-window paging both listen. Off the viewport they would go to the view
// instead, so pass them back by hand.
class DiffOverlayWheelForwarder final : public QObject
{
public:
    DiffOverlayWheelForwarder(QTextEdit *view, QObject *parent)
        : QObject(parent), m_view(view)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Wheel || !m_view || !m_view->viewport())
            return QObject::eventFilter(watched, event);
        QCoreApplication::sendEvent(m_view->viewport(), event);
        // Only swallow it if the diff took it: a wheel the view cannot act on
        // still belongs to whatever scrolls outside it, as it did when this
        // overlay was a child of the viewport itself.
        return event->isAccepted();
    }

private:
    QPointer<QTextEdit> m_view;
};

} // namespace

QWidget *diffOverlayParent(QTextEdit *view)
{
    return view; // NOT view->viewport() — see the header for why
}

void placeDiffOverlay(QWidget *overlay, QTextEdit *view,
                      const QRect &viewportRect)
{
    if (!overlay || !view || !view->viewport())
        return;
    static const char *kAdopted = "forkmeshDiffOverlay";
    if (!overlay->property(kAdopted).toBool()) {
        overlay->setProperty(kAdopted, true);
        overlay->installEventFilter(new DiffOverlayWheelForwarder(view, overlay));
    }
    overlay->setGeometry(viewportRect.translated(view->viewport()->pos()));
}

int diffStickyTopOffset(int stickyHeight, int viewTop, int fileBottom)
{
    // The bar stands in for the file whose rows are scrolled off the top. Once
    // the next file's own header has arrived there is nothing left to stand in
    // for, and a bar still pinned at y=0 simply covers that header — which is
    // what made a run of collapsed (viewed) rows look like it was overlapping
    // itself. Give the incoming header its space back by sliding the bar out by
    // exactly as much of it as the header has already claimed.
    const int remaining = fileBottom - viewTop;
    if (stickyHeight <= 0 || remaining >= stickyHeight)
        return 0;
    return qMax(-stickyHeight, remaining - stickyHeight);
}

QRectF diffFileRowRect(const QTextEdit *view, int position)
{
    if (!view || !view->document() || !view->document()->documentLayout())
        return {};
    QTextDocument *doc = view->document();
    QTextCursor cursor(doc);
    cursor.setPosition(qBound(0, position, doc->characterCount() - 1));
    QTextTable *table = cursor.currentTable();
    // Only the header table is one row tall. Anywhere else — a hunk row, a
    // notice-only page — the arithmetic below would measure the whole body.
    if (!table || table->rows() != 1)
        return {};
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    const QRectF frame = layout->frameBoundingRect(table);
    if (frame.isNull())
        return {};
    // frameBoundingRect() reports a table at the position of its first cell's
    // *content*: a border, a padding and the vertical centring below and to the
    // right of the row's own top-left corner. The row is a single line centred
    // in its own height, so the content's height gives that centring back.
    QRectF content;
    const QTextTableCell first = table->cellAt(0, 0);
    for (QTextFrame::iterator it = first.begin(); !it.atEnd(); ++it) {
        const QTextBlock block = it.currentBlock();
        if (block.isValid())
            content |= layout->blockBoundingRect(block);
    }
    // Horizontally there is nothing to recover: every diff table is laid out
    // across the full document width, so the row starts at the document's edge.
    return QRectF(0, frame.top() + content.height() / 2 - frame.height() / 2,
                  frame.width(), frame.height());
}

QString diffStickyStyleSheet(int fontPt)
{
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    return QStringLiteral("background:%1; border:1px solid %2; padding:6px 10px;"
                          " font-family:monospace; font-size:%3px;")
        .arg(dark ? QStringLiteral("#161b22") : QStringLiteral("#f6f8fa"),
             dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de"))
        .arg(qBound(8, fontPt, 28));
}

void configureDiffStickyPathLabel(QLabel *label)
{
    if (!label)
        return;
    label->setWordWrap(false);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

QString diffStickyPathText(const QString &path)
{
    constexpr int kMaxChars = 72;
    if (path.size() <= kMaxChars)
        return path;
    return QString::fromUtf8("\xE2\x80\xA6") + path.right(kMaxChars - 1);
}

QString diffStickyPathHtml(const QString &fullPath)
{
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString dirFg = Theme::nextOrHex(
        Theme::Role::FgMuted,
        dark ? QStringLiteral("#8b949e") : QStringLiteral("#6e7781"));
    const QString nameFg = Theme::nextOrHex(
        Theme::Role::Fg,
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"));
    const QString path = diffStickyPathText(fullPath);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        return QStringLiteral("<span style='color:%1'>%2</span>"
                              "<span style='color:%3; font-weight:600'>%4</span>")
            .arg(dirFg, path.left(slash + 1).toHtmlEscaped(), nameFg,
                 path.mid(slash + 1).toHtmlEscaped());
    return QStringLiteral("<span style='color:%1; font-weight:600'>%2</span>")
        .arg(nameFg, path.toHtmlEscaped());
}

QString agentStatusText(const QString &status)
{
    if (status == AgentStatus::Queued) return QStringLiteral("Queued");
    if (status == AgentStatus::Running) return QStringLiteral("Running");
    if (status == AgentStatus::Waiting) return QStringLiteral("Waiting");
    if (status == AgentStatus::Success) return QStringLiteral("Success");
    if (status == AgentStatus::Failed) return QStringLiteral("Failed");
    if (status == AgentStatus::Stopped) return QStringLiteral("Stopped");
    if (status == AgentStatus::Cleared) return QStringLiteral("Cleared");
    return status;
}

bool agentSessionActive(const AgentSession *s)
{
    return s && (s->status == AgentStatus::Running ||
                 s->status == AgentStatus::Queued);
}

QColor agentStatusColor(const QString &status)
{
    if (status == AgentStatus::Success) return QColor("#3fb950");
    if (status == AgentStatus::Failed) return QColor("#f85149");
    if (status == AgentStatus::Running) return QColor(Theme::kRunning);
    if (status == AgentStatus::Queued) return QColor("#d29922");
    if (status == AgentStatus::Waiting) return QColor("#d29922");
    if (status == AgentStatus::Stopped) return Theme::remapColor(QColor("#8b949e"));
    if (status == AgentStatus::Cleared) return Theme::remapColor(QColor("#8b949e"));
    return Theme::remapColor(QColor("#8b949e"));
}

QColor agentStatusIconColor(const AgentSession &s)
{
    if (s.merged)
        return Theme::nextOr(Theme::Role::AccentFg, QColor("#a371f7"));
    if (s.genieInFlight())
        return Theme::nextOr(Theme::Role::AccentFg, QColor(Theme::kGenie));
    if (s.status == AgentStatus::Running) return QColor(Theme::kRunning);
    if (s.status == AgentStatus::Success) return QColor("#3fb950");
    if (s.status == AgentStatus::Failed) return QColor("#f85149");
    if (s.status == AgentStatus::Stopped) return QColor("#f85149");
    if (s.status == AgentStatus::Waiting)
        return Theme::nextOr(Theme::Role::Warning, QColor("#e3742f"));
    if (s.awaitingStart())
        return Theme::remapColor(QColor("#8b949e"));
    if (s.status == AgentStatus::Queued) return QColor("#d29922");
    return Theme::remapColor(QColor("#8b949e")); // cleared / unknown
}

QIcon agentStatusOcticon(const AgentSession &s, int px)
{
    const QColor tint = agentStatusIconColor(s);
    if (s.merged)
        return themedOcticon("git-merge", tint, px);
    if (s.genieInFlight())
        return themedOcticon("sparkle", tint, px);
    if (s.status == AgentStatus::Running)
        return themedOcticon("sync", tint, px);
    if (s.status == AgentStatus::Success)
        return themedOcticon("check-circle", tint, px);
    if (s.status == AgentStatus::Failed)
        return themedOcticon("x", tint, px);
    if (s.status == AgentStatus::Stopped)
        return themedOcticon("stop", tint, px);
    if (s.status == AgentStatus::Waiting)
        return themedOcticon("hand", tint, px);
    if (s.awaitingStart())
        return themedOcticon("play", tint, px);
    if (s.status == AgentStatus::Queued)
        return themedOcticon("history", tint, px);
    return themedOcticon("circle-slash", tint, px);
}


QString diffImageMimeForPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".png")))
        return QStringLiteral("image/png");
    if (lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lower.endsWith(QStringLiteral(".gif")))
        return QStringLiteral("image/gif");
    if (lower.endsWith(QStringLiteral(".webp")))
        return QStringLiteral("image/webp");
    if (lower.endsWith(QStringLiteral(".bmp")))
        return QStringLiteral("image/bmp");
    if (lower.endsWith(QStringLiteral(".ico")))
        return QStringLiteral("image/x-icon");
    return QString();
}

// QTextDocument honours neither the CSS width nor max-height on an image, so an
// untouched screenshot lays itself out at its full pixel size and pushes the
// "after" side off the right edge of the diff — no side-by-side at all. Every
// preview is therefore decoded, scaled to fit this box, and emitted with
// explicit width/height attributes, which Qt does respect. Two of these plus
// the cell padding fit the narrowest pane we render a diff into.
constexpr int kDiffImageBoxWidth = 420;
constexpr int kDiffImageBoxHeight = 360;
// ~40MP, comfortably above any real diff asset: the guard is against a
// "decompression bomb" (a small file whose declared dimensions blow up into a
// huge pixel buffer once decoded).
constexpr qint64 kDiffImageMaxPixels = 40'000'000;
// What one <img> data URI may cost the document once base64-expanded. A frame
// scaled into the box above lands well inside this; the ceiling only catches
// pathological re-encodes.
constexpr qsizetype kDiffImageMaxInlineBytes = 192 * 1024;

bool diffBytesAreWebp(const QByteArray &bytes)
{
    return bytes.size() >= 12 && bytes.startsWith("RIFF") &&
           bytes.mid(8, 4) == QByteArray("WEBP");
}

QImage diffDecodeWebp(const QByteArray &bytes)
{
#ifdef FORKMESH_HAVE_WEBP
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
    const size_t length = size_t(bytes.size());
    WebPBitstreamFeatures features{};
    // Reading the features touches the header only, so the bomb check still
    // happens before any pixel buffer is allocated.
    if (WebPGetFeatures(data, length, &features) != VP8_STATUS_OK)
        return {};
    int width = features.width;
    int height = features.height;
    if (width <= 0 || height <= 0 ||
        qint64(width) * qint64(height) > kDiffImageMaxPixels)
        return {};
    // A lossy webp photo carries no alpha, and handing one back regardless
    // would claim it does: that alone holds the inline encoder to PNG, which
    // for a full-width screenshot lands past the budget and drops the preview
    // the reviewer came for. Without alpha it can fall back to JPEG.
    if (!features.has_alpha) {
        uint8_t *rgb = WebPDecodeRGB(data, length, &width, &height);
        if (!rgb)
            return {}; // animated webp needs the demuxer: falls back to a notice
        const QImage opaque =
            QImage(rgb, width, height, width * 3, QImage::Format_RGB888).copy();
        WebPFree(rgb);
        return opaque;
    }
    uint8_t *rgba = WebPDecodeRGBA(data, length, &width, &height);
    if (!rgba)
        return {}; // animated WebP needs the demuxer: falls back to a notice
    // The buffer is a plain malloc'd block libwebp still owns, so copy out of it
    // before handing it back.
    const QImage decoded =
        QImage(rgba, width, height, width * 4, QImage::Format_RGBA8888).copy();
    WebPFree(rgba);
    return decoded;
#else
    Q_UNUSED(bytes);
    return {};
#endif
}

struct DiffDecodedImage {
    QImage image;
    // True when Qt read the bytes itself, which is also the test for whether
    // QTextDocument could render those same bytes inline unmodified.
    bool qtReadable = false;
};

// A diff can carry an attacker-influenced image straight into an auto-rendered
// view (an untrusted PR's asset, or a file an issue agent fetched), so it isn't
// enough to trust the ".png" extension. QImageReader::canRead() sniffs the
// actual content and size() reads just the format header, so a bomb is rejected
// before its pixels are ever allocated.
DiffDecodedImage diffDecodeImage(const QByteArray &bytes)
{
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    if (reader.canRead()) {
        const QSize size = reader.size();
        if (size.isValid() &&
            qint64(size.width()) * qint64(size.height()) > kDiffImageMaxPixels)
            return {};
        // Where the plugin can scale during decode (JPEG especially) this skips
        // ever materialising the full-resolution frame.
        if (size.isValid() && (size.width() > kDiffImageBoxWidth ||
                               size.height() > kDiffImageBoxHeight))
            reader.setScaledSize(
                size.scaled(kDiffImageBoxWidth, kDiffImageBoxHeight,
                            Qt::KeepAspectRatio)
                    .expandedTo(QSize(1, 1)));
        const QImage decoded = reader.read();
        if (!decoded.isNull())
            return {decoded, true};
    }
    if (diffBytesAreWebp(bytes))
        return {diffDecodeWebp(bytes), false};
    return {};
}

struct DiffImageInline {
    QByteArray bytes;
    QString mime;
    QSize size;
};

DiffImageInline diffEncodeInlineImage(const QImage &image)
{
    QByteArray png;
    QBuffer pngBuf(&png);
    pngBuf.open(QIODevice::WriteOnly);
    if (!image.save(&pngBuf, "PNG"))
        return {};
    if (png.size() <= kDiffImageMaxInlineBytes || image.hasAlphaChannel())
        return {png, QStringLiteral("image/png"), image.size()};
    // A photograph stays large even at display size when it is stored
    // losslessly. There is no alpha left to lose here, so JPEG keeps the inline
    // payload inside the document's budget.
    QByteArray jpeg;
    QBuffer jpegBuf(&jpeg);
    jpegBuf.open(QIODevice::WriteOnly);
    if (image.save(&jpegBuf, "JPEG", 85) && !jpeg.isEmpty() && jpeg.size() < png.size())
        return {jpeg, QStringLiteral("image/jpeg"), image.size()};
    return {png, QStringLiteral("image/png"), image.size()};
}

DiffImageInline diffImageInlineFor(const QByteArray &source, const QString &mime)
{
    // Decode + smooth rescale + PNG (and maybe JPEG) encode, on the GUI thread
    // — sampled at ~1 s per stall on image-bearing diffs — and re-run for the
    // same blob every time the view re-renders (pane switches, re-keyed
    // renders, before/after pairs that share bytes). Content-addressed memo:
    // the result is a pure function of (bytes, mime).
    struct InlineMemo {
        DiffImageInline value;
        qsizetype cost = 0;
    };
    static QMutex memoMutex;
    static QHash<QByteArray, InlineMemo> memo;
    static qsizetype memoBytes = 0;
    constexpr qsizetype kMemoBudgetBytes = 24 * 1024 * 1024;
    const QByteArray memoKey =
        QCryptographicHash::hash(source, QCryptographicHash::Sha1) + '|' +
        mime.toUtf8();
    {
        QMutexLocker locker(&memoMutex);
        if (const auto hit = memo.constFind(memoKey); hit != memo.cend())
            return hit->value;
    }
    const auto remember = [&](const DiffImageInline &value) {
        const qsizetype cost = value.bytes.size() + 128;
        QMutexLocker locker(&memoMutex);
        if (memoBytes + cost > kMemoBudgetBytes) {
            memo.clear();
            memoBytes = 0;
        }
        memo.insert(memoKey, {value, cost});
        memoBytes += cost;
        return value;
    };
    const DiffDecodedImage decoded = diffDecodeImage(source);
    if (decoded.image.isNull())
        return remember({});
    QImage shown = decoded.image;
    if (shown.width() > kDiffImageBoxWidth || shown.height() > kDiffImageBoxHeight)
        shown = shown.scaled(kDiffImageBoxWidth, kDiffImageBoxHeight,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (shown.isNull() || shown.width() < 1 || shown.height() < 1)
        return remember({});
    // An icon that already fits goes in untouched: no re-encode artefacts, and
    // the original stays byte-for-byte what the repository holds.
    if (decoded.qtReadable && shown.size() == decoded.image.size() &&
        source.size() <= kDiffImageMaxInlineBytes)
        return remember({source, mime, shown.size()});
    return remember(diffEncodeInlineImage(shown));
}

QString diffImageNoticeCell(const QString &label, const QString &text)
{
    return QStringLiteral("<td class='imgcell'><div class='imgempty'>%1</div>"
                          "<div class='imgcaption'>%2</div></td>")
        .arg(text.toHtmlEscaped(), label.toHtmlEscaped());
}

QString diffImageCellHtml(const QString &label, const QString &path,
                          const QString &mime, const QByteArray &bytes,
                          const QString &absentNote)
{
    if (!absentNote.isEmpty())
        return diffImageNoticeCell(label, absentNote);
    if (bytes.isEmpty())
        return diffImageNoticeCell(label, QStringLiteral("Not present"));
    const DiffImageInline inlined = diffImageInlineFor(bytes, mime);
    if (inlined.bytes.isEmpty())
        return diffImageNoticeCell(
            label, QStringLiteral("Preview unavailable (unreadable image)"));
    if (inlined.bytes.size() > kDiffImageMaxInlineBytes)
        return diffImageNoticeCell(
            label, QStringLiteral("Preview too large to show inline"));
    return QStringLiteral("<td class='imgcell'><img width=\"%1\" height=\"%2\" "
                          "alt=\"%3: %4\" src=\"data:%5;base64,%6\">"
                          "<div class='imgcaption'>%3</div></td>")
        .arg(QString::number(inlined.size.width()),
             QString::number(inlined.size.height()), label.toHtmlEscaped(),
             path.toHtmlEscaped(), inlined.mime,
             QString::fromLatin1(inlined.bytes.toBase64()));
}

namespace {

struct DiffImageBlobCache {
    struct Entry {
        QByteArray bytes;
        qint64 readAtMs = 0;
    };
    QMutex mutex;
    QHash<QString, Entry> entries;
    QSet<QString> warming;
};

DiffImageBlobCache &diffImageBlobCache()
{
    static DiffImageBlobCache cache;
    return cache;
}

constexpr qint64 kDiffImageBlobTtlMs = 30'000;

QString diffImageBlobKey(const QString &dir, const QString &ref, const QString &path)
{
    return dir + QLatin1Char('\n') + ref + QLatin1Char('\n') + path;
}

bool diffImageBlobCached(const QString &key, QByteArray *bytes)
{
    DiffImageBlobCache &cache = diffImageBlobCache();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker locker(&cache.mutex);
    const auto hit = cache.entries.constFind(key);
    if (hit == cache.entries.constEnd() || now - hit->readAtMs >= kDiffImageBlobTtlMs)
        return false;
    *bytes = hit->bytes;
    return true;
}

bool onGuiThread()
{
    const QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

} // namespace

QByteArray diffImageBlob(const QString &dir, const QString &ref, const QString &path)
{
    const QString key = diffImageBlobKey(dir, ref, path);
    QByteArray bytes;
    if (diffImageBlobCached(key, &bytes))
        return bytes;
    if (!runGitCapture(dir, {QStringLiteral("show"), ref + QLatin1Char(':') + path},
                       &bytes, nullptr))
        bytes.clear();
    DiffImageBlobCache &cache = diffImageBlobCache();
    QMutexLocker locker(&cache.mutex);
    constexpr qsizetype kBlobCacheBytes = 24 * 1024 * 1024;
    qsizetype held = bytes.size();
    for (auto it = cache.entries.cbegin(); it != cache.entries.cend(); ++it)
        held += it->bytes.size();
    if (cache.entries.size() > 64 || held > kBlobCacheBytes)
        cache.entries.clear();
    cache.entries.insert(key, {bytes, QDateTime::currentMSecsSinceEpoch()});
    return bytes;
}

QByteArray diffImageBlobForPreview(const QString &dir, const QString &ref,
                                   const QString &path, bool *loading)
{
    if (!onGuiThread())
        return diffImageBlob(dir, ref, path);
    const QString key = diffImageBlobKey(dir, ref, path);
    QByteArray bytes;
    if (diffImageBlobCached(key, &bytes))
        return bytes;
    if (loading)
        *loading = true;
    DiffImageBlobCache &cache = diffImageBlobCache();
    {
        QMutexLocker locker(&cache.mutex);
        if (cache.warming.contains(key))
            return QByteArray();
        cache.warming.insert(key);
    }
    QThreadPool::globalInstance()->start([dir, ref, path, key] {
        diffImageBlob(dir, ref, path);
        DiffImageBlobCache &warm = diffImageBlobCache();
        QMutexLocker locker(&warm.mutex);
        warm.warming.remove(key);
    });
    return QByteArray();
}

// Sources above this never reach a decoder. Anything a repository realistically
// carries — logos, screenshots, sprite sheets — sits well inside it, and the
// preview is scaled to display size afterwards regardless.
constexpr qsizetype kDiffImageMaxSourceBytes = 2 * 1024 * 1024;

// git's base85 alphabet (base85.c), which is its own, not ascii85's.
int diffBase85Digit(char16_t ch)
{
    static const std::array<int, 128> table = [] {
        std::array<int, 128> digits{};
        digits.fill(-1);
        const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz"
                                "!#$%&()*+-;<=>?@^_`{|}~";
        for (int i = 0; alphabet[i] != '\0'; ++i)
            digits[static_cast<unsigned char>(alphabet[i])] = i;
        return digits;
    }();
    return ch < table.size() ? table[ch] : -1;
}

// Each payload line opens with its own byte count: 'A'-'Z' for 1-26 bytes,
// 'a'-'z' for 27-52.
int diffBase85LineBytes(char16_t ch)
{
    if (ch >= u'A' && ch <= u'Z')
        return int(ch - u'A') + 1;
    if (ch >= u'a' && ch <= u'z')
        return int(ch - u'a') + 27;
    return 0;
}

bool diffDecodeBase85Line(QStringView encoded, int bytes, QByteArray &out)
{
    int offset = 0;
    while (bytes > 0) {
        quint32 acc = 0;
        for (int i = 0; i < 5; ++i) {
            if (offset >= encoded.size())
                return false;
            const int digit = diffBase85Digit(encoded.at(offset++).unicode());
            if (digit < 0)
                return false;
            // Five digits can address more than 32 bits, so a group that would
            // wrap is corruption rather than data.
            if (acc > (std::numeric_limits<quint32>::max() - quint32(digit)) / 85)
                return false;
            acc = acc * 85 + quint32(digit);
        }
        for (int shift = 24; shift >= 0 && bytes > 0; shift -= 8, --bytes)
            out.append(char((acc >> shift) & 0xff));
    }
    return true;
}

// One side of a "GIT binary patch" block, decoded back into the file's bytes.
//
// git writes the post-image first and the pre-image second, each headed by
// "literal <inflated size>" and holding a zlib stream in the base85 above,
// closed by a blank line. That header is exactly what qUncompress() wants in
// front of the stream. A "delta" block is expressed against a blob that may not
// be here at all, so it is left to the "binary file changed" row.
QByteArray diffBinaryPatchImage(const QString &payload, bool preImage)
{
    if (payload.isEmpty())
        return {};
    const int wanted = preImage ? 1 : 0;
    int block = -1;
    bool literal = false;
    qint64 expanded = 0;
    QByteArray deflated;
    for (QStringView line : QStringView(payload).split(QLatin1Char('\n'))) {
        while (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const bool isLiteral = line.startsWith(QLatin1String("literal "));
        if (isLiteral || line.startsWith(QLatin1String("delta "))) {
            if (block == wanted)
                break; // the block we wanted has just ended
            ++block;
            literal = isLiteral;
            expanded = line.mid(line.indexOf(QLatin1Char(' ')) + 1).toLongLong();
            deflated.clear();
            continue;
        }
        if (block != wanted)
            continue;
        if (line.isEmpty())
            break; // a blank line closes the block
        const int bytes = diffBase85LineBytes(line.at(0).unicode());
        if (bytes <= 0 || !diffDecodeBase85Line(line.mid(1), bytes, deflated))
            return {};
        // Deflate rarely grows a file, so a stream this far past the source
        // ceiling cannot inflate to anything we would show.
        if (deflated.size() > kDiffImageMaxSourceBytes + 64 * 1024)
            return {};
    }
    if (block != wanted || !literal || expanded <= 0 ||
        expanded > kDiffImageMaxSourceBytes || deflated.isEmpty()) {
        return {};
    }
    QByteArray framed;
    framed.reserve(deflated.size() + 4);
    for (int shift = 24; shift >= 0; shift -= 8)
        framed.append(char((expanded >> shift) & 0xff));
    framed.append(deflated);
    const QByteArray image = qUncompress(framed);
    return image.size() == expanded ? image : QByteArray();
}

QString diffImageTooLargeNote(qsizetype bytes)
{
    return QStringLiteral("Too large to preview (%1)")
        .arg(QLocale().formattedDataSize(bytes));
}

QString diffImagePreviewHtml(const QString &dir, const QString &base,
                             const QString &head, const DiffFileEntry &f,
                             const QString &binaryPatch)
{
    const QString &path = f.path;
    const QString mime = diffImageMimeForPath(path);
    if (mime.isEmpty())
        return QString();

    // A patch written with --binary — which every patch ForkMesh moves between
    // nodes is — carries both frames itself. Reading them straight out of it
    // costs no git call, and it is the only source for a diff whose refs are
    // not on this machine: a patch-backed pull request, a fork's branch, a
    // range whose files never existed in this working tree.
    const QString loadingNote = QString::fromUtf8("Loading preview\xE2\x80\xA6");
    QByteArray oldBytes = diffBinaryPatchImage(binaryPatch, /*preImage=*/true);
    QString oldNote;
    if (oldBytes.isEmpty() && f.status != QLatin1String("added") && !dir.isEmpty()) {
        const QString oldRef = base.isEmpty() ? QStringLiteral("HEAD") : base;
        bool loading = false;
        oldBytes = diffImageBlobForPreview(dir, oldRef, path, &loading);
        if (loading)
            oldNote = loadingNote;
        if (oldBytes.size() > kDiffImageMaxSourceBytes) {
            oldNote = diffImageTooLargeNote(oldBytes.size());
            oldBytes.clear();
        }
    }
    QByteArray newBytes = diffBinaryPatchImage(binaryPatch, /*preImage=*/false);
    QString newNote;
    if (newBytes.isEmpty() && f.status != QLatin1String("deleted")) {
        if (head.isEmpty()) {
            if (!dir.isEmpty()) {
                QFile file(QDir(dir).filePath(path));
                if (file.open(QIODevice::ReadOnly))
                    newBytes = file.readAll();
            }
        } else {
            bool loading = false;
            newBytes = diffImageBlobForPreview(dir, head, path, &loading);
            if (loading)
                newNote = loadingNote;
        }
        if (newBytes.size() > kDiffImageMaxSourceBytes) {
            newNote = diffImageTooLargeNote(newBytes.size());
            newBytes.clear();
        }
    }
    // With neither side readable there is nothing a table would add over the
    // "Binary file changed" row the renderer already emits.
    if (oldBytes.isEmpty() && newBytes.isEmpty() && oldNote != loadingNote &&
        newNote != loadingNote)
        return QString();

    return QStringLiteral("<table class='imagetable' width='100%' cellspacing='0' "
                          "cellpadding='0'><tr>%1%2</tr></table>")
        .arg(diffImageCellHtml(QStringLiteral("Before"), path, mime, oldBytes,
                               oldNote),
             diffImageCellHtml(QStringLiteral("After"), path, mime, newBytes,
                               newNote));
}

void prefetchDiffImageBlobs(const QString &patch, const QString &dir,
                            const QString &base, const QString &head)
{
    if (dir.isEmpty() || patch.isEmpty())
        return;
    struct ImageFile {
        QString path;
        QString status = QStringLiteral("modified");
        QString payload;
        bool image = false;
    };
    QList<ImageFile> files;
    bool collecting = false;
    const QStringList lines = patch.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("diff --git "))) {
            ImageFile file;
            const int bpos = line.indexOf(QLatin1String(" b/"));
            file.path = bpos >= 0 ? line.mid(bpos + 3) : line;
            file.image = !diffImageMimeForPath(file.path).isEmpty();
            files.append(file);
            collecting = false;
            continue;
        }
        if (files.isEmpty() || !files.last().image)
            continue;
        ImageFile &file = files.last();
        if (line.startsWith(QLatin1String("new file")))
            file.status = QStringLiteral("added");
        else if (line.startsWith(QLatin1String("deleted file")))
            file.status = QStringLiteral("deleted");
        if (line.startsWith(QLatin1String("GIT binary patch"))) {
            collecting = true;
            continue;
        }
        if (collecting) {
            file.payload += line;
            file.payload += QLatin1Char('\n');
        }
    }
    constexpr int kMaxPrefetchedImages = 24;
    int fetched = 0;
    for (const ImageFile &file : std::as_const(files)) {
        if (!file.image)
            continue;
        if (fetched >= kMaxPrefetchedImages)
            break;
        ++fetched;
        if (file.status != QLatin1String("added") &&
            diffBinaryPatchImage(file.payload, true).isEmpty())
            diffImageBlob(dir, base.isEmpty() ? QStringLiteral("HEAD") : base,
                          file.path);
        if (file.status != QLatin1String("deleted") && !head.isEmpty() &&
            diffBinaryPatchImage(file.payload, false).isEmpty())
            diffImageBlob(dir, head, file.path);
    }
}

QString lineNoteKey(const QString &path, const QString &side,
                    const QString &num)
{
    return path + QLatin1Char('\x1f') + side + QLatin1Char(':') + num;
}

bool lineNoteHasThread(const QString &note)
{
    return note.contains(QLatin1String("class='reviewthread'"));
}

QString visibleLineNote(QString note)
{
    // A collapsed review thread stays in the map so the gutter can paint its
    // user marker. Only the bounded thread fragment is hidden; an inline
    // composer appended after the separator must remain in the document.
    if (note.startsWith(QLatin1Char('\x1c'))) {
        const int end = note.indexOf(QLatin1Char('\x1d'), 1);
        note = end < 0 ? QString() : note.mid(end + 1);
    }
    return note;
}

QString gutterCellHtml(const QString &cls, const QString &num, const QString &side,
                       bool anchors, const QString &path,
                       const QString &lineNote = QString())
{
    if (num.isEmpty())
        return QStringLiteral("<td class='ln %1' width='64'></td>").arg(cls);
    if (!anchors)
        return QStringLiteral("<td class='ln %1' width='64'>%2</td>").arg(cls, num);
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(path));
    const QString marker = lineNoteHasThread(lineNote)
                               ? QStringLiteral(
                                     " <a class='linecommenticon' "
                                     "href='threadtoggle:%1?s=%2&l=%3' "
                                     "title='Show or hide inline comments'>"
                                     "&#128100;</a>")
                                     .arg(enc, side, num)
                               : QString();
    return QStringLiteral("<td class='ln lnlink %1' width='64'>"
                          "<a href='cmt:%2?s=%3&l=%4' title='Comment on this line'>"
                          "%4</a>%5</td>")
        .arg(cls, enc, side, num, marker);
}

QString lineNoteRows(const QHash<QString, QString> &lineNotes, const QString &path,
                     const QString &oldNum, const QString &newNum, int columns)
{
    QString out;
    const auto addNote = [&](const QString &key) {
        const QString note = visibleLineNote(lineNotes.value(key));
        if (!note.isEmpty())
            out += QStringLiteral("<tr><td class='notecell' colspan='%1'>%2</td></tr>")
                       .arg(columns)
                       .arg(note);
    };
    const QString prefix = path + QLatin1Char('\x1f');
    if (!oldNum.isEmpty())
        addNote(prefix + QStringLiteral("old:") + oldNum);
    if (!newNum.isEmpty())
        addNote(prefix + QStringLiteral("new:") + newNum);
    return out;
}

// Review threads that no longer have a line to sit on (their commented code
// left the diff after a re-push) are keyed to the file itself so they stay in
// view — at the top of the file's table, marked outdated — instead of being
// dropped or painted on code they never talked about.
QString fileLevelNoteKey(const QString &path)
{
    return lineNoteKey(path, QStringLiteral("file"), QStringLiteral("0"));
}

QString fileLevelNoteRow(const QHash<QString, QString> &lineNotes,
                         const QString &path, int columns)
{
    const QString note = visibleLineNote(lineNotes.value(fileLevelNoteKey(path)));
    if (note.isEmpty())
        return QString();
    return QStringLiteral("<tr><td class='notecell' colspan='%1'>%2</td></tr>")
        .arg(columns)
        .arg(note);
}

QString diffBinaryRowHtml(const DiffFileEntry &f, int columns)
{
    QString verb = QStringLiteral("changed");
    if (f.status == QLatin1String("added"))
        verb = QStringLiteral("added");
    else if (f.status == QLatin1String("deleted"))
        verb = QStringLiteral("removed");
    else if (f.status == QLatin1String("renamed"))
        verb = QStringLiteral("renamed");
    return QString::fromUtf8("<tr><td class='code ctx' colspan='%1'>"
                             "<i>Binary file %2 \xE2\x80\x94 content not shown</i>"
                             "</td></tr>")
        .arg(columns)
        .arg(verb);
}

static void diffFileStatusLook(const DiffFileEntry &f, QString *icon, QColor *tint,
                               QString *word)
{
    *icon = QStringLiteral("file-diff");
    *tint = QColor(QStringLiteral("#d29922"));
    *word = QStringLiteral("Modified");
    if (f.status == QLatin1String("added")) {
        *icon = QStringLiteral("diff");
        *tint = QColor(QStringLiteral("#3fb950"));
        *word = QStringLiteral("Added");
    } else if (f.status == QLatin1String("deleted")) {
        *icon = QStringLiteral("trash");
        *tint = QColor(QStringLiteral("#f85149"));
        *word = QStringLiteral("Removed");
    } else if (f.status == QLatin1String("renamed")) {
        *tint = QColor(QStringLiteral("#58a6ff"));
        *word = QStringLiteral("Renamed");
    }
}

static bool appPaletteIsDark()
{
    Q_ASSERT(qApp);
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    return qApp->palette().color(QPalette::Base).lightness() < 128;
}

DiffRenderAppearance currentDiffRenderAppearance()
{
    DiffRenderAppearance appearance;
    appearance.dark = appPaletteIsDark();
    appearance.dpr = iconDevicePixelRatio();
    appearance.colorRemap = Theme::activePalette();
    appearance.treeIconDir = treeIconsDir();
    return appearance;
}

static QColor diffRemappedColor(const QColor &source,
                                const DiffRenderAppearance &appearance)
{
    const QString replacement = appearance.colorRemap.value(
        source.name(QColor::HexRgb).toLower());
    if (replacement.isEmpty())
        return source;
    QColor mapped(replacement);
    mapped.setAlpha(source.alpha());
    return mapped;
}

static QImage diffMarkupCanvas(int size, qreal dpr)
{
    QImage image(qMax(1, qRound(size * dpr)), qMax(1, qRound(size * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

static QString diffImageMarkup(const QImage &image, int size)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QStringLiteral(
               "<img src='data:image/png;base64,%1' width='%2' height='%2'>")
        .arg(QString::fromLatin1(png.toBase64()))
        .arg(size);
}

static QString diffOcticonMarkup(const QString &name, int size,
                                 const QColor &color,
                                 const DiffRenderAppearance &appearance)
{
    const QColor themedColor = diffRemappedColor(color, appearance);
    const QString key = name + QLatin1Char('|') +
                        QString::number(themedColor.rgba(), 16) +
                        QLatin1Char('|') + QString::number(size) +
                        QLatin1Char('|') + QString::number(appearance.dpr);
    static QMutex mutex;
    static QHash<QString, QString> cache;
    QMutexLocker lock(&mutex);
    if (const auto cached = cache.constFind(key); cached != cache.constEnd())
        return *cached;

    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    QImage image = diffMarkupCanvas(size, appearance.dpr);
    QSvgRenderer renderer(path);
    if (renderer.isValid()) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, size, size));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRect(0, 0, size, size), themedColor);
    }
    return *cache.insert(key, diffImageMarkup(image, size));
}

static QString diffFileTypeIconMarkup(
    const QString &fileName, int size,
    const DiffRenderAppearance &appearance)
{
    const QString name = fileTypeIconName(fileName.toLower());
    const QString key = name + QLatin1Char('|') + QString::number(size) +
                        QLatin1Char('|') + QString::number(appearance.dpr) +
                        QLatin1Char('|') + appearance.treeIconDir;
    static QMutex mutex;
    static QHash<QString, QString> cache;
    QMutexLocker lock(&mutex);
    if (const auto cached = cache.constFind(key); cached != cache.constEnd())
        return *cached;

    QString path = appearance.treeIconDir.isEmpty()
                       ? QString()
                       : appearance.treeIconDir + QLatin1Char('/') + name +
                             QStringLiteral(".svg");
    if (!path.isEmpty() && !QFileInfo::exists(path))
        path = appearance.treeIconDir + QStringLiteral("/default_file.svg");
    QSvgRenderer renderer(path);
    if (path.isEmpty() || !renderer.isValid())
        return *cache.insert(key, QString());

    QImage image = diffMarkupCanvas(size, appearance.dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(0, 0, size, size));
    painter.end();
    return *cache.insert(key, diffImageMarkup(image, size));
}

static QString diffFileLabelHtmlForTheme(
    const DiffFileEntry &f, const DiffRenderAppearance &appearance)
{
    QString icon, word;
    QColor tint;
    diffFileStatusLook(f, &icon, &tint, &word);
    const QString muted =
        Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#8b949e"));
    const QString fg = Theme::nextOrHex(
        Theme::Role::Fg, appearance.dark ? QStringLiteral("#e6edf3")
                                         : QStringLiteral("#1f2328"));
    const bool next = Theme::isNext();

    // Same reading order as the Changes tree: what happened to the file, what
    // kind of file it is, its name, then the directory that qualifies the name.
    // The status word is dropped — the tinted glyph already says it, and its
    // hover title spells it out for anyone who does not read the colour.
    const QString name = f.path.section(QLatin1Char('/'), -1);
    QString directory = f.path.left(f.path.size() - name.size());
    if (directory.endsWith(QLatin1Char('/')))
        directory.chop(1);

    QString labelHtml =
        QStringLiteral("<span title='%1'>%2</span>&nbsp;&nbsp;%3&nbsp;&nbsp;"
                       "<span style='font-family:monospace;color:%4;"
                       "font-weight:%6;font-size:%7px'>%5</span>")
            .arg(word, diffOcticonMarkup(icon, 16, tint, appearance),
                 diffFileTypeIconMarkup(name, 14, appearance), fg,
                 name.toHtmlEscaped(),
                 next ? QStringLiteral("600") : QStringLiteral("700"),
                 next ? QStringLiteral("13") : QStringLiteral("14"));
    if (!directory.isEmpty())
        labelHtml += QStringLiteral("&nbsp;&nbsp;<span style='font-family:monospace;"
                                    "font-size:13px;color:%1'>%2</span>")
                         .arg(muted, directory.toHtmlEscaped());

    return QStringLiteral("<span style='font-family:monospace;font-size:13px'>"
                          "%1</span>")
        .arg(labelHtml);
}

QString diffFileLabelHtml(const DiffFileEntry &f, bool /*viewed*/)
{
    return diffFileLabelHtmlForTheme(f, currentDiffRenderAppearance());
}

// The churn columns of a file row: the counts, then the five-block meter, as
// two cells of the controls table so both line up with the button beside them
// and, across rows, with each other.
static QString diffStatCellsHtml(const DiffFileEntry &f)
{
    const QString countsCell =
        QStringLiteral("<td width='96' align='right' valign='middle' "
                       "style='padding:0 10px 0 0'>%1</td>");
    const QString addFg =
        Theme::nextOrHex(Theme::Role::Success, QStringLiteral("#3fb950"));
    const QString delFg =
        Theme::nextOrHex(Theme::Role::Danger, QStringLiteral("#f85149"));
    const QString quietFg =
        Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#8b949e"));
    if (f.binary)
        return countsCell.arg(
            QStringLiteral("<span title='Binary file' style='font-family:monospace;"
                           "color:%1;font-size:12px'>BIN</span>")
                .arg(quietFg));

    const int total = f.adds + f.dels;
    int green = total > 0 ? qRound(5.0 * f.adds / total) : 0;
    if (f.adds > 0 && green == 0)
        green = 1;
    if (f.dels > 0 && green == 5)
        green = 4;
    const int red = total > 0 ? 5 - green : 0;
    QString bar;
    if (green > 0) {
        bar += QStringLiteral("<span style='color:%2'>%1</span>")
                   .arg(QString(green, QChar(0x2588)), addFg);
    }
    if (red > 0) {
        bar += QStringLiteral("<span style='color:%2'>%1</span>")
                   .arg(QString(red, QChar(0x2588)), delFg);
    }
    if (bar.isEmpty()) {
        bar = QStringLiteral("<span style='color:%2'>%1</span>")
                  .arg(QString(5, QChar(0x25a1)), quietFg);
    }

    const QString counts =
        QStringLiteral("<span style='color:%4'>+%1</span>&nbsp;"
                       "<span style='color:%5'>%2%3</span>")
            .arg(f.adds)
            .arg(QString::fromUtf8("\xE2\x88\x92"))
            .arg(f.dels)
            .arg(addFg, delFg);
    const QString tooltip = QString::fromUtf8("+%1 \xE2\x88\x92%2 \xC2\xB7 %3 changed line%4")
                                .arg(f.adds)
                                .arg(f.dels)
                                .arg(total)
                                .arg(total == 1 ? QString() : QStringLiteral("s"));
    return countsCell.arg(
               QStringLiteral("<span title='%1' style='font-family:monospace;"
                              "font-size:12px;font-weight:%3'>%2</span>")
                   .arg(tooltip.toHtmlEscaped(), counts,
                        Theme::isNext() ? QStringLiteral("600")
                                        : QStringLiteral("700"))) +
           QStringLiteral("<td valign='middle' style='padding:0 12px 0 0'>"
                          "<span title='%1' style='font-family:monospace;"
                          "font-size:12px;letter-spacing:-1px'>%2</span></td>")
               .arg(tooltip.toHtmlEscaped(), bar);
}

// A cell, not a styled span: Qt paints borders and padding around table cells
// but silently drops both on inline elements, so the "button" the reviewer is
// meant to click only looks like one from inside a <td> of its own.
static QString diffPillCellHtml(const QString &href, const QString &title,
                                const QString &fg, const QString &background,
                                const QString &stroke, const QString &body)
{
    if (Theme::isNext()) {
        const QString family = Theme::uiFontFamily();
        return QStringLiteral("<td valign='middle' style='background:%1;"
                              "border:1px solid %1;padding:4px 7px'>"
                              "<a href='%2' title='%3' style='color:%4;"
                              "text-decoration:none;%5"
                              "font-size:12px;font-weight:500'>%6</a></td>")
            .arg(Theme::hex(Theme::Role::Surface), href, title, fg,
                 family.isEmpty()
                     ? QString()
                     : QStringLiteral("font-family:\"%1\";").arg(family),
                 body);
    }
    return QStringLiteral("<td valign='middle' style='background:%1;"
                          "border:1px solid %2;padding:8px 12px'>"
                          "<a href='%3' title='%4' style='color:%5;"
                          "text-decoration:none;font-family:monospace;"
                          "font-size:13px;font-weight:700'>%6</a></td>")
        .arg(background, stroke, href, title, fg, body);
}

QString diffExpandHref(const QString &path)
{
    return QStringLiteral("expand:") +
           QString::fromLatin1(QUrl::toPercentEncoding(path));
}

static bool diffEditPathIsSafe(const QString &path)
{
    return !path.isEmpty() && QDir::cleanPath(path) == path &&
           !QDir::isAbsolutePath(path) && path != QLatin1String("..") &&
           !path.startsWith(QLatin1String("../")) &&
           path != QLatin1String(".git") &&
           !path.startsWith(QLatin1String(".git/"));
}

QString diffEditHref(const QString &path, const QString &dir)
{
    return QStringLiteral("edit:") +
           QString::fromLatin1(QUrl::toPercentEncoding(path)) +
           QStringLiteral("?d=") +
           QString::fromLatin1(QUrl::toPercentEncoding(dir));
}

QString diffEditableDir(const QString &dir, const QString &head)
{
    if (dir.isEmpty())
        return QString();
    if (head.isEmpty())
        return dir;
    return headBranchFromFile(dir) == head ? dir : QString();
}

QString diffDeleteHref(const QString &path, const QString &dir)
{
    return QStringLiteral("delete:") +
           QString::fromLatin1(QUrl::toPercentEncoding(path)) +
           QStringLiteral("?d=") +
           QString::fromLatin1(QUrl::toPercentEncoding(dir));
}

static bool diffFileActionTarget(const QUrl &url, const QString &scheme,
                                 QString *dir, QString *path)
{
    if (url.scheme() != scheme)
        return false;
    const QString rel = url.path(QUrl::FullyDecoded);
    const QString root =
        QUrlQuery(url).queryItemValue(QStringLiteral("d"), QUrl::FullyDecoded);
    if (root.isEmpty() || !diffEditPathIsSafe(rel) ||
        !QFileInfo(QDir(root).filePath(rel)).isFile())
        return false;
    *dir = root;
    *path = rel;
    return true;
}

bool diffEditTarget(const QUrl &url, QString *dir, QString *path)
{
    return diffFileActionTarget(url, QStringLiteral("edit"), dir, path);
}

bool diffDeleteTarget(const QUrl &url, QString *dir, QString *path)
{
    return diffFileActionTarget(url, QStringLiteral("delete"), dir, path);
}

QString diffFileMenuHref(const DiffFileEntry &f)
{
    QStringList query;
    const auto add = [&query](const QString &key, const QString &value) {
        query.append(key + QLatin1Char('=') +
                     QString::fromLatin1(QUrl::toPercentEncoding(value)));
    };
    if (!f.editDir.isEmpty()) {
        add(QStringLiteral("d"), f.editDir);
        if (f.deletable)
            query.append(QStringLiteral("x=1"));
    }
    if (!f.repoDir.isEmpty() && f.repoDir != f.editDir)
        add(QStringLiteral("r"), f.repoDir);
    if (!f.repoDir.isEmpty() && !f.branch.isEmpty())
        add(QStringLiteral("b"), f.branch);
    QString href = QStringLiteral("filemenu:") +
                   QString::fromLatin1(QUrl::toPercentEncoding(f.path));
    if (!query.isEmpty())
        href += QLatin1Char('?') + query.join(QLatin1Char('&'));
    return href;
}

bool diffFileMenuTarget(const QUrl &url, DiffFileMenuTarget *target)
{
    if (url.scheme() != QLatin1String("filemenu"))
        return false;
    const QString rel = url.path(QUrl::FullyDecoded);
    if (rel.isEmpty())
        return false;
    const QUrlQuery query(url);
    const QString root =
        query.queryItemValue(QStringLiteral("d"), QUrl::FullyDecoded);
    const QString repo =
        query.queryItemValue(QStringLiteral("r"), QUrl::FullyDecoded);
    target->path = rel;
    target->dir.clear();
    target->deletable = false;
    target->repoDir.clear();
    target->branch.clear();
    if (!diffEditPathIsSafe(rel))
        return true;
    if (!root.isEmpty() && QFileInfo(QDir(root).filePath(rel)).isFile()) {
        target->dir = root;
        target->deletable =
            query.queryItemValue(QStringLiteral("x")) == QLatin1String("1");
    }
    target->repoDir = repo.isEmpty() ? root : repo;
    if (!target->repoDir.isEmpty())
        target->branch =
            query.queryItemValue(QStringLiteral("b"), QUrl::FullyDecoded);
    return true;
}

static void diffStampEditDir(DiffFileEntry &f, const QString &editDir,
                             const QString &dir, const QString &head)
{
    if (!editDir.isEmpty() && !f.binary &&
        f.status != QLatin1String("deleted") && diffEditPathIsSafe(f.path) &&
        QFileInfo(QDir(editDir).filePath(f.path)).isFile()) {
        f.editDir = editDir;
        f.deletable = head.isEmpty();
    }
    if (!dir.isEmpty() && f.status != QLatin1String("deleted") &&
        diffEditPathIsSafe(f.path)) {
        f.repoDir = dir;
        f.branch = head;
    }
}

QString diffMarkdownPreviewHref(const QString &path, const QString &key)
{
    return QStringLiteral("mdpreview:") +
           QString::fromLatin1(QUrl::toPercentEncoding(path)) +
           QStringLiteral("?k=") + key;
}

namespace {

// Past this a preview is a wall nobody reads formatted, and a render keeps the
// patch text it collected for every markdown file it draws.
constexpr qsizetype kDiffMarkdownMaxChars = 2 * 1024 * 1024;

struct DiffMarkdownRegistry {
    QMutex mutex;
    QHash<QString, DiffMarkdownSource> sources;
    qsizetype chars = 0;
};

DiffMarkdownRegistry &diffMarkdownRegistry()
{
    static DiffMarkdownRegistry registry;
    return registry;
}

// Both sides of one markdown file as far as its patch spells them out — what a
// preview falls back to when neither git nor the working tree can hand it the
// whole file, as for a patch-backed pull request.
struct DiffMarkdownCollector {
    bool active = false;
    int hunks = 0;
    QString newText;
    QString oldText;

    void start(const QString &path)
    {
        active = previewSyntaxForPath(path) == PreviewSyntax::Markdown;
        hunks = 0;
        newText.clear();
        oldText.clear();
    }

    void line(const QString &line)
    {
        if (!active)
            return;
        if (line.startsWith(QLatin1String("@@"))) {
            // A rule where the patch skips lines, so two sections never run
            // together into one paragraph.
            if (hunks++ > 0) {
                newText += QStringLiteral("\n* * *\n\n");
                oldText += QStringLiteral("\n* * *\n\n");
            }
            return;
        }
        if (line.startsWith(QLatin1Char('\\'))) // "\ No newline at end of file"
            return;
        const QChar c0 = line.isEmpty() ? QLatin1Char(' ') : line.at(0);
        const QStringView text =
            line.isEmpty() ? QStringView() : QStringView(line).mid(1);
        if (c0 != QLatin1Char('-') && newText.size() < kDiffMarkdownMaxChars) {
            newText += text;
            newText += QLatin1Char('\n');
        }
        if (c0 != QLatin1Char('+') && oldText.size() < kDiffMarkdownMaxChars) {
            oldText += text;
            oldText += QLatin1Char('\n');
        }
    }

    // Stamps the file with the key its Preview pill carries.
    void finish(DiffFileEntry &f, const QString &dir, const QString &base,
                const QString &head)
    {
        if (active && !f.binary && (hunks > 0 || !dir.isEmpty())) {
            DiffMarkdownSource source;
            source.dir = dir;
            source.base = base;
            source.head = head;
            source.path = f.path;
            source.status = f.status;
            source.newText = std::move(newText);
            source.oldText = std::move(oldText);
            f.markdownPreviewKey = registerDiffMarkdownSource(source);
        }
        active = false;
        newText.clear();
        oldText.clear();
    }
};

} // namespace

QString registerDiffMarkdownSource(const DiffMarkdownSource &source)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const QString *field : {&source.dir, &source.base, &source.head,
                                 &source.path, &source.status, &source.newText,
                                 &source.oldText}) {
        hash.addData(field->toUtf8());
        hash.addData(QByteArray(1, '\0'));
    }
    const QString key = QString::fromLatin1(
        hash.result().toHex().left(kDiffFingerprintChars));
    DiffMarkdownRegistry &registry = diffMarkdownRegistry();
    QMutexLocker lock(&registry.mutex);
    if (registry.sources.contains(key))
        return key;
    // Every render registers what it draws again, so clearing a full registry
    // only strands pills drawn before the clear; the dialog tells the reviewer
    // to reload for those.
    constexpr qsizetype kRegistryBudgetChars = 8 * 1024 * 1024;
    const qsizetype cost = source.newText.size() + source.oldText.size() +
                           source.dir.size() + source.path.size() + 256;
    if (registry.chars + cost > kRegistryBudgetChars) {
        registry.sources.clear();
        registry.chars = 0;
    }
    registry.sources.insert(key, source);
    registry.chars += cost;
    return key;
}

bool diffMarkdownSourceForKey(const QString &key, DiffMarkdownSource *out)
{
    DiffMarkdownRegistry &registry = diffMarkdownRegistry();
    QMutexLocker lock(&registry.mutex);
    const auto it = registry.sources.constFind(key);
    if (key.isEmpty() || it == registry.sources.constEnd())
        return false;
    if (out)
        *out = *it;
    return true;
}

DiffMarkdownPreviewSide diffMarkdownPreviewSide(const DiffMarkdownSource &source,
                                                bool after)
{
    DiffMarkdownPreviewSide side;
    const bool added = source.status == QLatin1String("added");
    const bool deleted = source.status == QLatin1String("deleted");
    if (after ? deleted : added)
        return side;
    side.exists = true;
    const QString tooLarge =
        QStringLiteral("This file is too large to preview whole; only its "
                       "first %1 are shown.")
            .arg(QLocale().formattedDataSize(kDiffMarkdownMaxChars));
    const QString &patchText = after ? source.newText : source.oldText;
    if (after ? added : deleted) {
        // A one-sided file's patch carries every line; no read can say more.
        side.text = patchText;
        side.complete = patchText.size() < kDiffMarkdownMaxChars;
        if (!side.complete)
            side.note = tooLarge;
        return side;
    }
    if (!source.dir.isEmpty()) {
        QByteArray bytes;
        bool read = false;
        if (after && source.head.isEmpty()) {
            QFile file(QDir(source.dir).filePath(source.path));
            if (file.open(QIODevice::ReadOnly)) {
                bytes = file.read(kDiffMarkdownMaxChars + 1);
                read = true;
            }
        } else if (after || source.status != QLatin1String("renamed")) {
            // A renamed file's old side lives under a path this entry does not
            // carry, so that one side falls back to the patch.
            const QString ref = after ? source.head
                                      : (source.base.isEmpty() ? QStringLiteral("HEAD")
                                                               : source.base);
            read = runGitCapture(source.dir,
                                 {QStringLiteral("show"),
                                  ref + QLatin1Char(':') + source.path},
                                 &bytes, nullptr);
        }
        if (read) {
            side.complete = bytes.size() <= kDiffMarkdownMaxChars;
            bytes.truncate(kDiffMarkdownMaxChars);
            side.text = QString::fromUtf8(bytes);
            if (!side.complete)
                side.note = tooLarge;
            return side;
        }
    }
    side.text = patchText;
    side.note = QStringLiteral(
        "The whole file is not readable here, so this shows only the sections "
        "the diff touches, with a rule between them.");
    return side;
}

namespace {

struct DiffMarkdownPreviewRead {
    DiffMarkdownPreviewSide after;
    DiffMarkdownPreviewSide before;
};

DiffMarkdownPreviewRead readDiffMarkdownPreview(const DiffMarkdownSource &source,
                                                bool known)
{
    DiffMarkdownPreviewRead read;
    if (known) {
        read.after = diffMarkdownPreviewSide(source, /*after=*/true);
        read.before = diffMarkdownPreviewSide(source, /*after=*/false);
    } else {
        read.after.exists = true;
        read.after.note = QStringLiteral("This preview is no longer available. "
                                         "Reload the diff and try again.");
    }
    return read;
}

QDialog *buildDiffMarkdownPreviewDialog(QWidget *guard, const QString &path,
                                        const DiffMarkdownSource &source,
                                        const DiffMarkdownPreviewSide &after,
                                        const DiffMarkdownPreviewSide &before);

} // namespace

QDialog *showDiffMarkdownPreview(QWidget *parent, const QUrl &url)
{
    if (url.scheme() != QLatin1String("mdpreview"))
        return nullptr;
    const QString path = url.path();
    DiffMarkdownSource source;
    const bool known = diffMarkdownSourceForKey(
        QUrlQuery(url).queryItemValue(QStringLiteral("k")), &source);
    const QPointer<QWidget> guard(parent);
    const DiffMarkdownPreviewRead read = readDiffMarkdownPreview(source, known);
    if (parent && !guard)
        return nullptr;
    return buildDiffMarkdownPreviewDialog(guard, path, source, read.after,
                                          read.before);
}

void openDiffMarkdownPreview(QWidget *parent, const QUrl &url)
{
    if (url.scheme() != QLatin1String("mdpreview"))
        return;
    const QString path = url.path();
    DiffMarkdownSource source;
    const bool known = diffMarkdownSourceForKey(
        QUrlQuery(url).queryItemValue(QStringLiteral("k")), &source);
    const QPointer<QWidget> guard(parent);
    QObject *context = parent ? static_cast<QObject *>(parent)
                              : static_cast<QObject *>(QCoreApplication::instance());
    auto *watcher = new QFutureWatcher<DiffMarkdownPreviewRead>(context);
    QObject::connect(
        watcher, &QFutureWatcher<DiffMarkdownPreviewRead>::finished, context,
        [watcher, guard, parent, path, source] {
            const DiffMarkdownPreviewRead read = watcher->result();
            watcher->deleteLater();
            if (parent && !guard)
                return;
            buildDiffMarkdownPreviewDialog(guard, path, source, read.after,
                                           read.before);
        });
    watcher->setFuture(QtConcurrent::run(
        [source, known] { return readDiffMarkdownPreview(source, known); }));
}

namespace {

QDialog *buildDiffMarkdownPreviewDialog(QWidget *guard, const QString &path,
                                        const DiffMarkdownSource &source,
                                        const DiffMarkdownPreviewSide &after,
                                        const DiffMarkdownPreviewSide &before)
{
    auto *dialog = new QDialog(guard ? guard->window() : nullptr, Qt::Window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("diffMarkdownPreview"));
    dialog->setWindowTitle(QString::fromUtf8("Preview \xE2\x80\x94 %1")
                               .arg(path.section(QLatin1Char('/'), -1)));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);
    auto *top = new QHBoxLayout;
    auto *pathLabel = new QLabel(path, dialog);
    pathLabel->setTextFormat(Qt::PlainText);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("font-family:monospace;font-weight:700")));
    top->addWidget(pathLabel, 1);
    // Only an edited file has two sides worth flipping between.
    auto *sides = new QButtonGroup(dialog);
    if (after.exists && before.exists) {
        for (const bool isAfter : {false, true}) {
            auto *button = new QToolButton(dialog);
            button->setText(isAfter ? QStringLiteral("After") : QStringLiteral("Before"));
            button->setObjectName(isAfter ? QStringLiteral("diffMarkdownPreviewAfter")
                                          : QStringLiteral("diffMarkdownPreviewBefore"));
            button->setCheckable(true);
            sides->addButton(button, isAfter ? 1 : 0);
            top->addWidget(button);
        }
    }
    layout->addLayout(top);
    auto *note = new QLabel(dialog);
    note->setObjectName(QStringLiteral("diffMarkdownPreviewNote"));
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    note->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e")));
    note->hide();
    layout->addWidget(note);
    auto *browser = new QTextBrowser(dialog);
    browser->setObjectName(QStringLiteral("diffMarkdownPreviewBrowser"));
    browser->setOpenLinks(false);
    browser->document()->setDocumentMargin(16);
    // Relative images resolve against the file's own directory, as a README's
    // do, whenever the diff came from a repository on this machine.
    if (!source.dir.isEmpty())
        browser->setSearchPaths(
            {QFileInfo(QDir(source.dir).filePath(path)).absolutePath()});
    QObject::connect(browser, &QTextBrowser::anchorClicked, browser,
                     [](const QUrl &link) {
                         const QString scheme = link.scheme().toLower();
                         if (scheme == QLatin1String("http") ||
                             scheme == QLatin1String("https") ||
                             scheme == QLatin1String("mailto"))
                             QDesktopServices::openUrl(link);
                     });
    layout->addWidget(browser, 1);

    const auto render = [browser, note](const DiffMarkdownPreviewSide &side) {
        browser->setMarkdown(MarkdownRender::forPreview(side.text));
        fitReadmeImages(browser, qMax(1, browser->viewport()->width() - 32));
        styleReadmeDocument(browser);
        note->setText(side.note);
        note->setVisible(!side.note.isEmpty());
    };
    const bool showAfter = after.exists;
    if (QAbstractButton *button = sides->button(showAfter ? 1 : 0))
        button->setChecked(true);
    QObject::connect(sides, &QButtonGroup::idClicked, dialog,
                     [render, after, before](int id) {
                         render(id == 1 ? after : before);
                     });
    dialog->resize(880, 760);
    dialog->show();
    layout->activate();
    render(showAfter ? after : before);
    return dialog;
}

} // namespace

// Markdown files only: opens the file formatted instead of as diff rows.
static QString diffMarkdownPreviewPillHtml(const DiffFileEntry &f,
                                           const DiffRenderAppearance &appearance)
{
    if (f.markdownPreviewKey.isEmpty())
        return QString();
    const bool dark = appearance.dark;
    const QString fg = Theme::nextOrHex(
        Theme::Role::FgMuted,
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"));
    return diffPillCellHtml(
               diffMarkdownPreviewHref(f.path, f.markdownPreviewKey),
               QStringLiteral("Preview this markdown file formatted"), fg,
               dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2"),
               dark ? QStringLiteral("#484f58") : QStringLiteral("#8c959f"),
               diffOcticonMarkup(QStringLiteral("eye"), 14, QColor(fg), appearance) +
                   QStringLiteral("&nbsp;&nbsp;Preview")) +
           QStringLiteral("<td width='8'></td>");
}

static QString diffFileMenuPillHtml(const DiffFileEntry &f,
                                    const DiffRenderAppearance &appearance)
{
    const bool dark = appearance.dark;
    const QString fg = Theme::nextOrHex(
        Theme::Role::FgMuted,
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"));
    return diffPillCellHtml(
               diffFileMenuHref(f).toHtmlEscaped(), QStringLiteral("File actions"), fg,
               dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2"),
               dark ? QStringLiteral("#484f58") : QStringLiteral("#8c959f"),
               diffOcticonMarkup(QStringLiteral("kebab-horizontal"), 14,
                                 QColor(fg), appearance)) +
           QStringLiteral("<td width='8'></td>");
}

QString diffFileActionPillsHtml(const DiffFileEntry &f)
{
    return diffFileMenuPillHtml(f, currentDiffRenderAppearance());
}

// Only viewed files carry this: an unviewed file is already open, and the
// Viewed pill beside it is the thing that closes it. Marking a file viewed is
// how a reviewer says "done with this", which is not the same as "never show
// me this again" — the pill folds it away, and this reopens it without taking
// the review credit back.
static QString diffExpandPillHtml(
    const QString &path, bool expanded,
    const DiffRenderAppearance &appearance)
{
    const bool dark = appearance.dark;
    const QString fg = Theme::nextOrHex(
        Theme::Role::FgMuted,
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"));
    return diffPillCellHtml(
        diffExpandHref(path),
        expanded ? QStringLiteral("Collapse this file")
                 : QStringLiteral("Expand this file (it stays marked viewed)"),
        fg, dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2"),
        dark ? QStringLiteral("#484f58") : QStringLiteral("#8c959f"),
        diffOcticonMarkup(expanded ? QStringLiteral("chevron-down")
                                   : QStringLiteral("chevron-right"),
                           14, QColor(fg), appearance));
}

static QString diffViewedPillHtml(const QString &path, bool viewed,
                                  const DiffRenderAppearance &appearance)
{
    const bool dark = appearance.dark;
    const QString fg = viewed ? Theme::nextOrHex(Theme::Role::AccentFg,
                                                 QStringLiteral("#3fb950"))
                              : Theme::nextOrHex(
                                    Theme::Role::FgMuted,
                                    dark ? QStringLiteral("#e6edf3")
                                         : QStringLiteral("#1f2328"));
    const QString pillBg = dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2");
    const QString stroke = viewed ? Theme::nextOrHex(Theme::Role::AccentFg,
                                                     QStringLiteral("#3fb950"))
                                  : (dark ? QStringLiteral("#484f58")
                                          : QStringLiteral("#8c959f"));
    return diffPillCellHtml(
        QStringLiteral("viewed:") +
            QString::fromLatin1(QUrl::toPercentEncoding(path)),
        viewed ? QStringLiteral("Reopen this file")
               : QStringLiteral("Collapse this file and mark it viewed"),
        fg, pillBg, stroke,
        // The monospace face the rows are set in draws the ballot glyphs from a
        // fallback at half the size it is asked for; the UI face draws them.
        QStringLiteral("<span style='font-family:sans-serif;font-size:%2px'>%1"
                       "</span>&nbsp;&nbsp;Viewed")
            .arg(viewed ? QString::fromUtf8("\xE2\x98\x91")
                        : QString::fromUtf8("\xE2\x98\x90"),
                 Theme::isNext() ? QStringLiteral("14") : QStringLiteral("16")));
}

static QString diffFileCommentPillHtml(
    const QString &path, const DiffRenderAppearance &appearance)
{
    const bool dark = appearance.dark;
    return diffPillCellHtml(
        QStringLiteral("filecomment:") +
            QString::fromLatin1(QUrl::toPercentEncoding(path)),
        QStringLiteral("Comment on this file"),
        dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"),
        dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2"),
        dark ? QStringLiteral("#484f58") : QStringLiteral("#8c959f"),
        QString::fromUtf8("\xF0\x9F\x92\xAC"));
}

static QString diffRowControlsHtmlForTheme(
    const QString &path, double progress, bool viewed, bool comments,
    const QString &leadCells, bool expanded,
    const DiffRenderAppearance &appearance)
{
    Q_UNUSED(progress);
    // One row of cells so the churn columns, the comment pill and the viewed
    // button share a baseline and keep their edges square with each other.
    return QStringLiteral("<table cellspacing='0' cellpadding='0'><tr>%1%2%3%4"
                          "</tr></table>")
        .arg(leadCells,
             comments ? diffFileCommentPillHtml(path, appearance) +
                            QStringLiteral("<td width='8'></td>")
                      : QString(),
             viewed ? diffExpandPillHtml(path, expanded, appearance) +
                          QStringLiteral("<td width='8'></td>")
                    : QString(),
             diffViewedPillHtml(path, viewed, appearance));
}

QString diffRowControlsHtml(const QString &path, double progress, bool viewed,
                            bool comments, const QString &leadCells,
                            bool expanded)
{
    return diffRowControlsHtmlForTheme(
        path, progress, viewed, comments, leadCells, expanded,
        currentDiffRenderAppearance());
}

namespace {

bool diffHealthCodePath(const QString &path)
{
    static const QSet<QString> extensions{
        QStringLiteral("c"),     QStringLiteral("cc"),    QStringLiteral("cpp"),
        QStringLiteral("cxx"),   QStringLiteral("h"),     QStringLiteral("hpp"),
        QStringLiteral("cs"),    QStringLiteral("java"),  QStringLiteral("kt"),
        QStringLiteral("go"),    QStringLiteral("rs"),    QStringLiteral("py"),
        QStringLiteral("rb"),    QStringLiteral("js"),    QStringLiteral("jsx"),
        QStringLiteral("ts"),    QStringLiteral("tsx"),   QStringLiteral("mjs"),
        QStringLiteral("cjs"),   QStringLiteral("php"),   QStringLiteral("swift"),
        QStringLiteral("scala"), QStringLiteral("sh"),    QStringLiteral("bash"),
        QStringLiteral("sql"),   QStringLiteral("dart"),  QStringLiteral("ex"),
        QStringLiteral("exs"),   QStringLiteral("vue"),   QStringLiteral("svelte"),
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

bool diffHealthTestPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.startsWith(QLatin1String("test/")) ||
        lower.startsWith(QLatin1String("tests/")) ||
        lower.contains(QLatin1String("/test/")) ||
        lower.contains(QLatin1String("/tests/")) ||
        lower.contains(QLatin1String("/__tests__/")) ||
        lower.contains(QLatin1String("/spec/")))
        return true;
    const QString base = QFileInfo(lower).fileName();
    return base.startsWith(QLatin1String("test_")) ||
           base.contains(QLatin1String("_test.")) ||
           base.contains(QLatin1String(".test.")) ||
           base.contains(QLatin1String(".spec.")) ||
           base.contains(QLatin1String("_spec."));
}

bool diffHealthGeneratedPath(const QString &path)
{
    const QString lower = QLatin1Char('/') + path.toLower();
    return lower.contains(QLatin1String("/node_modules/")) ||
           lower.contains(QLatin1String("/vendor/")) ||
           lower.contains(QLatin1String("/dist/")) ||
           lower.contains(QLatin1String("/generated/")) ||
           lower.contains(QLatin1String("/build/")) ||
           lower.endsWith(QLatin1String(".min.js")) ||
           lower.endsWith(QLatin1String(".min.css")) ||
           lower.contains(QLatin1String(".generated."));
}

bool diffHealthSensitivePath(const DiffFileEntry &file)
{
    if (file.status != QLatin1String("added"))
        return false;
    const QString lower = file.path.toLower();
    const QString base = QFileInfo(lower).fileName();
    const QString suffix = QFileInfo(lower).suffix();
    return base == QLatin1String(".env") || base == QLatin1String("id_rsa") ||
           base == QLatin1String("id_ed25519") || suffix == QLatin1String("pem") ||
           suffix == QLatin1String("p12") || suffix == QLatin1String("pfx") ||
           suffix == QLatin1String("key");
}

bool diffHealthOddArtifactPath(const DiffFileEntry &file)
{
    if (file.status != QLatin1String("added"))
        return false;
    static const QSet<QString> extensions{
        QStringLiteral("exe"), QStringLiteral("dll"), QStringLiteral("dylib"),
        QStringLiteral("so"),  QStringLiteral("a"),   QStringLiteral("o"),
        QStringLiteral("db"),  QStringLiteral("sqlite"), QStringLiteral("zip"),
        QStringLiteral("tar"), QStringLiteral("gz"),  QStringLiteral("7z"),
    };
    return extensions.contains(QFileInfo(file.path).suffix().toLower());
}

QString diffHealthEvidence(const QStringList &parts, const QString &fallback,
                           const QString &caveat)
{
    QString result = parts.isEmpty() ? fallback : parts.join(QStringLiteral("; "));
    if (!result.endsWith(QLatin1Char('.')))
        result += QLatin1Char('.');
    if (!caveat.isEmpty())
        result += QLatin1Char(' ') + caveat;
    return result;
}

// What diffHealthReport() reads off the file list: churn, which kinds of path
// changed, and whether dependency files moved.
struct DiffHealthFileStats {
    int totalAdds = 0;
    int totalDels = 0;
    int maxFileChurn = 0;
    QString maxChurnPath;
    int sourceFiles = 0;
    int testFiles = 0;
    int testAdds = 0;
    int testDels = 0;
    int binaryFiles = 0;
    int generatedFiles = 0;
    int sensitiveFiles = 0;
    int oddArtifacts = 0;
    bool lockfileChanged = false;
    bool manifestChanged = false;
};

DiffHealthFileStats diffHealthFileStats(const QList<DiffFileEntry> &files)
{
    DiffHealthFileStats stats;

    static const QSet<QString> manifests{
        QStringLiteral("package.json"),     QStringLiteral("pyproject.toml"),
        QStringLiteral("requirements.txt"), QStringLiteral("cargo.toml"),
        QStringLiteral("go.mod"),           QStringLiteral("pom.xml"),
        QStringLiteral("build.gradle"),     QStringLiteral("build.gradle.kts"),
        QStringLiteral("gemfile"),          QStringLiteral("composer.json"),
        QStringLiteral("pubspec.yaml"),     QStringLiteral("mix.exs"),
    };
    static const QSet<QString> lockfiles{
        QStringLiteral("package-lock.json"), QStringLiteral("pnpm-lock.yaml"),
        QStringLiteral("yarn.lock"),         QStringLiteral("cargo.lock"),
        QStringLiteral("go.sum"),            QStringLiteral("gemfile.lock"),
        QStringLiteral("composer.lock"),     QStringLiteral("pubspec.lock"),
        QStringLiteral("pipfile.lock"),      QStringLiteral("poetry.lock"),
    };

    for (const DiffFileEntry &file : files) {
        stats.totalAdds += file.adds;
        stats.totalDels += file.dels;
        const int churn = file.adds + file.dels;
        if (churn > stats.maxFileChurn) {
            stats.maxFileChurn = churn;
            stats.maxChurnPath = file.path;
        }
        const bool test = diffHealthTestPath(file.path);
        if (diffHealthCodePath(file.path) && !test)
            ++stats.sourceFiles;
        if (test) {
            ++stats.testFiles;
            stats.testAdds += file.adds;
            stats.testDels += file.dels;
        }
        if (file.binary)
            ++stats.binaryFiles;
        if (diffHealthGeneratedPath(file.path))
            ++stats.generatedFiles;
        if (diffHealthSensitivePath(file))
            ++stats.sensitiveFiles;
        if (diffHealthOddArtifactPath(file))
            ++stats.oddArtifacts;
        const QString base = QFileInfo(file.path).fileName().toLower();
        stats.lockfileChanged = stats.lockfileChanged || lockfiles.contains(base);
        stats.manifestChanged = stats.manifestChanged || manifests.contains(base);
    }
    return stats;
}

// What diffHealthReport() counts in the added lines of the patch.
struct DiffHealthLineStats {
    int controlFlowLines = 0;
    int nestedControlFlowLines = 0;
    int longLines = 0;
    int todoLines = 0;
    int debugLines = 0;
    int broadCatchLines = 0;
    int unsafeApiLines = 0;
    int transportBypassLines = 0;
    int permissiveSecurityLines = 0;
    int requestLines = 0;
    int hardcodedUrls = 0;
    int insecureUrls = 0;
    int conflictMarkerLines = 0;
    int trailingWhitespaceLines = 0;
    bool resilienceEvidence = false;
};

DiffHealthLineStats diffHealthLineStats(const QString &patch)
{
    DiffHealthLineStats stats;
    QString currentPath;
    bool inHunk = false;

    static const QRegularExpression controlFlow(
        QStringLiteral(R"(\b(?:if|elif|else\s+if|for|foreach|while|switch|case|catch|except|match|when)\b|&&|\|\|)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression todoMarker(
        QStringLiteral(R"(\b(?:TODO|FIXME|HACK|XXX)\b)"));
    static const QRegularExpression debugCall(
        QStringLiteral(R"(\b(?:console\.(?:log|debug|trace)|debugger\s*;|qDebug\s*\(|System\.out\.print|var_dump\s*\(|dd\s*\())"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression broadCatch(
        QStringLiteral(R"((?:except\s*(?::|Exception\b)|catch\s*\([^)]*(?:Exception|Throwable|\.\.\.)[^)]*\)))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unsafeApi(
        QStringLiteral(R"(\b(?:eval|Function)\s*\(|\bexec\s*\(|\bsystem\s*\(|child_process\s*\.\s*exec|shell\s*=\s*True|dangerouslySetInnerHTML|\.innerHTML\s*=)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression transportBypass(
        QStringLiteral(R"(verify\s*=\s*(?:false|0)|rejectUnauthorized\s*:\s*false|NODE_TLS_REJECT_UNAUTHORIZED\s*=\s*['"]?0|CURLOPT_SSL_VERIFYPEER\s*,\s*(?:0|false)|danger_accept_invalid_certs\s*\(\s*true)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression permissiveSecurity(
        QStringLiteral(R"(Access-Control-Allow-Origin[^\n]*['"]\*['"]|\bcors\s*\(\s*\)|\bchmod\s+(?:-R\s+)?777\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression requestSite(
        QStringLiteral(R"(\bfetch\s*\(|\baxios\s*(?:\.|\[)|\brequests\s*\.\s*(?:get|post|put|patch|delete|request)\s*\(|\b(?:http|https)\s*\.\s*(?:get|request)\s*\(|\bQNetwork(?:Request|AccessManager)\b|\bURLSession\b|\bWebSocket\s*\(|\bcurl_easy_perform\b|\bHttpClient\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression resilience(
        QStringLiteral(R"(\b(?:timeout|AbortController|CancellationToken|retry|retries|backoff|cancell?ation)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression url(
        QStringLiteral(R"(https?://[^\s'"<>]+)"),
        QRegularExpression::CaseInsensitiveOption);

    const QStringList patchLines = patch.split(QLatin1Char('\n'));
    for (const QString &line : patchLines) {
        if (line.startsWith(QLatin1String("diff --git "))) {
            inHunk = false;
            currentPath.clear();
            continue;
        }
        if (!inHunk && line.startsWith(QLatin1String("+++ b/"))) {
            currentPath = line.mid(6).trimmed();
            continue;
        }
        if (line.startsWith(QLatin1String("@@ "))) {
            inHunk = true;
            continue;
        }
        if (!inHunk || !line.startsWith(QLatin1Char('+')) ||
            line.startsWith(QLatin1String("+++")))
            continue;

        const QString added = line.mid(1);
        const QString trimmed = added.trimmed();
        if (diffHealthCodePath(currentPath)) {
            const QRegularExpressionMatch control = controlFlow.match(added);
            if (control.hasMatch()) {
                ++stats.controlFlowLines;
                int indent = 0;
                while (indent < added.size() && added.at(indent).isSpace())
                    ++indent;
                if (indent >= 12)
                    ++stats.nestedControlFlowLines;
            }
            if (added.size() > 140)
                ++stats.longLines;
            if (!added.isEmpty() &&
                (added.endsWith(QLatin1Char(' ')) ||
                 added.endsWith(QLatin1Char('\t'))))
                ++stats.trailingWhitespaceLines;
        }
        if (todoMarker.match(added).hasMatch())
            ++stats.todoLines;
        if (debugCall.match(added).hasMatch())
            ++stats.debugLines;
        if (broadCatch.match(added).hasMatch())
            ++stats.broadCatchLines;
        if (unsafeApi.match(added).hasMatch())
            ++stats.unsafeApiLines;
        if (transportBypass.match(added).hasMatch())
            ++stats.transportBypassLines;
        if (permissiveSecurity.match(added).hasMatch())
            ++stats.permissiveSecurityLines;
        if (requestSite.match(added).hasMatch())
            ++stats.requestLines;
        stats.resilienceEvidence =
            stats.resilienceEvidence || resilience.match(added).hasMatch();
        if (trimmed.startsWith(QLatin1String("<<<<<<<")) ||
            trimmed.startsWith(QLatin1String(">>>>>>>")) ||
            trimmed == QLatin1String("======="))
            ++stats.conflictMarkerLines;

        auto urls = url.globalMatch(added);
        while (urls.hasNext()) {
            const QString found = urls.next().captured(0).toLower();
            ++stats.hardcodedUrls;
            if (!found.startsWith(QLatin1String("http://")))
                continue;
            if (found.startsWith(QLatin1String("http://localhost")) ||
                found.startsWith(QLatin1String("http://127.0.0.1")) ||
                found.startsWith(QLatin1String("http://0.0.0.0")) ||
                found.startsWith(QLatin1String("http://[::1]")) ||
                found.contains(QLatin1String("example.")) ||
                found.contains(QLatin1String(".invalid")))
                continue;
            ++stats.insecureUrls;
        }
    }
    return stats;
}

// One graded signal before it becomes a DiffHealthMetric: the 0..100 score
// and the evidence phrases behind it.
struct DiffHealthScore {
    int score = 100;
    QStringList bits;
};

int diffHealthBounded(int score)
{
    return qBound(0, score, 100);
}

DiffHealthScore diffHealthComplexity(const DiffHealthFileStats &fileStats,
                                     const DiffHealthLineStats &lineStats,
                                     int fileCount)
{
    const int churn = fileStats.totalAdds + fileStats.totalDels;
    const int maxFileChurn = fileStats.maxFileChurn;
    const int controlFlowLines = lineStats.controlFlowLines;
    const int nestedControlFlowLines = lineStats.nestedControlFlowLines;
    int complexityScore = 100;
    if (churn > 80)
        complexityScore -= 4;
    if (churn > 250)
        complexityScore -= 8;
    if (churn > 600)
        complexityScore -= 14;
    if (churn > 1500)
        complexityScore -= 20;
    if (maxFileChurn > 250)
        complexityScore -= 8;
    if (maxFileChurn > 750)
        complexityScore -= 10;
    if (controlFlowLines > 20)
        complexityScore -= 6;
    if (controlFlowLines > 50)
        complexityScore -= 10;
    if (controlFlowLines > 120)
        complexityScore -= 15;
    if (nestedControlFlowLines > 8)
        complexityScore -= 8;
    if (nestedControlFlowLines > 30)
        complexityScore -= 12;
    complexityScore = diffHealthBounded(complexityScore);
    QStringList complexityBits{
        QStringLiteral("%1 changed lines across %2 files").arg(churn).arg(fileCount)};
    if (controlFlowLines > 0)
        complexityBits << QStringLiteral("%1 added control-flow lines")
                              .arg(controlFlowLines);
    if (nestedControlFlowLines > 0)
        complexityBits << QStringLiteral("%1 deeply indented branches")
                              .arg(nestedControlFlowLines);
    if (maxFileChurn > 0 && !fileStats.maxChurnPath.isEmpty())
        complexityBits << QStringLiteral("largest file is %1 lines in %2")
                              .arg(maxFileChurn)
                              .arg(fileStats.maxChurnPath);
    return {complexityScore, complexityBits};
}

DiffHealthScore diffHealthQuality(const DiffHealthFileStats &fileStats,
                                  const DiffHealthLineStats &lineStats)
{
    int qualityScore = 100;
    QStringList qualityBits;
    if (fileStats.sourceFiles > 0 && fileStats.testFiles == 0) {
        qualityScore -= 18;
        qualityBits << QStringLiteral("%1 source files changed with no test file in the diff")
                           .arg(fileStats.sourceFiles);
        if (fileStats.totalAdds > 250)
            qualityScore -= 10;
    } else if (fileStats.testFiles > 0) {
        qualityBits << QStringLiteral("%1 test files changed").arg(fileStats.testFiles);
    } else {
        qualityBits << QStringLiteral("no source-code files changed");
    }
    if (fileStats.testFiles > 0 && fileStats.testAdds == 0 && fileStats.testDels > 0) {
        qualityScore -= 20;
        qualityBits << QStringLiteral("tests were only removed");
    }
    qualityScore -= qMin(25, lineStats.todoLines * 5);
    qualityScore -= qMin(15, lineStats.debugLines * 5);
    qualityScore -= qMin(15, lineStats.broadCatchLines * 5);
    qualityScore -= qMin(15, lineStats.longLines * 2);
    if (lineStats.todoLines)
        qualityBits << QStringLiteral("%1 new TODO/FIXME markers").arg(lineStats.todoLines);
    if (lineStats.debugLines)
        qualityBits << QStringLiteral("%1 debug statements").arg(lineStats.debugLines);
    if (lineStats.broadCatchLines)
        qualityBits << QStringLiteral("%1 broad exception handlers")
                           .arg(lineStats.broadCatchLines);
    if (lineStats.longLines)
        qualityBits << QStringLiteral("%1 code lines over 140 characters")
                           .arg(lineStats.longLines);
    return {diffHealthBounded(qualityScore), qualityBits};
}

DiffHealthScore diffHealthSecurity(const QList<RepoSecurityFinding> &secrets,
                                   const DiffHealthFileStats &fileStats,
                                   const DiffHealthLineStats &lineStats)
{
    int securityScore = 100;
    QStringList securityBits;
    if (!secrets.isEmpty()) {
        securityScore -= 70 + qMin(20, (secrets.size() - 1) * 10);
        securityBits << QStringLiteral("%1 high-confidence secret matches")
                            .arg(secrets.size());
    }
    securityScore -= qMin(70, lineStats.transportBypassLines * 35);
    securityScore -= qMin(50, lineStats.unsafeApiLines * 20);
    securityScore -= qMin(30, lineStats.permissiveSecurityLines * 15);
    securityScore -= qMin(70, fileStats.sensitiveFiles * 55);
    if (lineStats.transportBypassLines)
        securityBits << QStringLiteral("%1 TLS/certificate bypasses")
                            .arg(lineStats.transportBypassLines);
    if (lineStats.unsafeApiLines)
        securityBits << QStringLiteral("%1 dynamic execution or unsafe DOM calls")
                            .arg(lineStats.unsafeApiLines);
    if (lineStats.permissiveSecurityLines)
        securityBits << QStringLiteral("%1 permissive security settings")
                            .arg(lineStats.permissiveSecurityLines);
    if (fileStats.sensitiveFiles)
        securityBits << QStringLiteral("%1 newly tracked key or environment files")
                            .arg(fileStats.sensitiveFiles);
    return {diffHealthBounded(securityScore), securityBits};
}

DiffHealthScore diffHealthNetwork(const DiffHealthLineStats &lineStats)
{
    int networkScore = 100;
    QStringList networkBits;
    if (lineStats.requestLines > 0) {
        networkScore -= qMin(20, lineStats.requestLines * 4);
        networkBits << QStringLiteral("%1 added network request sites")
                           .arg(lineStats.requestLines);
        if (!lineStats.resilienceEvidence) {
            networkScore -= 18;
            networkBits << QStringLiteral("no added timeout, cancellation, retry, or backoff evidence");
        } else {
            networkBits << QStringLiteral("timeout/retry/cancellation evidence present");
        }
    }
    if (lineStats.hardcodedUrls) {
        networkScore -= qMin(10, lineStats.hardcodedUrls * 2);
        networkBits << QStringLiteral("%1 hard-coded URLs").arg(lineStats.hardcodedUrls);
    }
    if (lineStats.insecureUrls) {
        networkScore -= qMin(80, lineStats.insecureUrls * 40);
        networkBits << QStringLiteral("%1 non-local plain-HTTP endpoints")
                           .arg(lineStats.insecureUrls);
    }
    return {diffHealthBounded(networkScore), networkBits};
}

DiffHealthScore diffHealthPractices(const DiffHealthFileStats &fileStats,
                                    const DiffHealthLineStats &lineStats)
{
    const int churn = fileStats.totalAdds + fileStats.totalDels;
    const bool lockfileWithoutManifest =
        fileStats.lockfileChanged && !fileStats.manifestChanged;
    int practicesScore = 100;
    QStringList practicesBits;
    if (lineStats.conflictMarkerLines > 0) {
        practicesScore = 10;
        practicesBits << QStringLiteral("%1 unresolved conflict-marker lines")
                             .arg(lineStats.conflictMarkerLines);
    }
    practicesScore -= qMin(30, fileStats.generatedFiles * 12);
    practicesScore -= qMin(20, fileStats.binaryFiles * 8);
    practicesScore -= qMin(50, fileStats.oddArtifacts * 25);
    practicesScore -= qMin(15, lineStats.trailingWhitespaceLines * 2);
    practicesScore -= qMin(15, (lineStats.todoLines + lineStats.debugLines) * 3);
    if (lockfileWithoutManifest)
        practicesScore -= 8;
    if (churn > 1000)
        practicesScore -= 10;
    if (fileStats.generatedFiles)
        practicesBits << QStringLiteral("%1 generated/vendor paths changed")
                             .arg(fileStats.generatedFiles);
    if (fileStats.binaryFiles)
        practicesBits << QStringLiteral("%1 binary files changed").arg(fileStats.binaryFiles);
    if (fileStats.oddArtifacts)
        practicesBits << QStringLiteral("%1 unusual compiled/archive artifacts added")
                             .arg(fileStats.oddArtifacts);
    if (lineStats.trailingWhitespaceLines)
        practicesBits << QStringLiteral("%1 added code lines have trailing whitespace")
                             .arg(lineStats.trailingWhitespaceLines);
    if (lockfileWithoutManifest)
        practicesBits << QStringLiteral("a lockfile changed without its manifest");
    return {diffHealthBounded(practicesScore), practicesBits};
}

// The Merge score weights the five static scores, then caps it by what Git
// said about divergence and conflicts.
DiffHealthScore diffHealthMerge(const DiffHealthScore &complexity,
                                const DiffHealthScore &quality,
                                const DiffHealthScore &security,
                                const DiffHealthScore &network,
                                const DiffHealthScore &practices, int fileCount,
                                int behind, int ahead, bool mergeConflict,
                                bool worktreeConflict, bool conflictCheckPending)
{
    const int weighted = (complexity.score * 25 + quality.score * 20 +
                          security.score * 25 + network.score * 10 +
                          practices.score * 20 + 50) /
                         100;
    int mergeScore = weighted;
    QStringList mergeBits;
    if (mergeConflict) {
        mergeScore = qMin(mergeScore, 15);
        mergeBits << QStringLiteral("Git predicts content conflicts with the base");
    } else if (worktreeConflict) {
        mergeScore = qMin(mergeScore, 35);
        mergeBits << QStringLiteral("the branch worktree has unresolved local conflicts");
    } else if (behind < 0 || conflictCheckPending) {
        mergeScore = qMin(mergeScore, 84);
        mergeBits << QStringLiteral("Git base/conflict check is still running");
    } else {
        if (behind > 0) {
            mergeScore -= qMin(24, 4 + behind * 2);
            mergeBits << QStringLiteral("%1 commits behind the base").arg(behind);
        } else {
            mergeBits << QStringLiteral("up to date with the base");
        }
        if (ahead >= 0)
            mergeBits << QStringLiteral("%1 commits ahead").arg(ahead);
    }
    if (fileCount > 25)
        mergeScore -= 5;
    if (fileCount > 75)
        mergeScore -= 10;
    // A likely leaked credential, insecure transport, or a checked-in conflict
    // marker is a merge blocker even when its weighted share would be small.
    if (security.score < 60 || network.score < 40 || practices.score < 30)
        mergeScore = qMin(mergeScore, 55);
    mergeScore = diffHealthBounded(mergeScore);
    mergeBits << QStringLiteral("weighted static diff health %1/100").arg(weighted);
    return {mergeScore, mergeBits};
}

} // namespace

DiffHealthReport diffHealthReport(const QString &patch,
                                  const QList<DiffFileEntry> &files,
                                  int behind, int ahead, bool mergeConflict,
                                  bool worktreeConflict,
                                  bool conflictCheckPending)
{
    const DiffHealthFileStats fileStats = diffHealthFileStats(files);
    const DiffHealthLineStats lineStats = diffHealthLineStats(patch);
    const int fileCount = static_cast<int>(files.size());

    const DiffHealthScore complexity =
        diffHealthComplexity(fileStats, lineStats, fileCount);
    const DiffHealthScore quality = diffHealthQuality(fileStats, lineStats);
    const QList<RepoSecurityFinding> secrets =
        RepoSecurity::findSecretsInDiff(patch);
    const DiffHealthScore security =
        diffHealthSecurity(secrets, fileStats, lineStats);
    const DiffHealthScore network = diffHealthNetwork(lineStats);
    const DiffHealthScore practices = diffHealthPractices(fileStats, lineStats);
    const DiffHealthScore merge = diffHealthMerge(
        complexity, quality, security, network, practices, fileCount, behind,
        ahead, mergeConflict, worktreeConflict, conflictCheckPending);

    DiffHealthReport report;
    report.secrets = secrets;
    report.metrics = {
        {QStringLiteral("merge"), QStringLiteral("Merge"), merge.score,
         diffHealthEvidence(
             merge.bits, QStringLiteral("No merge signals available"),
             QStringLiteral("Combines Git divergence/conflict state with the five static diff scores."))},
        {QStringLiteral("complexity"), QStringLiteral("Complexity"),
         complexity.score,
         diffHealthEvidence(
             complexity.bits, QStringLiteral("No changed lines"),
             QStringLiteral("Estimated from churn, control flow, and concentration; no code was executed."))},
        {QStringLiteral("quality"), QStringLiteral("Quality"), quality.score,
         diffHealthEvidence(
             quality.bits, QStringLiteral("No quality findings"),
             QStringLiteral("Tests are inferred from paths and were not executed."))},
        {QStringLiteral("security"), QStringLiteral("Security"), security.score,
         diffHealthEvidence(
             security.bits,
             QStringLiteral("No high-confidence secrets or risky added-line patterns found"),
             QStringLiteral("Static heuristic only; it does not replace dependency or SAST scanning."))},
        {QStringLiteral("network"), QStringLiteral("Network"), network.score,
         diffHealthEvidence(
             network.bits,
             QStringLiteral("No added request sites or hard-coded URLs detected"),
             QStringLiteral("This is syntax analysis; ForkMesh sent no traffic for this check."))},
        {QStringLiteral("practices"), QStringLiteral("Practices"), practices.score,
         diffHealthEvidence(
             practices.bits,
             QStringLiteral("No unusual artifacts, conflict markers, or diff-hygiene issues found"),
             QStringLiteral("Checks only the files and added lines in this diff."))},
    };
    return report;
}

// One line of review state for the strip above a diff: how far through the
// files the reviewer is, the churn behind those files, and what kinds of change
// they are. Empty for an empty diff, so the caller can hide the strip on it.
QString diffViewedCountHtml(int seen, int total, bool current)
{
    if (total <= 0)
        return {};
    const QString text = QStringLiteral("%1 of %2 files viewed").arg(seen).arg(total);
    if (seen < total || !current)
        return text;
    return QString::fromUtf8("<span style='color:%2;font-weight:700'>"
                             "\xE2\x9C\x93 %1</span>")
        .arg(text, Theme::nextOrHex(Theme::Role::Success,
                                    QStringLiteral("#3fb950")));
}

QString diffReviewSummaryHtml(const QList<DiffFileEntry> &files,
                              const QSet<QString> &viewed, bool current)
{
    if (files.isEmpty())
        return {};

    int seen = 0, adds = 0, dels = 0;
    int added = 0, removed = 0, renamed = 0, binaries = 0;
    for (const DiffFileEntry &f : files) {
        if (viewed.contains(f.path))
            ++seen;
        adds += f.adds;
        dels += f.dels;
        if (f.binary)
            ++binaries;
        if (f.status == QLatin1String("added"))
            ++added;
        else if (f.status == QLatin1String("deleted"))
            ++removed;
        else if (f.status == QLatin1String("renamed"))
            ++renamed;
    }

    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString fg = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    const QString muted = QStringLiteral("#8b949e");
    const bool done = seen == files.size() && current;
    const auto separated = [&muted](const QString &html) {
        return QString::fromUtf8("<span style='color:%1'>&nbsp;&nbsp;\xC2\xB7"
                                 "&nbsp;&nbsp;</span>%2")
            .arg(muted, html);
    };

    QString html = QString::fromUtf8("<span style='color:%1;font-weight:700'>%2%3 of "
                                     "%4 files viewed</span>")
                       .arg(done ? QStringLiteral("#3fb950") : fg,
                            done ? QString::fromUtf8("\xE2\x9C\x93 ") : QString())
                       .arg(seen)
                       .arg(files.size());
    html += separated(
        QString::fromUtf8("<span style='color:#3fb950;font-weight:700'>+%1</span>"
                          "&nbsp;&nbsp;<span style='color:#f85149;"
                          "font-weight:700'>\xE2\x88\x92%2</span>")
            .arg(adds)
            .arg(dels));

    QStringList kinds;
    const int modified = files.size() - added - removed - renamed;
    if (modified > 0)
        kinds << QStringLiteral("%1 modified").arg(modified);
    if (added > 0)
        kinds << QStringLiteral("%1 added").arg(added);
    if (removed > 0)
        kinds << QStringLiteral("%1 removed").arg(removed);
    if (renamed > 0)
        kinds << QStringLiteral("%1 renamed").arg(renamed);
    if (binaries > 0)
        kinds << QStringLiteral("%1 binary").arg(binaries);
    if (!kinds.isEmpty())
        html += separated(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg(muted, kinds.join(QStringLiteral(", "))));

    const int remaining = files.size() - seen;
    html += separated(
        QStringLiteral("<span style='color:%1'>%2</span>")
            .arg(muted, remaining == 0
                            ? QStringLiteral("every file reviewed")
                            : QStringLiteral("%1 left to review").arg(remaining)));
    return QStringLiteral("<span style='font-size:12px'>%1</span>").arg(html);
}

QString diffFileHeaderHtml(const DiffFileEntry &f, bool viewed, bool anchors,
                           bool expanded,
                           const DiffRenderAppearance &appearance)
{
    // A viewed file the reviewer reopened is drawn like any other open file:
    // only a folded row gets the tight padding and the one-pixel bottom margin.
    const bool collapsed = viewed && !expanded;
    // The leading bare <a name> stays as the file-block split marker, but Qt
    // drops an empty anchor unless it opens a setHtml/insertHtml parse, so
    // scroll targets and file-position spans anchor on a real (zero-width)
    // character inside the header cell instead.
    const int verticalPadding = collapsed ? 3 : 9;
    // The controls cell holds a bordered button that already stands taller than
    // the text beside it, so it pads less than the path cell: matching them
    // would grow every collapsed row by the button's own frame twice over.
    const int controlPadding = collapsed ? 2 : 5;
    // The header table collapses its borders (see diffStyleSheet), so the row
    // outline is drawn by the two cells' outer edges: a table-level border
    // would be repeated around every cell and hang a stray divider between the
    // path and its controls. Qt only reads per-cell borders off a style
    // attribute, never off the document stylesheet, so the colour is inlined
    // here the way the label and pill inline theirs.
    const QString edge = Theme::nextOrHex(
        Theme::Role::Border, appearance.dark ? QStringLiteral("#30363d")
                                             : QStringLiteral("#d0d7de"));
    // The row itself is the click target a reviewer reaches for first, so a
    // folded file's whole path label opens it. Every span inside the label
    // carries its own colour, so wrapping it changes nothing but the cursor.
    QString label = diffFileLabelHtmlForTheme(f, appearance);
    if (viewed)
        label = QStringLiteral("<a href='%1' title='%2' style='color:%3;"
                               "text-decoration:none'>%4</a>")
                    .arg(diffExpandHref(f.path),
                         expanded ? QStringLiteral("Collapse this file")
                                  : QStringLiteral("Expand this file (it stays "
                                                   "marked viewed)"),
                         Theme::nextOrHex(Theme::Role::Fg,
                                          appearance.dark
                                              ? QStringLiteral("#e6edf3")
                                              : QStringLiteral("#1f2328")),
                         label);
    return QString::fromUtf8(
               "<a name=\"%1\"></a><div class='fileblock%4'>"
               "<table class='fileheader' width='100%' cellspacing='0' "
               "cellpadding='0'><tr>"
               "<td class='fpathcell' width='99%' valign='middle' "
               "style='border-top:1px solid %7;border-bottom:1px solid %7;"
               "border-left:1px solid %7;padding:%5px 4px %5px 12px'>"
               "<a name=\"%1\">&#8203;</a>%2</td>"
               "<td class='fctlcell' width='1%' align='right' valign='middle' "
               "style='border-top:1px solid %7;border-bottom:1px solid %7;"
               "border-right:1px solid %7;padding:%6px 12px %6px 4px'>"
               "%3</td>"
               "</tr></table>")
        .arg(f.anchor, label,
             diffRowControlsHtmlForTheme(f.path, 0.0, viewed, anchors,
                                         diffStatCellsHtml(f) +
                                             diffFileMenuPillHtml(f, appearance) +
                                             diffMarkdownPreviewPillHtml(f, appearance),
                                         expanded,
                                         appearance),
             collapsed ? QStringLiteral(" viewed") : QString(),
             QString::number(verticalPadding),
             QString::number(controlPadding), edge);
}

namespace {

// Everything an in-place viewed/expand swap needs about a rendered diff: each
// file's block exactly as it currently sits in the document, and the same file
// drawn open. While a file is open the two are the same QString and cost one
// copy between them; folding it keeps the open one so reopening is a swap too.
//
// Folding is then a header swap plus a body drop, and reopening puts the body
// back. Neither re-parses the patch, and neither disturbs the layout of a
// single other file — which is what made ticking Viewed re-stream the whole
// diff and visibly redraw it.
QHash<QTextEdit *, DiffBlockCache> &diffBlockCaches()
{
    static QHash<QTextEdit *, DiffBlockCache> caches;
    return caches;
}

// The document range one file's rendered block occupies: from the empty block
// its <div> opened, through its last table, stopping short of whatever opens
// the next file (or of the end-of-diff bar). Both edges must land on a plain
// block — a range that starts or ends inside a table would cut a row in half,
// so an unexpected shape declines the swap and leaves the caller its full
// re-render.
bool diffFileBlockRange(QTextEdit *view, const QString &anchor,
                        const QString &nextAnchor, int endCapPosition,
                        int *start, int *end)
{
    QTextDocument *doc = view->document();
    // Anchor names ride a zero-width space inside the header cell, so a file's
    // header table is the root child that opens on one of those positions. A
    // file sliced across a page start carries a second, bare copy of the same
    // anchor; taking the first table that opens on any of them is what makes
    // the range cover every slice of the file.
    QSet<int> anchorPositions;
    QSet<int> nextPositions;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isAnchor())
                continue;
            const QStringList names = frag.charFormat().anchorNames();
            if (names.contains(anchor))
                anchorPositions.insert(frag.position());
            else if (!nextAnchor.isEmpty() && names.contains(nextAnchor))
                nextPositions.insert(frag.position());
        }
    }
    if (anchorPositions.isEmpty())
        return false; // the file is not on a loaded page

    QList<int> firsts;
    QList<bool> frames;
    QList<bool> empties;
    int startIndex = -1;
    int endIndex = -1;
    int index = 0;
    for (QTextFrame::iterator it = doc->rootFrame()->begin(); !it.atEnd();
         ++it, ++index) {
        QTextFrame *frame = it.currentFrame();
        const QTextBlock block = it.currentBlock();
        const int first = frame ? frame->firstPosition() : block.position();
        firsts.append(first);
        frames.append(frame != nullptr);
        empties.append(!frame && block.isValid() && block.length() <= 1);
        if (frame && startIndex < 0 && anchorPositions.contains(first))
            startIndex = index;
        else if (frame && endIndex < 0 && nextPositions.contains(first))
            endIndex = index;
        else if (!frame && endIndex < 0 && startIndex >= 0 &&
                 endCapPosition >= 0 && first == endCapPosition)
            endIndex = index; // no next file here: stop above the end bar
    }
    if (startIndex < 1 || frames.at(startIndex - 1))
        return false;
    int fromIndex = startIndex - 1;
    while (fromIndex > 0 && empties.at(fromIndex) && empties.at(fromIndex - 1))
        --fromIndex;
    const int from = firsts.at(fromIndex);
    int to = doc->characterCount() - 1;
    if (endIndex > 0) {
        if (frames.at(endIndex - 1))
            return false;
        to = firsts.at(endIndex - 1);
    }
    if (to <= from)
        return false;
    *start = from;
    *end = to;
    return true;
}

// Where a block's planned fragments sit in the page plan. A block over the
// bounded apply size was sliced by the planner, so it occupies a run of
// consecutive slots rather than one — and a file bigger than a page runs across
// page edges, on and off the loaded window. Those are exactly the files a
// reviewer folds, so the run is looked up over the whole plan.
struct DiffBlockRun {
    int firstPage = -1;
    int firstSlot = -1;
    int lastPage = -1;
    int lastSlot = -1;
};

// code-health: allow(deep-nested-loop) runs per Viewed/fold toggle; a sequence match over the planned diff pages whose inner compare stops at the first mismatch, so about one string compare per stored fragment
bool diffBlockPageRun(const DiffStreamState &state, const QString &block,
                      DiffBlockRun *run)
{
    const QList<DiffHtmlFragment> planned = fragmentDiffHtml(block);
    if (planned.isEmpty())
        return false;
    for (int p = 0; p < state.pages.size(); ++p) {
        for (int at = 0; at < state.pages.at(p).size(); ++at) {
            int page = p;
            int slot = at;
            bool matched = true;
            for (int i = 0; i < planned.size() && matched; ++i) {
                while (page < state.pages.size() &&
                       slot >= state.pages.at(page).size()) {
                    ++page;
                    slot = 0;
                }
                if (page >= state.pages.size()) {
                    matched = false;
                    break;
                }
                // A slice that opens a page restates the file's bare anchor in
                // front of itself; inside a page it stands on its own.
                const QString &stored = state.pages.at(page).at(slot);
                matched = stored == planned.at(i).html ||
                          (!planned.at(i).pageStartAnchor.isEmpty() &&
                           stored == planned.at(i).pageStartAnchor +
                                         planned.at(i).html);
                if (matched && i + 1 < planned.size())
                    ++slot;
            }
            if (!matched)
                continue;
            *run = {p, at, page, slot};
            return true;
        }
    }
    return false;
}

// Replaces a block's run of slots with its new slices and drops any page the
// run emptied, renumbering the loaded window to match. The replacement lands
// on `targetPage`, which therefore always survives; a body too big for one
// page (a large file being reopened) is dealt onto pages of its own behind it,
// the way the planner would have. Returns that page's index afterwards.
int replaceDiffBlockRun(DiffStreamState &state, const DiffBlockRun &run,
                        int targetPage, const QList<DiffHtmlFragment> &replacement)
{
    const int targetSlot = targetPage == run.firstPage ? run.firstSlot : 0;
    for (int p = run.lastPage; p >= run.firstPage; --p) {
        QStringList &fragments = state.pages[p];
        const int from = p == run.firstPage ? run.firstSlot : 0;
        const int to = p == run.lastPage ? run.lastSlot : int(fragments.size()) - 1;
        if (to >= from)
            fragments.remove(from, to - from + 1);
    }

    QList<QStringList> chunks;
    QStringList chunk;
    qsizetype chunkChars = 0;
    for (const DiffHtmlFragment &fragment : replacement) {
        const qsizetype cost = diffLayoutChars(fragment.html);
        if (!chunk.isEmpty() && chunkChars + cost > kDiffPageChars) {
            chunks.append(std::move(chunk));
            chunk.clear();
            chunkChars = 0;
        }
        // A slice that opens a page restates the file's bare anchor.
        const bool opensPage = chunk.isEmpty() && !chunks.isEmpty();
        chunk.append(opensPage ? fragment.pageStartAnchor + fragment.html
                               : fragment.html);
        chunkChars += cost;
    }
    chunks.append(std::move(chunk));

    // Whatever followed the file on its page follows its last slice still.
    QStringList tail;
    if (chunks.size() > 1) {
        tail = state.pages.at(targetPage).mid(targetSlot);
        state.pages[targetPage].remove(targetSlot, tail.size());
    }
    for (int i = 0; i < chunks.first().size(); ++i)
        state.pages[targetPage].insert(targetSlot + i, chunks.first().at(i));

    const auto renumber = [&state](int from, int by) {
        QHash<int, int> positions;
        for (auto it = state.windowPagePositions.cbegin();
             it != state.windowPagePositions.cend(); ++it) {
            if (by < 0 && it.key() == from)
                continue; // the page that went away
            positions.insert(it.key() > from ? it.key() + by : it.key(),
                             it.value());
        }
        state.windowPagePositions = positions;
        if (state.page > from)
            state.page += by;
        if (state.windowLastPage > from || (by < 0 && state.windowLastPage == from))
            state.windowLastPage += by;
    };
    int target = targetPage;
    for (int p = run.lastPage; p >= run.firstPage; --p) {
        if (p == targetPage || !state.pages.at(p).isEmpty())
            continue;
        state.pages.removeAt(p);
        renumber(p, -1);
        if (target > p)
            --target;
    }
    for (int i = 1; i < chunks.size(); ++i)
        state.pages.insert(target + i, chunks.at(i));
    if (chunks.size() > 1) {
        state.pages[target + chunks.size() - 1].append(tail);
        renumber(target, int(chunks.size()) - 1);
    }
    state.windowLastPage = qBound(state.page, state.windowLastPage,
                                  int(state.pages.size()) - 1);
    return target;
}

// Draws the loaded window again from `page`, landing on `anchor`. This is what
// a fold costs when the document cannot be edited in place: one bounded page
// paint from the plan already in hand, not a parse of the whole patch and a
// walk back from page one.
void redrawDiffWindowAtAnchor(QTextEdit *view, DiffStreamState &state, int page,
                              const QString &anchor)
{
    resumeDiffViewportUpdates(view, state);
    state.pendingAnchor = anchor;
    state.pendingText.clear();
    state.pendingTextDirection = 1;
    state.pendingViewportPage = -1;
    state.pendingViewportY = 0;
    state.pendingScrollPixels = 0;
    state.pendingViewportPosition = 0;
    state.endCapPosition = -1;
    state.endCapExtraHeight = 0;
    state.endCapResizePending = false;
    if (QScrollBar *bar = view->verticalScrollBar())
        bar->setValue(0);
    renderDiffPage(view, page);
}

} // namespace

DiffBlockCache buildDiffBlockCache(const QStringList &blocks,
                                   const QList<DiffFileEntry> &files,
                                   const QSet<QString> &viewedFiles,
                                   const QSet<QString> &expandedFiles, bool comments,
                                   const DiffRenderAppearance &appearance)
{
    DiffBlockCache cache;
    cache.files = files;
    cache.comments = comments;
    int fileIndex = 0;
    for (const QString &block : blocks) {
        // splitDiffFileBlocks hands back any preamble first, then one entry per
        // file in the order the renderer appended them.
        if (!block.startsWith(QLatin1String("<a name=\"file-")))
            continue;
        if (fileIndex >= files.size())
            break;
        const DiffFileEntry &f = files.at(fileIndex++);
        DiffCachedFile cached;
        cached.block = block;
        const bool viewed = viewedFiles.contains(f.path);
        const bool expanded = expandedFiles.contains(f.path);
        if (!viewed || expanded) {
            // An exact header prefix is the only body worth keeping: anything
            // else means the markup moved on and rebuilding from it would
            // corrupt the file. The open block shares its bytes with the
            // current one until a fold replaces the latter.
            const QString header =
                diffFileHeaderHtml(f, viewed, comments, expanded, appearance);
            if (block.size() > header.size() && block.startsWith(header)) {
                cached.openBlock = block;
                cached.openHeaderChars = header.size();
            }
        }
        cache.byAnchor.insert(f.anchor, std::move(cached));
    }
    return cache;
}

void noteDiffFileBlocks(QTextEdit *view, DiffBlockCache cache)
{
    if (!view)
        return;
    if (cache.files.isEmpty()) {
        diffBlockCaches().remove(view);
        return;
    }
    if (!diffBlockCaches().contains(view)) {
        QObject::connect(view, &QObject::destroyed, qApp,
                         [view] { diffBlockCaches().remove(view); });
    }
    diffBlockCaches().insert(view, std::move(cache));
}

void noteDiffFileBlocks(QTextEdit *view, const QString &html,
                        const QList<DiffFileEntry> &files,
                        const QSet<QString> &viewedFiles,
                        const QSet<QString> &expandedFiles, bool comments)
{
    if (!view)
        return;
    if (files.isEmpty()) {
        diffBlockCaches().remove(view);
        return;
    }
    noteDiffFileBlocks(view, buildDiffBlockCache(splitDiffFileBlocks(html), files,
                                                 viewedFiles, expandedFiles, comments,
                                                 currentDiffRenderAppearance()));
}

QList<DiffFileEntry> diffRenderedFiles(QTextEdit *view)
{
    const auto it = diffBlockCaches().constFind(view);
    return it == diffBlockCaches().constEnd() ? QList<DiffFileEntry>() : it->files;
}

DiffFileBlockSwap swapDiffFileViewedState(QTextEdit *view, const QString &anchor,
                                          bool viewed, bool expanded)
{
    DiffFileBlockSwap swap;
    if (!view || anchor.isEmpty())
        return swap;
    const auto cache = diffBlockCaches().find(view);
    if (cache == diffBlockCaches().end())
        return swap;
    const auto stream = diffStreams().find(view);
    // A plan still running owns the pages this would edit. A window that is
    // still landing only rules out editing the document (see below).
    if (stream == diffStreams().end() || stream->planning ||
        stream->pages.isEmpty())
        return swap;
    const auto cached = cache->byAnchor.find(anchor);
    if (cached == cache->byAnchor.end() || cached->block.isEmpty())
        return swap;

    int entryIndex = -1;
    for (int i = 0; i < cache->files.size(); ++i) {
        if (cache->files.at(i).anchor == anchor) {
            entryIndex = i;
            break;
        }
    }
    if (entryIndex < 0)
        return swap;
    const DiffFileEntry entry = cache->files.at(entryIndex);
    const QString oldBlock = cached->block;

    const QString header = diffFileHeaderHtml(
        entry, viewed, cache->comments, expanded,
        currentDiffRenderAppearance());
    QString newBlock;
    if (viewed && !expanded) {
        newBlock = header + QStringLiteral("</div>");
    } else if (cached->openBlock.isEmpty()) {
        // Reopening a file this document only ever drew folded has no body to
        // restore, so that one click still costs a full render — and stocks the
        // cache for every toggle after it.
        return swap;
    } else {
        newBlock = header + cached->openBlock.mid(cached->openHeaderChars);
    }
    if (newBlock == oldBlock)
        return swap;

    // Every slice of the replacement goes in as one bounded apply, the same
    // gate every other route into QTextDocument passes.
    const QList<DiffHtmlFragment> planned = fragmentDiffHtml(newBlock);
    QStringList replacement;
    qsizetype replacementChars = 0;
    for (const DiffHtmlFragment &fragment : planned) {
        if (fragment.html.size() > diffApplyCapFor(fragment.html))
            return swap;
        replacement << fragment.html;
        replacementChars += diffLayoutChars(fragment.html);
    }
    if (replacement.isEmpty())
        return swap;

    // Where the planned pages keep the block, and where the document keeps it.
    DiffBlockRun run;
    if (!diffBlockPageRun(*stream, oldBlock, &run))
        return swap;
    const int gen = stream->gen;
    const int windowFirst = stream->page;
    const int windowLast = stream->windowLastPage;
    const bool resident = run.lastPage >= windowFirst && run.firstPage <= windowLast;
    // The replacement stands where the file's first loaded slice stood, so the
    // document and the plan keep agreeing about what the window holds.
    const int targetPage = resident ? qMax(run.firstPage, windowFirst)
                                    : run.firstPage;
    if (!resident) {
        // Nothing of the file is in the document: the plan is all there is to
        // change, and the loaded rows do not move.
        replaceDiffBlockRun(*stream, run, targetPage, planned);
        cache->byAnchor[anchor].block = newBlock;
        swap.applied = true;
        swap.oldHtml = oldBlock;
        swap.newHtml = newBlock;
        return swap;
    }

    // Editing the document in place is for a settled window and a bounded
    // insert: reopening a file of many slices in one edit block is the layout
    // stall the streamer exists to spread out, and a loaded page the fold
    // empties would leave the window without its second page to roll on.
    bool inPlace = stream->pendingPages.isEmpty() &&
                   replacementChars <= kDiffPageChars;
    for (int p = qMax(run.firstPage, windowFirst);
         inPlace && p <= qMin(run.lastPage, windowLast); ++p) {
        const int from = p == run.firstPage ? run.firstSlot : 0;
        const int to = p == run.lastPage ? run.lastSlot
                                         : int(stream->pages.at(p).size()) - 1;
        if (p != targetPage && to - from + 1 == stream->pages.at(p).size())
            inPlace = false;
    }
    const QString nextAnchor = entryIndex + 1 < cache->files.size()
                                   ? cache->files.at(entryIndex + 1).anchor
                                   : QString();
    int start = -1;
    int end = -1;
    if (inPlace &&
        !diffFileBlockRange(view, anchor, nextAnchor, stream->endCapPosition,
                            &start, &end))
        inPlace = false;
    if (!inPlace) {
        const int page = replaceDiffBlockRun(*stream, run, targetPage, planned);
        cache->byAnchor[anchor].block = newBlock;
        redrawDiffWindowAtAnchor(view, *stream, page, anchor);
        swap.applied = true;
        swap.oldHtml = oldBlock;
        swap.newHtml = newBlock;
        return swap;
    }

    QScrollBar *bar = view->verticalScrollBar();
    const int scrollValue = bar ? bar->value() : 0;
    QTextDocument *doc = view->document();
    auto headerRow = [view, doc](int from, int to) {
        QRectF found;
        for (QTextBlock block = doc->findBlock(from);
             found.isNull() && block.isValid() && block.position() < to;
             block = block.next())
            found = diffFileRowRect(view, block.position());
        return found;
    };
    int headerTop = scrollValue;
    const QRectF row = headerRow(start, end);
    if (!row.isNull()) {
        headerTop = qRound(row.top());
    } else {
        QTextCursor head(doc);
        head.setPosition(start);
        headerTop = view->cursorRect(head).top() + scrollValue;
    }
    const int before = doc->characterCount();
    {
        // One edit block so the layout runs once and the view repaints once.
        QTextCursor cursor(doc);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.beginEditBlock();
        cursor.removeSelectedText();
        // insertHtml leaves the cursor after what it inserted, so a sliced
        // file's fragments rejoin in order exactly as the streamer appends them.
        for (const QString &fragment : std::as_const(replacement))
            cursor.insertHtml(fragment);
        cursor.endEditBlock();
    }
    // Editing the document resizes the scroll bar, and a view can answer that
    // signal on the spot — so both records are looked up again rather than
    // written through iterators taken before the edit.
    const auto editedStream = diffStreams().find(view);
    const auto editedCache = diffBlockCaches().find(view);
    if (editedStream == diffStreams().end() ||
        editedCache == diffBlockCaches().end() || editedStream->gen != gen)
        return swap; // the view was re-rendered under us; it owns the document

    // Everything above the edit kept its geometry, so the scroll value still
    // points at the same content and the rows the reviewer is reading do not
    // move. Only the marks below it have to be walked forward; a page that
    // opened inside the removed file now opens right after what replaced it.
    const int delta = doc->characterCount() - before;
    if (editedStream->endCapPosition > start)
        editedStream->endCapPosition += delta;
    for (auto it = editedStream->windowPagePositions.begin();
         it != editedStream->windowPagePositions.end(); ++it) {
        if (it.value() > start)
            it.value() = qMax(it.value(), end) + delta;
    }
    // A file that opened above the loaded window had its body under the
    // viewport, not over it: the old scroll value now measures into whatever
    // follows, so the fold lands on the header that replaced the body instead.
    const bool openedAboveWindow = run.firstPage < windowFirst;
    int landing = qBound(0, headerTop, scrollValue);
    if (const QRectF swapped = headerRow(start, end + delta);
        !row.isNull() && !swapped.isNull()) {
        landing = headerTop <= scrollValue
                      ? qRound(swapped.top())
                      : scrollValue +
                            qRound(swapped.center().y() - row.center().y());
        landing = qMax(0, landing);
    }
    preserveDiffSwapScrollPosition(view, *editedStream,
                                   openedAboveWindow ? 0 : landing);
    if (openedAboveWindow && bar)
        bar->setValue(bar->minimum());
    replaceDiffBlockRun(*editedStream, run, targetPage, planned);
    // The open block is deliberately kept over a fold: it is what makes the
    // next reopen an in-place swap too.
    editedCache->byAnchor[anchor].block = newBlock;

    // Folding several large files can turn the resident page window into a few
    // short headers. Let the newly freed viewport space reveal following files
    // immediately; waiting for a wheel event here recreates the blank lower half
    // this in-place path was meant to avoid redrawing.
    scheduleDiffWindowFill(view);

    swap.applied = true;
    swap.oldHtml = oldBlock;
    swap.newHtml = newBlock;
    return swap;
}

QString renderUnifiedDiffHtml(const QString &patch, QList<DiffFileEntry> &files,
                              const QString &dir, const QString &base,
                              const QString &head,
                              const QString &anchorFile = QString(),
                              const QHash<QString, QString> &lineNotes = {},
                              const QSet<QString> &viewedFiles = {},
                              const QSet<QString> &expandedFiles = {},
                              const DiffRenderAppearance &appearance = {},
                              DiffRenderSink *sink = nullptr)
{
    static const QRegularExpression hunkRe(
        QStringLiteral("@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@"));
    const bool anchors = !anchorFile.isEmpty();
    QString html;
    if (!sink)
        html.reserve(patch.size() * 3);
    constexpr qsizetype kDiffSinkChunkChars = 160 * 1024;
    const auto flushSink = [&] {
        if (!sink || sink->aborted || html.size() < kDiffSinkChunkChars)
            return;
        if (!sink->deliver(html, int(files.size())))
            sink->aborted = true;
        html.clear();
    };
    QString fileBody;
    const QStringList lines = patch.split(QLatin1Char('\n'));
    int oldNo = 0, newNo = 0, fileIdx = -1;
    bool inFile = false;
    bool inBinary = false;
    // Held rather than written straight into the body: a file that turns out to
    // have a before/after image preview says everything this row would, so the
    // decision can only be made once the header has been built.
    int binaryRowColumns = 0;
    bool previewShown = false;
    // The bytes of an image the patch carries inline, kept for the preview.
    QString binaryPayload;
    bool collectBinary = false;
    DiffMarkdownCollector markdown; // feeds a markdown file's Preview pill
    const QString editDir = diffEditableDir(dir, head);
    const auto curPath = [&] {
        return fileIdx >= 0 ? files[fileIdx].path : QString();
    };

    // A viewed file folds away to its header row unless the reviewer reopened
    // it: `expandedFiles` is that reopening, and it must not take the review
    // credit — the Viewed pill stays ticked over an expanded body.
    const auto isCollapsed = [&](const DiffFileEntry &f) {
        return viewedFiles.contains(f.path) && !expandedFiles.contains(f.path);
    };
    auto emitFileHeader = [&](int idx) {
        const DiffFileEntry &f = files[idx];
        html += diffFileHeaderHtml(f, viewedFiles.contains(f.path), anchors,
                                   expandedFiles.contains(f.path), appearance);
        previewShown = false;
        if (!isCollapsed(f)) {
            const QString preview =
                diffImagePreviewHtml(dir, base, head, f, binaryPayload);
            previewShown = !preview.isEmpty();
            html += preview;
            html += QStringLiteral(
                "<table class='difftable' width='100%' cellspacing='0' "
                "cellpadding='0'>");
        }
    };
    struct PendingRow {
        QString cls, oldCell, newCell, text;
    };
    QList<PendingRow> pendingRun;
    const auto emitRow = [&](const PendingRow &row, const QString &cellHtml) {
        const QString &status =
            fileIdx >= 0 ? files[fileIdx].status : QString();
        const bool addOnly = status == QLatin1String("added");
        const bool delOnly = status == QLatin1String("deleted");
        QString gutters;
        if (addOnly)
            gutters = gutterCellHtml(row.cls, row.newCell, QStringLiteral("new"),
                                     anchors, curPath(),
                                     lineNotes.value(lineNoteKey(
                                         curPath(), QStringLiteral("new"), row.newCell)));
        else if (delOnly)
            gutters = gutterCellHtml(row.cls, row.oldCell, QStringLiteral("old"),
                                     anchors, curPath(),
                                     lineNotes.value(lineNoteKey(
                                         curPath(), QStringLiteral("old"), row.oldCell)));
        else
            gutters =
                gutterCellHtml(row.cls, row.oldCell, QStringLiteral("old"), anchors,
                               curPath(),
                               lineNotes.value(lineNoteKey(
                                   curPath(), QStringLiteral("old"), row.oldCell))) +
                gutterCellHtml(row.cls, row.newCell, QStringLiteral("new"), anchors,
                               curPath(),
                               lineNotes.value(lineNoteKey(
                                   curPath(), QStringLiteral("new"), row.newCell)));
        fileBody += QStringLiteral(
                        "<tr>%1<td class='code %2' width='99%'>%3</td></tr>")
                        .arg(gutters, row.cls,
                             !cellHtml.isEmpty() ? cellHtml
                             : row.text.isEmpty() ? QStringLiteral("&nbsp;")
                                                  : row.text.toHtmlEscaped());
        if (anchors)
            fileBody += lineNoteRows(lineNotes, curPath(), row.oldCell, row.newCell,
                                     addOnly || delOnly ? 2 : 3);
    };
    const auto flushRun = [&] {
        QList<qsizetype> dels, adds;
        for (qsizetype i = 0; i < pendingRun.size(); ++i)
            (pendingRun[i].cls == QLatin1String("del") ? dels : adds).append(i);
        QStringList cellHtml(pendingRun.size());
        for (qsizetype k = 0; k < qMin(dels.size(), adds.size()); ++k) {
            const std::optional<diffintra::CellsHtml> intra =
                diffintra::intraLineCellsHtml(pendingRun[dels[k]].text,
                                              pendingRun[adds[k]].text,
                                              kDiffIntraLineMaxHtmlChars);
            if (!intra)
                continue;
            cellHtml[dels[k]] = intra->oldHtml;
            cellHtml[adds[k]] = intra->newHtml;
        }
        for (qsizetype i = 0; i < pendingRun.size(); ++i)
            emitRow(pendingRun[i], cellHtml[i]);
        pendingRun.clear();
    };
    auto closeFile = [&] {
        if (inFile) {
            flushRun();
            const bool collapsed = isCollapsed(files[fileIdx]);
            markdown.finish(files[fileIdx], dir, base, head);
            diffStampEditDir(files[fileIdx], editDir, dir, head);
            emitFileHeader(fileIdx);
            if (collapsed) {
                html += QStringLiteral("</div>");
            } else {
                // Indexed fresh rather than through a held reference:
                // emitFileHeader reads image blobs through git, which pumps the
                // event loop, and this file's entry is the one thing that must
                // still be valid after.
                if (binaryRowColumns > 0 && !previewShown)
                    fileBody += diffBinaryRowHtml(files[fileIdx], binaryRowColumns);
                const QString &status = files[fileIdx].status;
                const bool oneSidedFile =
                    status == QLatin1String("added") ||
                    status == QLatin1String("deleted");
                html += fileLevelNoteRow(lineNotes, files[fileIdx].path,
                                         oneSidedFile ? 2 : 3);
                html += fileBody;
                html += QStringLiteral("</table></div>");
            }
            fileBody.clear();
            binaryRowColumns = 0;
            binaryPayload.clear();
            collectBinary = false;
            inFile = false;
            flushSink();
        }
    };

    for (const QString &line : lines) {
        if (sink && sink->aborted)
            break;
        if (line.startsWith(QLatin1String("diff --git "))) {
            closeFile();
            QString path = line;
            const int bpos = line.indexOf(QLatin1String(" b/"));
            if (bpos >= 0)
                path = line.mid(bpos + 3);
            DiffFileEntry f;
            f.path = path;
            f.anchor = QStringLiteral("file-%1").arg(files.size());
            files.append(f);
            fileIdx = files.size() - 1;
            inFile = true;
            inBinary = false;
            markdown.start(path);
            continue;
        }
        if (!inFile)
            continue;
        if (fileIdx >= 0) {
            if (line.startsWith(QLatin1String("new file")))
                files[fileIdx].status = QStringLiteral("added");
            else if (line.startsWith(QLatin1String("deleted file")))
                files[fileIdx].status = QStringLiteral("deleted");
            else if (line.startsWith(QLatin1String("rename ")) ||
                     line.startsWith(QLatin1String("similarity ")))
                files[fileIdx].status = QStringLiteral("renamed");
        }
        if (line.startsWith(QLatin1String("GIT binary patch")) ||
            line.startsWith(QLatin1String("Binary files "))) {
            if (fileIdx >= 0 && !files[fileIdx].binary) {
                files[fileIdx].binary = true;
                const bool oneSided =
                    files[fileIdx].status == QLatin1String("added") ||
                    files[fileIdx].status == QLatin1String("deleted");
                binaryRowColumns = oneSided ? 2 : 3;
            }
            inBinary = true;
            // Only an image's bytes are worth holding, and only as far as one
            // could weigh: base85 costs five characters per four bytes.
            collectBinary = fileIdx >= 0 &&
                            line.startsWith(QLatin1String("GIT binary patch")) &&
                            !diffImageMimeForPath(files[fileIdx].path).isEmpty();
            continue;
        }
        if (inBinary) {
            constexpr qsizetype kMaxBinaryPayloadChars = 4 * 1024 * 1024;
            if (collectBinary && binaryPayload.size() < kMaxBinaryPayloadChars) {
                binaryPayload += line;
                binaryPayload += QLatin1Char('\n');
            }
            continue;
        }
        if (line.startsWith(QLatin1String("index ")) ||
            line.startsWith(QLatin1String("--- ")) ||
            line.startsWith(QLatin1String("+++ ")) ||
            line.startsWith(QLatin1String("new file")) ||
            line.startsWith(QLatin1String("deleted file")) ||
            line.startsWith(QLatin1String("similarity ")) ||
            line.startsWith(QLatin1String("rename ")) ||
            line.startsWith(QLatin1String("old mode")) ||
            line.startsWith(QLatin1String("new mode")))
            continue;
        markdown.line(line);
        const bool oneSided =
            fileIdx >= 0 && (files[fileIdx].status == QLatin1String("added") ||
                             files[fileIdx].status == QLatin1String("deleted"));
        if (line.startsWith(QLatin1String("@@"))) {
            flushRun();
            const QRegularExpressionMatch m = hunkRe.match(line);
            if (m.hasMatch()) {
                oldNo = m.captured(1).toInt();
                newNo = m.captured(2).toInt();
            }
            fileBody += QStringLiteral("<tr>%1<td class='code hunk'>%2</td></tr>")
                            .arg(oneSided
                                     ? QStringLiteral("<td class='ln hunk' width='64'></td>")
                                     : QStringLiteral("<td class='ln hunk' width='64'></td>"
                                                      "<td class='ln hunk' width='64'></td>"),
                                 line.toHtmlEscaped());
            continue;
        }

        const QChar c0 = line.isEmpty() ? QLatin1Char(' ') : line.at(0);
        QString text = line.isEmpty() ? QString() : line.mid(1);
        QString cls, oldCell, newCell;
        if (c0 == QLatin1Char('+')) {
            cls = QStringLiteral("add");
            newCell = QString::number(newNo++);
            if (fileIdx >= 0)
                ++files[fileIdx].adds;
        } else if (c0 == QLatin1Char('-')) {
            cls = QStringLiteral("del");
            oldCell = QString::number(oldNo++);
            if (fileIdx >= 0)
                ++files[fileIdx].dels;
        } else if (c0 == QLatin1Char('\\')) { // "\ No newline at end of file"
            cls = QStringLiteral("ctx");
            text = line;
        } else {
            cls = QStringLiteral("ctx");
            oldCell = QString::number(oldNo++);
            newCell = QString::number(newNo++);
        }
        if (oneSided || cls == QLatin1String("ctx")) {
            flushRun();
            emitRow({cls, oldCell, newCell, text}, QString());
        } else {
            pendingRun.append({cls, oldCell, newCell, text});
        }
    }
    closeFile();
    if (sink) {
        if (!sink->aborted && !html.isEmpty() &&
            !sink->deliver(html, int(files.size())))
            sink->aborted = true;
        return QString();
    }
    return html;
}

QString renderSplitDiffHtml(const QString &patch, QList<DiffFileEntry> &files,
                            const QString &dir, const QString &base,
                            const QString &head,
                            const QString &anchorFile = QString(),
                            const QHash<QString, QString> &lineNotes = {},
                            const QSet<QString> &viewedFiles = {},
                            const QSet<QString> &expandedFiles = {},
                            const DiffRenderAppearance &appearance = {},
                            DiffRenderSink *sink = nullptr)
{
    static const QRegularExpression hunkRe(
        QStringLiteral("@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@"));
    const bool anchors = !anchorFile.isEmpty();
    QString html;
    if (!sink)
        html.reserve(patch.size() * 3);
    constexpr qsizetype kDiffSinkChunkChars = 160 * 1024;
    const auto flushSink = [&] {
        if (!sink || sink->aborted || html.size() < kDiffSinkChunkChars)
            return;
        if (!sink->deliver(html, int(files.size())))
            sink->aborted = true;
        html.clear();
    };
    QString fileBody;
    const QStringList lines = patch.split(QLatin1Char('\n'));
    int oldNo = 0, newNo = 0, fileIdx = -1;
    bool inFile = false;
    bool inBinary = false; // see renderUnifiedDiffHtml
    // Held until the header is built: see renderUnifiedDiffHtml.
    int binaryRowColumns = 0;
    bool previewShown = false;
    QString binaryPayload; // see renderUnifiedDiffHtml
    bool collectBinary = false;
    DiffMarkdownCollector markdown; // see renderUnifiedDiffHtml
    const QString editDir = diffEditableDir(dir, head);

    const auto curPath = [&] {
        return fileIdx >= 0 ? files[fileIdx].path : QString();
    };
    const auto gut = [&](const QString &extraCls, const QString &num,
                         const QString &side) {
        if (num.isEmpty())
            return QStringLiteral("<td class='ln %1' width='64'></td>").arg(extraCls);
        if (!anchors)
            return QStringLiteral("<td class='ln %1' width='64'>%2</td>")
                .arg(extraCls, num);
        const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(curPath()));
        const QString note = lineNotes.value(lineNoteKey(curPath(), side, num));
        const QString marker = lineNoteHasThread(note)
                                   ? QStringLiteral(
                                         " <a class='linecommenticon' "
                                         "href='threadtoggle:%1?s=%2&l=%3' "
                                         "title='Show or hide inline comments'>"
                                         "&#128100;</a>")
                                         .arg(enc, side, num)
                                   : QString();
        return QStringLiteral("<td class='ln lnlink %1' width='64'>"
                              "<a href='cmt:%2?s=%3&l=%4' title='Comment on this line'>"
                              "%4</a>%5</td>")
            .arg(extraCls, enc, side, num, marker);
    };

    QStringList pendingDel, pendingAdd;
    const auto emitText = [](const QString &t) {
        return t.isEmpty() ? QStringLiteral("&nbsp;") : t.toHtmlEscaped();
    };
    bool inChangeBlock = false;
    const auto changeArrowsHtml = [&]() -> QString {
        if (!appearance.changeArrows || fileIdx < 0 || files[fileIdx].binary ||
            !files[fileIdx].status.isEmpty())
            return QString();
        const bool staged = fileIdx < appearance.changeArrowsStagedFiles;
        const QString href =
            QStringLiteral("scmblock:%1?s=%2&o=%3&n=%4&d=")
                .arg(QString::fromLatin1(QUrl::toPercentEncoding(curPath())),
                     staged ? QStringLiteral("1") : QStringLiteral("0"),
                     QString::number(oldNo), QString::number(newNo));
        QString arrows =
            QStringLiteral("<a class='blockarrow' href='%1old' title='%2'>"
                           "&#8594;</a>")
                .arg(href, staged ? QStringLiteral("Unstage this change")
                                  : QStringLiteral("Revert this change in the "
                                                   "working file"));
        if (!staged)
            arrows += QStringLiteral("<a class='blockarrow' href='%1new' "
                                     "title='Stage this change'>&#8592;</a>")
                          .arg(href);
        return arrows + QLatin1Char(' ');
    };
    const auto oneSidedKind = [&]() -> int {
        if (fileIdx < 0)
            return 0;
        if (files[fileIdx].status == QLatin1String("added"))
            return 1; // add-only
        if (files[fileIdx].status == QLatin1String("deleted"))
            return -1; // del-only
        return 0;
    };
    const auto flushPairs = [&] {
        if (const int kind = oneSidedKind()) {
            const bool addOnly = kind > 0;
            const QStringList &buf = addOnly ? pendingAdd : pendingDel;
            const QString cls =
                addOnly ? QStringLiteral("add") : QStringLiteral("del");
            const QString side =
                addOnly ? QStringLiteral("new") : QStringLiteral("old");
            for (const QString &t : buf) {
                const QString ln = QString::number(addOnly ? newNo++ : oldNo++);
                fileBody += QStringLiteral(
                                 "<tr>%1<td class='code %2' width='99%'>%3</td></tr>")
                                 .arg(gut(cls, ln, side), cls, emitText(t));
                if (anchors)
                    fileBody += lineNoteRows(lineNotes, curPath(),
                                             addOnly ? QString() : ln,
                                             addOnly ? ln : QString(), 2);
            }
            pendingDel.clear();
            pendingAdd.clear();
            return;
        }
        const int n = qMax(pendingDel.size(), pendingAdd.size());
        const QString arrows =
            n > 0 && !inChangeBlock ? changeArrowsHtml() : QString();
        if (n > 0)
            inChangeBlock = true;
        for (int i = 0; i < n; ++i) {
            const bool hasDel = i < pendingDel.size();
            const bool hasAdd = i < pendingAdd.size();
            const QString oldLn = hasDel ? QString::number(oldNo++) : QString();
            const QString newLn = hasAdd ? QString::number(newNo++) : QString();
            const QString delCls = hasDel ? QStringLiteral("del") : QString();
            const QString addCls = hasAdd ? QStringLiteral("add") : QString();
            const std::optional<diffintra::CellsHtml> intra =
                hasDel && hasAdd
                    ? diffintra::intraLineCellsHtml(pendingDel.at(i), pendingAdd.at(i),
                                                    kDiffIntraLineMaxHtmlChars)
                    : std::nullopt;
            QString newGut =
                gut(QStringLiteral("nln ") + addCls, newLn, QStringLiteral("new"));
            if (i == 0 && !arrows.isEmpty())
                newGut.insert(newGut.indexOf(QLatin1Char('>')) + 1, arrows);
            fileBody += QStringLiteral("<tr>%1<td class='code ocode %2' width='49%'>%3</td>"
                                       "%4<td class='code ncode %5' width='49%'>%6</td></tr>")
                            .arg(gut(delCls, oldLn, QStringLiteral("old")), delCls,
                                 intra ? intra->oldHtml
                                 : hasDel ? emitText(pendingDel.at(i))
                                          : QStringLiteral("&nbsp;"),
                                 newGut,
                                 addCls,
                                 intra ? intra->newHtml
                                 : hasAdd ? emitText(pendingAdd.at(i))
                                          : QStringLiteral("&nbsp;"));
            if (anchors)
                fileBody += lineNoteRows(lineNotes, curPath(), oldLn, newLn, 4);
        }
        pendingDel.clear();
        pendingAdd.clear();
    };

    // See renderUnifiedDiffHtml: viewed folds the file away, expanded reopens
    // it without unticking the pill.
    const auto isCollapsed = [&](const DiffFileEntry &f) {
        return viewedFiles.contains(f.path) && !expandedFiles.contains(f.path);
    };
    auto emitFileHeader = [&](int idx) {
        const DiffFileEntry &f = files[idx];
        html += diffFileHeaderHtml(f, viewedFiles.contains(f.path), anchors,
                                   expandedFiles.contains(f.path), appearance);
        previewShown = false;
        if (!isCollapsed(f)) {
            const QString preview =
                diffImagePreviewHtml(dir, base, head, f, binaryPayload);
            previewShown = !preview.isEmpty();
            html += preview;
            html += QStringLiteral(
                "<table class='difftable' width='100%' cellspacing='0' "
                "cellpadding='0'>");
        }
    };
    auto closeFile = [&] {
        if (inFile) {
            const bool collapsed = isCollapsed(files[fileIdx]);
            flushPairs();
            markdown.finish(files[fileIdx], dir, base, head);
            diffStampEditDir(files[fileIdx], editDir, dir, head);
            emitFileHeader(fileIdx);
            if (collapsed) {
                html += QStringLiteral("</div>");
            } else {
                if (binaryRowColumns > 0 && !previewShown)
                    fileBody += diffBinaryRowHtml(files[fileIdx], binaryRowColumns);
                html += fileLevelNoteRow(lineNotes, files[fileIdx].path,
                                         oneSidedKind() ? 2 : 4);
                html += fileBody;
                html += QStringLiteral("</table></div>");
            }
            fileBody.clear();
            binaryRowColumns = 0;
            binaryPayload.clear();
            collectBinary = false;
            inFile = false;
            flushSink();
        }
    };

    for (const QString &line : lines) {
        if (sink && sink->aborted)
            break;
        if (line.startsWith(QLatin1String("diff --git "))) {
            closeFile();
            QString path = line;
            const int bpos = line.indexOf(QLatin1String(" b/"));
            if (bpos >= 0)
                path = line.mid(bpos + 3);
            DiffFileEntry f;
            f.path = path;
            f.anchor = QStringLiteral("file-%1").arg(files.size());
            files.append(f);
            fileIdx = files.size() - 1;
            inFile = true;
            inBinary = false;
            markdown.start(path);
            continue;
        }
        if (!inFile)
            continue;
        if (fileIdx >= 0) {
            if (line.startsWith(QLatin1String("new file")))
                files[fileIdx].status = QStringLiteral("added");
            else if (line.startsWith(QLatin1String("deleted file")))
                files[fileIdx].status = QStringLiteral("deleted");
            else if (line.startsWith(QLatin1String("rename ")) ||
                     line.startsWith(QLatin1String("similarity ")))
                files[fileIdx].status = QStringLiteral("renamed");
        }
        if (line.startsWith(QLatin1String("GIT binary patch")) ||
            line.startsWith(QLatin1String("Binary files "))) {
            if (fileIdx >= 0 && !files[fileIdx].binary) {
                files[fileIdx].binary = true;
                binaryRowColumns = oneSidedKind() ? 2 : 4;
            }
            inBinary = true;
            // Only an image's bytes are worth holding, and only as far as one
            // could weigh: base85 costs five characters per four bytes.
            collectBinary = fileIdx >= 0 &&
                            line.startsWith(QLatin1String("GIT binary patch")) &&
                            !diffImageMimeForPath(files[fileIdx].path).isEmpty();
            continue;
        }
        if (inBinary) {
            constexpr qsizetype kMaxBinaryPayloadChars = 4 * 1024 * 1024;
            if (collectBinary && binaryPayload.size() < kMaxBinaryPayloadChars) {
                binaryPayload += line;
                binaryPayload += QLatin1Char('\n');
            }
            continue;
        }
        if (line.startsWith(QLatin1String("index ")) ||
            line.startsWith(QLatin1String("--- ")) ||
            line.startsWith(QLatin1String("+++ ")) ||
            line.startsWith(QLatin1String("new file")) ||
            line.startsWith(QLatin1String("deleted file")) ||
            line.startsWith(QLatin1String("similarity ")) ||
            line.startsWith(QLatin1String("rename ")) ||
            line.startsWith(QLatin1String("old mode")) ||
            line.startsWith(QLatin1String("new mode")))
            continue;
        markdown.line(line);
        if (line.startsWith(QLatin1String("@@"))) {
            flushPairs();
            inChangeBlock = false;
            const QRegularExpressionMatch m = hunkRe.match(line);
            if (m.hasMatch()) {
                oldNo = m.captured(1).toInt();
                newNo = m.captured(2).toInt();
            }
            fileBody += oneSidedKind()
                            ? QStringLiteral("<tr><td class='ln hunk' width='64'></td>"
                                             "<td class='code hunk' width='99%'>"
                                             "%1</td></tr>")
                                  .arg(line.toHtmlEscaped())
                            : QStringLiteral("<tr><td class='code hunk' "
                                             "colspan='4'>%1</td></tr>")
                                  .arg(line.toHtmlEscaped());
            continue;
        }

        const QChar c0 = line.isEmpty() ? QLatin1Char(' ') : line.at(0);
        const QString text = line.isEmpty() ? QString() : line.mid(1);
        if (c0 == QLatin1Char('+')) {
            pendingAdd << text;
            if (fileIdx >= 0)
                ++files[fileIdx].adds;
        } else if (c0 == QLatin1Char('-')) {
            pendingDel << text;
            if (fileIdx >= 0)
                ++files[fileIdx].dels;
        } else if (c0 == QLatin1Char('\\')) { // "\ No newline at end of file"
            flushPairs();
            fileBody += oneSidedKind()
                            ? QStringLiteral("<tr><td class='ln' width='64'></td>"
                                             "<td class='code' width='99%'>%1</td>"
                                             "</tr>")
                                  .arg(line.toHtmlEscaped())
                            : QStringLiteral(
                                  "<tr><td class='ln' width='64'></td>"
                                  "<td class='code ocode' width='49%'>%1</td>"
                                  "<td class='ln nln' width='64'></td>"
                                  "<td class='code ncode' width='49%'>%1</td></tr>")
                                  .arg(line.toHtmlEscaped());
        } else if (const int kind = oneSidedKind()) {
            flushPairs();
            const bool addOnly = kind > 0;
            const QString ln = QString::number(addOnly ? newNo++ : oldNo++);
            if (addOnly)
                ++oldNo;
            else
                ++newNo;
            fileBody += QStringLiteral(
                            "<tr>%1<td class='code' width='99%'>%2</td></tr>")
                            .arg(gut(QString(), ln,
                                     addOnly ? QStringLiteral("new")
                                             : QStringLiteral("old")),
                                 emitText(text));
        } else {
            flushPairs();
            inChangeBlock = false;
            const QString ln1 = QString::number(oldNo++);
            const QString ln2 = QString::number(newNo++);
            fileBody += QStringLiteral(
                            "<tr>%1<td class='code ocode' width='49%'>%3</td>"
                            "%2<td class='code ncode' width='49%'>%3</td></tr>")
                            .arg(gut(QString(), ln1, QStringLiteral("old")),
                                 gut(QStringLiteral("nln"), ln2, QStringLiteral("new")),
                                 emitText(text));
            if (anchors)
                fileBody += lineNoteRows(lineNotes, curPath(), ln1, ln2, 4);
        }
    }
    closeFile();
    if (sink) {
        if (!sink->aborted && !html.isEmpty() &&
            !sink->deliver(html, int(files.size())))
            sink->aborted = true;
        return QString();
    }
    return html;
}

bool diffSplitPref()
{
    return QSettings().value(QStringLiteral("view/diffSplit"), true).toBool();
}
void setDiffSplitPref(bool split)
{
    QSettings().setValue(QStringLiteral("view/diffSplit"), split);
}

bool autoMarkViewedOnScrollPref()
{
    return QSettings()
        .value(QStringLiteral("view/autoMarkViewedOnScroll"), false)
        .toBool();
}
void setAutoMarkViewedOnScrollPref(bool on)
{
    QSettings().setValue(QStringLiteral("view/autoMarkViewedOnScroll"), on);
}

void applyDiffSearchHighlights(QTextBrowser *diff,
                               const QList<QTextCursor> &matches, int activeIndex,
                               QLabel *countLabel, bool termEmpty)
{
    diff->setExtraSelections(findMatchSelections(matches, activeIndex));
    if (countLabel)
        countLabel->setText(
            findMatchCountText(activeIndex, matches.size(), termEmpty));
}

QList<QTextEdit::ExtraSelection> findMatchSelections(const QList<QTextCursor> &matches,
                                                     int activeIndex)
{
    QList<QTextEdit::ExtraSelection> sels;
    QTextCharFormat matchFmt;
    matchFmt.setBackground(QColor("#e3b341"));
    matchFmt.setForeground(Theme::remapColor(QColor("#0d1117")));
    QTextCharFormat currentFmt;
    currentFmt.setBackground(QColor("#f78166"));
    currentFmt.setForeground(Theme::remapColor(QColor("#0d1117")));
    for (int i = 0; i < matches.size(); ++i) {
        QTextEdit::ExtraSelection sel;
        sel.cursor = matches.at(i);
        sel.format = (i == activeIndex) ? currentFmt : matchFmt;
        sels.append(sel);
    }
    return sels;
}

QString findMatchCountText(int activeIndex, int count, bool termEmpty)
{
    if (termEmpty)
        return {};
    return count == 0 ? QStringLiteral("No results")
                      : QStringLiteral("%1/%2").arg(activeIndex + 1).arg(count);
}

DiffViewedPrune pruneDiffViewedStamps(QHash<QString, QString> stamps,
                                      const QHash<QString, QString> &current)
{
    DiffViewedPrune out;
    for (auto it = stamps.begin(); it != stamps.end();) {
        const auto now = current.constFind(it.key());
        if (now == current.constEnd()) {
            // Not in this patch at all: the file may simply be out of the
            // range being viewed, so the mark keeps standing. Panes that know
            // better (source control) clear it themselves.
            ++it;
            continue;
        }
        if (it.value().isEmpty()) {
            // A mark from before fingerprints, or one ticked off a patch this
            // window never rendered. Stamp it now so the next change catches.
            *it = *now;
            out.changed = true;
            ++it;
            continue;
        }
        if (it.value() == *now) {
            ++it;
            continue;
        }
        out.dropped.append(it.key());
        it = stamps.erase(it); // the file changed; it wants looking at again
        out.changed = true;
    }
    out.stamps = std::move(stamps);
    return out;
}

QHash<QString, QString> diffFileFingerprints(const QString &patch)
{
    // Walked as views: this runs on every render of a diff pane, and a PR
    // carrying an encoded binary is tens of megabytes that must not be copied
    // line by line to answer a question about review state.
    const QStringView all(patch);
    QHash<QString, QByteArray> bodies;
    QString path;
    qsizetype sectionStart = 0;
    auto flush = [&](qsizetype end) {
        if (!path.isEmpty() && end > sectionStart)
            bodies[path] += all.sliced(sectionStart, end - sectionStart).toUtf8();
    };
    for (qsizetype start = 0; start <= all.size();) {
        const qsizetype newline = all.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? all.size() : newline;
        const QStringView line = all.sliced(start, end - start);
        if (line.startsWith(QLatin1String("diff --git "))) {
            flush(start);
            // Named exactly as the renderers name it, or the fingerprint would
            // be filed under a path no review state ever mentions.
            const qsizetype marker = line.indexOf(QLatin1String(" b/"));
            path = (marker >= 0 ? line.sliced(marker + 3) : line).toString();
            sectionStart = start;
        }
        if (newline < 0)
            break;
        start = newline + 1;
    }
    flush(all.size());

    QHash<QString, QString> out;
    out.reserve(bodies.size());
    for (auto it = bodies.cbegin(); it != bodies.cend(); ++it)
        out.insert(it.key(),
                   QString::fromLatin1(
                       QCryptographicHash::hash(it.value(),
                                                QCryptographicHash::Sha1)
                           .toHex()
                           .left(kDiffFingerprintChars)));
    return out;
}

QString renderDiffHtmlSplit(bool split, const QString &patch,
                            QList<DiffFileEntry> &files, const QString &dir,
                            const QString &base, const QString &head,
                            const QString &anchorFile,
                            const QHash<QString, QString> &lineNotes,
                            const QSet<QString> &viewedFiles,
                            const QSet<QString> &expandedFiles,
                            std::optional<DiffRenderAppearance> appearanceOverride,
                            DiffRenderSink *sink)
{
    // QApplication, QPalette and QPixmap are GUI-thread state. Background diff
    // rendering receives this immutable snapshot and builds icons into QImage,
    // so teardown cannot free raster pixmaps from the worker.
    const DiffRenderAppearance appearance =
        appearanceOverride.has_value() ? *appearanceOverride
                                       : currentDiffRenderAppearance();
    static const QString endOfDiff =
        QStringLiteral("<div class='diffend'>END OF DIFF</div>");
    QString html =
        split ? renderSplitDiffHtml(patch, files, dir, base, head, anchorFile,
                                    lineNotes, viewedFiles, expandedFiles,
                                    appearance, sink)
              : renderUnifiedDiffHtml(patch, files, dir, base, head, anchorFile,
                                      lineNotes, viewedFiles, expandedFiles,
                                      appearance, sink);
    if (sink) {
        if (!sink->aborted && !files.isEmpty() &&
            !sink->deliver(endOfDiff, int(files.size())))
            sink->aborted = true;
        return QString();
    }
    if (html.isEmpty())
        return html;
    // The bar closes the last file, so it sits straight under that file's last
    // row: the blank runway this used to lead with pushed it a screen away from
    // the diff it terminates. The room to scroll those last rows clear of the
    // viewport (adhoc #1450) comes from the end cap appendDiffEndCap() draws
    // below this bar, which is a whole viewport tall and resizes with the view.
    html += endOfDiff;
    return html;
}

QString renderDiffHtml(const QString &patch, QList<DiffFileEntry> &files,
                       const QString &dir, const QString &base, const QString &head,
                       const QString &anchorFile,
                       const QHash<QString, QString> &lineNotes,
                       const QSet<QString> &viewedFiles,
                       const QSet<QString> &expandedFiles)
{
    return renderDiffHtmlSplit(diffSplitPref(), patch, files, dir, base, head,
                               anchorFile, lineNotes, viewedFiles,
                               expandedFiles);
}

QString diffStyleSheet(int fontPt)
{
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString addBg = dark ? "#12261c" : "#e6ffec";
    const QString delBg = dark ? "#2d1416" : "#ffebe9";
    const QString addEmphBg = dark ? "#1a4a29" : "#abf2bc";
    const QString delEmphBg = dark ? "#6b2b2b" : "#ffcecb";
    const QString hunkBg =
        Theme::nextOrHex(Theme::Role::SurfaceRaised, dark ? "#0d1d33" : "#ddf4ff");
    const QString hunkFg =
        Theme::nextOrHex(Theme::Role::FgMuted, dark ? "#58a6ff" : "#0969da");
    const QString lnFg = Theme::nextOrHex(Theme::Role::FgMuted, "#8b949e");
    const QString lineNumberFg = Theme::nextOrHex(Theme::Role::FgSubtle, "#8b949e");
    const QString headBg =
        Theme::nextOrHex(Theme::Role::Surface, dark ? "#161b22" : "#f6f8fa");
    const QString border =
        Theme::nextOrHex(Theme::Role::Border, dark ? "#30363d" : "#d0d7de");
    const QString gutterBg =
        Theme::nextOrHex(Theme::Role::Bg, dark ? "#0d1117" : "#f6f8fa");
    const QString fg = Theme::nextOrHex(Theme::Role::Fg, dark ? "#e6edf3" : "#1f2328");
    const QString endBg = Theme::nextOrHex(Theme::Role::SurfaceRaised, "#000000");
    const QString endFg = Theme::nextOrHex(Theme::Role::FgSubtle, "#ffffff");
    return QStringLiteral(
               ".fileblock { margin:0 0 14px 0; }"
               ".fileblock.viewed { margin:0 0 1px 0; }"
               ".fileblockcont { margin:0; }"
               ".fileheader { background:%1; font-family:monospace; "
               "font-size:13px; border-collapse:collapse; border-radius:7px; }"
               ".fileheader td { background:%1; }"
               "td.fpathcell { white-space:nowrap; }"
               "td.fctlcell { white-space:nowrap; }"
               ".difftable { font-family:monospace; font-size:%10px; width:100%; "
               "border-left:1px solid %7; border-right:1px solid %7; "
               "border-bottom:1px solid %7; }"
               ".imagetable { border-left:1px solid %7; border-right:1px solid %7; }"
               ".imgcell { width:50%; padding:10px; text-align:center; }"
               ".imgcell img { max-width:100%; max-height:360px; }"
               ".imgempty { color:%2; padding:60px 0; border:1px solid %7; }"
               ".imgcaption { color:%2; font-size:12px; margin-top:6px; }"
               "td.ln { color:%11; text-align:right; padding:0 6px; width:4em; "
               "min-width:4em; max-width:4em; "
               "font-size:%10px; white-space:nowrap; background:%9; "
               "border-right:1px solid %7; }"
               "td.code { white-space:pre-wrap; padding:0 10px; color:%8; "
               "font-size:%10px; }"
               "td.ocode, td.ncode { width:49%; }"
               ".add { background:%3; } .del { background:%4; }"
               ".hunk { color:%5; background:%6; }"
               "td.ln.hunk { background:%6; border-right:1px solid %7; }"
               "td.nln { border-left:1px solid %7; }"
               "td.lnlink a { color:%11; text-decoration:none; }"
               "td.lnlink a.linecommenticon { color:#58a6ff; font-size:9px; "
               "padding-left:3px; }"
               "td.ln a.blockarrow { color:#58a6ff; text-decoration:none; "
               "font-weight:700; padding:0 2px; }"
               "td.notecell { padding:8px 12px; background:%1; "
               "border:1px solid %7; color:%8; white-space:normal; }"
               ".reviewthread { font-family:sans-serif; }"
               ".threadhead { color:%8; font-size:12px; margin-bottom:8px; }"
               ".threadstate { font-size:10px; font-weight:700; padding:1px 6px; "
               "border:1px solid %7; }"
               ".threadstate.resolved { color:#3fb950; }"
               ".threadstate.unresolved { color:#d29922; }"
               ".threadstate.outdated { color:%2; }"
               ".threadstate.moved { color:#58a6ff; }"
               ".threadplace { color:%2; font-size:11px; margin:-4px 0 8px 0; }"
               ".threadevent { margin-top:8px; padding-top:8px; "
               "border-top:1px solid %7; }"
               ".threadbody { color:%8; }"
               ".threadsystem { color:%2; font-size:11px; margin-top:6px; }"
               ".threadactions { margin-top:8px; }"
               ".threadactions a { color:#58a6ff; text-decoration:none; }"
               ".diffresponsive { color:%2; background:%1; padding:8px 12px; "
               "border:1px solid %7; font-family:sans-serif; font-size:12px; }"
               ".suggestion { background:%9; border:1px solid %7; color:%8; "
               "padding:8px; margin-top:6px; white-space:pre; }"
               ".notehdr { color:%2; font-size:11px; margin-bottom:4px; }"
               ".inlinecomposerspace { min-height:174px; color:%2; "
               "background:%1; border:1px solid %7; padding:8px; }"
               ".diffend { background:%12; color:%13; "
               "font-family:sans-serif; font-size:11px; font-weight:700; "
               "letter-spacing:2px; text-align:center; padding:10px; }")
               .arg(headBg, lnFg, addBg, delBg, hunkFg, hunkBg, border, fg, gutterBg)
               .arg(qBound(8, fontPt, 28))
               .arg(lineNumberFg, endBg, endFg) +
           QStringLiteral(".addx { background:%1; } .delx { background:%2; }")
               .arg(addEmphBg, delEmphBg);
}


} // namespace forkmesh::ui
