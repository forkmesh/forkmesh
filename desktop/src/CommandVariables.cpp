#include "CommandVariables.h"

#include "AgentScratch.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace forkmesh::commandvars {

namespace {

struct Piece {
    QString text;
    bool variable = false;
    bool quoted = false;
};

struct Word {
    QList<Piece> pieces;
    qsizetype start = -1;
    qsizetype end = -1;
    bool tainted = false;
};

struct Command {
    QList<Word> words;
    QString joiner;
    qsizetype start = -1;
    bool tainted = false;
};

struct Lexed {
    QList<Command> commands;
    bool strict = true;
};

struct Value {
    QString text;
    bool known = false;
};

struct State {
    QHash<QString, Value> values;
    bool missingEmpty = false;

    Value value(const QString &name) const
    {
        const auto it = values.constFind(name);
        return it != values.constEnd() ? *it : Value{QString(), missingEmpty};
    }
};

struct Expanded {
    QString value;
    QString mask;
    bool known = true;
    bool unsafe = false;
};

struct Assignment {
    QString name;
    QList<Piece> value;
    QString raw;
};

bool nameStart(QChar c)
{
    return c == QLatin1Char('_') || (c.unicode() < 128 && c.isLetter());
}

bool nameChar(QChar c)
{
    return nameStart(c) || (c.unicode() < 128 && c.isDigit());
}

bool validName(const QString &text)
{
    if (text.isEmpty() || !nameStart(text.at(0)))
        return false;
    for (const QChar c : text)
        if (!nameChar(c))
            return false;
    return true;
}

bool globChar(QChar c)
{
    return c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('[');
}

Lexed lex(const QString &command)
{
    Lexed out;
    Command current;
    Word word;
    bool inWord = false;
    bool redirectTarget = false;
    const qsizetype size = command.size();
    const auto at = [&command, size](qsizetype i) { return i < size ? command.at(i) : QChar(); };

    const auto add = [&](const QString &text, bool variable, bool quoted, qsizetype from,
                         qsizetype to) {
        if (!inWord)
            word.start = from;
        word.end = to;
        inWord = true;
        if (text.isEmpty() && !variable)
            return;
        if (!variable && !word.pieces.isEmpty() && !word.pieces.last().variable &&
            word.pieces.last().quoted == quoted)
            word.pieces.last().text += text;
        else
            word.pieces.append({text, variable, quoted});
    };
    const auto endWord = [&] {
        if (inWord) {
            if (redirectTarget) {
                redirectTarget = false;
            } else {
                if (current.words.isEmpty())
                    current.start = word.start;
                current.words.append(word);
            }
        }
        word = Word();
        inWord = false;
    };
    const auto endCommand = [&](const QString &joiner) {
        endWord();
        if (redirectTarget) {
            out.strict = false;
            redirectTarget = false;
        }
        if (!current.words.isEmpty()) {
            out.commands.append(current);
            current = Command();
            current.joiner = joiner;
        } else if (current.joiner != QLatin1String("&&") && current.joiner != QLatin1String("|")) {
            current.joiner = joiner;
        }
    };
    const auto dollar = [&](qsizetype i, bool quoted) -> qsizetype {
        const QChar next = at(i + 1);
        if (next == QLatin1Char('{')) {
            const qsizetype close = command.indexOf(QLatin1Char('}'), i + 2);
            const QString inner = close < 0 ? QString() : command.mid(i + 2, close - i - 2);
            if (close >= 0 && validName(inner)) {
                add(inner, true, quoted, i, close + 1);
                return close;
            }
            out.strict = false;
            const qsizetype stop = close < 0 ? size - 1 : close;
            add(command.mid(i, stop - i + 1), false, true, i, stop + 1);
            word.tainted = true;
            return stop;
        }
        if (nameStart(next)) {
            qsizetype j = i + 1;
            while (j < size && nameChar(command.at(j)))
                ++j;
            add(command.mid(i + 1, j - i - 1), true, quoted, i, j);
            return j - 1;
        }
        if (quoted && next == QLatin1Char('"')) {
            add(QStringLiteral("$"), false, true, i, i + 1);
            return i;
        }
        if (next == QLatin1Char('(') || next == QLatin1Char('\'') || next == QLatin1Char('"')) {
            out.strict = false;
            add(QStringLiteral("$"), false, true, i, i + 1);
            word.tainted = true;
            return i;
        }
        if (next.isDigit() || QStringLiteral("?@*#$!-").contains(next)) {
            out.strict = false;
            add(command.mid(i, 2), false, true, i, i + 2);
            word.tainted = true;
            return i + 1;
        }
        add(QStringLiteral("$"), false, true, i, i + 1);
        return i;
    };
    const auto redirect = [&](qsizetype i) -> qsizetype {
        const QChar c = command.at(i);
        qsizetype j = i + 1;
        if (c == QLatin1Char('<') && at(j) == QLatin1Char('<')) {
            out.strict = false;
            return j;
        }
        if (at(j) == QLatin1Char('(')) {
            out.strict = false;
            return i;
        }
        if (c == QLatin1Char('>') && (at(j) == QLatin1Char('>') || at(j) == QLatin1Char('|')))
            ++j;
        else if (c == QLatin1Char('<') && at(j) == QLatin1Char('>'))
            ++j;
        if (at(j) == QLatin1Char('&')) {
            ++j;
            qsizetype k = j;
            while (k < size && (command.at(k).isDigit() || command.at(k) == QLatin1Char('-')))
                ++k;
            if (k > j)
                return k - 1;
        }
        redirectTarget = true;
        return j - 1;
    };

    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = command.at(i);
        const QChar next = at(i + 1);
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r')) {
            endWord();
        } else if (c == QLatin1Char('\\')) {
            if (next == QLatin1Char('\n')) {
                ++i;
            } else if (next.isNull()) {
                add(QStringLiteral("\\"), false, true, i, i + 1);
            } else {
                add(QString(next), false, true, i, i + 2);
                ++i;
            }
        } else if (c == QLatin1Char('\'')) {
            const qsizetype close = command.indexOf(QLatin1Char('\''), i + 1);
            if (close < 0) {
                out.strict = false;
                add(command.mid(i + 1), false, true, i, size);
                word.tainted = true;
                break;
            }
            add(command.mid(i + 1, close - i - 1), false, true, i, close + 1);
            i = close;
        } else if (c == QLatin1Char('"')) {
            add(QString(), false, true, i, i + 1);
            bool closed = false;
            for (++i; i < size; ++i) {
                const QChar d = command.at(i);
                if (d == QLatin1Char('"')) {
                    closed = true;
                    word.end = i + 1;
                    break;
                }
                if (d == QLatin1Char('\\') && QStringLiteral("$`\"\\\n").contains(at(i + 1))) {
                    if (at(i + 1) != QLatin1Char('\n'))
                        add(QString(at(i + 1)), false, true, i, i + 2);
                    ++i;
                } else if (d == QLatin1Char('$')) {
                    i = dollar(i, true);
                } else {
                    add(QString(d), false, true, i, i + 1);
                    if (d == QLatin1Char('`')) {
                        out.strict = false;
                        word.tainted = true;
                    }
                }
            }
            if (!closed) {
                out.strict = false;
                word.tainted = true;
            }
        } else if (c == QLatin1Char('$')) {
            i = dollar(i, false);
        } else if (c == QLatin1Char('#') && !inWord) {
            while (i + 1 < size && command.at(i + 1) != QLatin1Char('\n'))
                ++i;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char(';')) {
            if (c == QLatin1Char(';') && next == QLatin1Char(';'))
                out.strict = false;
            endCommand(QStringLiteral(";"));
        } else if (c == QLatin1Char('&')) {
            if (next == QLatin1Char('&')) {
                endCommand(QStringLiteral("&&"));
                ++i;
            } else if (next == QLatin1Char('>')) {
                endWord();
                i = redirect(i + 1);
            } else {
                out.strict = false;
                endCommand(QStringLiteral(";"));
            }
        } else if (c == QLatin1Char('|')) {
            if (next == QLatin1Char('|')) {
                out.strict = false;
                endCommand(QStringLiteral(";"));
                ++i;
            } else {
                if (next == QLatin1Char('&'))
                    ++i;
                endCommand(QStringLiteral("|"));
            }
        } else if (c == QLatin1Char('<') || c == QLatin1Char('>')) {
            const bool descriptor =
                inWord && word.pieces.size() == 1 && !word.pieces.first().variable &&
                !word.pieces.first().quoted &&
                std::all_of(word.pieces.first().text.cbegin(), word.pieces.first().text.cend(),
                            [](QChar d) { return d.isDigit(); });
            if (descriptor) {
                word = Word();
                inWord = false;
            } else {
                endWord();
            }
            i = redirect(i);
        } else if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('{') ||
                   c == QLatin1Char('}') || c == QLatin1Char('`')) {
            out.strict = false;
            if ((inWord || !current.words.isEmpty()) && c != QLatin1Char(')') &&
                c != QLatin1Char('}'))
                current.tainted = true;
            endCommand(QStringLiteral(";"));
        } else if (c == QLatin1Char('~') && !inWord &&
                   (next.isNull() || next == QLatin1Char('/') || next.isSpace())) {
            add(QStringLiteral("HOME"), true, true, i, i + 1);
        } else {
            add(QString(c), false, false, i, i + 1);
        }
    }
    endCommand(QStringLiteral(";"));
    return out;
}

