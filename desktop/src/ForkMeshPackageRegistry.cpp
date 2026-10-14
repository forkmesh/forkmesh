#include "ForkMeshPackageRegistry.h"
#include "AppPaths.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUrl>

#include <functional>

namespace forkmesh::market {
namespace {

bool validPackageId(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$"));
    return id.size() <= 96 && pattern.match(id).hasMatch();
}

bool validTaxonomyId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$"));
    return value.size() <= 96 && pattern.match(value).hasMatch();
}

bool validHostEntrypoint(const QString &value)
{
    static const QRegularExpression builtin(
        QStringLiteral("^builtin:[a-z][a-z0-9.-]{0,95}$"));
    const QStringList parts = value.split(QLatin1Char('/'));
    return builtin.match(value).hasMatch() ||
           (!value.isEmpty() && value.size() <= 160 &&
            !value.startsWith(QLatin1Char('/')) &&
            !value.startsWith(QLatin1Char(':')) &&
            !value.contains(QLatin1Char('\\')) &&
            !parts.contains(QStringLiteral(".")) &&
            !parts.contains(QStringLiteral("..")));
}

bool validVersion(const QString &version)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$"));
    return version.size() <= 80 && pattern.match(version).hasMatch();
}

bool matches(const QString &value, const char *expression, int maxLength)
{
    const QRegularExpression pattern(QString::fromLatin1(expression));
    return !value.isEmpty() && value.size() <= maxLength &&
           pattern.match(value).hasMatch();
}

bool validThemeColor(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return pattern.match(value).hasMatch();
}

bool validAgentModelId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/-]*$"));
    return value.size() <= 128 && pattern.match(value).hasMatch();
}

bool validLoopbackEndpoint(const QString &value)
{
    const QUrl url(value);
    const QString host = url.host().toLower();
    return url.isValid() && url.scheme() == QLatin1String("http") &&
           (host == QLatin1String("127.0.0.1") ||
            host == QLatin1String("localhost") || host == QLatin1String("::1")) &&
           url.path() == QLatin1String("/v1/chat/completions") &&
           url.query().isEmpty() && url.fragment().isEmpty();
}

bool validHttpsUrl(const QString &value)
{
    const QUrl url(value);
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() &&
           url.fragment().isEmpty();
}

bool validPackageSettingKey(const QString &key)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z][A-Za-z0-9.-]{0,63}$"));
    return pattern.match(key).hasMatch();
}

constexpr auto kPackageDataFormat = "forkmesh-package-data";
constexpr int kCorePluginsCutoffYear = 2026;
constexpr int kCorePluginsCutoffMonth = 10;
constexpr int kCorePluginsCutoffDay = 11;
constexpr int kPackageDataFormatVersion = 1;
constexpr int kPackageDataMaxKeys = 256;

// QSettings hands back strings, numbers, booleans, lists and raw bytes. Bytes
// have no JSON form, so they travel as {"$bytes": base64} and come back as
// QByteArray; everything else maps onto JSON directly.
QJsonValue packageDataToJson(const QVariant &value)
{
    if (value.typeId() == QMetaType::QByteArray) {
        return QJsonObject{{QStringLiteral("$bytes"),
                            QString::fromLatin1(value.toByteArray().toBase64())}};
    }
    return QJsonValue::fromVariant(value);
}

QVariant packageDataFromJson(const QJsonValue &value)
{
    const QJsonObject object = value.toObject();
    if (value.isObject() && object.size() == 1 &&
        object.value(QStringLiteral("$bytes")).isString()) {
        return QByteArray::fromBase64(
            object.value(QStringLiteral("$bytes")).toString().toLatin1());
    }
    return value.toVariant();
}

