#include "MirrorActionsConfiguration.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <memory>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace forkmesh::mirror_actions {
namespace {

constexpr qsizetype kMaximumRequestBytes = 64 * 1024;
constexpr qsizetype kMaximumVariableValueBytes = 16 * 1024;
constexpr qsizetype kMaximumVariableBytes = 48 * 1024;
constexpr int kMaximumVariables = 64;
constexpr qsizetype kMaximumJournalBytes = 2 * 1024 * 1024;
constexpr int kDefaultConfigurationLockTimeoutMs = 30 * 1000;
int g_configurationLockTimeoutMs = kDefaultConfigurationLockTimeoutMs;
constexpr auto kJournalType =
    "forkmesh.mirror-actions-configuration-transaction";

const QRegularExpression kRequestId(
    QStringLiteral("^[a-f0-9]{32}$"));
const QRegularExpression kNodeName(
    QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
const QRegularExpression kRepositoryName(
    QStringLiteral("^[A-Za-z0-9._-]{1,100}$"));
const QRegularExpression kBranchName(
    QStringLiteral("^(?!/)(?!.*(?:\\.\\.|//))[A-Za-z0-9._/-]{1,120}(?<!/)$"));
const QRegularExpression kVariableName(
    QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,63}$"));

QJsonObject result(const QString &requestId, const QString &node,
                   bool enabled, bool replaced, int count, bool ok,
                   const QString &errorCode = {})
{
    QJsonObject value{
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.mirror-actions-configuration-result")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("ok"), ok},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("node"), node},
        {QStringLiteral("actionsEnabled"), enabled},
        {QStringLiteral("variablesReplaced"), replaced},
        {QStringLiteral("variableCount"), count},
    };
    if (!errorCode.isEmpty())
        value.insert(QStringLiteral("errorCode"), errorCode);
    return value;
}

bool exactFields(const QJsonObject &object, const QSet<QString> &required,
                 const QSet<QString> &optional = {})
{
    QSet<QString> actual;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        actual.insert(it.key());
    const QSet<QString> missing = required - actual;
    QSet<QString> allowed = required;
    allowed.unite(optional);
    const QSet<QString> unexpected = actual - allowed;
    return missing.isEmpty() && unexpected.isEmpty();
}

QString safeNode(const QJsonObject &request)
{
    const QString value =
        request.value(QStringLiteral("node")).toString().trimmed();
    return kNodeName.match(value).hasMatch() ? value : QString();
}

QString safeRequestId(const QJsonObject &request)
{
    const QString value =
        request.value(QStringLiteral("requestId")).toString().trimmed();
    return kRequestId.match(value).hasMatch()
               ? value
               : QString(32, QLatin1Char('0'));
}

bool safeAbsolutePath(const QString &value)
{
    if (value.size() < 2 || value.size() > 4096 ||
        value.contains(QChar::Null) || value.contains(QLatin1Char('\r')) ||
        value.contains(QLatin1Char('\n'))) {
        return false;
    }
    const QFileInfo info(value);
    return info.isAbsolute() && !info.isSymLink();
}

QString transactionIdentity(const QSettings &settings)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (QString::number(int(settings.format())) + QLatin1Char(':') +
             settings.fileName())
                .toUtf8(),
            QCryptographicHash::Sha256)
            .toHex()
            .left(24));
}

QString transactionBasePath(const QSettings &settings)
{
    const QString directory = detail::transactionDirectoryFor(
        settings.fileName(), settings.format(),
        forkmesh::paths::dataRoot());
    return QDir(directory).filePath(
        QStringLiteral(".forkmesh-actions-configuration-") +
        transactionIdentity(settings));
}

// Where builds before this fix put the lock and journal: next to whatever
// QSettings::fileName() named, even when that was a registry key.
QString legacyTransactionBasePath(const QSettings &settings)
{
    const QString settingsName = settings.fileName();
    if (settingsName.isEmpty() || !QFileInfo(settingsName).isAbsolute())
        return {};
    return QDir(QFileInfo(settingsName).absolutePath())
        .filePath(QStringLiteral(".forkmesh-actions-configuration-") +
                  transactionIdentity(settings));
}

QString transactionJournalPath(const QSettings &settings)
{
    return transactionBasePath(settings) + QStringLiteral(".json");
}

QString transactionLockPath(const QSettings &settings)
{
    return transactionBasePath(settings) + QStringLiteral(".lock");
}

