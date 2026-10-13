#include "CodeHealth.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <algorithm>

#if defined(__GNUC__) && !defined(_MSC_VER)
#include <cxxabi.h>
#include <cstdlib>
#endif

namespace forkmesh::codehealth {
namespace {

// A loop head. Kept deliberately narrow: `for (` / `while (` only, so a call
// named "forEach" or a comment never opens a phantom loop body.
const QRegularExpression &loopHead()
{
    static const QRegularExpression re(QStringLiteral("\\b(?:for|while)\\s*\\("));
    return re;
}

struct HazardRule {
    const char *kind;
    const char *pattern;
    const char *why;
};

// Only classes that have actually stalled this app. Order is the order a
// person should read them in.
const QVector<HazardRule> &hazardRules()
{
    static const QVector<HazardRule> rules = {
        {"git-in-loop",
         "\\b(?:runGit|waitForGit|waitForFinishedKeepAlive)\\s*\\(",
         "waits on a subprocess once per iteration"},
        {"stylesheet-in-loop", "\\bsetStyleSheet\\s*\\(",
         "re-runs the QSS selector match per widget; hoist the rule to the "
         "parent and let it cascade"},
        {"settings-in-loop", "\\bQSettings\\s+\\w+\\s*[({]",
         "constructs QSettings per iteration, reparsing the whole INI"},
        // A `static` local compiles once, so a line declaring one is skipped.
        {"regex-in-loop",
         "(?m)^(?![^\\n]*\\bstatic\\b)[^\\n]*\\bQRegularExpression\\s+\\w+\\s*\\(",
         "compiles a regular expression per iteration; make it static"},
        {"pump-in-loop", "\\b(?:processEvents|pumpKeepAlive)\\s*\\(",
         "pumps the event loop per iteration, which can re-enter the caller"},
        {"dir-walk-in-loop",
         "\\b(?:QDirIterator|entryList|entryInfoList|removeRecursively)\\s*[({]",
         "walks the filesystem per iteration"},
        {"raster-in-loop",
         "\\b(?:QPainter\\s+\\w+|QImageReader\\s+\\w+|QFontMetrics\\s+\\w+)",
         "rasterises per iteration instead of caching by key"},
    };
    return rules;
}

const QVector<QRegularExpression> &hazardExpressions()
{
    static const QVector<QRegularExpression> compiled = [] {
        QVector<QRegularExpression> out;
        for (const HazardRule &rule : hazardRules())
            out.append(QRegularExpression(QString::fromLatin1(rule.pattern)));
        return out;
    }();
    return compiled;
}

const QRegularExpression &blockingWait()
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(?:waitForGit|waitForFinished|pumpKeepAlive|processEvents)\\s*\\("));
    return re;
}

// A definition, not a declaration and not a call. Both patterns are anchored
// at column 0 on purpose: every out-of-class definition in this codebase
// starts there, while an indented `QTimer::singleShot(0, this, [] {` — which
// otherwise looks identical — does not, so anchoring is what keeps library
// calls with trailing lambdas out of the report.
const QRegularExpression &definitionHead()
{
    static const QRegularExpression re(QStringLiteral(
        // Qualified: an optional return type then Class::method(. The type may
        // end in `*` or `&` with no space, as in `QWidget *MainWindow::build(`;
        // requiring whitespace there once hid every pointer-returning builder,
        // some of them a thousand lines long.
        "^(?:[A-Za-z_][\\w:<>,&*\\s]*?[\\s*&])?"
        "([A-Za-z_]\\w*(?:::[A-Za-z_~]\\w*)+)\\s*\\("));
    return re;
}

// Free function: a return type is REQUIRED, which is what separates
// "bool isBlank(const QString &line)" from a bare call at column 0. It may take
// several words ("static const QString &label(").
const QRegularExpression &freeDefinitionHead()
{
    static const QRegularExpression re(QStringLiteral(
        "^(?:[A-Za-z_][\\w:<>,&*]*\\s+)*[A-Za-z_][\\w:<>,&*]*[\\w>&*]\\s*"
        "(?<=[\\s*&])[*&]?([A-Za-z_]\\w*)\\s*\\("));
    return re;
}

const QRegularExpression &statementHead()
{
    static const QRegularExpression re(QStringLiteral(
        "^(?:if|for|while|switch|catch|return|else|do|case|default)\\b"));
    return re;
}

bool isBlank(const QString &line)
{
    for (QChar ch : line) {
        if (!ch.isSpace())
            return false;
    }
    return true;
}

int braceDelta(const QString &line)
{
    int delta = 0;
    for (QChar ch : line) {
        if (ch == QLatin1Char('{'))
            ++delta;
        else if (ch == QLatin1Char('}'))
            --delta;
    }
    return delta;
}

// Blank every line compiled only into the window-test binary. Those `test…()`
// hooks pump the event loop on purpose to drive the UI from a test, and the
// shipped app never runs them, so their waits are not stalls anyone can hit.
// Line count is preserved; an `#else` branch is kept because it is the app's.
void blankWindowTestOnly(QStringList &lines)
{
    // One entry per open #if: whether that level is test-only right now.
    QVector<bool> stack;
    int testOnly = 0;
    for (QString &line : lines) {
        const QString directive = line.trimmed();
        if (directive.startsWith(QLatin1Char('#'))) {
            QString word = directive.mid(1).trimmed();
            if (word.startsWith(QLatin1String("if"))) {
                const bool tests =
                    word.startsWith(QLatin1String("ifdef")) &&
                    word.mid(5).trimmed() ==
                        QLatin1String("FORKMESH_WINDOW_TESTS");
                stack.append(tests);
                testOnly += tests ? 1 : 0;
            } else if (word.startsWith(QLatin1String("el")) && !stack.isEmpty()) {
                if (stack.last()) {
                    stack.last() = false;
                    --testOnly;
                }
            } else if (word.startsWith(QLatin1String("endif")) &&
                       !stack.isEmpty()) {
                testOnly -= stack.takeLast() ? 1 : 0;
            }
            continue;
        }
        if (testOnly > 0)
            line.fill(QLatin1Char(' '));
    }
}

struct LoopSpan {
    int head = 0;    // offset of `for` / `while`
    int repeats = 0; // where the per-iteration part starts
    int end = 0;     // offset one past the last character of the body
};

// Offset one past the statement that starts at `from`. A braced block ends at
// its matching brace; anything else at the first ';' outside parentheses and
// braces, so `for (...) x;` ends there instead of running on to whatever block
// closes next — which is what used to swallow sibling loops and later calls.
int statementEnd(const QString &text, int from)
{
    const int n = text.size();
    int i = from;
    while (i < n && text.at(i).isSpace())
        ++i;
    int parens = 0;
    int braces = 0;
    for (; i < n; ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('('))
            ++parens;
        else if (ch == QLatin1Char(')'))
            --parens;
        else if (ch == QLatin1Char('{'))
            ++braces;
        else if (ch == QLatin1Char('}')) {
            if (--braces < 0)
                return i; // the enclosing block closed first
            if (braces == 0 && parens == 0) {
                // `if (...) { } else { }` continues past the first block.
                int k = i + 1;
                while (k < n && text.at(k).isSpace())
                    ++k;
                if (QStringView(text).mid(k).startsWith(QLatin1String("else")))
                    continue;
                // A braced statement ends here unless a ';' finishes it, as
                // in `x = [] { ... };`.
                if (k < n && text.at(k) == QLatin1Char(';'))
                    return k + 1;
                return i + 1;
            }
        } else if (ch == QLatin1Char(';') && parens <= 0 && braces == 0) {
            return i + 1;
        }
    }
    return n;
}

QVector<LoopSpan> loopSpans(const QString &text)
{
    QVector<LoopSpan> spans;
    auto heads = loopHead().globalMatch(text);
    while (heads.hasNext()) {
        const QRegularExpressionMatch head = heads.next();
        int i = head.capturedEnd() - 1; // the '(' of the loop header
        int parens = 0;
        // A range-for header (a lone ':' and no ';') is evaluated once, so
        // `for (auto &e : dir.entryList())` lists the directory once. A
        // classic `for` or `while` header re-runs its condition every pass.
        bool semicolon = false;
        bool rangeColon = false;
        for (; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (ch == QLatin1Char('('))
                ++parens;
            else if (ch == QLatin1Char(')') && --parens == 0)
                break;
            else if (parens == 1 && ch == QLatin1Char(';'))
                semicolon = true;
            else if (parens == 1 && ch == QLatin1Char(':') &&
                     text.at(i - 1) != QLatin1Char(':') &&
                     (i + 1 >= text.size() || text.at(i + 1) != QLatin1Char(':')))
                rangeColon = true;
        }
        const bool rangeFor = head.captured(0).startsWith(QLatin1String("for")) &&
                              rangeColon && !semicolon;
        const int start = static_cast<int>(head.capturedStart());
        spans.append({start, rangeFor ? std::min<int>(i + 1, text.size()) : start,
                      statementEnd(text, i + 1)});
    }
    return spans;
}

} // namespace

