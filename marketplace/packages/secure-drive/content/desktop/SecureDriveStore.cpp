#include "SecureDriveStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>

namespace forkmesh::securedrive {
namespace {

constexpr char kKeyring = 1;
constexpr char kRequest = 2;
constexpr char kLog = 3;
constexpr char kBlob = 4;
constexpr qint64 kChunk = qint64(1) << 20;
constexpr int kSegmentLimit = 400;
constexpr qint64 kInviteLifetime = qint64(24) * 60 * 60 * 1000;
constexpr qint64 kSmallObjectLimit = qint64(64) << 20;

QByteArray header(char kind)
{
    QByteArray out("FMSD");
    out.append(char(1));
    out.append(kind);
    return out;
}

QByteArray label(const char *name)
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

QString newObjectName() { return randomId() + QStringLiteral(".fmsd"); }

bool validObjectName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}\\.fmsd$"));
    return pattern.match(name).hasMatch();
}

bool partialObjectName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}\\.fmsd\\..+$"));
    return pattern.match(name).hasMatch();
}

QString epochIdFor(const QByteArray &key)
{
    QByteArray id;
    hkdf(key, QStringLiteral("key-id"), &id, 8);
    return QString::fromLatin1(id.toHex());
}

QByteArray recordMessage(const QString &driveId, QJsonObject record)
{
    record.remove(QStringLiteral("sig"));
    return label("record\n") + driveId.toUtf8() + '\n' + compact(record);
}

QByteArray keyringMessage(QJsonObject payload)
{
    payload.remove(QStringLiteral("sig"));
    return label("keyring\n") + compact(payload);
}

QByteArray requestMessage(QJsonObject payload)
{
    payload.remove(QStringLiteral("sig"));
    return label("request\n") + compact(payload);
}

QByteArray inviteIdBytes(const QByteArray &secret)
{
    QByteArray id;
    hkdf(secret, QStringLiteral("invite-id"), &id, 12);
    return id;
}

QByteArray beBytes(quint64 value, int width)
{
    QByteArray out(width, '\0');
    for (int i = width - 1; i >= 0; --i) {
        out[i] = char(value & 0xff);
        value >>= 8;
    }
    return out;
}

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

Record recordFrom(const QJsonObject &json)
{
    Record record;
    record.device = json.value(QStringLiteral("d")).toString();
    record.seq = qint64(json.value(QStringLiteral("s")).toDouble(-1));
    record.lamport = qint64(json.value(QStringLiteral("l")).toDouble());
    record.time = qint64(json.value(QStringLiteral("t")).toDouble());
    record.action = json.value(QStringLiteral("a")).toString();
    record.data = json.value(QStringLiteral("x")).toObject();
    record.prev = json.value(QStringLiteral("p")).toString();
    record.witness = json.value(QStringLiteral("w")).toObject();
    record.signedJson = json;
    return record;
}

QString recordHash(const QString &driveId, const QJsonObject &json)
{
    QByteArray signature;
    fromBase64Url(json.value(QStringLiteral("sig")).toString(), &signature);
    return toBase64Url(sha256(recordMessage(driveId, json) + signature));
}

bool readSmall(const QString &path, QByteArray *bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kSmallObjectLimit)
        return false;
    *bytes = file.readAll();
    return true;
}

qint64 chunkCount(qint64 padded) { return (padded + kChunk - 1) / kChunk; }

QByteArray blobAad(const QString &driveId, const QString &object, qint64 index, bool last,
                   const QByteArray &head)
{
    return label("blob/") + driveId.toUtf8() + '/' + object.toUtf8() + beBytes(quint64(index), 8)
           + char(last ? 1 : 0) + head;
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

}  // namespace

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
    return {{QStringLiteral("slot"), slot},
            {QStringLiteral("folder"), folder},
            {QStringLiteral("label"), label},
            {QStringLiteral("drive"), driveId},
            {QStringLiteral("root"), rootDevice},
            {QStringLiteral("rootEd25519"), toBase64Url(rootEd25519)},
            {QStringLiteral("epochs"), epochsJson(epochs)},
            {QStringLiteral("segment"), segment},
            {QStringLiteral("segmentKey"), segmentKey},
            {QStringLiteral("lastSeq"), double(lastSeq)},
            {QStringLiteral("lastHash"), lastHash},
            {QStringLiteral("lamport"), double(lamport)},
            {QStringLiteral("heads"), headsJson},
            {QStringLiteral("joinSecret"), toBase64Url(joinSecret)},
            {QStringLiteral("joinRequest"), joinRequest},
            {QStringLiteral("joinedAt"), double(joinedAt)}};
}

