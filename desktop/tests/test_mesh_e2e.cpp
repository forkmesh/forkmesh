// Nightly end-to-end mesh-loop test (issue #352).
//
// Exercises one continuous full-product loop the way a real ForkMesh mesh does:
//
//   publish -> direct browse/clone -> issue -> agent PR -> merge
//
// Every past reliability outage (relay OOM, stale presence, stalled releases)
// would have tripped one of these steps. The test wires the *real* client-side
// building blocks — IssueStore + ForkMeshIdentity (signed issue events) —
// against an in-process RelayStub that speaks the same HTTPS Git + issue-inbox
// protocol the
// Cloudflare Worker relay does. Nothing here depends on the network, an API
// quota, or a model: the agent step shells out to a deterministic stub binary
// (FORKMESH_E2E_STUB_AGENT) so we assert the PR plumbing, not the agent.
//
// The RelayStub deliberately re-implements the *observable* relay contract
// (mirrored from app/src/entry.py): the direct gateway's Git
// smart-HTTP responses (info/refs advertisement + git-upload-pack POST),
// a /api/repositories catalog, a blob-browse endpoint, and a signed-issue
// inbox POST verified with the exact same canonical string the worker uses
// (verify_issue_event <-> IssueStore::canonicalString). The matching worker
// side of that contract is asserted in app/tests.

#include "../src/ForkMeshIdentity.h"
#include "../src/IssueStore.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    if (ok) {
        out << "  ok   " << what << '\n';
    } else {
        ++g_failures;
        out << "  FAIL " << what << '\n';
    }
    out.flush();
}

// Pump the shared event loop until `done` is true or the timeout elapses. Every
// actor here — RelayStub, the driver's QNetworkAccessManager and its
// git subprocesses — lives on this one thread, so the driver must never block:
// it cooperatively pumps so control events and HTTPS responses keep flowing.
bool pumpUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

QByteArray pktLine(const QByteArray &payload)
{
    return QByteArray(QByteArray::number(payload.size() + 4, 16).rightJustified(4, '0')) +
           payload;
}

// ---------------------------------------------------------------------------
// RelayStub: the minimal relay contract the client half of the loop touches.
// ---------------------------------------------------------------------------
class RelayStub : public QObject
{
public:
    explicit RelayStub(QObject *parent = nullptr) : QObject(parent)
    {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &RelayStub::onNewConnection);
    }

    bool listen()
    {
        return m_server->listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return m_server->serverPort(); }
    QString base() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(port());
    }
    void publish(const QString &owner, const QString &repo,
                 const QString &mirrorPath)
    {
        m_owner = owner;
        m_repo = repo;
        m_mirrorPath = mirrorPath;
    }
    const QList<QJsonObject> &inbox() const { return m_inbox; }

