#pragma once

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

class QJsonObject;
class QSettings;

namespace forkmesh::market {

struct PackagePermission {
    QString id;
    QString label;
    QString risk;
};

struct PackageScreenshot {
    QString path;
    QString alt;
};

struct PackageSettingOption {
    QString value;
    QString label;
};

struct PackageSettingField {
    QString key;
    QString type;
    QString label;
    QVariant defaultValue;
    QList<PackageSettingOption> options;

    bool accepts(const QVariant &value) const;
};

struct PackageStatusMonitor {
    QString label;
    QStringList systems;
    QStringList prefixes;

    bool covers(const QString &systemId) const;
};

struct PackageAgentModel {
    QString id;
    QString label;
    qint64 downloadBytes = 0;
    int contextTokens = 0;
};

// A declarative, allowlisted agent contribution. Packages name models and
// presentation here; executable download/run behavior remains in the host so a
// catalog manifest can never turn into an arbitrary command launcher.
struct PackageAgentContribution {
    QString provider;
    QString runtime;
    QString endpoint;
    QString defaultModel;
    QString avatarPath;
    QString documentationUrl;
    QString runtimeDownloadUrl;
    QList<PackageAgentModel> models;

    bool isValid() const;
    const PackageAgentModel *model(const QString &id) const;
};

struct PackageManifest {
    QString marketplaceId;
    QString id;
    QString name;
    QString creatorId;
    QString creatorHandle;
    QString creatorName;
    QString type;
    QString category;
    QString version;
    QString summary;
    QString licenseModel;
    QString licenseSpdx;
    QMap<QString, QString> entrypoints;
    QString desktopEntrypoint;
    QString settingsEntrypoint;
    QString themeBase;
    QString themeStyleSheetPath;
    QMap<QString, QString> themePalette;
    QString currency;
    double priceAmount = 0.0;
    bool creatorVerified = false;
    bool creatorRetainsOwnership = false;
    bool sourceAvailable = false;
    bool reproducibleBuild = false;
    bool signatureVerified = false;
    bool bundled = false;
    bool installedByDefault = false;
    QDate installedBefore;
    QStringList places;
    bool managedExternally = false;
    // dataLifecycle.scope: where the package's server-side records live
    // ("none", "organization" or "account"). Removing a package from this
    // desktop never deletes those records.
    QString dataScope = QStringLiteral("none");
    // dataLifecycle.localData: the package keeps the user's own records in
    // its package data on this device. Removal wipes that data, so the host
    // offers to export it first and to import a saved copy on install.
    bool localData = false;
    bool accountSync = false;
    QStringList dependencies;
    QList<PackageSettingField> settings;
    QList<PackageStatusMonitor> statusMonitors;
    QList<PackagePermission> permissions;
    QList<PackageScreenshot> screenshots;
    PackageAgentContribution agent;

    bool isValid() const;
    const PackageSettingField *setting(const QString &key) const;
    QString priceLabel() const;
};

struct PackagePack {
    QString id;
    QString name;
    QString tagline;
    QString summary;
    bool everything = false;
    QStringList packages;
};

struct PackageGuideLink {
    QString label;
    QString url;
};

struct PackageGuide {
    QString id;
    QString title;
    QString from;
    QString summary;
    QString pack;
    QStringList packages;
    QStringList steps;
    QList<PackageGuideLink> links;
};

// Manifest-backed local view of the ForkMesh creator catalog. Installation
// state is deliberately separate from licence/receipt data: removing content
// must never erase proof that the user bought or may use it.
class PackageRegistry {
public:
    explicit PackageRegistry(QSettings *settings = nullptr,
                             const QString &catalogPath =
                                 QStringLiteral(":/forkmesh-market/catalog.json"));
    ~PackageRegistry();

    PackageRegistry(const PackageRegistry &) = delete;
    PackageRegistry &operator=(const PackageRegistry &) = delete;

    bool isReady() const { return m_error.isEmpty(); }
    QString errorString() const { return m_error; }
    QString warningString() const { return m_warnings.join(QLatin1Char('\n')); }
    const QList<PackageManifest> &packages() const { return m_packages; }
    const PackageManifest *package(const QString &id) const;