Membership Membership::fromJson(const QJsonObject &json)
{
    Membership m;
    m.slot = json.value(QStringLiteral("slot")).toString();
    m.folder = json.value(QStringLiteral("folder")).toString();
    m.label = json.value(QStringLiteral("label")).toString();
    m.driveId = json.value(QStringLiteral("drive")).toString();
    m.rootDevice = json.value(QStringLiteral("root")).toString();
    fromBase64Url(json.value(QStringLiteral("rootEd25519")).toString(), &m.rootEd25519);
    m.epochs = epochsFrom(json.value(QStringLiteral("epochs")).toArray());
    m.segment = json.value(QStringLiteral("segment")).toString();
    m.segmentKey = json.value(QStringLiteral("segmentKey")).toString();
    m.lastSeq = qint64(json.value(QStringLiteral("lastSeq")).toDouble(-1));
    m.lastHash = json.value(QStringLiteral("lastHash")).toString();
    m.lamport = qint64(json.value(QStringLiteral("lamport")).toDouble());
    const QJsonObject headsJson = json.value(QStringLiteral("heads")).toObject();
    for (auto it = headsJson.constBegin(); it != headsJson.constEnd(); ++it) {
        const QJsonArray pair = it.value().toArray();
        m.heads.insert(it.key(), Head{qint64(pair.at(0).toDouble(-1)), pair.at(1).toString()});
    }
    fromBase64Url(json.value(QStringLiteral("joinSecret")).toString(), &m.joinSecret);
    m.joinRequest = json.value(QStringLiteral("joinRequest")).toString();
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

Membership *LocalState::membership(const QString &slot)
{
    for (Membership &m : drives)
        if (m.slot == slot)
            return &m;
    return nullptr;
}

const Membership *LocalState::membership(const QString &slot) const
{
    for (const Membership &m : drives)
        if (m.slot == slot)
            return &m;
    return nullptr;
}

void LocalState::forget(const QString &slot)
{
    for (int i = 0; i < drives.size(); ++i) {
        if (drives.at(i).slot != slot)
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
        return QStringLiteral("Activity records this computer wrote are missing from the drive.");
    if (c == QLatin1String("rolled_back"))
        return QStringLiteral("The drive shows older activity from %1 than this computer saw before (%2). The drive may have been restored from an old copy.").arg(device, issue.detail);
    if (c == QLatin1String("missing_content"))
        return QStringLiteral("The encrypted content of %1 is missing from the drive.").arg(quoted(issue.detail));
    if (c == QLatin1String("content_damaged"))
        return QStringLiteral("The encrypted content of %1 was changed or damaged and was not opened.").arg(quoted(issue.detail));
    if (c == QLatin1String("untrusted_keyring"))
        return QStringLiteral("A key file that no member device signed was ignored.");
    if (c == QLatin1String("unreadable_objects"))
        return QStringLiteral("%1 encrypted objects on the drive could not be opened with this computer's keys.").arg(issue.detail);
    if (c == QLatin1String("misplaced_object"))
        return QStringLiteral("An object from a different drive was found in this folder and ignored.");
    return c;
}

Drive::Drive(LocalState *state, QString slot) : m_state(state), m_slot(std::move(slot)) {}

const Membership *Drive::membership() const { return m_state ? m_state->membership(m_slot) : nullptr; }

Membership *Drive::mutableMembership() const { return m_state ? m_state->membership(m_slot) : nullptr; }

QString Drive::objectPath(const QString &object) const
{
    const Membership *m = membership();
    return m ? QDir(m->folder).filePath(object) : QString();
}

bool Drive::writeObject(const QString &object, const QByteArray &bytes, Failure *failure) const
{
    QSaveFile file(objectPath(object));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(failure, "drive_write_failed");
    return true;
}

bool Drive::removeObject(const QString &object) const
{
    if (!validObjectName(object))
        return false;
    return QFile::remove(objectPath(object));
}

QString Drive::keyringLabel() const { return QStringLiteral("keyring"); }

QByteArray Drive::folderFingerprint() const
{
    const Membership *m = membership();
    if (!m)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QFileInfoList entries = QDir(m->folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::System,
                                                                QDir::Name);
    for (const QFileInfo &entry : entries) {
        hash.addData(entry.fileName().toUtf8());
        hash.addData(QByteArray::number(entry.size()));
        hash.addData(QByteArray::number(entry.lastModified().toMSecsSinceEpoch()));
    }
    return hash.result();
}

bool Drive::create(LocalState *state, const QString &folder, const QString &label, QString *slot,
                   Failure *failure)
{
    if (!state || !state->isUnlocked())
        return fail(failure, "locked");
    const QFileInfo info(folder);
    if (!info.isDir() || !info.isWritable())
        return fail(failure, "folder_unavailable");
    const QString path = info.canonicalFilePath();
    for (const Membership &m : state->drives)
        if (QFileInfo(m.folder).canonicalFilePath() == path)
            return fail(failure, "folder_already_connected");
    const QStringList existing = QDir(path).entryList(QDir::Files | QDir::Hidden);
    for (const QString &name : existing)
        if (validObjectName(name))
            return fail(failure, "folder_in_use");
    Membership m;
    m.slot = randomId();
    m.folder = path;
    m.label = label.trimmed().isEmpty() ? info.fileName() : label.trimmed().left(80);
    m.driveId = randomId();
    m.rootDevice = state->deviceId;
    m.rootEd25519 = state->publicKeys.ed25519;
    EpochKey first;
    first.key = randomBytes(kKeyBytes);
    first.id = epochIdFor(first.key);
    m.epochs.append(first);
    m.joinedAt = now();
    state->drives.append(m);
    Drive drive(state, m.slot);
    const QJsonObject device{{QStringLiteral("id"), state->deviceId},
                             {QStringLiteral("name"), state->deviceName},
                             {QStringLiteral("keys"), state->publicKeys.toJson()}};
    if (!drive.append({{QStringLiteral("drive_created"),
                        {{QStringLiteral("label"), m.label}, {QStringLiteral("device"), device}}}},
                      failure)) {
        state->forget(m.slot);
        return false;
    }
    *slot = m.slot;
    return true;
}

bool Drive::requestJoin(LocalState *state, const QString &folder, const QString &code,
                        QString *slot, QString *safety, Failure *failure)
{
    if (!state || !state->isUnlocked())
        return fail(failure, "locked");
    QByteArray secret;
    if (!parseInviteCode(code, &secret))
        return fail(failure, "invalid_invite_code");
    const QFileInfo info(folder);
    if (!info.isDir() || !info.isWritable())
        return fail(failure, "folder_unavailable");
    const QString path = info.canonicalFilePath();
    for (const Membership &m : state->drives)
        if (QFileInfo(m.folder).canonicalFilePath() == path)
            return fail(failure, "folder_already_connected");
    QByteArray requestKey;
    if (!hkdf(secret, QStringLiteral("request"), &requestKey))
        return fail(failure, "encryption_failed");
    const QByteArray inviteId = inviteIdBytes(secret);
    QJsonObject payload{{QStringLiteral("suite"), QLatin1String(kSuite)},
                        {QStringLiteral("invite"), toBase64Url(inviteId)},
                        {QStringLiteral("device"), state->deviceId},
                        {QStringLiteral("name"), state->deviceName},
                        {QStringLiteral("keys"), state->publicKeys.toJson()},
                        {QStringLiteral("time"), double(now())}};
    QByteArray signature;
    if (!sign(state->keys.ed25519, requestMessage(payload), &signature))
        return fail(failure, "signing_failed");
    payload.insert(QStringLiteral("sig"), toBase64Url(signature));
    const QByteArray head = header(kRequest);
    const QByteArray nonce = randomBytes(12);
    QByteArray sealed;
    if (!aeadSeal(requestKey, nonce, head + label("request"), compact(payload), &sealed))
        return fail(failure, "encryption_failed");
    wipe(requestKey);
    Membership m;
    m.slot = randomId();
    m.folder = path;
    m.label = info.fileName();
    m.joinSecret = secret;
    m.joinRequest = newObjectName();
    state->drives.append(m);
    Drive drive(state, m.slot);
    if (!drive.writeObject(m.joinRequest, head + nonce + sealed, failure)) {
        state->forget(m.slot);
        return false;
    }
    if (!state->save(failure)) {
        drive.removeObject(m.joinRequest);
        state->forget(m.slot);
        return false;
    }
    *slot = m.slot;
    if (safety)
        *safety = safetyNumber(inviteId, state->publicKeys);
    return true;
}

bool Drive::cancelJoin(Failure *failure)
{
    const Membership *m = membership();
    if (!m || !m->isPending())
        return fail(failure, "not_pending");
    removeObject(m->joinRequest);
    m_state->forget(m_slot);
    return m_state->save(failure);
}

QString Drive::joinSafetyNumber() const
{
    const Membership *m = membership();
    if (!m || m->joinSecret.isEmpty())
        return {};
    return safetyNumber(inviteIdBytes(m->joinSecret), m_state->publicKeys);
}

bool Drive::tryAdopt(const QHash<QString, QByteArray> &keyrings, Failure *failure)
{
    Membership *m = mutableMembership();
    QByteArray approveKey;
    if (!hkdf(m->joinSecret, QStringLiteral("approve"), &approveKey))
        return false;
    const QString inviteId = toBase64Url(inviteIdBytes(m->joinSecret));
    for (auto it = keyrings.cbegin(); it != keyrings.cend(); ++it) {
        QByteArray plain;
        QJsonObject payload;
        if (!openFromDevice(m_state->keys, keyringLabel().toUtf8(), it.value().mid(6), &plain, nullptr)
            || !parseObject(plain, &payload))
            continue;
        wipe(plain);
        if (payload.value(QStringLiteral("device")).toString() != m_state->deviceId
            || payload.value(QStringLiteral("invite")).toString() != inviteId)
            continue;
        QJsonObject unsignedPayload = payload;
        unsignedPayload.remove(QStringLiteral("sig"));
        unsignedPayload.remove(QStringLiteral("mac"));
        QByteArray mac, signature;
        PublicKeys issuer;
        if (!fromBase64Url(payload.value(QStringLiteral("mac")).toString(), &mac)
            || !constantTimeEqual(mac, hmac(approveKey, compact(unsignedPayload)))
            || !PublicKeys::fromJson(payload.value(QStringLiteral("issuerKeys")).toObject(), &issuer)
            || !fromBase64Url(payload.value(QStringLiteral("sig")).toString(), &signature)
            || !verify(issuer.ed25519, keyringMessage(payload), signature))
            continue;
        const QJsonObject root = payload.value(QStringLiteral("root")).toObject();
        QByteArray rootEd;
        const QList<EpochKey> epochs = epochsFrom(payload.value(QStringLiteral("epochs")).toArray());
        if (epochs.isEmpty()
            || !fromBase64Url(root.value(QStringLiteral("ed25519")).toString(), &rootEd)
            || rootEd.size() != 32)
            continue;
        m->driveId = payload.value(QStringLiteral("drive")).toString();
        m->rootDevice = root.value(QStringLiteral("device")).toString();
        m->rootEd25519 = rootEd;
        m->epochs = epochs;
        wipe(m->joinSecret);
        m->joinSecret.clear();
        m->joinRequest.clear();
        m->joinedAt = now();
        wipe(approveKey);
        return m_state->save(failure);
    }
    wipe(approveKey);
    return false;
}

bool Drive::scan(Failure *failure)
{
    Membership *m = mutableMembership();
    if (!m)
        return fail(failure, "unknown_drive");
    const QDir dir(m->folder);
    if (!dir.exists())
        return fail(failure, "folder_unavailable");
    const QByteArray fingerprint = folderFingerprint();
    m_issues.clear();
    m_requests.clear();
    m_blobs.clear();
    m_objectCount = 0;
    m_foreignCount = 0;
    m_joinRequestMissing = false;
    m_removed = false;

    QHash<QString, QByteArray> keyrings, requests, logs;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System,
                                                    QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (!validObjectName(name)) {
            if (!name.startsWith(QLatin1Char('.')) && !partialObjectName(name))
                ++m_foreignCount;
            continue;
        }
        ++m_objectCount;
        QFile file(entry.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray head = file.peek(6);
        if (head.size() < 6 || !head.startsWith("FMSD") || head.at(4) != 1)
            continue;
        const char kind = head.at(5);
        if (kind == kBlob) {
            m_blobs.insert(name);
            continue;
        }
        if (file.size() > kSmallObjectLimit)
            continue;
        const QByteArray bytes = file.readAll();
        if (kind == kKeyring)
            keyrings.insert(name, bytes);
        else if (kind == kRequest)
            requests.insert(name, bytes);
        else if (kind == kLog)
            logs.insert(name, bytes);
    }

    if (m->isPending()) {
        Failure adoptFailure;
        if (!tryAdopt(keyrings, &adoptFailure)) {
            if (adoptFailure.failed()) {
                if (failure)
                    *failure = adoptFailure;
                return false;
            }
            m_joinRequestMissing = !requests.contains(m->joinRequest);
            m_scanned = true;
            m_fingerprint = fingerprint;
            return true;
        }
        m = mutableMembership();
    }
    if (!m->isMember())
        return fail(failure, "unknown_drive");
    const QJsonObject stateBefore = m->toJson();

    struct KeyringPayload
    {
        QString object;
        QJsonObject payload;
        QList<EpochKey> epochs;
    };
    QList<KeyringPayload> mine;
    for (auto it = keyrings.cbegin(); it != keyrings.cend(); ++it) {
        QByteArray plain;
        QJsonObject payload;
        if (!openFromDevice(m_state->keys, keyringLabel().toUtf8(), it.value().mid(6), &plain, nullptr)
            || !parseObject(plain, &payload))
            continue;
        wipe(plain);
        if (payload.value(QStringLiteral("drive")).toString() != m->driveId
            || payload.value(QStringLiteral("device")).toString() != m_state->deviceId)
            continue;
        mine.append({it.key(), payload, epochsFrom(payload.value(QStringLiteral("epochs")).toArray())});
    }

    QMap<QString, QMap<qint64, Record>> chains;
    QSet<QString> rejectedKeys;
    int unreadable = 0;
    QMap<QString, ParsedSegment> segments;
    const auto decode = [&](const QMap<QString, QByteArray> &keys) {
        chains.clear();
        segments.clear();
        unreadable = 0;
        for (auto it = logs.cbegin(); it != logs.cend(); ++it) {
            const QByteArray &bytes = it.value();
            if (bytes.size() < 6 + 8 + 12 + 16) {
                ++unreadable;
                continue;
            }
            const QString keyId = QString::fromLatin1(bytes.mid(6, 8).toHex());
            const QByteArray epochKey = keys.value(keyId);
            QByteArray logKey, plain;
            QJsonObject payload;
            if (epochKey.isEmpty() || !hkdf(epochKey, QStringLiteral("log"), &logKey)
                || !aeadOpen(logKey, bytes.mid(14, 12), bytes.left(14) + m->driveId.toUtf8() + it.key().toUtf8(),
                             bytes.mid(26), &plain)
                || !parseObject(plain, &payload)) {
                ++unreadable;
                continue;
            }
            if (payload.value(QStringLiteral("drive")).toString() != m->driveId) {
                addIssue(QStringLiteral("misplaced_object"), QString(), it.key());
                continue;
            }
            ParsedSegment segment{it.key(), payload.value(QStringLiteral("device")).toString(), keyId,
                                  payload.value(QStringLiteral("records")).toArray()};
            segments.insert(it.key(), segment);
            for (const QJsonValue &value : segment.records) {
                Record record = recordFrom(value.toObject());
                if (record.device != segment.device || record.seq < 0)
                    continue;
                record.hash = recordHash(m->driveId, record.signedJson);
                QMap<qint64, Record> &chain = chains[record.device];
                const auto found = chain.constFind(record.seq);
                if (found != chain.constEnd()) {
                    if (found->hash != record.hash)
                        addIssue(QStringLiteral("log_forked"), record.device, QString());
                    continue;
                }
                chain.insert(record.seq, record);
            }
        }
    };

    QMap<QString, QByteArray> keyMap;
    for (const EpochKey &epoch : m->epochs)
        keyMap.insert(epoch.id, epoch.key);
    for (const KeyringPayload &ring : mine)
        for (const EpochKey &epoch : ring.epochs)
            if (!keyMap.contains(epoch.id))
                keyMap.insert(epoch.id, epoch.key);

    QList<Record> accepted;
    const auto verifyChains = [&]() {
        accepted.clear();
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
                const QMap<qint64, Record> chain = chains.value(device);
                for (auto it = chain.cbegin(); it != chain.cend(); ++it) {
                    const Record &record = it.value();
                    QByteArray signature;
                    if (!fromBase64Url(record.signedJson.value(QStringLiteral("sig")).toString(), &signature)
                        || !verify(ed, recordMessage(m->driveId, record.signedJson), signature)) {
                        addIssue(QStringLiteral("bad_signature"), device, QString());
                        continue;
                    }
                    if (record.seq != expected) {
                        addIssue(QStringLiteral("records_missing"), device,
                                 record.seq - expected == 1
                                     ? QStringLiteral("record %1").arg(expected + 1)
                                     : QStringLiteral("records %1 to %2").arg(expected + 1).arg(record.seq));
                    } else if (record.prev != lastHash) {
                        addIssue(QStringLiteral("chain_broken"), device,
                                 QStringLiteral("record %1").arg(record.seq + 1));
                    }
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
    };

    decode(keyMap);
    verifyChains();
    fold(accepted);

    bool dropped = false;
    for (const KeyringPayload &ring : mine) {
        const QString issuer = ring.payload.value(QStringLiteral("issuer")).toString();
        const auto device = m_devices.constFind(issuer);
        QByteArray signature;
        const bool valid = device != m_devices.constEnd()
                           && fromBase64Url(ring.payload.value(QStringLiteral("sig")).toString(), &signature)
                           && verify(device->keys.ed25519, keyringMessage(ring.payload), signature);
        for (const EpochKey &epoch : ring.epochs) {
            if (m->epoch(epoch.id))
                continue;
            if (valid) {
                m->epochs.append(epoch);
            } else {
                rejectedKeys.insert(epoch.id);
                dropped = true;
            }
        }
        if (!valid)
            addIssue(QStringLiteral("untrusted_keyring"), issuer, ring.object);
    }
    if (dropped) {
        for (const QString &id : rejectedKeys)
            keyMap.remove(id);
        decode(keyMap);
        verifyChains();
        fold(accepted);
    }
    if (unreadable > 0)
        addIssue(QStringLiteral("unreadable_objects"), QString(), QString::number(unreadable));

    checkWitnesses(chains);

    const QMap<qint64, Record> own = chains.value(m_state->deviceId);
    const qint64 ownMax = own.isEmpty() ? -1 : own.lastKey();
    if (ownMax < m->lastSeq)
        addIssue(QStringLiteral("own_records_missing"), m_state->deviceId, QString());
    if (ownMax > m->lastSeq && m->lastSeq >= 0)
        addIssue(QStringLiteral("log_forked"), m_state->deviceId, QString());
    if (m->lastSeq < 0 && ownMax >= 0) {
        m->lastSeq = ownMax;
        m->lastHash = own.last().hash;
    }
    const auto openSegment = segments.constFind(m->segment);
    if (openSegment != segments.constEnd() && openSegment->device == m_state->deviceId)
        m_segmentRecords = openSegment->records;
    else {
        m_segmentRecords = QJsonArray();
        m->segment.clear();
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
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        const QByteArray &bytes = it.value();
        if (bytes.size() < 6 + 12 + 16)
            continue;
        for (const Invite &invite : std::as_const(m_invites)) {
            if (!invite.open(moment))
                continue;
            QByteArray plain;
            QJsonObject payload;
            if (!aeadOpen(invite.requestKey, bytes.mid(6, 12), bytes.left(6) + label("request"),
                          bytes.mid(18), &plain)
                || !parseObject(plain, &payload))
                continue;
            JoinRequest request;
            QByteArray signature;
            request.object = it.key();
            request.inviteId = payload.value(QStringLiteral("invite")).toString();
            request.device = payload.value(QStringLiteral("device")).toString();
            request.name = payload.value(QStringLiteral("name")).toString().left(64);
            request.time = qint64(payload.value(QStringLiteral("time")).toDouble());
            if (request.inviteId != invite.id
                || !PublicKeys::fromJson(payload.value(QStringLiteral("keys")).toObject(), &request.keys)
                || !fromBase64Url(payload.value(QStringLiteral("sig")).toString(), &signature)
                || !verify(request.keys.ed25519, requestMessage(payload), signature)
                || m_devices.contains(request.device))
                break;
            QByteArray inviteBytes;
            fromBase64Url(invite.id, &inviteBytes);
            request.safety = safetyNumber(inviteBytes, request.keys);
            m_requests.append(request);
            break;
        }
    }

    for (const FileEntry &entry : std::as_const(m_files)) {
        if (entry.deleted)
            continue;
        const FileVersion *version = entry.current();
        if (version && !m_blobs.contains(version->blob))
            addIssue(QStringLiteral("missing_content"), QString(), entry.name);
    }

    if (!m_driveLabel.isEmpty())
        m->label = m_driveLabel;
    const auto self = m_devices.constFind(m_state->deviceId);
    m_removed = self != m_devices.constEnd() && self->removed;
    if (m->toJson() != stateBefore && !m_state->save(failure))
        return false;
    m_scanned = true;
    m_fingerprint = fingerprint;
    if (!m_opened && !m_removed) {
        m_opened = true;
        return note(QStringLiteral("drive_opened"), {}, failure);
    }
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
    for (const Record &record : std::as_const(m_records)) {
        for (auto it = record.witness.constBegin(); it != record.witness.constEnd(); ++it) {
            const QJsonArray pair = it.value().toArray();
            const qint64 seq = qint64(pair.at(0).toDouble(-1));
            const QString hash = pair.at(1).toString();
            if (seq < 0 || it.key() == m_state->deviceId)
                continue;
            const QMap<qint64, Record> chain = chains.value(it.key());
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
        const QMap<qint64, Record> chain = chains.value(it.key());
        const auto found = chain.constFind(it->seq);
        if (found == chain.constEnd())
            addIssue(QStringLiteral("rolled_back"), it.key(),
                     QStringLiteral("this computer saw record %1").arg(it->seq + 1));
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
    m_keyringObjects.clear();
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
        const auto author = m_devices.constFind(r.device);
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
            info.root = true;
            m_devices.insert(info.id, info);
            m_driveLabel = x.value(QStringLiteral("label")).toString().left(80);
            m_records.append(r);
            continue;
        }
        if (author == m_devices.constEnd())
            continue;
        m_records.append(r);
        const QString fileId = x.value(QStringLiteral("file")).toString();
        const auto version = [&r, &x]() {
            FileVersion v;
            v.blob = x.value(QStringLiteral("blob")).toString();
            fromBase64Url(x.value(QStringLiteral("key")).toString(), &v.key);
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
            info.addedLamport = r.lamport;
            info.keyring = x.value(QStringLiteral("keyring")).toString();
            m_devices.insert(id, info);
            if (!info.keyring.isEmpty())
                m_keyringObjects.append(info.keyring);
        } else if (a == QLatin1String("device_removed")) {
            auto target = m_devices.find(x.value(QStringLiteral("device")).toString());
            if (target != m_devices.end() && !target->removed) {
                target->removed = true;
                target->removedBy = r.device;
                target->removedAt = r.time;
                target->removedSeq = qint64(x.value(QStringLiteral("upTo")).toDouble(-1));
            }
            for (const QJsonValue &value : x.value(QStringLiteral("keyrings")).toArray())
                m_keyringObjects.append(value.toString());
        } else if (a == QLatin1String("invite_created")) {
            Invite invite;
            invite.id = x.value(QStringLiteral("invite")).toString();
            fromBase64Url(x.value(QStringLiteral("requestKey")).toString(), &invite.requestKey);
            fromBase64Url(x.value(QStringLiteral("approveKey")).toString(), &invite.approveKey);
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
        return m_state->deviceName + QStringLiteral(" (this computer)");
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

bool Drive::writeSegment(Failure *failure)
{
    const Membership *m = membership();
    const EpochKey *epoch = m->epoch(m->segmentKey);
    if (!epoch)
        return fail(failure, "no_drive_key");
    QByteArray logKey, sealed;
    if (!hkdf(epoch->key, QStringLiteral("log"), &logKey))
        return fail(failure, "encryption_failed");
    const QByteArray head = header(kLog) + QByteArray::fromHex(epoch->id.toLatin1());
    const QByteArray nonce = randomBytes(12);
    const QJsonObject payload{{QStringLiteral("drive"), m->driveId},
                              {QStringLiteral("device"), m_state->deviceId},
                              {QStringLiteral("records"), m_segmentRecords}};
    const bool ok = aeadSeal(logKey, nonce, head + m->driveId.toUtf8() + m->segment.toUtf8(),
                             compact(payload), &sealed);
    wipe(logKey);
    if (!ok)
        return fail(failure, "encryption_failed");
    return writeObject(m->segment, head + nonce + sealed, failure);
}

bool Drive::append(const QList<QPair<QString, QJsonObject>> &entries, Failure *failure)
{
    Membership *m = mutableMembership();
    if (!m || !m->isMember())
        return fail(failure, "unknown_drive");
    if (m_removed)
        return fail(failure, "device_removed");
    const EpochKey *epoch = m->currentEpoch();
    if (!epoch)
        return fail(failure, "no_drive_key");
    const Membership before = *m;
    const QJsonArray segmentBefore = m_segmentRecords;
    if (m->segment.isEmpty() || m->segmentKey != epoch->id || m_segmentRecords.size() >= kSegmentLimit) {
        m->segment = newObjectName();
        m->segmentKey = epoch->id;
        m_segmentRecords = QJsonArray();
    }
    QList<Record> added;
    for (const auto &entry : entries) {
        QJsonObject witness;
        for (auto it = m->heads.cbegin(); it != m->heads.cend(); ++it)
            if (it.key() != m_state->deviceId && it->seq >= 0)
                witness.insert(it.key(), QJsonArray{double(it->seq), it->hash});
        QJsonObject json{{QStringLiteral("d"), m_state->deviceId},
                         {QStringLiteral("s"), double(m->lastSeq + 1)},
                         {QStringLiteral("l"), double(m->lamport + 1)},
                         {QStringLiteral("t"), double(now())},
                         {QStringLiteral("a"), entry.first},
                         {QStringLiteral("x"), entry.second},
                         {QStringLiteral("p"), m->lastHash},
                         {QStringLiteral("w"), witness}};
        QByteArray signature;
        if (!sign(m_state->keys.ed25519, recordMessage(m->driveId, json), &signature)) {
            *m = before;
            m_segmentRecords = segmentBefore;
            return fail(failure, "signing_failed");
        }
        json.insert(QStringLiteral("sig"), toBase64Url(signature));
        Record record = recordFrom(json);
        record.hash = recordHash(m->driveId, json);
        m->lastSeq = record.seq;
        m->lastHash = record.hash;
        m->lamport = record.lamport;
        m->heads[m_state->deviceId] = Head{record.seq, record.hash};
        m_segmentRecords.append(json);
        added.append(record);
    }
    if (!writeSegment(failure)) {
        *m = before;
        m_segmentRecords = segmentBefore;
        return false;
    }
    if (!m_state->save(failure))
        return false;
    QList<Record> all = m_records + added;
    fold(all);
    return true;
}

bool Drive::note(const QString &action, const QJsonObject &data, Failure *failure)
{
    return append({{action, data}}, failure);
}

bool Drive::createInvite(QString *code, QString *inviteId, Failure *failure)
{
    QByteArray secret, requestKey, approveKey;
    const QString text = newInviteCode(&secret);
    if (!hkdf(secret, QStringLiteral("request"), &requestKey)
        || !hkdf(secret, QStringLiteral("approve"), &approveKey))
        return fail(failure, "encryption_failed");
    const QString id = toBase64Url(inviteIdBytes(secret));
    wipe(secret);
    const bool ok = append({{QStringLiteral("invite_created"),
                             {{QStringLiteral("invite"), id},
                              {QStringLiteral("requestKey"), toBase64Url(requestKey)},
                              {QStringLiteral("approveKey"), toBase64Url(approveKey)},
                              {QStringLiteral("expires"), double(now() + kInviteLifetime)}}}},
                           failure);
    wipe(requestKey);
    wipe(approveKey);
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
    return note(QStringLiteral("invite_revoked"), {{QStringLiteral("invite"), inviteId}}, failure);
}

bool Drive::sealKeyring(const DeviceInfo &recipient, const QByteArray &approveKey,
                        const QString &inviteId, QString *object, Failure *failure)
{
    const Membership *m = membership();
    QJsonObject payload{{QStringLiteral("suite"), QLatin1String(kSuite)},
                        {QStringLiteral("drive"), m->driveId},
                        {QStringLiteral("device"), recipient.id},
                        {QStringLiteral("epochs"), epochsJson(m->epochs)},
                        {QStringLiteral("root"),
                         QJsonObject{{QStringLiteral("device"), m->rootDevice},
                                     {QStringLiteral("ed25519"), toBase64Url(m->rootEd25519)}}},
                        {QStringLiteral("issuer"), m_state->deviceId},
                        {QStringLiteral("issuerKeys"), m_state->publicKeys.toJson()},
                        {QStringLiteral("time"), double(now())}};
    if (!approveKey.isEmpty()) {
        payload.insert(QStringLiteral("invite"), inviteId);
        payload.insert(QStringLiteral("mac"), toBase64Url(hmac(approveKey, compact(payload))));
    }
    QByteArray signature, sealed;
    if (!sign(m_state->keys.ed25519, keyringMessage(payload), &signature))
        return fail(failure, "signing_failed");
    payload.insert(QStringLiteral("sig"), toBase64Url(signature));
    QByteArray plain = compact(payload);
    const bool ok = sealToDevice(recipient.keys, keyringLabel().toUtf8(), plain, &sealed, failure);
    wipe(plain);
    if (!ok)
        return false;
    *object = newObjectName();
    return writeObject(*object, header(kKeyring) + sealed, failure);
}

bool Drive::approve(const QString &object, Failure *failure)
{
    const auto found = std::find_if(m_requests.cbegin(), m_requests.cend(),
                                    [&object](const JoinRequest &r) { return r.object == object; });
    if (found == m_requests.cend())
        return fail(failure, "unknown_request");
    const JoinRequest request = *found;
    const auto invite = m_invites.constFind(request.inviteId);
    if (invite == m_invites.constEnd() || !invite->open(now()))
        return fail(failure, "invite_expired");
    DeviceInfo device;
    device.id = request.device;
    device.name = request.name;
    device.keys = request.keys;
    QString keyring;
    if (!sealKeyring(device, invite->approveKey, invite->id, &keyring, failure))
        return false;
    if (!append({{QStringLiteral("device_added"),
                  {{QStringLiteral("device"), device.id},
                   {QStringLiteral("name"), device.name},
                   {QStringLiteral("keys"), device.keys.toJson()},
                   {QStringLiteral("invite"), request.inviteId},
                   {QStringLiteral("keyring"), keyring}}},
                 {QStringLiteral("invite_used"),
                  {{QStringLiteral("invite"), request.inviteId},
                   {QStringLiteral("device"), device.id},
                   {QStringLiteral("name"), device.name}}}},
                failure)) {
        removeObject(keyring);
        return false;
    }
    removeObject(object);
    m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(),
                                    [&object](const JoinRequest &r) { return r.object == object; }),
                     m_requests.end());
    return true;
}

bool Drive::deny(const QString &object, Failure *failure)
{
    const auto found = std::find_if(m_requests.cbegin(), m_requests.cend(),
                                    [&object](const JoinRequest &r) { return r.object == object; });
    if (found == m_requests.cend())
        return fail(failure, "unknown_request");
    const JoinRequest request = *found;
    if (!append({{QStringLiteral("join_denied"),
                  {{QStringLiteral("invite"), request.inviteId},
                   {QStringLiteral("device"), request.device},
                   {QStringLiteral("name"), request.name}}},
                 {QStringLiteral("invite_revoked"), {{QStringLiteral("invite"), request.inviteId}}}},
                failure))
        return false;
    removeObject(object);
    m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(),
                                    [&object](const JoinRequest &r) { return r.object == object; }),
                     m_requests.end());
    return true;
}

bool Drive::writeBlob(QIODevice *source, QString *object, QByteArray *key, qint64 *size,
                      QString *digest, Failure *failure)
{
    const Membership *m = membership();
    if (!source || !source->isOpen() || source->isSequential())
        return fail(failure, "read_failed");
    const qint64 length = source->size() - source->pos();
    const qint64 padded = paddedSize(length);
    const qint64 chunks = chunkCount(padded);
    const QByteArray blobKey = randomBytes(kKeyBytes);
    const QByteArray prefix = randomBytes(8);
    const QByteArray head = header(kBlob) + prefix;
    const QString name = newObjectName();
    QSaveFile out(QDir(m->folder).filePath(name));
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || out.write(head) != head.size())
        return fail(failure, "drive_write_failed");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = length;
    for (qint64 i = 0; i < chunks; ++i) {
        const qint64 span = std::min(kChunk, padded - i * kChunk);
        QByteArray plain = remaining > 0 ? source->read(std::min(span, remaining)) : QByteArray();
        if (plain.size() != std::min(span, std::max<qint64>(remaining, 0))) {
            out.cancelWriting();
            return fail(failure, "read_failed");
        }
        hash.addData(plain);
        remaining -= plain.size();
        plain.append(QByteArray(int(span - plain.size()), '\0'));
        QByteArray sealed;
        const bool ok = aeadSeal(blobKey, prefix + beBytes(quint64(i), 4),
                                 blobAad(m->driveId, name, i, i == chunks - 1, head), plain, &sealed);
        wipe(plain);
        if (!ok || out.write(sealed) != sealed.size()) {
            out.cancelWriting();
            return fail(failure, "drive_write_failed");
        }
    }
    if (!out.commit())
        return fail(failure, "drive_write_failed");
    *object = name;
    *key = blobKey;
    *size = length;
    *digest = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool Drive::transcodeBlob(const FileVersion &version, QString *object, QByteArray *key,
                          Failure *failure)
{
    const Membership *m = membership();
    const qint64 padded = paddedSize(version.size);
    const qint64 chunks = chunkCount(padded);
    QFile in(objectPath(version.blob));
    if (!in.open(QIODevice::ReadOnly) || in.size() != 14 + padded + 16 * chunks)
        return fail(failure, "content_damaged");
    const QByteArray oldHead = in.read(14);
    const QByteArray newKey = randomBytes(kKeyBytes);
    const QByteArray prefix = randomBytes(8);
    const QByteArray head = header(kBlob) + prefix;
    const QString name = newObjectName();
    QSaveFile out(QDir(m->folder).filePath(name));
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || out.write(head) != head.size())
        return fail(failure, "drive_write_failed");
    for (qint64 i = 0; i < chunks; ++i) {
        const qint64 span = std::min(kChunk, padded - i * kChunk);
        const bool last = i == chunks - 1;
        QByteArray plain, sealed;
        if (!aeadOpen(version.key, oldHead.mid(6, 8) + beBytes(quint64(i), 4),
                      blobAad(m->driveId, version.blob, i, last, oldHead), in.read(span + 16), &plain)) {
            out.cancelWriting();
            return fail(failure, "content_damaged");
        }
        const bool ok = aeadSeal(newKey, prefix + beBytes(quint64(i), 4),
                                 blobAad(m->driveId, name, i, last, head), plain, &sealed);
        wipe(plain);
        if (!ok || out.write(sealed) != sealed.size()) {
            out.cancelWriting();
            return fail(failure, "drive_write_failed");
        }
    }
    if (!out.commit())
        return fail(failure, "drive_write_failed");
    *object = name;
    *key = newKey;
    return true;
}

bool Drive::addFile(const QString &name, QIODevice *source, QString *fileId, Failure *failure,
                    const QString &kind)
{
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty() || cleaned.size() > 255 || cleaned.contains(QLatin1Char('/'))
        || cleaned.contains(QLatin1Char('\\')))
        return fail(failure, "invalid_name");
    QString blob, digest;
    QByteArray key;
    qint64 size = 0;
    if (!writeBlob(source, &blob, &key, &size, &digest, failure))
        return false;
    const QString id = randomId();
    if (!append({{QStringLiteral("file_created"),
                  {{QStringLiteral("file"), id},
                   {QStringLiteral("name"), cleaned},
                   {QStringLiteral("kind"), kind.isEmpty() ? fileKindForName(cleaned) : kind},
                   {QStringLiteral("blob"), blob},
                   {QStringLiteral("key"), toBase64Url(key)},
                   {QStringLiteral("size"), double(size)},
                   {QStringLiteral("sha256"), digest}}}},
                failure)) {
        removeObject(blob);
        return false;
    }
    wipe(key);
    if (fileId)
        *fileId = id;
    return true;
}

