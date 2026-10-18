#pragma once

#include "SmbProtocol.h"

#include <QIODevice>

#include <functional>
#include <memory>

namespace forkmesh::smb {

struct Credentials {
    QString user;
    QString password;
    bool guest = false;
};

class Transport
{
public:
    virtual ~Transport() = default;
    virtual bool open(const QString &host, quint16 port, QString *error) = 0;
    virtual bool send(const QByteArray &message) = 0;
    virtual bool receive(QByteArray *message, int timeoutMs) = 0;
    virtual void close() = 0;
};

using TransportFactory = std::function<std::unique_ptr<Transport>()>;
using Progress = std::function<bool(qint64, qint64)>;

std::unique_ptr<Transport> makeTcpTransport();

class Client
{
public:
    explicit Client(TransportFactory factory = {});
    ~Client();

    bool open(const QString &host, quint16 port, const Credentials &credentials, const QString &share);
    void close();
    bool isOpen() const { return m_open; }
    bool guestSession() const { return m_guestSession; }
    bool signing() const { return m_signing; }
    quint16 dialect() const { return m_dialect; }

    QList<ShareInfo> shares(bool *ok);
    bool list(const QString &path, QList<Entry> *entries);
    bool stat(const QString &path, Entry *entry);
    bool readFile(const QString &path, QIODevice *sink, const Progress &progress = {}, qint64 limit = -1);
    bool writeFile(const QString &path, QIODevice *source, qint64 size, const Progress &progress = {});
    bool createFile(const QString &path);
    bool makeFolder(const QString &path);
    bool remove(const QString &path, bool folder);
    bool removeEntry(const QString &path);
    bool rename(const QString &from, const QString &to);

    QString error() const { return m_error; }
    quint32 status() const { return m_status; }
    bool cancelled() const { return m_cancelled; }
    bool tooLarge() const { return m_tooLarge; }

private:
    bool connectOnce();
    bool authenticate(const QString &user, const QString &password, const QString &domain, bool anonymous);
    bool treeConnect(const QString &share, quint32 *treeId);
    void treeDisconnect(quint32 treeId);
    Message request(quint16 command, const QByteArray &body, quint32 treeId);
    Message request(quint16 command, const QByteArray &body) { return request(command, body, m_treeId); }
    bool openFile(const QString &path, quint32 access, quint32 attributes, quint32 shareAccess, quint32 disposition,
                  quint32 options, CreateReply *reply, quint32 treeId);
    bool closeFile(const FileId &fileId, quint32 treeId);
    bool fail(quint32 status);
    bool failText(const QString &text);
    bool retry(const std::function<bool()> &operation);
    bool readPipe(const FileId &pipe, quint32 treeId, QByteArray *out);

    TransportFactory m_factory;
    std::unique_ptr<Transport> m_transport;
    QString m_host;
    quint16 m_port = kDefaultPort;
    Credentials m_credentials;
    QString m_share;
    QByteArray m_clientGuid;
    QByteArray m_sessionKey;
    quint64 m_sessionId = 0;
    quint64 m_nextMessageId = 0;
    quint32 m_treeId = 0;
    quint32 m_maxRead = kChunkSize;
    quint32 m_maxWrite = kChunkSize;
    quint16 m_dialect = 0;
    bool m_signRequired = false;
    bool m_signing = false;
    bool m_guestSession = false;
    bool m_open = false;
    bool m_transportFailed = false;
    bool m_cancelled = false;
    bool m_tooLarge = false;
    QString m_error;
    quint32 m_status = 0;
};

}