bool prepareTransactionDirectory(const QSettings &settings,
                                 QString *errorCode)
{
    const QString directory =
        QFileInfo(transactionBasePath(settings)).absolutePath();
    if (directory.isEmpty() || !QFileInfo(directory).isAbsolute()) {
        if (errorCode)
            *errorCode = QStringLiteral("configuration_lock_unavailable");
        return false;
    }
    const QFileInfo existing(directory);
    if ((existing.exists() &&
         (!existing.isDir() || existing.isSymLink())) ||
        (!existing.exists() && !QDir().mkpath(directory))) {
        if (errorCode)
            *errorCode = QStringLiteral("configuration_lock_unavailable");
        return false;
    }
#if defined(Q_OS_UNIX)
    if (!QFile::setPermissions(
            directory,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        if (errorCode)
            *errorCode = QStringLiteral("configuration_lock_unavailable");
        return false;
    }
#endif
    return true;
}

bool syncDirectory(const QString &path)
{
#if defined(Q_OS_UNIX)
    const QByteArray encoded = QFile::encodeName(path);
    const int descriptor =
        ::open(encoded.constData(), O_RDONLY
#if defined(O_DIRECTORY)
                                   | O_DIRECTORY
#endif
        );
    if (descriptor < 0)
        return false;
    const bool ok = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return ok;
#else
    Q_UNUSED(path);
    return true;
#endif
}

QJsonObject encodeVariant(const QVariant &value, bool *ok);

QJsonArray encodeVariantList(const QVariantList &values, bool *ok)
{
    QJsonArray encoded;
    for (const QVariant &value : values) {
        const QJsonObject item = encodeVariant(value, ok);
        if (!*ok)
            return {};
        encoded.append(item);
    }
    return encoded;
}

QJsonObject encodeVariant(const QVariant &value, bool *ok)
{
    QJsonObject encoded;
    switch (value.typeId()) {
    case QMetaType::Bool:
        encoded.insert(QStringLiteral("type"), QStringLiteral("bool"));
        encoded.insert(QStringLiteral("value"), value.toBool());
        break;
    case QMetaType::Int:
        encoded.insert(QStringLiteral("type"), QStringLiteral("int"));
        encoded.insert(QStringLiteral("value"),
                       QString::number(value.toInt()));
        break;
    case QMetaType::UInt:
        encoded.insert(QStringLiteral("type"), QStringLiteral("uint"));
        encoded.insert(QStringLiteral("value"),
                       QString::number(value.toUInt()));
        break;
    case QMetaType::LongLong:
        encoded.insert(QStringLiteral("type"), QStringLiteral("longlong"));
        encoded.insert(QStringLiteral("value"),
                       QString::number(value.toLongLong()));
        break;
    case QMetaType::ULongLong:
        encoded.insert(QStringLiteral("type"), QStringLiteral("ulonglong"));
        encoded.insert(QStringLiteral("value"),
                       QString::number(value.toULongLong()));
        break;
    case QMetaType::Double:
        encoded.insert(QStringLiteral("type"), QStringLiteral("double"));
        encoded.insert(QStringLiteral("value"),
                       QString::number(value.toDouble(), 'g', 17));
        break;
    case QMetaType::QString:
        encoded.insert(QStringLiteral("type"), QStringLiteral("string"));
        encoded.insert(QStringLiteral("value"), value.toString());
        break;
    case QMetaType::QByteArray:
        encoded.insert(QStringLiteral("type"), QStringLiteral("bytes"));
        encoded.insert(
            QStringLiteral("value"),
            QString::fromLatin1(
                value.toByteArray().toBase64(QByteArray::Base64UrlEncoding |
                                             QByteArray::OmitTrailingEquals)));
        break;
    case QMetaType::QStringList: {
        encoded.insert(QStringLiteral("type"), QStringLiteral("strings"));
        QJsonArray strings;
        for (const QString &item : value.toStringList())
            strings.append(item);
        encoded.insert(QStringLiteral("value"), strings);
        break;
    }
    case QMetaType::QVariantList:
        encoded.insert(QStringLiteral("type"), QStringLiteral("list"));
        encoded.insert(QStringLiteral("value"),
                       encodeVariantList(value.toList(), ok));
        break;
    case QMetaType::QVariantMap: {
        encoded.insert(QStringLiteral("type"), QStringLiteral("map"));
        QJsonObject values;
        const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const QJsonObject item = encodeVariant(it.value(), ok);
            if (!*ok)
                return {};
            values.insert(it.key(), item);
        }
        encoded.insert(QStringLiteral("value"), values);
        break;
    }
    default:
        *ok = false;
        return {};
    }
    return encoded;
}

bool exactObjectFields(const QJsonObject &object,
                       std::initializer_list<QString> fields)
{
    QSet<QString> expected(fields.begin(), fields.end());
    QSet<QString> actual;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        actual.insert(it.key());
    return actual == expected;
}

QVariant decodeVariant(const QJsonObject &encoded, bool *ok)
{
    if (!exactObjectFields(
            encoded,
            {QStringLiteral("type"), QStringLiteral("value")})) {
        *ok = false;
        return {};
    }
    const QString type = encoded.value(QStringLiteral("type")).toString();
    const QJsonValue raw = encoded.value(QStringLiteral("value"));
    if (type == QLatin1String("bool") && raw.isBool())
        return raw.toBool();
    if (type == QLatin1String("string") && raw.isString())
        return raw.toString();
    if (type == QLatin1String("bytes") && raw.isString()) {
        const QByteArray text = raw.toString().toLatin1();
        static const QRegularExpression base64Url(
            QStringLiteral("^[A-Za-z0-9_-]*$"));
        if (!base64Url.match(QString::fromLatin1(text)).hasMatch()) {
            *ok = false;
            return {};
        }
        const QByteArray value =
            QByteArray::fromBase64(
                text, QByteArray::Base64UrlEncoding |
                          QByteArray::AbortOnBase64DecodingErrors);
        if (value.toBase64(QByteArray::Base64UrlEncoding |
                           QByteArray::OmitTrailingEquals) != text) {
            *ok = false;
            return {};
        }
        return value;
    }
    if ((type == QLatin1String("int") ||
         type == QLatin1String("uint") ||
         type == QLatin1String("longlong") ||
         type == QLatin1String("ulonglong") ||
         type == QLatin1String("double")) &&
        raw.isString()) {
        bool converted = false;
        if (type == QLatin1String("int")) {
            const int value = raw.toString().toInt(&converted);
            if (converted)
                return value;
        } else if (type == QLatin1String("uint")) {
            const uint value = raw.toString().toUInt(&converted);
            if (converted)
                return value;
        } else if (type == QLatin1String("longlong")) {
            const qlonglong value = raw.toString().toLongLong(&converted);
            if (converted)
                return value;
        } else if (type == QLatin1String("ulonglong")) {
            const qulonglong value =
                raw.toString().toULongLong(&converted);
            if (converted)
                return value;
        } else {
            const double value = raw.toString().toDouble(&converted);
            if (converted && qIsFinite(value))
                return value;
        }
        *ok = false;
        return {};
    }
    if (type == QLatin1String("strings") && raw.isArray()) {
        QStringList values;
        for (const QJsonValue &item : raw.toArray()) {
            if (!item.isString()) {
                *ok = false;
                return {};
            }
            values.append(item.toString());
        }
        return values;
    }
    if (type == QLatin1String("list") && raw.isArray()) {
        QVariantList values;
        for (const QJsonValue &item : raw.toArray()) {
            if (!item.isObject()) {
                *ok = false;
                return {};
            }
            const QVariant value = decodeVariant(item.toObject(), ok);
            if (!*ok)
                return {};
            values.append(value);
        }
        return values;
    }
    if (type == QLatin1String("map") && raw.isObject()) {
        QVariantMap values;
        const QJsonObject object = raw.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!it.value().isObject()) {
                *ok = false;
                return {};
            }
            const QVariant value =
                decodeVariant(it.value().toObject(), ok);
            if (!*ok)
                return {};
            values.insert(it.key(), value);
        }
        return values;
    }
    *ok = false;
    return {};
}