bool Drive::saveFile(const QString &fileId, QIODevice *source, Failure *failure)
{
    const FileEntry *entry = file(fileId);
    if (!entry || !entry->current())
        return fail(failure, "unknown_file");
    const QString base = entry->current()->blob;
    const QString name = entry->name;
    QString blob, digest;
    QByteArray key;
    qint64 size = 0;
    if (!writeBlob(source, &blob, &key, &size, &digest, failure))
        return false;
    if (!append({{QStringLiteral("file_saved"),
                  {{QStringLiteral("file"), fileId},
                   {QStringLiteral("name"), name},
                   {QStringLiteral("blob"), blob},
                   {QStringLiteral("key"), toBase64Url(key)},
                   {QStringLiteral("size"), double(size)},
                   {QStringLiteral("sha256"), digest},
                   {QStringLiteral("base"), base}}}},
                failure)) {
        removeObject(blob);
        return false;
    }
    wipe(key);
    return true;
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
    const Membership *m = membership();
    const qint64 padded = paddedSize(version.size);
    const qint64 chunks = chunkCount(padded);
    QFile in(objectPath(version.blob));
    const auto damaged = [&]() {
        Failure ignored;
        note(QStringLiteral("integrity_alert"),
             {{QStringLiteral("code"), QStringLiteral("content_damaged")},
              {QStringLiteral("file"), fileId},
              {QStringLiteral("detail"), name}},
             &ignored);
        addIssue(QStringLiteral("content_damaged"), QString(), name);
        return fail(failure, "content_damaged");
    };
    if (!in.open(QIODevice::ReadOnly))
        return fail(failure, "content_missing");
    if (in.size() != 14 + padded + 16 * chunks)
        return damaged();
    const QByteArray head = in.read(14);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = version.size;
    for (qint64 i = 0; i < chunks; ++i) {
        const qint64 span = std::min(kChunk, padded - i * kChunk);
        QByteArray plain;
        if (!aeadOpen(version.key, head.mid(6, 8) + beBytes(quint64(i), 4),
                      blobAad(m->driveId, version.blob, i, i == chunks - 1, head), in.read(span + 16),
                      &plain))
            return damaged();
        const qint64 take = std::min<qint64>(remaining, plain.size());
        if (take > 0) {
            const QByteArray part = plain.left(int(take));
            hash.addData(part);
            if (out->write(part) != take) {
                wipe(plain);
                return fail(failure, "write_failed");
            }
        }
        wipe(plain);
        remaining -= take;
    }
    if (QString::fromLatin1(hash.result().toHex()) != version.sha256)
        return damaged();
    return note(QStringLiteral("file_opened"),
                {{QStringLiteral("file"), fileId},
                 {QStringLiteral("name"), name},
                 {QStringLiteral("version"), index},
                 {QStringLiteral("purpose"), purpose.isEmpty() ? QStringLiteral("view") : purpose}},
                failure);
}

