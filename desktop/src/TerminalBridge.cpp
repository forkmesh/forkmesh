#include "TerminalBridge.h"

#include <QJsonArray>
#include <QRandomGenerator>

namespace forkmesh::terminalbridge {

namespace {

constexpr int kIdChars = 16;

QString text(const QJsonObject &params, const char *key)
{
    return params.value(QLatin1String(key)).toString();
}

bool fail(QString *error, const char *code)
{
    if (error)
        *error = QString::fromLatin1(code);
    return false;
}

QString randomHex(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator *random = QRandomGenerator::system();
    for (char &byte : raw)
        byte = char(random->bounded(256));
    return QString::fromLatin1(raw.toHex());
}

}

QStringList kinds()
{
    return {ShellKind, ClaudeKind, CodexKind, CommandKind};
}

QString kindLabel(const QString &kind)
{
    if (kind == ClaudeKind)
        return QStringLiteral("Claude Code");
    if (kind == CodexKind)
        return QStringLiteral("Codex");
    if (kind == CommandKind)
        return QStringLiteral("Command");
    return QStringLiteral("Shell");
}

bool validTerminalId(const QString &id)
{
    if (id.size() != kIdChars)
        return false;
    for (const QChar c : id)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    return true;
}

QString newTerminalId()
{
    return randomHex(kIdChars / 2);
}

QString newToken()
{
    return QStringLiteral("term.%1").arg(randomHex(32));
}

QByteArray keyBytes(const QString &name, bool appCursorKeys, bool *ok)
{
    if (ok)
        *ok = true;
    QString key = name.trimmed().toLower();
    key.replace(QLatin1Char('+'), QLatin1Char('-'));
    const auto arrow = [appCursorKeys](char c) {
        return appCursorKeys ? QByteArray("\x1bO") + c : QByteArray("\x1b[") + c;
    };
    if (key == QLatin1String("enter") || key == QLatin1String("return"))
        return QByteArrayLiteral("\r");
    if (key == QLatin1String("tab"))
        return QByteArrayLiteral("\t");
    if (key == QLatin1String("escape") || key == QLatin1String("esc"))
        return QByteArrayLiteral("\x1b");
    if (key == QLatin1String("backspace"))
        return QByteArrayLiteral("\x7f");
    if (key == QLatin1String("space"))
        return QByteArrayLiteral(" ");
    if (key == QLatin1String("up"))
        return arrow('A');
    if (key == QLatin1String("down"))
        return arrow('B');
    if (key == QLatin1String("right"))
        return arrow('C');
    if (key == QLatin1String("left"))
        return arrow('D');
    if (key == QLatin1String("home"))
        return QByteArrayLiteral("\x1b[H");
    if (key == QLatin1String("end"))
        return QByteArrayLiteral("\x1b[F");
    if (key == QLatin1String("insert"))
        return QByteArrayLiteral("\x1b[2~");
    if (key == QLatin1String("delete"))
        return QByteArrayLiteral("\x1b[3~");
    if (key == QLatin1String("pageup"))
        return QByteArrayLiteral("\x1b[5~");
    if (key == QLatin1String("pagedown"))
        return QByteArrayLiteral("\x1b[6~");
    if (key.startsWith(QLatin1String("ctrl-")) && key.size() == 6) {
        const QChar letter = key.at(5);
        if (letter >= QLatin1Char('a') && letter <= QLatin1Char('z'))
            return QByteArray(1, char(letter.unicode() - 'a' + 1));
    }
    if (ok)
        *ok = false;
    return {};
}

QString terminalParam(const QJsonObject &params)
{
    return text(params, "terminal").trimmed().toLower();
}

bool parseOpenRequest(const QJsonObject &params, OpenRequest *out, QString *error)
{
    OpenRequest request;
    const QString kind = text(params, "kind").trimmed().toLower();
    request.kind = kind.isEmpty() ? ShellKind : kind;
    if (!kinds().contains(request.kind))
        return fail(error, "bad_kind");
    request.command = text(params, "command").trimmed();
    if (request.command.size() > kMaxCommandChars || request.command.contains(QChar(0)))
        return fail(error, "bad_command");
    if (request.kind == CommandKind && request.command.isEmpty())
        return fail(error, "bad_command");
    if (request.kind != CommandKind && !request.command.isEmpty())
        return fail(error, "bad_command");
    const QString repository = text(params, "repository").trimmed();
    if (!repository.isEmpty()) {
        const QStringList parts = repository.split(QLatin1Char('/'));
        if (parts.size() != 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
            return fail(error, "bad_repository");
        request.owner = parts.at(0);
        request.name = parts.at(1);
    }
    request.title = text(params, "title").simplified();
    if (request.title.size() > kMaxTitleChars)
        return fail(error, "bad_title");
    if (out)
        *out = request;
    return true;
}

bool parseInputRequest(const QJsonObject &params, bool appCursorKeys, InputRequest *out,
                       QString *error)
{
    InputRequest request;
    request.terminal = terminalParam(params);
    if (!validTerminalId(request.terminal))
        return fail(error, "bad_terminal");
    const QJsonValue textValue = params.value(QStringLiteral("text"));
    if (!textValue.isUndefined() && !textValue.isNull() && !textValue.isString())
        return fail(error, "bad_text");
    QString typed = textValue.toString();
    if (typed.size() > kMaxInputChars || typed.contains(QChar(0)))
        return fail(error, "bad_text");
    typed.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    typed.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    request.bytes = typed.toUtf8();
    QStringList summary;
    if (!typed.isEmpty())
        summary << QStringLiteral("typed “%1”")
                       .arg(typed.simplified().left(60));
    const QJsonValue keysValue = params.value(QStringLiteral("keys"));
    if (!keysValue.isUndefined() && !keysValue.isNull()) {
        if (!keysValue.isArray() || keysValue.toArray().size() > kMaxKeys)
            return fail(error, "bad_keys");
        QStringList names;
        for (const QJsonValue &key : keysValue.toArray()) {
            bool ok = false;
            const QByteArray bytes = keyBytes(key.toString(), appCursorKeys, &ok);
            if (!key.isString() || !ok)
                return fail(error, "bad_keys");
            request.bytes += bytes;
            names << key.toString().trimmed().toLower();
        }
        if (!names.isEmpty())
            summary << QStringLiteral("pressed %1").arg(names.join(QStringLiteral(", ")));
    }
    const QJsonValue enter = params.value(QStringLiteral("enter"));
    if (!enter.isUndefined() && !enter.isNull() && !enter.isBool())
        return fail(error, "bad_enter");
    if (enter.toBool()) {
        request.bytes += '\r';
        summary << QStringLiteral("pressed enter");
    }
    if (request.bytes.isEmpty())
        return fail(error, "empty_input");
    request.summary = summary.join(QStringLiteral(", "));
    if (out)
        *out = request;
    return true;
}

bool parseReadRequest(const QJsonObject &params, ReadRequest *out, QString *error)
{
    ReadRequest request;
    request.terminal = terminalParam(params);
    if (!validTerminalId(request.terminal))
        return fail(error, "bad_terminal");
    const QJsonValue lines = params.value(QStringLiteral("lines"));
    if (!lines.isUndefined() && !lines.isNull()) {
        if (!lines.isDouble() || lines.toDouble() != double(lines.toInt()) ||
            lines.toInt() < 1 || lines.toInt() > kMaxReadLines)
            return fail(error, "bad_lines");
        request.lines = lines.toInt();
    }
    if (out)
        *out = request;
    return true;
}

QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString tomlString(const QString &value)
{
    QString out = QStringLiteral("\"");
    for (const QChar ch : value) {
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\\'))
            out += QLatin1Char('\\') + QString(ch);
        else if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            out += QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QLatin1Char('0'));
        else
            out += ch;
    }
    return out + QLatin1Char('"');
}

QString launchCommand(const QString &kind, const QString &command, const QString &mcpConfigPath,
                      bool windows, const QString &python, const QString &mcpServerScript)
{
    if (kind == ClaudeKind) {
        if (mcpConfigPath.isEmpty())
            return QStringLiteral("claude");
        return QStringLiteral("claude --mcp-config %1")
            .arg(windows ? QLatin1Char('"') + mcpConfigPath + QLatin1Char('"')
                         : shellQuote(mcpConfigPath));
    }
    if (kind == CodexKind) {
        if (windows || mcpConfigPath.isEmpty() || python.isEmpty() || mcpServerScript.isEmpty())
            return QStringLiteral("codex");
        const QString server = QStringLiteral("mcp_servers.forkmesh.");
        return QStringLiteral("codex -c %1 -c %2")
            .arg(shellQuote(server + QStringLiteral("command=") + tomlString(python)),
                 shellQuote(server + QStringLiteral("args=[%1, %2, %3]")
                                         .arg(tomlString(mcpServerScript),
                                              tomlString(QStringLiteral("--env-file")),
                                              tomlString(mcpConfigPath))));
    }
    if (kind == CommandKind)
        return command;
    return windows ? QStringLiteral("rem") : QStringLiteral("exec \"${SHELL:-bash}\" -i");
}

QString tailLines(const QString &text, int lines)
{
    QStringList all = text.split(QLatin1Char('\n'));
    while (!all.isEmpty() && all.constLast().trimmed().isEmpty())
        all.removeLast();
    if (lines > 0 && all.size() > lines)
        all = all.mid(all.size() - lines);
    return all.join(QLatin1Char('\n'));
}

QString errorMessage(const QString &code)
{
    if (code == QLatin1String("bad_kind"))
        return QStringLiteral("kind must be shell, claude, codex or command.");
    if (code == QLatin1String("bad_command"))
        return QStringLiteral("command is required for kind command, at most %1 characters, "
                              "and only allowed with kind command.")
            .arg(kMaxCommandChars);
    if (code == QLatin1String("bad_repository"))
        return QStringLiteral("repository must be owner/name.");
    if (code == QLatin1String("bad_title"))
        return QStringLiteral("title must be at most %1 characters.").arg(kMaxTitleChars);
    if (code == QLatin1String("bad_terminal"))
        return QStringLiteral("terminal must be a terminal id from list_terminals.");
    if (code == QLatin1String("bad_text"))
        return QStringLiteral("text must be a string of at most %1 characters.")
            .arg(kMaxInputChars);
    if (code == QLatin1String("bad_keys"))
        return QStringLiteral("keys must be at most %1 key names such as enter, tab, escape, "
                              "up, down, left, right, backspace or ctrl-c.")
            .arg(kMaxKeys);
    if (code == QLatin1String("bad_enter"))
        return QStringLiteral("enter must be true or false.");
    if (code == QLatin1String("empty_input"))
        return QStringLiteral("Pass text, keys or enter.");
    if (code == QLatin1String("bad_lines"))
        return QStringLiteral("lines must be a whole number from 1 to %1.").arg(kMaxReadLines);
    return QStringLiteral("The request is malformed.");
}

}
