#include "SecureDriveStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <optional>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace forkmesh::securedrive {

class ContainerFile
{
public:
    explicit ContainerFile(const QString &path) : m_file(path) {}

    bool openRead() { return m_file.open(QIODevice::ReadOnly); }
    bool openWrite() { return QFileInfo::exists(m_file.fileName()) && m_file.open(QIODevice::ReadWrite); }
    qint64 pages() const { return m_file.size() / kPageBytes; }
    qint64 size() const { return m_file.size(); }

    QByteArray readAt(qint64 offset, qint64 length)
    {
        if (!m_file.seek(offset))
            return {};
        QByteArray bytes = m_file.read(length);
        return bytes.size() == length ? bytes : QByteArray();
    }

    bool writeAt(qint64 offset, const QByteArray &bytes)
    {
        return m_file.seek(offset) && m_file.write(bytes) == bytes.size();
    }

    bool sync()
    {
        if (!m_file.flush())
            return false;
#ifdef Q_OS_WIN
        return _commit(m_file.handle()) == 0;
#else
        return ::fsync(m_file.handle()) == 0;
#endif
    }

    bool appendRandom(qint64 pages)
    {
        if (!m_file.seek(m_file.size()))
            return false;
        qint64 left = pages * kPageBytes;
        while (left > 0) {
            const QByteArray block = randomBytes(int(std::min<qint64>(left, qint64(1) << 20)));
            if (m_file.write(block) != block.size())
                return false;
            left -= block.size();
        }
        return sync();
    }

private:
    QFile m_file;
};

namespace {

LeaseTiming g_timing;
qint64 g_inviteLifetime = qint64(24) * 60 * 60 * 1000;
constexpr qint64 kLogUnitLimitPages = 4;

QByteArray label(const QByteArray &name)
{
    return QByteArrayLiteral("forkmesh-secure-drive/fmsd1/") + name;
}

QByteArray compact(const QJsonObject &value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

bool parseObject(const QByteArray &bytes, QJsonObject *value)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *value = document.object();
    return true;
}

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

QByteArray derive(const QByteArray &key, const char *info, int length = kKeyBytes)
{
    QByteArray out;
    if (!hkdf(key, QString::fromLatin1(info), &out, length))
        return {};
    return out;
}

QString epochIdFor(const QByteArray &key)
{
    return QString::fromLatin1(derive(key, "key-id", 8).toHex());
}

QByteArray stripPadding(const QByteArray &plain)
{
    const int end = int(plain.indexOf('\0'));
    return end < 0 ? plain : plain.left(end);
}

QByteArray sealUnit(const QByteArray &key, const QByteArray &aad, const QByteArray &plain, qint64 size)
{
    if (plain.size() > size - kUnitOverhead)
        return {};
    QByteArray padded = plain;
    padded.append(QByteArray(int(size - kUnitOverhead - plain.size()), '\0'));
    const QByteArray nonce = randomBytes(12);
    QByteArray sealed;
    const bool ok = aeadSeal(key, nonce, aad, padded, &sealed);
    wipe(padded);
    return ok ? nonce + sealed : QByteArray();
}

bool openUnit(const QByteArray &key, const QByteArray &aad, const QByteArray &bytes, QByteArray *plain)
{
    if (bytes.size() < kUnitOverhead || key.size() != kKeyBytes)
        return false;
    return aeadOpen(key, bytes.left(12), aad, bytes.mid(12), plain);
}

QJsonObject envelope(const QByteArray &body, const QByteArray &signature)
{
    return {{QStringLiteral("b"), toBase64Url(body)}, {QStringLiteral("s"), toBase64Url(signature)}};
}

bool openEnvelope(const QJsonObject &json, QByteArray *body, QByteArray *signature, QJsonObject *parsed)
{
    return fromBase64Url(json.value(QStringLiteral("b")).toString(), body)
           && fromBase64Url(json.value(QStringLiteral("s")).toString(), signature) && signature->size() == 64
           && parseObject(*body, parsed);
}

QByteArray recordMessage(const QString &driveId, const QByteArray &body)
{
    return label("record\n") + driveId.toUtf8() + '\n' + body;
}

QByteArray rootMessage(const QByteArray &body) { return label("root\n") + body; }
QByteArray keyringMessage(const QByteArray &body) { return label("keyring\n") + body; }
QByteArray requestMessage(const QByteArray &body) { return label("request\n") + body; }

QByteArray mailboxLabel(int mailbox) { return QByteArrayLiteral("keyring/") + QByteArray::number(mailbox); }
QByteArray requestAad(int mailbox) { return label("request/") + QByteArray::number(mailbox); }
QByteArray logAad(const QString &driveId, qint64 start)
{
    return label("log/") + driveId.toUtf8() + '/' + QByteArray::number(start);
}
QByteArray blobAad(const QString &driveId, const QString &blob, qint64 chunk, bool last, qint64 page)
{
    return label("blob/") + driveId.toUtf8() + '/' + blob.toUtf8() + '/' + QByteArray::number(chunk) + '/'
           + (last ? "1" : "0") + '/' + QByteArray::number(page);
}
QByteArray rootAad(bool slotA) { return label(slotA ? "root/A" : "root/B"); }

qint64 mailboxOffset(int mailbox) { return kMailboxPage * kPageBytes + qint64(mailbox) * kSlotBytes; }

QByteArray inviteIdBytes(const QByteArray &secret) { return derive(secret, "invite-id", 12); }

QJsonArray epochsJson(const QList<EpochKey> &epochs)
{
    QJsonArray out;
    for (const EpochKey &epoch : epochs)
        out.append(QJsonObject{{QStringLiteral("id"), epoch.id},
                               {QStringLiteral("n"), epoch.number},
                               {QStringLiteral("key"), toBase64Url(epoch.key)}});
    return out;
}

QList<EpochKey> epochsFrom(const QJsonArray &array)
{
    QList<EpochKey> out;
    for (const QJsonValue &value : array) {
        const QJsonObject json = value.toObject();
        EpochKey epoch;
        epoch.number = json.value(QStringLiteral("n")).toInt();
        if (!fromBase64Url(json.value(QStringLiteral("key")).toString(), &epoch.key)
            || epoch.key.size() != kKeyBytes)
            continue;
        epoch.id = epochIdFor(epoch.key);
        if (epoch.id != json.value(QStringLiteral("id")).toString())
            continue;
        out.append(epoch);
    }
    return out;
}

qint64 chunkCount(qint64 size) { return std::max<qint64>(1, (paddedSize(size) + kChunkBytes - 1) / kChunkBytes); }

qint64 runPages(const QList<Run> &runs)
{
    qint64 total = 0;
    for (const Run &run : runs)
        total += run.count;
    return total;
}

QList<qint64> expand(const QList<Run> &runs)
{
    QList<qint64> pages;
    for (const Run &run : runs)
        for (qint64 i = 0; i < run.count; ++i)
            pages.append(run.start + i);
    return pages;
}

QString quoted(const QString &text) { return QStringLiteral("“%1”").arg(text); }

QString csvField(QString text)
{
    if (text.contains(QLatin1Char(',')) || text.contains(QLatin1Char('"'))
        || text.contains(QLatin1Char('\n'))) {
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + text + QLatin1Char('"');
    }
    return text;
}

bool readSmall(const QString &path, QByteArray *bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > (qint64(1) << 24))
        return false;
    *bytes = file.readAll();
    return true;
}

}  // namespace

int mailboxForInvite(const QByteArray &secret)
{
    const QByteArray bytes = derive(secret, "mailbox", 2);
    if (bytes.size() != 2)
        return 0;
    return int(((quint8(bytes.at(0)) << 8) | quint8(bytes.at(1))) % kMailboxSlots);
}

QList<Run> runsFromJson(const QJsonArray &array)
{
    QList<Run> runs;
    for (const QJsonValue &value : array) {
        const QJsonArray pair = value.toArray();
        const qint64 start = qint64(pair.at(0).toDouble(-1));
        const qint64 count = qint64(pair.at(1).toDouble(-1));
        if (start < kDataPage || count <= 0)
            return {};
        runs.append({start, count});
    }
    return runs;
}

QJsonArray runsToJson(const QList<Run> &runs)
{
    QJsonArray out;
    for (const Run &run : runs)
        out.append(QJsonArray{double(run.start), double(run.count)});
    return out;
}

const EpochKey *Membership::currentEpoch() const
{
    const EpochKey *best = nullptr;
    for (const EpochKey &epoch : epochs)
        if (!best || epoch.number > best->number
            || (epoch.number == best->number && epoch.id > best->id))
            best = &epoch;
    return best;
}

const EpochKey *Membership::epoch(const QString &id) const
{
    for (const EpochKey &epoch : epochs)
        if (epoch.id == id)
            return &epoch;
    return nullptr;
}

QJsonObject Membership::toJson() const
{
    QJsonObject headsJson;
    for (auto it = heads.cbegin(); it != heads.cend(); ++it)
        headsJson.insert(it.key(), QJsonArray{double(it->seq), it->hash});
    return {{QStringLiteral("id"), id},
            {QStringLiteral("container"), container},
            {QStringLiteral("label"), label},
            {QStringLiteral("drive"), driveId},
            {QStringLiteral("root"), rootDevice},
            {QStringLiteral("rootEd25519"), toBase64Url(rootEd25519)},
            {QStringLiteral("epochs"), epochsJson(epochs)},
            {QStringLiteral("mailbox"), mailbox},
            {QStringLiteral("lastSeq"), double(lastSeq)},
            {QStringLiteral("lastHash"), lastHash},
            {QStringLiteral("lamport"), double(lamport)},
            {QStringLiteral("heads"), headsJson},
            {QStringLiteral("rootGeneration"), double(rootGeneration)},
            {QStringLiteral("rootHash"), rootHash},
            {QStringLiteral("removed"), QJsonArray::fromStringList(removedDevices)},
            {QStringLiteral("cachedRoot"), cachedRoot},
            {QStringLiteral("keyringSeen"), keyringSeen},
            {QStringLiteral("joinSecret"), toBase64Url(joinSecret)},
            {QStringLiteral("joinedAt"), double(joinedAt)}};
}

Membership Membership::fromJson(const QJsonObject &json)
{
    Membership m;
    m.id = json.value(QStringLiteral("id")).toString();
    m.container = json.value(QStringLiteral("container")).toString();
    m.label = json.value(QStringLiteral("label")).toString();
    m.driveId = json.value(QStringLiteral("drive")).toString();
    m.rootDevice = json.value(QStringLiteral("root")).toString();
    fromBase64Url(json.value(QStringLiteral("rootEd25519")).toString(), &m.rootEd25519);
    m.epochs = epochsFrom(json.value(QStringLiteral("epochs")).toArray());
    m.mailbox = json.value(QStringLiteral("mailbox")).toInt(-1);
    m.lastSeq = qint64(json.value(QStringLiteral("lastSeq")).toDouble(-1));
    m.lastHash = json.value(QStringLiteral("lastHash")).toString();
    m.lamport = qint64(json.value(QStringLiteral("lamport")).toDouble());
    const QJsonObject headsJson = json.value(QStringLiteral("heads")).toObject();
    for (auto it = headsJson.constBegin(); it != headsJson.constEnd(); ++it) {
        const QJsonArray pair = it.value().toArray();
        m.heads.insert(it.key(), Head{qint64(pair.at(0).toDouble(-1)), pair.at(1).toString()});
    }
    m.rootGeneration = qint64(json.value(QStringLiteral("rootGeneration")).toDouble(-1));
    m.rootHash = json.value(QStringLiteral("rootHash")).toString();
    m.cachedRoot = json.value(QStringLiteral("cachedRoot")).toString();
    m.keyringSeen = json.value(QStringLiteral("keyringSeen")).toBool();
    for (const QJsonValue &value : json.value(QStringLiteral("removed")).toArray())
        m.removedDevices.append(value.toString());
    fromBase64Url(json.value(QStringLiteral("joinSecret")).toString(), &m.joinSecret);
    m.joinedAt = qint64(json.value(QStringLiteral("joinedAt")).toDouble());
    return m;
}

LocalState::LocalState(QString directory) : m_directory(std::move(directory)) {}

LocalState::~LocalState() { lock(); }

QString LocalState::filePath() const { return QDir(m_directory).filePath(QStringLiteral("device.json")); }

bool LocalState::exists() const { return QFileInfo::exists(filePath()); }