bool Drive::renameFile(const QString &fileId, const QString &name, Failure *failure)
{
    const FileEntry *entry = file(fileId);
    const QString cleaned = name.trimmed();
    if (!entry)
        return fail(failure, "unknown_file");
    if (cleaned.isEmpty() || cleaned.size() > 255 || cleaned.contains(QLatin1Char('/'))
        || cleaned.contains(QLatin1Char('\\')))
        return fail(failure, "invalid_name");
    return note(QStringLiteral("file_renamed"),
                {{QStringLiteral("file"), fileId},
                 {QStringLiteral("name"), cleaned},
                 {QStringLiteral("from"), entry->name}},
                failure);
}

bool Drive::deleteFile(const QString &fileId, Failure *failure)
{
    const FileEntry *entry = file(fileId);
    if (!entry)
        return fail(failure, "unknown_file");
    QSet<QString> blobs;
    for (const FileVersion &version : entry->versions)
        blobs.insert(version.blob);
    if (!note(QStringLiteral("file_deleted"),
              {{QStringLiteral("file"), fileId}, {QStringLiteral("name"), entry->name}}, failure))
        return false;
    for (const QString &blob : std::as_const(blobs))
        removeObject(blob);
    return true;
}

bool Drive::restoreVersion(const QString &fileId, int index, Failure *failure)
{
    const FileEntry *entry = file(fileId);
    if (!entry)
        return fail(failure, "unknown_file");
    if (index < 0 || index >= entry->versions.size())
        return fail(failure, "unknown_version");
    const FileVersion version = entry->versions.at(index);
    return note(QStringLiteral("file_restored"),
                {{QStringLiteral("file"), fileId},
                 {QStringLiteral("name"), entry->name},
                 {QStringLiteral("version"), index},
                 {QStringLiteral("blob"), version.blob},
                 {QStringLiteral("key"), toBase64Url(version.key)},
                 {QStringLiteral("size"), double(version.size)},
                 {QStringLiteral("sha256"), version.sha256}},
                failure);
}

