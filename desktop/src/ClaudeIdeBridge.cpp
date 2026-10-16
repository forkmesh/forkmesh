#include "ForkMeshVersion.h"
#include "ClaudeIdeBridge.h"
#include "AgentGuardrails.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "0"
#endif

namespace {
// RFC 6455 handshake GUID.
const QByteArray kWsGuid = QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

QString fileUrl(const QString &path)
{
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}
} // namespace

ClaudeIdeBridge::ClaudeIdeBridge(QObject *parent) : QObject(parent) {}

ClaudeIdeBridge::~ClaudeIdeBridge() { stop(); }


bool ClaudeIdeBridge::start(const QString &workspaceFolder)
{
    m_workspaceFolder = workspaceFolder;
    if (isListening()) {
        writeLockfile(); // refresh workspace folder in the lockfile
        return true;
    }

    quint32 r[4];
    for (quint32 &x : r)
        x = QRandomGenerator::system()->generate();
    m_authToken = QString::asprintf("%08x%08x%08x%08x", r[0], r[1], r[2], r[3]);

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this,
            &ClaudeIdeBridge::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit log(QStringLiteral("[ide] failed to bind localhost: %1")
                     .arg(m_server->errorString()));
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_port = m_server->serverPort();
    writeLockfile();
    emit log(QStringLiteral("[ide] listening on 127.0.0.1:%1").arg(m_port));
    return true;
}

void ClaudeIdeBridge::stop()
{
    removeLockfile();
    const QList<QTcpSocket *> socks = m_conns.keys();
    m_conns.clear();
    m_pendingDiffs.clear();
    for (QTcpSocket *s : socks) {
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_port = 0;
    m_authToken.clear();
}

bool ClaudeIdeBridge::isListening() const
{
    return m_server && m_server->isListening();
}

QStringList ClaudeIdeBridge::env() const
{
    if (!isListening())
        return {};
    return {QStringLiteral("CLAUDE_CODE_SSE_PORT=%1").arg(m_port),
            QStringLiteral("ENABLE_IDE_INTEGRATION=true")};
}

void ClaudeIdeBridge::setWorkspaceFolder(const QString &path)
{
    if (m_workspaceFolder == path)
        return;
    m_workspaceFolder = path;
    if (isListening())
        writeLockfile();
}

void ClaudeIdeBridge::setRepositoryGuardrails(
    bool enabled, const QStringList &allowedDirectories)
{
    m_guardrailsEnabled = enabled;
    m_allowedDirectories =
        forkmesh::agentguard::normalizeDirectories(allowedDirectories);
}

bool ClaudeIdeBridge::repositoryPathAllowed(
    bool enabled, const QString &workspace,
    const QStringList &allowedDirectories, const QString &rawPath)
{
    if (!enabled)
        return true;
    QString path = rawPath.trimmed();
    const QUrl url(path);
    if (url.isLocalFile())
        path = url.toLocalFile();
    return !path.isEmpty() && forkmesh::agentguard::pathAllowed(
                                  workspace, allowedDirectories, path);
}


void ClaudeIdeBridge::setActiveFile(const QString &filePath)
{
    m_activeFile = filePath;
}

void ClaudeIdeBridge::setSelection(const QString &filePath, const QString &text,
                                   int startLine, int startChar, int endLine,
                                   int endChar)
{
    m_selFile = filePath;
    m_selText = text;
    m_selStartLine = startLine;
    m_selStartChar = startChar;
    m_selEndLine = endLine;
    m_selEndChar = endChar;
    m_hasSelection = !text.isEmpty();
    if (!filePath.isEmpty())
        m_activeFile = filePath;
}

void ClaudeIdeBridge::clearSelection()
{
    m_selText.clear();
    m_hasSelection = false;
}

// ----------------------------------------------------------- TCP / handshake

void ClaudeIdeBridge::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *sock = m_server->nextPendingConnection();
        m_conns.insert(sock, Conn{sock, false, {}, {}, 0});
        connect(sock, &QTcpSocket::readyRead, this,
                [this, sock] { onReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, this,
                [this, sock] { onDisconnected(sock); });
    }
}