QString stripNoise(const QString &text)
{
    // code-health: allow(deep-nested-loop) the inner loops only advance
    // one pass over each line's characters.
    // Line-oriented on purpose. A character-stream version let an unterminated
    // literal run past the newline, which deleted lines outright and threw
    // every reported line number off by hundreds.
    QStringList out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    out.reserve(lines.size());
    bool inBlockComment = false;
    for (const QString &line : lines) {
        QString result;
        result.reserve(line.size());
        const int n = line.size();
        int i = 0;
        while (i < n) {
            const QChar ch = line.at(i);
            if (inBlockComment) {
                if (ch == QLatin1Char('*') && i + 1 < n &&
                    line.at(i + 1) == QLatin1Char('/')) {
                    inBlockComment = false;
                    result.append(QLatin1String("  "));
                    i += 2;
                } else {
                    result.append(QLatin1Char(' '));
                    ++i;
                }
                continue;
            }
            if (ch == QLatin1Char('/') && i + 1 < n) {
                const QChar next = line.at(i + 1);
                if (next == QLatin1Char('/')) {
                    result.append(QString(n - i, QLatin1Char(' ')));
                    i = n;
                    continue;
                }
                if (next == QLatin1Char('*')) {
                    inBlockComment = true;
                    result.append(QLatin1String("  "));
                    i += 2;
                    continue;
                }
            }
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
                const QChar quote = ch;
                int j = i + 1;
                while (j < n && line.at(j) != quote)
                    j += (line.at(j) == QLatin1Char('\\')) ? 2 : 1;
                // An unterminated literal ends at the newline, never past it.
                j = std::min(j + 1, n);
                result.append(QString(j - i, QLatin1Char(' ')));
                i = j;
                continue;
            }
            result.append(ch);
            ++i;
        }
        out.append(result);
    }
    return out.join(QLatin1Char('\n'));
}