bool LocalState::create(const QString &passphrase, const QString &name, Failure *failure)
{
    if (exists())
        return fail(failure, "device_exists");
    if (passphrase.size() < 10)
        return fail(failure, "passphrase_too_short");
    DeviceKeys fresh;
    if (!DeviceKeys::generate(&fresh, failure) || !securedrive::publicKeys(fresh, &publicKeys, failure))
        return false;
    const Kdf kdf = Kdf::fresh();
    QByteArray key;
    if (!stretch(passphrase, kdf, &key, failure))
        return false;
    keys = fresh;
    deviceId = randomId();
    deviceName = name.trimmed().isEmpty() ? QStringLiteral("This computer") : name.trimmed().left(64);
    drives.clear();
    m_kdf = kdf;
    m_key = key;
    wipe(key);
    if (!save(failure)) {
        lock();
        return false;
    }
    return true;
}

bool LocalState::unlock(const QString &passphrase, Failure *failure)
{
    QByteArray bytes;
    QJsonObject document;
    if (!readSmall(filePath(), &bytes) || !parseObject(bytes, &document))
        return fail(failure, "device_unreadable");
    if (document.value(QStringLiteral("suite")).toString() != QLatin1String(kSuite))
        return fail(failure, "device_unreadable");
    Kdf kdf;
    if (!Kdf::fromJson(document.value(QStringLiteral("kdf")).toObject(), &kdf, failure))
        return false;
    QByteArray key;
    if (!stretch(passphrase, kdf, &key, failure))
        return false;
    const bool ok = load(key, kdf, document, failure);
    wipe(key);
    return ok;
}

bool LocalState::load(const QByteArray &key, const Kdf &kdf, const QJsonObject &document,
                      Failure *failure)
{
    QByteArray nonce, sealed, plain;
    QJsonObject inner;
    if (!fromBase64Url(document.value(QStringLiteral("nonce")).toString(), &nonce)
        || !fromBase64Url(document.value(QStringLiteral("sealed")).toString(), &sealed))
        return fail(failure, "device_unreadable");
    if (!aeadOpen(key, nonce, label("local"), sealed, &plain))
        return fail(failure, "wrong_passphrase");
    const bool parsed = parseObject(plain, &inner);
    wipe(plain);
    DeviceKeys loaded;
    PublicKeys published;
    if (!parsed
        || !DeviceKeys::fromJson(inner.value(QStringLiteral("keys")).toObject(), &loaded, failure)
        || !securedrive::publicKeys(loaded, &published, failure))
        return fail(failure, "device_unreadable");
    keys = loaded;
    publicKeys = published;
    deviceId = inner.value(QStringLiteral("device")).toString();
    deviceName = inner.value(QStringLiteral("name")).toString();
    drives.clear();
    for (const QJsonValue &value : inner.value(QStringLiteral("drives")).toArray())
        drives.append(Membership::fromJson(value.toObject()));
    m_kdf = kdf;
    m_key = key;
    return true;
}

bool LocalState::changePassphrase(const QString &current, const QString &next, Failure *failure)
{
    if (!isUnlocked())
        return fail(failure, "locked");
    if (next.size() < 10)
        return fail(failure, "passphrase_too_short");
    QByteArray check;
    if (!stretch(current, m_kdf, &check, failure))
        return false;
    const bool matches = constantTimeEqual(check, m_key);
    wipe(check);
    if (!matches)
        return fail(failure, "wrong_passphrase");
    const Kdf kdf = Kdf::fresh();
    QByteArray key;
    if (!stretch(next, kdf, &key, failure))
        return false;
    const Kdf previousKdf = m_kdf;
    const QByteArray previousKey = m_key;
    m_kdf = kdf;
    m_key = key;
    wipe(key);
    if (!save(failure)) {
        m_kdf = previousKdf;
        m_key = previousKey;
        return false;
    }
    return true;
}

void LocalState::lock()
{
    wipe(m_key);
    m_key.clear();
    keys.clear();
    publicKeys = PublicKeys();
    for (Membership &m : drives) {
        for (EpochKey &epoch : m.epochs)
            wipe(epoch.key);
        wipe(m.joinSecret);
    }
    drives.clear();
    deviceId.clear();
}

bool LocalState::save(Failure *failure)
{
    if (!isUnlocked())
        return fail(failure, "locked");
    QJsonArray drivesJson;
    for (const Membership &m : drives)
        drivesJson.append(m.toJson());
    QByteArray plain = compact({{QStringLiteral("device"), deviceId},
                                {QStringLiteral("name"), deviceName},
                                {QStringLiteral("keys"), keys.toJson()},
                                {QStringLiteral("drives"), drivesJson}});
    const QByteArray nonce = randomBytes(12);
    QByteArray sealed;
    const bool sealedOk = aeadSeal(m_key, nonce, label("local"), plain, &sealed);
    wipe(plain);
    if (!sealedOk)
        return fail(failure, "encryption_failed");
    const QJsonObject document{{QStringLiteral("suite"), QLatin1String(kSuite)},
                               {QStringLiteral("v"), 1},
                               {QStringLiteral("kdf"), m_kdf.toJson()},
                               {QStringLiteral("nonce"), toBase64Url(nonce)},
                               {QStringLiteral("sealed"), toBase64Url(sealed)}};
    if (!QDir().mkpath(m_directory))
        return fail(failure, "device_write_failed");
    QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner);
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
        return fail(failure, "device_write_failed");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(failure, "device_write_failed");
    return true;
}

QJsonObject LocalState::exportDevice() const
{
    QJsonArray drivesJson;
    for (const Membership &m : drives)
        drivesJson.append(m.toJson());
    return {{QStringLiteral("device"), deviceId},
            {QStringLiteral("name"), deviceName},
            {QStringLiteral("keys"), keys.toJson()},
            {QStringLiteral("drives"), drivesJson}};
}

bool LocalState::importDevice(const QJsonObject &device, const QString &passphrase, Failure *failure)
{
    if (passphrase.size() < 10)
        return fail(failure, "passphrase_too_short");
    DeviceKeys loaded;
    PublicKeys published;
    if (!DeviceKeys::fromJson(device.value(QStringLiteral("keys")).toObject(), &loaded, failure)
        || !securedrive::publicKeys(loaded, &published, failure))
        return false;
    const Kdf kdf = Kdf::fresh();
    QByteArray key;
    if (!stretch(passphrase, kdf, &key, failure))
        return false;
    keys = loaded;
    publicKeys = published;
    deviceId = device.value(QStringLiteral("device")).toString();
    deviceName = device.value(QStringLiteral("name")).toString();
    drives.clear();
    for (const QJsonValue &value : device.value(QStringLiteral("drives")).toArray())
        drives.append(Membership::fromJson(value.toObject()));
    m_kdf = kdf;
    m_key = key;
    wipe(key);
    return save(failure);
}

Membership *LocalState::membership(const QString &id)
{
    for (Membership &m : drives)
        if (m.id == id)
            return &m;
    return nullptr;
}

const Membership *LocalState::membership(const QString &id) const
{
    for (const Membership &m : drives)
        if (m.id == id)
            return &m;
    return nullptr;
}

void LocalState::forget(const QString &id)
{
    for (int i = 0; i < drives.size(); ++i) {
        if (drives.at(i).id != id)
            continue;
        for (EpochKey &epoch : drives[i].epochs)
            wipe(epoch.key);
        wipe(drives[i].joinSecret);
        drives.removeAt(i);
        return;
    }
}

QString fileKindForName(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix == QLatin1String("html") || suffix == QLatin1String("htm"))
        return QStringLiteral("document");
    if (suffix == QLatin1String("csv"))
        return QStringLiteral("spreadsheet");
    if (suffix == QLatin1String("svg"))
        return QStringLiteral("drawing");
    if (suffix == QLatin1String("txt") || suffix == QLatin1String("md")
        || suffix == QLatin1String("json") || suffix == QLatin1String("log"))
        return QStringLiteral("text");
    static const QStringList images{QStringLiteral("png"), QStringLiteral("jpg"),
                                    QStringLiteral("jpeg"), QStringLiteral("gif"),
                                    QStringLiteral("bmp"), QStringLiteral("webp")};
    if (images.contains(suffix))
        return QStringLiteral("image");
    return QStringLiteral("file");
}

QString describeRecord(const Record &record, const std::function<QString(const QString &)> &deviceName)
{
    const QJsonObject &x = record.data;
    const QString name = quoted(x.value(QStringLiteral("name")).toString());
    const QString &a = record.action;
    if (a == QLatin1String("drive_created"))
        return QStringLiteral("Created the drive %1").arg(quoted(x.value(QStringLiteral("label")).toString()));
    if (a == QLatin1String("drive_opened"))
        return QStringLiteral("Connected to the drive");
    if (a == QLatin1String("device_added"))
        return QStringLiteral("Approved device %1").arg(name);
    if (a == QLatin1String("device_removed"))
        return QStringLiteral("Removed device %1 and replaced the drive key").arg(name);
    if (a == QLatin1String("invite_created"))
        return QStringLiteral("Created an invitation code");
    if (a == QLatin1String("invite_revoked"))
        return QStringLiteral("Cancelled an invitation code");
    if (a == QLatin1String("invite_used"))
        return QStringLiteral("Used an invitation code to add %1").arg(name);
    if (a == QLatin1String("join_denied"))
        return QStringLiteral("Turned down a request to join from %1").arg(name);
    if (a == QLatin1String("file_created"))
        return QStringLiteral("Added %1").arg(name);
    if (a == QLatin1String("file_saved"))
        return QStringLiteral("Saved a new version of %1").arg(name);
    if (a == QLatin1String("file_restored"))
        return QStringLiteral("Restored version %1 of %2")
            .arg(x.value(QStringLiteral("version")).toInt() + 1)
            .arg(name);
    if (a == QLatin1String("file_renamed"))
        return QStringLiteral("Renamed %1 to %2").arg(quoted(x.value(QStringLiteral("from")).toString()), name);
    if (a == QLatin1String("file_deleted"))
        return QStringLiteral("Deleted %1").arg(name);
    if (a == QLatin1String("file_rekeyed"))
        return QStringLiteral("Re-encrypted %1 under the new drive key").arg(name);
    if (a == QLatin1String("file_opened")) {
        const QString purpose = x.value(QStringLiteral("purpose")).toString();
        if (purpose == QLatin1String("edit"))
            return QStringLiteral("Opened %1 to edit").arg(name);
        if (purpose == QLatin1String("export"))
            return QStringLiteral("Saved a decrypted copy of %1 outside the drive").arg(name);
        if (purpose == QLatin1String("external"))
            return QStringLiteral("Opened %1 in another app").arg(name);
        return QStringLiteral("Opened %1").arg(name);
    }
    if (a == QLatin1String("drive_rekeyed"))
        return QStringLiteral("Replaced the drive key");
    if (a == QLatin1String("log_exported"))
        return QStringLiteral("Exported the activity log");
    if (a == QLatin1String("integrity_alert"))
        return QStringLiteral("Reported a problem: %1")
            .arg(issueText({x.value(QStringLiteral("code")).toString(),
                            x.value(QStringLiteral("device")).toString(),
                            x.value(QStringLiteral("detail")).toString()},
                           deviceName));
    return a;
}