QJsonObject readGatewayConfiguration(QString *pathOut)
{
    QStringList candidates;
    const QString configured =
        qEnvironmentVariable("FORKMESH_MIRROR_REFRESH_CONFIG").trimmed();
    if (!configured.isEmpty())
        candidates.append(configured);
    candidates.append(
        QStringLiteral(
            "/var/lib/forkmesh-mirror/gateway/mirror-refresh.json"));
    for (const QString &path : candidates) {
        if (!safeAbsolutePath(path))
            continue;
        QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
            info.size() > 64 * 1024)
            continue;
        const QFileDevice::Permissions permissions = info.permissions();
        if (permissions.testFlag(QFileDevice::WriteGroup) ||
            permissions.testFlag(QFileDevice::WriteOther)) {
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject())
            continue;
        const QJsonObject value = document.object();
        if (value.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
            value.value(QStringLiteral("type")).toString() !=
                QLatin1String("forkmesh.headless-mirror-refresh"))
            continue;
        if (pathOut)
            *pathOut = path;
        return value;
    }
    return {};
}

QString repositoryOwner(const QJsonObject &gateway)
{
    const QString node =
        gateway.value(QStringLiteral("nodeOwner")).toString().trimmed();
    const QJsonArray aliases =
        gateway.value(QStringLiteral("ownerAliases")).toArray();
    for (const QJsonValue &value : aliases) {
        const QString alias = value.toString().trimmed();
        if (alias != node && kNodeName.match(alias).hasMatch())
            return alias;
    }
    return node;
}

QString actionMirrorPath(QSettings &settings, const QString &node,
                         const QString &owner,
                         const QString &repository)
{
    const QByteArray key =
        QCryptographicHash::hash(
            (node + QLatin1Char('/') + owner + QLatin1Char('/') + repository)
                .toUtf8(),
            QCryptographicHash::Sha256)
            .toHex()
            .left(24);
    const QString dataRoot =
        settings.format() == QSettings::IniFormat
            ? QDir(QFileInfo(settings.fileName()).absolutePath())
                  .filePath(QStringLiteral("forkmesh-actions-test-data"))
            : forkmesh::paths::dataRoot();
    return QDir(dataRoot)
        .filePath(QStringLiteral("actions/mirrors/") +
                  QString::fromLatin1(key) + QStringLiteral(".git"));
}

// code-health: allow(blocking-wait) CLI only: reached from runConfigurationStdin (--configure-mirror-actions-stdin), never from the GUI
bool runGit(const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForStarted(5000) ||
        !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

bool prepareActionMirror(const QString &source, const QString &destination)
{
    if (!safeAbsolutePath(source) || !QFileInfo(source).isDir() ||
        QFileInfo(source).isSymLink() || !safeAbsolutePath(destination))
        return false;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return false;
    if (!QFileInfo(destination).exists()) {
        return runGit(
            {QStringLiteral("clone"), QStringLiteral("--mirror"),
             QStringLiteral("--no-hardlinks"), source, destination},
            30000);
    }
    if (!QFileInfo(QDir(destination).filePath(QStringLiteral("HEAD"))).isFile())
        return false;
    return runGit(
        {QStringLiteral("--git-dir"), destination, QStringLiteral("fetch"),
         QStringLiteral("--prune"), source,
         QStringLiteral("+refs/heads/*:refs/heads/*"),
         QStringLiteral("+refs/tags/*:refs/tags/*")},
        30000);
}

constexpr auto kRepositoriesSetting = "repositories/items";

QList<QVariantMap> readRepositories(QSettings &settings)
{
    QList<QVariantMap> records;
    const int count = settings.beginReadArray(
        QString::fromLatin1(kRepositoriesSetting));
    records.reserve(qMax(0, count));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        QVariantMap record;
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys)
            record.insert(key, settings.value(key));
        records.append(record);
    }
    settings.endArray();
    return records;
}