QString jsonString(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

QString catalogChildPath(const QString &catalogPath, const QString &relative)
{
    if (relative.isEmpty() || relative.startsWith(QLatin1Char('/')) ||
        relative.startsWith(QLatin1Char(':')) ||
        relative.contains(QLatin1Char('\\')) ||
        relative.split(QLatin1Char('/')).contains(QStringLiteral("..")) ||
        relative.split(QLatin1Char('/')).contains(QStringLiteral(".")))
        return {};
    const int slash = catalogPath.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return relative;
    return catalogPath.left(slash + 1) + relative;
}

bool readJsonObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not open %1: %2")
                         .arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid JSON in %1: %2")
                         .arg(path, parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

} // namespace

bool PackageAgentContribution::isValid() const
{
    if (provider != QLatin1String("local-llama") ||
        runtime != QLatin1String("ollama") ||
        !validLoopbackEndpoint(endpoint) || !validAgentModelId(defaultModel) ||
        avatarPath.isEmpty() || models.isEmpty() ||
        !validHttpsUrl(documentationUrl) ||
        !validHttpsUrl(runtimeDownloadUrl)) {
        return false;
    }
    QSet<QString> ids;
    bool hasDefault = false;
    for (const PackageAgentModel &candidate : models) {
        if (!validAgentModelId(candidate.id) || candidate.label.isEmpty() ||
            candidate.label.size() > 80 || candidate.downloadBytes <= 0 ||
            candidate.contextTokens < 1024 || ids.contains(candidate.id)) {
            return false;
        }
        ids.insert(candidate.id);
        hasDefault = hasDefault || candidate.id == defaultModel;
    }
    return hasDefault;
}

const PackageAgentModel *PackageAgentContribution::model(const QString &id) const
{
    for (const PackageAgentModel &candidate : models) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

bool PackageSettingField::accepts(const QVariant &value) const
{
    if (!value.isValid())
        return false;
    if (type == QLatin1String("boolean")) {
        if (value.typeId() == QMetaType::Bool)
            return true;
        const QString text = value.toString();
        return value.typeId() == QMetaType::QString &&
               (text == QLatin1String("true") || text == QLatin1String("false"));
    }
    if (type == QLatin1String("choice")) {
        if (value.typeId() != QMetaType::QString)
            return false;
        const QString text = value.toString();
        for (const PackageSettingOption &option : options) {
            if (option.value == text)
                return true;
        }
    }
    return false;
}

bool PackageStatusMonitor::covers(const QString &systemId) const
{
    if (systemId.isEmpty())
        return false;
    if (systems.contains(systemId))
        return true;
    for (const QString &prefix : prefixes) {
        if (systemId.startsWith(prefix))
            return true;
    }
    return false;
}

const PackageSettingField *PackageManifest::setting(const QString &key) const
{
    for (const PackageSettingField &field : settings) {
        if (field.key == key)
            return &field;
    }
    return nullptr;
}

bool PackageManifest::isValid() const
{
    return validPackageId(marketplaceId) && validPackageId(id) &&
           !name.isEmpty() && name.size() <= 80 &&
           !creatorId.isEmpty() && creatorId.size() <= 160 &&
           creatorId.startsWith(QLatin1String("did:forkmesh:")) &&
           matches(creatorHandle, "^[a-z0-9][a-z0-9-]*$", 80) &&
           !creatorName.isEmpty() && creatorName.size() <= 80 &&
           validTaxonomyId(type) && validVersion(version) &&
           !summary.isEmpty() && summary.size() <= 240 &&
           validTaxonomyId(licenseModel) &&
           !licenseSpdx.isEmpty() && licenseSpdx.size() <= 64 &&
           creatorRetainsOwnership &&
           !entrypoints.isEmpty() && priceAmount >= 0.0 &&
           validHostEntrypoint(settingsEntrypoint) &&
           matches(currency, "^[A-Z]{3}$", 3) && !screenshots.isEmpty();
}

QString PackageManifest::priceLabel() const
{
    if (priceAmount <= 0.0)
        return QStringLiteral("Free");
    return QStringLiteral("%1 %2")
        .arg(currency, QString::number(priceAmount, 'f', 2));
}

PackageRegistry::PackageRegistry(QSettings *settings, const QString &catalogPath)
    : m_settings(settings)
{
    if (!m_settings) {
        m_ownedSettings = std::make_unique<QSettings>();
        m_settings = m_ownedSettings.get();
    }
    if (forkmesh::paths::isActive() &&
        !forkmesh::paths::layout().isHomeProject())
        m_projectCreated =
            readProjectCreated(forkmesh::paths::layout().projectDir);
    if (!loadCatalog(catalogPath))
        return;
    loadOnboarding(catalogPath);
    // The package manager writes this overlay atomically after verifying an
    // external archive and running its up migrations. No source-specific path
    // is compiled into the application.
    if (catalogPath == QLatin1String(":/forkmesh-market/catalog.json")) {
        const QString installed =
            forkmesh::paths::dataRoot() +
            QStringLiteral("/marketplace/catalog.json");
        if (QFile::exists(installed) && !loadCatalog(installed, true)) {
            // A damaged optional overlay must not hide the verified catalog
            // embedded in the app. Keep the overlay error visible as a
            // warning while leaving the bundled packages usable.
            m_warnings.append(m_error);
            m_error.clear();
        }
    }
}

PackageRegistry::~PackageRegistry() = default;

const PackageManifest *PackageRegistry::package(const QString &id) const
{
    for (const PackageManifest &manifest : m_packages) {
        if (manifest.id == id)
            return &manifest;
    }
    return nullptr;
}

QString PackageRegistry::statePrefix(const QString &id) const
{
    return QStringLiteral("market/packages/%1").arg(id);
}

QString PackageRegistry::packageDataPrefix(const QString &id) const
{
    return statePrefix(id) + QStringLiteral("/data");
}

QString PackageRegistry::libraryReceiptPrefix(const QString &id) const
{
    return QStringLiteral("market/library/%1").arg(id);
}

QVariant PackageRegistry::packageSetting(const QString &id, const QString &key,
                                         const QVariant &fallback) const
{
    if (!package(id) || !validPackageSettingKey(key))
        return fallback;
    return m_settings->value(packageDataPrefix(id) + QLatin1Char('/') + key,
                             fallback);
}

bool PackageRegistry::setPackageSetting(const QString &id, const QString &key,
                                        const QVariant &value, QString *error)
{
    if (error)
        error->clear();
    const PackageManifest *manifest = package(id);
    if (!manifest || !validPackageSettingKey(key)) {
        if (error)
            *error = QStringLiteral("Invalid package setting for %1.").arg(id);
        return false;
    }
    m_settings->setValue(packageDataPrefix(id) + QLatin1Char('/') + key, value);
    m_settings->sync();
    if (m_settings->status() == QSettings::NoError)
        return true;
    if (error)
        *error = QStringLiteral("Could not save the %1 package settings.")
                     .arg(manifest->name);
    return false;
}

bool PackageRegistry::hasPackageData(const QString &id) const
{
    if (!package(id))
        return false;
    m_settings->beginGroup(packageDataPrefix(id));
    const bool any = !m_settings->childKeys().isEmpty();
    m_settings->endGroup();
    return any;
}

QJsonObject PackageRegistry::exportPackageData(const QString &id) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest)
        return {};
    QJsonObject data;
    m_settings->beginGroup(packageDataPrefix(id));
    for (const QString &key : m_settings->childKeys()) {
        if (validPackageSettingKey(key))
            data.insert(key, packageDataToJson(m_settings->value(key)));
    }
    m_settings->endGroup();
    return QJsonObject{
        {QStringLiteral("format"), QLatin1String(kPackageDataFormat)},
        {QStringLiteral("formatVersion"), kPackageDataFormatVersion},
        {QStringLiteral("packageId"), manifest->id},
        {QStringLiteral("packageName"), manifest->name},
        {QStringLiteral("packageVersion"), manifest->version},
        {QStringLiteral("exportedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("data"), data},
    };
}

bool PackageRegistry::importPackageData(const QString &id,
                                        const QJsonObject &bundle,
                                        QString *error)
{
    if (error)
        error->clear();
    const PackageManifest *manifest = package(id);
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!manifest)
        return fail(QStringLiteral("Unknown package: %1").arg(id));
    if (bundle.value(QStringLiteral("format")).toString() !=
            QLatin1String(kPackageDataFormat) ||
        bundle.value(QStringLiteral("formatVersion")).toInt() !=
            kPackageDataFormatVersion)
        return fail(QStringLiteral(
            "This file is not a ForkMesh package data export."));
    const QString bundlePackage =
        bundle.value(QStringLiteral("packageId")).toString();
    if (bundlePackage != id) {
        return fail(QStringLiteral("This file holds data for %1, not %2.")
                        .arg(bundlePackage.isEmpty()
                                 ? QStringLiteral("another package")
                                 : bundlePackage,
                             manifest->name));
    }
    const QJsonValue dataValue = bundle.value(QStringLiteral("data"));
    const QJsonObject data = dataValue.toObject();
    if (!dataValue.isObject() || data.size() > kPackageDataMaxKeys)
        return fail(QStringLiteral("The %1 data in this file is damaged.")
                        .arg(manifest->name));
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (!validPackageSettingKey(it.key()))
            return fail(QStringLiteral("The %1 data in this file is damaged.")
                            .arg(manifest->name));
    }

    // Replace rather than merge: the file is the whole saved state, and a
    // leftover key from a fresh start would contradict it.
    const QString prefix = packageDataPrefix(id);
    m_settings->remove(prefix);
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        m_settings->setValue(prefix + QLatin1Char('/') + it.key(),
                             packageDataFromJson(it.value()));
    }
    m_settings->sync();
    if (m_settings->status() == QSettings::NoError)
        return true;
    return fail(QStringLiteral("Could not save the imported %1 data.")
                    .arg(manifest->name));
}