QString issueText(const Issue &issue, const std::function<QString(const QString &)> &deviceName)
{
    const QString device = deviceName ? deviceName(issue.device) : issue.device;
    const QString &c = issue.code;
    if (c == QLatin1String("records_missing"))
        return QStringLiteral("Activity records from %1 are missing from the drive (%2). Someone with access to the drive may have deleted them.").arg(device, issue.detail);
    if (c == QLatin1String("log_forked"))
        return QStringLiteral("%1 has two different activity records with the same number. The log was altered or a device's keys were copied.").arg(device);
    if (c == QLatin1String("chain_broken"))
        return QStringLiteral("The activity records from %1 do not link up (%2).").arg(device, issue.detail);
    if (c == QLatin1String("bad_signature"))
        return QStringLiteral("A record claiming to come from %1 has an invalid signature and was ignored.").arg(device);
    if (c == QLatin1String("unknown_device"))
        return QStringLiteral("Records signed by a device that was never approved were ignored.");
    if (c == QLatin1String("after_removal"))
        return QStringLiteral("%1 wrote records after it was removed; they were ignored.").arg(device);
    if (c == QLatin1String("own_records_missing"))
        return QStringLiteral("Activity records this device wrote are missing from the drive.");
    if (c == QLatin1String("rolled_back") && issue.device.isEmpty())
        return QStringLiteral("The drive's encrypted index went back to an older version (%1). The drive file may have been restored from an old copy.").arg(issue.detail);
    if (c == QLatin1String("untrusted_root"))
        return QStringLiteral("The drive's encrypted index was written by a device that is not an approved member (%1) and was not used.").arg(device);
    if (c == QLatin1String("keyring_missing"))
        return QStringLiteral("This device's key slot on the drive is gone: it was probably removed from the drive, or the drive file was damaged. It can no longer make changes.");
    if (c == QLatin1String("keyring_restored"))
        return QStringLiteral("This device's key slot on the drive was overwritten. The drive still opens with its keys, so it writes the slot again with its next change.");
    if (c == QLatin1String("index_restored"))
        return QStringLiteral("The drive's encrypted index was older than the one this device last saw, or was damaged. This device used its own verified copy; the next change saves it back to the drive.");
    if (c == QLatin1String("drive_truncated"))
        return QStringLiteral("The drive file is shorter than its index says. Part of it was cut off or lost.");
    if (c == QLatin1String("rolled_back"))
        return QStringLiteral("The drive shows older activity from %1 than this device saw before (%2). The drive may have been restored from an old copy.").arg(device, issue.detail);
    if (c == QLatin1String("missing_content"))
        return QStringLiteral("The encrypted content of %1 is missing from the drive.").arg(quoted(issue.detail));
    if (c == QLatin1String("content_damaged"))
        return QStringLiteral("The encrypted content of %1 was changed or damaged and was not opened.").arg(quoted(issue.detail));
    if (c == QLatin1String("untrusted_keyring"))
        return QStringLiteral("A key file that no member device signed was ignored.");
    if (c == QLatin1String("unreadable_objects"))
        return QStringLiteral("%1 encrypted parts of the drive could not be opened with this device's keys.").arg(issue.detail);
    if (c == QLatin1String("misplaced_object"))
        return QStringLiteral("An object from a different drive was found in this folder and ignored.");
    return c;
}


struct Drive::Txn
{
    ContainerFile *file = nullptr;
    QList<QPair<QString, QJsonObject>> records;
    QSet<qint64> reserved;
    QList<Run> freed;
    QList<std::function<bool()>> afterCommit;
    std::function<void()> renew;
    bool bothRoots = false;
};

struct Drive::RootCandidate
{
    qint64 generation = -1;
    int keyNumber = -1;
    QString keyId;
    QJsonObject body;
    QByteArray bodyBytes;
    QByteArray signature;
    QString hash;
};

Drive::Drive(LocalState *state, QString id) : m_state(state), m_id(std::move(id)) {}

Drive::~Drive() = default;

void Drive::setLeaseTiming(const LeaseTiming &timing) { g_timing = timing; }

void Drive::setInviteLifetime(qint64 milliseconds) { g_inviteLifetime = milliseconds; }

const Membership *Drive::membership() const { return m_state ? m_state->membership(m_id) : nullptr; }

Membership *Drive::mutableMembership() const { return m_state ? m_state->membership(m_id) : nullptr; }

QByteArray Drive::fingerprint() const
{
    const Membership *m = membership();
    if (!m)
        return {};
    ContainerFile file(m->container);
    if (!file.openRead())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(file.size()));
    hash.addData(file.readAt(kRootAPage * kPageBytes, (kDataPage - kRootAPage) * kPageBytes));
    return hash.result();
}

bool Drive::create(LocalState *state, const QString &path, const QString &label, QString *id,
                   Failure *failure)
{
    if (!state || !state->isUnlocked())
        return fail(failure, "locked");
    const QFileInfo info(path);
    if (info.exists())
        return fail(failure, "container_exists");
    if (!QFileInfo(info.absolutePath()).isWritable())
        return fail(failure, "folder_unavailable");
    for (const Membership &m : std::as_const(state->drives))
        if (QFileInfo(m.container).absoluteFilePath() == info.absoluteFilePath())
            return fail(failure, "folder_already_connected");
    {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return fail(failure, "folder_unavailable");
        file.close();
        ContainerFile container(info.absoluteFilePath());
        if (!container.openWrite() || !container.appendRandom(kDataPage + kInitialDataPages)) {
            QFile::remove(info.absoluteFilePath());
            return fail(failure, "drive_write_failed");
        }
    }
    Membership m;
    m.id = randomId();
    m.container = info.absoluteFilePath();
    m.label = label.trimmed().isEmpty() ? info.completeBaseName() : label.trimmed().left(80);
    m.driveId = randomId();
    m.rootDevice = state->deviceId;
    m.rootEd25519 = state->publicKeys.ed25519;
    m.mailbox = int(quint8(randomBytes(1).at(0))) % kMailboxSlots;
    EpochKey first;
    first.key = randomBytes(kKeyBytes);
    first.id = epochIdFor(first.key);
    m.epochs.append(first);
    m.joinedAt = now();
    state->drives.append(m);
    Drive drive(state, m.id);
    const QJsonObject device{{QStringLiteral("id"), state->deviceId},
                             {QStringLiteral("name"), state->deviceName},
                             {QStringLiteral("keys"), state->publicKeys.toJson()}};
    const bool ok = drive.transact(
        [&](Txn &txn, Failure *) {
            txn.records.append({QStringLiteral("drive_created"),
                                {{QStringLiteral("label"), m.label},
                                 {QStringLiteral("device"), device},
                                 {QStringLiteral("mailbox"), m.mailbox}}});
            DeviceInfo self;
            self.id = state->deviceId;
            self.keys = state->publicKeys;
            Failure keyringFailure;
            const QByteArray unit = drive.keyringUnit(self, m.mailbox, {}, {}, &keyringFailure);
            if (unit.isEmpty())
                return false;
            ContainerFile *file = txn.file;
            const int mailbox = m.mailbox;
            txn.afterCommit.append([file, mailbox, unit] {
                return file->writeAt(mailboxOffset(mailbox), unit) && file->sync();
            });
            return true;
        },
        failure);
    if (!ok) {
        state->forget(m.id);
        QFile::remove(info.absoluteFilePath());
        return false;
    }
    *id = m.id;
    return true;
}

bool Drive::requestJoin(LocalState *state, const QString &path, const QString &code, QString *id,
                        QString *safety, Failure *failure)
{
    if (!state || !state->isUnlocked())
        return fail(failure, "locked");
    QByteArray secret;
    if (!parseInviteCode(code, &secret))
        return fail(failure, "invalid_invite_code");
    const QFileInfo info(path);
    for (const Membership &m : std::as_const(state->drives))
        if (QFileInfo(m.container).absoluteFilePath() == info.absoluteFilePath())
            return fail(failure, "folder_already_connected");
    ContainerFile container(info.absoluteFilePath());
    if (!container.openWrite())
        return fail(failure, "container_unavailable");
    if (container.pages() < kDataPage)
        return fail(failure, "not_a_drive");
    const QByteArray inviteId = inviteIdBytes(secret);
    const int mailbox = mailboxForInvite(secret);
    const QByteArray body = compact({{QStringLiteral("suite"), QLatin1String(kSuite)},
                                     {QStringLiteral("invite"), toBase64Url(inviteId)},
                                     {QStringLiteral("device"), state->deviceId},
                                     {QStringLiteral("name"), state->deviceName},
                                     {QStringLiteral("keys"), state->publicKeys.toJson()},
                                     {QStringLiteral("time"), double(now())}});
    QByteArray signature;
    if (!sign(state->keys.ed25519, requestMessage(body), &signature))
        return fail(failure, "signing_failed");
    QByteArray requestKey = derive(secret, "request");
    const QByteArray unit = sealUnit(requestKey, requestAad(mailbox), compact(envelope(body, signature)), kSlotBytes);
    wipe(requestKey);
    if (unit.isEmpty())
        return fail(failure, "encryption_failed");
    if (!container.writeAt(mailboxOffset(mailbox), unit) || !container.sync())
        return fail(failure, "drive_write_failed");
    Membership m;
    m.id = randomId();
    m.container = info.absoluteFilePath();
    m.label = info.completeBaseName();
    m.joinSecret = secret;
    m.mailbox = mailbox;
    state->drives.append(m);
    if (!state->save(failure)) {
        state->forget(m.id);
        return false;
    }
    *id = m.id;
    if (safety)
        *safety = safetyNumber(inviteId, state->publicKeys);
    return true;
}

bool Drive::cancelJoin(Failure *failure)
{
    const Membership *m = membership();
    if (!m || !m->isPending())
        return fail(failure, "not_pending");
    ContainerFile container(m->container);
    if (container.openWrite()) {
        QByteArray requestKey = derive(m->joinSecret, "request");
        QByteArray plain;
        if (openUnit(requestKey, requestAad(m->mailbox), container.readAt(mailboxOffset(m->mailbox), kSlotBytes),
                     &plain)) {
            container.writeAt(mailboxOffset(m->mailbox), randomBytes(int(kSlotBytes)));
            container.sync();
        }
        wipe(requestKey);
    }
    m_state->forget(m_id);
    return m_state->save(failure);
}

QString Drive::joinSafetyNumber() const
{
    const Membership *m = membership();
    if (!m || m->joinSecret.isEmpty())
        return {};
    return safetyNumber(inviteIdBytes(m->joinSecret), m_state->publicKeys);
}

bool Drive::scan(Failure *failure)
{
    const Membership *m = membership();
    if (!m)
        return fail(failure, "unknown_drive");
    ContainerFile file(m->container);
    if (!file.openRead())
        return fail(failure, "container_unavailable");
    const QByteArray print = fingerprint();
    if (!load(file, failure))
        return false;
    m_fingerprint = print;
    if (!m_opened && membership() && membership()->isMember() && !m_removed) {
        m_opened = true;
        return note(QStringLiteral("drive_opened"), {}, failure);
    }
    return true;
}

bool Drive::loadPending(ContainerFile &file, Failure *failure)
{
    Membership *m = mutableMembership();
    const QByteArray slot = file.readAt(mailboxOffset(m->mailbox), kSlotBytes);
    QByteArray approveKey = derive(m->joinSecret, "approve");
    QByteArray requestKey = derive(m->joinSecret, "request");
    const QString inviteId = toBase64Url(inviteIdBytes(m->joinSecret));
    QByteArray plain;
    bool adopted = false;
    if (openFromDevice(m_state->keys, mailboxLabel(m->mailbox), slot, &plain, nullptr)) {
        QJsonObject outer, body;
        QByteArray bodyBytes, signature, mac;
        PublicKeys issuer;
        QByteArray rootEd;
        if (parseObject(stripPadding(plain), &outer) && openEnvelope(outer, &bodyBytes, &signature, &body)
            && fromBase64Url(outer.value(QStringLiteral("m")).toString(), &mac)
            && constantTimeEqual(mac, hmac(approveKey, bodyBytes))
            && body.value(QStringLiteral("device")).toString() == m_state->deviceId
            && body.value(QStringLiteral("invite")).toString() == inviteId
            && PublicKeys::fromJson(body.value(QStringLiteral("issuerKeys")).toObject(), &issuer)
            && verify(issuer.ed25519, keyringMessage(bodyBytes), signature)
            && fromBase64Url(body.value(QStringLiteral("root")).toObject().value(QStringLiteral("ed25519")).toString(),
                             &rootEd)
            && rootEd.size() == 32) {
            const QList<EpochKey> epochs = epochsFrom(body.value(QStringLiteral("epochs")).toArray());
            if (!epochs.isEmpty()) {
                m->driveId = body.value(QStringLiteral("drive")).toString();
                m->rootDevice = body.value(QStringLiteral("root")).toObject().value(QStringLiteral("device")).toString();
                m->rootEd25519 = rootEd;
                m->epochs = epochs;
                wipe(m->joinSecret);
                m->joinSecret.clear();
                m->joinedAt = now();
                adopted = true;
            }
        }
        wipe(plain);
    }
    wipe(approveKey);
    if (adopted) {
        wipe(requestKey);
        if (!m_state->save(failure))
            return false;
        return load(file, failure);
    }
    QByteArray request;
    m_joinRequestMissing = !openUnit(requestKey, requestAad(m->mailbox), slot, &request);
    wipe(requestKey);
    m_scanned = true;
    return true;
}