QList<QVariantMap> configuredRepositories(
    const QList<QVariantMap> &current, const QString &owner,
    const QString &repository, const QString &source, const QString &mirror,
    const QString &branch, bool enabled)
{
    QList<QVariantMap> records = current;
    int match = -1;
    for (int index = 0; index < records.size(); ++index) {
        if (records.at(index).value(QStringLiteral("owner")).toString() ==
                owner &&
            records.at(index).value(QStringLiteral("name")).toString() ==
                repository) {
            match = index;
            break;
        }
    }
    QVariantMap configured{
        {QStringLiteral("owner"), owner},
        {QStringLiteral("name"), repository},
        {QStringLiteral("cloneUrl"), source},
        {QStringLiteral("localPath"), QString()},
        {QStringLiteral("mirrorPath"), mirror},
        {QStringLiteral("publishToNetwork"), false},
        {QStringLiteral("isPrivate"), false},
        {QStringLiteral("actionsEnabled"), enabled},
        {QStringLiteral("externallyManagedActions"), true},
        {QStringLiteral("externalActionsSource"), source},
        {QStringLiteral("externalActionsRef"),
         QStringLiteral("refs/heads/") + branch},
        {QStringLiteral("secretScanningEnabled"), true},
    };
    if (match >= 0) {
        for (auto it = configured.constBegin(); it != configured.constEnd();
             ++it)
            records[match].insert(it.key(), it.value());
    } else {
        records.append(configured);
    }
    return records;
}

void writeRepositories(QSettings &settings,
                       const QList<QVariantMap> &records)
{
    settings.remove(QString::fromLatin1(kRepositoriesSetting));
    settings.beginWriteArray(QString::fromLatin1(kRepositoriesSetting),
                             records.size());
    for (int index = 0; index < records.size(); ++index) {
        settings.setArrayIndex(index);
        const QVariantMap record = records.at(index);
        for (auto it = record.constBegin(); it != record.constEnd(); ++it)
            settings.setValue(it.key(), it.value());
    }
    settings.endArray();
}

const QStringList &configurationSettingKeys()
{
    static const QStringList keys{
        QString::fromLatin1(kEnabledSetting),
        QString::fromLatin1(kGenerationSetting),
        QString::fromLatin1(kNodeSetting),
        QStringLiteral("actions/mirrorRefreshConfigPath"),
        QStringLiteral("actions/mirrorStatePath"),
        QString::fromLatin1(kSummaryPathSetting),
        QStringLiteral("actions/mirrorConfiguredAt"),
        QStringLiteral("actions/variables"),
    };
    return keys;
}

struct ConfigurationSnapshot {
    QMap<QString, QVariant> repositoryValues;
    QMap<QString, QVariant> directValues;
    QSet<QString> presentDirectKeys;
};

bool operator==(const ConfigurationSnapshot &left,
                const ConfigurationSnapshot &right)
{
    return left.repositoryValues == right.repositoryValues &&
           left.directValues == right.directValues &&
           left.presentDirectKeys == right.presentDirectKeys;
}

ConfigurationSnapshot captureConfiguration(QSettings &settings)
{
    ConfigurationSnapshot snapshot;
    const QString repositoryPrefix =
        QString::fromLatin1(kRepositoriesSetting) + QLatin1Char('/');
    const QStringList allKeys = settings.allKeys();
    for (const QString &key : allKeys) {
        if (key.startsWith(repositoryPrefix))
            snapshot.repositoryValues.insert(key, settings.value(key));
    }
    for (const QString &key : configurationSettingKeys()) {
        if (settings.contains(key)) {
            snapshot.presentDirectKeys.insert(key);
            snapshot.directValues.insert(key, settings.value(key));
        }
    }
    return snapshot;
}

std::unique_ptr<QSettings> independentSettings(const QSettings &settings)
{
    if (settings.fileName().isEmpty())
        return {};
    return std::make_unique<QSettings>(settings.fileName(),
                                       settings.format());
}

bool durableConfigurationMatches(QSettings &settings,
                                 const ConfigurationSnapshot &expected)
{
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;
    std::unique_ptr<QSettings> verifier = independentSettings(settings);
    if (!verifier)
        return captureConfiguration(settings) == expected;
    verifier->sync();
    return verifier->status() == QSettings::NoError &&
           captureConfiguration(*verifier) == expected;
}