Expanded expand(const QList<Piece> &pieces, const State &state)
{
    Expanded out;
    for (const Piece &piece : pieces) {
        if (piece.variable) {
            const Value value = state.value(piece.text);
            if (!value.known)
                out.known = false;
            out.value += value.text;
            out.mask += QString(value.text.size(), QLatin1Char(' '));
            if (!piece.quoted)
                for (const QChar c : value.text)
                    if (c.isSpace() || globChar(c))
                        out.unsafe = true;
            continue;
        }
        out.value += piece.text;
        for (const QChar c : piece.text)
            out.mask += !piece.quoted && globChar(c) ? QLatin1Char('g') : QLatin1Char(' ');
    }
    return out;
}

bool assignment(const Word &word, const QString &command, Assignment *out)
{
    static const QRegularExpression name(QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)="));
    if (word.pieces.isEmpty() || word.pieces.first().variable || word.pieces.first().quoted)
        return false;
    const QRegularExpressionMatch match = name.match(word.pieces.first().text);
    if (!match.hasMatch())
        return false;
    out->name = match.captured(1);
    out->value = word.pieces;
    out->value.first().text = out->value.first().text.mid(match.capturedLength());
    if (out->value.first().text.isEmpty())
        out->value.removeFirst();
    for (Piece &piece : out->value)
        piece.quoted = true;
    const QString raw = command.mid(word.start, word.end - word.start);
    out->raw = raw.mid(raw.indexOf(QLatin1Char('=')) + 1);
    return true;
}

