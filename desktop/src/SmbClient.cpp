#include "SmbClient.h"

#include <QHostInfo>
#include <QRandomGenerator>
#include <QTcpSocket>

#include <algorithm>

namespace forkmesh::smb {

namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kReplyTimeoutMs = 30000;
constexpr quint32 kPipeAccess = 0x0012019F;
constexpr quint32 kShareAll = 0x7;

QByteArray randomBytes(int count)
{
    QByteArray out(count, '\0');
    for (int i = 0; i < count; ++i)
        out[i] = char(QRandomGenerator::system()->bounded(256));
    return out;
}

QByteArray shortBody()
{
    QByteArray out;
    putU16(out, 4);
    putU16(out, 0);
    return out;
}

bool pipeReplyComplete(const QByteArray &data)
{
    int pos = 0;
    while (pos < data.size()) {
        int length = 0;
        bool last = false;
        if (!rpcFragmentComplete(data.mid(pos), &length, &last))
            return false;
        pos += length;
        if (last)
            return true;
    }
    return false;
}

class TcpTransport : public Transport
{
public:
    bool open(const QString &host, quint16 port, QString *error) override
    {
        QList<QHostAddress> addresses;
        QHostAddress literal;
        if (literal.setAddress(host)) {
            addresses << literal;
        } else {
            const QHostInfo info = QHostInfo::fromName(host);
            addresses = info.addresses();
        }
        if (addresses.isEmpty()) {
            *error = unreachableText(host);
            return false;
        }
        for (const QHostAddress &address : std::as_const(addresses)) {
            if (!isLocalAddress(address)) {
                *error = notLocalText();
                return false;
            }
        }
        for (const QHostAddress &address : std::as_const(addresses)) {
            m_socket.abort();
            m_socket.connectToHost(address, port);
            if (m_socket.waitForConnected(kConnectTimeoutMs)) {
                m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
                return true;
            }
        }
        *error = unreachableText(host);
        return false;
    }

    bool send(const QByteArray &message) override
    {
        if (m_socket.state() != QAbstractSocket::ConnectedState)
            return false;
        if (m_socket.write(frame(message)) < 0)
            return false;
        while (m_socket.bytesToWrite() > 0) {
            if (!m_socket.waitForBytesWritten(kReplyTimeoutMs))
                return false;
        }
        return true;
    }

    bool receive(QByteArray *message, int timeoutMs) override
    {
        for (;;) {
            if (m_buffer.size() >= 4) {
                const quint32 length = (quint32(quint8(m_buffer[1])) << 16) | (quint32(quint8(m_buffer[2])) << 8)
                    | quint32(quint8(m_buffer[3]));
                if (m_buffer.size() >= qsizetype(4 + length)) {
                    *message = m_buffer.mid(4, length);
                    m_buffer.remove(0, 4 + length);
                    if (message->isEmpty())
                        continue;
                    return true;
                }
            }
            if (m_socket.state() != QAbstractSocket::ConnectedState && m_socket.bytesAvailable() == 0)
                return false;
            if (m_socket.bytesAvailable() == 0 && !m_socket.waitForReadyRead(timeoutMs))
                return false;
            m_buffer += m_socket.readAll();
        }
    }

    void close() override
    {
        m_socket.abort();
        m_buffer.clear();
    }

private:
    QTcpSocket m_socket;
    QByteArray m_buffer;
};

}

std::unique_ptr<Transport> makeTcpTransport()
{
    return std::make_unique<TcpTransport>();
}

Client::Client(TransportFactory factory) : m_factory(std::move(factory)), m_clientGuid(randomBytes(16)) {}

Client::~Client()
{
    close();
}

bool Client::open(const QString &host, quint16 port, const Credentials &credentials, const QString &share)
{
    m_host = host;
    m_port = port;
    m_credentials = credentials;
    m_share = share;
    return connectOnce();
}

void Client::close()
{
    if (m_transport && m_open && !m_transportFailed) {
        if (m_treeId)
            treeDisconnect(m_treeId);
        if (m_sessionId)
            request(Logoff, shortBody(), 0);
    }
    if (m_transport)
        m_transport->close();
    m_transport.reset();
    m_open = false;
    m_treeId = 0;
    m_sessionId = 0;
    m_signing = false;
    m_sessionKey.clear();
}