bool restoreConfiguration(QSettings &settings,
                          const ConfigurationSnapshot &snapshot)
{
    settings.remove(QString::fromLatin1(kRepositoriesSetting));
    for (auto it = snapshot.repositoryValues.constBegin();
         it != snapshot.repositoryValues.constEnd(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    for (const QString &key : configurationSettingKeys()) {
        if (snapshot.presentDirectKeys.contains(key))
            settings.setValue(key, snapshot.directValues.value(key));
        else
            settings.remove(key);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;
    const QString fileName = settings.fileName();
    if (!fileName.isEmpty() && QFileInfo(fileName).exists() &&
        !QFile::setPermissions(
            fileName,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    return durableConfigurationMatches(settings, snapshot);
}

QJsonObject encodeSnapshot(const ConfigurationSnapshot &snapshot, bool *ok)
{
    QJsonObject repositories;
    for (auto it = snapshot.repositoryValues.constBegin();
         it != snapshot.repositoryValues.constEnd(); ++it) {
        const QJsonObject encoded = encodeVariant(it.value(), ok);
        if (!*ok)
            return {};
        repositories.insert(it.key(), encoded);
    }
    QJsonObject direct;
    for (auto it = snapshot.directValues.constBegin();
         it != snapshot.directValues.constEnd(); ++it) {
        const QJsonObject encoded = encodeVariant(it.value(), ok);
        if (!*ok)
            return {};
        direct.insert(it.key(), encoded);
    }
    QJsonArray present;
    for (const QString &key : snapshot.presentDirectKeys)
        present.append(key);
    return QJsonObject{
        {QStringLiteral("repositoryValues"), repositories},
        {QStringLiteral("directValues"), direct},
        {QStringLiteral("presentDirectKeys"), present},
    };
}

bool decodeValueMap(const QJsonObject &object, const QString &prefix,
                    bool direct, QMap<QString, QVariant> *values)
{
    if (!values)
        return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const bool allowed =
            direct ? configurationSettingKeys().contains(it.key())
                   : it.key().startsWith(prefix);
        if (!allowed || !it.value().isObject())
            return false;
        bool ok = true;
        const QVariant value =
            decodeVariant(it.value().toObject(), &ok);
        if (!ok)
            return false;
        values->insert(it.key(), value);
    }
    return true;
}

bool decodeSnapshot(const QJsonObject &encoded,
                    ConfigurationSnapshot *snapshot)
{
    if (!snapshot ||
        !exactObjectFields(
            encoded,
            {QStringLiteral("repositoryValues"),
             QStringLiteral("directValues"),
             QStringLiteral("presentDirectKeys")}) ||
        !encoded.value(QStringLiteral("repositoryValues")).isObject() ||
        !encoded.value(QStringLiteral("directValues")).isObject() ||
        !encoded.value(QStringLiteral("presentDirectKeys")).isArray()) {
        return false;
    }
    ConfigurationSnapshot decoded;
    const QString repositoryPrefix =
        QString::fromLatin1(kRepositoriesSetting) + QLatin1Char('/');
    if (!decodeValueMap(
            encoded.value(QStringLiteral("repositoryValues")).toObject(),
            repositoryPrefix, false, &decoded.repositoryValues) ||
        !decodeValueMap(
            encoded.value(QStringLiteral("directValues")).toObject(),
            {}, true, &decoded.directValues)) {
        return false;
    }
    for (const QJsonValue &value :
         encoded.value(QStringLiteral("presentDirectKeys")).toArray()) {
        if (!value.isString() ||
            !configurationSettingKeys().contains(value.toString()) ||
            decoded.presentDirectKeys.contains(value.toString())) {
            return false;
        }
        decoded.presentDirectKeys.insert(value.toString());
    }
    if (decoded.presentDirectKeys.size() != decoded.directValues.size()) {
        return false;
    }
    for (auto it = decoded.directValues.constBegin();
         it != decoded.directValues.constEnd(); ++it) {
        if (!decoded.presentDirectKeys.contains(it.key()))
            return false;
    }
    *snapshot = decoded;
    return true;
}

struct ConfigurationJournal {
    QString requestId;
    ConfigurationSnapshot previous;
};

QJsonObject journalPayload(const ConfigurationJournal &journal, bool *ok)
{
    const QJsonObject previous = encodeSnapshot(journal.previous, ok);
    if (!*ok)
        return {};
    return QJsonObject{
        {QStringLiteral("type"), QString::fromLatin1(kJournalType)},
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("requestId"), journal.requestId},
        {QStringLiteral("previous"), previous},
    };
}

QByteArray encodeJournal(const ConfigurationJournal &journal)
{
    bool ok = true;
    QJsonObject payload = journalPayload(journal, &ok);
    if (!ok)
        return {};
    const QByteArray canonical =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    payload.insert(
        QStringLiteral("sha256"),
        QString::fromLatin1(
            QCryptographicHash::hash(canonical,
                                     QCryptographicHash::Sha256)
                .toHex()));
    const QByteArray encoded =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    return encoded.size() <= kMaximumJournalBytes ? encoded : QByteArray();
}

bool decodeJournal(const QByteArray &encoded,
                   ConfigurationJournal *journal)
{
    if (!journal || encoded.isEmpty() ||
        encoded.size() > kMaximumJournalBytes)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return false;
    }
    QJsonObject payload = document.object();
    if (!exactObjectFields(
            payload,
            {QStringLiteral("type"), QStringLiteral("schemaVersion"),
             QStringLiteral("requestId"), QStringLiteral("previous"),
             QStringLiteral("sha256")}) ||
        payload.value(QStringLiteral("type")).toString() !=
            QLatin1String(kJournalType) ||
        payload.value(QStringLiteral("schemaVersion")).toInt() != 2 ||
        !payload.value(QStringLiteral("previous")).isObject()) {
        return false;
    }
    const QString checksum =
        payload.take(QStringLiteral("sha256")).toString().toLower();
    static const QRegularExpression exactSha(
        QStringLiteral("^[a-f0-9]{64}$"));
    if (!exactSha.match(checksum).hasMatch())
        return false;
    const QByteArray canonical =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (QString::fromLatin1(
            QCryptographicHash::hash(canonical,
                                     QCryptographicHash::Sha256)
                .toHex()) != checksum) {
        return false;
    }
    const QString requestId =
        payload.value(QStringLiteral("requestId")).toString();
    ConfigurationSnapshot previous;
    if (!kRequestId.match(requestId).hasMatch() ||
        !decodeSnapshot(
            payload.value(QStringLiteral("previous")).toObject(),
            &previous)) {
        return false;
    }
    journal->requestId = requestId;
    journal->previous = previous;
    return true;
}

bool writeJournal(const QSettings &settings,
                  const ConfigurationJournal &journal)
{
    const QByteArray encoded = encodeJournal(journal);
    if (encoded.isEmpty())
        return false;
    const QString path = transactionJournalPath(settings);
    const QString parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent))
        return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(encoded) != encoded.size() || !file.commit() ||
        !QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        !syncDirectory(parent)) {
        return false;
    }
    QFile verify(path);
    if (!verify.open(QIODevice::ReadOnly) ||
        verify.size() <= 0 || verify.size() > kMaximumJournalBytes) {
        return false;
    }
    ConfigurationJournal decoded;
    return decodeJournal(verify.readAll(), &decoded) &&
           decoded.requestId == journal.requestId &&
           decoded.previous == journal.previous;
}

enum class JournalReadResult {
    Missing,
    Loaded,
    Invalid,
};