void PackageRegistry::clearPackageData(const QString &id)
{
    if (!package(id))
        return;
    m_settings->remove(packageDataPrefix(id));
    m_settings->sync();
}

QDateTime PackageRegistry::readProjectCreated(const QString &projectDir)
{
    if (projectDir.isEmpty())
        return {};
    QJsonObject object;
    if (!readJsonObject(projectDir + QLatin1Char('/') +
                            QLatin1String(forkmesh::paths::kProjectFileName),
                        &object, nullptr))
        return {};
    const QDateTime created = QDateTime::fromString(
        object.value(QStringLiteral("created")).toString().trimmed(),
        Qt::ISODate);
    return created.isValid() ? created.toUTC() : QDateTime();
}

void PackageRegistry::setProjectCreated(const QDateTime &created)
{
    m_projectCreated = created.isValid() ? created.toUTC() : QDateTime();
}

bool PackageRegistry::projectCreatedBefore(const QDate &date) const
{
    if (!date.isValid())
        return false;
    if (!m_projectCreated.isValid())
        return true;
    return m_projectCreated < QDateTime(date, QTime(0, 0), QTimeZone::UTC);
}

bool PackageRegistry::isLegacyProject() const
{
    return projectCreatedBefore(QDate(kCorePluginsCutoffYear,
                                      kCorePluginsCutoffMonth,
                                      kCorePluginsCutoffDay));
}

bool PackageRegistry::legacyEntryInstalled(
    const PackageManifest &manifest) const
{
    return installedWithoutChoice(manifest) ||
           (manifest.bundled && isLegacyProject());
}

bool PackageRegistry::projectOwnsInstalls() const
{
    return !isLegacyProject();
}

bool PackageRegistry::hasStoredInstall(const QString &id) const
{
    const QString key = statePrefix(id) + QStringLiteral("/state");
    return m_settings->value(key).toString() == QLatin1String("installed");
}

bool PackageRegistry::accountAnswerInstalls(const PackageManifest &manifest,
                                            bool accountInstalled,
                                            bool legacyEntry,
                                            bool unchosen) const
{
    if (unchosen && hasStoredInstall(manifest.id))
        return true;
    if (!projectOwnsInstalls())
        return legacyEntry ? legacyEntryInstalled(manifest) : accountInstalled;
    if (legacyEntry)
        return isInstalled(manifest.id);
    return accountInstalled && isInstalled(manifest.id);
}

const QStringList &PackageRegistry::allPlaces()
{
    static const QStringList places{QStringLiteral("mobile"),
                                    QStringLiteral("desktop"),
                                    QStringLiteral("web")};
    return places;
}

QString PackageRegistry::desktopPlace()
{
    return QStringLiteral("desktop");
}

QStringList PackageRegistry::places(const QString &id) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest)
        return {};
    const QString key = statePrefix(id) + QStringLiteral("/places");
    if (!manifest->accountSync || !m_settings->contains(key))
        return manifest->places;
    const QStringList stored = m_settings->value(key).toStringList();
    QStringList chosen;
    for (const QString &place : manifest->places) {
        if (stored.contains(place))
            chosen.append(place);
    }
    return chosen;
}

bool PackageRegistry::setPlaces(const QString &id, const QStringList &places)
{
    const PackageManifest *manifest = package(id);
    if (!manifest || !manifest->accountSync)
        return false;
    QStringList chosen;
    for (const QString &place : allPlaces()) {
        if (places.contains(place))
            chosen.append(place);
    }
    const QString key = statePrefix(id) + QStringLiteral("/places");
    const QStringList before = this->places(id);
    if (chosen.isEmpty())
        m_settings->remove(key);
    else
        m_settings->setValue(key, chosen);
    m_settings->sync();
    return this->places(id) != before;
}

bool PackageRegistry::isPlacedHere(const QString &id) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest || !manifest->accountSync)
        return true;
    return places(id).contains(desktopPlace());
}

bool PackageRegistry::isInstalledHere(const QString &id) const
{
    return isInstalled(id) && isPlacedHere(id);
}

bool PackageRegistry::isShownHere(const QString &id) const
{
    return isEffectivelyInstalled(id) && isPlacedHere(id);
}

bool PackageRegistry::installedWithoutChoice(
    const PackageManifest &manifest) const
{
    if (manifest.managedExternally)
        return true;
    if (!manifest.bundled)
        return false;
    return manifest.installedByDefault ||
           projectCreatedBefore(manifest.installedBefore);
}

bool PackageRegistry::isInstalled(const QString &id) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest)
        return false;
    const QString key = statePrefix(id) + QStringLiteral("/state");
    if (!m_settings->contains(key))
        return installedWithoutChoice(*manifest);
    const QString state = m_settings->value(key).toString();
    if (state == QLatin1String("installed"))
        return true;
    // A malformed state must not silently reactivate executable code.
    return false;
}

bool PackageRegistry::effectivelyInstalled(const QString &id,
                                           QSet<QString> *visiting) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest || visiting->contains(id) || !isInstalled(id))
        return false;
    visiting->insert(id);
    for (const QString &dependency : manifest->dependencies) {
        if (!effectivelyInstalled(dependency, visiting))
            return false;
    }
    visiting->remove(id);
    return true;
}

bool PackageRegistry::isEffectivelyInstalled(const QString &id) const
{
    QSet<QString> visiting;
    return effectivelyInstalled(id, &visiting);
}

QStringList PackageRegistry::missingDependencies(const QString &id,
                                                 QString *error) const
{
    if (error)
        error->clear();
    QStringList missing;
    QSet<QString> visited;
    QString failure;
    std::function<void(const QString &)> visit = [&](const QString &current) {
        if (!failure.isEmpty() || visited.contains(current))
            return;
        visited.insert(current);
        const PackageManifest *manifest = package(current);
        if (!manifest)
            return;
        for (const QString &dependency : manifest->dependencies) {
            const PackageManifest *required = package(dependency);
            if (!required) {
                failure = QStringLiteral("%1 needs %2, which is not in the "
                                         "package catalog.")
                              .arg(manifest->name, dependency);
                return;
            }
            visit(dependency);
            if (!isInstalled(dependency) && !missing.contains(dependency))
                missing.append(dependency);
        }
    };
    visit(id);
    missing.removeAll(id);
    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        return {};
    }
    return missing;
}

QStringList PackageRegistry::installedDependents(const QString &id) const
{
    QStringList dependents;
    QStringList queue{id};
    while (!queue.isEmpty()) {
        const QString current = queue.takeFirst();
        for (const PackageManifest &candidate : m_packages) {
            if (candidate.id == id || dependents.contains(candidate.id) ||
                !candidate.dependencies.contains(current) ||
                !isInstalled(candidate.id))
                continue;
            dependents.append(candidate.id);
            queue.append(candidate.id);
        }
    }
    return dependents;
}