private:
    void onNewConnection()
    {
        while (QTcpSocket *sock = m_server->nextPendingConnection()) {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onReadyRead(sock); });
            connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
                m_httpBuf.remove(sock);
                sock->deleteLater();
            });
        }
    }

    void onReadyRead(QTcpSocket *sock)
    {
        QByteArray &buf = m_httpBuf[sock];
        buf += sock->readAll();
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = buf.left(headerEnd);
        // Plain HTTP: for POST we must have the whole body before dispatching.
        const int contentLength = headerValue(header, "content-length").toInt();
        const int total = headerEnd + 4 + contentLength;
        if (buf.size() < total)
            return;
        const QByteArray body = buf.mid(headerEnd + 4, contentLength);
        const QByteArray requestLine = header.left(header.indexOf("\r\n"));
        m_httpBuf.remove(sock);
        handleHttp(sock, requestLine, header, body);
    }

    static QByteArray headerValue(const QByteArray &header, const QByteArray &name)
    {
        for (const QByteArray &line : header.split('\n')) {
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            if (line.left(colon).trimmed().toLower() == name)
                return line.mid(colon + 1).trimmed();
        }
        return {};
    }

    void writeHttp(QTcpSocket *sock, int status, const QByteArray &contentType,
                   const QByteArray &body)
    {
        if (!sock)
            return;
        QByteArray head =
            "HTTP/1.1 " + QByteArray::number(status) + " OK\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            "Connection: close\r\n\r\n";
        sock->write(head);
        sock->write(body);
        sock->flush();
        sock->disconnectFromHost();
    }

    bool runDirectGit(const QStringList &arguments,
                      const QByteArray &input, QByteArray *output)
    {
        QProcess process;
        process.start(QStringLiteral("git"), arguments);
        if (!process.waitForStarted(5000))
            return false;
        if (!input.isEmpty())
            process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(30000) ||
            process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0) {
            return false;
        }
        if (output)
            *output = process.readAllStandardOutput();
        return true;
    }

    void handleHttp(QTcpSocket *sock, const QByteArray &requestLine,
                    const QByteArray &header, const QByteArray &body)
    {
        const QList<QByteArray> parts = requestLine.split(' ');
        const QString method = parts.value(0);
        const QUrl url = QUrl::fromEncoded("http://x" + parts.value(1));
        const QString path = url.path();
        if (path == "/api/repositories") {
            QJsonArray repos;
            if (!m_owner.isEmpty()) {
                repos.append(QJsonObject{{"owner", m_owner},
                                         {"name", m_repo},
                                         {"live", !m_mirrorPath.isEmpty()}});
            }
            writeHttp(sock, 200, "application/json",
                      QJsonDocument(repos).toJson(QJsonDocument::Compact));
            return;
        }

        // TLS is terminated outside this loopback harness. These handlers model
        // the direct-HTTPS mirror endpoint itself and never consult the
        // persistent control socket.
        if (path.endsWith("/info/refs")) {
            QByteArray data;
            const bool ok = runDirectGit(
                {QStringLiteral("-c"),
                 QStringLiteral(
                     "uploadpack.allowTipSHA1InWant=true"),
                 QStringLiteral("-c"),
                 QStringLiteral(
                     "uploadpack.allowReachableSHA1InWant=true"),
                 QStringLiteral("upload-pack"),
                 QStringLiteral("--stateless-rpc"),
                 QStringLiteral("--advertise-refs"),
                 m_mirrorPath},
                {}, &data);
            if (!ok) {
                writeHttp(sock, 502, "text/plain",
                          "direct mirror error");
                return;
            }
            const QByteArray response =
                pktLine("# service=git-upload-pack\n") +
                "0000" + data;
            writeHttp(
                sock, 200,
                "application/x-git-upload-pack-advertisement",
                response);
            return;
        }
        if (path.endsWith("/git-upload-pack") && method == "POST") {
            if (!headerValue(header, "content-encoding").isEmpty()) {
                writeHttp(sock, 400, "text/plain", "unexpected content-encoding");
                return;
            }
            QByteArray data;
            const bool ok = runDirectGit(
                {QStringLiteral("-c"),
                 QStringLiteral(
                     "uploadpack.allowTipSHA1InWant=true"),
                 QStringLiteral("-c"),
                 QStringLiteral(
                     "uploadpack.allowReachableSHA1InWant=true"),
                 QStringLiteral("upload-pack"),
                 QStringLiteral("--stateless-rpc"),
                 m_mirrorPath},
                body, &data);
            writeHttp(
                sock, ok ? 200 : 502,
                ok ? QByteArray(
                         "application/x-git-upload-pack-result")
                   : QByteArray("text/plain"),
                ok ? data : QByteArray("direct mirror error"));
            return;
        }

        // Direct browse: normal HTTP response, no socket request or payload.
        if (path.endsWith("/blob")) {
            const QUrlQuery query(url);
            const QString blobPath = query.queryItemValue("path");
            QByteArray content;
            const bool safe =
                !blobPath.isEmpty() &&
                !blobPath.startsWith(QLatin1Char('/')) &&
                !blobPath.contains(QStringLiteral(".."));
            const bool ok =
                safe &&
                runDirectGit(
                    {QStringLiteral("--git-dir"), m_mirrorPath,
                     QStringLiteral("show"),
                     QStringLiteral("HEAD:") + blobPath},
                    {}, &content);
            const QJsonObject response{
                {"ok", ok},
                {"content",
                 ok ? QString::fromUtf8(content) : QString()},
                {"transport", "direct-https"},
            };
            writeHttp(
                sock, ok ? 200 : 404, "application/json",
                QJsonDocument(response).toJson(
                    QJsonDocument::Compact));
            return;
        }

        // Signed-issue inbox POST: verify with the same canonical the worker uses.
        if (path.endsWith("/issues") && method == "POST") {
            const QJsonObject payload = QJsonDocument::fromJson(body).object();
            const QJsonObject eventJson = payload.value("event").toObject();
            const int number = payload.value("number").toInt();
            IssueEvent ev = IssueEvent::fromJson(eventJson);
            ev.body = eventJson.value("body").toString(); // sent for the sig, not stored
            const bool sigOk = ForkMeshIdentity::verifySignature(
                ev.author, ev.sig, IssueStore::canonicalString(number, ev));
            if (!sigOk) {
                writeHttp(sock, 400, "application/json",
                          "{\"ok\":false,\"error\":\"bad_signature\"}");
                return;
            }
            m_inbox.append(payload);
            writeHttp(sock, 200, "application/json", "{\"ok\":true}");
            return;
        }

        writeHttp(sock, 404, "text/plain", "not found");
    }

    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, QByteArray> m_httpBuf;
    QString m_owner;
    QString m_repo;
    QString m_mirrorPath;
    QList<QJsonObject> m_inbox;
};