    bool isInstalled(const QString &id) const;
    bool isEffectivelyInstalled(const QString &id) const;
    bool installedWithoutChoice(const PackageManifest &manifest) const;
    void setProjectCreated(const QDateTime &created);
    QDateTime projectCreated() const { return m_projectCreated; }
    bool projectCreatedBefore(const QDate &date) const;
    bool isLegacyProject() const;
    bool legacyEntryInstalled(const PackageManifest &manifest) const;
    bool projectOwnsInstalls() const;
    bool hasStoredInstall(const QString &id) const;
    bool accountAnswerInstalls(const PackageManifest &manifest,
                               bool accountInstalled, bool legacyEntry,
                               bool unchosen = false) const;
    static const QStringList &allPlaces();
    static QString desktopPlace();
    static bool reliesOnDesktop(const QString &id);
    QStringList places(const QString &id) const;
    bool setPlaces(const QString &id, const QStringList &places);
    bool isPlacedHere(const QString &id) const;
    bool isInstalledHere(const QString &id) const;
    bool isShownHere(const QString &id) const;
    static QDateTime readProjectCreated(const QString &projectDir);
    bool install(const QString &id, QString *error = nullptr,
                 QStringList *changed = nullptr);
    bool uninstall(const QString &id, QString *error = nullptr,
                   QStringList *changed = nullptr);
    bool setInstalledState(const QString &id, bool installed,
                           QString *error = nullptr);
    QStringList missingDependencies(const QString &id,
                                    QString *error = nullptr) const;
    QStringList installedDependents(const QString &id) const;
    QString marketParent(const QString &id) const;
    QStringList nestedPackages(const QString &id) const;
    static bool installableHere(const PackageManifest &manifest);
    const QList<PackagePack> &packs() const { return m_packs; }
    const QList<PackageGuide> &guides() const { return m_guides; }
    const PackagePack *pack(const QString &id) const;
    const PackageGuide *guide(const QString &id) const;
    QStringList installAllPackages() const;
    QStringList packPackages(const QString &packId) const;
    QStringList guidePackages(const QString &guideId) const;
    bool statusMonitorShows(const QString &systemId) const;
    QVariant declaredSetting(const QString &id, const QString &key) const;

    QString packageDataPrefix(const QString &id) const;
    QString libraryReceiptPrefix(const QString &id) const;
    QVariant packageSetting(const QString &id, const QString &key,
                            const QVariant &fallback = QVariant()) const;
    bool setPackageSetting(const QString &id, const QString &key,
                           const QVariant &value, QString *error = nullptr);

    // Portable copy of a package's local data (everything packageSetting
    // stores), for saving before removal and restoring on a later install.
    bool hasPackageData(const QString &id) const;
    QJsonObject exportPackageData(const QString &id) const;
    // Replaces the package's local data with a bundle exportPackageData
    // wrote. The bundle is validated completely before anything changes.
    bool importPackageData(const QString &id, const QJsonObject &bundle,
                           QString *error = nullptr);
    void clearPackageData(const QString &id);

private:
    bool loadCatalog(const QString &path, bool append = false);
    void loadOnboarding(const QString &catalogPath);
    bool loadManifest(const QString &path, PackageManifest *manifest,
                      QString *error, const QString &marketplaceId) const;
    bool persistState(const PackageManifest &manifest, bool installed,
                      QString *error);
    bool removeOne(const PackageManifest &manifest, QString *error);
    bool effectivelyInstalled(const QString &id, QSet<QString> *visiting) const;
    QString statePrefix(const QString &id) const;
    void reindex();

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    QDateTime m_projectCreated;
    QList<PackageManifest> m_packages;
    QHash<QString, qsizetype> m_packageIndex;
    QHash<QString, QStringList> m_nestedPackages;
    QList<PackagePack> m_packs;
    QList<PackageGuide> m_guides;
    QString m_error;
    QStringList m_warnings;
};

} // namespace forkmesh::market