bool plainWord(const Word &word, const QString &text)
{
    return word.pieces.size() == 1 && !word.pieces.first().variable &&
           word.pieces.first().text == text;
}

bool special(const QString &name)
{
    static const QSet<QString> names{
        QStringLiteral("UID"),         QStringLiteral("EUID"),        QStringLiteral("PPID"),
        QStringLiteral("SHELLOPTS"),   QStringLiteral("RANDOM"),      QStringLiteral("SRANDOM"),
        QStringLiteral("SECONDS"),     QStringLiteral("LINENO"),      QStringLiteral("HISTCMD"),
        QStringLiteral("FUNCNAME"),    QStringLiteral("GROUPS"),      QStringLiteral("DIRSTACK"),
        QStringLiteral("EPOCHSECONDS"), QStringLiteral("EPOCHREALTIME"), QStringLiteral("PWD"),
        QStringLiteral("OLDPWD"),      QStringLiteral("IFS"),         QStringLiteral("GLOBIGNORE")};
    return names.contains(name) || name.startsWith(QLatin1String("BASH"));
}

using Visit = std::function<bool(const Lexed &, qsizetype, const State &)>;
using Assigned = std::function<void(const Assignment &, const Expanded &)>;

void walk(const QString &command, const Lexed &lexed, State &state, const Visit &visit,
          const Assigned &assigned = {}, bool trust = true)
{
    QSet<QString> pending;
    bool ran = false;
    for (qsizetype i = 0; i < lexed.commands.size(); ++i) {
        const Command &current = lexed.commands.at(i);
        if (current.joiner != QLatin1String("&&") && current.joiner != QLatin1String("|")) {
            for (const QString &name : std::as_const(pending))
                state.values.insert(name, Value());
            pending.clear();
            ran = false;
        }
        const bool pipeline =
            current.joiner == QLatin1String("|") ||
            (i + 1 < lexed.commands.size() &&
             lexed.commands.at(i + 1).joiner == QLatin1String("|"));
        if (!visit(lexed, i, state))
            return;
        QList<Assignment> leading;
        qsizetype w = 0;
        Assignment found;
        while (w < current.words.size() && assignment(current.words.at(w), command, &found)) {
            leading << found;
            ++w;
        }
        if (w == 0 && !current.words.isEmpty() &&
            plainWord(current.words.first(), QStringLiteral("export"))) {
            QList<Assignment> exported;
            bool plain = true;
            for (qsizetype k = 1; k < current.words.size() && plain; ++k) {
                const Word &word = current.words.at(k);
                if (assignment(word, command, &found))
                    exported << found;
                else if (word.pieces.size() != 1 || word.pieces.first().variable ||
                         !validName(word.pieces.first().text))
                    plain = false;
            }
            if (plain) {
                leading = exported;
                w = current.words.size();
            }
        }
        if (w >= current.words.size()) {
            for (const Assignment &entry : std::as_const(leading)) {
                const Expanded value = expand(entry.value, state);
                if (assigned)
                    assigned(entry, value);
                state.values.insert(entry.name,
                                    Value{value.value, trust && value.known && !pipeline &&
                                                           !special(entry.name)});
                if (ran)
                    pending.insert(entry.name);
            }
            continue;
        }
        for (const Assignment &entry : std::as_const(leading))
            if (assigned)
                assigned(entry, expand(entry.value, state));
        for (const Assignment &entry : std::as_const(leading))
            state.values.insert(entry.name, Value());
        const Expanded program = expand(current.words.at(w).pieces, state);
        const QString name = program.value.section(QLatin1Char('/'), -1);
        if (name == QLatin1String("cd") || name == QLatin1String("pushd") ||
            name == QLatin1String("popd")) {
            state.values.insert(QStringLiteral("PWD"), Value());
            state.values.insert(QStringLiteral("OLDPWD"), Value());
        }
        ran = true;
    }
}

