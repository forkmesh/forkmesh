#include "SmbProtocol.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>

namespace forkmesh::smb {

namespace {

constexpr quint32 kBaseNtlmFlags = NtlmUnicode | NtlmRequestTarget | NtlmNtlm | NtlmAlwaysSign | NtlmExtendedSessionSecurity
    | NtlmTargetInfo | Ntlm128 | Ntlm56;
constexpr qint64 kFileTimeEpochOffsetMs = 11644473600000LL;
const QByteArray kNtlmSignature("NTLMSSP\0", 8);

QByteArray derLength(int length)
{
    QByteArray out;
    if (length < 0x80) {
        out.append(char(length));
    } else if (length < 0x100) {
        out.append(char(0x81));
        out.append(char(length));
    } else {
        out.append(char(0x82));
        out.append(char((length >> 8) & 0xff));
        out.append(char(length & 0xff));
    }
    return out;
}

QByteArray der(quint8 tag, const QByteArray &content)
{
    QByteArray out;
    out.append(char(tag));
    out.append(derLength(int(content.size())));
    out.append(content);
    return out;
}

void putField(QByteArray &header, int offset, int length, int payloadOffset)
{
    QByteArray field;
    putU16(field, quint16(length));
    putU16(field, quint16(length));
    putU32(field, quint32(payloadOffset));
    header.replace(offset, 8, field);
}

QByteArray rpcHeader(quint8 type, quint16 fragLength, quint32 callId)
{
    QByteArray out;
    out.append(char(5));
    out.append(char(0));
    out.append(char(type));
    out.append(char(0x03));
    out.append(char(0x10));
    out.append(char(0));
    out.append(char(0));
    out.append(char(0));
    putU16(out, fragLength);
    putU16(out, 0);
    putU32(out, callId);
    return out;
}

void align4(QByteArray &data)
{
    while (data.size() % 4)
        data.append(char(0));
}

class NdrReader
{
public:
    explicit NdrReader(const QByteArray &data) : m_data(data) {}
    bool ok() const { return m_ok; }
    quint32 u32()
    {
        align(4);
        if (m_pos + 4 > m_data.size()) {
            m_ok = false;
            return 0;
        }
        const quint32 value = readU32(m_data, int(m_pos));
        m_pos += 4;
        return value;
    }
    QString string()
    {
        const quint32 max = u32();
        u32();
        const quint32 actual = u32();
        if (!m_ok || actual > max || m_pos + qsizetype(actual) * 2 > m_data.size()) {
            m_ok = false;
            return {};
        }
        QString text = fromUtf16(m_data.mid(m_pos, qsizetype(actual) * 2));
        m_pos += qsizetype(actual) * 2;
        while (text.endsWith(QChar(0)))
            text.chop(1);
        return text;
    }

private:
    void align(int to)
    {
        while (m_pos % to)
            ++m_pos;
    }
    QByteArray m_data;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

bool privateIpv4(quint32 address)
{
    const auto in = [address](quint32 network, int bits) {
        const quint32 mask = bits == 0 ? 0 : 0xffffffffu << (32 - bits);
        return (address & mask) == (network & mask);
    };
    return in(0x0a000000u, 8) || in(0xac100000u, 12) || in(0xc0a80000u, 16) || in(0xa9fe0000u, 16) || in(0x7f000000u, 8);
}

}

QByteArray utf16(const QString &text)
{
    QByteArray out;
    out.reserve(text.size() * 2);
    for (const QChar c : text) {
        out.append(char(c.unicode() & 0xff));
        out.append(char(c.unicode() >> 8));
    }
    return out;
}

QString fromUtf16(const QByteArray &bytes)
{
    QString out;
    out.reserve(bytes.size() / 2);
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2)
        out.append(QChar(ushort(quint8(bytes[i]) | (quint8(bytes[i + 1]) << 8))));
    return out;
}

QByteArray md4(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md4);
}

QByteArray hmacMd5(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Md5);
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