bool Client::connectOnce()
{
    close();
    m_error.clear();
    m_status = 0;
    m_transportFailed = false;
    m_guestSession = false;
    const auto start = [this] {
        if (m_transport)
            m_transport->close();
        m_transport = m_factory ? m_factory() : makeTcpTransport();
        m_transportFailed = false;
        m_nextMessageId = 0;
        m_sessionId = 0;
        m_signing = false;
        QString error;
        if (!m_transport->open(m_host, m_port, &error))
            return failText(error);
        const Message reply = request(Negotiate, negotiateRequest(m_clientGuid), 0);
        const NegotiateReply negotiated = parseNegotiate(reply);
        if (!negotiated.ok)
            return failText(dialectText(m_host));
        m_dialect = negotiated.dialect;
        m_maxRead = std::min(negotiated.maxRead ? negotiated.maxRead : kChunkSize, kChunkSize);
        m_maxWrite = std::min(negotiated.maxWrite ? negotiated.maxWrite : kChunkSize, kChunkSize);
        m_signRequired = negotiated.securityMode & 0x2;
        return true;
    };
    if (!start())
        return false;
    bool signedIn = false;
    if (m_credentials.guest) {
        signedIn = authenticate(QStringLiteral("guest"), {}, {}, false);
        if (!signedIn) {
            if (m_transportFailed && !start())
                return false;
            signedIn = authenticate({}, {}, {}, true);
        }
    } else {
        QString user;
        QString domain;
        splitUser(m_credentials.user, &user, &domain);
        signedIn = authenticate(user, m_credentials.password, domain, false);
    }
    if (!signedIn)
        return false;
    if (!m_share.isEmpty() && !treeConnect(m_share, &m_treeId))
        return false;
    m_open = true;
    return true;
}

bool Client::authenticate(const QString &user, const QString &password, const QString &domain, bool anonymous)
{
    m_sessionId = 0;
    const Message first = request(SessionSetup, sessionSetupRequest(spnegoInit(ntlmNegotiate(anonymous))), 0);
    if (!first.valid)
        return failText(unreachableText(m_host));
    if (first.header.status != StatusMoreProcessingRequired)
        return fail(first.header.status);
    m_sessionId = first.header.sessionId;
    const NtlmChallenge challenge = parseChallenge(sessionSecurityBlob(first));
    if (!challenge.ok)
        return failText(QStringLiteral("%1 answered the sign-in in a way ForkMesh does not understand.").arg(m_host));
    const NtlmAuthenticate auth
        = ntlmAuthenticate(challenge, user, password, domain, QStringLiteral("FORKMESH"), randomBytes(8), anonymous);
    const Message second = request(SessionSetup, sessionSetupRequest(spnegoResponse(auth.message)), 0);
    if (!second.valid)
        return failText(unreachableText(m_host));
    if (second.header.status != StatusSuccess) {
        m_sessionId = 0;
        return fail(second.header.status);
    }
    m_guestSession = anonymous || (sessionFlags(second) & (SessionGuest | SessionNull));
    m_sessionKey = auth.sessionKey;
    m_signing = m_signRequired && !m_guestSession && !m_sessionKey.isEmpty();
    return true;
}

bool Client::treeConnect(const QString &share, quint32 *treeId)
{
    const Message reply = request(TreeConnect, treeConnectRequest(uncPath(m_host, share)), 0);
    if (!reply.valid)
        return failText(unreachableText(m_host));
    if (reply.header.status != StatusSuccess) {
        m_status = reply.header.status;
        m_error = statusText(reply.header.status, m_host, share);
        return false;
    }
    *treeId = reply.header.treeId;
    return true;
}

void Client::treeDisconnect(quint32 treeId)
{
    request(TreeDisconnect, shortBody(), treeId);
}

