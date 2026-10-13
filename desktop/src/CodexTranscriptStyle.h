#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace CodexTranscriptStyle {

inline QString unescapeDoubleQuotedShellWord(const QString &word)
{
    QString out;
    out.reserve(word.size());
    for (int i = 0; i < word.size(); ++i) {
        const QChar c = word.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= word.size()) {
            out += c;
            continue;
        }
        const QChar next = word.at(i + 1);
        if (next == QLatin1Char('"') || next == QLatin1Char('\\')
            || next == QLatin1Char('$') || next == QLatin1Char('`')) {
            out += next;
            ++i;
        } else if (next == QLatin1Char('\n')) {
            ++i; // a shell line continuation contributes no character
        } else {
            out += c; // backslashes before other characters stay literal
        }
    }
    return out;
}

// Recent Codex commandExecution events sometimes report the already assembled
// process line rather than argv. Match the same bash/sh -c wrapper that the argv
// path below removes, so the transcript shows (and its Run button replays) the
// command the agent meant rather than an implementation-detail shell wrapper.
inline QString unwrapShellCommand(const QString &raw)
{
    static const QRegularExpression wrapper(QStringLiteral(
        "^\\s*((?:/(?:[^\\s/]+/)*)?(?:bash|sh|zsh|dash|ksh))\\s+"
        "(-[^\\s]*c[^\\s]*)\\s+([\\\"'])([\\s\\S]*)\\3\\s*$"));
    const QRegularExpressionMatch match = wrapper.match(raw);
    if (!match.hasMatch())
        return raw.trimmed();
    const QString body = match.captured(4);
    return (match.captured(3) == QLatin1String("\"")
                ? unescapeDoubleQuotedShellWord(body)
                : body)
        .trimmed();
}

inline QString commandText(const QJsonObject &input)
{
    const QJsonValue raw = input.value(QLatin1String("command"));
    if (raw.isString())
        return unwrapShellCommand(raw.toString());
    if (!raw.isArray())
        return QString();
    QStringList argv;
    for (const QJsonValue &v : raw.toArray())
        argv << v.toString();
    if (argv.size() >= 3 && argv.at(1).startsWith(QLatin1Char('-'))
        && argv.at(1).contains(QLatin1Char('c'))) {
        const QString shell = argv.first().section(QLatin1Char('/'), -1);
        if (shell == QLatin1String("bash") || shell == QLatin1String("sh")
            || shell == QLatin1String("zsh"))
            return argv.at(2).trimmed();
    }
    return argv.join(QLatin1Char(' ')).trimmed();
}

inline bool shellLineBreakNeedsSeparator(const QString &before,
                                         const QString &after)
{
    const QString left = before.trimmed();
    const QString right = after.trimmed();
    if (left.isEmpty() || right.isEmpty())
        return false;
    for (const QString &tail :
         {QStringLiteral("&&"), QStringLiteral("||"), QStringLiteral("|"),
          QStringLiteral(";"), QStringLiteral("&"), QStringLiteral("("),
          QStringLiteral("{")}) {
        if (left.endsWith(tail))
            return false;
    }
    for (const QString &head :
         {QStringLiteral("&&"), QStringLiteral("||"), QStringLiteral("|"),
          QStringLiteral(";"), QStringLiteral("&"), QStringLiteral(")"),
          QStringLiteral("}")}) {
        if (right.startsWith(head))
            return false;
    }
    return true;
}

