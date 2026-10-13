// Headless protocol test for ClaudeIdeBridge (issue #191): drives the bridge's
// WebSocket/JSON-RPC server exactly as the `claude` CLI would — RFC 6455
// handshake with the lockfile auth token, MCP initialize, and a tools/call — so
// the wire format is verified without needing the real CLI installed.
#include "../src/ClaudeIdeBridge.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

namespace {

int failures = 0;
void check(bool condition, const char *what)
{
    if (condition)
        qInfo("PASS: %s", what);
    else {
        qCritical("FAIL: %s", what);
        ++failures;
    }
}

// Spin the event loop until pred() is true or the deadline passes. Both the
// in-process server and the client socket live in this thread, so we must pump
// rather than block on waitForReadyRead.
template <typename Pred> bool pump(Pred pred, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

// Client -> server frames must be masked (RFC 6455 §5.3).
QByteArray maskedTextFrame(const QByteArray &payload)
{
    QByteArray f;
    f.append(char(0x80 | 0x01)); // FIN + text
    const int n = payload.size();
    if (n < 126) {
        f.append(char(0x80 | n));
    } else if (n <= 0xffff) {
        f.append(char(0x80 | 126));
        f.append(char((n >> 8) & 0xff));
        f.append(char(n & 0xff));
    } else {
        f.append(char(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            f.append(char((quint64(n) >> (i * 8)) & 0xff));
    }
    unsigned char mask[4];
    for (unsigned char &m : mask)
        m = (unsigned char)(QRandomGenerator::global()->generate() & 0xff);
    f.append((const char *)mask, 4);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask[i % 4];
    f.append(masked);
    return f;
}

// Pull complete unmasked text messages out of a server->client byte stream.
QList<QByteArray> drainServerMessages(QByteArray &buf)
{
    QList<QByteArray> out;
    for (;;) {
        if (buf.size() < 2)
            break;
        const auto u = [&](int i) { return (unsigned char)buf.at(i); };
        quint64 len = u(1) & 0x7f;
        int idx = 2;
        if (len == 126) {
            if (buf.size() < 4)
                break;
            len = (quint64(u(2)) << 8) | u(3);
            idx = 4;
        } else if (len == 127) {
            if (buf.size() < 10)
                break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | u(2 + i);
            idx = 10;
        }
        if ((quint64)buf.size() < (quint64)idx + len)
            break;
        out.append(buf.mid(idx, (int)len));
        buf.remove(0, idx + (int)len);
    }
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Isolate the lockfile under a throwaway HOME.
    QTemporaryDir home;
    check(home.isValid(), "temp HOME created");
    qputenv("HOME", home.path().toUtf8());

    QTemporaryDir workspace;
    QDir(workspace.path()).mkpath(QStringLiteral("allowed"));
    QDir(workspace.path()).mkpath(QStringLiteral("blocked"));
    const QString allowedFile =
        QDir(workspace.path()).filePath(QStringLiteral("allowed/x.txt"));
    const QString blockedFile =
        QDir(workspace.path()).filePath(QStringLiteral("blocked/secret.txt"));
    check(ClaudeIdeBridge::repositoryPathAllowed(
              true, workspace.path(), {QStringLiteral("allowed")}, allowedFile),
          "IDE bridge policy permits an allowlisted path");
    check(!ClaudeIdeBridge::repositoryPathAllowed(
              true, workspace.path(), {QStringLiteral("allowed")}, blockedFile),
          "IDE bridge policy rejects a path outside the allowlist");
    check(ClaudeIdeBridge::repositoryPathAllowed(
              true, workspace.path(), {QStringLiteral("allowed")},
              QUrl::fromLocalFile(allowedFile).toString()),
          "IDE bridge policy understands local file URLs");

    // Some hermetic test runners prohibit creating even loopback sockets.
    // Keep the path-policy checks above active there, and exercise the full
    // WebSocket protocol whenever the local socket API is available.
    QTcpServer socketProbe;
    if (!socketProbe.listen(QHostAddress::LocalHost, 0)) {
        qInfo("SKIP: local TCP sockets are unavailable: %s",
              qPrintable(socketProbe.errorString()));
        return failures == 0 ? 0 : 1;
    }
    socketProbe.close();

    ClaudeIdeBridge bridge;
    bridge.setRepositoryGuardrails(true, {QStringLiteral("allowed")});
    check(bridge.start(workspace.path()), "bridge starts listening");
    check(bridge.port() != 0, "bridge bound a port");
    check(bridge.env().contains(
              QStringLiteral("CLAUDE_CODE_SSE_PORT=%1").arg(bridge.port())),
          "env advertises the SSE port");
    check(bridge.env().contains(QStringLiteral("ENABLE_IDE_INTEGRATION=true")),
          "env enables IDE integration");

    // The CLI discovers us via ~/.claude/ide/<port>.lock.
    const QString lockPath =
        home.path() + QStringLiteral("/.claude/ide/%1.lock").arg(bridge.port());
    QString authToken;
    {
        QFile lf(lockPath);
        check(lf.open(QIODevice::ReadOnly), "lockfile written");
        const QJsonObject lock = QJsonDocument::fromJson(lf.readAll()).object();
        authToken = lock.value(QStringLiteral("authToken")).toString();
        check(authToken.size() == 32, "lockfile authToken is 32 hex chars");
        check(lock.value(QStringLiteral("transport")).toString() == "ws",
              "lockfile transport is ws");
        check(lock.value(QStringLiteral("ideName")).toString() == "ForkMesh",
              "lockfile ideName is ForkMesh");
        check(lock.value(QStringLiteral("workspaceFolders"))
                  .toArray()
                  .first()
                  .toString() == workspace.path(),
              "lockfile carries the workspace folder");
#ifndef Q_OS_WIN
        check(QFile::permissions(lockPath) ==
                  (QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                   QFileDevice::ReadUser | QFileDevice::WriteUser),
              "lockfile holding the auth token is owner-only");
#endif
    }

    // ---- a wrong token is rejected at the handshake -----------------------
    {
        QTcpSocket bad;
        bad.connectToHost(QHostAddress::LocalHost, bridge.port());
        check(pump([&] { return bad.state() == QAbstractSocket::ConnectedState; }),
              "bad client connects (TCP)");
        bad.write(
            "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "x-claude-code-ide-authorization: wrong-token\r\n\r\n");
        QByteArray resp;
        pump([&] {
            resp += bad.readAll();
            return resp.contains("\r\n\r\n");
        });
        check(resp.startsWith("HTTP/1.1 401"), "wrong auth token is rejected");
    }

    // ---- the authorized handshake + JSON-RPC ------------------------------
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, bridge.port());
    check(pump([&] { return sock.state() == QAbstractSocket::ConnectedState; }),
          "client connects (TCP)");

    // Mirror the real CLI handshake: it requests the "mcp" subprotocol and the
    // server must echo it back (a strict ws client rejects an ignored protocol).
    QByteArray req =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: mcp\r\n"
        "x-claude-code-ide-authorization: " +
        authToken.toUtf8() + "\r\n\r\n";
    sock.write(req);

    QByteArray in;
    check(pump([&] {
              in += sock.readAll();
              return in.contains("\r\n\r\n");
          }),
          "handshake response received");
    check(in.startsWith("HTTP/1.1 101"), "server completes the WS handshake");
    check(in.contains("Sec-WebSocket-Protocol: mcp"),
          "server echoes the mcp subprotocol");
    in.remove(0, in.indexOf("\r\n\r\n") + 4); // anything after is frame data

    auto rpc = [&](const QJsonObject &msg) -> QJsonObject {
        sock.write(maskedTextFrame(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        QJsonObject result;
        const int wantId = msg.value(QStringLiteral("id")).toInt();
        pump([&] {
            in += sock.readAll();
            for (const QByteArray &m : drainServerMessages(in)) {
                const QJsonObject o = QJsonDocument::fromJson(m).object();
                if (o.value(QStringLiteral("id")).toInt() == wantId) {
                    result = o;
                    return true;
                }
            }
            return false;
        });
        return result;
    };

    const QJsonObject initResult =
        rpc(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), 1},
                        {QStringLiteral("method"), QStringLiteral("initialize")},
                        {QStringLiteral("params"), QJsonObject{}}})
            .value(QStringLiteral("result"))
            .toObject();
    check(initResult.value(QStringLiteral("protocolVersion")).toString()
              == "2024-11-05",
          "initialize returns the MCP protocol version");
    check(initResult.value(QStringLiteral("serverInfo"))
              .toObject()
              .value(QStringLiteral("name"))
              .toString() == "forkmesh",
          "initialize advertises serverInfo.name=forkmesh");

    const QJsonObject toolsResult =
        rpc(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), 2},
                        {QStringLiteral("method"), QStringLiteral("tools/list")},
                        {QStringLiteral("params"), QJsonObject{}}})
            .value(QStringLiteral("result"))
            .toObject();
    bool sawOpenDiff = false;
    for (const QJsonValue &t : toolsResult.value(QStringLiteral("tools")).toArray())
        if (t.toObject().value(QStringLiteral("name")).toString() == "openDiff")
            sawOpenDiff = true;
    check(sawOpenDiff, "tools/list advertises openDiff");

