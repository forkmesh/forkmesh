#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace forkmesh::smb {

constexpr quint16 kDefaultPort = 445;
constexpr quint32 kChunkSize = 65536;
constexpr quint16 kCreditRequest = 64;

enum Command : quint16 {
    Negotiate = 0,
    SessionSetup = 1,
    Logoff = 2,
    TreeConnect = 3,
    TreeDisconnect = 4,
    Create = 5,
    Close = 6,
    Read = 8,
    Write = 9,
    QueryDirectory = 14,
    SetInfo = 17,
};

enum Status : quint32 {
    StatusSuccess = 0x00000000,
    StatusPending = 0x00000103,
    StatusBufferOverflow = 0x80000005,
    StatusNoMoreFiles = 0x80000006,
    StatusMoreProcessingRequired = 0xC0000016,
    StatusEndOfFile = 0xC0000011,
    StatusAccessDenied = 0xC0000022,
    StatusObjectNameNotFound = 0xC0000034,
    StatusObjectNameCollision = 0xC0000035,
    StatusObjectPathNotFound = 0xC000003A,
    StatusSharingViolation = 0xC0000043,
    StatusLogonFailure = 0xC000006D,
    StatusDiskFull = 0xC000007F,
    StatusMediaWriteProtected = 0xC00000A2,
    StatusNotSupported = 0xC00000BB,
    StatusBadNetworkName = 0xC00000CC,
    StatusDirectoryNotEmpty = 0xC0000101,
    StatusFileIsADirectory = 0xC00000BA,
    StatusNotADirectory = 0xC0000103,
};

enum HeaderFlag : quint32 {
    FlagServerToRedir = 0x1,
    FlagAsync = 0x2,
    FlagSigned = 0x8,
};

enum SessionFlag : quint16 {
    SessionGuest = 0x1,
    SessionNull = 0x2,
};

enum Access : quint32 {
    AccessReadData = 0x1,
    AccessWriteData = 0x2,
    AccessAppendData = 0x4,
    AccessReadEa = 0x8,
    AccessWriteEa = 0x10,
    AccessReadAttributes = 0x80,
    AccessWriteAttributes = 0x100,
    AccessDelete = 0x10000,
    AccessReadControl = 0x20000,
    AccessSynchronize = 0x100000,
};

enum Disposition : quint32 {
    DispositionOpen = 1,
    DispositionCreate = 2,
    DispositionOpenIf = 3,
    DispositionOverwriteIf = 5,
};

enum CreateOption : quint32 {
    OptionDirectory = 0x1,
    OptionNonDirectory = 0x40,
    OptionDeleteOnClose = 0x1000,
};

enum Attribute : quint32 {
    AttributeReadOnly = 0x1,
    AttributeHidden = 0x2,
    AttributeSystem = 0x4,
    AttributeDirectory = 0x10,
    AttributeNormal = 0x80,
};

enum NtlmFlag : quint32 {
    NtlmUnicode = 0x00000001,
    NtlmRequestTarget = 0x00000004,
    NtlmNtlm = 0x00000200,
    NtlmAnonymous = 0x00000800,
    NtlmAlwaysSign = 0x00008000,
    NtlmExtendedSessionSecurity = 0x00080000,
    NtlmTargetInfo = 0x00800000,
    Ntlm128 = 0x20000000,
    Ntlm56 = 0x80000000,
};

struct Header {
    quint16 command = 0;
    quint16 creditCharge = 0;
    quint16 credits = kCreditRequest;
    quint32 status = 0;
    quint32 flags = 0;
    quint64 messageId = 0;
    quint64 asyncId = 0;
    quint32 treeId = 0;
    quint64 sessionId = 0;
};

struct Message {
    Header header;
    QByteArray body;
    QByteArray raw;
    bool valid = false;
};

struct FileId {
    QByteArray bytes = QByteArray(16, '\0');
};

struct NegotiateReply {
    bool ok = false;
    quint16 dialect = 0;
    quint16 securityMode = 0;
    quint32 maxRead = 0;
    quint32 maxWrite = 0;
    QByteArray securityBlob;
};

struct CreateReply {
    bool ok = false;
    FileId fileId;
    quint32 attributes = 0;
    qint64 size = 0;
    QDateTime modified;
};

struct Entry {
    QString name;
    bool folder = false;
    qint64 size = 0;
    QDateTime modified;
    bool hidden = false;
    bool readOnly = false;
};

struct ShareInfo {
    QString name;
    QString remark;
    quint32 type = 0;
};