// ---------------------------------------------------------------------------
// Driver helpers
// ---------------------------------------------------------------------------
bool runGit(const QString &dir, const QStringList &args, QByteArray *out = nullptr,
            QString *errText = nullptr)
{
    QProcess proc;
    if (!dir.isEmpty())
        proc.setWorkingDirectory(dir);
    proc.start(QStringLiteral("git"), args);
    bool finished = false;
    QObject::connect(&proc,
                     QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [&finished] { finished = true; });
    if (!proc.waitForStarted(5000)) {
        if (errText)
            *errText = QStringLiteral("git failed to start");
        return false;
    }
    // Pump so the loopback direct-HTTP exchange keeps flowing while Git runs;
    // waitForFinished would starve the server's event loop.
    if (!pumpUntil([&finished] { return finished; }, 30000)) {
        proc.kill();
        if (errText)
            *errText = QStringLiteral("git timed out: ") + args.join(' ');
        return false;
    }
    if (out)
        *out = proc.readAllStandardOutput();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (errText)
            *errText = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

struct HttpResult {
    int status = 0;
    QByteArray body;
    bool ok = false;
};

HttpResult httpGet(QNetworkAccessManager &nam, const QString &url)
{
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(url)));
    HttpResult result;
    bool done = false;
    QObject::connect(reply, &QNetworkReply::finished, [&] { done = true; });
    pumpUntil([&done] { return done; }, 15000);
    result.ok = reply->error() == QNetworkReply::NoError;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