quint16 readU16(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;
    return quint16(quint8(data[offset]) | (quint8(data[offset + 1]) << 8));
}

quint32 readU32(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;
    return quint32(readU16(data, offset)) | (quint32(readU16(data, offset + 2)) << 16);
}

quint64 readU64(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 8 > data.size())
        return 0;
    return quint64(readU32(data, offset)) | (quint64(readU32(data, offset + 4)) << 32);
}

void putU16(QByteArray &data, quint16 value)
{
    data.append(char(value & 0xff));
    data.append(char(value >> 8));
}

void putU32(QByteArray &data, quint32 value)
{
    putU16(data, quint16(value & 0xffff));
    putU16(data, quint16(value >> 16));
}

void putU64(QByteArray &data, quint64 value)
{
    putU32(data, quint32(value & 0xffffffffu));
    putU32(data, quint32(value >> 32));
}

QDateTime fromFileTime(quint64 value)
{
    if (value == 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(qint64(value / 10000) - kFileTimeEpochOffsetMs, QTimeZone::UTC);
}

quint64 toFileTime(const QDateTime &time)
{
    if (!time.isValid())
        return 0;
    return quint64(time.toMSecsSinceEpoch() + kFileTimeEpochOffsetMs) * 10000;
}

QByteArray ntowfv2(const QString &password, const QString &user, const QString &domain)
{
    return hmacMd5(md4(utf16(password)), utf16(user.toUpper() + domain));
}

NtlmResponse ntlmv2(const QByteArray &ntowf, const QByteArray &serverChallenge, const QByteArray &clientChallenge,
                    const QByteArray &timestamp, const QByteArray &targetInfo)
{
    QByteArray blob;
    blob.append(char(1));
    blob.append(char(1));
    blob.append(QByteArray(6, '\0'));
    blob.append(timestamp.left(8).leftJustified(8, '\0'));
    blob.append(clientChallenge.left(8).leftJustified(8, '\0'));
    blob.append(QByteArray(4, '\0'));
    blob.append(targetInfo);
    blob.append(QByteArray(4, '\0'));
    NtlmResponse out;
    out.ntProof = hmacMd5(ntowf, serverChallenge + blob);
    out.response = out.ntProof + blob;
    out.sessionKey = hmacMd5(ntowf, out.ntProof);
    return out;
}

QByteArray ntlmNegotiate(bool anonymous)
{
    QByteArray out = kNtlmSignature;
    putU32(out, 1);
    putU32(out, kBaseNtlmFlags | (anonymous ? NtlmAnonymous : 0));
    out.append(QByteArray(16, '\0'));
    return out;
}

NtlmChallenge parseChallenge(const QByteArray &blob)
{
    NtlmChallenge out;
    const QByteArray data = findNtlmToken(blob);
    if (data.size() < 32 || readU32(data, 8) != 2)
        return out;
    out.flags = readU32(data, 20);
    out.serverChallenge = data.mid(24, 8);
    if (data.size() >= 48) {
        const quint16 length = readU16(data, 40);
        const quint32 offset = readU32(data, 44);
        if (offset + length <= quint32(data.size()))
            out.targetInfo = data.mid(offset, length);
    }
    for (int pos = 0; pos + 4 <= out.targetInfo.size();) {
        const quint16 id = readU16(out.targetInfo, pos);
        const quint16 length = readU16(out.targetInfo, pos + 2);
        if (id == 0)
            break;
        if (id == 7 && length == 8)
            out.timestamp = out.targetInfo.mid(pos + 4, 8);
        pos += 4 + length;
    }
    out.ok = out.serverChallenge.size() == 8;
    return out;
}

NtlmAuthenticate ntlmAuthenticate(const NtlmChallenge &challenge, const QString &user, const QString &password,
                                  const QString &domain, const QString &workstation, const QByteArray &clientChallenge,
                                  bool anonymous)
{
    NtlmAuthenticate out;
    QByteArray lm;
    QByteArray nt;
    if (anonymous) {
        lm = QByteArray(1, '\0');
    } else {
        QByteArray stamp = challenge.timestamp;
        if (stamp.size() != 8) {
            stamp.clear();
            putU64(stamp, toFileTime(QDateTime::currentDateTimeUtc()));
        }
        const NtlmResponse response
            = ntlmv2(ntowfv2(password, user, domain), challenge.serverChallenge, clientChallenge, stamp, challenge.targetInfo);
        lm = QByteArray(24, '\0');
        nt = response.response;
        out.sessionKey = response.sessionKey;
    }
    const QByteArray domainBytes = anonymous ? QByteArray() : utf16(domain);
    const QByteArray userBytes = anonymous ? QByteArray() : utf16(user);
    const QByteArray workstationBytes = utf16(workstation);
    QByteArray header = kNtlmSignature;
    putU32(header, 3);
    header.append(QByteArray(48, '\0'));
    putU32(header, kBaseNtlmFlags | (anonymous ? NtlmAnonymous : 0));
    int offset = int(header.size());
    QByteArray payload;
    const auto add = [&](int field, const QByteArray &value) {
        putField(header, field, int(value.size()), offset + int(payload.size()));
        payload.append(value);
    };
    add(28, domainBytes);
    add(36, userBytes);
    add(44, workstationBytes);
    add(12, lm);
    add(20, nt);
    add(52, {});
    out.message = header + payload;
    return out;
}

QByteArray findNtlmToken(const QByteArray &blob)
{
    const qsizetype at = blob.indexOf(kNtlmSignature);
    return at < 0 ? QByteArray() : blob.mid(at);
}

QByteArray spnegoInit(const QByteArray &token)
{
    const QByteArray ntlmOid = QByteArray::fromHex("2b06010401823702020a");
    const QByteArray mechTypes = der(0xa0, der(0x30, der(0x06, ntlmOid)));
    const QByteArray mechToken = der(0xa2, der(0x04, token));
    const QByteArray init = der(0xa0, der(0x30, mechTypes + mechToken));
    return der(0x60, der(0x06, QByteArray::fromHex("2b0601050502")) + init);
}

QByteArray spnegoResponse(const QByteArray &token)
{
    return der(0xa1, der(0x30, der(0xa2, der(0x04, token))));
}

void splitUser(const QString &input, QString *user, QString *domain)
{
    const QString text = input.trimmed();
    const qsizetype slash = text.indexOf(QLatin1Char('\\'));
    if (slash > 0) {
        *domain = text.left(slash);
        *user = text.mid(slash + 1);
        return;
    }
    domain->clear();
    *user = text;
}

QByteArray encodeHeader(const Header &header)
{
    QByteArray out;
    out.append(char(0xfe));
    out.append("SMB", 3);
    putU16(out, 64);
    putU16(out, header.creditCharge);
    putU32(out, header.status);
    putU16(out, header.command);
    putU16(out, header.credits);
    putU32(out, header.flags);
    putU32(out, 0);
    putU64(out, header.messageId);
    if (header.flags & FlagAsync) {
        putU64(out, header.asyncId);
    } else {
        putU32(out, 0xfeff);
        putU32(out, header.treeId);
    }
    putU64(out, header.sessionId);
    out.append(QByteArray(16, '\0'));
    return out;
}

QByteArray packet(const Header &header, const QByteArray &body)
{
    return encodeHeader(header) + body;
}

Message parseMessage(const QByteArray &raw)
{
    Message out;
    if (raw.size() < 64 || quint8(raw[0]) != 0xfe || raw.mid(1, 3) != "SMB" || readU16(raw, 4) != 64)
        return out;
    out.raw = raw;
    out.header.creditCharge = readU16(raw, 6);
    out.header.status = readU32(raw, 8);
    out.header.command = readU16(raw, 12);
    out.header.credits = readU16(raw, 14);
    out.header.flags = readU32(raw, 16);
    out.header.messageId = readU64(raw, 24);
    if (out.header.flags & FlagAsync)
        out.header.asyncId = readU64(raw, 32);
    else
        out.header.treeId = readU32(raw, 36);
    out.header.sessionId = readU64(raw, 40);
    out.body = raw.mid(64);
    out.valid = true;
    return out;
}

QByteArray frame(const QByteArray &message)
{
    QByteArray out;
    const quint32 length = quint32(message.size());
    out.append(char(0));
    out.append(char((length >> 16) & 0xff));
    out.append(char((length >> 8) & 0xff));
    out.append(char(length & 0xff));
    return out + message;
}

QByteArray sign(const QByteArray &message, const QByteArray &sessionKey)
{
    QByteArray out = message;
    if (out.size() < 64)
        return out;
    const quint32 flags = readU32(out, 16) | FlagSigned;
    QByteArray flagBytes;
    putU32(flagBytes, flags);
    out.replace(16, 4, flagBytes);
    out.replace(48, 16, QByteArray(16, '\0'));
    out.replace(48, 16, hmacSha256(sessionKey, out).left(16));
    return out;
}

QByteArray negotiateRequest(const QByteArray &clientGuid)
{
    QByteArray out;
    putU16(out, 36);
    putU16(out, 2);
    putU16(out, 1);
    putU16(out, 0);
    putU32(out, 0);
    out.append(clientGuid.left(16).leftJustified(16, '\0'));
    putU64(out, 0);
    putU16(out, 0x0202);
    putU16(out, 0x0210);
    return out;
}

NegotiateReply parseNegotiate(const Message &message)
{
    NegotiateReply out;
    const QByteArray &b = message.body;
    if (!message.valid || message.header.status != StatusSuccess || b.size() < 64 || readU16(b, 0) != 65)
        return out;
    out.securityMode = readU16(b, 2);
    out.dialect = readU16(b, 4);
    out.maxRead = readU32(b, 32);
    out.maxWrite = readU32(b, 36);
    const quint16 offset = readU16(b, 56);
    const quint16 length = readU16(b, 58);
    if (offset + length <= message.raw.size())
        out.securityBlob = message.raw.mid(offset, length);
    out.ok = out.dialect == 0x0202 || out.dialect == 0x0210;
    return out;
}

QByteArray sessionSetupRequest(const QByteArray &securityBlob)
{
    QByteArray out;
    putU16(out, 25);
    out.append(char(0));
    out.append(char(1));
    putU32(out, 0);
    putU32(out, 0);
    putU16(out, 64 + 24);
    putU16(out, quint16(securityBlob.size()));
    putU64(out, 0);
    out.append(securityBlob);
    return out;
}

quint16 sessionFlags(const Message &message)
{
    return readU16(message.body, 2);
}

QByteArray sessionSecurityBlob(const Message &message)
{
    const quint16 offset = readU16(message.body, 4);
    const quint16 length = readU16(message.body, 6);
    if (offset < 64 || offset + length > message.raw.size())
        return {};
    return message.raw.mid(offset, length);
}

QByteArray treeConnectRequest(const QString &uncPath)
{
    const QByteArray path = utf16(uncPath);
    QByteArray out;
    putU16(out, 9);
    putU16(out, 0);
    putU16(out, 64 + 8);
    putU16(out, quint16(path.size()));
    out.append(path);
    return out;
}

QByteArray createRequest(const QString &path, quint32 access, quint32 attributes, quint32 shareAccess,
                         quint32 disposition, quint32 options)
{
    const QByteArray name = utf16(wirePath(path));
    QByteArray out;
    putU16(out, 57);
    out.append(char(0));
    out.append(char(0));
    putU32(out, 2);
    putU64(out, 0);
    putU64(out, 0);
    putU32(out, access);
    putU32(out, attributes);
    putU32(out, shareAccess);
    putU32(out, disposition);
    putU32(out, options);
    putU16(out, 64 + 56);
    putU16(out, quint16(name.size()));
    putU32(out, 0);
    putU32(out, 0);
    out.append(name.isEmpty() ? QByteArray(1, '\0') : name);
    return out;
}

CreateReply parseCreate(const Message &message)
{
    CreateReply out;
    const QByteArray &b = message.body;
    if (!message.valid || message.header.status != StatusSuccess || b.size() < 88 || readU16(b, 0) != 89)
        return out;
    out.modified = fromFileTime(readU64(b, 24));
    out.size = qint64(readU64(b, 48));
    out.attributes = readU32(b, 56);
    out.fileId.bytes = b.mid(64, 16);
    out.ok = true;
    return out;
}

QByteArray closeRequest(const FileId &fileId)
{
    QByteArray out;
    putU16(out, 24);
    putU16(out, 0);
    putU32(out, 0);
    out.append(fileId.bytes);
    return out;
}

QByteArray readRequest(const FileId &fileId, quint64 offset, quint32 length)
{
    QByteArray out;
    putU16(out, 49);
    out.append(char(0x50));
    out.append(char(0));
    putU32(out, length);
    putU64(out, offset);
    out.append(fileId.bytes);
    putU32(out, 0);
    putU32(out, 0);
    putU32(out, 0);
    putU16(out, 0);
    putU16(out, 0);
    out.append(char(0));
    return out;
}

QByteArray parseRead(const Message &message)
{
    const QByteArray &b = message.body;
    if (b.size() < 16)
        return {};
    const quint8 offset = quint8(b[2]);
    const quint32 length = readU32(b, 4);
    if (offset < 64 || offset + length > quint32(message.raw.size()))
        return {};
    return message.raw.mid(offset, length);
}

QByteArray writeRequest(const FileId &fileId, quint64 offset, const QByteArray &data)
{
    QByteArray out;
    putU16(out, 49);
    putU16(out, 64 + 48);
    putU32(out, quint32(data.size()));
    putU64(out, offset);
    out.append(fileId.bytes);
    putU32(out, 0);
    putU32(out, 0);
    putU16(out, 0);
    putU16(out, 0);
    putU32(out, 0);
    out.append(data.isEmpty() ? QByteArray(1, '\0') : data);
    return out;
}

quint32 parseWrite(const Message &message)
{
    return readU32(message.body, 4);
}

QByteArray queryDirectoryRequest(const FileId &fileId, bool restart, const QString &pattern)
{
    const QByteArray name = utf16(pattern);
    QByteArray out;
    putU16(out, 33);
    out.append(char(0x01));
    out.append(char(restart ? 0x01 : 0x00));
    putU32(out, 0);
    out.append(fileId.bytes);
    putU16(out, 64 + 32);
    putU16(out, quint16(name.size()));
    putU32(out, kChunkSize);
    out.append(name);
    return out;
}

QList<Entry> parseDirectory(const Message &message)
{
    QList<Entry> out;
    const QByteArray &b = message.body;
    if (b.size() < 8)
        return out;
    const quint16 offset = readU16(b, 2);
    const quint32 length = readU32(b, 4);
    if (offset < 64 || offset + length > quint32(message.raw.size()))
        return out;
    const QByteArray data = message.raw.mid(offset, length);
    int pos = 0;
    while (pos + 64 <= data.size()) {
        const quint32 next = readU32(data, pos);
        const quint32 nameLength = readU32(data, pos + 60);
        if (pos + 64 + int(nameLength) > data.size())
            break;
        Entry entry;
        entry.name = fromUtf16(data.mid(pos + 64, nameLength));
        const quint32 attributes = readU32(data, pos + 56);
        entry.folder = attributes & AttributeDirectory;
        entry.hidden = (attributes & AttributeHidden) || entry.name.startsWith(QLatin1Char('.'));
        entry.readOnly = attributes & AttributeReadOnly;
        entry.size = entry.folder ? 0 : qint64(readU64(data, pos + 40));
        entry.modified = fromFileTime(readU64(data, pos + 24));
        if (entry.name != QLatin1String(".") && entry.name != QLatin1String(".."))
            out.append(entry);
        if (next == 0)
            break;
        pos += int(next);
    }
    return out;
}

QByteArray renameRequest(const FileId &fileId, const QString &target, bool replace)
{
    const QByteArray name = utf16(wirePath(target));
    QByteArray info;
    info.append(char(replace ? 1 : 0));
    info.append(QByteArray(7, '\0'));
    putU64(info, 0);
    putU32(info, quint32(name.size()));
    info.append(name);
    QByteArray out;
    putU16(out, 33);
    out.append(char(1));
    out.append(char(10));
    putU32(out, quint32(info.size()));
    putU16(out, 64 + 32);
    putU16(out, 0);
    putU32(out, 0);
    out.append(fileId.bytes);
    out.append(info);
    return out;
}

QByteArray rpcBind(quint32 callId)
{
    QByteArray body;
    putU16(body, 4280);
    putU16(body, 4280);
    putU32(body, 0);
    body.append(char(1));
    body.append(char(0));
    putU16(body, 0);
    putU16(body, 0);
    body.append(char(1));
    body.append(char(0));
    body.append(QByteArray::fromHex("c84f324b7016d30112785a47bf6ee188"));
    putU16(body, 3);
    putU16(body, 0);
    body.append(QByteArray::fromHex("045d888aeb1cc9119fe808002b104860"));
    putU32(body, 2);
    return rpcHeader(11, quint16(16 + body.size()), callId) + body;
}

bool rpcBindAccepted(const QByteArray &pdu)
{
    if (pdu.size() < 28 || pdu[2] != char(12))
        return false;
    const quint16 secondaryLength = readU16(pdu, 24);
    int pos = 26 + secondaryLength;
    while (pos % 4)
        ++pos;
    if (pos + 6 > pdu.size() || quint8(pdu[pos]) < 1)
        return false;
    return readU16(pdu, pos + 4) == 0;
}

QByteArray rpcShareEnumRequest(quint32 callId, const QString &server)
{
    QByteArray stub;
    putU32(stub, 0x00020000);
    const QString name = QStringLiteral("\\\\") + server;
    const quint32 count = quint32(name.size() + 1);
    putU32(stub, count);
    putU32(stub, 0);
    putU32(stub, count);
    stub.append(utf16(name));
    putU16(stub, 0);
    align4(stub);
    putU32(stub, 1);
    putU32(stub, 1);
    putU32(stub, 0x00020004);
    putU32(stub, 0);
    putU32(stub, 0);
    putU32(stub, 0xffffffffu);
    putU32(stub, 0x00020008);
    putU32(stub, 0);
    QByteArray body;
    putU32(body, quint32(stub.size()));
    putU16(body, 0);
    putU16(body, 15);
    body.append(stub);
    return rpcHeader(0, quint16(16 + body.size()), callId) + body;
}

bool rpcFragmentComplete(const QByteArray &data, int *length, bool *last)
{
    if (data.size() < 16)
        return false;
    *length = readU16(data, 8);
    *last = quint8(data[3]) & 0x02;
    return *length >= 16 && data.size() >= *length;
}

QByteArray rpcStub(const QByteArray &fragments, bool *ok)
{
    *ok = false;
    QByteArray stub;
    int pos = 0;
    while (pos < fragments.size()) {
        const QByteArray rest = fragments.mid(pos);
        int length = 0;
        bool last = false;
        if (!rpcFragmentComplete(rest, &length, &last) || rest[2] != char(2) || length < 24)
            return {};
        const quint16 authLength = readU16(rest, 10);
        stub.append(rest.mid(24, length - 24 - authLength));
        pos += length;
        if (last) {
            *ok = true;
            return stub;
        }
    }
    return {};
}

QList<ShareInfo> parseShareEnum(const QByteArray &stub, bool *ok)
{
    QList<ShareInfo> out;
    NdrReader reader(stub);
    reader.u32();
    reader.u32();
    const quint32 container = reader.u32();
    if (container == 0) {
        *ok = reader.ok();
        return out;
    }
    const quint32 entries = reader.u32();
    const quint32 buffer = reader.u32();
    if (buffer == 0 || entries == 0) {
        *ok = reader.ok();
        return out;
    }
    const quint32 count = reader.u32();
    if (!reader.ok() || count > 4096) {
        *ok = false;
        return out;
    }
    QList<std::pair<quint32, quint32>> pointers;
    for (quint32 i = 0; i < count; ++i) {
        const quint32 name = reader.u32();
        ShareInfo share;
        share.type = reader.u32();
        const quint32 remark = reader.u32();
        pointers.append({name, remark});
        out.append(share);
    }
    for (quint32 i = 0; i < count; ++i) {
        if (pointers[i].first)
            out[i].name = reader.string();
        if (pointers[i].second)
            out[i].remark = reader.string();
    }
    *ok = reader.ok();
    return out;
}

QString wirePath(const QString &path)
{
    QString out = path;
    out.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (out.startsWith(QLatin1Char('\\')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char('\\')))
        out.chop(1);
    return out;
}

QString joinPath(const QString &folder, const QString &name)
{
    QString base = folder;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    while (base.startsWith(QLatin1Char('/')))
        base.remove(0, 1);
    return base.isEmpty() ? name : base + QLatin1Char('/') + name;
}

QString parentPath(const QString &path)
{
    QString text = path;
    while (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    const qsizetype slash = text.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QString() : text.left(slash);
}

QString leafName(const QString &path)
{
    QString text = path;
    while (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    return text.mid(text.lastIndexOf(QLatin1Char('/')) + 1);
}

QString uncPath(const QString &host, const QString &share)
{
    return QStringLiteral("\\\\%1\\%2").arg(host, share);
}

Location parseUrl(const QString &text)
{
    Location out;
    QString rest = text.trimmed();
    if (rest.startsWith(QLatin1String("smb://"), Qt::CaseInsensitive))
        rest = rest.mid(6);
    else if (rest.startsWith(QLatin1String("\\\\")))
        rest = rest.mid(2);
    rest.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const qsizetype at = rest.indexOf(QLatin1Char('@'));
    const qsizetype firstSlash = rest.indexOf(QLatin1Char('/'));
    if (at >= 0 && (firstSlash < 0 || at < firstSlash)) {
        out.user = QUrl::fromPercentEncoding(rest.left(at).toUtf8());
        rest = rest.mid(at + 1);
    }
    const QStringList parts = rest.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return out;
    QString error;
    if (!parseAddress(parts.first(), &out.host, &out.port, &error)) {
        out.host.clear();
        return out;
    }
    if (parts.size() > 1)
        out.share = QUrl::fromPercentEncoding(parts.at(1).toUtf8());
    QStringList path;
    for (qsizetype i = 2; i < parts.size(); ++i)
        path << QUrl::fromPercentEncoding(parts.at(i).toUtf8());
    out.path = path.join(QLatin1Char('/'));
    return out;
}

QString formatUrl(const Location &location)
{
    QString host = location.host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(location.host) : location.host;
    if (location.port != kDefaultPort)
        host += QStringLiteral(":%1").arg(location.port);
    QString out = QStringLiteral("smb://") + host + QLatin1Char('/') + location.share + QLatin1Char('/');
    if (!location.path.isEmpty())
        out += location.path;
    return out;
}

bool parseAddress(const QString &input, QString *host, quint16 *port, QString *error)
{
    QString text = input.trimmed();
    if (text.startsWith(QLatin1String("smb://"), Qt::CaseInsensitive))
        text = text.mid(6);
    while (text.startsWith(QLatin1Char('\\')) || text.startsWith(QLatin1Char('/')))
        text.remove(0, 1);
    const qsizetype cut = std::min(text.indexOf(QLatin1Char('/')) < 0 ? text.size() : text.indexOf(QLatin1Char('/')),
                                   text.indexOf(QLatin1Char('\\')) < 0 ? text.size() : text.indexOf(QLatin1Char('\\')));
    text = text.left(cut);
    *port = kDefaultPort;
    if (text.startsWith(QLatin1Char('['))) {
        const qsizetype close = text.indexOf(QLatin1Char(']'));
        if (close < 0) {
            *error = QStringLiteral("Enter the address of the computer or router that shares the files.");
            return false;
        }
        *host = text.mid(1, close - 1);
        const QString tail = text.mid(close + 1);
        if (tail.startsWith(QLatin1Char(':'))) {
            bool ok = false;
            const uint value = tail.mid(1).toUInt(&ok);
            if (!ok || value == 0 || value > 65535) {
                *error = QStringLiteral("The port must be a number from 1 to 65535.");
                return false;
            }
            *port = quint16(value);
        }
    } else if (text.count(QLatin1Char(':')) == 1) {
        *host = text.section(QLatin1Char(':'), 0, 0);
        bool ok = false;
        const uint value = text.section(QLatin1Char(':'), 1).toUInt(&ok);
        if (!ok || value == 0 || value > 65535) {
            *error = QStringLiteral("The port must be a number from 1 to 65535.");
            return false;
        }
        *port = quint16(value);
    } else {
        *host = text;
    }
    *host = host->trimmed();
    static const QString allowed = QStringLiteral("-._:%");
    const bool clean = !host->isEmpty() && std::all_of(host->begin(), host->end(), [](QChar c) {
        return c.isLetterOrNumber() || allowed.contains(c);
    });
    if (!clean) {
        *error = QStringLiteral("Enter the address of the computer or router that shares the files.");
        return false;
    }
    return true;
}

bool isLocalAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (ok)
        return privateIpv4(v4);
    return address.isLoopback() || address.isLinkLocal() || address.isUniqueLocalUnicast();
}

bool isLocalName(const QString &host)
{
    const QString name = host.trimmed().toLower();
    if (name.isEmpty())
        return false;
    QHostAddress address;
    if (address.setAddress(name))
        return isLocalAddress(address);
    if (name == QLatin1String("localhost") || !name.contains(QLatin1Char('.')))
        return true;
    static const QStringList suffixes = {QStringLiteral(".lan"), QStringLiteral(".local"), QStringLiteral(".home"),
                                         QStringLiteral(".home.arpa"), QStringLiteral(".internal"),
                                         QStringLiteral(".localdomain"), QStringLiteral(".router")};
    return std::any_of(suffixes.begin(), suffixes.end(), [&name](const QString &suffix) { return name.endsWith(suffix); });
}

QString statusText(quint32 status, const QString &host, const QString &share)
{
    switch (status) {
    case StatusLogonFailure:
        return QStringLiteral("The user name or password is wrong.");
    case StatusAccessDenied:
        return QStringLiteral("Access denied.");
    case StatusBadNetworkName:
        return QStringLiteral("There is no share named %1 on %2.").arg(share, host);
    case StatusObjectNameNotFound:
    case StatusObjectPathNotFound:
        return QStringLiteral("That file or folder no longer exists.");
    case StatusObjectNameCollision:
        return QStringLiteral("Something with that name is already there.");
    case StatusDirectoryNotEmpty:
        return QStringLiteral("The folder is not empty.");
    case StatusSharingViolation:
        return QStringLiteral("The file is open somewhere else.");
    case StatusDiskFull:
        return QStringLiteral("The disk is full.");
    case StatusMediaWriteProtected:
        return QStringLiteral("The share is read only.");
    default:
        return QStringLiteral("The file server answered 0x%1.").arg(QString::number(status, 16).toUpper().rightJustified(8, QLatin1Char('0')));
    }
}

QString unreachableText(const QString &host)
{
    return QStringLiteral("Could not reach %1. Is file sharing on?").arg(host);
}

QString dialectText(const QString &host)
{
    return QStringLiteral("%1 does not speak SMB 2.1.").arg(host);
}

QString notLocalText()
{
    return QStringLiteral("Network shares must be on your local network.");
}

QString formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const char *units[] = {"KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = -1;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10 ? 1 : 0).arg(QLatin1String(units[unit]));
}

}