QStringList acknowledgedKinds(const QStringList &rawLines, int first, int last)
{
    static const QRegularExpression marker(QStringLiteral(
        "code-health:\\s*allow\\(\\s*([a-z-]+(?:\\s*,\\s*[a-z-]+)*)\\s*\\)"
        "\\s*\\S"));
    // Take in the comment block sitting directly above the definition.
    int from = std::clamp(first, 0, static_cast<int>(rawLines.size()));
    while (from > 0) {
        const QString above = rawLines.at(from - 1).trimmed();
        if (!above.startsWith(QLatin1String("//")) &&
            !above.startsWith(QLatin1Char('*')) &&
            !above.startsWith(QLatin1String("/*")))
            break;
        --from;
    }
    const int to = std::min<int>(last, rawLines.size());
    QStringList kinds;
    for (int i = from; i < to; ++i) {
        const QString &line = rawLines.at(i);
        if (!line.contains(QLatin1String("code-health:")))
            continue;
        const QRegularExpressionMatch match = marker.match(line);
        if (!match.hasMatch())
            continue;
        const QStringList named =
            match.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &kind : named)
            kinds.append(kind.trimmed());
    }
    return kinds;
}

QVector<Function> scanFunctions(const QString &clean)
{
    QVector<Function> functions;
    const QStringList lines = clean.split(QLatin1Char('\n'));
    int i = 0;
    while (i < lines.size()) {
        const QString &line = lines.at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) ||
            trimmed.startsWith(QLatin1Char('}')) ||
            statementHead().match(trimmed).hasMatch()) {
            ++i;
            continue;
        }
        QRegularExpressionMatch head = definitionHead().match(line);
        if (!head.hasMatch())
            head = freeDefinitionHead().match(line);
        if (!head.hasMatch()) {
            ++i;
            continue;
        }
        // Find the opening brace, allowing a signature spread over a few
        // lines. A ';' first means it was only a declaration.
        int open = -1;
        for (int j = i; j < lines.size() && j < i + 12; ++j) {
            if (lines.at(j).contains(QLatin1Char('{'))) {
                open = j;
                break;
            }
            if (lines.at(j).contains(QLatin1Char(';')))
                break;
        }
        if (open < 0) {
            ++i;
            continue;
        }
        int depth = 0;
        int end = open;
        for (int k = open; k < lines.size(); ++k) {
            depth += braceDelta(lines.at(k));
            end = k;
            if (depth <= 0)
                break;
        }
        if (end <= i) {
            i = open + 1;
            continue;
        }
        Function fn;
        fn.name = head.captured(1);
        fn.line = i + 1;
        fn.loc = end - i + 1;
        functions.append(fn);
        i = end + 1;
    }
    return functions;
}