HttpResult httpPostJson(QNetworkAccessManager &nam, const QString &url,
                        const QByteArray &body)
{
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = nam.post(request, body);
    HttpResult result;
    bool done = false;
    QObject::connect(reply, &QNetworkReply::finished, [&] { done = true; });
    pumpUntil([&done] { return done; }, 15000);
    result.ok = reply->error() == QNetworkReply::NoError;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTest"));
    QCoreApplication::setApplicationName(QStringLiteral("forkmesh-e2e"));
    QStandardPaths::setTestModeEnabled(true);

    const QString owner = QStringLiteral("e2e");
    const QString repo = QStringLiteral("scratch");

    out << "ForkMesh end-to-end mesh-loop test (publish -> "
           "direct browse/clone -> issue -> agent PR -> merge)\n";
    out.flush();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        err << "cannot create temp dir\n";
        return 1;
    }
    const QString workDir = tmp.filePath(QStringLiteral("work"));
    const QString mirrorDir = tmp.filePath(QStringLiteral("mirror.git"));
    const QString cloneDir = tmp.filePath(QStringLiteral("clone"));

    // --- Scratch repo (the thing we publish) --------------------------------
    QDir().mkpath(workDir);
    QString gitErr;
    bool setup = runGit(QString(), {"init", "-q", "-b", "main", workDir}) &&
                 runGit(workDir, {"config", "user.email", "e2e@forkmesh.test"}) &&
                 runGit(workDir, {"config", "user.name", "E2E"});
    {
        QFile readme(QDir(workDir).filePath(QStringLiteral("README.md")));
        const QByteArray contents("# scratch\nHello mesh.\n");
        const bool written = readme.open(QIODevice::WriteOnly) &&
                             readme.write(contents) == contents.size();
        check(written, QStringLiteral("scratch README written"));
        setup = setup && written;
    }
    setup = setup && runGit(workDir, {"add", "README.md"}) &&
            runGit(workDir, {"commit", "-q", "-m", "initial"}) &&
            runGit(QString(),
                   {"clone", "-q", "--bare", workDir, mirrorDir}, nullptr, &gitErr);
    check(setup, QStringLiteral("scratch repo + bare mirror created"));
    if (!setup) {
        err << gitErr << '\n';
        return 1;
    }

    // --- Identity (signs the issue) -----------------------------------------
    ForkMeshIdentity identity;
    check(identity.load() && identity.isValid(),
          QStringLiteral("node identity generated"));

    // --- Relay + direct repository endpoint ---------------------------------
    RelayStub relay;
    check(relay.listen(), QStringLiteral("relay stub listening on loopback"));
    relay.publish(owner, repo, mirrorDir);

    QNetworkAccessManager nam;

    // --- Step 1: publish shows up in /api/repositories ----------------------
    bool cataloged = pumpUntil(
        [&] {
            HttpResult r = httpGet(nam, relay.base() + "/api/repositories");
            if (!r.ok)
                return false;
            const QJsonArray repos = QJsonDocument::fromJson(r.body).array();
            for (const QJsonValue &v : repos) {
                const QJsonObject o = v.toObject();
                if (o.value("owner").toString() == owner &&
                    o.value("name").toString() == repo && o.value("live").toBool())
                    return true;
            }
            return false;
        },
        8000);
    check(cataloged, QStringLiteral("published repo appears live in /api/repositories"));

    // --- Step 2: browse through the normal direct gateway response ----------
    {
        HttpResult r = httpGet(
            nam, relay.base() + "/api/repo/" + owner + "/" + repo + "/blob?path=README.md");
        const QJsonObject o = QJsonDocument::fromJson(r.body).object();
        const QString content = o.value("content").toString();
        check(r.status == 200 && content.contains("Hello mesh.") &&
                  o.value(QStringLiteral("transport")).toString() ==
                      QStringLiteral("direct-https"),
              QStringLiteral("direct gateway browse returns repository contents without the socket"));
    }

    // --- Step 3: Git clone through direct smart HTTP ------------------------
    {
        const QString cloneUrl = relay.base() + "/" + owner + "/" + repo;
        QString cloneErr;
        // -c protocol.version=0 keeps the negotiation on the v0 advertisement the
        // direct endpoint returns from `upload-pack --advertise-refs`.
        const bool cloned = runGit(
            QString(),
            {"-c", "protocol.version=0", "clone", "-q", cloneUrl, cloneDir}, nullptr,
            &cloneErr);
        bool fileOk = false;
        if (cloned) {
            QFile f(QDir(cloneDir).filePath(QStringLiteral("README.md")));
            fileOk = f.open(QIODevice::ReadOnly) && f.readAll().contains("Hello mesh.");
        }
        check(cloned && fileOk,
              QStringLiteral("direct smart-HTTP clone checks out"));
        if (!cloned)
            err << "  clone error: " << cloneErr << '\n';
    }

    // --- Step 4: file a signed issue via POST .../issues --------------------
    const int issueNumber = 1;
    {
        IssueStore store(workDir, mirrorDir, &identity, QStringLiteral("e2e"));
        IssueEvent ev;
        ev.type = QStringLiteral("open");
        ev.id = QStringLiteral("open-%1").arg(issueNumber);
        ev.title = QStringLiteral("Nightly loop found a bug");
        ev.body = QStringLiteral("The mesh loop should be green.");
        ev = store.makeSignedEvent(issueNumber, ev);

        QJsonObject eventJson = ev.toJson();
        eventJson.insert("body", ev.body);
        const QJsonObject payload{{"owner", owner},
                                  {"repo", repo},
                                  {"number", issueNumber},
                                  {"titleIfNew", ev.title},
                                  {"event", eventJson}};
        HttpResult r = httpPostJson(nam,
                                    relay.base() + "/api/repo/" + owner + "/" + repo +
                                        "/issues",
                                    QJsonDocument(payload).toJson(QJsonDocument::Compact));
        check(r.status == 200 && relay.inbox().size() == 1,
              QStringLiteral("signed issue accepted into the relay inbox"));
    }

    // --- Step 5: owner drains the inbox and commits the issue ---------------
    {
        bool drained = false;
        if (relay.inbox().size() == 1) {
            const QJsonObject item = relay.inbox().first();
            const QJsonObject eventJson = item.value("event").toObject();
            IssueStore store(workDir, mirrorDir, &identity, QStringLiteral("e2e"));
            QString drainError;
            drained = store.applyRemoteEvent(
                item.value("number").toInt(), IssueEvent::fromJson(eventJson),
                item.value("titleIfNew").toString(), &drainError);
        }
        check(drained, QStringLiteral("owner drains inbox and commits the issue"));
        check(QFileInfo::exists(QDir(workDir).filePath(QStringLiteral(
                  ".forkmesh/issues/open/1/issue-1.json"))),
              QStringLiteral("drained issue is stored as one JSON file under .forkmesh"));
    }

    // --- Step 6: run the stub agent -> it produces a PR branch --------------
    const QString branch = QStringLiteral("agent/issue-%1").arg(issueNumber);
    {
        const QString stubAgent = qEnvironmentVariable("FORKMESH_E2E_STUB_AGENT");
        QProcess agent;
        bool finished = false;
        QObject::connect(&agent,
                         QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         [&finished] { finished = true; });
        agent.start(stubAgent,
                    {cloneDir, branch, QString::number(issueNumber)});
        agent.waitForStarted(5000);
        pumpUntil([&finished] { return finished; }, 30000);
        const bool agentOk =
            finished && agent.exitStatus() == QProcess::NormalExit &&
            agent.exitCode() == 0;
        bool branchExists =
            agentOk &&
            runGit(cloneDir, {"rev-parse", "--verify", "-q", "refs/heads/" + branch});
        check(agentOk && branchExists,
              QStringLiteral("stub agent produced a PR branch with a change"));
    }

    // --- Step 7: merge the resulting PR -------------------------------------
    {
        // The PR head lives in the clone; fetch it into the source and merge —
        // exactly the plumbing a maintainer's "merge PR" runs (branch-backed PR).
        QString mergeErr;
        bool merged =
            runGit(workDir, {"fetch", "-q", cloneDir, branch + ":" + branch},
                   nullptr, &mergeErr) &&
            runGit(workDir, {"merge", "-q", "--no-ff", "-m",
                             QStringLiteral("Merge PR for issue #%1").arg(issueNumber),
                             branch},
                   nullptr, &mergeErr);
        bool fixLanded =
            merged && QFile::exists(QDir(workDir).filePath(QStringLiteral("AGENT_FIX.md")));
        check(merged && fixLanded,
              QStringLiteral("PR merges into main and the agent's change lands"));
        if (!merged)
            err << "  merge error: " << mergeErr << '\n';
    }

    out << "\n" << (g_failures == 0 ? "PASS" : "FAIL") << ": " << (g_checks - g_failures)
        << "/" << g_checks << " checks passed\n";
    out.flush();
    return g_failures == 0 ? 0 : 1;
}