JournalReadResult readJournal(const QSettings &settings,
                              ConfigurationJournal *journal)
{
    const QString path = transactionJournalPath(settings);
    const QFileInfo info(path);
    if (!info.exists())
        return JournalReadResult::Missing;
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaximumJournalBytes) {
        return JournalReadResult::Invalid;
    }
#if defined(Q_OS_UNIX)
    // Windows reports every file as group/other readable unless NTFS
    // permission lookup is switched on, so the mode check is POSIX-only; the
    // journal there lives under the per-user profile ACL.
    if (info.permissions().testFlag(QFileDevice::ReadGroup) ||
        info.permissions().testFlag(QFileDevice::WriteGroup) ||
        info.permissions().testFlag(QFileDevice::ReadOther) ||
        info.permissions().testFlag(QFileDevice::WriteOther) ||
        info.ownerId() != static_cast<uint>(::geteuid()))
        return JournalReadResult::Invalid;
#endif
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return JournalReadResult::Invalid;
    return decodeJournal(file.readAll(), journal)
               ? JournalReadResult::Loaded
               : JournalReadResult::Invalid;
}

bool removeJournal(const QSettings &settings)
{
    const QString path = transactionJournalPath(settings);
    if (QFileInfo::exists(path) && !QFile::remove(path))
        return false;
    return !QFileInfo::exists(path) &&
           syncDirectory(QFileInfo(path).absolutePath());
}

bool restrictSettingsFile(QSettings &settings)
{
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;
    const QString fileName = settings.fileName();
    // A Windows registry key is protected by its own per-user ACL; there is
    // no file to restrict.
    if (fileName.isEmpty() ||
        !detail::settingsNameIsFilePath(fileName, settings.format()))
        return true;
    QFileInfo info(fileName);
    if (!info.exists())
        return false;
    return QFile::setPermissions(
        fileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}


bool recoverPendingLocked(QSettings &settings, QString *errorCode)
{
    if (errorCode)
        errorCode->clear();
    ConfigurationJournal journal;
    const JournalReadResult read = readJournal(settings, &journal);
    if (read == JournalReadResult::Missing)
        return true;
    if (read == JournalReadResult::Invalid) {
        if (errorCode)
            *errorCode = QStringLiteral("configuration_journal_invalid");
        return false;
    }

    // Restore the last durable local snapshot before accepting a new request.
    if (!restoreConfiguration(settings, journal.previous)) {
        if (errorCode)
            *errorCode =
                QStringLiteral("configuration_local_rollback_failed");
        return false;
    }

    if (!removeJournal(settings)) {
        if (errorCode)
            *errorCode =
                QStringLiteral("configuration_journal_cleanup_failed");
        return false;
    }
    return true;
}

} // namespace

QString configurationRecoveryJournalPath(const QSettings &settings)
{
    return transactionJournalPath(settings);
}

bool recoverPendingConfiguration(QSettings &settings, QString *errorCode)
{
    if (!prepareTransactionDirectory(settings, errorCode))
        return false;
    QLockFile lock(transactionLockPath(settings));
    lock.setStaleLockTime(30 * 1000);
    if (!lock.tryLock(g_configurationLockTimeoutMs)) {
        if (errorCode)
            *errorCode =
                lock.error() == QLockFile::LockFailedError
                    ? QStringLiteral("configuration_busy")
                    : QStringLiteral("configuration_lock_unavailable");
        return false;
    }
    return recoverPendingLocked(settings, errorCode);
}

namespace {
StartupRecoveryStatus g_startupRecovery;
} // namespace

QString configurationRecoveryLockPath(const QSettings &settings)
{
    return transactionLockPath(settings);
}

StartupRecoveryStatus recoverAtStartup(QSettings &settings)
{
    StartupRecoveryStatus status;
    status.attempted = true;
    QString errorCode;
    status.ok = recoverPendingConfiguration(settings, &errorCode);
    if (!status.ok) {
        status.errorCode = errorCode.isEmpty()
                               ? QStringLiteral("configuration_recovery_failed")
                               : errorCode;
        status.lockPath = transactionLockPath(settings);
        status.journalPath = transactionJournalPath(settings);
    }
    g_startupRecovery = status;
    return status;
}

StartupRecoveryStatus startupRecoveryStatus()
{
    return g_startupRecovery;
}

bool actionsSuspendedByRecoveryFailure()
{
    return g_startupRecovery.attempted && !g_startupRecovery.ok;
}

QStringList resetConfigurationRecoveryFiles(const QSettings &settings,
                                            QString *errorCode)
{
    if (errorCode)
        errorCode->clear();
    QStringList removed;
    const auto removeExact = [&removed](const QString &path) -> bool {
        if (path.isEmpty())
            return true;
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink())
            return true;
        // Only the exact regular file (or a dangling link) by name; never a
        // directory, never anything recursive.
        if (info.isDir() && !info.isSymLink())
            return false;
        if (!QFile::remove(path))
            return false;
        removed << path;
        return true;
    };

    const QString lockPath = transactionLockPath(settings);
    const QString journalPath = transactionJournalPath(settings);
    bool ok = true;
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30 * 1000);
    if (lock.tryLock(0)) {
        // Nobody holds it (a stale lock was already cleared by tryLock).
        ok = removeExact(journalPath);
        lock.unlock();
    } else {
        qint64 pid = 0;
        QString host, application;
        if (lock.error() == QLockFile::LockFailedError &&
            lock.getLockInfo(&pid, &host, &application) && pid > 0) {
            // A live helper holds a readable lock: never pull it away.
            if (errorCode)
                *errorCode = QStringLiteral("configuration_busy");
            return removed;
        }
        // Unreadable, unparseable or uncreatable: exactly the stuck state
        // reset exists for.
        ok = removeExact(journalPath) && removeExact(lockPath);
    }
    const QString legacyBase = legacyTransactionBasePath(settings);
    if (!legacyBase.isEmpty() && legacyBase != transactionBasePath(settings)) {
        ok = removeExact(legacyBase + QStringLiteral(".json")) && ok;
        ok = removeExact(legacyBase + QStringLiteral(".lock")) && ok;
    }
    if (!ok && errorCode)
        *errorCode = QStringLiteral("configuration_reset_failed");
    return removed;
}