QString componentFor(const QString &relativePath)
{
    const QString base = QFileInfo(relativePath).fileName();
    QString stem = base.section(QLatin1Char('.'), 0, 0);
    if (stem.startsWith(QLatin1String("MainWindow")) &&
        stem != QLatin1String("MainWindow")) {
        QString tail = stem.mid(QStringLiteral("MainWindow").size());
        // "RepoDetail" -> "Repo Detail", so the card label reads as words.
        QString spaced;
        for (int i = 0; i < tail.size(); ++i) {
            if (i > 0 && tail.at(i).isUpper() && !tail.at(i - 1).isUpper())
                spaced.append(QLatin1Char(' '));
            spaced.append(tail.at(i));
        }
        return QStringLiteral("UI · ") + spaced;
    }
    static const QVector<QPair<QString, QString>> prefixes = {
        {QStringLiteral("Agent"), QStringLiteral("Agents")},
        {QStringLiteral("Chat"), QStringLiteral("Chat")},
        {QStringLiteral("Pull"), QStringLiteral("Pulls")},
        {QStringLiteral("Issue"), QStringLiteral("Issues")},
        {QStringLiteral("Mirror"), QStringLiteral("Mirrors")},
        {QStringLiteral("Node"), QStringLiteral("Mesh")},
        {QStringLiteral("Local"), QStringLiteral("Mesh")},
        {QStringLiteral("Server"), QStringLiteral("Mesh")},
        {QStringLiteral("Stats"), QStringLiteral("Stats")},
        {QStringLiteral("Stall"), QStringLiteral("Diagnostics")},
        {QStringLiteral("Qt"), QStringLiteral("Diagnostics")},
        {QStringLiteral("Repo"), QStringLiteral("Repositories")},
        {QStringLiteral("Release"), QStringLiteral("Releases")},
        {QStringLiteral("Action"), QStringLiteral("Actions")},
        {QStringLiteral("Room"), QStringLiteral("Crypto")},
        {QStringLiteral("Crypto"), QStringLiteral("Crypto")},
        {QStringLiteral("Terminal"), QStringLiteral("Terminal")},
        {QStringLiteral("Diff"), QStringLiteral("Diff")},
        {QStringLiteral("Code"), QStringLiteral("Diagnostics")},
    };
    for (const auto &entry : prefixes) {
        if (stem.startsWith(entry.first))
            return entry.second;
    }
    return QStringLiteral("Core");
}