void ClaudeIdeBridge::onDisconnected(QTcpSocket *sock)
{
    for (auto it = m_pendingDiffs.begin(); it != m_pendingDiffs.end();) {
        if (it.value().sock == sock)
            it = m_pendingDiffs.erase(it);
        else
            ++it;
    }
    if (m_conns.remove(sock) > 0)
        emit clientDisconnected();
    sock->deleteLater();
}

void ClaudeIdeBridge::onReadyRead(QTcpSocket *sock)
{
    auto it = m_conns.find(sock);
    if (it == m_conns.end())
        return;
    Conn &c = it.value();
    c.buf += sock->readAll();
    if (!c.upgraded) {
        if (!tryHandshake(c))
            return; // waiting for more bytes, or the connection was rejected
    }
    if (c.upgraded)
        processFrames(sock);
}

bool ClaudeIdeBridge::tryHandshake(Conn &c)
{
    const int end = c.buf.indexOf("\r\n\r\n");
    if (end < 0)
        return false; // headers not complete yet

    const QByteArray head = c.buf.left(end);
    QHash<QString, QString> headers;
    const QList<QByteArray> lines = head.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        headers.insert(QString::fromUtf8(line.left(colon)).trimmed().toLower(),
                       QString::fromUtf8(line.mid(colon + 1)).trimmed());
    }

    const QString key = headers.value(QStringLiteral("sec-websocket-key"));
    const QString auth =
        headers.value(QStringLiteral("x-claude-code-ide-authorization"));

    if (!constantTimeEqual(auth.toUtf8(), m_authToken.toUtf8()) ||
        m_authToken.isEmpty() || key.isEmpty()) {
        emit log(QStringLiteral("[ide] rejected connection (auth mismatch)"));
        c.sock->write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
        c.sock->disconnectFromHost();
        return false;
    }

    const QByteArray accept =
        QCryptographicHash::hash(key.toUtf8() + kWsGuid, QCryptographicHash::Sha1)
            .toBase64();
    QByteArray resp = "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + accept + "\r\n";
    // The CLI connects with `Sec-WebSocket-Protocol: mcp`; echo it back so its
    // strict ws client accepts the handshake. We don't negotiate any extension
    // (permessage-deflate), so frames stay uncompressed.
    if (headers.value(QStringLiteral("sec-websocket-protocol"))
            .contains(QStringLiteral("mcp")))
        resp += "Sec-WebSocket-Protocol: mcp\r\n";
    resp += "\r\n";
    c.sock->write(resp);

    c.buf.remove(0, end + 4); // keep any frame bytes that arrived with the request
    c.guardrailsEnabled = m_guardrailsEnabled;
    c.guardrailWorkspace = m_workspaceFolder;
    c.allowedDirectories = m_allowedDirectories;
    c.upgraded = true;
    emit clientConnected();
    emit log(QStringLiteral("[ide] CLI connected"));
    return true;
}


void ClaudeIdeBridge::processFrames(QTcpSocket *sock)
{
    for (;;) {
        auto it = m_conns.find(sock);
        if (it == m_conns.end())
            return;
        Conn &c = it.value();
        const QByteArray &b = c.buf;
        if (b.size() < 2)
            return;
        const auto u = [&](int i) { return (unsigned char)b.at(i); };
        const bool fin = u(0) & 0x80;
        const int opcode = u(0) & 0x0f;
        const bool masked = u(1) & 0x80;
        quint64 len = u(1) & 0x7f;
        int idx = 2;
        if (len == 126) {
            if (b.size() < idx + 2)
                return;
            len = (quint64(u(idx)) << 8) | u(idx + 1);
            idx += 2;
        } else if (len == 127) {
            if (b.size() < idx + 8)
                return;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | u(idx + i);
            idx += 8;
        }
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (b.size() < idx + 4)
                return;
            for (int i = 0; i < 4; ++i)
                mask[i] = u(idx + i);
            idx += 4;
        }
        if ((quint64)b.size() < (quint64)idx + len)
            return; // frame not fully arrived

        QByteArray payload = b.mid(idx, (int)len);
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = payload.at(i) ^ mask[i % 4];
        }
        c.buf.remove(0, idx + (int)len);

        switch (opcode) {
        case 0x0: // continuation
            c.fragment += payload;
            if (fin) {
                const int op = c.fragmentOpcode;
                const QByteArray full = c.fragment;
                c.fragment.clear();
                c.fragmentOpcode = 0;
                if (op == 0x1) {
                    const QJsonDocument doc = QJsonDocument::fromJson(full);
                    if (doc.isObject())
                        dispatch(sock, doc.object());
                }
            }
            break;
        case 0x1: // text
        case 0x2: // binary (treated like text payloads carrying JSON)
            if (fin) {
                const QJsonDocument doc = QJsonDocument::fromJson(payload);
                if (doc.isObject())
                    dispatch(sock, doc.object());
            } else {
                c.fragment = payload;
                c.fragmentOpcode = opcode;
            }
            break;
        case 0x8: // close
            sendClose(sock);
            sock->disconnectFromHost();
            return;
        case 0x9: { // ping -> pong (same payload, unmasked)
            QByteArray frame;
            frame.append(char(0x80 | 0x0A));
            frame.append(char(payload.size() & 0x7f));
            frame.append(payload);
            sock->write(frame);
            break;
        }
        case 0xA: // pong
            break;
        default:
            break;
        }
    }
}

