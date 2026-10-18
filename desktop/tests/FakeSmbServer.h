#pragma once

#include "SmbClient.h"

#include <QMap>
#include <QTimeZone>
#include <QMutex>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include <atomic>
#include <memory>
#include <thread>

namespace fakesmb {

using namespace forkmesh::smb;

struct Node {
    bool folder = false;
    QByteArray data;
    QDateTime modified = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
};

class Server
{
public:
    QString user = QStringLiteral("alice");
    QString password = QStringLiteral("secret");
    QStringList shareNames = {QStringLiteral("media"), QStringLiteral("backup")};
    bool allowGuest = false;
    bool allowNamedGuest = false;
    bool signingRequired = false;
    bool pendingOnce = false;
    bool refuseDialects = false;
    std::atomic<int> readDelayMs{0};
    QMap<QString, Node> files;
    int signedRequests = 0;
    int badSignatures = 0;
    int sessionSetups = 0;
    QStringList commands;

    Server()
    {
        files.insert(QString(), Node{true, {}});
    }

    QList<QByteArray> handle(const QByteArray &raw)
    {
        const Message request = parseMessage(raw);
        if (request.valid && request.header.command == Read && readDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(int(readDelayMs)));
        QMutexLocker lock(&m_mutex);
        if (!request.valid)
            return {};
        commands << QString::number(request.header.command);
        if (request.header.flags & FlagSigned) {
            ++signedRequests;
            const QByteArray expected = sign(raw, m_sessionKey);
            if (expected.mid(48, 16) != raw.mid(48, 16))
                ++badSignatures;
        }
        QList<QByteArray> out;
        if (pendingOnce && request.header.command == Read) {
            pendingOnce = false;
            Header interim = request.header;
            interim.status = StatusPending;
            interim.flags = FlagServerToRedir | FlagAsync;
            interim.asyncId = 7;
            QByteArray body;
            putU16(body, 9);
            body.append(QByteArray(7, '\0'));
            out << packet(interim, body);
        }
        quint32 status = StatusSuccess;
        QByteArray body;
        Header header = request.header;
        header.flags = FlagServerToRedir;
        if (m_signedSession && signingRequired && request.header.command > SessionSetup
            && !(request.header.flags & FlagSigned)) {
            status = StatusAccessDenied;
            body = errorBody();
        } else {
            body = dispatch(request, &status, &header);
        }
        header.status = status;
        out << packet(header, body);
        return out;
    }

    QByteArray read(const QString &path) const { return files.value(path).data; }
    bool exists(const QString &path) const { return files.contains(path); }

private:
    struct Handle {
        QString path;
        bool pipe = false;
        bool deleteOnClose = false;
        bool listed = false;
        QByteArray pipeOut;
    };

    static QByteArray errorBody()
    {
        QByteArray body;
        putU16(body, 9);
        body.append(QByteArray(7, '\0'));
        return body;
    }

    static QString key(const QString &wire)
    {
        QString path = wire;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return path;
    }

    QString parentOf(const QString &path) const
    {
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        return slash < 0 ? QString() : path.left(slash);
    }

    QStringList children(const QString &folder) const
    {
        QStringList out;
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (it.key().isEmpty() || parentOf(it.key()) != folder)
                continue;
            out << it.key();
        }
        return out;
    }

    QByteArray challengeBlob()
    {
        QByteArray info;
        putU16(info, 2);
        putU16(info, 12);
        info.append(utf16(QStringLiteral("DOMAIN")));
        putU16(info, 7);
        putU16(info, 8);
        putU64(info, 132000000000000000ULL);
        putU32(info, 0);
        QByteArray blob("NTLMSSP\0", 8);
        putU32(blob, 2);
        putU16(blob, 0);
        putU16(blob, 0);
        putU32(blob, 56);
        putU32(blob, 0xe2898215u);
        m_serverChallenge = QByteArray::fromHex("0123456789abcdef");
        blob.append(m_serverChallenge);
        blob.append(QByteArray(8, '\0'));
        putU16(blob, quint16(info.size()));
        putU16(blob, quint16(info.size()));
        putU32(blob, 56);
        blob.append(QByteArray(8, '\0'));
        blob.append(info);
        return spnegoResponse(blob);
    }