State environmentState(const Environment &environment)
{
    State state;
    state.missingEmpty = environment.sent;
    for (auto it = environment.values.cbegin(); it != environment.values.cend(); ++it)
        state.values.insert(it.key(), Value{it.value(), true});
    for (const QString &name : environment.hidden)
        state.values.insert(name, Value());
    return state;
}

bool recursive(const QList<Word> &words, const State &state)
{
    for (const Word &word : words) {
        const Expanded value = expand(word.pieces, state);
        if (!value.known)
            return true;
        if (value.value == QLatin1String("--"))
            return false;
        if (value.value == QLatin1String("--recursive"))
            return true;
        if (value.value.size() > 1 && value.value.startsWith(QLatin1Char('-')) &&
            !value.value.startsWith(QLatin1String("--")) &&
            (value.value.contains(QLatin1Char('r')) || value.value.contains(QLatin1Char('R'))))
            return true;
    }
    return false;
}

bool targetInside(const Expanded &target, const QString &root, const QString &canonicalRoot)
{
    const QString &path = target.value;
    if (!path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/')))
        return false;
    for (const QString &part : path.mid(1).split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    for (qsizetype k = 0; k < target.mask.size(); ++k)
        if (target.mask.at(k) == QLatin1Char('g') &&
            (k < slash || path.at(k) == QLatin1Char('[')))
            return false;
    if (target.mask.mid(slash + 1).contains(QLatin1Char('g')) &&
        path.at(slash + 1) == QLatin1Char('.'))
        return false;
    const QString parent = slash == 0 ? QStringLiteral("/") : path.left(slash);
    if (parent != root && !parent.startsWith(root + QLatin1Char('/')))
        return false;
    QString existing = parent;
    while (!QFileInfo::exists(existing)) {
        if (QFileInfo(existing).isSymLink() || existing == root)
            return false;
        existing = QFileInfo(existing).path();
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    return canonical == canonicalRoot || canonical.startsWith(canonicalRoot + QLatin1Char('/'));
}

bool targetsInside(const QList<Word> &words, const State &state, const QString &root,
                   const QString &canonicalRoot)
{
    bool literal = false;
    bool any = false;
    for (const Word &word : words) {
        const Expanded value = expand(word.pieces, state);
        if (!value.known || value.unsafe)
            return false;
        if (!literal && value.value == QLatin1String("--")) {
            literal = true;
            continue;
        }
        if (!literal && value.value.startsWith(QLatin1Char('-'))) {
            if (value.mask.contains(QLatin1Char('g')))
                return false;
            continue;
        }
        if (!targetInside(value, root, canonicalRoot))
            return false;
        any = true;
    }
    return any;
}

bool harmless(const QString &program, const QList<Word> &arguments, const State &state)
{
    static const QSet<QString> programs{
        QStringLiteral("cd"),    QStringLiteral("pushd"), QStringLiteral("popd"),
        QStringLiteral("echo"),  QStringLiteral("printf"), QStringLiteral("true"),
        QStringLiteral("false"), QStringLiteral(":"),     QStringLiteral("test"),
        QStringLiteral("["),     QStringLiteral("ls"),    QStringLiteral("pwd"),
        QStringLiteral("mkdir"), QStringLiteral("touch"), QStringLiteral("sleep"),
        QStringLiteral("cat"),   QStringLiteral("head"),  QStringLiteral("tail"),
        QStringLiteral("wc"),    QStringLiteral("date"),  QStringLiteral("set"),
        QStringLiteral("du"),    QStringLiteral("df"),    QStringLiteral("stat"),
        QStringLiteral("which"), QStringLiteral("whoami"), QStringLiteral("rm")};
    if (!programs.contains(program))
        return false;
    if (program == QLatin1String("printf"))
        for (const Word &word : arguments)
            if (expand(word.pieces, state).value == QLatin1String("-v"))
                return false;
    return true;
}

} // namespace

QString trustedScratch(const Environment &environment)
{
    const QString value = environment.values.value(ScratchName);
    if (value.isEmpty() || !QDir::isAbsolutePath(value) || QDir::cleanPath(value) != value)
        return QString();
    const QFileInfo info(value);
    if (!info.isDir() || info.fileName().startsWith(QLatin1Char('.')))
        return QString();
    const QFileInfo parent(info.absolutePath());
    if (parent.fileName() != QLatin1String(".scratch") ||
        !agentscratch::isAgentRoot(parent.absolutePath()))
        return QString();
    return value;
}

bool pathInside(const QString &path, const QString &root)
{
    if (root.isEmpty() || path.contains(QLatin1String("..")))
        return false;
    return QDir::cleanPath(path).startsWith(QDir::cleanPath(root) + QLatin1Char('/'));
}

bool deletesOnlyInside(const QString &command, const QString &root)
{
    if (root.isEmpty())
        return false;
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return false;
    const Lexed lexed = lex(command);
    if (!lexed.strict)
        return false;
    static const QSet<QString> wrappers{
        QStringLiteral("sudo"),    QStringLiteral("doas"),   QStringLiteral("env"),
        QStringLiteral("time"),    QStringLiteral("nice"),   QStringLiteral("nohup"),
        QStringLiteral("exec"),    QStringLiteral("command"), QStringLiteral("builtin"),
        QStringLiteral("xargs"),   QStringLiteral("stdbuf"), QStringLiteral("ionice"),
        QStringLiteral("proxychains"), QStringLiteral("torsocks"), QStringLiteral("timeout")};
    State state;
    state.values.insert(ScratchName, Value{root, true});
    bool blocked = false;
    bool failed = false;
    bool any = false;
    walk(command, lexed, state, [&](const Lexed &all, qsizetype index, const State &before) {
        const Command &current = all.commands.at(index);
        qsizetype w = 0;
        Assignment found;
        while (w < current.words.size() && assignment(current.words.at(w), command, &found)) {
            if (found.name == QLatin1String("IFS") || found.name == QLatin1String("GLOBIGNORE")) {
                failed = true;
                return false;
            }
            ++w;
        }
        if (w >= current.words.size())
            return true;
        const Expanded program = expand(current.words.at(w).pieces, before);
        const QList<Word> arguments = current.words.mid(w + 1);
        bool mentionsRm = false;
        for (const Word &word : arguments)
            if (expand(word.pieces, before).value.section(QLatin1Char('/'), -1).toLower() ==
                QLatin1String("rm"))
                mentionsRm = true;
        const QString name = program.value.section(QLatin1Char('/'), -1);
        if (!program.known) {
            blocked = true;
            return true;
        }
        if (name.toLower() == QLatin1String("rm")) {
            if (!recursive(arguments, before))
                return true;
            if (blocked || !targetsInside(arguments, before, root, canonicalRoot)) {
                failed = true;
                return false;
            }
            any = true;
            return true;
        }
        if (wrappers.contains(name) && mentionsRm) {
            failed = true;
            return false;
        }
        if (name == QLatin1String("export")) {
            for (const Word &word : arguments)
                if (!assignment(word, command, &found) &&
                    (word.pieces.size() != 1 || !validName(word.pieces.first().text)))
                    blocked = true;
            return true;
        }
        if (!harmless(name, arguments, before))
            blocked = true;
        return true;
    });
    return !failed && any;
}

QList<Variable> variables(const QString &command, const Environment &environment)
{
    QList<Variable> out;
    QHash<QString, qsizetype> index;
    const Lexed lexed = lex(command);
    State state = environmentState(environment);
    const auto fromEnvironment = [&environment](const QString &name) {
        Variable variable;
        variable.name = name;
        if (environment.hidden.contains(name)) {
            variable.source = Variable::Source::Hidden;
        } else if (environment.values.contains(name)) {
            variable.source = Variable::Source::Environment;
            variable.value = environment.values.value(name);
            variable.known = true;
        } else if (environment.sent) {
            variable.source = Variable::Source::Unset;
            variable.known = true;
        }
        return variable;
    };
    walk(
        command, lexed, state,
        [&](const Lexed &all, qsizetype at, const State &) {
            for (const Word &word : all.commands.at(at).words)
                for (const Piece &piece : word.pieces)
                    if (piece.variable && !index.contains(piece.text)) {
                        index.insert(piece.text, out.size());
                        out.append(fromEnvironment(piece.text));
                    }
            return true;
        },
        [&](const Assignment &entry, const Expanded &value) {
            const auto existing = index.constFind(entry.name);
            if (existing != index.constEnd() &&
                out.at(*existing).source == Variable::Source::Command)
                return;
            Variable variable;
            variable.name = entry.name;
            variable.assigned = entry.raw;
            variable.value = value.value;
            variable.known = value.known;
            variable.source = Variable::Source::Command;
            if (existing != index.constEnd()) {
                out[*existing] = variable;
            } else {
                index.insert(entry.name, out.size());
                out.append(variable);
            }
        });
    return out;
}

QHash<QString, QString> valuesAt(const QString &command, const Environment &environment,
                                 qsizetype position)
{
    QHash<QString, QString> out;
    const Lexed lexed = lex(command);
    State state = environmentState(environment);
    State captured = state;
    walk(command, lexed, state, [&](const Lexed &all, qsizetype at, const State &before) {
        captured = before;
        return at + 1 < all.commands.size() && all.commands.at(at + 1).start <= position;
    });
    for (auto it = captured.values.cbegin(); it != captured.values.cend(); ++it)
        if (it.value().known)
            out.insert(it.key(), it.value().text);
    if (captured.missingEmpty) {
        for (const Command &current : lexed.commands)
            for (const Word &word : current.words)
                for (const Piece &piece : word.pieces)
                    if (piece.variable && !captured.values.contains(piece.text))
                        out.insert(piece.text, QString());
    }
    return out;
}

namespace {

bool definesFunction(const QString &command)
{
    static const QRegularExpression definition(QStringLiteral(
        "(^|[\\s;&|({])(function\\s+[^\\s(]|[\\w.:-]+\\s*\\(\\s*\\))"));
    return definition.match(command).hasMatch();
}

int mentions(const QString &command, const QString &name)
{
    const QRegularExpression bare(
        QStringLiteral("(?<![\\w$])(?<!\\$\\{)%1(?!\\w)").arg(QRegularExpression::escape(name)));
    int count = 0;
    for (auto it = bare.globalMatch(command); it.hasNext(); it.next())
        ++count;
    return count;
}

QStringList resolvedTarget(const Expanded &target)
{
    QString path = target.value;
    QString mask = target.mask;
    if (!path.startsWith(QLatin1Char('/')))
        return {};
    bool trailing = false;
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        if (mask.endsWith(QLatin1Char('g')))
            return {};
        path.chop(1);
        mask.chop(1);
        trailing = true;
    }
    if (path == QLatin1String("/"))
        return {path};
    for (const QString &part : path.mid(1).split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return {};
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    for (qsizetype k = 0; k < mask.size(); ++k)
        if (mask.at(k) == QLatin1Char('g') && (k < slash || path.at(k) == QLatin1Char('[')))
            return {};
    if (mask.mid(slash + 1).contains(QLatin1Char('g')) && path.at(slash + 1) == QLatin1Char('.'))
        return {};
    QString existing = trailing ? path : (slash == 0 ? QStringLiteral("/") : path.left(slash));
    while (!QFileInfo::exists(existing)) {
        if (QFileInfo(existing).isSymLink())
            return {};
        existing = QFileInfo(existing).path();
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    const QString rest = existing == QLatin1String("/") ? path.mid(1) : path.mid(existing.size() + 1);
    const QString resolved = rest.isEmpty() ? canonical
                             : canonical == QLatin1String("/") ? QLatin1Char('/') + rest
                                                               : canonical + QLatin1Char('/') + rest;
    if (resolved == path)
        return {path};
    return {path, resolved};
}

bool segmentMatches(const QString &pattern, const QString &name)
{
    if (name.contains(QLatin1Char('*')) || name.contains(QLatin1Char('?')))
        return pattern == QLatin1String("*");
    QString expression;
    for (const QChar c : pattern) {
        if (c == QLatin1Char('*'))
            expression += QStringLiteral(".*");
        else if (c == QLatin1Char('?'))
            expression += QLatin1Char('.');
        else
            expression += QRegularExpression::escape(QString(c));
    }
    return QRegularExpression(QRegularExpression::anchoredPattern(expression),
                              QRegularExpression::DotMatchesEverythingOption)
        .match(name)
        .hasMatch();
}

bool segmentsMatch(const QStringList &pattern, qsizetype p, const QStringList &path, qsizetype t)
{
    if (p == pattern.size())
        return t == path.size();
    if (pattern.at(p) == QLatin1String("**")) {
        if (p + 1 == pattern.size())
            return t < path.size();
        for (qsizetype k = t; k <= path.size(); ++k)
            if (segmentsMatch(pattern, p + 1, path, k))
                return true;
        return false;
    }
    return t < path.size() && segmentMatches(pattern.at(p), path.at(t)) &&
           segmentsMatch(pattern, p + 1, path, t + 1);
}

QStringList segments(const QString &path)
{
    return path == QLatin1String("/") ? QStringList() : path.mid(1).split(QLatin1Char('/'));
}

QString shownPath(const QString &path)
{
    const QString home = QDir::cleanPath(QDir::homePath());
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

} // namespace

QStringList removedPaths(const QString &command, const Environment &environment,
                         const QList<std::pair<qsizetype, qsizetype>> &spans)
{
    if (spans.isEmpty() || definesFunction(command) || mentions(command, QStringLiteral("IFS")))
        return {};
    const Lexed lexed = lex(command);
    State state;
    const QString scratch = trustedScratch(environment);
    if (!scratch.isEmpty())
        state.values.insert(ScratchName, Value{scratch, true});
    const QString home = QDir::cleanPath(QDir::homePath());
    const QString sentHome = environment.values.value(QStringLiteral("HOME"));
    if (!sentHome.isEmpty() && QDir::cleanPath(sentHome) == home)
        state.values.insert(QStringLiteral("HOME"), Value{home, true});
    QHash<QString, int> assignedCount;
    QSet<QString> used;
    QList<qsizetype> analysed;
    QStringList out;
    bool failed = false;
    bool linked = false;
    static const QSet<QString> evaluators{QStringLiteral("eval"), QStringLiteral("source"),
                                          QStringLiteral(".")};
    static const QSet<QString> linkers{QStringLiteral("ln"), QStringLiteral("cp"),
                                       QStringLiteral("mv")};
    const auto flagged = [&spans](qsizetype start) {
        return std::any_of(spans.cbegin(), spans.cend(), [start](const auto &span) {
            return start >= span.first && start < span.first + qMax<qsizetype>(span.second, 1);
        });
    };
    walk(
        command, lexed, state,
        [&](const Lexed &all, qsizetype index, const State &before) {
            const Command &current = all.commands.at(index);
            qsizetype w = 0;
            Assignment found;
            while (w < current.words.size() && assignment(current.words.at(w), command, &found))
                ++w;
            if (w >= current.words.size())
                return true;
            const Expanded program = expand(current.words.at(w).pieces, before);
            if (!program.known)
                return true;
            const QString name = program.value.section(QLatin1Char('/'), -1);
            if (evaluators.contains(name)) {
                failed = true;
                return false;
            }
            if (linkers.contains(name)) {
                linked = true;
                return true;
            }
            if (name != QLatin1String("rm") || !flagged(current.start))
                return true;
            const QList<Word> arguments = current.words.mid(w + 1);
            if (current.tainted || linked || !recursive(arguments, before)) {
                failed = true;
                return false;
            }
            bool literal = false;
            bool any = false;
            for (const Word &word : arguments) {
                const Expanded value = expand(word.pieces, before);
                if (word.tainted || !value.known || value.unsafe) {
                    failed = true;
                    return false;
                }
                for (const Piece &piece : word.pieces)
                    if (piece.variable)
                        used.insert(piece.text);
                if (!literal && value.value == QLatin1String("--")) {
                    literal = true;
                    continue;
                }
                if (!literal && value.value.startsWith(QLatin1Char('-'))) {
                    if (value.mask.contains(QLatin1Char('g'))) {
                        failed = true;
                        return false;
                    }
                    continue;
                }
                const QStringList resolved = resolvedTarget(value);
                if (resolved.isEmpty()) {
                    failed = true;
                    return false;
                }
                for (const QString &path : resolved)
                    if (!out.contains(path))
                        out.append(path);
                any = true;
            }
            if (!any) {
                failed = true;
                return false;
            }
            analysed.append(current.start);
            return true;
        },
        [&](const Assignment &entry, const Expanded &) { ++assignedCount[entry.name]; },
        lexed.strict);
    if (failed)
        return {};
    for (const auto &span : spans)
        if (std::none_of(analysed.cbegin(), analysed.cend(), [&span](qsizetype start) {
                return start >= span.first && start < span.first + qMax<qsizetype>(span.second, 1);
            }))
            return {};
    for (const QString &name : std::as_const(used))
        if (mentions(command, name) > assignedCount.value(name))
            return {};
    return out;
}

QString expandDeletePattern(const QString &pattern, const QString &scratch)
{
    QString text = pattern.trimmed();
    const QString home = QDir::cleanPath(QDir::homePath());
    const auto replacePrefix = [&text](const QString &prefix, const QString &value) {
        if (text != prefix && !text.startsWith(prefix + QLatin1Char('/')))
            return false;
        text = value + text.mid(prefix.size());
        return true;
    };
    if (!replacePrefix(QStringLiteral("~"), home) && !replacePrefix(QStringLiteral("$HOME"), home) &&
        !replacePrefix(QStringLiteral("${HOME}"), home)) {
        const bool scratchVariable =
            text.startsWith(QLatin1Char('$')) &&
            (replacePrefix(QLatin1Char('$') + ScratchName, scratch) ||
             replacePrefix(QStringLiteral("${%1}").arg(ScratchName), scratch));
        if (scratchVariable && scratch.isEmpty())
            return {};
    }
    if (text.contains(QLatin1Char('$')) || !text.startsWith(QLatin1Char('/')))
        return {};
    while (text.size() > 1 && text.endsWith(QLatin1Char('/')))
        text.chop(1);
    for (const QString &part : segments(text))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return {};
    return text;
}

QString deletePatternProblem(const QString &pattern)
{
    const QString expanded =
        expandDeletePattern(pattern, QStringLiteral("/forkmesh-scratch/agent"));
    if (expanded.isEmpty())
        return QStringLiteral("Start the pattern with /, ~ or $%1, use no other variables, "
                              "and leave out “.” and “..”.")
            .arg(ScratchName);
    QString prefix;
    for (const QString &part : segments(expanded)) {
        if (part.contains(QLatin1Char('*')) || part.contains(QLatin1Char('?')))
            break;
        prefix += QLatin1Char('/') + part;
    }
    const QString home = QDir::cleanPath(QDir::homePath());
    if (prefix.isEmpty() || prefix == home || home.startsWith(prefix + QLatin1Char('/')))
        return QStringLiteral("Too broad: name a folder below your home folder before the "
                              "first * or **.");
    return {};
}

bool deletePatternMatches(const QString &pattern, const QString &path, const QString &scratch)
{
    const QString expanded = expandDeletePattern(pattern, scratch);
    if (expanded.isEmpty() || !deletePatternProblem(pattern).isEmpty())
        return false;
    return segmentsMatch(segments(expanded), 0, segments(QDir::cleanPath(path)), 0);
}

bool protectedDeletePath(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    const QString home = QDir::cleanPath(QDir::homePath());
    if (clean == QLatin1String("/") || clean == home || home.startsWith(clean + QLatin1Char('/')))
        return true;
    if (segments(clean).contains(QStringLiteral(".git")))
        return true;
    static const QRegularExpression guarded(QStringLiteral(
        "(/projects/forkmesh/(data|config|ssh|state|logs)(/|$)|/\\.forkmesh(/|$)|"
        "/\\.config/ForkMesh(/|$)|/\\.local/share/ForkMesh(/|$)|/projects/forkmesh-backup(/|$)|"
        "/projects(/forkmesh(/src)?)?$|/\\.worktrees(/[^/]+)?$)"));
    return guarded.match(clean).hasMatch();
}

QString uncoveredDeletePath(const QStringList &patterns, const QStringList &paths,
                            const QString &scratch)
{
    if (paths.isEmpty())
        return QStringLiteral("?");
    for (const QString &path : paths) {
        if (protectedDeletePath(path))
            return path;
        if (std::none_of(patterns.cbegin(), patterns.cend(), [&](const QString &pattern) {
                return deletePatternMatches(pattern, path, scratch);
            }))
            return path;
    }
    return {};
}

bool deletesAllowed(const QStringList &patterns, const QStringList &paths, const QString &scratch)
{
    return !patterns.isEmpty() && !paths.isEmpty() &&
           uncoveredDeletePath(patterns, paths, scratch).isEmpty();
}

QStringList suggestedDeletePatterns(const QStringList &paths, const QString &scratch)
{
    if (paths.isEmpty())
        return {};
    QStringList candidates;
    if (!scratch.isEmpty())
        candidates << QStringLiteral("$%1/**").arg(ScratchName);
    for (const QString &path : paths) {
        const qsizetype at = path.lastIndexOf(QLatin1String("/.worktrees/"));
        if (at < 0)
            continue;
        const QString root = shownPath(path.left(at)) + QStringLiteral("/.worktrees");
        candidates << root + QStringLiteral("/.scratch/*/**") << root + QStringLiteral("/*/**");
        break;
    }
    QString common = QFileInfo(paths.first()).path();
    for (const QString &path : paths)
        while (common != QLatin1String("/") && !path.startsWith(common + QLatin1Char('/')))
            common = QFileInfo(common).path();
    if (common != QLatin1String("/"))
        candidates << shownPath(common) + QStringLiteral("/**");
    QStringList out;
    for (const QString &candidate : std::as_const(candidates))
        if (!out.contains(candidate) && deletePatternProblem(candidate).isEmpty() &&
            deletesAllowed({candidate}, paths, scratch))
            out << candidate;
    return out;
}

} // namespace forkmesh::commandvars