void ClaudeIdeBridge::sendText(QTcpSocket *sock, const QByteArray &payload)
{
    if (!sock)
        return;
    QByteArray frame;
    frame.append(char(0x80 | 0x01)); // FIN + text, server frames are not masked
    const int n = payload.size();
    if (n < 126) {
        frame.append(char(n));
    } else if (n <= 0xffff) {
        frame.append(char(126));
        frame.append(char((n >> 8) & 0xff));
        frame.append(char(n & 0xff));
    } else {
        frame.append(char(127));
        for (int i = 7; i >= 0; --i)
            frame.append(char((quint64(n) >> (i * 8)) & 0xff));
    }
    frame.append(payload);
    sock->write(frame);
}

void ClaudeIdeBridge::sendClose(QTcpSocket *sock)
{
    if (!sock)
        return;
    const char close[] = {char(0x88), 0};
    sock->write(close, 2);
}


void ClaudeIdeBridge::dispatch(QTcpSocket *sock, const QJsonObject &msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    const bool isRequest = !id.isUndefined() && !id.isNull();
    if (!method.isEmpty())
        emit log(QStringLiteral("[ide] rpc %1").arg(method));

    if (method == QLatin1String("initialize")) {
        QJsonObject caps{
            {QStringLiteral("logging"), QJsonObject{}},
            {QStringLiteral("prompts"),
             QJsonObject{{QStringLiteral("listChanged"), true}}},
            {QStringLiteral("tools"),
             QJsonObject{{QStringLiteral("listChanged"), true}}}};
        sendResult(sock, id,
                   QJsonObject{
                       {QStringLiteral("protocolVersion"),
                        QStringLiteral("2024-11-05")},
                       {QStringLiteral("capabilities"), caps},
                       {QStringLiteral("serverInfo"),
                        QJsonObject{
                            {QStringLiteral("name"), QStringLiteral("forkmesh")},
                            {QStringLiteral("version"),
                             QStringLiteral(FORKMESH_VERSION)}}}});
        return;
    }
    if (method == QLatin1String("tools/list")) {
        sendResult(sock, id, toolDescriptors());
        return;
    }
    if (method == QLatin1String("prompts/list")) {
        sendResult(sock, id, QJsonObject{{QStringLiteral("prompts"), QJsonArray{}}});
        return;
    }
    if (method == QLatin1String("resources/list")) {
        sendResult(sock, id,
                   QJsonObject{{QStringLiteral("resources"), QJsonArray{}}});
        return;
    }
    if (method == QLatin1String("tools/call")) {
        handleToolCall(sock, id, params.value(QStringLiteral("name")).toString(),
                       params.value(QStringLiteral("arguments")).toObject());
        return;
    }
    if (method == QLatin1String("ping")) {
        sendResult(sock, id, QJsonObject{});
        return;
    }
    if (isRequest)
        sendError(sock, id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

void ClaudeIdeBridge::handleToolCall(QTcpSocket *sock, const QJsonValue &id,
                                     const QString &name, const QJsonObject &args)
{
    emit log(QStringLiteral("[ide] tools/call %1").arg(name));

    if (name == QLatin1String("openFile")) {
        const QString path = args.value(QStringLiteral("filePath")).toString();
        if (!pathAllowedFor(sock, path)) {
            emit log(QStringLiteral("[ide] guardrail blocked openFile"));
            sendError(sock, id, -32001,
                      QStringLiteral("Repository guardrail blocked this path."));
            return;
        }
        emit openFileRequested(path);
        sendResult(sock, id, mcpText(QStringLiteral("Opened file: %1").arg(path)));
        return;
    }

    if (name == QLatin1String("openDiff")) {
        const QString tab = args.value(QStringLiteral("tab_name")).toString();
        const QString oldPath = args.value(QStringLiteral("old_file_path")).toString();
        const QString newPath = args.value(QStringLiteral("new_file_path")).toString();
        const QString contents =
            args.value(QStringLiteral("new_file_contents")).toString();
        if ((!oldPath.isEmpty() && !pathAllowedFor(sock, oldPath)) ||
            !pathAllowedFor(sock, newPath)) {
            emit log(QStringLiteral("[ide] guardrail blocked openDiff"));
            sendError(sock, id, -32001,
                      QStringLiteral("Repository guardrail blocked this diff path."));
            return;
        }
        m_pendingDiffs.insert(tab, PendingDiff{sock, id, oldPath, newPath});
        emit openDiffRequested(tab, oldPath, newPath, contents);
        return;
    }

    if (name == QLatin1String("getCurrentSelection")
        || name == QLatin1String("getLatestSelection")) {
        if (!m_selFile.isEmpty() && !pathAllowedFor(sock, m_selFile)) {
            sendError(sock, id, -32001,
                      QStringLiteral(
                          "Repository guardrail blocked the current selection."));
            return;
        }
        QJsonObject sel{
            {QStringLiteral("success"), true},
            {QStringLiteral("text"), m_selText},
            {QStringLiteral("filePath"), m_selFile},
            {QStringLiteral("fileUrl"), fileUrl(m_selFile)},
            {QStringLiteral("selection"),
             QJsonObject{
                 {QStringLiteral("start"),
                  QJsonObject{{QStringLiteral("line"), m_selStartLine},
                              {QStringLiteral("character"), m_selStartChar}}},
                 {QStringLiteral("end"),
                  QJsonObject{{QStringLiteral("line"), m_selEndLine},
                              {QStringLiteral("character"), m_selEndChar}}},
                 {QStringLiteral("isEmpty"), !m_hasSelection}}}};
        sendResult(sock, id, mcpText(QString::fromUtf8(
                                 QJsonDocument(sel).toJson(QJsonDocument::Compact))));
        return;
    }

    if (name == QLatin1String("getOpenEditors")) {
        QJsonArray tabs;
        if (!m_activeFile.isEmpty() && pathAllowedFor(sock, m_activeFile)) {
            tabs.append(QJsonObject{
                {QStringLiteral("uri"), fileUrl(m_activeFile)},
                {QStringLiteral("isActive"), true},
                {QStringLiteral("label"), QFileInfo(m_activeFile).fileName()},
                {QStringLiteral("isDirty"), false}});
        }
        const QJsonObject out{{QStringLiteral("tabs"), tabs}};
        sendResult(sock, id, mcpText(QString::fromUtf8(
                                 QJsonDocument(out).toJson(QJsonDocument::Compact))));
        return;
    }

    if (name == QLatin1String("getWorkspaceFolders")) {
        QJsonArray folders;
        if (!m_workspaceFolder.isEmpty()) {
            folders.append(QJsonObject{
                {QStringLiteral("name"), QFileInfo(m_workspaceFolder).fileName()},
                {QStringLiteral("uri"), fileUrl(m_workspaceFolder)},
                {QStringLiteral("path"), m_workspaceFolder}});
        }
        const QJsonObject out{{QStringLiteral("folders"), folders},
                              {QStringLiteral("rootPath"), m_workspaceFolder}};
        sendResult(sock, id, mcpText(QString::fromUtf8(
                                 QJsonDocument(out).toJson(QJsonDocument::Compact))));
        return;
    }

    if (name == QLatin1String("getDiagnostics")) {
        sendResult(sock, id, mcpText(QStringLiteral("[]")));
        return;
    }

    if (name == QLatin1String("closeAllDiffTabs")) {
        const int n = m_pendingDiffs.size();
        const QList<QString> tabs = m_pendingDiffs.keys();
        for (const QString &tab : tabs)
            resolveDiff(tab, false, QString());
        emit closeAllDiffTabsRequested();
        sendResult(sock, id,
                   mcpText(QStringLiteral("CLOSED_%1_DIFF_TABS").arg(n)));
        return;
    }

    if (name == QLatin1String("checkDocumentDirty")) {
        sendResult(sock, id, mcpText(QStringLiteral("{\"isDirty\":false}")));
        return;
    }
    if (name == QLatin1String("saveDocument")) {
        sendResult(sock, id, mcpText(QStringLiteral("saved")));
        return;
    }

    sendError(sock, id, -32601, QStringLiteral("Tool not found: %1").arg(name));
}

void ClaudeIdeBridge::sendResult(QTcpSocket *sock, const QJsonValue &id,
                                 const QJsonObject &result)
{
    if (id.isUndefined() || id.isNull())
        return; // a notification expects no response
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("result"), result}};
    sendText(sock, QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void ClaudeIdeBridge::sendError(QTcpSocket *sock, const QJsonValue &id, int code,
                                const QString &message)
{
    const QJsonObject msg{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), code},
                     {QStringLiteral("message"), message}}}};
    sendText(sock, QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void ClaudeIdeBridge::sendNotification(const QString &method,
                                       const QJsonObject &params)
{
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("method"), method},
                          {QStringLiteral("params"), params}};
    const QByteArray payload = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    const QString path = params.value(QStringLiteral("filePath")).toString();
    for (auto it = m_conns.constBegin(); it != m_conns.constEnd(); ++it)
        if (it.value().upgraded &&
            (path.isEmpty() || pathAllowedFor(it.key(), path)))
            sendText(it.key(), payload);
}