bool Drive::load(ContainerFile &file, Failure *failure)
{
    Membership *m = mutableMembership();
    if (!m)
        return fail(failure, "unknown_drive");
    m_issues.clear();
    m_requests.clear();
    m_joinRequestMissing = false;
    m_removed = false;
    m_totalPages = file.pages();
    if (m_totalPages < kDataPage)
        return fail(failure, "not_a_drive");
    if (m->isPending())
        return loadPending(file, failure);
    if (!m->isMember())
        return fail(failure, "unknown_drive");
    const QJsonObject stateBefore = m->toJson();
    const QString me = m_state->deviceId;

    struct Ring
    {
        QJsonObject body;
        QByteArray bodyBytes;
        QByteArray signature;
        QList<EpochKey> epochs;
    };
    std::optional<Ring> ring;
    if (m->mailbox >= 0) {
        QByteArray plain;
        if (openFromDevice(m_state->keys, mailboxLabel(m->mailbox), file.readAt(mailboxOffset(m->mailbox), kSlotBytes),
                           &plain, nullptr)) {
            QJsonObject outer;
            Ring candidate;
            if (parseObject(stripPadding(plain), &outer)
                && openEnvelope(outer, &candidate.bodyBytes, &candidate.signature, &candidate.body)
                && candidate.body.value(QStringLiteral("drive")).toString() == m->driveId
                && candidate.body.value(QStringLiteral("device")).toString() == me) {
                candidate.epochs = epochsFrom(candidate.body.value(QStringLiteral("epochs")).toArray());
                ring = candidate;
            }
            wipe(plain);
        }
    }

    m_rootABytes = file.readAt(kRootAPage * kPageBytes, kRootPages * kPageBytes);
    m_rootBBytes = file.readAt(kRootBPage * kPageBytes, kRootPages * kPageBytes);

    QMap<qint64, Record> emptyChain;
    QMap<QString, QMap<qint64, Record>> chains;
    QList<Record> accepted;
    QSet<QString> usedKeys;
    int unreadable = 0;
    bool restored = false;
    std::optional<RootCandidate> chosen;

    const auto attempt = [&](const QList<EpochKey> &keys) {
        m_issues.clear();
        chains.clear();
        accepted.clear();
        usedKeys.clear();
        unreadable = 0;
        chosen.reset();
        for (const bool slotA : {true, false}) {
            const QByteArray &bytes = slotA ? m_rootABytes : m_rootBBytes;
            for (const EpochKey &epoch : keys) {
                QByteArray plain;
                if (!openUnit(derive(epoch.key, "root"), rootAad(slotA), bytes, &plain))
                    continue;
                QJsonObject outer;
                RootCandidate candidate;
                if (parseObject(stripPadding(plain), &outer)
                    && openEnvelope(outer, &candidate.bodyBytes, &candidate.signature, &candidate.body)
                    && candidate.body.value(QStringLiteral("drive")).toString() == m->driveId
                    && candidate.body.value(QStringLiteral("v")).toInt() == 2) {
                    candidate.generation = qint64(candidate.body.value(QStringLiteral("gen")).toDouble(-1));
                    candidate.keyNumber = epoch.number;
                    candidate.keyId = epoch.id;
                    candidate.hash = toBase64Url(sha256(rootMessage(candidate.bodyBytes) + candidate.signature));
                    if (m->removedDevices.contains(candidate.body.value(QStringLiteral("writer")).toString())) {
                        addIssue(QStringLiteral("untrusted_root"), candidate.body.value(QStringLiteral("writer")).toString(),
                                 QString());
                        break;
                    }
                    if (!chosen || candidate.keyNumber > chosen->keyNumber
                        || (candidate.keyNumber == chosen->keyNumber && candidate.generation > chosen->generation))
                        chosen = candidate;
                }
                break;
            }
        }
        restored = false;
        QByteArray cachedBytes;
        QJsonObject cachedOuter;
        RootCandidate cached;
        if (!m->cachedRoot.isEmpty() && fromBase64Url(m->cachedRoot, &cachedBytes)
            && parseObject(cachedBytes, &cachedOuter)
            && openEnvelope(cachedOuter, &cached.bodyBytes, &cached.signature, &cached.body)
            && cached.body.value(QStringLiteral("drive")).toString() == m->driveId) {
            cached.generation = qint64(cached.body.value(QStringLiteral("gen")).toDouble(-1));
            cached.hash = toBase64Url(sha256(rootMessage(cached.bodyBytes) + cached.signature));
            if (cached.hash == m->rootHash && (!chosen || cached.generation > chosen->generation)) {
                chosen = cached;
                restored = true;
                addIssue(QStringLiteral("index_restored"), QString(), QString());
            }
        }
        m_logs.clear();
        m_myLastRecords = QJsonArray();
        if (!chosen)
            return;
        usedKeys.insert(chosen->keyId);
        const QJsonObject logs = chosen->body.value(QStringLiteral("logs")).toObject();
        for (auto it = logs.constBegin(); it != logs.constEnd(); ++it) {
            const QString device = it.key();
            const QList<Run> runs = runsFromJson(it.value().toArray());
            m_logs.insert(device, runs);
            for (int r = 0; r < runs.size(); ++r) {
                const Run &run = runs.at(r);
                if (run.start + run.count > m_totalPages) {
                    ++unreadable;
                    continue;
                }
                const QByteArray bytes = file.readAt(run.start * kPageBytes, run.count * kPageBytes);
                QJsonObject payload;
                bool opened = false;
                for (const EpochKey &epoch : keys) {
                    QByteArray plain;
                    if (!openUnit(derive(epoch.key, "log"), logAad(m->driveId, run.start), bytes, &plain))
                        continue;
                    opened = parseObject(stripPadding(plain), &payload);
                    usedKeys.insert(epoch.id);
                    break;
                }
                if (!opened || payload.value(QStringLiteral("device")).toString() != device) {
                    ++unreadable;
                    continue;
                }
                const QJsonArray stored = payload.value(QStringLiteral("records")).toArray();
                if (device == me && r == runs.size() - 1)
                    m_myLastRecords = stored;
                for (const QJsonValue &value : stored) {
                    QByteArray bodyBytes, signature;
                    QJsonObject body;
                    if (!openEnvelope(value.toObject(), &bodyBytes, &signature, &body))
                        continue;
                    Record record;
                    record.device = body.value(QStringLiteral("d")).toString();
                    record.seq = qint64(body.value(QStringLiteral("s")).toDouble(-1));
                    record.lamport = qint64(body.value(QStringLiteral("l")).toDouble());
                    record.time = qint64(body.value(QStringLiteral("t")).toDouble());
                    record.action = body.value(QStringLiteral("a")).toString();
                    record.data = body.value(QStringLiteral("x")).toObject();
                    record.prev = body.value(QStringLiteral("p")).toString();
                    record.witness = body.value(QStringLiteral("w")).toObject();
                    record.stored = value.toObject();
                    record.hash = toBase64Url(sha256(recordMessage(m->driveId, bodyBytes) + signature));
                    if (record.device != device || record.seq < 0)
                        continue;
                    QMap<qint64, Record> &chain = chains[device];
                    const auto found = chain.constFind(record.seq);
                    if (found != chain.constEnd()) {
                        if (found->hash != record.hash)
                            addIssue(QStringLiteral("log_forked"), device, QString());
                        continue;
                    }
                    chain.insert(record.seq, record);
                }
            }
        }

        QMap<QString, QByteArray> trusted;
        trusted.insert(m->rootDevice, m->rootEd25519);
        QSet<QString> verified;
        bool changed = true;
        while (changed) {
            changed = false;
            const QStringList pending = trusted.keys();
            for (const QString &device : pending) {
                if (verified.contains(device))
                    continue;
                verified.insert(device);
                changed = true;
                const QByteArray ed = trusted.value(device);
                qint64 expected = 0;
                QString lastHash;
                const auto chainIt = chains.constFind(device);
                const QMap<qint64, Record> &chain = chainIt == chains.constEnd() ? emptyChain : chainIt.value();
                for (auto it = chain.cbegin(); it != chain.cend(); ++it) {
                    const Record &record = it.value();
                    QByteArray bodyBytes, signature;
                    QJsonObject body;
                    if (!openEnvelope(record.stored, &bodyBytes, &signature, &body)
                        || !verify(ed, recordMessage(m->driveId, bodyBytes), signature)) {
                        addIssue(QStringLiteral("bad_signature"), device, QString());
                        continue;
                    }
                    if (record.seq != expected)
                        addIssue(QStringLiteral("records_missing"), device,
                                 record.seq - expected == 1
                                     ? QStringLiteral("record %1").arg(expected + 1)
                                     : QStringLiteral("records %1 to %2").arg(expected + 1).arg(record.seq));
                    else if (record.prev != lastHash)
                        addIssue(QStringLiteral("chain_broken"), device, QStringLiteral("record %1").arg(record.seq + 1));
                    expected = record.seq + 1;
                    lastHash = record.hash;
                    accepted.append(record);
                    if (record.action == QLatin1String("device_added")) {
                        PublicKeys keys;
                        const QString added = record.data.value(QStringLiteral("device")).toString();
                        if (PublicKeys::fromJson(record.data.value(QStringLiteral("keys")).toObject(), &keys)
                            && !trusted.contains(added))
                            trusted.insert(added, keys.ed25519);
                    }
                }
            }
        }
        for (auto it = chains.cbegin(); it != chains.cend(); ++it)
            if (!trusted.contains(it.key()))
                addIssue(QStringLiteral("unknown_device"), it.key(), QString());
        fold(accepted);
    };

    QList<EpochKey> keys = m->epochs;
    QSet<QString> ringKeys;
    if (ring)
        for (const EpochKey &epoch : ring->epochs)
            if (!m->epoch(epoch.id)) {
                keys.append(epoch);
                ringKeys.insert(epoch.id);
            }
    std::sort(keys.begin(), keys.end(), [](const EpochKey &a, const EpochKey &b) { return a.number > b.number; });
    attempt(keys);

    bool ringValid = false;
    if (ring) {
        const QString issuer = ring->body.value(QStringLiteral("issuer")).toString();
        const auto device = m_devices.constFind(issuer);
        ringValid = device != m_devices.constEnd() && !device->removed
                    && verify(device->keys.ed25519, keyringMessage(ring->bodyBytes), ring->signature);
        if (!ringValid && !ringKeys.isEmpty()) {
            bool usedRingKey = false;
            for (const QString &id : std::as_const(ringKeys))
                usedRingKey = usedRingKey || usedKeys.contains(id);
            keys.erase(std::remove_if(keys.begin(), keys.end(),
                                      [&](const EpochKey &e) { return ringKeys.contains(e.id); }),
                       keys.end());
            if (usedRingKey)
                attempt(keys);
            addIssue(QStringLiteral("untrusted_keyring"), issuer, QString());
        }
    }
    if (ringValid) {
        m->keyringSeen = true;
        for (const EpochKey &epoch : ring->epochs)
            if (!m->epoch(epoch.id))
                m->epochs.append(epoch);
    }
    const bool slotEmpty = !ring && m->keyringSeen;
    const bool keyringGone = slotEmpty && (!chosen || restored);
    m_repairKeyring = slotEmpty && !keyringGone;
    if (keyringGone)
        addIssue(QStringLiteral("keyring_missing"), me, QString());
    else if (m_repairKeyring)
        addIssue(QStringLiteral("keyring_restored"), me, QString());

    if (!chosen) {
        const bool fresh = m->rootGeneration < 0 && m->rootDevice == me && m->lastSeq < 0;
        if (!fresh)
            return fail(failure, "drive_unreadable");
        m_rootGeneration = -1;
        m_rootHash.clear();
    } else {
        m_rootGeneration = chosen->generation;
        m_rootHash = chosen->hash;
        const QString writer = chosen->body.value(QStringLiteral("writer")).toString();
        const auto device = m_devices.constFind(writer);
        if (device == m_devices.constEnd() || device->removed
            || !verify(device->keys.ed25519, rootMessage(chosen->bodyBytes), chosen->signature))
            addIssue(QStringLiteral("untrusted_root"), writer, QString());
        if (qint64(chosen->body.value(QStringLiteral("pages")).toDouble()) > m_totalPages)
            addIssue(QStringLiteral("drive_truncated"), QString(), QString());
        if (chosen->generation < m->rootGeneration)
            addIssue(QStringLiteral("rolled_back"), QString(),
                     QStringLiteral("index %1, this device saw %2").arg(chosen->generation).arg(m->rootGeneration));
        else if (chosen->generation == m->rootGeneration && chosen->hash != m->rootHash)
            addIssue(QStringLiteral("log_forked"), QString(), QString());
        if (chosen->generation > m->rootGeneration) {
            m->rootGeneration = chosen->generation;
            m->rootHash = chosen->hash;
            m->cachedRoot = toBase64Url(compact(envelope(chosen->bodyBytes, chosen->signature)));
        }
        Q_UNUSED(restored);
    }
    if (unreadable > 0)
        addIssue(QStringLiteral("unreadable_objects"), QString(), QString::number(unreadable));

    checkWitnesses(chains);
    const QMap<qint64, Record> own = chains.value(me);
    const qint64 ownMax = own.isEmpty() ? -1 : own.lastKey();
    if (ownMax < m->lastSeq)
        addIssue(QStringLiteral("own_records_missing"), me, QString());
    if (ownMax > m->lastSeq && m->lastSeq >= 0)
        addIssue(QStringLiteral("log_forked"), me, QString());
    if (m->lastSeq < 0 && ownMax >= 0) {
        m->lastSeq = ownMax;
        m->lastHash = own.last().hash;
    }
    for (const Record &record : std::as_const(m_records))
        m->lamport = std::max(m->lamport, record.lamport);
    for (auto it = chains.cbegin(); it != chains.cend(); ++it) {
        if (!m_devices.contains(it.key()) || it.value().isEmpty())
            continue;
        const Record &last = it.value().last();
        Head &head = m->heads[it.key()];
        if (last.seq > head.seq)
            head = Head{last.seq, last.hash};
    }

    const qint64 moment = now();
    for (const Invite &invite : std::as_const(m_invites)) {
        if (!invite.open(moment) || invite.mailbox < 0)
            continue;
        QByteArray plain;
        if (!openUnit(invite.requestKey, requestAad(invite.mailbox), file.readAt(mailboxOffset(invite.mailbox), kSlotBytes),
                      &plain))
            continue;
        QJsonObject outer, body;
        QByteArray bodyBytes, signature;
        JoinRequest request;
        request.mailbox = invite.mailbox;
        if (!parseObject(stripPadding(plain), &outer) || !openEnvelope(outer, &bodyBytes, &signature, &body)
            || !PublicKeys::fromJson(body.value(QStringLiteral("keys")).toObject(), &request.keys)
            || !verify(request.keys.ed25519, requestMessage(bodyBytes), signature))
            continue;
        request.inviteId = body.value(QStringLiteral("invite")).toString();
        request.device = body.value(QStringLiteral("device")).toString();
        request.name = body.value(QStringLiteral("name")).toString().left(64);
        request.time = qint64(body.value(QStringLiteral("time")).toDouble());
        if (request.inviteId != invite.id || m_devices.contains(request.device))
            continue;
        QByteArray inviteBytes;
        fromBase64Url(invite.id, &inviteBytes);
        request.safety = safetyNumber(inviteBytes, request.keys);
        m_requests.append(request);
    }

    for (const FileEntry &entry : std::as_const(m_files)) {
        if (entry.deleted || !entry.current())
            continue;
        for (const Run &run : entry.current()->pages)
            if (run.start + run.count > m_totalPages) {
                addIssue(QStringLiteral("missing_content"), QString(), entry.name);
                break;
            }
    }

    if (!m_driveLabel.isEmpty())
        m->label = m_driveLabel;
    const auto self = m_devices.constFind(me);
    m_removed = (self != m_devices.constEnd() && self->removed) || keyringGone;
    for (const DeviceInfo &device : std::as_const(m_devices))
        if (device.removed && !m->removedDevices.contains(device.id))
            m->removedDevices.append(device.id);
    if (m->toJson() != stateBefore && !m_state->save(failure))
        return false;
    m_scanned = true;
    return true;
}