Message Client::request(quint16 command, const QByteArray &body, quint32 treeId)
{
    if (!m_transport) {
        m_transportFailed = true;
        return {};
    }
    Header header;
    header.command = command;
    header.messageId = m_nextMessageId++;
    header.sessionId = m_sessionId;
    header.treeId = treeId;
    header.creditCharge = m_dialect == 0x0210 ? 1 : 0;
    QByteArray raw = packet(header, body);
    if (m_signing && command != Negotiate && command != SessionSetup)
        raw = sign(raw, m_sessionKey);
    if (!m_transport->send(raw)) {
        m_transportFailed = true;
        return {};
    }
    for (;;) {
        QByteArray in;
        if (!m_transport->receive(&in, kReplyTimeoutMs)) {
            m_transportFailed = true;
            return {};
        }
        const Message reply = parseMessage(in);
        if (!reply.valid) {
            m_transportFailed = true;
            return {};
        }
        if (reply.header.messageId != header.messageId)
            continue;
        if (reply.header.status == StatusPending && (reply.header.flags & FlagAsync))
            continue;
        return reply;
    }
}

bool Client::openFile(const QString &path, quint32 access, quint32 attributes, quint32 shareAccess, quint32 disposition,
                      quint32 options, CreateReply *reply, quint32 treeId)
{
    const Message message = request(Create, createRequest(path, access, attributes, shareAccess, disposition, options), treeId);
    if (!message.valid)
        return failText(unreachableText(m_host));
    if (message.header.status != StatusSuccess)
        return fail(message.header.status);
    *reply = parseCreate(message);
    if (!reply->ok)
        return failText(QStringLiteral("%1 answered in a way ForkMesh does not understand.").arg(m_host));
    return true;
}

bool Client::closeFile(const FileId &fileId, quint32 treeId)
{
    const Message message = request(Close, closeRequest(fileId), treeId);
    if (!message.valid)
        return failText(unreachableText(m_host));
    if (message.header.status != StatusSuccess)
        return fail(message.header.status);
    return true;
}

bool Client::fail(quint32 status)
{
    m_status = status;
    m_error = statusText(status, m_host, m_share);
    return false;
}

bool Client::failText(const QString &text)
{
    m_status = 0;
    m_error = text;
    return false;
}

bool Client::retry(const std::function<bool()> &operation)
{
    m_cancelled = false;
    m_tooLarge = false;
    if (!m_open && !connectOnce())
        return false;
    m_transportFailed = false;
    if (operation())
        return true;
    if (!m_transportFailed || m_cancelled)
        return false;
    if (!connectOnce())
        return false;
    return operation();
}