bool ClaudeIdeBridge::pathAllowedFor(QTcpSocket *sock,
                                     const QString &rawPath) const
{
    const auto connection = m_conns.constFind(sock);
    if (connection == m_conns.constEnd())
        return true;
    return repositoryPathAllowed(connection->guardrailsEnabled,
                                 connection->guardrailWorkspace,
                                 connection->allowedDirectories, rawPath);
}

QJsonObject ClaudeIdeBridge::mcpText(const QString &text)
{
    return mcpText(QStringList{text});
}

QJsonObject ClaudeIdeBridge::mcpText(const QStringList &texts)
{
    QJsonArray content;
    for (const QString &t : texts)
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), t}});
    return QJsonObject{{QStringLiteral("content"), content}};
}

QJsonObject ClaudeIdeBridge::toolDescriptors() const
{
    auto str = [] { return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}; };
    auto tool = [](const QString &name, const QString &desc,
                   const QJsonObject &props, const QJsonArray &required) {
        return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), desc},
            {QStringLiteral("inputSchema"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                         {QStringLiteral("properties"), props},
                         {QStringLiteral("required"), required}}}};
    };

    QJsonArray tools;
    tools.append(tool(QStringLiteral("openFile"),
                      QStringLiteral("Open a file in the editor."),
                      QJsonObject{{QStringLiteral("filePath"), str()}},
                      QJsonArray{QStringLiteral("filePath")}));
    tools.append(tool(
        QStringLiteral("openDiff"),
        QStringLiteral("Open a diff of proposed changes; blocks until the user "
                       "accepts or rejects."),
        QJsonObject{{QStringLiteral("old_file_path"), str()},
                    {QStringLiteral("new_file_path"), str()},
                    {QStringLiteral("new_file_contents"), str()},
                    {QStringLiteral("tab_name"), str()}},
        QJsonArray{QStringLiteral("old_file_path"), QStringLiteral("new_file_path"),
                   QStringLiteral("new_file_contents"), QStringLiteral("tab_name")}));
    tools.append(tool(QStringLiteral("getCurrentSelection"),
                      QStringLiteral("Get the current editor selection."),
                      QJsonObject{}, QJsonArray{}));
    tools.append(tool(QStringLiteral("getLatestSelection"),
                      QStringLiteral("Get the most recent editor selection."),
                      QJsonObject{}, QJsonArray{}));
    tools.append(tool(QStringLiteral("getOpenEditors"),
                      QStringLiteral("List open editor tabs."), QJsonObject{},
                      QJsonArray{}));
    tools.append(tool(QStringLiteral("getWorkspaceFolders"),
                      QStringLiteral("List workspace folders."), QJsonObject{},
                      QJsonArray{}));
    tools.append(tool(QStringLiteral("getDiagnostics"),
                      QStringLiteral("Get language diagnostics for a file."),
                      QJsonObject{{QStringLiteral("uri"), str()}}, QJsonArray{}));
    tools.append(tool(QStringLiteral("closeAllDiffTabs"),
                      QStringLiteral("Close all open diff tabs."), QJsonObject{},
                      QJsonArray{}));
    return QJsonObject{{QStringLiteral("tools"), tools}};
}