    static QByteArray field(const QByteArray &message, int offset)
    {
        const quint16 length = readU16(message, offset);
        const quint32 at = readU32(message, offset + 4);
        return message.mid(at, length);
    }

    QByteArray sessionReply(quint16 flags, const QByteArray &blob)
    {
        QByteArray body;
        putU16(body, 9);
        putU16(body, flags);
        putU16(body, blob.isEmpty() ? 0 : 72);
        putU16(body, quint16(blob.size()));
        body.append(blob.isEmpty() ? QByteArray(1, '\0') : blob);
        return body;
    }

    QByteArray dispatch(const Message &request, quint32 *status, Header *header)
    {
        const QByteArray &b = request.body;
        const QByteArray &raw = request.raw;
        switch (request.header.command) {
        case Negotiate: {
            if (refuseDialects) {
                *status = StatusNotSupported;
                return errorBody();
            }
            QByteArray body;
            putU16(body, 65);
            putU16(body, signingRequired ? 0x3 : 0x1);
            putU16(body, 0x0210);
            putU16(body, 0);
            body.append(QByteArray(16, 'g'));
            putU32(body, 0);
            putU32(body, 1 << 20);
            putU32(body, 1 << 20);
            putU32(body, 1 << 20);
            putU64(body, 0);
            putU64(body, 0);
            putU16(body, 0);
            putU16(body, 0);
            putU32(body, 0);
            return body;
        }
        case SessionSetup: {
            ++sessionSetups;
            const QByteArray token = findNtlmToken(raw.mid(readU16(b, 12), readU16(b, 14)));
            if (readU32(token, 8) == 1) {
                header->sessionId = 0x1000 + sessionSetups;
                *status = StatusMoreProcessingRequired;
                return sessionReply(0, challengeBlob());
            }
            const QByteArray nt = field(token, 20);
            const QString name = fromUtf16(field(token, 36));
            const QString domain = fromUtf16(field(token, 28));
            if (name.isEmpty() && nt.isEmpty()) {
                if (!allowGuest) {
                    *status = StatusLogonFailure;
                    return errorBody();
                }
                m_signedSession = false;
                return sessionReply(SessionNull, {});
            }
            if (name.compare(user, Qt::CaseInsensitive) != 0) {
                if (allowNamedGuest) {
                    m_signedSession = false;
                    return sessionReply(SessionGuest, {});
                }
                *status = StatusLogonFailure;
                return errorBody();
            }
            const QByteArray ntowf = ntowfv2(password, name, domain);
            const QByteArray proof = hmacMd5(ntowf, m_serverChallenge + nt.mid(16));
            if (proof != nt.left(16)) {
                *status = StatusLogonFailure;
                return errorBody();
            }
            m_sessionKey = hmacMd5(ntowf, proof);
            m_signedSession = true;
            return sessionReply(0, {});
        }
        case TreeConnect: {
            const QString path = fromUtf16(raw.mid(readU16(b, 4), readU16(b, 6)));
            const QString share = path.section(QLatin1Char('\\'), -1);
            if (share == QLatin1String("IPC$")) {
                header->treeId = 2;
            } else if (shareNames.contains(share)) {
                header->treeId = 1;
            } else {
                *status = StatusBadNetworkName;
                return errorBody();
            }
            QByteArray body;
            putU16(body, 16);
            body.append(char(share == QLatin1String("IPC$") ? 2 : 1));
            body.append(char(0));
            putU32(body, 0);
            putU32(body, 0);
            putU32(body, 0x1f01ff);
            return body;
        }
        case TreeDisconnect:
        case Logoff: {
            QByteArray body;
            putU16(body, 4);
            putU16(body, 0);
            return body;
        }
        case Create:
            return create(request, status);
        case Close: {
            const quint64 id = readU64(b, 8);
            const Handle handle = m_handles.take(id);
            if (handle.deleteOnClose) {
                if (!children(handle.path).isEmpty()) {
                    *status = StatusDirectoryNotEmpty;
                    return errorBody();
                }
                files.remove(handle.path);
            }
            QByteArray body;
            putU16(body, 60);
            body.append(QByteArray(58, '\0'));
            return body;
        }
        case Read: {
            const quint32 length = readU32(b, 4);
            const quint64 offset = readU64(b, 8);
            Handle &handle = m_handles[readU64(b, 16)];
            QByteArray data;
            if (handle.pipe) {
                data = handle.pipeOut.left(length);
                handle.pipeOut.remove(0, data.size());
                if (!handle.pipeOut.isEmpty())
                    *status = StatusBufferOverflow;
            } else {
                const QByteArray &content = files[handle.path].data;
                if (qsizetype(offset) >= content.size()) {
                    *status = StatusEndOfFile;
                    return errorBody();
                }
                data = content.mid(qsizetype(offset), length);
            }
            QByteArray body;
            putU16(body, 17);
            body.append(char(80));
            body.append(char(0));
            putU32(body, quint32(data.size()));
            putU32(body, 0);
            putU32(body, 0);
            body.append(data);
            return body;
        }
        case Write: {
            const quint16 dataOffset = readU16(b, 2);
            const quint32 length = readU32(b, 4);
            const quint64 offset = readU64(b, 8);
            Handle &handle = m_handles[readU64(b, 16)];
            const QByteArray data = raw.mid(dataOffset, length);
            if (handle.pipe) {
                handle.pipeOut += rpc(data);
            } else {
                QByteArray &content = files[handle.path].data;
                if (content.size() < qsizetype(offset + length))
                    content.resize(qsizetype(offset + length));
                content.replace(qsizetype(offset), length, data);
            }
            QByteArray body;
            putU16(body, 17);
            putU16(body, 0);
            putU32(body, length);
            putU32(body, 0);
            putU16(body, 0);
            putU16(body, 0);
            return body;
        }
        case QueryDirectory: {
            const bool restart = quint8(b[3]) & 0x01;
            Handle &handle = m_handles[readU64(b, 8)];
            if (restart)
                handle.listed = false;
            if (handle.listed) {
                *status = StatusNoMoreFiles;
                return errorBody();
            }
            handle.listed = true;
            QStringList names = {QStringLiteral("."), QStringLiteral("..")};
            for (const QString &child : children(handle.path))
                names << child;
            QByteArray buffer;
            for (int i = 0; i < names.size(); ++i) {
                const QString &path = names.at(i);
                const bool dots = path == QLatin1String(".") || path == QLatin1String("..");
                const Node node = dots ? Node{true, {}} : files.value(path);
                const QString leaf = dots ? path : path.section(QLatin1Char('/'), -1);
                const QByteArray name = utf16(leaf);
                QByteArray entry;
                putU32(entry, 0);
                putU32(entry, 0);
                putU64(entry, toFileTime(node.modified));
                putU64(entry, toFileTime(node.modified));
                putU64(entry, toFileTime(node.modified));
                putU64(entry, toFileTime(node.modified));
                putU64(entry, quint64(node.data.size()));
                putU64(entry, quint64(node.data.size()));
                putU32(entry, node.folder ? AttributeDirectory : (leaf.startsWith(QLatin1Char('.')) ? AttributeHidden : AttributeNormal));
                putU32(entry, quint32(name.size()));
                entry.append(name);
                while (entry.size() % 8)
                    entry.append(char(0));
                if (i + 1 < names.size()) {
                    QByteArray next;
                    putU32(next, quint32(entry.size()));
                    entry.replace(0, 4, next);
                }
                buffer.append(entry);
            }
            QByteArray body;
            putU16(body, 9);
            putU16(body, 72);
            putU32(body, quint32(buffer.size()));
            body.append(buffer);
            return body;
        }
        case SetInfo: {
            Handle &handle = m_handles[readU64(b, 16)];
            const QByteArray info = raw.mid(readU16(b, 8), readU32(b, 4));
            const QString target = key(fromUtf16(info.mid(20, readU32(info, 16))));
            if (files.contains(target)) {
                *status = StatusObjectNameCollision;
                return errorBody();
            }
            QMap<QString, Node> moved;
            for (auto it = files.begin(); it != files.end();) {
                if (it.key() == handle.path || it.key().startsWith(handle.path + QLatin1Char('/'))) {
                    moved.insert(target + it.key().mid(handle.path.size()), it.value());
                    it = files.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = moved.begin(); it != moved.end(); ++it)
                files.insert(it.key(), it.value());
            handle.path = target;
            QByteArray body;
            putU16(body, 2);
            return body;
        }
        default:
            *status = StatusNotSupported;
            return errorBody();
        }
    }

    QByteArray create(const Message &request, quint32 *status)
    {
        const QByteArray &b = request.body;
        const QString path = key(fromUtf16(request.raw.mid(readU16(b, 44), readU16(b, 46))));
        const quint32 disposition = readU32(b, 36);
        const quint32 options = readU32(b, 40);
        Handle handle;
        handle.path = path;
        if (request.header.treeId == 2) {
            if (path != QLatin1String("srvsvc")) {
                *status = StatusObjectNameNotFound;
                return errorBody();
            }
            handle.pipe = true;
        } else {
            const bool present = files.contains(path);
            const bool wantFolder = options & OptionDirectory;
            if (disposition == DispositionOpen) {
                if (!present) {
                    *status = StatusObjectNameNotFound;
                    return errorBody();
                }
                if (wantFolder && !files[path].folder) {
                    *status = StatusNotADirectory;
                    return errorBody();
                }
                if ((options & OptionNonDirectory) && files[path].folder) {
                    *status = StatusFileIsADirectory;
                    return errorBody();
                }
            } else {
                if (!files.contains(parentOf(path)) || !files[parentOf(path)].folder) {
                    *status = StatusObjectPathNotFound;
                    return errorBody();
                }
                if (present && disposition == DispositionCreate) {
                    *status = StatusObjectNameCollision;
                    return errorBody();
                }
                if (!present || disposition == DispositionOverwriteIf) {
                    Node node;
                    node.folder = wantFolder;
                    files.insert(path, node);
                }
            }
            handle.deleteOnClose = options & OptionDeleteOnClose;
        }
        const quint64 id = ++m_nextHandle;
        m_handles.insert(id, handle);
        const Node node = handle.pipe ? Node{} : files.value(path);
        QByteArray body;
        putU16(body, 89);
        body.append(char(0));
        body.append(char(0));
        putU32(body, 1);
        for (int i = 0; i < 4; ++i)
            putU64(body, toFileTime(node.modified));
        putU64(body, quint64(node.data.size()));
        putU64(body, quint64(node.data.size()));
        putU32(body, node.folder ? AttributeDirectory : AttributeNormal);
        putU32(body, 0);
        putU64(body, id);
        putU64(body, id);
        putU32(body, 0);
        putU32(body, 0);
        return body;
    }

    static QByteArray rpcFrame(quint8 type, quint8 flags, quint32 callId, const QByteArray &body)
    {
        QByteArray out;
        out.append(char(5));
        out.append(char(0));
        out.append(char(type));
        out.append(char(flags));
        out.append(QByteArray::fromHex("10000000"));
        putU16(out, quint16(16 + body.size()));
        putU16(out, 0);
        putU32(out, callId);
        return out + body;
    }

    static void ndrString(QByteArray &out, const QString &text)
    {
        const quint32 count = quint32(text.size() + 1);
        putU32(out, count);
        putU32(out, 0);
        putU32(out, count);
        out.append(utf16(text));
        putU16(out, 0);
        while (out.size() % 4)
            out.append(char(0));
    }

    QByteArray rpc(const QByteArray &pdu)
    {
        const quint32 callId = readU32(pdu, 12);
        if (pdu[2] == char(11)) {
            QByteArray body;
            putU16(body, 4280);
            putU16(body, 4280);
            putU32(body, 0x1234);
            const QByteArray address("\\PIPE\\srvsvc\0", 13);
            putU16(body, quint16(address.size()));
            body.append(address);
            while ((16 + body.size()) % 4)
                body.append(char(0));
            body.append(char(1));
            body.append(QByteArray(3, '\0'));
            putU16(body, 0);
            putU16(body, 0);
            body.append(QByteArray::fromHex("045d888aeb1cc9119fe808002b104860"));
            putU32(body, 2);
            return rpcFrame(12, 3, callId, body);
        }
        struct Row {
            QString name;
            quint32 type;
            QString remark;
        };
        QList<Row> rows;
        for (const QString &name : std::as_const(shareNames))
            rows << Row{name, 0, QStringLiteral("Disk %1").arg(name)};
        rows << Row{QStringLiteral("IPC$"), 0x80000003u, QStringLiteral("IPC Service")};
        rows << Row{QStringLiteral("print$"), 0, QStringLiteral("Printer drivers")};
        QByteArray stub;
        putU32(stub, 1);
        putU32(stub, 1);
        putU32(stub, 0x20000);
        putU32(stub, quint32(rows.size()));
        putU32(stub, 0x20004);
        putU32(stub, quint32(rows.size()));
        for (int i = 0; i < rows.size(); ++i) {
            putU32(stub, 0x20008 + i * 8);
            putU32(stub, rows[i].type);
            putU32(stub, 0x2000c + i * 8);
        }
        for (const Row &row : std::as_const(rows)) {
            ndrString(stub, row.name);
            ndrString(stub, row.remark);
        }
        putU32(stub, quint32(rows.size()));
        putU32(stub, 0);
        putU32(stub, 0);
        const int half = int(stub.size() / 2);
        const auto fragment = [&](const QByteArray &part, quint8 flags) {
            QByteArray body;
            putU32(body, quint32(stub.size()));
            putU16(body, 0);
            body.append(char(0));
            body.append(char(0));
            body.append(part);
            return rpcFrame(2, flags, callId, body);
        };
        return fragment(stub.left(half), 1) + fragment(stub.mid(half), 2);
    }

    QMutex m_mutex;
    QByteArray m_serverChallenge;
    QByteArray m_sessionKey;
    bool m_signedSession = false;
    QMap<quint64, Handle> m_handles;
    quint64 m_nextHandle = 0;
};

class Transport : public forkmesh::smb::Transport
{
public:
    explicit Transport(std::shared_ptr<Server> server) : m_server(std::move(server)) {}
    bool open(const QString &host, quint16, QString *error) override
    {
        if (host == QLatin1String("unreachable")) {
            *error = unreachableText(host);
            return false;
        }
        m_open = true;
        return true;
    }
    bool send(const QByteArray &message) override
    {
        if (!m_open)
            return false;
        m_pending += m_server->handle(message);
        return true;
    }
    bool receive(QByteArray *message, int) override
    {
        if (m_pending.isEmpty())
            return false;
        *message = m_pending.takeFirst();
        return true;
    }
    void close() override { m_open = false; }

private:
    std::shared_ptr<Server> m_server;
    QList<QByteArray> m_pending;
    bool m_open = false;
};

inline forkmesh::smb::TransportFactory factory(std::shared_ptr<Server> server)
{
    return [server] { return std::make_unique<Transport>(server); };
}

class TcpHost
{
public:
    explicit TcpHost(std::shared_ptr<Server> server) : m_server(std::move(server))
    {
        m_thread = std::thread([this] {
            QTcpServer listener;
            listener.listen(QHostAddress::LocalHost);
            m_port = listener.serverPort();
            m_ready = true;
            while (!m_stop) {
                if (!listener.waitForNewConnection(50))
                    continue;
                std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
                QByteArray buffer;
                while (!m_stop && socket->state() == QAbstractSocket::ConnectedState) {
                    if (!socket->waitForReadyRead(50))
                        continue;
                    buffer += socket->readAll();
                    while (buffer.size() >= 4) {
                        const quint32 length = (quint32(quint8(buffer[1])) << 16) | (quint32(quint8(buffer[2])) << 8)
                            | quint32(quint8(buffer[3]));
                        if (buffer.size() < qsizetype(4 + length))
                            break;
                        const QByteArray message = buffer.mid(4, length);
                        buffer.remove(0, 4 + length);
                        for (const QByteArray &reply : m_server->handle(message))
                            socket->write(frame(reply));
                        socket->waitForBytesWritten(1000);
                    }
                }
            }
        });
        while (!m_ready)
            std::this_thread::yield();
    }
    ~TcpHost()
    {
        m_stop = true;
        m_thread.join();
    }
    quint16 port() const { return m_port; }

private:
    std::shared_ptr<Server> m_server;
    std::thread m_thread;
    std::atomic_bool m_ready{false};
    std::atomic_bool m_stop{false};
    std::atomic<quint16> m_port{0};
};

}