struct NtlmChallenge {
    bool ok = false;
    quint32 flags = 0;
    QByteArray serverChallenge;
    QByteArray targetInfo;
    QByteArray timestamp;
};

struct NtlmResponse {
    QByteArray ntProof;
    QByteArray response;
    QByteArray sessionKey;
};

struct NtlmAuthenticate {
    QByteArray message;
    QByteArray sessionKey;
};

struct Location {
    QString host;
    quint16 port = kDefaultPort;
    QString share;
    QString path;
    QString user;
    bool valid() const { return !host.isEmpty() && !share.isEmpty(); }
};

QByteArray utf16(const QString &text);
QString fromUtf16(const QByteArray &bytes);
QByteArray md4(const QByteArray &data);
QByteArray hmacMd5(const QByteArray &key, const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);

quint16 readU16(const QByteArray &data, int offset);
quint32 readU32(const QByteArray &data, int offset);
quint64 readU64(const QByteArray &data, int offset);
void putU16(QByteArray &data, quint16 value);
void putU32(QByteArray &data, quint32 value);
void putU64(QByteArray &data, quint64 value);

QDateTime fromFileTime(quint64 value);
quint64 toFileTime(const QDateTime &time);

QByteArray ntowfv2(const QString &password, const QString &user, const QString &domain);
NtlmResponse ntlmv2(const QByteArray &ntowf, const QByteArray &serverChallenge, const QByteArray &clientChallenge,
                    const QByteArray &timestamp, const QByteArray &targetInfo);
QByteArray ntlmNegotiate(bool anonymous);
NtlmChallenge parseChallenge(const QByteArray &blob);
NtlmAuthenticate ntlmAuthenticate(const NtlmChallenge &challenge, const QString &user, const QString &password,
                                  const QString &domain, const QString &workstation, const QByteArray &clientChallenge,
                                  bool anonymous);
QByteArray findNtlmToken(const QByteArray &blob);
QByteArray spnegoInit(const QByteArray &token);
QByteArray spnegoResponse(const QByteArray &token);
void splitUser(const QString &input, QString *user, QString *domain);

QByteArray encodeHeader(const Header &header);
QByteArray packet(const Header &header, const QByteArray &body);
Message parseMessage(const QByteArray &raw);
QByteArray frame(const QByteArray &message);
QByteArray sign(const QByteArray &message, const QByteArray &sessionKey);

QByteArray negotiateRequest(const QByteArray &clientGuid);
NegotiateReply parseNegotiate(const Message &message);
QByteArray sessionSetupRequest(const QByteArray &securityBlob);
quint16 sessionFlags(const Message &message);
QByteArray sessionSecurityBlob(const Message &message);
QByteArray treeConnectRequest(const QString &uncPath);
QByteArray createRequest(const QString &path, quint32 access, quint32 attributes, quint32 shareAccess,
                         quint32 disposition, quint32 options);
CreateReply parseCreate(const Message &message);
QByteArray closeRequest(const FileId &fileId);
QByteArray readRequest(const FileId &fileId, quint64 offset, quint32 length);
QByteArray parseRead(const Message &message);
QByteArray writeRequest(const FileId &fileId, quint64 offset, const QByteArray &data);
quint32 parseWrite(const Message &message);
QByteArray queryDirectoryRequest(const FileId &fileId, bool restart, const QString &pattern = QStringLiteral("*"));
QList<Entry> parseDirectory(const Message &message);
QByteArray renameRequest(const FileId &fileId, const QString &target, bool replace = false);

QByteArray rpcBind(quint32 callId);
bool rpcBindAccepted(const QByteArray &pdu);
QByteArray rpcShareEnumRequest(quint32 callId, const QString &server);
bool rpcFragmentComplete(const QByteArray &data, int *length, bool *last);
QByteArray rpcStub(const QByteArray &fragments, bool *ok);
QList<ShareInfo> parseShareEnum(const QByteArray &stub, bool *ok);

QString wirePath(const QString &path);
QString joinPath(const QString &folder, const QString &name);
QString parentPath(const QString &path);
QString leafName(const QString &path);
QString uncPath(const QString &host, const QString &share);

Location parseUrl(const QString &text);
QString formatUrl(const Location &location);
bool parseAddress(const QString &input, QString *host, quint16 *port, QString *error);

bool isLocalAddress(const QHostAddress &address);
bool isLocalName(const QString &host);

QString statusText(quint32 status, const QString &host = {}, const QString &share = {});
QString unreachableText(const QString &host);
QString dialectText(const QString &host);
QString notLocalText();
QString formatSize(qint64 bytes);

}
