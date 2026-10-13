#include "AgentToolImprovementLog.h"

#include "AgentToolCatalog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {

constexpr int kSchemaVersion = 1;
constexpr int kMaxDetailChars = 160;

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString tokensText(qint64 tokens)
{
    if (tokens < 10'000)
        return QString::number(tokens);
    if (tokens < 1'000'000)
        return QStringLiteral("%1K").arg(tokens / 1000.0, 0, 'f',
                                         tokens < 100'000 ? 1 : 0);
    return QStringLiteral("%1M").arg(tokens / 1'000'000.0, 0, 'f', 2);
}

QString oneLine(const QString &text, int maxChars = kMaxDetailChars)
{
    QString line;
    for (const QStringView part : QStringView(text).split(QLatin1Char('\n'))) {
        if (!part.trimmed().isEmpty()) {
            line = part.trimmed().toString();
            break;
        }
    }
    line.remove(QRegularExpression(QStringLiteral("</?tool_use_error>")));
    line = line.simplified();
    if (line.size() > maxChars)
        line = line.left(maxChars - 1) + QChar(0x2026);
    return line;
}

// Groups errors that differ only in numbers, paths, or quoted values.
QString errorSignature(const QString &line)
{
    static const QRegularExpression quoted(QStringLiteral("([\"'`]).*?\\1"));
    static const QRegularExpression path(QStringLiteral("(?:~|\\.{0,2})/[^\\s:,;)]+"));
    static const QRegularExpression digits(QStringLiteral("\\d+"));
    QString signature = line.toLower();
    signature.replace(quoted, QStringLiteral("…"));
    signature.replace(path, QStringLiteral("<path>"));
    signature.replace(digits, QStringLiteral("#"));
    return signature.left(80);
}

// What the call was about, for the example column: a workdir-relative path,
// a search pattern, the first line of a command, or a URL.
QString subjectOf(const AgentToolCallObservation &call)
{
    const QJsonObject &input = call.input;
    for (const char *key : {"file_path", "path", "notebook_path"}) {
        QString path = input.value(QLatin1String(key)).toString().trimmed();
        if (path.isEmpty())
            continue;
        if (!call.workdir.isEmpty() && QDir::isAbsolutePath(path)) {
            const QString relative = QDir(call.workdir).relativeFilePath(path);
            if (!relative.startsWith(QLatin1String("..")))
                path = relative;
        }
        if (const QString pattern = input.value(QStringLiteral("pattern")).toString();
            !pattern.isEmpty())
            return oneLine(QStringLiteral("%1 in %2").arg(pattern, path));
        return oneLine(path);
    }
    for (const char *key : {"pattern", "command", "cmd", "url", "query", "description"}) {
        const QString value = input.value(QLatin1String(key)).toString();
        if (!value.trimmed().isEmpty())
            return oneLine(value, 120);
    }
    return QString();
}

QString unwrapShellLauncher(const QString &command)
{
    static const QRegularExpression launcher(QStringLiteral(
        "^\\s*(?:\\S*/)?(?:bash|sh|zsh)\\s+-l?c\\s+(.*?)\\s*$"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto match = launcher.match(command);
    if (!match.hasMatch())
        return command;
    const QString rest = match.captured(1);
    if (rest.size() >= 2 && rest.startsWith(QLatin1Char('\'')) && rest.endsWith(QLatin1Char('\''))) {
        QString inner = rest.mid(1, rest.size() - 2);
        QString bare = inner;
        bare.remove(QStringLiteral("'\\''")).remove(QStringLiteral("'\"'\"'"));
        if (bare.contains(QLatin1Char('\'')))
            return command;
        return inner.replace(QStringLiteral("'\\''"), QStringLiteral("'"))
            .replace(QStringLiteral("'\"'\"'"), QStringLiteral("'"));
    }
    if (rest.size() >= 2 && rest.startsWith(QLatin1Char('"')) && rest.endsWith(QLatin1Char('"'))) {
        QString inner = rest.mid(1, rest.size() - 2);
        QString bare = inner;
        bare.remove(QStringLiteral("\\\\")).remove(QStringLiteral("\\\""));
        if (bare.contains(QLatin1Char('"')))
            return command;
        return inner.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    }
    if (rest.startsWith(QLatin1Char('\'')) || rest.startsWith(QLatin1Char('"')))
        return command;
    return rest;
}

QString stripHeredocs(const QString &command)
{
    QString text = command;
    // A heredoc body is data: drop it, terminator included, so a command
    // that follows the script it just wrote is still seen.
    static const QRegularExpression heredoc(
        QStringLiteral("<<-?\\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\\1"));
    for (int from = 0, guard = 0; guard < 8; ++guard) {
        const auto opener = heredoc.match(text, from);
        if (!opener.hasMatch())
            break;
        const int eol = text.indexOf(QLatin1Char('\n'), opener.capturedEnd());
        if (eol < 0) {
            text.truncate(opener.capturedEnd());
            break;
        }
        const QString terminator = QLatin1Char('\n') + opener.captured(2);
        int end = text.indexOf(terminator, eol);
        while (end >= 0 && end + terminator.size() < text.size() &&
               text.at(end + terminator.size()) != QLatin1Char('\n'))
            end = text.indexOf(terminator, end + 1);
        if (end < 0) {
            text.truncate(eol);
            break;
        }
        text.remove(eol, end + terminator.size() - eol);
        from = eol;
    }
    return text;
}

// Top-level segments: split on ';' and newlines outside quotes and
// brackets, so `python3 -c "a; b"` and `$(x; y)` stay whole.
QStringList topLevelSegments(const QString &text)
{
    QStringList segments;
    QString current;
    int depth = 0;
    QChar quote;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (!quote.isNull()) {
            current += c;
            if (c == QLatin1Char('\\') && quote == QLatin1Char('"') && i + 1 < text.size())
                current += text.at(++i);
            else if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < text.size()) {
            current += c;
            current += text.at(++i);
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            current += c;
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('{'))
            ++depth;
        else if ((c == QLatin1Char(')') || c == QLatin1Char('}')) && depth > 0)
            --depth;
        if (depth == 0 && (c == QLatin1Char(';') || c == QLatin1Char('\n'))) {
            segments << current;
            current.clear();
            continue;
        }
        current += c;
    }
    segments << current;
    return segments;
}

// The verb a shell command line is about, for grouping slow calls and
// errors. A `;` or newline chain reports its last command's status, so the
// last top-level segment is the one that failed; within it, `cd …`,
// variable assignments, `timeout N`, `env`, `time`, `nice`, `set -e`, and
// an opening subshell paren are noise, while the interpreter flags that
// tell an inline script from a module or a file are what matter.
QString commandShape(const QString &command)
{
    const QStringList segments = topLevelSegments(stripHeredocs(unwrapShellLauncher(command)));
    static const QStringList keywords = {
        QStringLiteral("do"),   QStringLiteral("done"), QStringLiteral("then"),
        QStringLiteral("else"), QStringLiteral("elif"), QStringLiteral("fi"),
        QStringLiteral("esac"), QStringLiteral("}"),    QStringLiteral(")")};
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QString chosen;
    for (int i = segments.size() - 1; i >= 0 && chosen.isEmpty(); --i) {
        const QString segment = segments.at(i).trimmed();
        if (segment.isEmpty())
            continue;
        QString firstWord = segment.section(whitespace, 0, 0);
        while (firstWord.size() > 1 && (firstWord.startsWith(QLatin1Char('(')) ||
                                        firstWord.startsWith(QLatin1Char('{'))))
            firstWord.remove(0, 1);
        if (!keywords.contains(firstWord))
            chosen = segment;
    }
    static const QRegularExpression separator(
        QStringLiteral("^(?:&&|\\|\\||;|\\|)"));
    static const QRegularExpression assignment(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*=\\S*$"));
    QStringList tokens = chosen.split(whitespace, Qt::SkipEmptyParts);
    static const QStringList setup = {
        QStringLiteral("cd"),    QStringLiteral("set"),   QStringLiteral("export"),
        QStringLiteral("pushd"), QStringLiteral("mkdir"), QStringLiteral("rm"),
        QStringLiteral("cp"),    QStringLiteral("mv"),    QStringLiteral("touch"),
        QStringLiteral("chmod"), QStringLiteral("echo"),  QStringLiteral("printf"),
        QStringLiteral("true"),  QStringLiteral("source"), QStringLiteral("."),
        QStringLiteral("ulimit"), QStringLiteral("umask"), QStringLiteral("date")};
    while (!tokens.isEmpty()) {
        QString first = tokens.first();
        while (first.startsWith(QLatin1Char('(')) || first.startsWith(QLatin1Char('{')))
            first.remove(0, 1);
        if (first.isEmpty() || separator.match(first).hasMatch()) {
            tokens.removeFirst();
            continue;
        }
        tokens.first() = first;
        if (setup.contains(first)) {
            // Setup that leads a chain is noise; alone, it is the command.
            int end = 1;
            while (end < tokens.size() &&
                   !separator.match(tokens.at(end)).hasMatch() &&
                   !tokens.at(end).endsWith(QLatin1Char(';')))
                ++end;
            if (end + 1 >= tokens.size())
                break;
            tokens.erase(tokens.begin(), tokens.begin() + end + 1);
            continue;
        }
        if (first == QLatin1String("timeout")) {
            tokens.removeFirst();
            while (!tokens.isEmpty() && tokens.first().startsWith(QLatin1Char('-'))) {
                const QString flag = tokens.takeFirst();
                if ((flag == QLatin1String("-s") || flag == QLatin1String("-k") ||
                     flag == QLatin1String("--signal") || flag == QLatin1String("--kill-after")) &&
                    !tokens.isEmpty())
                    tokens.removeFirst();
            }
            if (!tokens.isEmpty())
                tokens.removeFirst(); // the duration
            continue;
        }
        if (first == QLatin1String("env") || first == QLatin1String("sudo")) {
            tokens.removeFirst();
            while (!tokens.isEmpty() && tokens.first().startsWith(QLatin1Char('-'))) {
                const QString flag = tokens.takeFirst();
                if ((flag == QLatin1String("-u") || flag == QLatin1String("-C") ||
                     flag == QLatin1String("-g")) &&
                    !tokens.isEmpty())
                    tokens.removeFirst();
            }
            continue;
        }
        if (first == QLatin1String("nice")) {
            tokens.removeFirst();
            if (tokens.size() >= 2 && tokens.first() == QLatin1String("-n"))
                tokens.erase(tokens.begin(), tokens.begin() + 2);
            else if (!tokens.isEmpty() && tokens.first().startsWith(QLatin1String("-n")))
                tokens.removeFirst();
            continue;
        }
        if (first == QLatin1String("time") || first == QLatin1String("exec") ||
            first == QLatin1String("command") || first == QLatin1String("builtin") ||
            assignment.match(first).hasMatch()) {
            tokens.removeFirst();
            continue;
        }
        break;
    }
    if (tokens.isEmpty())
        return QString();
    QString verb = tokens.first();
    if (verb.contains(QLatin1Char('/')) && !verb.startsWith(QLatin1Char('-')))
        verb = verb.mid(verb.lastIndexOf(QLatin1Char('/')) + 1);
    const QString next = tokens.value(1);
    const auto isInlineFlag = [](const QString &flag) {
        return flag == QLatin1String("-") || flag == QLatin1String("-c") ||
               flag == QLatin1String("-e") || flag.startsWith(QLatin1String("-<<")) ||
               flag.startsWith(QLatin1String("<<")) ||
               flag == QLatin1String("--input-type=module") ||
               flag == QLatin1String("--eval") || flag == QLatin1String("-p");
    };
    static const QRegularExpression interpreter(
        QStringLiteral("^(python[0-9.]*|node|ruby|perl|bash|sh|zsh|dart)$"));
    if (interpreter.match(verb).hasMatch()) {
        if (verb.startsWith(QLatin1String("python")))
            verb = QStringLiteral("python3");
        if (next.isEmpty() || isInlineFlag(next))
            return verb + QStringLiteral(" inline script");
        if (next == QLatin1String("-m")) {
            const QString module = tokens.value(2);
            if (module == QLatin1String("pytest"))
                return QStringLiteral("pytest");
            return module.isEmpty() ? verb : verb + QStringLiteral(" -m ") + module;
        }
        if (next.startsWith(QLatin1Char('-')))
            return verb;
        return verb + QLatin1Char(' ') +
               next.mid(next.lastIndexOf(QLatin1Char('/')) + 1);
    }
    if (verb == QLatin1String("for") || verb == QLatin1String("while") ||
        verb == QLatin1String("until"))
        return verb + QStringLiteral(" loop");
    static const QStringList multiVerb = {
        QStringLiteral("git"),   QStringLiteral("go"),      QStringLiteral("cargo"),
        QStringLiteral("npm"),   QStringLiteral("npx"),     QStringLiteral("pip"),
        QStringLiteral("pip3"),  QStringLiteral("flutter"), QStringLiteral("gh"),
        QStringLiteral("docker"), QStringLiteral("systemctl"), QStringLiteral("cmake"),
        QStringLiteral("wrangler"), QStringLiteral("gradle"), QStringLiteral("gradlew"),
        QStringLiteral("ctest"), QStringLiteral("make"),   QStringLiteral("ninja")};
    if (multiVerb.contains(verb) && !next.isEmpty() &&
        !next.startsWith(QLatin1Char('-')) && !next.contains(QLatin1Char('/')) &&
        !separator.match(next).hasMatch())
        return verb + QLatin1Char(' ') + next;
    return verb;
}

// A shell command whose non-zero exit is the answer it was run for: a test,
// build or vet run reporting failures is the tool working, not a tool-use problem.
bool testRunnerCommand(const QString &command)
{
    static const QRegularExpression runner(QStringLiteral(
        "(?:^|[\\s;&|(])(?:pytest|python[0-9.]*\\s+-m\\s+(?:pytest|unittest)|"
        "go\\s+(?:test|build|vet)|cargo\\s+(?:test|build|check)|ctest|"
        "cmake\\s+--build|ninja|swift\\s+(?:test|build)|npm\\s+test|npm\\s+run\\s+test|"
        "npx\\s+playwright\\s+test|npx\\s+jest|jest|vitest|node\\s+--test|"
        "flutter\\s+test|gradlew?\\s+test|make\\s+(?:test|check))(?:\\s|$)"));
    return runner.match(command).hasMatch();
}

QStringList pipelinePieces(const QString &segment)
{
    QStringList pieces;
    QString current;
    int depth = 0;
    QChar quote;
    for (int i = 0; i < segment.size(); ++i) {
        const QChar c = segment.at(i);
        if (!quote.isNull()) {
            current += c;
            if (c == QLatin1Char('\\') && quote == QLatin1Char('"') && i + 1 < segment.size())
                current += segment.at(++i);
            else if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < segment.size()) {
            current += c;
            current += segment.at(++i);
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            current += c;
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('{'))
            ++depth;
        else if ((c == QLatin1Char(')') || c == QLatin1Char('}')) && depth > 0)
            --depth;
        const QChar next = i + 1 < segment.size() ? segment.at(i + 1) : QChar();
        const bool andOr = (c == QLatin1Char('&') && next == QLatin1Char('&')) ||
                           (c == QLatin1Char('|') && next == QLatin1Char('|'));
        const bool pipe = c == QLatin1Char('|') && !andOr &&
                          !(i > 0 && segment.at(i - 1) == QLatin1Char('>'));
        if (depth == 0 && (andOr || pipe)) {
            pieces << current;
            current.clear();
            if (andOr || next == QLatin1Char('&'))
                ++i;
            continue;
        }
        current += c;
    }
    pieces << current;
    return pieces;
}

QStringList chainShapes(const QString &command)
{
    QStringList shapes;
    for (const QString &segment :
         topLevelSegments(stripHeredocs(unwrapShellLauncher(command))))
        for (const QString &piece : pipelinePieces(segment))
            if (const QString shape = commandShape(piece); !shape.isEmpty())
                shapes << shape;
    return shapes;
}

QString readOnlyProbeChainMiss(const QString &command)
{
    static const QStringList readers = {
        QStringLiteral("sed"),        QStringLiteral("rg"),          QStringLiteral("grep"),
        QStringLiteral("cat"),        QStringLiteral("head"),        QStringLiteral("tail"),
        QStringLiteral("ls"),         QStringLiteral("wc"),          QStringLiteral("cut"),
        QStringLiteral("find"),       QStringLiteral("nl"),          QStringLiteral("sort"),
        QStringLiteral("uniq"),       QStringLiteral("awk"),         QStringLiteral("printf"),
        QStringLiteral("echo"),       QStringLiteral("test"),        QStringLiteral("["),
        QStringLiteral("diff"),       QStringLiteral("cmp"),         QStringLiteral("stat"),
        QStringLiteral("which"),      QStringLiteral("command"),     QStringLiteral("file"),
        QStringLiteral("jq"),         QStringLiteral("tr"),          QStringLiteral("column"),
        QStringLiteral("pwd"),        QStringLiteral("cd"),          QStringLiteral("true"),
        QStringLiteral("realpath"),   QStringLiteral("readlink"),    QStringLiteral("basename"),
        QStringLiteral("dirname"),    QStringLiteral("du"),          QStringLiteral("df"),
        QStringLiteral("git grep"),   QStringLiteral("git status"),  QStringLiteral("git log"),
        QStringLiteral("git show"),   QStringLiteral("git diff"),    QStringLiteral("git branch"),
        QStringLiteral("git rev-parse"), QStringLiteral("git ls-files"),
        QStringLiteral("git worktree")};
    static const QStringList missable = {
        QStringLiteral("grep"), QStringLiteral("rg"),   QStringLiteral("git grep"),
        QStringLiteral("ls"),   QStringLiteral("find"), QStringLiteral("test"),
        QStringLiteral("["),    QStringLiteral("diff"), QStringLiteral("cmp"),
        QStringLiteral("stat"), QStringLiteral("command"), QStringLiteral("which")};
    const QStringList shapes = chainShapes(command);
    QString miss;
    for (const QString &shape : shapes) {
        if (!readers.contains(shape))
            return QString();
        if (missable.contains(shape))
            miss = shape;
    }
    return miss;
}

bool reportsFailedChecks(const QStringList &lines)
{
    static const QRegularExpression report(QStringLiteral(
        "^(?:FAIL!?\\s*:|FAIL\\b|FAILED\\b|--- FAIL\\b|not ok \\d|"
        "[^\\s:]+:\\d+:\\d+: (?:fatal )?error:|[^\\s:]+:\\d+: (?:trailing whitespace|"
        "space before tab|indent with spaces|new blank line at EOF|leftover conflict marker))"));
    return std::any_of(lines.cbegin(), lines.cend(),
                       [](const QString &line) { return report.match(line).hasMatch(); });
}

// One failed shell call, classified so recurring failures of one kind fold
// into a single signal carrying the fix for that kind.
struct ShellFailure {
    enum Kind { Other, Blocked, Refused, Interrupted, SelfKill, TimedOut, WaitLoop,
                MissingPath, EmptyRange, ProbeMiss, InlineScript, TestRun,
                UnguardedDelete, MergeConflict, TailCount, MissingCommand };
    Kind kind = Other;
    int exitCode = -1;
    QString message; // the line that says what went wrong
    QString verb;    // commandShape()
};

// A failure that is an outcome rather than a tool-use problem: nothing in
// the tooling would have made it succeed.
bool shellFailureIsResult(const ShellFailure &failure)
{
    return failure.kind == ShellFailure::TestRun ||
           failure.kind == ShellFailure::MergeConflict ||
           failure.kind == ShellFailure::Interrupted;
}

// A foreground command that exists to wait: a poll loop, a follow, a sleep
// chain. It holds the turn until its bound kills it.
bool waitLoopCommand(const QString &command)
{
    static const QRegularExpression waiting(QStringLiteral(
        "\\b(?:until|while)\\b[^\\n]*\\bsleep\\b|\\btail\\s+(?:-[a-zA-Z]*F|--follow|"
        "--pid)|\\binotifywait\\b|\\bsleep\\s+\\d+\\s*(?:;|&&|\\n)"));
    return waiting.match(command).hasMatch();
}

QString slowCommandShape(const QString &command)
{
    if (waitLoopCommand(command))
        return QStringLiteral("foreground wait");
    static const QStringList quick = {
        QStringLiteral("tail"),  QStringLiteral("head"),   QStringLiteral("cat"),
        QStringLiteral("grep"),  QStringLiteral("rg"),     QStringLiteral("ls"),
        QStringLiteral("wc"),    QStringLiteral("cut"),    QStringLiteral("sed"),
        QStringLiteral("awk"),   QStringLiteral("echo"),   QStringLiteral("printf"),
        QStringLiteral("date"),  QStringLiteral("uptime"), QStringLiteral("sort"),
        QStringLiteral("uniq"),  QStringLiteral("jq"),     QStringLiteral("stat"),
        QStringLiteral("test"),  QStringLiteral("["),      QStringLiteral("diff"),
        QStringLiteral("which"), QStringLiteral("df"),     QStringLiteral("du"),
        QStringLiteral("systemctl"), QStringLiteral("git status"),
        QStringLiteral("git rev-parse"), QStringLiteral("true")};
    const QStringList segments = topLevelSegments(stripHeredocs(unwrapShellLauncher(command)));
    for (const QString &segment : segments) {
        const QString shape = commandShape(segment);
        if (!shape.isEmpty() && !quick.contains(shape))
            return shape;
    }
    return commandShape(command);
}

ShellFailure classifyShellFailure(const QString &command, const QString &resultText)
{
    ShellFailure failure;
    failure.verb = commandShape(command);
    QStringList lines;
    for (const QStringView part : QStringView(resultText).split(QLatin1Char('\n'))) {
        QString line = part.trimmed().toString();
        line.remove(QRegularExpression(QStringLiteral("</?tool_use_error>")));
        if (!line.trimmed().isEmpty())
            lines << line.simplified();
    }
    static const QRegularExpression exitLine(QStringLiteral("^Exit code:? (\\d+)$"));
    if (!lines.isEmpty()) {
        if (const auto match = exitLine.match(lines.first()); match.hasMatch()) {
            failure.exitCode = match.captured(1).toInt();
            lines.removeFirst();
        } else if (const auto last = exitLine.match(lines.last()); last.hasMatch()) {
            failure.exitCode = last.captured(1).toInt();
            lines.removeLast();
        }
    }
    const bool traceback = std::any_of(lines.cbegin(), lines.cend(), [](const QString &l) {
        return l.startsWith(QLatin1String("Traceback (most recent call last)"));
    });
    // In a chain the failing command runs last, after the output of the
    // ones that succeeded: the line that says what went wrong is the first
    // that reads like an error, else the first line.
    static const QRegularExpression errorMarker(QStringLiteral(
        "(?i)(?:^|\\b)(?:error|fatal|failed|blocked|traceback|no such file|"
        "not found|cannot|can't|permission denied|usage:|unbound variable|"
        "syntax error|timed out|killed|interrupted|unexpected|unknown|invalid|"
        "statically analyzed)(?:\\b|:)"));
    bool errorLine = false;
    if (!lines.isEmpty()) {
        failure.message = traceback ? lines.last() : lines.first();
        errorLine = traceback;
        if (!traceback)
            for (const QString &line : std::as_const(lines))
                if (errorMarker.match(line).hasMatch()) {
                    failure.message = line;
                    errorLine = true;
                    break;
                }
    }
    const QString lowered = lines.join(QLatin1Char('\n')).toLower();
    const bool killer = failure.verb == QLatin1String("pkill") ||
                        failure.verb == QLatin1String("kill") ||
                        failure.verb == QLatin1String("killall") ||
                        command.contains(QLatin1String("pkill "));
    if (failure.message.startsWith(QLatin1String("Blocked:")))
        failure.kind = ShellFailure::Blocked;
    else if (lowered.contains(QLatin1String("[request interrupted by user")))
        failure.kind = ShellFailure::Interrupted;
    else if (lowered.contains(QLatin1String("dangerous rm operation")) ||
             lowered.contains(QLatin1String("before the removal")))
        failure.kind = ShellFailure::UnguardedDelete;
    else if (lowered.contains(QLatin1String("cannot be statically analyzed")) ||
             lowered.contains(QLatin1String("requested permissions to use")) ||
             lowered.contains(QLatin1String("permission to use this tool")) ||
             lowered.contains(QLatin1String("requires approval")) ||
             lowered.contains(QLatin1String("denied by")))
        failure.kind = ShellFailure::Refused;
    else if ((testRunnerCommand(command) || failure.verb.endsWith(QLatin1String("-tests"))) &&
             failure.exitCode == 1)
        failure.kind = ShellFailure::TestRun;
    else if (lowered.contains(QLatin1String("automatic merge failed")) ||
             lowered.contains(QLatin1String("conflict (content)")) ||
             lowered.contains(QLatin1String("resolve all conflicts manually")))
        failure.kind = ShellFailure::MergeConflict;
    else if (!traceback && failure.exitCode >= 1 && failure.exitCode < 124 &&
             reportsFailedChecks(lines))
        failure.kind = ShellFailure::TestRun;
    else if (lowered.contains(QLatin1String("option used in invalid context")))
        failure.kind = ShellFailure::TailCount;
    else if (failure.exitCode == 144 || (killer && failure.exitCode >= 128))
        failure.kind = ShellFailure::SelfKill;
    else if (waitLoopCommand(command) &&
             (failure.exitCode >= 124 || failure.message.isEmpty() || !errorLine))
        failure.kind = ShellFailure::WaitLoop;
    else if (failure.exitCode == 124 || failure.exitCode == 137 ||
             failure.exitCode == 143 ||
             lowered.contains(QLatin1String("command timed out")))
        failure.kind = ShellFailure::TimedOut;
    else if (lowered.contains(QLatin1String("unknown command: `,'")) ||
             lowered.contains(QLatin1String("invalid usage of line address 0")) ||
             lowered.contains(QLatin1String("unexpected `,'")))
        failure.kind = ShellFailure::EmptyRange;
    else if (lowered.contains(QLatin1String("no such file or directory")) ||
             lowered.contains(QLatin1String("can't read")) ||
             lowered.contains(QLatin1String("cannot access")) ||
             lowered.contains(QLatin1String("cannot open")) ||
             lowered.contains(QLatin1String("not a directory")))
        failure.kind = ShellFailure::MissingPath;
    else if (failure.exitCode == 127 || lowered.contains(QLatin1String("command not found")))
        failure.kind = ShellFailure::MissingCommand;
    else if (const QString miss = failure.exitCode == 1 ? readOnlyProbeChainMiss(command)
                                                        : QString();
             !miss.isEmpty()) {
        failure.kind = ShellFailure::ProbeMiss;
        failure.verb = miss;
    } else if (failure.verb.endsWith(QLatin1String("inline script")) &&
             (traceback || failure.exitCode == 1))
        failure.kind = ShellFailure::InlineScript;
    else if (!errorLine && failure.exitCode >= 1 && failure.exitCode <= 2) {
        static const QStringList probes = {
            QStringLiteral("grep"), QStringLiteral("rg"), QStringLiteral("ls"),
            QStringLiteral("find"), QStringLiteral("test"), QStringLiteral("["),
            QStringLiteral("git grep"), QStringLiteral("diff"), QStringLiteral("cmp"),
            QStringLiteral("wc"), QStringLiteral("cat"), QStringLiteral("head"),
            QStringLiteral("tail"), QStringLiteral("sed"), QStringLiteral("awk"),
            QStringLiteral("command"), QStringLiteral("which"), QStringLiteral("stat")};
        if (probes.contains(failure.verb))
            failure.kind = ShellFailure::ProbeMiss;
    }
    // The command the harness blocked, or the one that killed the shell, is
    // the one to name, wherever it sat in the chain.
    if (failure.kind == ShellFailure::Blocked) {
        static const QRegularExpression blocked(
            QStringLiteral("^Blocked:\\s*(.+?)(?:\\s+followed by\\b|$)"));
        if (const auto match = blocked.match(failure.message); match.hasMatch())
            if (const QString shape = commandShape(match.captured(1)); !shape.isEmpty())
                failure.verb = shape;
    } else if (failure.kind == ShellFailure::SelfKill && command.contains(QLatin1String("pkill"))) {
        failure.verb = QStringLiteral("pkill");
    }
    return failure;
}

QString shellFailureTopic(const ShellFailure &failure)
{
    QString topic;
    switch (failure.kind) {
    case ShellFailure::Blocked: topic = QStringLiteral("blocked by the harness"); break;
    case ShellFailure::Refused: topic = QStringLiteral("refused by the harness"); break;
    case ShellFailure::Interrupted: topic = QStringLiteral("interrupted by the user"); break;
    case ShellFailure::SelfKill: topic = QStringLiteral("killed its own shell"); break;
    case ShellFailure::TimedOut: topic = QStringLiteral("timed out"); break;
    case ShellFailure::WaitLoop: topic = QStringLiteral("foreground wait"); break;
    case ShellFailure::MissingPath: topic = QStringLiteral("missing path"); break;
    case ShellFailure::EmptyRange: topic = QStringLiteral("empty line number"); break;
    case ShellFailure::ProbeMiss: topic = QStringLiteral("probe found nothing"); break;
    case ShellFailure::InlineScript: topic = QStringLiteral("inline script failed"); break;
    case ShellFailure::TestRun: topic = QStringLiteral("tests failed"); break;
    case ShellFailure::UnguardedDelete: topic = QStringLiteral("unguarded delete"); break;
    case ShellFailure::MergeConflict: topic = QStringLiteral("merge conflict"); break;
    case ShellFailure::TailCount: topic = QStringLiteral("tail -N on several files"); break;
    case ShellFailure::MissingCommand: topic = QStringLiteral("command not found"); break;
    case ShellFailure::Other:
        if (failure.exitCode >= 0)
            topic = QStringLiteral("exit %1").arg(failure.exitCode);
        break;
    }
    if (failure.verb.isEmpty())
        return topic;
    return topic.isEmpty() ? failure.verb
                           : QStringLiteral("%1 · %2").arg(failure.verb, topic);
}

QString shellFailureSuggestion(const ShellFailure &failure)
{
    switch (failure.kind) {
    case ShellFailure::Blocked:
        return QStringLiteral("The harness refuses a foreground sleep or wait "
                              "loop; run the work with run_in_background and "
                              "wait with Monitor.");
    case ShellFailure::Refused:
        return QStringLiteral("The harness would not run this unreviewed: it "
                              "could not parse the quoting or a permission rule "
                              "denies it. Simplify the quoting, split the "
                              "pipeline, or move the script into a file.");
    case ShellFailure::Interrupted:
        return QStringLiteral("The user interrupted the call; nothing to change "
                              "in the tooling.");
    case ShellFailure::SelfKill:
        return QStringLiteral("pkill -f matched this call's own shell, whose "
                              "command line contains the pattern; bracket its "
                              "first character ([f]oo) or kill by pid.");
    case ShellFailure::TimedOut:
        return QStringLiteral("The command outlived its bound and was killed; "
                              "run it with run_in_background, tee its output to "
                              "a file, and wait with Monitor.");
    case ShellFailure::WaitLoop:
        return QStringLiteral("A foreground poll loop, sleep chain, or tail -F "
                              "holds the turn until its bound kills it; run the "
                              "work with run_in_background and wait with "
                              "Monitor instead.");
    case ShellFailure::MissingPath:
        return QStringLiteral("The working directory persists between calls, so "
                              "an earlier cd moves later relative paths: use "
                              "absolute paths, git -C, or (cd dir && …) in a "
                              "subshell, and Glob (or rg --files) a path before "
                              "assuming it rather than guessing.");
    case ShellFailure::EmptyRange:
        return QStringLiteral("sed -n was handed an empty line number from a "
                              "grep -n that found nothing; check the number is "
                              "non-empty before using it, or Grep then Read a "
                              "range.");
    case ShellFailure::ProbeMiss:
        return QStringLiteral("A probe that finds nothing exits non-zero and "
                              "cancels its parallel siblings; end the chain "
                              "with || true so a miss is a result, not an error.");
    case ShellFailure::InlineScript:
        return QStringLiteral("A failing inline script costs its whole body "
                              "again; write anything longer than a few lines to "
                              "a file under the build dir, run it by path, and "
                              "fix it with Edit.");
    case ShellFailure::TestRun:
        return QStringLiteral("A failing test run is the tool working; nothing "
                              "to change in the tooling.");
    case ShellFailure::UnguardedDelete:
        return QStringLiteral("The harness refuses rm on a $VAR path or a relative "
                              "glob after cd, since an empty variable widens it; "
                              "spell the absolute path out, or delete with "
                              "fm_safe_rm_tree <expected parent> <path> from "
                              "tools/forkmesh-safe-rm.sh.");
    case ShellFailure::MergeConflict:
        return QStringLiteral("A merge that stops on conflicts is the tool "
                              "working; nothing to change in the tooling.");
    case ShellFailure::TailCount:
        return QStringLiteral("GNU tail rejects the obsolete -N count when given "
                              "more than one file; write tail -n N.");
    case ShellFailure::MissingCommand:
        return QStringLiteral("The command is not installed on this host; probe "
                              "with command -v name || true before relying on it, "
                              "and report it missing instead of retrying.");
    case ShellFailure::Other:
        break;
    }
    return QStringLiteral("Recurring failures of one kind point at a tool "
                          "description, path convention, or permission worth "
                          "fixing in the runtime prompt or host.");
}

bool readOnlyFamily(const QString &family)
{
    static const QStringList families = {
        QStringLiteral("read"),     QStringLiteral("grep"),
        QStringLiteral("glob"),     QStringLiteral("ast_grep"),
        QStringLiteral("lsp"),      QStringLiteral("web_search"),
        QStringLiteral("github")};
    return families.contains(family);
}

bool mutatingFamily(const QString &family)
{
    static const QStringList families = {
        QStringLiteral("write"), QStringLiteral("edit"),
        QStringLiteral("ast_edit"), QStringLiteral("bash"),
        QStringLiteral("eval"),  QStringLiteral("rewind")};
    return families.contains(family);
}

// Families whose calls legitimately run for minutes: waiting on a subagent
// or on the operator is not a slow tool.
bool longRunningFamily(const QString &family)
{
    static const QStringList families = {
        QStringLiteral("task"), QStringLiteral("hub"), QStringLiteral("ask"),
        QStringLiteral("yield"), QStringLiteral("goal")};
    return families.contains(family);
}

QString oversizedSuggestion(const QString &family)
{
    if (family == QLatin1String("bash"))
        return QStringLiteral("Bound command output (head/tail, grep -m, --quiet) "
                              "and line width (cut -c, rg --max-columns) for logs, "
                              "JSONL or minified files, or write it to a file and "
                              "read the relevant range.");
    if (family == QLatin1String("grep") || family == QLatin1String("ast_grep"))
        return QStringLiteral("Narrow the pattern or path, or list matching files "
                              "first and read only the hits that matter.");
    if (family == QLatin1String("glob"))
        return QStringLiteral("Use a narrower glob; a path listing this large "
                              "crowds out working context.");
    if (family == QLatin1String("read"))
        return QStringLiteral("Read a line range (offset/limit) around the code "
                              "that matters.");
    return QStringLiteral("Bound this tool's output; results this large crowd "
                          "out working context.");
}

QString oversizedShellTopic(const QString &command)
{
    if (command.isEmpty())
        return QString();
    if (testRunnerCommand(command))
        return QStringLiteral("test run");
    static const QStringList searches = {
        QStringLiteral("rg"), QStringLiteral("grep"), QStringLiteral("git grep"),
        QStringLiteral("find")};
    for (const QString &shape : chainShapes(command))
        if (searches.contains(shape))
            return QStringLiteral("search");
    return QString();
}

QString oversizedShellSuggestion(const QString &command)
{
    const QString topic = oversizedShellTopic(command);
    if (topic == QLatin1String("test run"))
        return QStringLiteral("Run tests quietly (pytest -q --tb=short, go test "
                              "without -v) and pipe the run through tail -n 80; "
                              "rerun one failing test verbosely only when the "
                              "summary is not enough.");
    if (topic == QLatin1String("search"))
        return QStringLiteral("rg -m and grep -m cap matches per file, not in "
                              "total: list files first with rg -l, then pipe the "
                              "search through head -n 60 and add --max-columns "
                              "200; leave data, logs and build trees out.");
    return oversizedSuggestion(QStringLiteral("bash"));
}

} // namespace

QString agentShellCommandShape(const QString &command)
{
    return commandShape(command);
}

AgentToolImprovementLog::AgentToolImprovementLog(QString logPath)
    : m_logPath(std::move(logPath))
{
}

QString AgentToolImprovementLog::kindLabel(const QString &kind)
{
    if (kind == KindRepeatedCall)
        return QStringLiteral("Repeated call");
    if (kind == KindWholeFileRead)
        return QStringLiteral("Whole-file read");
    if (kind == KindWholeFileEdit)
        return QStringLiteral("Edit rewrites most of the file");
    if (kind == KindOversizedResult)
        return QStringLiteral("Oversized result");
    if (kind == KindToolError)
        return QStringLiteral("Tool error");
    if (kind == KindSlowCall)
        return QStringLiteral("Slow call");
    if (kind == KindCalledWhileOff)
        return QStringLiteral("Used while switched off");
    if (kind == KindUncatalogued)
        return QStringLiteral("Uncatalogued tool");
    if (kind == KindAbandonedCall)
        return QStringLiteral("Unanswered call");
    return kind;
}

bool AgentToolImprovementLog::kindIsFailure(const QString &kind)
{
    return kind == KindToolError || kind == KindCalledWhileOff ||
           kind == KindAbandonedCall;
}

int AgentToolImprovementLog::observe(const AgentToolCallObservation &call)
{
    const qint64 ms = call.completedMs > 0 ? call.completedMs
                                           : QDateTime::currentMSecsSinceEpoch();
    const QString subject = subjectOf(call);
    QVector<Occurrence> found;
    auto add = [&](const QString &kind, qint64 tokens, const QString &detail,
                   const QString &suggestion, const QString &signature = QString(),
                   const QString &topic = QString()) {
        Occurrence occurrence;
        occurrence.kind = kind;
        occurrence.family = call.family;
        occurrence.runtimeName = call.runtimeName;
        occurrence.signature = signature;
        occurrence.topic = topic;
        occurrence.detail = detail;
        occurrence.suggestion = suggestion;
        occurrence.provider = call.provider;
        occurrence.model = call.model;
        occurrence.sessionId = call.sessionId;
        occurrence.tokens = qMax<qint64>(0, tokens);
        occurrence.ms = ms;
        found.append(occurrence);
    };

    if (call.whileOff)
        add(KindCalledWhileOff, call.inputTokens + call.resultTokens,
            subject.isEmpty() ? call.runtimeName : subject,
            QStringLiteral("The runtime still reached %1 after the family was "
                           "switched off; its enforcement needs to cover this "
                           "tool name.")
                .arg(call.runtimeName.isEmpty() ? call.family : call.runtimeName));
    if (call.family == QLatin1String("other"))
        add(KindUncatalogued, 0, call.runtimeName,
            QStringLiteral("Map \"%1\" to a tool family in AgentToolCatalog so "
                           "its cost is attributed and it can be switched off.")
                .arg(call.runtimeName));

    const QString command =
        call.family == QLatin1String("bash")
            ? unwrapShellLauncher(call.input.value(QStringLiteral("command")).toString())
            : QString();
    if (call.abandoned) {
        const QString shape = command.isEmpty() ? QString() : slowCommandShape(command);
        add(KindAbandonedCall, call.inputTokens, subject,
            QStringLiteral("The turn ended (stopped, interrupted, or crashed) "
                           "while this call was still running, so its result "
                           "never reached the transcript; keep work this long "
                           "in the background so a stop does not lose it."),
            shape, shape);
    } else if (call.isError && !command.isEmpty()) {
        // A shell result leads with "Exit code N"; the line that says what
        // went wrong follows it, and that is what the reader needs to see.
        const ShellFailure failure = classifyShellFailure(command, call.resultText);
        if (shellFailureIsResult(failure)) {
            if (call.resultTokens >= kOversizedResultTokens)
                add(KindOversizedResult, call.resultTokens,
                    QStringLiteral("%1 — %2 tokens returned")
                        .arg(subject, tokensText(call.resultTokens)),
                    oversizedShellSuggestion(command), oversizedShellTopic(command),
                    oversizedShellTopic(command));
        } else {
            QString what = failure.exitCode >= 0
                               ? QStringLiteral("exit %1").arg(failure.exitCode)
                               : QString();
            if (!failure.message.isEmpty())
                what += (what.isEmpty() ? QString() : QStringLiteral(": ")) +
                        oneLine(failure.message);
            const QString topic = shellFailureTopic(failure);
            // Classified failures fold by class; the rest by normalized message.
            const QString signature =
                failure.kind == ShellFailure::Other
                    ? topic + QLatin1Char('\x1f') + errorSignature(failure.message)
                    : topic;
            add(KindToolError, call.inputTokens + call.resultTokens,
                what.isEmpty() ? subject : QStringLiteral("%1 — %2").arg(subject, what),
                shellFailureSuggestion(failure), signature, topic);
        }
    } else if (call.isError) {
        const QString line = oneLine(call.resultText);
        const QString lowered = call.resultText.toLower();
        QString signature = errorSignature(line);
        QString topic;
        QString suggestion = QStringLiteral(
            "Recurring failures of one kind point at a tool description, path "
            "convention, or permission worth fixing in the runtime prompt or host.");
        if (lowered.contains(QLatin1String("while in plan mode"))) {
            signature = topic = QStringLiteral("refused in plan mode");
            suggestion = QStringLiteral(
                "Plan mode refuses every file change; keep to reading and "
                "planning, and present the plan with ExitPlanMode before the "
                "first Edit or Write.");
        } else if (lowered.contains(QLatin1String("file does not exist"))) {
            signature = topic = QStringLiteral("guessed path");
            suggestion = QStringLiteral(
                "The path was guessed; Glob the file name (or rg --files | rg "
                "name) first and Read the absolute path it returns.");
        } else if (lowered.contains(QLatin1String("has not been read yet")) ||
                   lowered.contains(QLatin1String("modified since read"))) {
            signature = topic = QStringLiteral("write before read");
            suggestion = QStringLiteral(
                "Write and Edit refuse a file not Read in this conversation, and "
                "the whole new content is paid for again; Read a range of an "
                "existing file first and change it with Edit, and Write only "
                "new files.");
        } else if (lowered.contains(QLatin1String("concurrent subagent limit"))) {
            signature = topic = QStringLiteral("subagent limit");
            suggestion = QStringLiteral(
                "The runtime caps concurrent subagents; launch at most ten at "
                "once, fold small audits into one subagent, and start the next "
                "batch when earlier ones report back.");
        }
        add(KindToolError, call.inputTokens + call.resultTokens,
            subject.isEmpty() ? line
                              : QStringLiteral("%1 — %2").arg(subject, line),
            suggestion, signature, topic);
    } else {
        bool wholeFile = false;
        if (call.measured && call.baselineTokens > 0) {
            const qint64 floor = call.outputSide ? kWholeEditMinTokens
                                                 : kWholeFileMinTokens;
            // Nine tenths or more of the naive cost: the efficient tool saved
            // almost nothing on this call.
            if (call.baselineTokens >= floor &&
                call.actualTokens * 10 >= call.baselineTokens * 9) {
                wholeFile = true;
                if (call.outputSide)
                    add(KindWholeFileEdit, call.actualTokens, subject,
                        QStringLiteral("Emit small anchored edits around the "
                                       "changed lines instead of replacing most "
                                       "of the file."));
                else
                    // Line-number prefixes make a whole-file result a little
                    // larger than the file, so "3315 of 3063" would mislead.
                    add(KindWholeFileRead, call.actualTokens,
                        call.actualTokens >= call.baselineTokens
                            ? QStringLiteral("%1 — whole file, ~%2 tokens")
                                  .arg(subject, tokensText(call.actualTokens))
                            : QStringLiteral("%1 — %2 of %3 tokens")
                                  .arg(subject, tokensText(call.actualTokens),
                                       tokensText(call.baselineTokens)),
                        QStringLiteral("Search for the symbol first, then read a "
                                       "line range (offset/limit) instead of the "
                                       "whole file."));
            }
        }
        if (!wholeFile && call.resultTokens >= kOversizedResultTokens)
            add(KindOversizedResult, call.resultTokens,
                QStringLiteral("%1 — %2 tokens returned")
                    .arg(subject.isEmpty() ? call.runtimeName : subject,
                         tokensText(call.resultTokens)),
                command.isEmpty() ? oversizedSuggestion(call.family)
                                  : oversizedShellSuggestion(command),
                oversizedShellTopic(command), oversizedShellTopic(command));
        if (noteRepeat(call))
            add(KindRepeatedCall, call.resultTokens, subject,
                QStringLiteral("The same call already ran this turn; reuse the "
                               "earlier result instead of paying for it again."));
    }

    const qint64 latency = call.completedMs - call.startedMs;
    if (!call.abandoned && !call.heldBehindSibling && call.startedMs > 0 &&
        latency >= kSlowCallMs && !longRunningFamily(call.family)) {
        // Slow shell calls fold by what ran (a test run, a build, an inline
        // script), so the board says which kind of work blocks turns.
        const QString shape = command.isEmpty() ? QString() : slowCommandShape(command);
        add(KindSlowCall, 0,
            QStringLiteral("%1 — %2 s")
                .arg(subject.isEmpty() ? call.runtimeName : subject)
                .arg(latency / 1000),
            command.isEmpty()
                ? QStringLiteral("Run long work in the background or bound it "
                                 "with a timeout so the turn is not blocked.")
            : shape == QLatin1String("foreground wait")
                ? QStringLiteral("A foreground poll loop holds the turn for its "
                                 "whole bound, and a timeout bash -c wrapper only "
                                 "slips past the sleep guard; run the until-loop "
                                 "with Bash run_in_background (one notice when it "
                                 "ends) or Monitor, and keep working meanwhile.")
                : QStringLiteral("A foreground command this long blocks the turn "
                                 "and risks the harness timeout; run it with "
                                 "run_in_background, tee its output to a file, "
                                 "and wait with Monitor."),
            shape, shape);
    }

    for (const Occurrence &occurrence : std::as_const(found)) {
        record(occurrence, true);
        if (occurred)
            occurred(occurrence.sessionId, occurrence.kind, occurrence.tokens);
    }
    if (!found.isEmpty() && changed)
        changed();
    return int(found.size());
}

bool AgentToolImprovementLog::noteRepeat(const AgentToolCallObservation &call)
{
    if (mutatingFamily(call.family)) {
        // Anything that may change files makes a later identical read fresh.
        m_seen.remove(call.sessionId);
        return false;
    }
    if (!readOnlyFamily(call.family))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(call.runtimeName.toUtf8());
    hash.addData(QByteArrayView("\x1f"));
    hash.addData(QJsonDocument(call.input).toJson(QJsonDocument::Compact));
    const QByteArray fingerprint = hash.result();

    QHash<QByteArray, qint64> &seen = m_seen[call.sessionId];
    const qint64 previous = seen.value(fingerprint, -1);
    const bool repeat = previous >= 0 && call.completedMs - previous <= kRepeatWindowMs;
    if (seen.size() >= kMaxSeenPerSession && !seen.contains(fingerprint)) {
        for (auto it = seen.begin(); it != seen.end();)
            it = call.completedMs - it.value() > kRepeatWindowMs ? seen.erase(it)
                                                                 : std::next(it);
        if (seen.size() >= kMaxSeenPerSession)
            seen.clear();
    }
    seen.insert(fingerprint, call.completedMs);
    return repeat;
}

void AgentToolImprovementLog::forgetSession(int sessionId)
{
    m_seen.remove(sessionId);
}

void AgentToolImprovementLog::record(const Occurrence &occurrence, bool append)
{
    const QString key = QStringList{occurrence.kind, occurrence.family,
                                    occurrence.runtimeName, occurrence.signature}
                            .join(QLatin1Char('\x1f'));
    auto found = m_index.constFind(key);
    if (found == m_index.constEnd()) {
        if (m_signals.size() >= kMaxSignals) {
            // Forget the signal that has been quiet the longest.
            const auto stalest = std::min_element(
                m_signals.cbegin(), m_signals.cend(),
                [](const AgentToolImprovement &a, const AgentToolImprovement &b) {
                    return a.lastMs < b.lastMs;
                });
            m_signals.remove(int(stalest - m_signals.cbegin()));
            m_index.clear();
            for (int i = 0; i < m_signals.size(); ++i)
                m_index.insert(m_signals.at(i).key, i);
        }
        AgentToolImprovement signal;
        signal.key = key;
        signal.kind = occurrence.kind;
        signal.family = occurrence.family;
        signal.runtimeName = occurrence.runtimeName;
        signal.signature = occurrence.signature;
        signal.firstMs = occurrence.ms;
        m_signals.append(signal);
        found = m_index.insert(key, int(m_signals.size() - 1));
    }
    AgentToolImprovement &signal = m_signals[found.value()];
    signal.occurrences += 1;
    signal.tokensAtStake += occurrence.tokens;
    signal.firstMs = qMin(signal.firstMs, occurrence.ms);
    if (occurrence.ms >= signal.lastMs) {
        signal.lastMs = occurrence.ms;
        signal.detail = occurrence.detail;
        signal.suggestion = occurrence.suggestion;
        signal.topic = occurrence.topic;
    }
    m_occurrences += 1;
    if (append)
        appendLine(occurrence);
}

void AgentToolImprovementLog::appendLine(const Occurrence &occurrence)
{
    if (m_logPath.isEmpty())
        return;
    const QFileInfo info(m_logPath);
    if (info.exists() && info.size() >= kRotateBytes) {
        const QString rotated = m_logPath + QStringLiteral(".1");
        QFile::remove(rotated);
        QFile::rename(m_logPath, rotated);
    } else if (!info.exists()) {
        QDir().mkpath(info.absolutePath());
    }
    QFile file(m_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const QJsonObject line{
        {QStringLiteral("schema"), kSchemaVersion},
        {QStringLiteral("ms"), double(occurrence.ms)},
        {QStringLiteral("kind"), occurrence.kind},
        {QStringLiteral("family"), occurrence.family},
        {QStringLiteral("tool"), occurrence.runtimeName},
        {QStringLiteral("signature"), occurrence.signature},
        {QStringLiteral("session"), occurrence.sessionId},
        {QStringLiteral("provider"), occurrence.provider},
        {QStringLiteral("model"), occurrence.model},
        {QStringLiteral("tokens"), double(occurrence.tokens)},
        {QStringLiteral("detail"), occurrence.detail},
        {QStringLiteral("suggestion"), occurrence.suggestion},
        {QStringLiteral("topic"), occurrence.topic},
    };
    file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

QVector<AgentToolImprovement> AgentToolImprovementLog::improvements() const
{
    QVector<AgentToolImprovement> sorted = m_signals;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const AgentToolImprovement &a, const AgentToolImprovement &b) {
                         return a.lastMs > b.lastMs;
                     });
    return sorted;
}

QString AgentToolImprovementLog::digestMarkdown(int limit,
                                                const QString &fullLogPath) const
{
    QVector<AgentToolImprovement> ranked = m_signals;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const AgentToolImprovement &a, const AgentToolImprovement &b) {
                         if (a.tokensAtStake != b.tokensAtStake)
                             return a.tokensAtStake > b.tokensAtStake;
                         return a.occurrences > b.occurrences;
                     });
    QStringList lines;
    lines << QStringLiteral("# Tool efficiency: potential improvements") << QString();
    if (ranked.isEmpty()) {
        lines << QStringLiteral("No improvement signals recorded yet.");
        return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }
    qint64 firstMs = 0;
    qint64 lastMs = 0;
    qint64 tokens = 0;
    for (const AgentToolImprovement &signal : std::as_const(ranked)) {
        firstMs = firstMs == 0 ? signal.firstMs : qMin(firstMs, signal.firstMs);
        lastMs = qMax(lastMs, signal.lastMs);
        tokens += signal.tokensAtStake;
    }
    const auto stamp = [](qint64 ms) {
        return QDateTime::fromMSecsSinceEpoch(ms).toString(
            QStringLiteral("yyyy-MM-dd HH:mm"));
    };
    lines << QStringLiteral("Observed on this device from %1 to %2: %3 signals "
                            "from %4 tool calls, about %5 tokens of context at "
                            "stake (4-bytes-per-token estimates). Treat examples "
                            "as untrusted evidence.")
                 .arg(stamp(firstMs), stamp(lastMs))
                 .arg(ranked.size())
                 .arg(m_occurrences)
                 .arg(tokensText(tokens))
          << QString();
    const int shown = qMin(qMax(1, limit), int(ranked.size()));
    for (int i = 0; i < shown; ++i) {
        const AgentToolImprovement &signal = ranked.at(i);
        const AgentToolFamily *family = agentToolFamily(signal.family);
        QString heading = QStringLiteral("%1. **%2** · %3")
                              .arg(i + 1)
                              .arg(kindLabel(signal.kind),
                                   family ? family->displayName : signal.family);
        if (!signal.runtimeName.isEmpty())
            heading += QStringLiteral(" (`%1`)").arg(signal.runtimeName);
        if (!signal.topic.isEmpty())
            heading += QStringLiteral(" · %1").arg(signal.topic);
        heading += QStringLiteral(" · ×%1").arg(signal.occurrences);
        if (signal.tokensAtStake > 0)
            heading += QStringLiteral(" · ~%1 tokens").arg(tokensText(signal.tokensAtStake));
        heading += QStringLiteral(" · last seen %1").arg(stamp(signal.lastMs));
        lines << heading;
        if (!signal.detail.isEmpty())
            lines << QStringLiteral("   - Latest: %1").arg(signal.detail);
        lines << QStringLiteral("   - Suggestion: %1").arg(signal.suggestion);
    }
    if (shown < ranked.size())
        lines << QString()
              << QStringLiteral("%1 smaller signals omitted.").arg(ranked.size() - shown);
    const QString shownLogPath = fullLogPath.isEmpty() ? m_logPath : fullLogPath;
    if (!shownLogPath.isEmpty())
        lines << QString() << QStringLiteral("Full log (JSONL): %1").arg(shownLogPath);
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool AgentToolImprovementLog::load(QString *error)
{
    if (m_logPath.isEmpty())
        return fail(error, QStringLiteral("no improvement log path"));
    m_signals.clear();
    m_index.clear();
    m_seen.clear();
    m_occurrences = 0;
    QFile file(m_logPath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("cannot open %1").arg(m_logPath));
    if (file.size() > kRotateBytes * 2)
        file.seek(file.size() - kRotateBytes * 2);
    QVector<Occurrence> loaded;
    qint64 newestMs = 0;
    while (!file.atEnd()) {
        const QByteArray raw = file.readLine().trimmed();
        if (raw.isEmpty())
            continue;
        const QJsonObject line = QJsonDocument::fromJson(raw).object();
        const QString kind = line.value(QStringLiteral("kind")).toString();
        if (line.value(QStringLiteral("schema")).toInt() != kSchemaVersion ||
            kindLabel(kind) == kind || !line.value(QStringLiteral("ms")).isDouble())
            continue;
        Occurrence occurrence;
        occurrence.kind = kind;
        occurrence.family = line.value(QStringLiteral("family")).toString();
        occurrence.runtimeName = line.value(QStringLiteral("tool")).toString();
        occurrence.signature = line.value(QStringLiteral("signature")).toString();
        occurrence.detail = line.value(QStringLiteral("detail")).toString();
        occurrence.suggestion = line.value(QStringLiteral("suggestion")).toString();
        occurrence.topic = line.value(QStringLiteral("topic")).toString();
        occurrence.sessionId = line.value(QStringLiteral("session")).toInt();
        occurrence.tokens =
            qMax<qint64>(0, qint64(line.value(QStringLiteral("tokens")).toDouble()));
        occurrence.ms = qint64(line.value(QStringLiteral("ms")).toDouble());
        newestMs = qMax(newestMs, occurrence.ms);
        loaded.append(occurrence);
    }
    // Aged against the newest line, not the clock, so a replayed log keeps
    // its shape.
    for (const Occurrence &occurrence : std::as_const(loaded))
        if (occurrence.ms >= newestMs - kMaxAgeMs)
            record(occurrence, false);
    return true;
}

void AgentToolImprovementLog::clear()
{
    m_signals.clear();
    m_index.clear();
    m_seen.clear();
    m_occurrences = 0;
    if (!m_logPath.isEmpty()) {
        QFile::remove(m_logPath);
        QFile::remove(m_logPath + QStringLiteral(".1"));
    }
    if (changed)
        changed();
}

QString AgentToolImprovementLog::sentLogPath() const
{
    return m_logPath.isEmpty() ? QString() : m_logPath + QStringLiteral(".sent");
}

bool AgentToolImprovementLog::archiveSent()
{
    if (m_logPath.isEmpty())
        return false;
    const QString sent = sentLogPath();
    QFile::remove(sent);
    const bool moved = !QFile::exists(m_logPath) || QFile::rename(m_logPath, sent);
    if (!moved)
        return false;
    QFile::remove(m_logPath + QStringLiteral(".1"));
    m_signals.clear();
    m_index.clear();
    m_seen.clear();
    m_occurrences = 0;
    if (changed)
        changed();
    return true;
}