    const QJsonObject wsResult =
        rpc(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), 3},
                {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"),
                 QJsonObject{{QStringLiteral("name"),
                              QStringLiteral("getWorkspaceFolders")},
                             {QStringLiteral("arguments"), QJsonObject{}}}}})
            .value(QStringLiteral("result"))
            .toObject();
    const QString wsText = wsResult.value(QStringLiteral("content"))
                               .toArray()
                               .first()
                               .toObject()
                               .value(QStringLiteral("text"))
                               .toString();
    const QJsonObject folders = QJsonDocument::fromJson(wsText.toUtf8()).object();
    check(folders.value(QStringLiteral("folders"))
                  .toArray()
                  .first()
                  .toObject()
                  .value(QStringLiteral("path"))
                  .toString() == workspace.path(),
          "getWorkspaceFolders returns the workspace path");

    // selection sync round-trips through getCurrentSelection
    bridge.setSelection(allowedFile, QStringLiteral("hello"), 1, 0, 1, 5);
    const QJsonObject selResult =
        rpc(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), 4},
                {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"),
                 QJsonObject{{QStringLiteral("name"),
                              QStringLiteral("getCurrentSelection")},
                             {QStringLiteral("arguments"), QJsonObject{}}}}})
            .value(QStringLiteral("result"))
            .toObject();
    const QString selText = selResult.value(QStringLiteral("content"))
                                .toArray()
                                .first()
                                .toObject()
                                .value(QStringLiteral("text"))
                                .toString();
    check(QJsonDocument::fromJson(selText.toUtf8())
                  .object()
                  .value(QStringLiteral("text"))
                  .toString() == "hello",
          "getCurrentSelection reflects the active selection");

    int openRequests = 0;
    QObject::connect(&bridge, &ClaudeIdeBridge::openFileRequested,
                     [&openRequests](const QString &) { ++openRequests; });
    const QJsonObject allowedOpen = rpc(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 5},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("openFile")},
                     {QStringLiteral("arguments"),
                      QJsonObject{{QStringLiteral("filePath"), allowedFile}}}}}});
    const QJsonObject blockedOpen = rpc(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 6},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("openFile")},
                     {QStringLiteral("arguments"),
                      QJsonObject{{QStringLiteral("filePath"), blockedFile}}}}}});
    check(allowedOpen.contains(QStringLiteral("result")) &&
              blockedOpen.value(QStringLiteral("error"))
                      .toObject()
                      .value(QStringLiteral("code"))
                      .toInt() == -32001 &&
              openRequests == 1,
          "IDE tool calls are stopped before opening a disallowed file");

    int diffRequests = 0;
    QObject::connect(
        &bridge, &ClaudeIdeBridge::openDiffRequested,
        [&diffRequests](const QString &, const QString &, const QString &,
                        const QString &) { ++diffRequests; });
    const QJsonObject blockedDiff = rpc(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 7},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{
             {QStringLiteral("name"), QStringLiteral("openDiff")},
             {QStringLiteral("arguments"),
              QJsonObject{{QStringLiteral("old_file_path"), allowedFile},
                          {QStringLiteral("new_file_path"), blockedFile},
                          {QStringLiteral("new_file_contents"),
                           QStringLiteral("secret")},
                          {QStringLiteral("tab_name"),
                           QStringLiteral("blocked")}}}}}});
    check(blockedDiff.contains(QStringLiteral("error")) && diffRequests == 0,
          "IDE diff writes outside the repository allowlist are blocked");

    bridge.setSelection(blockedFile, QStringLiteral("private"), 1, 0, 1, 7);
    const QJsonObject blockedSelection = rpc(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 8},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"),
                      QStringLiteral("getCurrentSelection")},
                     {QStringLiteral("arguments"), QJsonObject{}}}}});
    check(blockedSelection.contains(QStringLiteral("error")),
          "IDE selection contents outside the allowlist are not disclosed");

    // Stopping the bridge removes the lockfile.
    bridge.stop();
    check(!QFile::exists(lockPath), "stop() removes the lockfile");

    if (failures == 0)
        qInfo("All ClaudeIdeBridge protocol tests passed.");
    else
        qCritical("%d ClaudeIdeBridge test(s) failed.", failures);
    return failures == 0 ? 0 : 1;
}