void ClaudeIdeBridge::notifySelectionChanged()
{
    QJsonObject params{
        {QStringLiteral("text"), m_selText},
        {QStringLiteral("filePath"), m_selFile},
        {QStringLiteral("fileUrl"), fileUrl(m_selFile)},
        {QStringLiteral("selection"),
         QJsonObject{
             {QStringLiteral("start"),
              QJsonObject{{QStringLiteral("line"), m_selStartLine},
                          {QStringLiteral("character"), m_selStartChar}}},
             {QStringLiteral("end"),
              QJsonObject{{QStringLiteral("line"), m_selEndLine},
                          {QStringLiteral("character"), m_selEndChar}}},
             {QStringLiteral("isEmpty"), !m_hasSelection}}}};
    sendNotification(QStringLiteral("selection_changed"), params);
}

void ClaudeIdeBridge::notifyAtMentioned(const QString &filePath, int lineStart,
                                        int lineEnd)
{
    sendNotification(QStringLiteral("at_mentioned"),
                     QJsonObject{{QStringLiteral("filePath"), filePath},
                                 {QStringLiteral("lineStart"), lineStart},
                                 {QStringLiteral("lineEnd"), lineEnd}});
}


void ClaudeIdeBridge::resolveDiff(const QString &tabName, bool accepted,
                                  const QString &finalContents)
{
    auto it = m_pendingDiffs.find(tabName);
    if (it == m_pendingDiffs.end())
        return;
    const PendingDiff pd = it.value();
    m_pendingDiffs.erase(it);
    if (!m_conns.contains(pd.sock))
        return; // client went away

    if (accepted) {
        const QString target = pd.oldPath.isEmpty() ? pd.newPath : pd.oldPath;
        if (!pathAllowedFor(pd.sock, target)) {
            emit log(QStringLiteral("[ide] guardrail blocked accepted diff"));
            sendError(pd.sock, pd.id, -32001,
                      QStringLiteral("Repository guardrail blocked this diff path."));
            return;
        }
        if (!target.isEmpty()) {
            QDir().mkpath(QFileInfo(target).absolutePath());
            QFile f(target);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(finalContents.toUtf8());
                f.close();
            } else {
                emit log(QStringLiteral("[ide] openDiff: could not write %1")
                             .arg(target));
            }
        }
        sendResult(pd.sock, pd.id,
                   mcpText(QStringList{QStringLiteral("FILE_SAVED"), finalContents}));
    } else {
        sendResult(pd.sock, pd.id,
                   mcpText(QStringList{QStringLiteral("DIFF_REJECTED"), tabName}));
    }
}


QString ClaudeIdeBridge::lockfilePath() const
{
    return QDir::homePath() + QStringLiteral("/.claude/ide/%1.lock").arg(m_port);
}

void ClaudeIdeBridge::writeLockfile()
{
    if (m_port == 0)
        return;
    const QString dir = QDir::homePath() + QStringLiteral("/.claude/ide");
    QDir().mkpath(dir);
    QJsonObject obj{
        {QStringLiteral("pid"), (double)QCoreApplication::applicationPid()},
        {QStringLiteral("workspaceFolders"),
         QJsonArray{m_workspaceFolder.isEmpty() ? QDir::homePath()
                                                : m_workspaceFolder}},
        {QStringLiteral("ideName"), QStringLiteral("ForkMesh")},
        {QStringLiteral("transport"), QStringLiteral("ws")},
        {QStringLiteral("runningInWindows"), false},
        {QStringLiteral("authToken"), m_authToken}};
    QFile f(lockfilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate,
               QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void ClaudeIdeBridge::removeLockfile()
{
    if (m_port != 0)
        QFile::remove(lockfilePath());
}