int scoreFile(const FileHealth &file)
{
    int points = 100;
    points -= std::min(30, file.findingCount * 3);
    points -= std::min(30, static_cast<int>(file.stallMs / 1000));
    if (file.longestFunction > 400)
        points -= 15;
    else if (file.longestFunction > 200)
        points -= 8;
    if (file.loc > 8000)
        points -= 10;
    else if (file.loc > 3000)
        points -= 5;
    return std::max(0, points);
}

namespace {

// --- runtime attribution -------------------------------------------------

QString demangle(const QString &symbol)
{
#if defined(__GNUC__) && !defined(_MSC_VER)
    int status = 0;
    char *out = abi::__cxa_demangle(symbol.toUtf8().constData(), nullptr,
                                    nullptr, &status);
    if (status == 0 && out) {
        const QString result = QString::fromUtf8(out);
        std::free(out);
        return result;
    }
    if (out)
        std::free(out);
#endif
    return symbol;
}

// One stall record: how long the UI was blocked and every qualified name its
// stack samples mentioned.
struct StallRecord {
    int ms = 0;
    // The watchdog's own verdict that the whole machine was paging: the frames
    // show where the GUI thread happened to wait, not code at fault.
    bool starved = false;
    QSet<QString> names;
};

// Only a stack frame or the watchdog's "likely origin:" summary names code.
// Everything else in a record is prose, and prose parenthesises ordinary
// words: "git rev-parse main (forkmesh)" once blamed main() for 23 s of stalls
// it never ran.
bool namesFrames(const QString &line)
{
    return line.startsWith(QLatin1String("likely origin:")) ||
           line.contains(QLatin1String("+0x")) ||
           line.contains(QLatin1String("[0x"));
}

// Pull "Class::method" or "freeFunction" out of a demangled or plain frame.
const QRegularExpression &qualifiedName()
{
    static const QRegularExpression re(
        QStringLiteral("\\b([A-Za-z_]\\w*(?:::[A-Za-z_~]\\w*)*)\\s*\\("));
    return re;
}

bool isDispatchResidue(const QString &name)
{
    return name.endsWith(QLatin1String("::eventFilter")) ||
           name == QLatin1String("MainWindow::handleFramelessResizeEvent") ||
           name == QLatin1String("MainWindow::resizeEdgesAtGlobalPos") ||
           name == QLatin1String("MainWindow::updateFramelessResizeCursor");
}

void noteFrameName(StallRecord &record, const QString &name)
{
    if (!isDispatchResidue(name))
        record.names.insert(name);
}

QVector<StallRecord> readStallRecords(const QString &stallsPath)
{
    // code-health: allow(deep-nested-loop) one pass over the log; the inner
    // loops walk a single line's regex hits.
    QVector<StallRecord> records;
    QFile file(stallsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return records;
    static const QRegularExpression stalledRe(
        QStringLiteral("UI stalled ~(\\d+) ms"));
    static const QRegularExpression mangledRe(
        QStringLiteral("\\b(_Z[A-Za-z0-9_]+)"));

    QTextStream stream(&file);
    StallRecord current;
    bool inRecord = false;
    auto flush = [&] {
        if (inRecord)
            records.append(current);
        current = StallRecord{};
    };
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.startsWith(QLatin1String("===== STALL-")) &&
            line.contains(QLatin1String("BEGIN"))) {
            flush();
            inRecord = true;
            continue;
        }
        if (!inRecord)
            continue;
        const QRegularExpressionMatch stalled = stalledRe.match(line);
        if (stalled.hasMatch() && current.ms == 0)
            current.ms = stalled.captured(1).toInt();
        if (line.startsWith(QLatin1String(
                "verdict: machine-wide memory/io starvation")))
            current.starved = true;
        if (!namesFrames(line))
            continue;
        auto plain = qualifiedName().globalMatch(line);
        while (plain.hasNext())
            noteFrameName(current, plain.next().captured(1));
        auto mangled = mangledRe.globalMatch(line);
        while (mangled.hasNext()) {
            const QString sym = demangle(mangled.next().captured(1));
            auto hits = qualifiedName().globalMatch(sym + QLatin1Char('('));
            while (hits.hasNext())
                noteFrameName(current, hits.next().captured(1));
        }
    }
    flush();
    return records;
}

} // namespace

