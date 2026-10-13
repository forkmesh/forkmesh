#include "PullReviewModel.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QStringView>

#include <cstdlib>

namespace {

const QLatin1String kAgentTrailer("ForkMesh-Agent:");

QString agentTrailerIn(QStringView text)
{
    for (qsizetype start = 0; start <= text.size();) {
        const qsizetype newline = text.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? text.size() : newline;
        const QStringView t = text.sliced(start, end - start).trimmed();
        if (t.startsWith(kAgentTrailer))
            return t.mid(kAgentTrailer.size()).trimmed().toString();
        if (newline < 0)
            break;
        start = newline + 1;
    }
    return QString();
}

QString legacyThreadId(const PullEvent &ev, int index)
{
    if (!ev.id.isEmpty())
        return ev.id;
    return QStringLiteral("legacy:%1:%2:%3:%4")
        .arg(ev.path, ev.side)
        .arg(ev.line)
        .arg(index);
}

int ensureThread(QList<PullReviewThread> &threads, QHash<QString, int> &indexById,
                 const QString &id)
{
    const QString key = id.isEmpty()
                            ? QStringLiteral("thread:%1").arg(threads.size() + 1)
                            : id;
    const auto it = indexById.constFind(key);
    if (it != indexById.constEnd())
        return it.value();
    PullReviewThread thread;
    thread.id = key;
    threads.append(thread);
    const int index = threads.size() - 1;
    indexById.insert(key, index);
    return index;
}

void copyAnchor(PullReviewThread &thread, const PullEvent &ev)
{
    if (!ev.path.isEmpty())
        thread.path = ev.path;
    if (!ev.side.isEmpty())
        thread.side = ev.side;
    if (ev.lineStart > 0)
        thread.lineStart = ev.lineStart;
    if (ev.lineEnd > 0)
        thread.lineEnd = ev.lineEnd;
    if (thread.lineStart == 0 && ev.line > 0)
        thread.lineStart = ev.line;
    if (thread.lineEnd == 0 && ev.line > 0)
        thread.lineEnd = ev.line;
    if (!ev.patchset.isEmpty())
        thread.patchset = ev.patchset;
    if (!ev.anchorLines.isEmpty())
        thread.anchorLines = ev.anchorLines;
}

bool isMboxPatchBoundary(QStringView line)
{
    if (!line.startsWith(QLatin1String("From ")))
        return false;
    const qsizetype end = line.indexOf(u' ', 5);
    const qsizetype length = end - 5;
    if (end < 0 || length < 7 || length > 40)
        return false;
    for (qsizetype i = 5; i < end; ++i) {
        const QChar ch = line.at(i).toLower();
        if (!ch.isDigit() && (ch < u'a' || ch > u'f'))
            return false;
    }
    return true;
}

} // namespace

PullAgentProvenance pullAgentProvenance(const PullRequest &pr)
{
    return pullAgentProvenanceIn(pr.commits);
}

PullAgentProvenance pullAgentProvenanceIn(const QString &commits)
{
    PullAgentProvenance prov;
    // The trailer rides inside the signed commit series; the first commit that
    // carries it settles the tool/model for the whole PR.
    const QString value = agentTrailerIn(commits);
    if (value.isEmpty())
        return prov;
    prov.isAgent = true;
    prov.value = value;
    const int slash = value.indexOf(u'/');
    if (slash >= 0) {
        prov.tool = value.left(slash);
        prov.model = value.mid(slash + 1);
    } else {
        prov.tool = value;
    }
    return prov;
}

QHash<QString, bool> pullFileAuthorship(const PullRequest &pr)
{
    QHash<QString, bool> authorship;
    // Walk the mbox as views instead of splitting it into one QString per
    // commit and then one QString per line. A deleted binary is base64-encoded
    // in format-patch output; a single such PR can make the commit series tens
    // of megabytes, and the old cascade of copies blocked the pulls page for
    // seconds merely to decide which file rows receive an agent badge.
    const QStringView mbox(pr.commits);
    bool agent = false;
    bool inDiff = false;
    for (qsizetype start = 0; start <= mbox.size();) {
        const qsizetype newline = mbox.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? mbox.size() : newline;
        QStringView line = mbox.sliced(start, end - start);
        if (line.endsWith(u'\r'))
            line.chop(1);

        if (isMboxPatchBoundary(line)) {
            agent = false;
            inDiff = false;
        } else if (!inDiff && line.trimmed().startsWith(kAgentTrailer)) {
            agent = true;
        }

        if (line.startsWith(QLatin1String("diff --git "))) {
            inDiff = true;
            const qsizetype marker = line.indexOf(QLatin1String(" b/"));
            if (marker >= 0) {
                const QString path = line.sliced(marker + 3).toString();
                if (!path.isEmpty())
                    authorship[path] = authorship.value(path, false) || agent;
            }
        }

        if (newline < 0)
            break;
        start = newline + 1;
    }
    return authorship;
}


