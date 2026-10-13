#include "LocalControlTerminals.h"

#include <QFileInfo>
#include <QJsonValue>
#include <QRandomGenerator>

#include <algorithm>

namespace forkmesh::localcontrol::terminals {

QString familyPrefix()
{
    return QStringLiteral("terminal.");
}

QString capability()
{
    return QStringLiteral("terminal.agents");
}

QString resumeCapability()
{
    return QStringLiteral("terminal.resume");
}

QString accountsCapability()
{
    return QStringLiteral("terminal.accounts");
}

QString hostShellProvider()
{
    return QStringLiteral("ssh");
}

QString cliProgram(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("claude");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("codex");
    return {};
}

QString cliLabel(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("Claude Code");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("Codex");
    if (provider == hostShellProvider())
        return QStringLiteral("SSH");
    return {};
}

bool validMode(const QString &mode)
{
    return mode.isEmpty() || mode == QLatin1String("agent") || mode == QLatin1String("login") ||
           mode == QLatin1String("logout");
}

QStringList cliArguments(const QString &provider, const QString &mode)
{
    const bool codex = provider == QLatin1String("codex");
    if (provider != QLatin1String("claude-code") && !codex)
        return {};
    if (mode == QLatin1String("login") || mode == QLatin1String("logout")) {
        if (codex)
            return {mode};
        return {QStringLiteral("auth"), mode};
    }
    if (codex)
        return {QStringLiteral("--dangerously-bypass-approvals-and-sandbox")};
    return {QStringLiteral("--dangerously-skip-permissions")};
}

QString accountEnvVariable(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("CLAUDE_CONFIG_DIR");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("CODEX_HOME");
    return {};
}

bool validTerminalId(const QString &id)
{
    if (id.size() != kIdChars)
        return false;
    for (const QChar c : id) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

QString newTerminalId()
{
    QByteArray bytes(kIdChars / 2, Qt::Uninitialized);
    QRandomGenerator *random = QRandomGenerator::system();
    for (char &byte : bytes)
        byte = char(random->bounded(256));
    return QString::fromLatin1(bytes.toHex());
}

int clampCols(int cols)
{
    return qBound(20, cols, 500);
}

int clampRows(int rows)
{
    return qBound(5, rows, 200);
}

void appendReplay(QByteArray &replay, const QByteArray &chunk, int cap)
{
    replay.append(chunk);
    if (replay.size() <= cap)
        return;
    qsizetype cut = replay.size() - cap;
    const qsizetype line = replay.indexOf('\n', cut);
    if (line >= 0 && line - cut < kReplayLineSearchBytes)
        cut = line + 1;
    else
        while (cut < replay.size() && (uchar(replay.at(cut)) & 0xC0) == 0x80)
            ++cut;
    replay.remove(0, cut);
}

QByteArray decodeInput(const QJsonValue &data, QString *error)
{
    const auto fail = [error] {
        if (error)
            *error = QStringLiteral("bad_input");
        return QByteArray();
    };
    if (!data.isString())
        return fail();
    const QByteArray text = data.toString().toLatin1();
    if (text.isEmpty() || text.size() > (kMaxInputBytes / 3 + 1) * 4)
        return fail();
    const auto decoded = QByteArray::fromBase64Encoding(
        text, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > kMaxInputBytes)
        return fail();
    return decoded.decoded;
}

void queueOutput(Outbox &box, const QByteArray &chunk, int maxPending)
{
    if (box.reset)
        return;
    if (box.pending.size() + chunk.size() > maxPending) {
        box.pending.clear();
        box.reset = true;
        return;
    }
    box.pending.append(chunk);
}

bool hasOutput(const Outbox &box)
{
    return box.reset || !box.pending.isEmpty();
}

QJsonObject takeEvent(Outbox &box, const QString &terminal, const QByteArray &replay,
                      int maxBytes)
{
    QByteArray data;
    const bool reset = box.reset;
    if (reset) {
        data = replay;
        box.pending.clear();
        box.reset = false;
    } else {
        qsizetype size = std::min<qsizetype>(box.pending.size(), maxBytes);
        while (size < box.pending.size() && size > 0 &&
               (uchar(box.pending.at(size)) & 0xC0) == 0x80)
            --size;
        if (size == 0)
            size = std::min<qsizetype>(box.pending.size(), maxBytes);
        data = box.pending.left(size);
        box.pending.remove(0, size);
    }
    return QJsonObject{{QStringLiteral("terminal"), terminal},
                       {QStringLiteral("data"), QString::fromLatin1(data.toBase64())},
                       {QStringLiteral("reset"), reset}};
}

QString terminalLabel(const Terminal &terminal)
{
    if (terminal.provider == hostShellProvider() && !terminal.host.isEmpty())
        return QStringLiteral("SSH to %1").arg(terminal.host);
    return cliLabel(terminal.provider);
}

QJsonObject terminalJson(const Terminal &terminal)
{
    QJsonObject json{
        {QStringLiteral("terminal"), terminal.id},
        {QStringLiteral("provider"), terminal.provider},
        {QStringLiteral("label"), terminalLabel(terminal)},
        {QStringLiteral("cli"), terminal.cli},
        {QStringLiteral("repositoryOwner"), terminal.owner},
        {QStringLiteral("repositoryName"), terminal.name},
        {QStringLiteral("cwd"), terminal.cwd},
        {QStringLiteral("startedAtMs"), double(terminal.startedAtMs)},
        {QStringLiteral("device"), terminal.device},
        {QStringLiteral("running"), terminal.pid > 0},
        {QStringLiteral("cols"), terminal.cols},
        {QStringLiteral("rows"), terminal.rows},
    };
    if (terminal.session > 0)
        json.insert(QStringLiteral("session"), terminal.session);
    if (!terminal.mode.isEmpty())
        json.insert(QStringLiteral("mode"), terminal.mode);
    if (!terminal.account.isEmpty())
        json.insert(QStringLiteral("account"), terminal.account);
    if (!terminal.host.isEmpty())
        json.insert(QStringLiteral("host"), terminal.host);
    return json;
}

QJsonObject outsideRow(const Terminal &terminal, int id)
{
    const QString folder = QFileInfo(terminal.cwd).fileName();
    const QString label = cliLabel(terminal.provider);
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), terminal.cli},
        {QStringLiteral("label"), label},
        {QStringLiteral("provider"), terminal.provider},
        {QStringLiteral("model"), QString()},
        {QStringLiteral("title"), folder.isEmpty() ? label : folder},
        {QStringLiteral("cwd"), terminal.cwd},
        {QStringLiteral("branchName"), QString()},
        {QStringLiteral("repositoryOwner"), terminal.owner},
        {QStringLiteral("repositoryName"), terminal.name},
        {QStringLiteral("lastActivityMs"), double(terminal.startedAtMs)},
        {QStringLiteral("steer"), false},
        {QStringLiteral("typeable"), terminal.pid > 0},
        {QStringLiteral("stoppable"), terminal.pid > 0},
        {QStringLiteral("terminal"), terminal.id},
    };
}

} // namespace forkmesh::localcontrol::terminals