namespace detail {

bool settingsNameIsFilePath(const QString &fileName, QSettings::Format format)
{
    return !forkmesh::paths::settingsFilePathFor(fileName, format).isEmpty();
}

QString transactionDirectoryFor(const QString &settingsFileName,
                                QSettings::Format format,
                                const QString &dataRoot)
{
    const QString folder =
        forkmesh::paths::settingsFolderFor(settingsFileName, format);
    return folder.isEmpty() ? QDir(dataRoot).filePath(QStringLiteral("actions"))
                            : folder;
}

void setConfigurationLockTimeoutMsForTesting(int timeoutMs)
{
    g_configurationLockTimeoutMs =
        timeoutMs > 0 ? timeoutMs : kDefaultConfigurationLockTimeoutMs;
}

void resetStartupRecoveryStatusForTesting()
{
    g_startupRecovery = {};
}

} // namespace detail

QJsonObject applyConfiguration(const QJsonObject &request,
                               QSettings &settings)
{
    const QString requestId = safeRequestId(request);
    const QString node = safeNode(request);
    const bool requestedEnabled =
        request.value(QStringLiteral("actionsEnabled")).toBool(false);
    const QSet<QString> required{
        QStringLiteral("type"), QStringLiteral("schemaVersion"),
        QStringLiteral("requestId"), QStringLiteral("node"),
        QStringLiteral("actionsEnabled")};
    const QSet<QString> optional{QStringLiteral("variables")};
    if (!exactFields(request, required, optional) ||
        request.value(QStringLiteral("type")).toString() !=
            QLatin1String("forkmesh.mirror-actions-configuration") ||
        request.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        node.isEmpty() ||
        !request.value(QStringLiteral("actionsEnabled")).isBool()) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("invalid_request"));
    }

    const QString machine =
        settings.value(QStringLiteral("node/machineName")).toString().trimmed();
    const QString account =
        settings.value(QStringLiteral("account/nodeName")).toString().trimmed();
    const QString localNode = !machine.isEmpty() ? machine : account;
    if (!kNodeName.match(localNode).hasMatch() || localNode != node) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("node_mismatch"));
    }

    QString lockDirectoryError;
    if (!prepareTransactionDirectory(settings, &lockDirectoryError)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      lockDirectoryError);
    }
    QLockFile lock(transactionLockPath(settings));
    lock.setStaleLockTime(30 * 1000);
    if (!lock.tryLock(g_configurationLockTimeoutMs)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      lock.error() == QLockFile::LockFailedError
                          ? QStringLiteral("configuration_busy")
                          : QStringLiteral(
                                "configuration_lock_unavailable"));
    }
    QString recoveryError;
    if (!recoverPendingLocked(settings, &recoveryError)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      recoveryError.isEmpty()
                          ? QStringLiteral("configuration_recovery_failed")
                          : recoveryError);
    }

    if (!restrictSettingsFile(settings)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("settings_write_failed"));
    }

    bool replaceVariables = false;
    QMap<QString, QString> variables;
    if (request.contains(QStringLiteral("variables"))) {
        const QJsonObject envelope =
            request.value(QStringLiteral("variables")).toObject();
        if (!request.value(QStringLiteral("variables")).isObject() ||
            !exactFields(
                envelope,
                {QStringLiteral("mode"), QStringLiteral("values")}) ||
            envelope.value(QStringLiteral("mode")).toString() !=
                QLatin1String("replace") ||
            !envelope.value(QStringLiteral("values")).isObject()) {
            return result(requestId, node, requestedEnabled, false, 0, false,
                          QStringLiteral("invalid_variables"));
        }
        const QJsonObject values =
            envelope.value(QStringLiteral("values")).toObject();
        if (values.size() > kMaximumVariables) {
            return result(requestId, node, requestedEnabled, false, 0, false,
                          QStringLiteral("invalid_variables"));
        }
        qsizetype totalBytes = 0;
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            if (!kVariableName.match(it.key()).hasMatch() ||
                !it.value().isString()) {
                return result(requestId, node, requestedEnabled, false, 0,
                              false,
                              QStringLiteral("invalid_variables"));
            }
            const QString value = it.value().toString();
            const qsizetype bytes = value.toUtf8().size();
            totalBytes += bytes;
            if (value.contains(QChar::Null) ||
                bytes > kMaximumVariableValueBytes ||
                totalBytes > kMaximumVariableBytes) {
                return result(requestId, node, requestedEnabled, false, 0,
                              false,
                              QStringLiteral("invalid_variables"));
            }
            variables.insert(it.key(), value);
        }
        replaceVariables = true;
    }

    QString gatewayPath;
    const QJsonObject gateway = readGatewayConfiguration(&gatewayPath);
    const QString source =
        gateway.value(QStringLiteral("sourceRepository")).toString().trimmed();
    const QString repository =
        gateway.value(QStringLiteral("repositoryName")).toString().trimmed();
    const QString owner = repositoryOwner(gateway);
    QString branch = gateway.value(QStringLiteral("catalog"))
                         .toObject()
                         .value(QStringLiteral("branch"))
                         .toString()
                         .trimmed();
    if (branch.isEmpty())
        branch = QStringLiteral("main");
    if (gateway.isEmpty() ||
        gateway.value(QStringLiteral("nodeOwner")).toString().trimmed() !=
            node ||
        owner.isEmpty() ||
        !kRepositoryName.match(repository).hasMatch() ||
        !kBranchName.match(branch).hasMatch() ||
        !safeAbsolutePath(source)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("mirror_config_unavailable"));
    }
    const QString mirror =
        actionMirrorPath(settings, node, owner, repository);
    if (!prepareActionMirror(source, mirror)) {
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("actions_mirror_unavailable"));
    }

    // Everything below is staged in memory first. The long-running managed
    // mirror daemon observes the committed local generation and owns any
    // subsequent catalog publication.
    const QList<QVariantMap> repositories =
        configuredRepositories(readRepositories(settings), owner, repository,
                               source, mirror, branch, requestedEnabled);
    const ConfigurationSnapshot previous =
        captureConfiguration(settings);
    const auto clearVariables = [&] {
        for (QString &value : variables)
            value.fill(QChar::Null);
        variables.clear();
    };
    const int variableCount = replaceVariables ? variables.size() : 0;
    const ConfigurationJournal journal{
        requestId,
        previous,
    };
    if (!writeJournal(settings, journal)) {
        clearVariables();
        return result(requestId, node, requestedEnabled, false, 0, false,
                      QStringLiteral("configuration_journal_write_failed"));
    }

    writeRepositories(settings, repositories);
    settings.setValue(QString::fromLatin1(kEnabledSetting), requestedEnabled);
    settings.setValue(QString::fromLatin1(kNodeSetting), node);
    settings.setValue(QStringLiteral("actions/mirrorRefreshConfigPath"),
                      gatewayPath);
    settings.setValue(QStringLiteral("actions/mirrorStatePath"),
                      QDir(QFileInfo(gatewayPath).absolutePath())
                          .filePath(QStringLiteral("actions-state.json")));
    settings.setValue(
        QString::fromLatin1(kSummaryPathSetting),
        QDir(QFileInfo(gatewayPath).absolutePath())
            .filePath(QStringLiteral("actions-summary.json")));
    settings.setValue(QStringLiteral("actions/mirrorConfiguredAt"),
                      QDateTime::currentMSecsSinceEpoch());
    QByteArray encodedVariables;
    if (replaceVariables) {
        QJsonObject object;
        for (auto it = variables.constBegin(); it != variables.constEnd();
             ++it)
            object.insert(it.key(), it.value());
        encodedVariables =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        settings.setValue(
            QStringLiteral("actions/variables"),
            encodedVariables);
    }
    settings.setValue(QString::fromLatin1(kGenerationSetting), requestId);

    const ConfigurationSnapshot committed =
        captureConfiguration(settings);
    clearVariables();
    encodedVariables.fill('\0');
    encodedVariables.clear();
    if (!restrictSettingsFile(settings) ||
        !durableConfigurationMatches(settings, committed)) {
        QString rollbackError;
        const bool rolledBack =
            recoverPendingLocked(settings, &rollbackError);
        return result(requestId, node, requestedEnabled, false, 0, false,
                      rolledBack
                          ? QStringLiteral("settings_write_failed")
                          : (rollbackError.isEmpty()
                                 ? QStringLiteral(
                                       "configuration_recovery_failed")
                                 : rollbackError));
    }
    if (!removeJournal(settings)) {
        QString rollbackError;
        const bool rolledBack =
            recoverPendingLocked(settings, &rollbackError);
        return result(
            requestId, node, requestedEnabled, false, 0, false,
            rolledBack
                ? QStringLiteral("configuration_finalize_failed")
                : (rollbackError.isEmpty()
                       ? QStringLiteral("configuration_recovery_failed")
                       : rollbackError));
    }
    return result(requestId, node, requestedEnabled, replaceVariables,
                  variableCount,
                  true);
}