bool Drive::removeDevice(const QString &deviceId, const std::function<void(int, int)> &progress,
                         Failure *failure)
{
    Membership *m = mutableMembership();
    const auto target = m_devices.constFind(deviceId);
    if (target == m_devices.constEnd() || target->removed)
        return fail(failure, "unknown_device");
    if (deviceId == m_state->deviceId)
        return fail(failure, "cannot_remove_self");
    const QString targetName = target->name;
    const QStringList oldKeyrings = m_keyringObjects;
    EpochKey next;
    next.key = randomBytes(kKeyBytes);
    next.id = epochIdFor(next.key);
    next.number = m->currentEpoch() ? m->currentEpoch()->number + 1 : 1;
    m->epochs.append(next);
    QStringList written;
    const auto undo = [&]() {
        for (const QString &object : std::as_const(written))
            removeObject(object);
        Membership *current = mutableMembership();
        for (int i = 0; i < current->epochs.size(); ++i)
            if (current->epochs.at(i).id == next.id)
                current->epochs.removeAt(i--);
        return false;
    };
    for (const DeviceInfo &device : std::as_const(m_devices)) {
        if (device.removed || device.id == deviceId || device.id == m_state->deviceId)
            continue;
        QString object;
        if (!sealKeyring(device, {}, {}, &object, failure))
            return undo();
        written.append(object);
    }
    QJsonArray keyrings;
    for (const QString &object : std::as_const(written))
        keyrings.append(object);
    if (!append({{QStringLiteral("device_removed"),
                  {{QStringLiteral("device"), deviceId},
                   {QStringLiteral("name"), targetName},
                   {QStringLiteral("epoch"), next.number},
                   {QStringLiteral("keyId"), next.id},
                   {QStringLiteral("upTo"), double(m->heads.value(deviceId).seq)},
                   {QStringLiteral("keyrings"), keyrings}}}},
                failure))
        return undo();
    for (const QString &object : oldKeyrings)
        if (!written.contains(object))
            removeObject(object);

    const QList<FileEntry> current = files();
    int total = 0, done = 0;
    for (const FileEntry &entry : current)
        total += int(entry.versions.size());
    for (const FileEntry &entry : current) {
        QHash<QString, QPair<QString, QByteArray>> moved;
        QJsonArray changes;
        for (const FileVersion &version : entry.versions) {
            if (progress)
                progress(++done, total);
            if (moved.contains(version.blob) || !m_blobs.contains(version.blob))
                continue;
            QString object;
            QByteArray key;
            Failure transcodeFailure;
            if (!transcodeBlob(version, &object, &key, &transcodeFailure)) {
                if (transcodeFailure.code == QLatin1String("content_damaged")) {
                    addIssue(QStringLiteral("content_damaged"), QString(), entry.name);
                    continue;
                }
                if (failure)
                    *failure = transcodeFailure;
                return false;
            }
            moved.insert(version.blob, {object, key});
            changes.append(QJsonObject{{QStringLiteral("old"), version.blob},
                                       {QStringLiteral("blob"), object},
                                       {QStringLiteral("key"), toBase64Url(key)}});
        }
        if (changes.isEmpty())
            continue;
        if (!note(QStringLiteral("file_rekeyed"),
                  {{QStringLiteral("file"), entry.id},
                   {QStringLiteral("name"), entry.name},
                   {QStringLiteral("versions"), changes}},
                  failure)) {
            for (const auto &pair : std::as_const(moved))
                removeObject(pair.first);
            return false;
        }
        for (auto it = moved.cbegin(); it != moved.cend(); ++it) {
            removeObject(it.key());
            m_blobs.remove(it.key());
            m_blobs.insert(it->first);
        }
    }
    return true;
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