void Drive::addIssue(const QString &code, const QString &device, const QString &detail)
{
    const bool perFile = code == QLatin1String("missing_content") || code == QLatin1String("content_damaged");
    for (const Issue &issue : std::as_const(m_issues))
        if (issue.code == code && issue.device == device && (!perFile || issue.detail == detail))
            return;
    m_issues.append({code, device, detail});
}

void Drive::checkWitnesses(const QMap<QString, QMap<qint64, Record>> &chains)
{
    const Membership *m = membership();
    const QMap<qint64, Record> empty;
    for (const Record &record : std::as_const(m_records)) {
        for (auto it = record.witness.constBegin(); it != record.witness.constEnd(); ++it) {
            const QJsonArray pair = it.value().toArray();
            const qint64 seq = qint64(pair.at(0).toDouble(-1));
            const QString hash = pair.at(1).toString();
            if (seq < 0 || it.key() == m_state->deviceId)
                continue;
            const auto chainIt = chains.constFind(it.key());
            const QMap<qint64, Record> &chain = chainIt == chains.constEnd() ? empty : chainIt.value();
            const auto found = chain.constFind(seq);
            if (found == chain.constEnd())
                addIssue(QStringLiteral("records_missing"), it.key(),
                         QStringLiteral("%1 saw record %2").arg(deviceName(record.device)).arg(seq + 1));
            else if (found->hash != hash)
                addIssue(QStringLiteral("log_forked"), it.key(), QString());
        }
    }
    for (auto it = m->heads.cbegin(); it != m->heads.cend(); ++it) {
        if (it.key() == m_state->deviceId || it->seq < 0)
            continue;
        const auto chainIt = chains.constFind(it.key());
        const QMap<qint64, Record> &chain = chainIt == chains.constEnd() ? empty : chainIt.value();
        const auto found = chain.constFind(it->seq);
        if (found == chain.constEnd())
            addIssue(QStringLiteral("rolled_back"), it.key(),
                     QStringLiteral("this device saw record %1").arg(it->seq + 1));
        else if (found->hash != it->hash)
            addIssue(QStringLiteral("log_forked"), it.key(), QString());
    }
}

void Drive::fold(const QList<Record> &input)
{
    QList<Record> records = input;
    std::sort(records.begin(), records.end(), [](const Record &a, const Record &b) {
        if (a.lamport != b.lamport)
            return a.lamport < b.lamport;
        if (a.device != b.device)
            return a.device < b.device;
        return a.seq < b.seq;
    });
    const Membership *m = membership();
    m_records.clear();
    m_driveLabel.clear();
    m_devices.clear();
    m_files.clear();
    m_invites.clear();
    QMap<QString, qint64> cutoff;
    for (const Record &r : std::as_const(records)) {
        if (r.action != QLatin1String("device_removed"))
            continue;
        const QString id = r.data.value(QStringLiteral("device")).toString();
        const qint64 upTo = qint64(r.data.value(QStringLiteral("upTo")).toDouble(-1));
        cutoff.insert(id, cutoff.contains(id) ? std::min(cutoff.value(id), upTo) : upTo);
    }
    for (const Record &r : std::as_const(records)) {
        const QJsonObject &x = r.data;
        if (cutoff.contains(r.device) && r.seq > cutoff.value(r.device)) {
            addIssue(QStringLiteral("after_removal"), r.device, QString());
            continue;
        }
        if (r.action == QLatin1String("drive_created")) {
            if (r.device != m->rootDevice || r.seq != 0 || !m_devices.isEmpty())
                continue;
            const QJsonObject device = x.value(QStringLiteral("device")).toObject();
            DeviceInfo info;
            info.id = r.device;
            info.name = device.value(QStringLiteral("name")).toString().left(64);
            PublicKeys::fromJson(device.value(QStringLiteral("keys")).toObject(), &info.keys);
            info.addedBy = r.device;
            info.addedAt = r.time;
            info.mailbox = x.value(QStringLiteral("mailbox")).toInt(-1);
            info.root = true;
            m_devices.insert(info.id, info);
            m_driveLabel = x.value(QStringLiteral("label")).toString().left(80);
            m_records.append(r);
            continue;
        }
        if (!m_devices.contains(r.device))
            continue;
        m_records.append(r);
        const QString fileId = x.value(QStringLiteral("file")).toString();
        const auto version = [&r, &x]() {
            FileVersion v;
            v.blob = x.value(QStringLiteral("blob")).toString();
            fromBase64Url(x.value(QStringLiteral("key")).toString(), &v.key);
            v.pages = runsFromJson(x.value(QStringLiteral("pages")).toArray());
            v.size = qint64(x.value(QStringLiteral("size")).toDouble());
            v.sha256 = x.value(QStringLiteral("sha256")).toString();
            v.time = r.time;
            v.device = r.device;
            return v;
        };
        const QString &a = r.action;
        if (a == QLatin1String("device_added")) {
            const QString id = x.value(QStringLiteral("device")).toString();
            if (id.isEmpty() || m_devices.contains(id))
                continue;
            DeviceInfo info;
            info.id = id;
            info.name = x.value(QStringLiteral("name")).toString().left(64);
            PublicKeys::fromJson(x.value(QStringLiteral("keys")).toObject(), &info.keys);
            info.addedBy = r.device;
            info.addedAt = r.time;
            info.mailbox = x.value(QStringLiteral("mailbox")).toInt(-1);
            m_devices.insert(id, info);
        } else if (a == QLatin1String("device_removed")) {
            auto target = m_devices.find(x.value(QStringLiteral("device")).toString());
            if (target != m_devices.end() && !target->removed) {
                target->removed = true;
                target->removedBy = r.device;
                target->removedAt = r.time;
                target->removedSeq = qint64(x.value(QStringLiteral("upTo")).toDouble(-1));
            }
        } else if (a == QLatin1String("invite_created")) {
            Invite invite;
            invite.id = x.value(QStringLiteral("invite")).toString();
            fromBase64Url(x.value(QStringLiteral("requestKey")).toString(), &invite.requestKey);
            fromBase64Url(x.value(QStringLiteral("approveKey")).toString(), &invite.approveKey);
            invite.mailbox = x.value(QStringLiteral("mailbox")).toInt(-1);
            invite.created = r.time;
            invite.expires = qint64(x.value(QStringLiteral("expires")).toDouble());
            invite.createdBy = r.device;
            if (!invite.id.isEmpty() && !m_invites.contains(invite.id))
                m_invites.insert(invite.id, invite);
        } else if (a == QLatin1String("invite_used") || a == QLatin1String("invite_revoked")) {
            auto invite = m_invites.find(x.value(QStringLiteral("invite")).toString());
            if (invite != m_invites.end())
                (a == QLatin1String("invite_used") ? invite->used : invite->revoked) = true;
        } else if (a == QLatin1String("file_created")) {
            if (fileId.isEmpty() || m_files.contains(fileId))
                continue;
            FileEntry entry;
            entry.id = fileId;
            entry.name = x.value(QStringLiteral("name")).toString();
            entry.kind = x.value(QStringLiteral("kind")).toString();
            entry.versions.append(version());
            entry.created = entry.modified = r.time;
            entry.createdBy = entry.modifiedBy = r.device;
            m_files.insert(fileId, entry);
        } else if (a == QLatin1String("file_saved") || a == QLatin1String("file_restored")) {
            auto entry = m_files.find(fileId);
            if (entry == m_files.end() || entry->deleted)
                continue;
            if (a == QLatin1String("file_saved") && entry->current()
                && x.value(QStringLiteral("base")).toString() != entry->current()->blob)
                ++entry->conflicts;
            entry->versions.append(version());
            entry->modified = r.time;
            entry->modifiedBy = r.device;
        } else if (a == QLatin1String("file_renamed")) {
            auto entry = m_files.find(fileId);
            if (entry != m_files.end() && !entry->deleted)
                entry->name = x.value(QStringLiteral("name")).toString();
        } else if (a == QLatin1String("file_deleted")) {
            auto entry = m_files.find(fileId);
            if (entry != m_files.end())
                entry->deleted = true;
        } else if (a == QLatin1String("file_rekeyed")) {
            auto entry = m_files.find(fileId);
            if (entry == m_files.end())
                continue;
            for (const QJsonValue &value : x.value(QStringLiteral("versions")).toArray()) {
                const QJsonObject change = value.toObject();
                const QString old = change.value(QStringLiteral("old")).toString();
                for (FileVersion &v : entry->versions) {
                    if (v.blob != old)
                        continue;
                    v.blob = change.value(QStringLiteral("blob")).toString();
                    fromBase64Url(change.value(QStringLiteral("key")).toString(), &v.key);
                    v.pages = runsFromJson(change.value(QStringLiteral("pages")).toArray());
                }
            }
        } else if (a == QLatin1String("file_opened")) {
            auto entry = m_files.find(fileId);
            if (entry != m_files.end()) {
                ++entry->opens;
                entry->lastOpened = r.time;
                entry->lastOpenedBy = r.device;
            }
        }
    }
}