// A command card is a command line, not a miniature source editor. Preserve the
// shell meaning of a multi-statement script by putting semicolons between bare
// newlines, while continuations and newlines following operators become spaces.
// Newlines embedded in a quoted value stay visible as a literal \n marker.
inline QString singleLineCommand(const QString &raw)
{
    QString command = raw;
    command.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    command.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QString out;
    out.reserve(command.size() + 8);
    QChar quote;
    bool escaped = false;
    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (c == QLatin1Char('\n')) {
            if (escaped && quote != QLatin1Char('\'')) {
                if (!out.isEmpty() && out.endsWith(QLatin1Char('\\')))
                    out.chop(1);
                escaped = false;
                continue;
            }
            if (!quote.isNull()) {
                out += QStringLiteral("\\n");
                escaped = false;
                continue;
            }
            while (!out.isEmpty() && out.back().isSpace())
                out.chop(1);
            int next = i + 1;
            while (next < command.size() && command.at(next).isSpace()
                   && command.at(next) != QLatin1Char('\n'))
                ++next;
            const QString remainder = command.mid(next);
            if (!out.isEmpty() && next < command.size()) {
                if (shellLineBreakNeedsSeparator(out, remainder))
                    out += QStringLiteral("; ");
                else
                    out += QLatin1Char(' ');
            }
            i = next - 1;
            escaped = false;
            continue;
        }
        out += c;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
            escaped = true;
            continue;
        }
        if (quote.isNull() && (c == QLatin1Char('\'') || c == QLatin1Char('"')))
            quote = c;
        else if (c == quote)
            quote = QChar();
    }
    return out.trimmed();
}

inline QStringList shellWords(const QString &line)
{
    QStringList words;
    QString word;
    QChar quote;
    bool started = false;
    for (const QChar &c : line) {
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            else
                word += c;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            started = true;
            continue;
        }
        if (c.isSpace()) {
            if (started)
                words << word;
            word.clear();
            started = false;
            continue;
        }
        word += c;
        started = true;
    }
    if (started)
        words << word;
    return words;
}

inline bool isShellOperator(const QString &token)
{
    static const QSet<QString> ops{
        QStringLiteral("&&"), QStringLiteral("||"), QStringLiteral("|"),
        QStringLiteral(";"),  QStringLiteral("&"),  QStringLiteral(">"),
        QStringLiteral(">>"), QStringLiteral("<"),  QStringLiteral("2>&1")};
    return ops.contains(token);
}

inline bool exploreLine(const QString &command, QString &verb, QString &target)
{
    const QString line = command.trimmed();
    if (line.isEmpty() || line.contains(QLatin1Char('\n')))
        return false;
    QStringList words = shellWords(line);
    if (words.isEmpty())
        return false;
    for (const QString &w : words)
        if (isShellOperator(w) || w.startsWith(QLatin1Char('>'))
            || w.startsWith(QLatin1Char('<')))
            return false;
    const QString cmd = words.takeFirst().section(QLatin1Char('/'), -1);

    QStringList args;
    for (const QString &w : words)
        if (!w.startsWith(QLatin1Char('-')))
            args << w;

    if (cmd == QLatin1String("cat") || cmd == QLatin1String("sed")
        || cmd == QLatin1String("head") || cmd == QLatin1String("tail")
        || cmd == QLatin1String("nl") || cmd == QLatin1String("wc")) {
        if (args.isEmpty())
            return false;
        verb = QStringLiteral("Read");
        target = args.last().section(QLatin1Char('/'), -1);
        return !target.isEmpty();
    }
    if (cmd == QLatin1String("rg") || cmd == QLatin1String("grep")
        || cmd == QLatin1String("ag") || cmd == QLatin1String("ack")) {
        if (args.isEmpty())
            return false; // a bare "rg --files" isn't a search for anything
        verb = QStringLiteral("Search");
        target = args.first();
        if (args.size() > 1)
            target += QStringLiteral(" in ")
                      + args.at(1).section(QLatin1Char('/'), -1);
        return true;
    }
    if (cmd == QLatin1String("ls") || cmd == QLatin1String("find")) {
        verb = QStringLiteral("List");
        target = args.isEmpty() ? QStringLiteral(".") : args.first();
        return true;
    }
    return false;
}

inline QString toolVerb(const QString &name)
{
    if (name == QLatin1String("FileChange") || name == QLatin1String("Edit")
        || name == QLatin1String("Write") || name == QLatin1String("MultiEdit")
        || name == QLatin1String("NotebookEdit"))
        return QStringLiteral("Edited");
    if (name == QLatin1String("WebSearch"))
        return QStringLiteral("Searched");
    if (name == QLatin1String("TodoWrite"))
        return QStringLiteral("Updated plan");
    return QString();
}

} // namespace CodexTranscriptStyle
