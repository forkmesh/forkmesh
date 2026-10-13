#include "LocalControlTerminals.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSet>

#include <iostream>

using namespace forkmesh::localcontrol::terminals;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    check(familyPrefix() == QStringLiteral("terminal."), "family prefix");
    check(capability() == QStringLiteral("terminal.agents"), "capability");
    check(resumeCapability() == QStringLiteral("terminal.resume"), "resume capability");
    check(cliProgram(QStringLiteral("claude-code")) == QStringLiteral("claude") &&
              cliProgram(QStringLiteral("codex")) == QStringLiteral("codex"),
          "the two providers map to their CLIs");
    check(cliProgram(QStringLiteral("claude-api")).isEmpty() &&
              cliProgram(QStringLiteral("sh")).isEmpty() && cliProgram(QString()).isEmpty(),
          "nothing else can be started");
    check(cliLabel(QStringLiteral("codex")) == QStringLiteral("Codex"), "label");
    check(accountsCapability() == QStringLiteral("terminal.accounts"), "accounts capability");
    check(validMode(QString()) && validMode(QStringLiteral("agent")) &&
              validMode(QStringLiteral("login")) && validMode(QStringLiteral("logout")) &&
              !validMode(QStringLiteral("shell")),
          "only agent, login and logout terminals");
    check(cliArguments(QStringLiteral("claude-code"), QString()) ==
                  QStringList{QStringLiteral("--dangerously-skip-permissions")} &&
              cliArguments(QStringLiteral("claude-code"), QStringLiteral("agent")) ==
                  QStringList{QStringLiteral("--dangerously-skip-permissions")},
          "Claude Code agents start with full permissions");
    check(cliArguments(QStringLiteral("codex"), QStringLiteral("agent")) ==
              QStringList{QStringLiteral("--dangerously-bypass-approvals-and-sandbox")},
          "Codex agents start without approvals or sandbox");
    check(cliArguments(QStringLiteral("claude-code"), QStringLiteral("login")) ==
                  (QStringList{QStringLiteral("auth"), QStringLiteral("login")}) &&
              cliArguments(QStringLiteral("claude-code"), QStringLiteral("logout")) ==
                  (QStringList{QStringLiteral("auth"), QStringLiteral("logout")}) &&
              cliArguments(QStringLiteral("codex"), QStringLiteral("login")) ==
                  QStringList{QStringLiteral("login")} &&
              cliArguments(QStringLiteral("codex"), QStringLiteral("logout")) ==
                  QStringList{QStringLiteral("logout")},
          "sign-in terminals run the CLI's own login and logout");
    check(cliArguments(QStringLiteral("sh"), QString()).isEmpty(), "no arguments for others");
    check(accountEnvVariable(QStringLiteral("claude-code")) == QStringLiteral("CLAUDE_CONFIG_DIR") &&
              accountEnvVariable(QStringLiteral("codex")) == QStringLiteral("CODEX_HOME") &&
              accountEnvVariable(QStringLiteral("sh")).isEmpty(),
          "account folders per provider");
    check(kReplayBytes == 512 * 1024, "the replay keeps 512 KiB");

    QSet<QString> ids;
    for (int i = 0; i < 50; ++i) {
        const QString id = newTerminalId();
        check(validTerminalId(id), "a new id is valid");
        ids.insert(id);
    }
    check(ids.size() == 50, "ids do not repeat");
    check(!validTerminalId(QStringLiteral("../../etc")) &&
              !validTerminalId(QString(32, QLatin1Char('g'))) &&
              !validTerminalId(QString(31, QLatin1Char('a'))) &&
              !validTerminalId(QString(32, QLatin1Char('A'))),
          "only 32 lowercase hex characters");

    check(clampCols(1) == 20 && clampCols(9000) == 500 && clampCols(120) == 120, "cols clamp");
    check(clampRows(0) == 5 && clampRows(900) == 200 && clampRows(40) == 40, "rows clamp");

    {
        QByteArray replay;
        appendReplay(replay, QByteArray("one\ntwo\nthree\n"), 9);
        check(replay == QByteArray("three\n"), "the replay is cut at a line start");
        QByteArray wide;
        appendReplay(wide, QByteArray(20, 'x'), 10);
        check(wide.size() == 10, "a replay without lines keeps the cap");
        QByteArray utf8;
        appendReplay(utf8, QByteArray("ab") + QByteArray("\xc3\xa9\xc3\xa9\xc3\xa9"), 5);
        check(utf8 == QByteArray("\xc3\xa9\xc3\xa9"), "a cut never starts inside a character");
        QByteArray small;
        appendReplay(small, QByteArray("abc"), 10);
        appendReplay(small, QByteArray("def"), 10);
        check(small == QByteArray("abcdef"), "under the cap nothing is dropped");
    }

    {
        QString error;
        check(decodeInput(QJsonValue(QString::fromLatin1(QByteArray("hi\r").toBase64())), &error) ==
                      QByteArray("hi\r") &&
                  error.isEmpty(),
              "input decodes");
        error.clear();
        check(decodeInput(QJsonValue(QStringLiteral("not base64!")), &error).isEmpty() &&
                  error == QStringLiteral("bad_input"),
              "bad base64 is refused");
        error.clear();
        check(decodeInput(QJsonValue(42), &error).isEmpty() && error == QStringLiteral("bad_input"),
              "a number is refused");
        error.clear();
        check(decodeInput(QJsonValue(QString()), &error).isEmpty() && !error.isEmpty(),
              "empty input is refused");
        error.clear();
        const QByteArray big(kMaxInputBytes + 1, 'a');
        check(decodeInput(QJsonValue(QString::fromLatin1(big.toBase64())), &error).isEmpty() &&
                  !error.isEmpty(),
              "input over 16 KiB is refused");
        error.clear();
        const QByteArray limit(kMaxInputBytes, 'a');
        check(decodeInput(QJsonValue(QString::fromLatin1(limit.toBase64())), &error) == limit &&
                  error.isEmpty(),
              "16 KiB is accepted");
    }

    {
        Outbox box;
        check(!hasOutput(box), "an empty outbox has nothing");
        queueOutput(box, QByteArray(10, 'a'), 64);
        queueOutput(box, QByteArray(10, 'b'), 64);
        check(hasOutput(box), "queued output is pending");
        QJsonObject event = takeEvent(box, QStringLiteral("t"), QByteArray(), 12);
        check(event.value(QStringLiteral("terminal")).toString() == QStringLiteral("t") &&
                  QByteArray::fromBase64(event.value(QStringLiteral("data")).toString().toLatin1()) ==
                      QByteArray(10, 'a') + QByteArray(2, 'b') &&
                  !event.value(QStringLiteral("reset")).toBool(),
              "an event carries at most its size");
        event = takeEvent(box, QStringLiteral("t"), QByteArray(), 12);
        check(QByteArray::fromBase64(event.value(QStringLiteral("data")).toString().toLatin1()) ==
                      QByteArray(8, 'b') &&
                  !hasOutput(box),
              "the rest follows");

        queueOutput(box, QByteArray(60, 'c'), 64);
        queueOutput(box, QByteArray(10, 'd'), 64);
        check(box.reset && box.pending.isEmpty(), "falling behind turns into a reset");
        queueOutput(box, QByteArray(10, 'e'), 64);
        check(box.pending.isEmpty(), "output after a reset is covered by the replay");
        event = takeEvent(box, QStringLiteral("t"), QByteArray("REPLAY"), 12);
        check(event.value(QStringLiteral("reset")).toBool() &&
                  QByteArray::fromBase64(event.value(QStringLiteral("data")).toString().toLatin1()) ==
                      QByteArray("REPLAY") &&
                  !hasOutput(box),
              "a reset sends the replay once");

        Outbox split;
        queueOutput(split, QByteArray("abc\xc3\xa9"), 64);
        event = takeEvent(split, QStringLiteral("t"), QByteArray(), 4);
        check(QByteArray::fromBase64(event.value(QStringLiteral("data")).toString().toLatin1()) ==
                  QByteArray("abc"),
              "an event does not end inside a character");
    }

    {
        Terminal terminal;
        terminal.id = QString(32, QLatin1Char('a'));
        terminal.provider = QStringLiteral("claude-code");
        terminal.cli = QStringLiteral("claude");
        terminal.owner = QStringLiteral("mona");
        terminal.name = QStringLiteral("app");
        terminal.cwd = QStringLiteral("/home/mona/app");
        terminal.device = QStringLiteral("Mona's iPhone");
        terminal.startedAtMs = 1000;
        terminal.pid = 42;
        terminal.cols = 90;
        terminal.rows = 30;
        const QJsonObject json = terminalJson(terminal);
        check(json.value(QStringLiteral("terminal")).toString() == terminal.id &&
                  json.value(QStringLiteral("label")).toString() == QStringLiteral("Claude Code") &&
                  json.value(QStringLiteral("repositoryOwner")).toString() == QStringLiteral("mona") &&
                  json.value(QStringLiteral("running")).toBool() &&
                  json.value(QStringLiteral("cols")).toInt() == 90 &&
                  json.value(QStringLiteral("device")).toString() == QStringLiteral("Mona's iPhone") &&
                  !json.contains(QStringLiteral("pid")) &&
                  !json.contains(QStringLiteral("session")),
              "the terminal object");
        terminal.session = 17;
        check(terminalJson(terminal).value(QStringLiteral("session")).toInt() == 17,
              "a terminal opened for an agent names its session");
        terminal.session = 0;
        const QJsonObject row = outsideRow(terminal, -1007);
        check(row.value(QStringLiteral("id")).toInt() == -1007 &&
                  row.value(QStringLiteral("kind")).toString() == QStringLiteral("claude") &&
                  row.value(QStringLiteral("title")).toString() == QStringLiteral("app") &&
                  row.value(QStringLiteral("typeable")).toBool() &&
                  row.value(QStringLiteral("stoppable")).toBool() &&
                  !row.value(QStringLiteral("steer")).toBool() &&
                  row.value(QStringLiteral("terminal")).toString() == terminal.id &&
                  row.value(QStringLiteral("repositoryName")).toString() == QStringLiteral("app"),
              "the agents list row");
        terminal.pid = 0;
        check(!outsideRow(terminal, -1).value(QStringLiteral("typeable")).toBool() &&
                  !terminalJson(terminal).value(QStringLiteral("running")).toBool(),
              "an ended terminal takes no input");
    }

    {
        Terminal shell;
        shell.id = QString(32, QLatin1Char('b'));
        shell.provider = hostShellProvider();
        shell.cli = QStringLiteral("ssh");
        shell.mode = hostShellProvider();
        shell.host = QStringLiteral("mirror35");
        shell.cwd = QStringLiteral("root@203.0.113.5");
        shell.pid = 7;
        const QJsonObject json = terminalJson(shell);
        check(json.value(QStringLiteral("label")).toString() == QStringLiteral("SSH to mirror35") &&
                  json.value(QStringLiteral("host")).toString() == QStringLiteral("mirror35") &&
                  json.value(QStringLiteral("provider")).toString() == QStringLiteral("ssh") &&
                  json.value(QStringLiteral("repositoryName")).toString().isEmpty(),
              "a host shell names its host");
        check(cliProgram(hostShellProvider()).isEmpty(),
              "terminal.start cannot open a host shell; only a confirmed hosts.run can");
        check(!terminalJson(Terminal{}).contains(QStringLiteral("host")),
              "an agent terminal names no host");
    }

    if (failures == 0)
        std::cout << "local control terminals tests passed\n";
    return failures == 0 ? 0 : 1;
}