QList<FileEntry> Drive::files() const
{
    QList<FileEntry> out;
    for (const FileEntry &entry : m_files)
        if (!entry.deleted)
            out.append(entry);
    std::sort(out.begin(), out.end(), [](const FileEntry &a, const FileEntry &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

const FileEntry *Drive::file(const QString &id) const
{
    const auto found = m_files.constFind(id);
    return found == m_files.constEnd() || found->deleted ? nullptr : &found.value();
}

QList<DeviceInfo> Drive::devices() const { return m_devices.values(); }

QList<Invite> Drive::openInvites() const
{
    QList<Invite> out;
    const qint64 moment = now();
    for (const Invite &invite : m_invites)
        if (invite.open(moment))
            out.append(invite);
    return out;
}

QString Drive::deviceName(const QString &id) const
{
    if (m_state && id == m_state->deviceId)
        return m_state->deviceName + QStringLiteral(" (this device)");
    const auto found = m_devices.constFind(id);
    if (found != m_devices.constEnd() && !found->name.isEmpty())
        return found->name;
    return id.isEmpty() ? QStringLiteral("A device") : QStringLiteral("Device %1").arg(id.left(8));
}

QString Drive::deviceSafetyNumber(const QString &id) const
{
    const auto found = m_devices.constFind(id);
    if (found == m_devices.constEnd() || !found->keys.isValid())
        return {};
    return safetyNumber(QByteArrayLiteral("device"), found->keys);
}

QSet<int> Drive::usedMailboxes() const
{
    QSet<int> used;
    for (const DeviceInfo &device : m_devices)
        if (!device.removed && device.mailbox >= 0)
            used.insert(device.mailbox);
    const qint64 moment = now();
    for (const Invite &invite : m_invites)
        if (invite.open(moment) && invite.mailbox >= 0)
            used.insert(invite.mailbox);
    return used;
}

QList<Run> Drive::dataRunsInUse() const
{
    QList<Run> runs;
    for (const QList<Run> &log : m_logs)
        runs += log;
    for (const FileEntry &entry : m_files)
        if (!entry.deleted)
            for (const FileVersion &version : entry.versions)
                runs += version.pages;
    return runs;
}

QList<int> Drive::mailboxesInUse() const
{
    QList<int> out = usedMailboxes().values();
    std::sort(out.begin(), out.end());
    return out;
}

QList<qint64> Drive::freePages(const Txn &txn) const
{
    const qint64 total = txn.file ? txn.file->pages() : m_totalPages;
    QVector<bool> used(int(std::max<qint64>(total, 0)), false);
    for (qint64 page = 0; page < std::min(total, kDataPage); ++page)
        used[int(page)] = true;
    const auto mark = [&used, total](const QList<Run> &runs) {
        for (const Run &run : runs)
            for (qint64 i = 0; i < run.count; ++i)
                if (run.start + i < total)
                    used[int(run.start + i)] = true;
    };
    for (const QList<Run> &runs : m_logs)
        mark(runs);
    for (const FileEntry &entry : m_files)
        if (!entry.deleted)
            for (const FileVersion &version : entry.versions)
                mark(version.pages);
    for (const qint64 page : txn.reserved)
        if (page < total)
            used[int(page)] = true;
    QList<qint64> free;
    for (qint64 page = kDataPage; page < total; ++page)
        if (!used[int(page)])
            free.append(page);
    return free;
}

qint64 Drive::usedBytes() const
{
    qint64 pages = kDataPage;
    for (const QList<Run> &runs : m_logs)
        pages += runPages(runs);
    for (const FileEntry &entry : m_files)
        if (!entry.deleted) {
            QSet<qint64> seen;
            for (const FileVersion &version : entry.versions)
                for (const Run &run : version.pages)
                    if (!seen.contains(run.start)) {
                        seen.insert(run.start);
                        pages += run.count;
                    }
        }
    return pages * kPageBytes;
}

bool Drive::allocate(Txn &txn, qint64 count, bool contiguous, QList<Run> *runs, Failure *failure)
{
    QList<qint64> free = freePages(txn);
    const auto findRun = [&free, count]() -> qint64 {
        qint64 length = 0;
        for (int i = 0; i < free.size(); ++i) {
            length = (i > 0 && free.at(i) == free.at(i - 1) + 1) ? length + 1 : 1;
            if (length >= count)
                return free.at(i) - count + 1;
        }
        return -1;
    };
    qint64 start = contiguous ? findRun() : 0;
    if ((contiguous && start < 0) || (!contiguous && free.size() < count)) {
        const qint64 total = txn.file->pages();
        const qint64 shortfall = contiguous ? count : count - free.size();
        const qint64 grow = std::max({shortfall, kInitialDataPages, total / 8});
        if (!txn.file->appendRandom(grow))
            return fail(failure, "drive_write_failed");
        if (txn.renew)
            txn.renew();
        free = freePages(txn);
        start = contiguous ? findRun() : 0;
        if ((contiguous && start < 0) || (!contiguous && free.size() < count))
            return fail(failure, "drive_full");
    }
    runs->clear();
    QList<qint64> taken;
    if (contiguous)
        for (qint64 i = 0; i < count; ++i)
            taken.append(start + i);
    else
        taken = free.mid(0, int(count));
    for (const qint64 page : std::as_const(taken)) {
        txn.reserved.insert(page);
        if (!runs->isEmpty() && runs->last().start + runs->last().count == page)
            ++runs->last().count;
        else
            runs->append({page, 1});
    }
    return true;
}

bool Drive::acquireLease(ContainerFile &file, Failure *failure)
{
    const Membership *m = membership();
    const EpochKey *epoch = m->currentEpoch();
    if (!epoch)
        return fail(failure, "no_drive_key");
    QByteArray observed;
    QElapsedTimer seen, total;
    total.start();
    for (;;) {
        const QByteArray bytes = file.readAt(kLeasePage * kPageBytes, kPageBytes);
        QString holder;
        for (const EpochKey &key : m->epochs) {
            QByteArray plain;
            QJsonObject lease;
            if (openUnit(derive(key.key, "lease"), label("lease"), bytes, &plain)
                && parseObject(stripPadding(plain), &lease)) {
                holder = lease.value(QStringLiteral("device")).toString();
                break;
            }
        }
        const auto holderInfo = m_devices.constFind(holder);
        bool free = holder.isEmpty() || holder == m_state->deviceId
                    || (holderInfo != m_devices.constEnd() && holderInfo->removed);
        if (!free) {
            if (bytes != observed) {
                observed = bytes;
                seen.start();
            } else if (seen.elapsed() >= g_timing.staleMs) {
                free = true;
            }
        }
        if (free) {
            const QByteArray mine = sealUnit(derive(epoch->key, "lease"), label("lease"),
                                             compact({{QStringLiteral("device"), m_state->deviceId},
                                                      {QStringLiteral("token"), randomId()}}),
                                             kPageBytes);
            if (mine.isEmpty() || !file.writeAt(kLeasePage * kPageBytes, mine) || !file.sync())
                return fail(failure, "drive_write_failed");
            if (g_timing.settleMs > 0)
                QThread::msleep(ulong(g_timing.settleMs));
            if (file.readAt(kLeasePage * kPageBytes, kPageBytes) == mine) {
                m_leaseBytes = mine;
                return true;
            }
            observed.clear();
        }
        if (total.elapsed() > qint64(g_timing.staleMs) * 2 + 10000)
            return fail(failure, "drive_busy");
        QThread::msleep(ulong(std::max(1, g_timing.pollMs)));
    }
}

bool Drive::renewLease(ContainerFile &file)
{
    const EpochKey *epoch = membership()->currentEpoch();
    if (!epoch || file.readAt(kLeasePage * kPageBytes, kPageBytes) != m_leaseBytes)
        return false;
    const QByteArray mine = sealUnit(derive(epoch->key, "lease"), label("lease"),
                                     compact({{QStringLiteral("device"), m_state->deviceId},
                                              {QStringLiteral("token"), randomId()}}),
                                     kPageBytes);
    if (mine.isEmpty() || !file.writeAt(kLeasePage * kPageBytes, mine) || !file.sync())
        return false;
    m_leaseBytes = mine;
    return true;
}

void Drive::releaseLease(ContainerFile &file)
{
    if (!m_leaseBytes.isEmpty() && file.readAt(kLeasePage * kPageBytes, kPageBytes) == m_leaseBytes) {
        file.writeAt(kLeasePage * kPageBytes, randomBytes(int(kPageBytes)));
        file.sync();
    }
    m_leaseBytes.clear();
}

bool Drive::transact(const std::function<bool(Txn &, Failure *)> &body, Failure *failure,
                     const std::function<void()> &)
{
    Membership *m = mutableMembership();
    if (!m || !m->isMember())
        return fail(failure, "unknown_drive");
    ContainerFile file(m->container);
    if (!file.openWrite())
        return fail(failure, "container_unavailable");
    if (!load(file, failure))
        return false;
    if (m_removed)
        return fail(failure, "device_removed");
    if (!acquireLease(file, failure))
        return false;
    bool ok = load(file, failure);
    m = mutableMembership();
    const Membership before = *m;
    Txn txn;
    txn.file = &file;
    QElapsedTimer sinceRenew;
    sinceRenew.start();
    txn.renew = [this, &file, &sinceRenew]() {
        if (sinceRenew.elapsed() > std::max(1000, g_timing.staleMs / 8)) {
            renewLease(file);
            sinceRenew.restart();
        }
    };
    ok = ok && !m_removed && body(txn, failure) && commit(txn, failure);
    if (!ok) {
        if (Membership *current = mutableMembership())
            *current = before;
    }
    releaseLease(file);
    if (ok) {
        Failure ignored;
        load(file, &ignored);
    }
    return ok;
}

bool Drive::commit(Txn &txn, Failure *failure)
{
    Membership *m = mutableMembership();
    const EpochKey *epoch = m->currentEpoch();
    if (!epoch)
        return fail(failure, "no_drive_key");
    const QString me = m_state->deviceId;
    QList<Run> mine = m_logs.value(me);
    bool reuse = !m_myLastRecords.isEmpty() && !mine.isEmpty();
    QJsonArray added;
    for (const auto &entry : std::as_const(txn.records)) {
        QJsonObject witness;
        for (auto it = m->heads.cbegin(); it != m->heads.cend(); ++it)
            if (it.key() != me && it->seq >= 0)
                witness.insert(it.key(), QJsonArray{double(it->seq), it->hash});
        const QByteArray bodyBytes = compact({{QStringLiteral("d"), me},
                                              {QStringLiteral("s"), double(m->lastSeq + 1)},
                                              {QStringLiteral("l"), double(m->lamport + 1)},
                                              {QStringLiteral("t"), double(now())},
                                              {QStringLiteral("a"), entry.first},
                                              {QStringLiteral("x"), entry.second},
                                              {QStringLiteral("p"), m->lastHash},
                                              {QStringLiteral("w"), witness}});
        QByteArray signature;
        if (!sign(m_state->keys.ed25519, recordMessage(m->driveId, bodyBytes), &signature))
            return fail(failure, "signing_failed");
        const QString hash = toBase64Url(sha256(recordMessage(m->driveId, bodyBytes) + signature));
        m->lastSeq += 1;
        m->lastHash = hash;
        m->lamport += 1;
        m->heads[me] = Head{m->lastSeq, hash};
        added.append(envelope(bodyBytes, signature));
    }
    const auto payloadFor = [&me](const QJsonArray &records) {
        return compact({{QStringLiteral("device"), me}, {QStringLiteral("records"), records}});
    };
    QJsonArray records = reuse ? m_myLastRecords : QJsonArray();
    for (const QJsonValue &value : std::as_const(added))
        records.append(value);
    QByteArray payload = payloadFor(records);
    if (reuse && (payload.size() + kUnitOverhead + kPageBytes - 1) / kPageBytes > kLogUnitLimitPages) {
        reuse = false;
        payload = payloadFor(added);
    }
    const qint64 pages = std::max<qint64>(1, (payload.size() + kUnitOverhead + kPageBytes - 1) / kPageBytes);
    QList<Run> unitRuns;
    if (!allocate(txn, pages, true, &unitRuns, failure))
        return false;
    const Run unit = unitRuns.first();
    const QByteArray sealed = sealUnit(derive(epoch->key, "log"), logAad(m->driveId, unit.start), payload,
                                       unit.count * kPageBytes);
    if (sealed.isEmpty() || !txn.file->writeAt(unit.start * kPageBytes, sealed))
        return fail(failure, "drive_write_failed");
    if (reuse)
        txn.freed.append(mine.takeLast());
    mine.append(unit);
    QMap<QString, QList<Run>> logs = m_logs;
    logs.insert(me, mine);
    QJsonObject logsJson;
    for (auto it = logs.cbegin(); it != logs.cend(); ++it)
        logsJson.insert(it.key(), runsToJson(it.value()));
    const qint64 generation = m_rootGeneration + 1;
    const QByteArray rootBody = compact({{QStringLiteral("v"), 2},
                                         {QStringLiteral("drive"), m->driveId},
                                         {QStringLiteral("gen"), double(generation)},
                                         {QStringLiteral("pages"), double(txn.file->pages())},
                                         {QStringLiteral("logs"), logsJson},
                                         {QStringLiteral("writer"), me},
                                         {QStringLiteral("time"), double(now())},
                                         {QStringLiteral("prev"), m_rootHash}});
    QByteArray rootSignature;
    if (!sign(m_state->keys.ed25519, rootMessage(rootBody), &rootSignature))
        return fail(failure, "signing_failed");
    const bool slotA = generation % 2 == 0;
    const QByteArray rootUnit = sealUnit(derive(epoch->key, "root"), rootAad(slotA),
                                         compact(envelope(rootBody, rootSignature)), kRootPages * kPageBytes);
    if (rootUnit.isEmpty())
        return fail(failure, "drive_index_full");
    if (!txn.file->sync())
        return fail(failure, "drive_write_failed");
    if (txn.file->readAt(kLeasePage * kPageBytes, kPageBytes) != m_leaseBytes)
        return fail(failure, "drive_busy");
    if (txn.file->readAt(kRootAPage * kPageBytes, kRootPages * kPageBytes) != m_rootABytes
        || txn.file->readAt(kRootBPage * kPageBytes, kRootPages * kPageBytes) != m_rootBBytes)
        return fail(failure, "drive_changed");
    const qint64 rootOffset = (slotA ? kRootAPage : kRootBPage) * kPageBytes;
    if (!txn.file->writeAt(rootOffset, rootUnit) || !txn.file->sync()
        || txn.file->readAt(rootOffset, kRootPages * kPageBytes) != rootUnit)
        return fail(failure, "drive_write_failed");
    if (txn.bothRoots) {
        const QByteArray twin = sealUnit(derive(epoch->key, "root"), rootAad(!slotA),
                                         compact(envelope(rootBody, rootSignature)), kRootPages * kPageBytes);
        const qint64 twinOffset = (slotA ? kRootBPage : kRootAPage) * kPageBytes;
        if (twin.isEmpty() || !txn.file->writeAt(twinOffset, twin) || !txn.file->sync())
            return fail(failure, "drive_write_failed");
    }
    if (m_repairKeyring && m->mailbox >= 0) {
        DeviceInfo self;
        self.id = me;
        self.keys = m_state->publicKeys;
        Failure ignored;
        const QByteArray unit = keyringUnit(self, m->mailbox, {}, {}, &ignored);
        ContainerFile *file = txn.file;
        const int mailbox = m->mailbox;
        if (!unit.isEmpty())
            txn.afterCommit.append([file, mailbox, unit] {
                return file->writeAt(mailboxOffset(mailbox), unit) && file->sync();
            });
    }
    m->rootGeneration = generation;
    m->rootHash = toBase64Url(sha256(rootMessage(rootBody) + rootSignature));
    m->cachedRoot = toBase64Url(compact(envelope(rootBody, rootSignature)));
    bool after = true;
    for (const auto &action : std::as_const(txn.afterCommit))
        after = action() && after;
    for (const Run &run : std::as_const(txn.freed))
        txn.file->writeAt(run.start * kPageBytes, randomBytes(int(run.count * kPageBytes)));
    txn.file->sync();
    if (!m_state->save(failure))
        return false;
    return after ? true : fail(failure, "drive_write_failed");
}

bool Drive::note(const QString &action, const QJsonObject &data, Failure *failure)
{
    return transact(
        [&](Txn &txn, Failure *) {
            txn.records.append({action, data});
            return true;
        },
        failure);
}

bool Drive::createInvite(QString *code, QString *inviteId, Failure *failure)
{
    QString text, id;
    const bool ok = transact(
        [&](Txn &txn, Failure *f) {
            const QSet<int> used = usedMailboxes();
            if (used.size() >= kMailboxSlots)
                return fail(f, "drive_full");
            QByteArray secret;
            int mailbox = -1;
            for (int tries = 0; tries < 4096; ++tries) {
                text = newInviteCode(&secret);
                mailbox = mailboxForInvite(secret);
                if (!used.contains(mailbox))
                    break;
                mailbox = -1;
            }
            if (mailbox < 0)
                return fail(f, "drive_full");
            id = toBase64Url(inviteIdBytes(secret));
            const QByteArray requestKey = derive(secret, "request");
            const QByteArray approveKey = derive(secret, "approve");
            wipe(secret);
            txn.records.append({QStringLiteral("invite_created"),
                                {{QStringLiteral("invite"), id},
                                 {QStringLiteral("requestKey"), toBase64Url(requestKey)},
                                 {QStringLiteral("approveKey"), toBase64Url(approveKey)},
                                 {QStringLiteral("mailbox"), mailbox},
                                 {QStringLiteral("expires"), double(now() + g_inviteLifetime)}}});
            ContainerFile *file = txn.file;
            txn.afterCommit.append([file, mailbox] {
                return file->writeAt(mailboxOffset(mailbox), randomBytes(int(kSlotBytes))) && file->sync();
            });
            return true;
        },
        failure);
    if (!ok)
        return false;
    *code = text;
    if (inviteId)
        *inviteId = id;
    return true;
}

bool Drive::revokeInvite(const QString &inviteId, Failure *failure)
{
    if (!m_invites.contains(inviteId))
        return fail(failure, "unknown_invite");
    const int mailbox = m_invites.value(inviteId).mailbox;
    return transact(
        [&](Txn &txn, Failure *) {
            txn.records.append({QStringLiteral("invite_revoked"), {{QStringLiteral("invite"), inviteId}}});
            ContainerFile *file = txn.file;
            if (mailbox >= 0 && !usedMailboxes().contains(mailbox))
                txn.afterCommit.append([file, mailbox] {
                    return file->writeAt(mailboxOffset(mailbox), randomBytes(int(kSlotBytes))) && file->sync();
                });
            return true;
        },
        failure);
}

QByteArray Drive::keyringUnit(const DeviceInfo &recipient, int mailbox, const QByteArray &approveKey,
                              const QString &inviteId, Failure *failure) const
{
    const Membership *m = membership();
    QJsonObject body{{QStringLiteral("suite"), QLatin1String(kSuite)},
                     {QStringLiteral("drive"), m->driveId},
                     {QStringLiteral("device"), recipient.id},
                     {QStringLiteral("mailbox"), mailbox},
                     {QStringLiteral("epochs"), epochsJson(m->epochs)},
                     {QStringLiteral("root"),
                      QJsonObject{{QStringLiteral("device"), m->rootDevice},
                                  {QStringLiteral("ed25519"), toBase64Url(m->rootEd25519)}}},
                     {QStringLiteral("issuer"), m_state->deviceId},
                     {QStringLiteral("issuerKeys"), m_state->publicKeys.toJson()},
                     {QStringLiteral("time"), double(now())}};
    if (!inviteId.isEmpty())
        body.insert(QStringLiteral("invite"), inviteId);
    const QByteArray bodyBytes = compact(body);
    QByteArray signature;
    if (!sign(m_state->keys.ed25519, keyringMessage(bodyBytes), &signature)) {
        fail(failure, "signing_failed");
        return {};
    }
    QJsonObject outer = envelope(bodyBytes, signature);
    if (!approveKey.isEmpty())
        outer.insert(QStringLiteral("m"), toBase64Url(hmac(approveKey, bodyBytes)));
    QByteArray plain = compact(outer);
    const qint64 capacity = kSlotBytes - 32 - kMlkemCiphertextBytes - kUnitOverhead;
    if (plain.size() > capacity) {
        fail(failure, "keyring_too_large");
        return {};
    }
    plain.append(QByteArray(int(capacity - plain.size()), '\0'));
    QByteArray sealed;
    const bool ok = sealToDevice(recipient.keys, mailboxLabel(mailbox), plain, &sealed, failure);
    wipe(plain);
    return ok && sealed.size() == kSlotBytes ? sealed : QByteArray();
}

bool Drive::approve(int mailbox, Failure *failure)
{
    const auto found = std::find_if(m_requests.cbegin(), m_requests.cend(),
                                    [mailbox](const JoinRequest &r) { return r.mailbox == mailbox; });
    if (found == m_requests.cend())
        return fail(failure, "unknown_request");
    const JoinRequest request = *found;
    return transact(
        [&](Txn &txn, Failure *f) {
            const auto invite = m_invites.constFind(request.inviteId);
            if (invite == m_invites.constEnd() || !invite->open(now()))
                return fail(f, "invite_expired");
            DeviceInfo device;
            device.id = request.device;
            device.name = request.name;
            device.keys = request.keys;
            const QByteArray unit = keyringUnit(device, mailbox, invite->approveKey, invite->id, f);
            if (unit.isEmpty())
                return false;
            txn.records.append({QStringLiteral("device_added"),
                                {{QStringLiteral("device"), device.id},
                                 {QStringLiteral("name"), device.name},
                                 {QStringLiteral("keys"), device.keys.toJson()},
                                 {QStringLiteral("invite"), request.inviteId},
                                 {QStringLiteral("mailbox"), mailbox}}});
            txn.records.append({QStringLiteral("invite_used"),
                                {{QStringLiteral("invite"), request.inviteId},
                                 {QStringLiteral("device"), device.id},
                                 {QStringLiteral("name"), device.name}}});
            ContainerFile *file = txn.file;
            txn.afterCommit.append([file, mailbox, unit] {
                return file->writeAt(mailboxOffset(mailbox), unit) && file->sync()
                       && file->readAt(mailboxOffset(mailbox), kSlotBytes) == unit;
            });
            return true;
        },
        failure);
}

bool Drive::deny(int mailbox, Failure *failure)
{
    const auto found = std::find_if(m_requests.cbegin(), m_requests.cend(),
                                    [mailbox](const JoinRequest &r) { return r.mailbox == mailbox; });
    if (found == m_requests.cend())
        return fail(failure, "unknown_request");
    const JoinRequest request = *found;
    return transact(
        [&](Txn &txn, Failure *) {
            txn.records.append({QStringLiteral("join_denied"),
                                {{QStringLiteral("invite"), request.inviteId},
                                 {QStringLiteral("device"), request.device},
                                 {QStringLiteral("name"), request.name}}});
            txn.records.append({QStringLiteral("invite_revoked"), {{QStringLiteral("invite"), request.inviteId}}});
            ContainerFile *file = txn.file;
            txn.afterCommit.append([file, mailbox] {
                return file->writeAt(mailboxOffset(mailbox), randomBytes(int(kSlotBytes))) && file->sync();
            });
            return true;
        },
        failure);
}

bool Drive::writeBlob(Txn &txn, QIODevice *source, FileVersion *version, Failure *failure)
{
    const Membership *m = membership();
    if (!source || !source->isOpen() || source->isSequential())
        return fail(failure, "read_failed");
    const qint64 length = source->size() - source->pos();
    const qint64 chunks = chunkCount(length);
    QList<Run> runs;
    if (!allocate(txn, chunks, false, &runs, failure))
        return false;
    const QList<qint64> pages = expand(runs);
    const QByteArray key = randomBytes(kKeyBytes);
    const QString blob = randomId();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = length;
    for (qint64 i = 0; i < chunks; ++i) {
        const qint64 take = std::min(kChunkBytes, std::max<qint64>(remaining, 0));
        QByteArray plain = take > 0 ? source->read(take) : QByteArray();
        if (plain.size() != take)
            return fail(failure, "read_failed");
        hash.addData(plain);
        remaining -= take;
        plain.append(QByteArray(int(kChunkBytes - plain.size()), '\0'));
        const qint64 page = pages.at(int(i));
        const QByteArray unit = sealUnit(key, blobAad(m->driveId, blob, i, i == chunks - 1, page), plain, kPageBytes);
        wipe(plain);
        if (unit.isEmpty() || !txn.file->writeAt(page * kPageBytes, unit))
            return fail(failure, "drive_write_failed");
        if (txn.renew && i % 256 == 255)
            txn.renew();
    }
    version->blob = blob;
    version->key = key;
    version->pages = runs;
    version->size = length;
    version->sha256 = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool Drive::readBlob(ContainerFile &file, const FileVersion &version,
                     const std::function<bool(const QByteArray &)> &sink, Failure *failure) const
{
    const Membership *m = membership();
    const QList<qint64> pages = expand(version.pages);
    const qint64 chunks = chunkCount(version.size);
    if (pages.size() != chunks)
        return fail(failure, "content_damaged");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = version.size;
    for (qint64 i = 0; i < chunks; ++i) {
        const qint64 page = pages.at(int(i));
        QByteArray plain;
        if (!openUnit(version.key, blobAad(m->driveId, version.blob, i, i == chunks - 1, page),
                      file.readAt(page * kPageBytes, kPageBytes), &plain))
            return fail(failure, "content_damaged");
        const qint64 take = std::min<qint64>(remaining, plain.size());
        if (take > 0) {
            const QByteArray part = plain.left(int(take));
            hash.addData(part);
            if (!sink(part)) {
                wipe(plain);
                return fail(failure, "write_failed");
            }
        }
        wipe(plain);
        remaining -= take;
    }
    if (QString::fromLatin1(hash.result().toHex()) != version.sha256)
        return fail(failure, "content_damaged");
    return true;
}

bool Drive::addFile(const QString &name, QIODevice *source, QString *fileId, Failure *failure,
                    const QString &kind)
{
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty() || cleaned.size() > 255 || cleaned.contains(QLatin1Char('/'))
        || cleaned.contains(QLatin1Char('\\')))
        return fail(failure, "invalid_name");
    const QString id = randomId();
    const qint64 start = source ? source->pos() : 0;
    const bool ok = transact(
        [&](Txn &txn, Failure *f) {
            if (source && source->pos() != start && !source->seek(start))
                return fail(f, "read_failed");
            FileVersion version;
            if (!writeBlob(txn, source, &version, f))
                return false;
            txn.records.append({QStringLiteral("file_created"),
                                {{QStringLiteral("file"), id},
                                 {QStringLiteral("name"), cleaned},
                                 {QStringLiteral("kind"), kind.isEmpty() ? fileKindForName(cleaned) : kind},
                                 {QStringLiteral("blob"), version.blob},
                                 {QStringLiteral("pages"), runsToJson(version.pages)},
                                 {QStringLiteral("key"), toBase64Url(version.key)},
                                 {QStringLiteral("size"), double(version.size)},
                                 {QStringLiteral("sha256"), version.sha256}}});
            return true;
        },
        failure);
    if (ok && fileId)
        *fileId = id;
    return ok;
}

bool Drive::saveFile(const QString &fileId, QIODevice *source, Failure *failure, const QString &startedFrom)
{
    const qint64 start = source ? source->pos() : 0;
    return transact(
        [&](Txn &txn, Failure *f) {
            const FileEntry *entry = file(fileId);
            if (!entry || !entry->current())
                return fail(f, "unknown_file");
            const QString base = startedFrom.isEmpty() ? entry->current()->blob : startedFrom;
            const QString name = entry->name;
            if (source && source->pos() != start && !source->seek(start))
                return fail(f, "read_failed");
            FileVersion version;
            if (!writeBlob(txn, source, &version, f))
                return false;
            txn.records.append({QStringLiteral("file_saved"),
                                {{QStringLiteral("file"), fileId},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("blob"), version.blob},
                                 {QStringLiteral("pages"), runsToJson(version.pages)},
                                 {QStringLiteral("key"), toBase64Url(version.key)},
                                 {QStringLiteral("size"), double(version.size)},
                                 {QStringLiteral("sha256"), version.sha256},
                                 {QStringLiteral("base"), base}}});
            return true;
        },
        failure);
}

bool Drive::readFile(const QString &fileId, int versionIndex, QIODevice *out, const QString &purpose,
                     Failure *failure)
{
    const FileEntry *entry = file(fileId);
    if (!entry || entry->versions.isEmpty())
        return fail(failure, "unknown_file");
    const int index = versionIndex < 0 ? int(entry->versions.size()) - 1 : versionIndex;
    if (index >= entry->versions.size())
        return fail(failure, "unknown_version");
    const FileVersion version = entry->versions.at(index);
    const QString name = entry->name;
    ContainerFile container(membership()->container);
    if (!container.openRead())
        return fail(failure, "container_unavailable");
    Failure readFailure;
    if (!readBlob(container, version, [out](const QByteArray &part) { return out->write(part) == part.size(); },
                  &readFailure)) {
        if (readFailure.code == QLatin1String("content_damaged")) {
            Failure ignored;
            note(QStringLiteral("integrity_alert"),
                 {{QStringLiteral("code"), QStringLiteral("content_damaged")},
                  {QStringLiteral("file"), fileId},
                  {QStringLiteral("detail"), name}},
                 &ignored);
            addIssue(QStringLiteral("content_damaged"), QString(), name);
        }
        if (failure)
            *failure = readFailure;
        return false;
    }
    return note(QStringLiteral("file_opened"),
                {{QStringLiteral("file"), fileId},
                 {QStringLiteral("name"), name},
                 {QStringLiteral("version"), index},
                 {QStringLiteral("purpose"), purpose.isEmpty() ? QStringLiteral("view") : purpose}},
                failure);
}

bool Drive::renameFile(const QString &fileId, const QString &name, Failure *failure)
{
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty() || cleaned.size() > 255 || cleaned.contains(QLatin1Char('/'))
        || cleaned.contains(QLatin1Char('\\')))
        return fail(failure, "invalid_name");
    return transact(
        [&](Txn &txn, Failure *f) {
            const FileEntry *entry = file(fileId);
            if (!entry)
                return fail(f, "unknown_file");
            txn.records.append({QStringLiteral("file_renamed"),
                                {{QStringLiteral("file"), fileId},
                                 {QStringLiteral("name"), cleaned},
                                 {QStringLiteral("from"), entry->name}}});
            return true;
        },
        failure);
}

bool Drive::deleteFile(const QString &fileId, Failure *failure)
{
    return transact(
        [&](Txn &txn, Failure *f) {
            const FileEntry *entry = file(fileId);
            if (!entry)
                return fail(f, "unknown_file");
            QSet<qint64> seen;
            for (const FileVersion &version : entry->versions)
                for (const Run &run : version.pages)
                    if (!seen.contains(run.start)) {
                        seen.insert(run.start);
                        txn.freed.append(run);
                    }
            txn.records.append({QStringLiteral("file_deleted"),
                                {{QStringLiteral("file"), fileId}, {QStringLiteral("name"), entry->name}}});
            return true;
        },
        failure);
}

bool Drive::restoreVersion(const QString &fileId, int index, Failure *failure)
{
    return transact(
        [&](Txn &txn, Failure *f) {
            const FileEntry *entry = file(fileId);
            if (!entry)
                return fail(f, "unknown_file");
            if (index < 0 || index >= entry->versions.size())
                return fail(f, "unknown_version");
            const FileVersion version = entry->versions.at(index);
            txn.records.append({QStringLiteral("file_restored"),
                                {{QStringLiteral("file"), fileId},
                                 {QStringLiteral("name"), entry->name},
                                 {QStringLiteral("version"), index},
                                 {QStringLiteral("blob"), version.blob},
                                 {QStringLiteral("pages"), runsToJson(version.pages)},
                                 {QStringLiteral("key"), toBase64Url(version.key)},
                                 {QStringLiteral("size"), double(version.size)},
                                 {QStringLiteral("sha256"), version.sha256}}});
            return true;
        },
        failure);
}

bool Drive::removeDevice(const QString &deviceId, const std::function<void(int, int)> &progress,
                         Failure *failure)
{
    if (deviceId == m_state->deviceId)
        return fail(failure, "cannot_remove_self");
    const bool removed = transact(
        [&](Txn &txn, Failure *f) {
            Membership *m = mutableMembership();
            const auto target = m_devices.constFind(deviceId);
            if (target == m_devices.constEnd() || target->removed)
                return fail(f, "unknown_device");
            txn.bothRoots = true;
            EpochKey next;
            next.key = randomBytes(kKeyBytes);
            next.id = epochIdFor(next.key);
            next.number = m->currentEpoch() ? m->currentEpoch()->number + 1 : 1;
            m->epochs.append(next);
            ContainerFile *file = txn.file;
            for (const DeviceInfo &device : std::as_const(m_devices)) {
                if (device.removed || device.id == deviceId || device.id == m_state->deviceId || device.mailbox < 0)
                    continue;
                const QByteArray unit = keyringUnit(device, device.mailbox, {}, {}, f);
                if (unit.isEmpty())
                    return false;
                const int mailbox = device.mailbox;
                txn.afterCommit.append([file, mailbox, unit] {
                    return file->writeAt(mailboxOffset(mailbox), unit) && file->sync();
                });
            }
            if (target->mailbox >= 0) {
                const int mailbox = target->mailbox;
                txn.afterCommit.append([file, mailbox] {
                    return file->writeAt(mailboxOffset(mailbox), randomBytes(int(kSlotBytes))) && file->sync();
                });
            }
            txn.records.append({QStringLiteral("device_removed"),
                                {{QStringLiteral("device"), deviceId},
                                 {QStringLiteral("name"), target->name},
                                 {QStringLiteral("epoch"), next.number},
                                 {QStringLiteral("keyId"), next.id},
                                 {QStringLiteral("upTo"), double(m->heads.value(deviceId).seq)}}});
            return true;
        },
        failure);
    if (!removed)
        return false;
    if (files().isEmpty())
        return true;
    return transact(
        [&](Txn &txn, Failure *f) {
            const Membership *m = membership();
            const QList<FileEntry> current = files();
            int total = 0, done = 0;
            for (const FileEntry &entry : current)
                total += int(entry.versions.size());
            for (const FileEntry &entry : current) {
                QHash<QString, QJsonObject> moved;
                QJsonArray changes;
                for (const FileVersion &version : entry.versions) {
                    if (progress)
                        progress(++done, total);
                    if (moved.contains(version.blob))
                        continue;
                    QList<Run> runs;
                    if (!allocate(txn, runPages(version.pages), false, &runs, f))
                        return false;
                    const QList<qint64> target = expand(runs);
                    const QByteArray key = randomBytes(kKeyBytes);
                    const QString blob = randomId();
                    const qint64 chunks = chunkCount(version.size);
                    bool intact = expand(version.pages).size() == chunks;
                    QList<qint64> source = expand(version.pages);
                    for (qint64 i = 0; intact && i < chunks; ++i) {
                        QByteArray plain;
                        intact = openUnit(version.key, blobAad(m->driveId, version.blob, i, i == chunks - 1, source.at(int(i))),
                                          txn.file->readAt(source.at(int(i)) * kPageBytes, kPageBytes), &plain);
                        if (!intact)
                            break;
                        const QByteArray unit = sealUnit(key, blobAad(m->driveId, blob, i, i == chunks - 1, target.at(int(i))),
                                                         plain, kPageBytes);
                        wipe(plain);
                        if (unit.isEmpty() || !txn.file->writeAt(target.at(int(i)) * kPageBytes, unit))
                            return fail(f, "drive_write_failed");
                        if (txn.renew && i % 256 == 255)
                            txn.renew();
                    }
                    if (!intact) {
                        for (const qint64 page : target)
                            txn.reserved.remove(page);
                        addIssue(QStringLiteral("content_damaged"), QString(), entry.name);
                        continue;
                    }
                    const QJsonObject change{{QStringLiteral("old"), version.blob},
                                             {QStringLiteral("blob"), blob},
                                             {QStringLiteral("pages"), runsToJson(runs)},
                                             {QStringLiteral("key"), toBase64Url(key)}};
                    moved.insert(version.blob, change);
                    changes.append(change);
                    txn.freed.append(version.pages);
                    if (txn.renew)
                        txn.renew();
                }
                if (!changes.isEmpty())
                    txn.records.append({QStringLiteral("file_rekeyed"),
                                        {{QStringLiteral("file"), entry.id},
                                         {QStringLiteral("name"), entry.name},
                                         {QStringLiteral("versions"), changes}}});
            }
            if (txn.records.isEmpty())
                txn.records.append({QStringLiteral("drive_rekeyed"), {}});
            return true;
        },
        failure);
}

QString Drive::activityCsv() const
{
    QList<Record> sorted = m_records;
    std::sort(sorted.begin(), sorted.end(),
              [](const Record &a, const Record &b) { return a.time < b.time; });
    const auto name = [this](const QString &id) { return deviceName(id); };
    QStringList lines{QStringLiteral("time,device,activity,record,hash")};
    for (const Record &record : std::as_const(sorted))
        lines << QStringList{QDateTime::fromMSecsSinceEpoch(record.time).toUTC().toString(Qt::ISODateWithMs),
                             csvField(deviceName(record.device)), csvField(describeRecord(record, name)),
                             QString::number(record.seq + 1), record.hash}
                     .join(QLatin1Char(','));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

}  // namespace forkmesh::securedrive