Report analyse(const QString &root, const QStringList &dirs,
               const QString &diagnosticsDir, const ProgressFn &progress)
{
    // code-health: allow(deep-nested-loop, dir-walk-in-loop) runs on
    // runOffThread from refreshComponentHealth; one listing per scanned dir.
    QElapsedTimer timer;
    timer.start();
    Report report;

    QStringList paths;
    for (const QString &dir : dirs) {
        QDir base(QDir(root).filePath(dir));
        if (!base.exists())
            continue;
        const QStringList names =
            base.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                           QDir::Files, QDir::Name);
        for (const QString &name : names)
            paths.append(base.filePath(name));
    }

    // Read the stall records now, but attribute them only once the scan knows
    // which names this codebase actually defines. Guessing from a name prefix
    // both invented owners for Qt frames and disowned real classes whose names
    // merely start with Q (QrCode, QtRuntimeDiagnostics).
    const QVector<StallRecord> stallRecords =
        diagnosticsDir.isEmpty()
            ? QVector<StallRecord>{}
            : readStallRecords(QDir(diagnosticsDir).filePath(
                  QStringLiteral("stalls.log")));

    const QDir rootDir(root);
    const QVector<QRegularExpression> &hazards = hazardExpressions();
    const QVector<HazardRule> &rules = hazardRules();

    for (int index = 0; index < paths.size(); ++index) {
        const QString &path = paths.at(index);
        if (progress && !progress(index, paths.size(),
                                  QFileInfo(path).fileName())) {
            report.cancelled = true;
            break;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString raw = QString::fromUtf8(file.readAll());
        file.close();
        QStringList cleanLines = stripNoise(raw).split(QLatin1Char('\n'));
        blankWindowTestOnly(cleanLines);
        const QString clean = cleanLines.join(QLatin1Char('\n'));

        const QStringList rawLines = raw.split(QLatin1Char('\n'));

        FileHealth entry;
        entry.path = rootDir.relativeFilePath(path);
        entry.display = QFileInfo(path).fileName();
        entry.component = componentFor(entry.path);
        entry.modifiedMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        entry.loc = cleanLines.size();
        for (const QString &line : cleanLines) {
            if (!isBlank(line))
                ++entry.codeLoc;
        }

        entry.functions = scanFunctions(clean);
        for (Function &fn : entry.functions) {
            const int first = fn.line - 1;
            const int last =
                std::min<int>(first + fn.loc, cleanLines.size());
            const QStringList body = cleanLines.mid(first, last - first);
            QSet<QString> seen;
            const QString bodyText = body.join(QLatin1Char('\n'));
            const QVector<LoopSpan> spans = loopSpans(bodyText);
            int lineOffset = 0;
            int lineAt = 0;
            for (int s = 0; s < spans.size(); ++s) {
                const LoopSpan &span = spans.at(s);
                // Depth is how many loops enclose this head, counting itself;
                // sibling loops inside one body no longer add up.
                int nesting = 1;
                for (int o = 0; o < s; ++o) {
                    if (spans.at(o).end > span.head)
                        ++nesting;
                }
                fn.loopDepth = std::max(fn.loopDepth, nesting);
                for (; lineOffset < span.head; ++lineOffset) {
                    if (bodyText.at(lineOffset) == QLatin1Char('\n'))
                        ++lineAt;
                }
                const QString text =
                    bodyText.mid(span.repeats, span.end - span.repeats);
                for (int r = 0; r < hazards.size(); ++r) {
                    if (!hazards.at(r).match(text).hasMatch())
                        continue;
                    const QString kind = QString::fromLatin1(rules.at(r).kind);
                    if (seen.contains(kind))
                        continue;
                    seen.insert(kind);
                    fn.findings.append({kind,
                                        QString::fromLatin1(rules.at(r).why),
                                        fn.line + lineAt});
                }
            }
            if (fn.loopDepth >= 3 && !seen.contains(QStringLiteral("deep-nested-loop"))) {
                fn.findings.append(
                    {QStringLiteral("deep-nested-loop"),
                     QStringLiteral("%1 nested loop levels").arg(fn.loopDepth),
                     fn.line});
            }
            if (blockingWait().match(bodyText).hasMatch()) {
                fn.findings.append(
                    {QStringLiteral("blocking-wait"),
                     QStringLiteral("blocks the caller on a worker or the "
                                    "event loop"),
                     fn.line});
            }
            if (!fn.findings.isEmpty()) {
                const QStringList allowed =
                    acknowledgedKinds(rawLines, fn.line - 1, last);
                if (!allowed.isEmpty()) {
                    const auto before = fn.findings.size();
                    fn.findings.removeIf([&](const Finding &finding) {
                        return allowed.contains(finding.kind);
                    });
                    entry.acknowledgedCount +=
                        static_cast<int>(before - fn.findings.size());
                }
            }
            entry.findingCount += fn.findings.size();
            for (const Finding &finding : fn.findings)
                entry.findingKinds[finding.kind] += 1;
            entry.longestFunction = std::max(entry.longestFunction, fn.loc);
        }
        entry.functionCount = entry.functions.size();
        report.totalLoc += entry.loc;
        report.totalFunctions += entry.functionCount;
        report.acknowledgedCount += entry.acknowledgedCount;
        report.files.append(entry);
    }

    // Attribute stall milliseconds to the functions this codebase defines. A
    // record whose samples name no such function stays unattributed — most do,
    // because the sampler usually caught only Qt/libc frames, and spreading
    // that time over the code would invent blame.
    QHash<QString, QVector<QPair<int, int>>> owners; // name -> (file, function)
    for (int f = 0; f < report.files.size(); ++f) {
        const QVector<Function> &fns = report.files.at(f).functions;
        for (int k = 0; k < fns.size(); ++k)
            owners[fns.at(k).name].append({f, k});
    }
    for (const StallRecord &record : stallRecords) {
        report.stallRecords += 1;
        report.stallTotalMs += record.ms;
        if (record.starved) {
            report.stallStarvedMs += record.ms;
            continue;
        }
        QStringList known;
        for (const QString &name : record.names) {
            if (owners.contains(name))
                known.append(name);
        }
        if (known.isEmpty()) {
            report.stallUnattributedMs += record.ms;
            continue;
        }
        // Split evenly: a sample naming several of our frames gives no basis
        // for blaming one more than another.
        const int share = record.ms / known.size();
        for (const QString &name : std::as_const(known)) {
            for (const QPair<int, int> &where : owners.value(name)) {
                report.files[where.first].functions[where.second].stallMs +=
                    share;
                report.files[where.first].stallMs += share;
            }
        }
    }
    for (FileHealth &file : report.files)
        file.health = scoreFile(file);

    std::sort(report.files.begin(), report.files.end(),
              [](const FileHealth &a, const FileHealth &b) {
                  if (a.health != b.health)
                      return a.health < b.health;
                  return a.stallMs > b.stallMs;
              });

    QHash<QString, ComponentRollup> groups;
    for (const FileHealth &file : std::as_const(report.files)) {
        ComponentRollup &group = groups[file.component];
        group.component = file.component;
        group.files += 1;
        group.loc += file.loc;
        group.functions += file.functionCount;
        group.findings += file.findingCount;
        group.stallMs += file.stallMs;
    }
    for (ComponentRollup &group : groups) {
        group.health = std::max(0, 100 - std::min(40, group.findings) -
                                       std::min(40, static_cast<int>(
                                                        group.stallMs / 1000)));
        report.components.append(group);
    }
    std::sort(report.components.begin(), report.components.end(),
              [](const ComponentRollup &a, const ComponentRollup &b) {
                  return a.health < b.health;
              });

    report.scannedAt = QDateTime::currentDateTime();
    report.scanMs = timer.elapsed();
    // A cancelled scan reports no completion tick: the caller asked it to
    // stop, and a final "done" would make the progress row claim otherwise.
    if (progress && !report.cancelled)
        progress(paths.size(), paths.size(), QString());
    return report;
}

} // namespace forkmesh::codehealth