namespace {
ConfigurationDelegate g_configurationDelegate;
} // namespace

void setConfigurationDelegate(ConfigurationDelegate delegate)
{
    g_configurationDelegate = std::move(delegate);
}

QJsonObject refusedConfiguration(const QJsonObject &request,
                                 const QString &errorCode)
{
    return result(safeRequestId(request), safeNode(request),
                  request.value(QStringLiteral("actionsEnabled")).toBool(false),
                  false, 0, false, errorCode);
}

int runConfigurationStdin(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ForkMesh"));
    app.setOrganizationName(QStringLiteral("ForkMesh"));
    forkmesh::paths::activate(QCoreApplication::arguments());
    QByteArray input;
    char buffer[4096];
    while (!std::feof(stdin) && input.size() <= kMaximumRequestBytes) {
        const std::size_t count =
            std::fread(buffer, 1, sizeof(buffer), stdin);
        if (count > 0)
            input.append(buffer, int(count));
        if (std::ferror(stdin))
            break;
    }
    QJsonObject request;
    if (!input.isEmpty() && input.size() <= kMaximumRequestBytes) {
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(input, &parseError);
        if (parseError.error == QJsonParseError::NoError &&
            document.isObject())
            request = document.object();
    }
    input.fill('\0');
    input.clear();
    QJsonObject response;
    if (!g_configurationDelegate || !g_configurationDelegate(request, &response)) {
        QSettings settings;
        response = applyConfiguration(request, settings);
    }
    QByteArray encoded =
        QJsonDocument(response)
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals);
    std::fputs("FORKMESH_ACTIONS_RESULT=", stdout);
    std::fwrite(encoded.constData(), 1, std::size_t(encoded.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    const bool ok = response.value(QStringLiteral("ok")).toBool();
    encoded.fill('\0');
    return ok ? 0 : 2;
}

} // namespace forkmesh::mirror_actions