QString PackageRegistry::marketParent(const QString &id) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest || manifest->dependencies.isEmpty())
        return {};
    const QString parent = manifest->dependencies.first();
    return parent != id && package(parent) ? parent : QString();
}

QStringList PackageRegistry::nestedPackages(const QString &id) const
{
    QStringList nested;
    for (const PackageManifest &candidate : m_packages) {
        if (marketParent(candidate.id) == id)
            nested.append(candidate.id);
    }
    return nested;
}

bool PackageRegistry::installableHere(const PackageManifest &manifest)
{
    const bool theme = manifest.type == QLatin1String("theme") &&
                       manifest.entrypoints.contains(QStringLiteral("theme"));
    return theme || manifest.accountSync ||
           manifest.entrypoints.contains(QStringLiteral("desktop"));
}

const PackagePack *PackageRegistry::pack(const QString &id) const
{
    for (const PackagePack &candidate : m_packs) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

const PackageGuide *PackageRegistry::guide(const QString &id) const
{
    for (const PackageGuide &candidate : m_guides) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

QStringList PackageRegistry::packPackages(const QString &packId) const
{
    const PackagePack *found = pack(packId);
    if (!found)
        return {};
    QStringList ids;
    const auto bundleable = [](const PackageManifest &manifest) {
        return installableHere(manifest) && !manifest.agent.isValid() &&
               manifest.type != QLatin1String("agent") &&
               manifest.type != QLatin1String("theme");
    };
    if (found->everything) {
        for (const PackageManifest &manifest : m_packages) {
            if (marketParent(manifest.id).isEmpty() && bundleable(manifest))
                ids.append(manifest.id);
        }
        return ids;
    }
    for (const QString &id : found->packages) {
        const PackageManifest *manifest = package(id);
        if (manifest && bundleable(*manifest) && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

QStringList PackageRegistry::guidePackages(const QString &guideId) const
{
    const PackageGuide *found = guide(guideId);
    if (!found)
        return {};
    QStringList ids = packPackages(found->pack);
    for (const QString &id : found->packages) {
        const PackageManifest *manifest = package(id);
        if (manifest && installableHere(*manifest) && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

void PackageRegistry::loadOnboarding(const QString &catalogPath)
{
    m_packs.clear();
    m_guides.clear();
    const auto boundedText = [](const QJsonObject &object, const char *key,
                                int maximum) {
        const QString value = jsonString(object, key);
        return value.size() <= maximum ? value : QString();
    };
    const auto packageIds = [this](const QJsonValue &value) {
        QStringList ids;
        for (const QJsonValue &item : value.toArray()) {
            const QString id = item.toString();
            if (validPackageId(id) && package(id) && !ids.contains(id))
                ids.append(id);
        }
        return ids;
    };

    const QString packsPath =
        catalogChildPath(catalogPath, QStringLiteral("packs.json"));
    if (!packsPath.isEmpty() && QFile::exists(packsPath)) {
        QJsonObject root;
        QString error;
        if (!readJsonObject(packsPath, &root, &error) ||
            root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
            m_warnings.append(error.isEmpty()
                                  ? QStringLiteral("Unsupported Market packs file.")
                                  : error);
        } else {
            for (const QJsonValue &value :
                 root.value(QStringLiteral("packs")).toArray()) {
                const QJsonObject entry = value.toObject();
                PackagePack item;
                item.id = jsonString(entry, "id");
                item.name = boundedText(entry, "name", 60);
                item.tagline = boundedText(entry, "tagline", 120);
                item.summary = boundedText(entry, "summary", 400);
                const QJsonValue packages = entry.value(QStringLiteral("packages"));
                item.everything = packages.toString() == QLatin1String("*");
                if (!item.everything)
                    item.packages = packageIds(packages);
                if (!validPackageId(item.id) || item.name.isEmpty() ||
                    pack(item.id) ||
                    (!item.everything && item.packages.isEmpty())) {
                    m_warnings.append(
                        QStringLiteral("Market pack %1 is invalid.").arg(item.id));
                    continue;
                }
                m_packs.append(item);
            }
        }
    }

    const QString guidesPath =
        catalogChildPath(catalogPath, QStringLiteral("guides.json"));
    if (guidesPath.isEmpty() || !QFile::exists(guidesPath))
        return;
    QJsonObject root;
    QString error;
    if (!readJsonObject(guidesPath, &root, &error) ||
        root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        m_warnings.append(error.isEmpty()
                              ? QStringLiteral("Unsupported Market guides file.")
                              : error);
        return;
    }
    for (const QJsonValue &value :
         root.value(QStringLiteral("guides")).toArray()) {
        const QJsonObject entry = value.toObject();
        PackageGuide item;
        item.id = jsonString(entry, "id");
        item.title = boundedText(entry, "title", 80);
        item.from = boundedText(entry, "from", 60);
        item.summary = boundedText(entry, "summary", 400);
        item.pack = jsonString(entry, "pack");
        item.packages = packageIds(entry.value(QStringLiteral("packages")));
        for (const QJsonValue &step :
             entry.value(QStringLiteral("steps")).toArray()) {
            const QString text = step.toString().trimmed();
            if (!text.isEmpty() && text.size() <= 400)
                item.steps.append(text);
        }
        for (const QJsonValue &link :
             entry.value(QStringLiteral("links")).toArray()) {
            const QJsonObject object = link.toObject();
            const PackageGuideLink parsed{boundedText(object, "label", 60),
                                          jsonString(object, "url")};
            if (!parsed.label.isEmpty() && validHttpsUrl(parsed.url))
                item.links.append(parsed);
        }
        if (!validPackageId(item.id) || item.title.isEmpty() ||
            item.steps.size() < 2 || guide(item.id) ||
            (!item.pack.isEmpty() && !pack(item.pack)) ||
            (item.pack.isEmpty() && item.packages.isEmpty())) {
            m_warnings.append(
                QStringLiteral("Market guide %1 is invalid.").arg(item.id));
            continue;
        }
        m_guides.append(item);
    }
}

bool PackageRegistry::install(const QString &id, QString *error,
                              QStringList *changed)
{
    if (error)
        error->clear();
    const PackageManifest *manifest = package(id);
    if (!manifest) {
        if (error)
            *error = QStringLiteral("Unknown package: %1").arg(id);
        return false;
    }
    QString dependencyError;
    const QStringList dependencies = missingDependencies(id, &dependencyError);
    if (!dependencyError.isEmpty()) {
        if (error)
            *error = dependencyError;
        return false;
    }
    for (const QString &dependency : dependencies) {
        const PackageManifest *required = package(dependency);
        if (required->managedExternally)
            continue;
        if (!persistState(*required, true, error))
            return false;
        if (changed)
            changed->append(dependency);
    }
    if (manifest->managedExternally)
        return true; // Its verified on-disk catalog is the installation record.
    const bool wasInstalled = isInstalled(id);
    if (!persistState(*manifest, true, error))
        return false;
    if (changed && !wasInstalled)
        changed->append(id);
    if (wasInstalled)
        return true;
    for (const QString &nested : nestedPackages(id)) {
        const PackageManifest *child = package(nested);
        if (child->managedExternally || !installedWithoutChoice(*child) ||
            isInstalled(nested))
            continue;
        if (!persistState(*child, true, error))
            return false;
        if (changed)
            changed->append(nested);
    }
    return true;
}

bool PackageRegistry::uninstall(const QString &id, QString *error,
                                QStringList *changed)
{
    if (error)
        error->clear();
    const PackageManifest *manifest = package(id);
    if (!manifest) {
        if (error)
            *error = QStringLiteral("Unknown package: %1").arg(id);
        return false;
    }
    if (manifest->managedExternally) {
        if (error) {
            *error = QStringLiteral(
                "%1 is an external package. Remove it with marketctl so its "
                "down migrations run before its files are deleted.")
                         .arg(manifest->name);
        }
        return false;
    }
    const QStringList dependents = installedDependents(id);
    for (const QString &dependent : dependents) {
        const PackageManifest *other = package(dependent);
        if (other->managedExternally) {
            if (error) {
                *error = QStringLiteral(
                    "%1 needs %2. Remove %1 with marketctl first.")
                             .arg(other->name, manifest->name);
            }
            return false;
        }
    }
    QString cleanupWarning;
    for (auto it = dependents.crbegin(); it != dependents.crend(); ++it) {
        QString dependentError;
        if (!removeOne(*package(*it), &dependentError)) {
            if (error)
                *error = dependentError;
            return false;
        }
        if (cleanupWarning.isEmpty())
            cleanupWarning = dependentError;
        if (changed)
            changed->append(*it);
    }
    const bool wasInstalled = isInstalled(id);
    QString ownError;
    if (!removeOne(*manifest, &ownError)) {
        if (error)
            *error = ownError;
        return false;
    }
    if (changed && wasInstalled)
        changed->append(id);
    if (error)
        *error = ownError.isEmpty() ? cleanupWarning : ownError;
    return true;
}

bool PackageRegistry::setInstalledState(const QString &id, bool installed,
                                        QString *error)
{
    if (error)
        error->clear();
    const PackageManifest *manifest = package(id);
    if (!manifest || manifest->managedExternally) {
        if (error)
            *error = QStringLiteral("Unknown package: %1").arg(id);
        return false;
    }
    const QString key = statePrefix(id) + QStringLiteral("/state");
    if (m_settings->contains(key) && isInstalled(id) == installed)
        return true;
    if (installed)
        return persistState(*manifest, true, error);
    return removeOne(*manifest, error);
}

bool PackageRegistry::removeOne(const PackageManifest &manifest, QString *error)
{
    // Deactivate executable code first. Even if a damaged settings backend
    // cannot finish cache cleanup, the host must still tear down the runtime.
    if (!persistState(manifest, false, error))
        return false;

    // Package-owned preferences and caches are disposable. Library receipts
    // intentionally live under market/library and are never touched here.
    m_settings->remove(packageDataPrefix(manifest.id));
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError && error) {
        *error = QStringLiteral(
            "%1 was removed, but some package-owned data could not be cleared.")
                     .arg(manifest.name);
    }
    return true;
}

bool PackageRegistry::statusMonitorShows(const QString &systemId) const
{
    for (const PackageManifest &manifest : m_packages) {
        if (manifest.statusMonitors.isEmpty())
            continue;
        bool covered = false;
        for (const PackageStatusMonitor &monitor : manifest.statusMonitors)
            covered = covered || monitor.covers(systemId);
        if (covered && isEffectivelyInstalled(manifest.id))
            return true;
    }
    return false;
}

QVariant PackageRegistry::declaredSetting(const QString &id,
                                          const QString &key) const
{
    const PackageManifest *manifest = package(id);
    if (!manifest)
        return {};
    const PackageSettingField *field = manifest->setting(key);
    const QVariant stored = packageSetting(id, key);
    if (!field)
        return stored;
    if (!field->accepts(stored))
        return field->defaultValue;
    if (field->type == QLatin1String("boolean"))
        return QVariant(stored.typeId() == QMetaType::Bool
                            ? stored.toBool()
                            : stored.toString() == QLatin1String("true"));
    return QVariant(stored.toString());
}

bool PackageRegistry::persistState(const PackageManifest &manifest,
                                   bool installed, QString *error)
{
    const QString prefix = statePrefix(manifest.id);
    m_settings->setValue(prefix + QStringLiteral("/state"),
                         installed ? QStringLiteral("installed")
                                   : QStringLiteral("removed"));
    if (installed) {
        m_settings->setValue(prefix + QStringLiteral("/version"),
                             manifest.version);
    } else {
        m_settings->remove(prefix + QStringLiteral("/version"));
    }
    m_settings->sync();
    if (m_settings->status() == QSettings::NoError)
        return true;
    if (error)
        *error = QStringLiteral("Could not persist the %1 package state.")
                     .arg(manifest.name);
    return false;
}

bool PackageRegistry::loadCatalog(const QString &path, bool append)
{
    if (!append) {
        m_error.clear();
        m_warnings.clear();
    }
    QJsonObject catalog;
    if (!readJsonObject(path, &catalog, &m_error))
        return false;
    if (catalog.value(QStringLiteral("schemaVersion")).toInt() != 2) {
        m_error = QStringLiteral("Unsupported ForkMesh package catalog version.");
        return false;
    }
    const QJsonObject marketplace =
        catalog.value(QStringLiteral("marketplace")).toObject();
    const QString marketplaceId = jsonString(marketplace, "id");
    if (!validPackageId(marketplaceId)) {
        m_error = QStringLiteral("The package catalog marketplace id is invalid.");
        return false;
    }
    const QJsonArray packagePaths = catalog.value(QStringLiteral("packages")).toArray();
    if (packagePaths.isEmpty() && !append) {
        m_error = QStringLiteral("The ForkMesh package catalog is empty.");
        return false;
    }

    QSet<QString> ids;
    for (const PackageManifest &existing : m_packages)
        ids.insert(existing.id);
    QList<PackageManifest> additions;
    QStringList rejected;
    for (const QJsonValue &value : packagePaths) {
        const QJsonObject entry = value.toObject();
        const QString expectedId = jsonString(entry, "id");
        const QString expectedVersion = jsonString(entry, "version");
        const QString declaredMarketplace = jsonString(entry, "marketplace");
        const QString entryMarketplaceId = declaredMarketplace.isEmpty()
            ? marketplaceId : declaredMarketplace;
        if (!validPackageId(entryMarketplaceId)) {
            rejected.append(QStringLiteral(
                "The package catalog contains invalid marketplace provenance."));
            continue;
        }
        const QString manifestPath = catalogChildPath(
            path, jsonString(entry, "manifest"));
        if (manifestPath.isEmpty()) {
            rejected.append(
                QStringLiteral("The package catalog contains an unsafe path."));
            continue;
        }
        PackageManifest manifest;
        QString manifestError;
        if (!loadManifest(manifestPath, &manifest, &manifestError,
                          entryMarketplaceId)) {
            rejected.append(manifestError);
            continue;
        }
        manifest.managedExternally = append;
        if (manifest.id != expectedId || manifest.version != expectedVersion) {
            rejected.append(
                QStringLiteral("Catalog identity does not match package %1.")
                    .arg(manifest.id));
            continue;
        }
        if (ids.contains(manifest.id)) {
            rejected.append(
                QStringLiteral("Duplicate package id: %1").arg(manifest.id));
            continue;
        }
        ids.insert(manifest.id);
        additions.append(manifest);
    }
    if (additions.isEmpty()) {
        m_error = rejected.isEmpty()
                      ? QStringLiteral("The ForkMesh package catalog is empty.")
                      : rejected.join(QLatin1Char('\n'));
        return false;
    }
    if (!append)
        m_packages.clear();
    m_packages += additions;
    m_error.clear();
    m_warnings.append(rejected);
    return true;
}

bool PackageRegistry::loadManifest(const QString &path,
                                   PackageManifest *manifest,
                                   QString *error,
                                   const QString &marketplaceId) const
{
    QJsonObject root;
    if (!readJsonObject(path, &root, error))
        return false;
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        if (error)
            *error = QStringLiteral("Unsupported package manifest in %1.").arg(path);
        return false;
    }

    const QJsonObject creator = root.value(QStringLiteral("creator")).toObject();
    const QJsonObject license = root.value(QStringLiteral("license")).toObject();
    const QJsonObject price = root.value(QStringLiteral("price")).toObject();
    const QJsonObject entrypoints =
        root.value(QStringLiteral("entrypoints")).toObject();
    const QJsonObject trust = root.value(QStringLiteral("trust")).toObject();
    const QJsonObject install = root.value(QStringLiteral("install")).toObject();

    PackageManifest parsed;
    parsed.marketplaceId = marketplaceId;
    parsed.id = jsonString(root, "id");
    parsed.name = jsonString(root, "name");
    parsed.creatorId = jsonString(creator, "id");
    parsed.creatorHandle = jsonString(creator, "handle");
    parsed.creatorName = jsonString(creator, "displayName");
    parsed.creatorVerified = creator.value(QStringLiteral("verified")).toBool();
    parsed.type = jsonString(root, "type");
    parsed.category = jsonString(root, "category");
    parsed.version = jsonString(root, "version");
    parsed.summary = jsonString(root, "summary");
    parsed.licenseModel = jsonString(license, "model");
    parsed.licenseSpdx = jsonString(license, "spdx");
    parsed.creatorRetainsOwnership =
        license.value(QStringLiteral("creatorRetainsOwnership")).toBool();
    for (auto it = entrypoints.constBegin(); it != entrypoints.constEnd(); ++it) {
        const QString key = it.key().trimmed();
        const QString value = it.value().toString().trimmed();
        if (key.isEmpty() || value.isEmpty() || value.size() > 160) {
            if (error)
                *error = QStringLiteral("Package %1 has invalid entry points.")
                             .arg(parsed.id);
            return false;
        }
        parsed.entrypoints.insert(key, value);
    }
    parsed.desktopEntrypoint = jsonString(entrypoints, "desktop");
    parsed.settingsEntrypoint = jsonString(entrypoints, "settings");
    parsed.priceAmount = price.value(QStringLiteral("amount")).toDouble(-1.0);
    parsed.currency = jsonString(price, "currency");
    parsed.sourceAvailable =
        trust.value(QStringLiteral("sourceAvailable")).toBool();
    parsed.reproducibleBuild =
        trust.value(QStringLiteral("reproducibleBuild")).toBool();
    parsed.signatureVerified =
        trust.value(QStringLiteral("signature")).toString() ==
        QLatin1String("verified-bundle");
    parsed.bundled = install.value(QStringLiteral("bundled")).toBool();
    parsed.installedByDefault =
        install.value(QStringLiteral("default")).toBool();
    const QJsonValue installedBefore =
        install.value(QStringLiteral("installedBefore"));
    if (!installedBefore.isUndefined()) {
        parsed.installedBefore =
            QDate::fromString(installedBefore.toString(), Qt::ISODate);
        if (!installedBefore.isString() || !parsed.installedBefore.isValid() ||
            installedBefore.toString().size() != 10) {
            if (error)
                *error = QStringLiteral("Package %1 has an invalid "
                                        "install.installedBefore date.")
                             .arg(parsed.id);
            return false;
        }
    }
    parsed.places = allPlaces();
    const QJsonValue placesValue = install.value(QStringLiteral("places"));
    if (!placesValue.isUndefined()) {
        const QJsonArray declared = placesValue.toArray();
        QStringList names;
        bool placesValid = placesValue.isArray() && !declared.isEmpty();
        for (const QJsonValue &place : declared) {
            const QString name = place.toString();
            placesValid = placesValid && place.isString() &&
                          allPlaces().contains(name) && !names.contains(name);
            names.append(name);
        }
        if (!placesValid) {
            if (error)
                *error = QStringLiteral("Package %1 has invalid "
                                        "install.places.")
                             .arg(parsed.id);
            return false;
        }
        parsed.places.clear();
        for (const QString &place : allPlaces()) {
            if (names.contains(place))
                parsed.places.append(place);
        }
    }
    const QJsonObject dataLifecycle =
        root.value(QStringLiteral("dataLifecycle")).toObject();
    const QString dataScope = jsonString(dataLifecycle, "scope");
    if (dataScope == QLatin1String("organization") ||
        dataScope == QLatin1String("account"))
        parsed.dataScope = dataScope;
    parsed.localData =
        dataLifecycle.value(QStringLiteral("localData")).toBool();

    const QJsonObject lifecycle =
        root.value(QStringLiteral("lifecycle")).toObject();
    const QJsonValue migrationsValue =
        lifecycle.value(QStringLiteral("migrations"));
    if (!migrationsValue.isArray() || migrationsValue.toArray().size() > 128) {
        if (error)
            *error = QStringLiteral("Package %1 has no valid lifecycle.")
                         .arg(parsed.id);
        return false;
    }
    QSet<QString> migrationIds;
    for (const QJsonValue &value : migrationsValue.toArray()) {
        const QJsonObject migration = value.toObject();
        const QString migrationId = jsonString(migration, "id");
        const QString up = jsonString(migration, "up");
        const QString down = jsonString(migration, "down");
        if (!matches(migrationId, "^[0-9]{4}_[a-z][a-z0-9_-]{0,90}$", 96) ||
            migrationIds.contains(migrationId) || up == down ||
            !up.startsWith(QLatin1String("migrations/")) ||
            !down.startsWith(QLatin1String("migrations/")) ||
            catalogChildPath(path, up).isEmpty() ||
            catalogChildPath(path, down).isEmpty()) {
            if (error)
                *error = QStringLiteral(
                    "Package %1 has an invalid reversible migration.")
                             .arg(parsed.id);
            return false;
        }
        migrationIds.insert(migrationId);
    }

    const QString agentEntrypoint =
        parsed.entrypoints.value(QStringLiteral("agent"));
    if (!agentEntrypoint.isEmpty()) {
        const QString agentPath = catalogChildPath(path, agentEntrypoint);
        QJsonObject agentObject;
        if (agentPath.isEmpty() ||
            !agentEntrypoint.startsWith(QLatin1String("content/")) ||
            !readJsonObject(agentPath, &agentObject, error)) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral(
                             "Agent package %1 has no safe agent entry point.")
                             .arg(parsed.id);
            }
            return false;
        }
        if (agentObject.value(QStringLiteral("schemaVersion")).toInt() != 1) {
            if (error) {
                *error = QStringLiteral(
                             "Agent package %1 uses an unsupported descriptor.")
                             .arg(parsed.id);
            }
            return false;
        }
        PackageAgentContribution contribution;
        contribution.provider = jsonString(agentObject, "provider");
        contribution.runtime = jsonString(agentObject, "runtime");
        contribution.endpoint = jsonString(agentObject, "endpoint");
        contribution.defaultModel = jsonString(agentObject, "defaultModel");
        contribution.documentationUrl =
            jsonString(agentObject, "documentation");
        contribution.runtimeDownloadUrl =
            jsonString(agentObject, "runtimeDownload");
        contribution.avatarPath =
            catalogChildPath(agentPath, jsonString(agentObject, "avatar"));
        for (const QJsonValue &value :
             agentObject.value(QStringLiteral("models")).toArray()) {
            const QJsonObject modelObject = value.toObject();
            contribution.models.append(PackageAgentModel{
                jsonString(modelObject, "id"),
                jsonString(modelObject, "label"),
                static_cast<qint64>(
                    modelObject.value(QStringLiteral("downloadBytes")).toDouble()),
                modelObject.value(QStringLiteral("contextTokens")).toInt(),
            });
        }
        if (!contribution.isValid() ||
            !QFile::exists(contribution.avatarPath)) {
            if (error) {
                *error = QStringLiteral(
                             "Agent package %1 has an invalid local-model descriptor.")
                             .arg(parsed.id);
            }
            return false;
        }
        parsed.agent = contribution;
    }

    const bool agentModelEntrypoint =
        parsed.desktopEntrypoint == QLatin1String("builtin:agent-model");
    if (agentModelEntrypoint != !parsed.agent.runtime.isEmpty() ||
        (agentModelEntrypoint &&
         parsed.settingsEntrypoint != QLatin1String("builtin:agent-model"))) {
        if (error) {
            *error = QStringLiteral(
                         "Package %1 has inconsistent agent entry points.")
                         .arg(parsed.id);
        }
        return false;
    }

    if (parsed.type == QLatin1String("theme")) {
        const QString themeEntrypoint =
            parsed.entrypoints.value(QStringLiteral("theme"));
        const QString themePath = catalogChildPath(path, themeEntrypoint);
        QJsonObject theme;
        if (themeEntrypoint.isEmpty() || themePath.isEmpty() ||
            !themeEntrypoint.startsWith(QLatin1String("content/")) ||
            !readJsonObject(themePath, &theme, error)) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral(
                             "Theme package %1 has no safe theme entry point.")
                             .arg(parsed.id);
            }
            return false;
        }
        if (theme.value(QStringLiteral("schemaVersion")).toInt() != 1) {
            if (error) {
                *error = QStringLiteral(
                             "Theme package %1 uses an unsupported theme schema.")
                             .arg(parsed.id);
            }
            return false;
        }
        parsed.themeBase = jsonString(theme, "base").toLower();
        const QString styleSheet = jsonString(theme, "stylesheet");
        parsed.themeStyleSheetPath = catalogChildPath(themePath, styleSheet);
        const QJsonObject palette =
            theme.value(QStringLiteral("palette")).toObject();
        if ((parsed.themeBase != QLatin1String("dark") &&
             parsed.themeBase != QLatin1String("light")) ||
            styleSheet.isEmpty() || parsed.themeStyleSheetPath.isEmpty() ||
            palette.isEmpty() || !QFile::exists(parsed.themeStyleSheetPath)) {
            if (error) {
                *error = QStringLiteral(
                             "Theme package %1 has incomplete theme content.")
                             .arg(parsed.id);
            }
            return false;
        }
        for (auto it = palette.constBegin(); it != palette.constEnd(); ++it) {
            const QString source = it.key().trimmed().toLower();
            const QString replacement =
                it.value().toString().trimmed().toLower();
            if (!validThemeColor(source) || !validThemeColor(replacement) ||
                parsed.themePalette.contains(source)) {
                if (error) {
                    *error = QStringLiteral(
                                 "Theme package %1 has invalid palette colors.")
                                 .arg(parsed.id);
                }
                return false;
            }
            parsed.themePalette.insert(source, replacement);
        }
        QFile styleSheetFile(parsed.themeStyleSheetPath);
        if (!styleSheetFile.open(QIODevice::ReadOnly) ||
            styleSheetFile.size() <= 0 || styleSheetFile.size() > 256 * 1024) {
            if (error) {
                *error = QStringLiteral(
                             "Theme package %1 has an unreadable stylesheet.")
                             .arg(parsed.id);
            }
            return false;
        }
    }

    const QJsonArray screenshots =
        root.value(QStringLiteral("screenshots")).toArray();
    if (screenshots.isEmpty() || screenshots.size() > 8) {
        if (error)
            *error = QStringLiteral(
                "Package %1 must provide between one and eight screenshots.")
                         .arg(parsed.id);
        return false;
    }
    QSet<QString> screenshotPaths;
    for (const QJsonValue &value : screenshots) {
        const QJsonObject object = value.toObject();
        const QString relativePath = jsonString(object, "path");
        const QString alt = jsonString(object, "alt");
        const QString resolvedPath = catalogChildPath(path, relativePath);
        const QString extension = relativePath.section(QLatin1Char('.'), -1)
                                      .toLower();
        if (!relativePath.startsWith(QLatin1String("preview/")) ||
            relativePath.size() > 240 || resolvedPath.isEmpty() ||
            alt.isEmpty() || alt.size() > 160 ||
            !QSet<QString>{QStringLiteral("png"), QStringLiteral("jpg"),
                           QStringLiteral("jpeg"), QStringLiteral("webp"),
                           QStringLiteral("svg")}
                 .contains(extension) ||
            screenshotPaths.contains(relativePath) ||
            !QFile::exists(resolvedPath)) {
            if (error)
                *error = QStringLiteral("Package %1 has invalid screenshots.")
                             .arg(parsed.id);
            return false;
        }
        screenshotPaths.insert(relativePath);
        parsed.screenshots.append({resolvedPath, alt});
    }

    const QJsonArray permissions = root.value(QStringLiteral("permissions")).toArray();
    if (permissions.size() > 32) {
        if (error)
            *error = QStringLiteral("Package %1 requests too many permissions.")
                         .arg(parsed.id);
        return false;
    }
    QSet<QString> permissionIds;
    for (const QJsonValue &value : permissions) {
        const QJsonObject object = value.toObject();
        PackagePermission permission{
            jsonString(object, "id"),
            jsonString(object, "label"),
            jsonString(object, "risk"),
        };
        if (!matches(permission.id, "^[a-z][a-z0-9.]*$", 96) ||
            permission.label.isEmpty() || permission.label.size() > 120 ||
            !QSet<QString>{QStringLiteral("low"), QStringLiteral("sensitive"),
                           QStringLiteral("high")}
                 .contains(permission.risk) ||
            permissionIds.contains(permission.id)) {
            if (error)
                *error = QStringLiteral("Package %1 has invalid permissions.")
                             .arg(parsed.id);
            return false;
        }
        permissionIds.insert(permission.id);
        parsed.permissions.append(permission);
    }

    parsed.accountSync =
        install.value(QStringLiteral("sync")).toString() ==
        QLatin1String("account");

    const QJsonArray dependencies =
        root.value(QStringLiteral("dependencies")).toArray();
    if (dependencies.size() > 32) {
        if (error)
            *error = QStringLiteral("Package %1 has too many dependencies.")
                         .arg(parsed.id);
        return false;
    }
    for (const QJsonValue &value : dependencies) {
        const QString dependency = value.isString()
                                       ? value.toString().trimmed()
                                       : jsonString(value.toObject(), "id");
        if (!validPackageId(dependency) || dependency == parsed.id ||
            parsed.dependencies.contains(dependency)) {
            if (error)
                *error = QStringLiteral("Package %1 has invalid dependencies.")
                             .arg(parsed.id);
            return false;
        }
        parsed.dependencies.append(dependency);
    }

    const QJsonArray declaredSettings =
        root.value(QStringLiteral("settings")).toArray();
    if (declaredSettings.size() > 32) {
        if (error)
            *error = QStringLiteral("Package %1 declares too many settings.")
                         .arg(parsed.id);
        return false;
    }
    for (const QJsonValue &value : declaredSettings) {
        const QJsonObject object = value.toObject();
        PackageSettingField field;
        field.key = jsonString(object, "key");
        field.type = jsonString(object, "type");
        field.label = jsonString(object, "label");
        const QJsonValue fallback = object.value(QStringLiteral("default"));
        if (field.type == QLatin1String("boolean") && fallback.isBool()) {
            field.defaultValue = fallback.toBool();
        } else if (field.type == QLatin1String("choice") &&
                   fallback.isString()) {
            field.defaultValue = fallback.toString();
            const QJsonArray options =
                object.value(QStringLiteral("options")).toArray();
            for (const QJsonValue &optionValue : options) {
                const QJsonObject option = optionValue.toObject();
                const QJsonValue raw = option.value(QStringLiteral("value"));
                const PackageSettingOption parsedOption{
                    raw.isString() ? raw.toString() : QString(),
                    jsonString(option, "label")};
                bool duplicate = false;
                for (const PackageSettingOption &existing : field.options)
                    duplicate = duplicate || existing.value == parsedOption.value;
                if (!raw.isString() || parsedOption.value.size() > 64 ||
                    parsedOption.label.isEmpty() ||
                    parsedOption.label.size() > 80 || duplicate) {
                    field.options.clear();
                    break;
                }
                field.options.append(parsedOption);
            }
            if (options.isEmpty() || options.size() > 32)
                field.options.clear();
        }
        static const QRegularExpression settingKey(
            QStringLiteral("^[A-Za-z][A-Za-z0-9]{0,63}$"));
        if (!settingKey.match(field.key).hasMatch() ||
            field.label.isEmpty() || field.label.size() > 120 ||
            !field.accepts(field.defaultValue) || parsed.setting(field.key)) {
            if (error)
                *error = QStringLiteral("Package %1 declares invalid settings.")
                             .arg(parsed.id);
            return false;
        }
        parsed.settings.append(field);
    }

    for (const QJsonValue &value :
         root.value(QStringLiteral("contributions")).toArray()) {
        const QJsonObject contribution = value.toObject();
        if (jsonString(contribution, "socket") !=
            QLatin1String("status.monitor"))
            continue;
        const QJsonObject config =
            contribution.value(QStringLiteral("config")).toObject();
        PackageStatusMonitor monitor;
        monitor.label = jsonString(config, "label");
        static const QRegularExpression systemId(
            QStringLiteral("^[a-z][a-z0-9_.:-]{0,63}$"));
        bool valid = !monitor.label.isEmpty() && monitor.label.size() <= 80;
        for (const QJsonValue &system :
             config.value(QStringLiteral("systems")).toArray()) {
            const QString id = system.toString().trimmed();
            valid = valid && systemId.match(id).hasMatch();
            monitor.systems.append(id);
        }
        for (const QJsonValue &prefix :
             config.value(QStringLiteral("prefixes")).toArray()) {
            const QString id = prefix.toString().trimmed();
            valid = valid && systemId.match(id).hasMatch();
            monitor.prefixes.append(id);
        }
        if (!valid || (monitor.systems.isEmpty() && monitor.prefixes.isEmpty())) {
            if (error)
                *error = QStringLiteral(
                             "Package %1 has an invalid status monitor.")
                             .arg(parsed.id);
            return false;
        }
        parsed.statusMonitors.append(monitor);
    }

    if (!parsed.isValid()) {
        if (error)
            *error = QStringLiteral("Package manifest %1 is incomplete or invalid.")
                         .arg(path);
        return false;
    }
    *manifest = parsed;
    return true;
}

} // namespace forkmesh::market