QList<ShareInfo> Client::shares(bool *ok)
{
    QList<ShareInfo> out;
    *ok = retry([&] {
        out.clear();
        quint32 treeId = 0;
        if (!treeConnect(QStringLiteral("IPC$"), &treeId))
            return false;
        CreateReply pipe;
        if (!openFile(QStringLiteral("srvsvc"), kPipeAccess, 0, 0x3, DispositionOpen, 0, &pipe, treeId)) {
            treeDisconnect(treeId);
            return false;
        }
        const bool listed = [&] {
            const Message bind = request(Write, writeRequest(pipe.fileId, 0, rpcBind(1)), treeId);
            if (!bind.valid || bind.header.status != StatusSuccess)
                return false;
            QByteArray ack;
            if (!readPipe(pipe.fileId, treeId, &ack) || !rpcBindAccepted(ack))
                return false;
            const Message call = request(Write, writeRequest(pipe.fileId, 0, rpcShareEnumRequest(2, m_host)), treeId);
            if (!call.valid || call.header.status != StatusSuccess)
                return false;
            QByteArray reply;
            if (!readPipe(pipe.fileId, treeId, &reply))
                return false;
            bool stubOk = false;
            const QByteArray stub = rpcStub(reply, &stubOk);
            bool parsed = false;
            const QList<ShareInfo> all = stubOk ? parseShareEnum(stub, &parsed) : QList<ShareInfo>();
            if (!parsed)
                return false;
            for (const ShareInfo &share : all) {
                if ((share.type & 0x0FFFFFFF) == 0 && !share.name.isEmpty() && !share.name.endsWith(QLatin1Char('$')))
                    out.append(share);
            }
            return true;
        }();
        if (m_transportFailed)
            return false;
        closeFile(pipe.fileId, treeId);
        treeDisconnect(treeId);
        if (!listed)
            return failText(QStringLiteral("%1 did not list its shares. Type the share name instead.").arg(m_host));
        std::sort(out.begin(), out.end(), [](const ShareInfo &a, const ShareInfo &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        return true;
    });
    return out;
}

bool Client::readPipe(const FileId &pipe, quint32 treeId, QByteArray *out)
{
    out->clear();
    for (int i = 0; i < 64; ++i) {
        const Message reply = request(Read, readRequest(pipe, 0, kChunkSize), treeId);
        if (!reply.valid)
            return false;
        if (reply.header.status != StatusSuccess && reply.header.status != StatusBufferOverflow)
            return false;
        out->append(parseRead(reply));
        if (reply.header.status == StatusSuccess && pipeReplyComplete(*out))
            return true;
    }
    return false;
}

bool Client::list(const QString &path, QList<Entry> *entries)
{
    return retry([&] {
        entries->clear();
        CreateReply folder;
        if (!openFile(path, AccessReadData | AccessReadAttributes | AccessSynchronize, 0, kShareAll, DispositionOpen,
                      OptionDirectory, &folder, m_treeId))
            return false;
        bool restart = true;
        for (;;) {
            const Message reply = request(QueryDirectory, queryDirectoryRequest(folder.fileId, restart));
            restart = false;
            if (!reply.valid)
                return failText(unreachableText(m_host));
            if (reply.header.status == StatusNoMoreFiles)
                break;
            if (reply.header.status != StatusSuccess) {
                const quint32 status = reply.header.status;
                closeFile(folder.fileId, m_treeId);
                return fail(status);
            }
            const QList<Entry> page = parseDirectory(reply);
            if (page.isEmpty() && reply.body.size() < 8)
                break;
            entries->append(page);
        }
        return closeFile(folder.fileId, m_treeId);
    });
}

bool Client::stat(const QString &path, Entry *entry)
{
    return retry([&] {
        CreateReply file;
        if (!openFile(path, AccessReadAttributes | AccessSynchronize, 0, kShareAll, DispositionOpen, 0, &file, m_treeId))
            return false;
        entry->name = leafName(path);
        entry->folder = file.attributes & AttributeDirectory;
        entry->hidden = (file.attributes & AttributeHidden) || entry->name.startsWith(QLatin1Char('.'));
        entry->readOnly = file.attributes & AttributeReadOnly;
        entry->size = entry->folder ? 0 : file.size;
        entry->modified = file.modified;
        return closeFile(file.fileId, m_treeId);
    });
}

bool Client::readFile(const QString &path, QIODevice *sink, const Progress &progress, qint64 limit)
{
    const qint64 start = sink->pos();
    return retry([&] {
        if (sink->pos() != start && !sink->seek(start))
            return failText(QStringLiteral("The connection dropped during the download."));
        CreateReply file;
        if (!openFile(path, AccessReadData | AccessReadAttributes | AccessSynchronize, 0, 0x1, DispositionOpen,
                      OptionNonDirectory, &file, m_treeId))
            return false;
        if (limit >= 0 && file.size > limit) {
            closeFile(file.fileId, m_treeId);
            m_tooLarge = true;
            return failText(QStringLiteral("The file is too large to open here."));
        }
        qint64 offset = 0;
        if (progress && !progress(0, file.size)) {
            closeFile(file.fileId, m_treeId);
            m_cancelled = true;
            return failText(QStringLiteral("Cancelled."));
        }
        while (offset < file.size) {
            const quint32 want = quint32(std::min<qint64>(m_maxRead, file.size - offset));
            const Message reply = request(Read, readRequest(file.fileId, quint64(offset), want));
            if (!reply.valid)
                return failText(unreachableText(m_host));
            if (reply.header.status == StatusEndOfFile)
                break;
            if (reply.header.status != StatusSuccess) {
                const quint32 status = reply.header.status;
                closeFile(file.fileId, m_treeId);
                return fail(status);
            }
            const QByteArray data = parseRead(reply);
            if (data.isEmpty())
                break;
            if (sink->write(data) != data.size()) {
                closeFile(file.fileId, m_treeId);
                return failText(QStringLiteral("Could not save the downloaded data."));
            }
            offset += data.size();
            if (progress && !progress(offset, file.size)) {
                closeFile(file.fileId, m_treeId);
                m_cancelled = true;
                return failText(QStringLiteral("Cancelled."));
            }
        }
        return closeFile(file.fileId, m_treeId);
    });
}

bool Client::writeFile(const QString &path, QIODevice *source, qint64 size, const Progress &progress)
{
    const qint64 start = source->pos();
    return retry([&] {
        if (source->pos() != start && !source->seek(start))
            return failText(QStringLiteral("The connection dropped during the upload."));
        CreateReply file;
        if (!openFile(path, AccessWriteData | AccessReadAttributes | AccessSynchronize, AttributeNormal, 0x1,
                      DispositionOverwriteIf, OptionNonDirectory, &file, m_treeId))
            return false;
        qint64 offset = 0;
        for (;;) {
            const QByteArray chunk = source->read(m_maxWrite);
            if (chunk.isEmpty())
                break;
            const Message reply = request(Write, writeRequest(file.fileId, quint64(offset), chunk));
            if (!reply.valid)
                return failText(unreachableText(m_host));
            if (reply.header.status != StatusSuccess) {
                const quint32 status = reply.header.status;
                closeFile(file.fileId, m_treeId);
                return fail(status);
            }
            offset += chunk.size();
            if (progress && !progress(offset, size)) {
                closeFile(file.fileId, m_treeId);
                remove(path, false);
                m_cancelled = true;
                return failText(QStringLiteral("Cancelled."));
            }
        }
        return closeFile(file.fileId, m_treeId);
    });
}

bool Client::createFile(const QString &path)
{
    return retry([&] {
        CreateReply file;
        if (!openFile(path, AccessWriteData | AccessReadAttributes | AccessSynchronize, AttributeNormal, kShareAll,
                      DispositionCreate, OptionNonDirectory, &file, m_treeId))
            return false;
        return closeFile(file.fileId, m_treeId);
    });
}

bool Client::makeFolder(const QString &path)
{
    return retry([&] {
        CreateReply folder;
        if (!openFile(path, AccessReadAttributes | AccessSynchronize, AttributeDirectory, kShareAll, DispositionCreate,
                      OptionDirectory, &folder, m_treeId))
            return false;
        return closeFile(folder.fileId, m_treeId);
    });
}

bool Client::remove(const QString &path, bool folder)
{
    return retry([&] {
        CreateReply entry;
        if (!openFile(path, AccessDelete | AccessReadAttributes | AccessSynchronize, 0, kShareAll, DispositionOpen,
                      OptionDeleteOnClose | (folder ? OptionDirectory : OptionNonDirectory), &entry, m_treeId))
            return false;
        return closeFile(entry.fileId, m_treeId);
    });
}

bool Client::removeEntry(const QString &path)
{
    Entry entry;
    if (!stat(path, &entry))
        return false;
    if (!entry.folder)
        return remove(path, false);
    QList<Entry> children;
    if (!list(path, &children))
        return false;
    for (const Entry &child : std::as_const(children)) {
        if (!removeEntry(joinPath(path, child.name)))
            return false;
    }
    return remove(path, true);
}

bool Client::rename(const QString &from, const QString &to)
{
    return retry([&] {
        CreateReply entry;
        if (!openFile(from, AccessDelete | AccessReadAttributes | AccessSynchronize, 0, kShareAll, DispositionOpen, 0,
                      &entry, m_treeId))
            return false;
        const Message reply = request(SetInfo, renameRequest(entry.fileId, to));
        if (!reply.valid)
            return failText(unreachableText(m_host));
        const quint32 status = reply.header.status;
        closeFile(entry.fileId, m_treeId);
        if (status != StatusSuccess)
            return fail(status);
        return true;
    });
}

}