namespace {

// One side ("old" or "new") of one file in a unified diff: the line numbers
// that side has in the diff and the text at each, in diff order. Consecutive
// entries are consecutive lines only within a hunk, so a match must also check
// the numbers run without gaps.
struct DiffSideIndex {
    QList<int> lines;
    QStringList texts;

    int indexOfLine(int line) const
    {
        // Lines are ascending; a binary search keeps large diffs cheap.
        int lo = 0, hi = lines.size() - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (lines.at(mid) == line)
                return mid;
            if (lines.at(mid) < line)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return -1;
    }
};

QString sideKey(const QString &path, const QString &side)
{
    return path + QLatin1Char('\x1f') + side;
}

// Indexes every file and side of `patch` in one pass over the text.
QHash<QString, DiffSideIndex> indexDiffSides(const QString &patch)
{
    QHash<QString, DiffSideIndex> index;
    static const QRegularExpression hunkRe(
        QStringLiteral("^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@"));
    const QStringView text(patch);
    QString path;
    DiffSideIndex *oldSide = nullptr;
    DiffSideIndex *newSide = nullptr;
    bool inHunk = false;
    bool binary = false;
    int oldNo = 0, newNo = 0;
    for (qsizetype start = 0; start <= text.size();) {
        const qsizetype newline = text.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? text.size() : newline;
        QStringView line = text.sliced(start, end - start);
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (line.startsWith(QLatin1String("diff --git "))) {
            const qsizetype marker = line.indexOf(QLatin1String(" b/"));
            path = marker < 0 ? QString() : line.sliced(marker + 3).toString();
            oldSide = path.isEmpty() ? nullptr
                                     : &index[sideKey(path, QStringLiteral("old"))];
            newSide = path.isEmpty() ? nullptr
                                     : &index[sideKey(path, QStringLiteral("new"))];
            inHunk = false;
            binary = false;
        } else if (!path.isEmpty() && !binary) {
            if (line.startsWith(QLatin1String("@@"))) {
                const QRegularExpressionMatch m = hunkRe.matchView(line);
                if (m.hasMatch()) {
                    oldNo = m.captured(1).toInt();
                    newNo = m.captured(2).toInt();
                    inHunk = true;
                }
            } else if (line.startsWith(QLatin1String("GIT binary patch")) ||
                       line.startsWith(QLatin1String("Binary files "))) {
                binary = true;
                inHunk = false;
            } else if (inHunk && !line.isEmpty()) {
                const QChar marker = line.at(0);
                if (marker == u'+' && !line.startsWith(QLatin1String("+++ "))) {
                    newSide->lines.append(newNo++);
                    newSide->texts.append(line.sliced(1).toString());
                } else if (marker == u'-' &&
                           !line.startsWith(QLatin1String("--- "))) {
                    oldSide->lines.append(oldNo++);
                    oldSide->texts.append(line.sliced(1).toString());
                } else if (marker == u' ') {
                    const QString ctx = line.sliced(1).toString();
                    oldSide->lines.append(oldNo++);
                    oldSide->texts.append(ctx);
                    newSide->lines.append(newNo++);
                    newSide->texts.append(ctx);
                } else if (marker != u'\\') {
                    // "diff --git" already handled; anything else (index,
                    // mode, rename lines) ends the hunk.
                    inHunk = false;
                }
            } else if (inHunk && line.isEmpty()) {
                // An empty context line lost its leading space (some tools
                // trim it); it is still one line on both sides.
                oldSide->lines.append(oldNo++);
                oldSide->texts.append(QString());
                newSide->lines.append(newNo++);
                newSide->texts.append(QString());
            }
        }
        if (newline < 0)
            break;
        start = newline + 1;
    }
    return index;
}

QString anchorLinesFrom(const DiffSideIndex &side, int lineStart, int lineEnd)
{
    if (lineStart <= 0)
        return QString();
    if (lineEnd < lineStart)
        lineEnd = lineStart;
    const int first = side.indexOfLine(lineStart);
    if (first < 0)
        return QString();
    QStringList out;
    const int wanted = qMin(lineEnd - lineStart + 1, kPullAnchorMaxLines);
    for (int i = 0; i < wanted; ++i) {
        const int at = first + i;
        if (at >= side.lines.size() || side.lines.at(at) != lineStart + i)
            break; // the range crossed into a gap between hunks
        out.append(side.texts.at(at));
    }
    return out.join(u'\n');
}

// Every line number where `needle` (newline-joined) occurs as a run of
// consecutive lines in `side`, nearest to `preferred` first.
int nearestAnchorMatch(const DiffSideIndex &side, const QString &needle,
                       int preferred)
{
    const QStringList want = needle.split(u'\n');
    if (want.isEmpty())
        return -1;
    int best = -1;
    for (int i = 0; i + want.size() <= side.texts.size(); ++i) {
        if (side.texts.at(i) != want.at(0))
            continue;
        bool ok = true;
        for (int k = 1; k < want.size(); ++k) {
            if (side.texts.at(i + k) != want.at(k) ||
                side.lines.at(i + k) != side.lines.at(i) + k) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        const int line = side.lines.at(i);
        if (best < 0 || std::abs(line - preferred) < std::abs(best - preferred))
            best = line;
        if (line >= preferred)
            break; // matches only get farther from here on
    }
    return best;
}

void placeWithIndex(PullReviewThread &thread, const DiffSideIndex *side,
                    const QString &currentPatchset)
{
    thread.currentLineStart = 0;
    thread.currentLineEnd = 0;
    if (thread.path.isEmpty() || thread.lineStart <= 0) {
        thread.placement = PullThreadPlacement::Legacy;
        return;
    }
    const int span = qMax(0, thread.lineEnd - thread.lineStart);
    const auto placeAt = [&](PullThreadPlacement placement, int start) {
        thread.placement = placement;
        thread.currentLineStart = start;
        thread.currentLineEnd = start + span;
    };
    const bool lineExists = side && side->indexOfLine(thread.lineStart) >= 0;

    if (thread.patchset.isEmpty() && thread.anchorLines.isEmpty()) {
        // Pre-anchor thread: paint it where it says while that line exists.
        if (lineExists)
            placeAt(PullThreadPlacement::Legacy, thread.lineStart);
        else
            thread.placement = PullThreadPlacement::Outdated;
        return;
    }
    if (!thread.patchset.isEmpty() && !currentPatchset.isEmpty() &&
        thread.patchset.compare(currentPatchset, Qt::CaseInsensitive) == 0) {
        placeAt(PullThreadPlacement::Current, thread.lineStart);
        return;
    }
    if (!thread.anchorLines.isEmpty()) {
        const int at = side ? nearestAnchorMatch(*side, thread.anchorLines,
                                                 thread.lineStart)
                            : -1;
        if (at < 0)
            thread.placement = PullThreadPlacement::Outdated;
        else
            placeAt(at == thread.lineStart ? PullThreadPlacement::Current
                                           : PullThreadPlacement::Moved,
                    at);
        return;
    }
    // A patchset but no text: only an unnamed current patchset (an immutable
    // patch-backed pull) lets the line stand on its own.
    if (currentPatchset.isEmpty() && lineExists)
        placeAt(PullThreadPlacement::Current, thread.lineStart);
    else
        thread.placement = PullThreadPlacement::Outdated;
}

} // namespace

QString pullPatchsetId(const PullRequest &pr)
{
    return pr.patchHeadOid.isEmpty() ? pr.creationHeadOid : pr.patchHeadOid;
}

QString pullAnchorLines(const QString &patch, const QString &path,
                        const QString &side, int lineStart, int lineEnd)
{
    if (path.isEmpty() || lineStart <= 0)
        return QString();
    const QString wantSide =
        side == QLatin1String("old") ? QStringLiteral("old") : QStringLiteral("new");
    const QHash<QString, DiffSideIndex> index = indexDiffSides(patch);
    const auto it = index.constFind(sideKey(path, wantSide));
    if (it == index.constEnd())
        return QString();
    return anchorLinesFrom(it.value(), lineStart, lineEnd);
}

PullAnchor pullAnchorFor(const PullRequest &pr, const QString &path,
                         const QString &side, int lineStart, int lineEnd)
{
    PullAnchor anchor;
    anchor.patchset = pullPatchsetId(pr);
    anchor.anchorLines = pullAnchorLines(pr.patch, path, side, lineStart, lineEnd);
    return anchor;
}

void placePullReviewThread(PullReviewThread &thread, const QString &patch,
                           const QString &currentPatchset)
{
    const QHash<QString, DiffSideIndex> index = indexDiffSides(patch);
    const QString side =
        thread.side == QLatin1String("old") ? QStringLiteral("old")
                                            : QStringLiteral("new");
    const auto it = index.constFind(sideKey(thread.path, side));
    placeWithIndex(thread, it == index.constEnd() ? nullptr : &it.value(),
                   currentPatchset);
}

QString pullThreadPlacementLabel(const PullReviewThread &thread)
{
    switch (thread.placement) {
    case PullThreadPlacement::Outdated:
        return QStringLiteral("Outdated");
    case PullThreadPlacement::Moved:
        return thread.lineEnd > thread.lineStart
                   ? QStringLiteral("Moved from lines %1–%2")
                         .arg(thread.lineStart)
                         .arg(thread.lineEnd)
                   : QStringLiteral("Moved from line %1").arg(thread.lineStart);
    case PullThreadPlacement::Current:
    case PullThreadPlacement::Legacy:
        break;
    }
    return QString();
}

PullReviewSnapshot buildPullReviewSnapshot(const PullRequest &pr)
{
    PullReviewSnapshot snapshot;
    snapshot.reviewSummary = pr.reviewSummary();

    QHash<QString, int> indexById;
    for (int i = 0; i < pr.events.size(); ++i) {
        const PullEvent &ev = pr.events.at(i);
        if (ev.type == QLatin1String("comment") ||
            ev.type == QLatin1String("review")) {
            ++snapshot.topLevelItems;
            continue;
        }

        if (ev.type == QLatin1String("line-comment")) {
            const int index =
                ensureThread(snapshot.threads, indexById, legacyThreadId(ev, i));
            PullReviewThread &thread = snapshot.threads[index];
            copyAnchor(thread, ev);
            thread.events.append(ev);
            continue;
        }

        if (ev.type == QLatin1String("thread-comment")) {
            const int index =
                ensureThread(snapshot.threads, indexById, ev.threadId);
            PullReviewThread &thread = snapshot.threads[index];
            copyAnchor(thread, ev);
            if (!ev.suggestionPatch.isEmpty())
                thread.hasSuggestion = true;
            thread.events.append(ev);
            continue;
        }

        if (ev.type == QLatin1String("thread-reply")) {
            const int index =
                ensureThread(snapshot.threads, indexById, ev.threadId);
            snapshot.threads[index].events.append(ev);
            continue;
        }

        if (ev.type == QLatin1String("thread-state")) {
            const int index =
                ensureThread(snapshot.threads, indexById, ev.threadId);
            PullReviewThread &thread = snapshot.threads[index];
            thread.resolved = ev.state == QLatin1String("resolved");
            thread.events.append(ev);
            continue;
        }

        if (ev.type == QLatin1String("suggestion-state")) {
            const int index =
                ensureThread(snapshot.threads, indexById, ev.threadId);
            PullReviewThread &thread = snapshot.threads[index];
            thread.suggestionState = ev.state;
            thread.appliedCommit = ev.appliedCommit;
            thread.events.append(ev);
            continue;
        }
    }

    // Place every line-anchored thread against the diff being shown. The
    // patch is indexed once, whatever the thread count.
    const QString currentPatchset = pullPatchsetId(pr);
    QHash<QString, DiffSideIndex> index;
    bool indexed = false;
    for (PullReviewThread &thread : snapshot.threads) {
        if (thread.path.isEmpty() || thread.lineStart <= 0) {
            thread.placement = PullThreadPlacement::Legacy;
            continue;
        }
        if (!indexed) {
            index = indexDiffSides(pr.patch);
            indexed = true;
        }
        const QString side =
            thread.side == QLatin1String("old") ? QStringLiteral("old")
                                                : QStringLiteral("new");
        const auto it = index.constFind(sideKey(thread.path, side));
        placeWithIndex(thread, it == index.constEnd() ? nullptr : &it.value(),
                       currentPatchset);
    }

    snapshot.totalThreads = snapshot.threads.size();
    for (const PullReviewThread &thread : std::as_const(snapshot.threads)) {
        if (thread.resolved)
            ++snapshot.resolvedThreads;
        else
            ++snapshot.unresolvedThreads;
        if (thread.hasSuggestion)
            ++snapshot.suggestions;
        if (thread.outdated())
            ++snapshot.outdatedThreads;
        if (thread.path.isEmpty())
            continue;
        PullReviewFileSummary summary = snapshot.files.value(thread.path);
        summary.path = thread.path;
        ++summary.totalThreads;
        if (thread.resolved)
            ++summary.resolvedThreads;
        else
            ++summary.unresolvedThreads;
        if (thread.hasSuggestion)
            ++summary.suggestions;
        if (thread.outdated())
            ++summary.outdatedThreads;
        snapshot.files.insert(thread.path, summary);
    }

    return snapshot;
}
