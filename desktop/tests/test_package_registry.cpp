#include "ForkMeshPackageRegistry.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary package settings directory exists");
    if (!temporary.isValid())
        return 1;

    QSettings settings(temporary.filePath(QStringLiteral("market.ini")),
                       QSettings::IniFormat);
    forkmesh::market::PackageRegistry registry(&settings);
    if (!registry.isReady())
        std::fprintf(stderr, "catalog error: %s\n",
                     qPrintable(registry.errorString()));
    check(registry.isReady(), "the embedded marketplace catalog validates");
    check(registry.packages().size() == 129,
          "every embedded marketplace package is retained");
    {
        bool lookupsMatch =
            !registry.package(QStringLiteral("no-such-package")) &&
            registry.nestedPackages(QStringLiteral("no-such-package")).isEmpty();
        int nestedTotal = 0;
        for (const forkmesh::market::PackageManifest &manifest : registry.packages()) {
            const forkmesh::market::PackageManifest *found =
                registry.package(manifest.id);
            lookupsMatch = lookupsMatch && found == &manifest;
            QStringList scanned;
            for (const forkmesh::market::PackageManifest &candidate :
                 registry.packages()) {
                if (registry.marketParent(candidate.id) == manifest.id)
                    scanned.append(candidate.id);
            }
            lookupsMatch =
                lookupsMatch && registry.nestedPackages(manifest.id) == scanned;
            nestedTotal += int(scanned.size());
        }
        check(lookupsMatch && nestedTotal > 0,
              "indexed package and nested-package lookups match a full catalog scan");
    }
    const forkmesh::market::PackageManifest *designSystem =
        registry.package(QStringLiteral("design-system"));
    check(designSystem &&
              designSystem->desktopEntrypoint ==
                  QStringLiteral("builtin:design-system") &&
              registry.isInstalled(QStringLiteral("design-system")),
          "the Design System link is bundled and on by default");
    const forkmesh::market::PackageManifest *statusPage =
        registry.package(QStringLiteral("status-page"));
    check(statusPage &&
              statusPage->desktopEntrypoint ==
                  QStringLiteral("builtin:status-page") &&
              registry.isInstalled(QStringLiteral("status-page")),
          "the Status Page lights are bundled and on by default");
    const forkmesh::market::PackageManifest *docs =
        registry.package(QStringLiteral("docs"));
    check(docs && docs->desktopEntrypoint == QStringLiteral("builtin:docs") &&
              registry.isInstalled(QStringLiteral("docs")),
          "the Docs viewer is bundled and on by default");
    check(!registry.packages().isEmpty() &&
              registry.packages().first().id == QStringLiteral("capture"),
          "Capture is the first package in the catalog");
    check(registry.packages().size() >= 4 &&
              registry.packages().at(1).id == QStringLiteral("ledger") &&
              registry.packages().at(2).id ==
                  QStringLiteral("x-banner-board") &&
              registry.packages().at(3).id ==
                  QStringLiteral("reddit-banner-board"),
          "Ledger and the World boards follow Capture in the shared catalog");
    bool everyPackageHasSettings = true;
    for (const forkmesh::market::PackageManifest &manifest :
         registry.packages()) {
        everyPackageHasSettings =
            everyPackageHasSettings && !manifest.settingsEntrypoint.isEmpty();
    }
    check(everyPackageHasSettings,
          "every marketplace package declares a settings page entry point");

    const forkmesh::market::PackagePack *drawer =
        registry.pack(QStringLiteral("whole-cutlery-drawer"));
    const QStringList everything =
        registry.packPackages(QStringLiteral("whole-cutlery-drawer"));
    check(drawer && drawer->everything && everything.contains(QStringLiteral("crm")) &&
              everything.contains(QStringLiteral("chat")) &&
              !everything.contains(QStringLiteral("gemma-4")) &&
              !everything.contains(QStringLiteral("winamp")) &&
              !everything.contains(QStringLiteral("forkmesh-ide")) &&
              !everything.contains(QStringLiteral("status-check-api")),
          "the everything pack holds every installable top-level package but "
          "no model downloads, skins, catalog-only or nested packages");
    const QStringList chatPack =
        registry.packPackages(QStringLiteral("chat-essentials"));
    check(chatPack.first() == QStringLiteral("chat") &&
              chatPack.contains(QStringLiteral("inbox")) &&
              chatPack.size() < everything.size(),
          "the chat pack lists chat and a few essentials");
    const QStringList cloudflareGuide =
        registry.guidePackages(QStringLiteral("cloudflare-pages-github"));
    const forkmesh::market::PackageGuide *flyGuide =
        registry.guide(QStringLiteral("fly-io-app"));
    check(registry.guides().size() >= 6 &&
              cloudflareGuide == registry.packPackages(QStringLiteral("site-mover")) &&
              flyGuide && flyGuide->steps.size() >= 2 &&
              registry.guidePackages(QStringLiteral("fly-io-app"))
                  .contains(QStringLiteral("sentry")),
          "guides resolve to their pack plus any extra packages");

    const forkmesh::market::PackageManifest *capture =
        registry.package(QStringLiteral("capture"));
    check(capture && capture->name == QStringLiteral("Capture") &&
              capture->type == QStringLiteral("extension.native") &&
              capture->desktopEntrypoint == QStringLiteral("builtin:capture"),
          "Capture declares the native extension entry point");
    check(capture && capture->creatorVerified &&
              capture->creatorRetainsOwnership && capture->sourceAvailable &&
              capture->signatureVerified,
          "creator ownership and trust metadata survive parsing");
    check(capture && capture->permissions.size() == 3 &&
              capture->permissions.first().id ==
                  QStringLiteral("screen.capture") &&
              capture->permissions.last().id ==
                  QStringLiteral("clipboard.write"),
          "Capture discloses its bounded permissions");
    check(capture && capture->screenshots.size() == 1 &&
              capture->screenshots.first().path.endsWith(
                  QStringLiteral("preview/market.png")) &&
              QFile::exists(capture->screenshots.first().path) &&
              !capture->screenshots.first().alt.isEmpty(),
          "Capture exposes a readable screenshot and accessible description");
    check(registry.isInstalled(QStringLiteral("capture")),
          "the moved built-in remains installed on first run");
    const forkmesh::market::PackageManifest *ledger =
        registry.package(QStringLiteral("ledger"));
    check(ledger && ledger->name == QStringLiteral("Ledger") &&
              ledger->type == QStringLiteral("extension.native") &&
              ledger->desktopEntrypoint == QStringLiteral("builtin:ledger"),
          "Ledger declares its native desktop entry point");
    QStringList ledgerPermissions;
    QString ledgerPaymentRisk;
    for (const forkmesh::market::PackagePermission &permission :
         ledger ? ledger->permissions
                : QList<forkmesh::market::PackagePermission>()) {
        ledgerPermissions << permission.id;
        if (permission.id == QStringLiteral("wallet.payment.send"))
            ledgerPaymentRisk = permission.risk;
    }
    check(ledgerPermissions ==
                  QStringList{QStringLiteral("ledger.records.read"),
                              QStringLiteral("wallet.payment.send"),
                              QStringLiteral("repository.reward.write"),
                              QStringLiteral("external.link.open")} &&
              ledgerPaymentRisk == QStringLiteral("high"),
          "Ledger discloses reading records, paying from the linked wallet, "
          "writing the receipt back and opening the browser");
    check(registry.isInstalled(QStringLiteral("ledger")),
          "the Ledger rail contribution is installed by default");
    const forkmesh::market::PackageManifest *xBoard =
        registry.package(QStringLiteral("x-banner-board"));
    const forkmesh::market::PackageManifest *redditBoard =
        registry.package(QStringLiteral("reddit-banner-board"));
    check(xBoard && redditBoard &&
              xBoard->type == QStringLiteral("virtual.object") &&
              redditBoard->type == QStringLiteral("virtual.object") &&
              xBoard->entrypoints.value(QStringLiteral("world")) ==
                  QStringLiteral("content/world/index.js") &&
              redditBoard->entrypoints.value(QStringLiteral("world")) ==
                  QStringLiteral("content/world/index.js") &&
              xBoard->desktopEntrypoint.isEmpty() &&
              redditBoard->desktopEntrypoint.isEmpty(),
          "the social boards declare World-only virtual-object entry points");
    check(!registry.isInstalled(QStringLiteral("x-banner-board")) &&
              !registry.isInstalled(QStringLiteral("reddit-banner-board")),
          "both bundled World boards stay off until installed");

    const forkmesh::market::PackageManifest *gemma =
        registry.package(QStringLiteral("gemma-4"));
    check(gemma && gemma->type == QStringLiteral("agent") &&
              gemma->desktopEntrypoint ==
                  QStringLiteral("builtin:agent-model") &&
              gemma->settingsEntrypoint ==
                  QStringLiteral("builtin:agent-model") &&
              gemma->agent.provider == QStringLiteral("local-llama") &&
              gemma->agent.runtime == QStringLiteral("ollama") &&
              gemma->agent.defaultModel == QStringLiteral("gemma4:e4b") &&
              gemma->agent.models.size() == 5 &&
              QFile::exists(gemma->agent.avatarPath),
          "Gemma 4 exposes a validated Ollama contribution and distinct avatar");
    check(gemma && gemma->bundled &&
              !registry.isInstalled(QStringLiteral("gemma-4")),
          "Gemma 4 is discoverable but does not download weights by default");

    const QStringList customModelIds{
        QStringLiteral("nemotron-3-ultra"),
        QStringLiteral("nemotron-3.5-lightning"),
        QStringLiteral("muse-spark-1.3-contributor"),
    };
    for (const QString &id : customModelIds) {
        const forkmesh::market::PackageManifest *model = registry.package(id);
        check(model && model->category == QStringLiteral("Custom models") &&
                  model->desktopEntrypoint ==
                      QStringLiteral("builtin:agent-model") &&
                  model->agent.provider == QStringLiteral("local-llama") &&
                  model->agent.runtime == QStringLiteral("ollama") &&
                  model->agent.models.size() == 1 && model->bundled &&
                  !registry.isInstalled(id) &&
                  QFile::exists(model->agent.avatarPath),
              "custom model packages stay opt-in and contribute one Ollama model");
    }

    const forkmesh::market::PackageManifest *tasks =
        registry.package(QStringLiteral("tasks"));
    check(tasks && tasks->screenshots.size() == 1 &&
              tasks->screenshots.first().path.endsWith(
                  QStringLiteral("preview/market.svg")) &&
              QFile::exists(tasks->screenshots.first().path),
          "vector marketplace previews are accepted and remain readable");

    const forkmesh::market::PackageManifest *ideExtension =
        registry.package(QStringLiteral("forkmesh-ide"));
    check(ideExtension &&
              ideExtension->type == QStringLiteral("extension.vscode") &&
              ideExtension->entrypoints.value(QStringLiteral("ide")) ==
                  QStringLiteral("content/vscode/package.json") &&
              ideExtension->desktopEntrypoint.isEmpty(),
          "the IDE extension is owned by an IDE-only Marketplace entry point");
    check(ideExtension && ideExtension->bundled &&
              !registry.isInstalled(QStringLiteral("forkmesh-ide")),
          "the bundled IDE extension is not installed in the base system");

    QString gemmaError;
    check(registry.setPackageSetting(QStringLiteral("gemma-4"),
                                     QStringLiteral("desiredModel"),
                                     QStringLiteral("gemma4:12b"),
                                     &gemmaError) &&
              gemmaError.isEmpty() &&
              registry.packageSetting(QStringLiteral("gemma-4"),
                                      QStringLiteral("desiredModel")) ==
                  QStringLiteral("gemma4:12b"),
          "agent-package settings are namespaced in the shared registry");
    check(registry.install(QStringLiteral("gemma-4"), &gemmaError) &&
              registry.isInstalled(QStringLiteral("gemma-4")) &&
              registry.uninstall(QStringLiteral("gemma-4"), &gemmaError) &&
              !registry.isInstalled(QStringLiteral("gemma-4")) &&
              !registry.packageSetting(QStringLiteral("gemma-4"),
                                       QStringLiteral("desiredModel"))
                   .isValid(),
          "Gemma uses the modular package lifecycle and clears owned settings");

    const forkmesh::market::PackageManifest *winamp =
        registry.package(QStringLiteral("winamp"));
    check(winamp && winamp->name == QStringLiteral("Winamp Classic") &&
              winamp->type == QStringLiteral("theme") &&
              winamp->entrypoints.value(QStringLiteral("theme")) ==
                  QStringLiteral("content/theme.json") &&
              winamp->themeBase == QStringLiteral("dark") &&
              winamp->themePalette.value(QStringLiteral("#58a6ff")) ==
                  QStringLiteral("#19e64a") &&
              winamp->themeStyleSheetPath.endsWith(
                  QStringLiteral("content/winamp.qss")) &&
              QFile::exists(winamp->themeStyleSheetPath),
          "Winamp exposes a validated dark palette and desktop stylesheet");
    check(winamp && winamp->permissions.isEmpty() && winamp->bundled &&
              !registry.isInstalled(QStringLiteral("winamp")),
          "the bundled Winamp skin asks for no permissions and is opt-in");
    QString themeError;
    check(registry.install(QStringLiteral("winamp"), &themeError) &&
              themeError.isEmpty() &&
              registry.isInstalled(QStringLiteral("winamp")),
          "a declarative theme installs through the shared package lifecycle");
    check(registry.uninstall(QStringLiteral("winamp"), &themeError) &&
              themeError.isEmpty() &&
              !registry.isInstalled(QStringLiteral("winamp")),
          "a theme can be removed through the shared package lifecycle");

    const QString dataKey =
        registry.packageDataPrefix(QStringLiteral("capture")) +
        QStringLiteral("/unfinishedMarkup");
    const QString receiptKey =
        registry.libraryReceiptPrefix(QStringLiteral("capture")) +
        QStringLiteral("/licence");
    settings.setValue(dataKey, QStringLiteral("transient package data"));
    settings.setValue(receiptKey, QStringLiteral("durable entitlement"));
    QString error;
    check(registry.uninstall(QStringLiteral("capture"), &error) &&
              error.isEmpty(),
          "Capture can be removed without restarting");
    check(!registry.isInstalled(QStringLiteral("capture")),
          "removal disables the bundled default through an explicit tombstone");
    check(!settings.contains(dataKey),
          "removal deletes all package-owned settings and cache state");
    check(settings.value(receiptKey).toString() ==
              QStringLiteral("durable entitlement"),
          "removal preserves the separate Library receipt");

    forkmesh::market::PackageRegistry reopened(&settings);
    check(reopened.isReady() &&
              !reopened.isInstalled(QStringLiteral("capture")),
          "removed state persists across application restarts");
    check(reopened.install(QStringLiteral("capture"), &error) &&
              reopened.isInstalled(QStringLiteral("capture")),
          "Capture reinstalls and activates from the same manifest");
    check(capture && !capture->version.isEmpty() &&
              settings.value(QStringLiteral("market/packages/capture/version"))
                      .toString() == capture->version,
          "install records the exact activated package version");

    const QString ledgerDataKey =
        registry.packageDataPrefix(QStringLiteral("ledger")) +
        QStringLiteral("/lastView");
    const QString ledgerReceiptKey =
        registry.libraryReceiptPrefix(QStringLiteral("ledger")) +
        QStringLiteral("/licence");
    settings.setValue(ledgerDataKey, QStringLiteral("all"));
    settings.setValue(ledgerReceiptKey, QStringLiteral("durable entitlement"));
    check(registry.uninstall(QStringLiteral("ledger"), &error) &&
              error.isEmpty() &&
              !registry.isInstalled(QStringLiteral("ledger")) &&
              !settings.contains(ledgerDataKey) &&
              settings.value(ledgerReceiptKey).toString() ==
                  QStringLiteral("durable entitlement"),
          "Ledger removal clears its runtime data but preserves entitlement");
    check(registry.install(QStringLiteral("ledger"), &error) &&
              error.isEmpty() &&
              registry.isInstalled(QStringLiteral("ledger")),
          "Ledger reinstalls through the native entry-point allowlist");

    settings.setValue(QStringLiteral("market/packages/capture/state"),
                      QStringLiteral("corrupt-state"));
    forkmesh::market::PackageRegistry corruptState(&settings);
    check(!corruptState.isInstalled(QStringLiteral("capture")),
          "a malformed install record fails closed instead of reactivating code");
    check(corruptState.install(QStringLiteral("capture"), &error),
          "a clean install repairs a malformed local state record");
    check(!reopened.install(QStringLiteral("missing"), &error) &&
              error.contains(QStringLiteral("Unknown package")),
          "unknown packages fail closed");

    // Catalog authorship is not reserved for the core account. A member-owned
    // package remains explicitly unverified until review establishes that
    // separate trust signal, but it is otherwise a first-class listing.
    const QString memberRoot =
        temporary.filePath(QStringLiteral("member-market"));
    const QString memberPackageRoot =
        memberRoot + QStringLiteral("/packages/maya-theme");
    check(QDir().mkpath(memberPackageRoot + QStringLiteral("/preview")) &&
              QDir().mkpath(memberPackageRoot + QStringLiteral("/content")),
          "member package fixture directory is created");
    QFile memberPreview(memberPackageRoot +
                        QStringLiteral("/preview/market.png"));
    check(memberPreview.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              memberPreview.write("member preview") > 0,
          "member package screenshot fixture is written");
    memberPreview.close();
    QFile memberTheme(memberPackageRoot + QStringLiteral("/content/theme.json"));
    check(memberTheme.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              memberTheme.write(
                  "{\"schemaVersion\":1,\"base\":\"dark\","
                  "\"stylesheet\":\"maya.qss\","
                  "\"palette\":{\"#0d1117\":\"#101010\"}}") > 0,
          "member theme descriptor fixture is written");
    memberTheme.close();
    QFile memberStyle(memberPackageRoot + QStringLiteral("/content/maya.qss"));
    check(memberStyle.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              memberStyle.write(
                  "/* member theme */\nQWidget { border-radius: 0; }") > 0,
          "member theme stylesheet fixture is written");
    memberStyle.close();
    QFile memberManifest(memberPackageRoot + QStringLiteral("/manifest.json"));
    check(memberManifest.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              memberManifest.write(R"JSON({
  "schemaVersion": 1,
  "id": "maya-theme",
  "name": "Maya Theme",
  "creator": {
    "id": "did:forkmesh:maya",
    "handle": "maya",
    "displayName": "Maya",
    "verified": false
  },
  "type": "theme",
  "category": "Themes",
  "version": "1.0.0",
  "summary": "A member-published theme used by the package registry test.",
  "license": {
    "model": "free-open-source",
    "spdx": "MIT",
    "creatorRetainsOwnership": true
  },
  "price": {"amount": 0, "currency": "USD"},
  "entrypoints": {"theme": "content/theme.json", "settings": "builtin:package"},
  "lifecycle": {"migrations": []},
  "permissions": [],
  "screenshots": [
    {"path": "preview/market.png", "alt": "Maya Theme preview"}
  ]
})JSON") > 0,
          "member-owned package manifest fixture is written");
    memberManifest.close();
    QFile memberCatalog(memberRoot + QStringLiteral("/catalog.json"));
    check(memberCatalog.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              memberCatalog.write(
                  R"JSON({"schemaVersion":2,"marketplace":{"id":"member-market"},"packages":[{"id":"maya-theme","version":"1.0.0","manifest":"packages/maya-theme/manifest.json"}]})JSON") > 0,
          "member package catalog fixture is written");
    memberCatalog.close();
    QSettings memberSettings(
        temporary.filePath(QStringLiteral("member-market.ini")),
        QSettings::IniFormat);
    forkmesh::market::PackageRegistry memberRegistry(
        &memberSettings, memberRoot + QStringLiteral("/catalog.json"));
    const forkmesh::market::PackageManifest *memberPackage =
        memberRegistry.package(QStringLiteral("maya-theme"));
    check(memberRegistry.isReady() && memberPackage &&
              memberPackage->creatorHandle == QStringLiteral("maya") &&
              !memberPackage->creatorVerified &&
              memberPackage->screenshots.size() == 1,
          "a ForkMesh member can publish under their own unverified creator identity");

    const QString unsafeCatalog =
        temporary.filePath(QStringLiteral("unsafe-catalog.json"));
    QFile catalogFile(unsafeCatalog);
    check(catalogFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              catalogFile.write(
                  "{\"schemaVersion\":2,\"marketplace\":{\"id\":\"unsafe\"},"
                  "\"packages\":[{\"id\":\"escape\",\"version\":\"1.0.0\","
                  "\"manifest\":\"../escape.json\"}]}") > 0,
          "unsafe catalog fixture is written");
    catalogFile.close();
    QSettings unsafeSettings(
        temporary.filePath(QStringLiteral("unsafe.ini")), QSettings::IniFormat);
    forkmesh::market::PackageRegistry unsafe(&unsafeSettings, unsafeCatalog);
    check(!unsafe.isReady() &&
              unsafe.errorString().contains(QStringLiteral("unsafe path")),
          "catalog paths cannot escape the package root");

    // Package data travels as one portable bundle: export before removal,
    // import on the next install.
    const forkmesh::market::PackageManifest *timers =
        registry.package(QStringLiteral("timers"));
    const forkmesh::market::PackageManifest *crm =
        registry.package(QStringLiteral("crm"));
    check(timers && timers->localData && timers->dataScope == QStringLiteral("none") &&
              crm && !crm->localData &&
              crm->dataScope == QStringLiteral("organization"),
          "dataLifecycle scope and localData are read from the manifest");
    QString dataError;
    check(!registry.hasPackageData(QStringLiteral("timers")) &&
              registry.install(QStringLiteral("timers"), &dataError) &&
              registry.setPackageSetting(QStringLiteral("timers"),
                                         QStringLiteral("state"),
                                         QStringLiteral("[\"standup\"]")) &&
              registry.setPackageSetting(QStringLiteral("timers"),
                                         QStringLiteral("raw"),
                                         QByteArray("\x00\x01" "bytes", 7)) &&
              registry.hasPackageData(QStringLiteral("timers")),
          "package data is detected once the package stores some");
    const QJsonObject timersBundle =
        registry.exportPackageData(QStringLiteral("timers"));
    check(timersBundle.value(QStringLiteral("format")).toString() ==
                  QStringLiteral("forkmesh-package-data") &&
              timersBundle.value(QStringLiteral("packageId")).toString() ==
                  QStringLiteral("timers") &&
              timersBundle.value(QStringLiteral("data")).toObject().size() == 2,
          "the export bundle names its package and carries every key");
    check(registry.uninstall(QStringLiteral("timers"), &dataError) &&
              !registry.hasPackageData(QStringLiteral("timers")),
          "removal wipes the package data the bundle preserved");
    check(!registry.importPackageData(QStringLiteral("crm"), timersBundle,
                                      &dataError) &&
              dataError.contains(QStringLiteral("timers")) &&
              !registry.hasPackageData(QStringLiteral("crm")),
          "a bundle for another package is refused without writing anything");
    QJsonObject damaged = timersBundle;
    damaged.insert(QStringLiteral("data"),
                   QJsonObject{{QStringLiteral("../escape"), 1}});
    check(!registry.importPackageData(QStringLiteral("timers"), damaged,
                                      &dataError) &&
              !registry.hasPackageData(QStringLiteral("timers")),
          "a bundle with an invalid key is refused whole");
    check(registry.importPackageData(QStringLiteral("timers"), timersBundle,
                                     &dataError) &&
              registry.packageSetting(QStringLiteral("timers"),
                                      QStringLiteral("state"))
                      .toString() == QStringLiteral("[\"standup\"]") &&
              registry.packageSetting(QStringLiteral("timers"),
                                      QStringLiteral("raw"))
                      .toByteArray() == QByteArray("\x00\x01" "bytes", 7),
          "importing the bundle restores strings and raw bytes exactly");
    registry.clearPackageData(QStringLiteral("timers"));
    check(!registry.hasPackageData(QStringLiteral("timers")),
          "clearPackageData drops an import whose install failed");

    const QString partialCatalog =
        memberRoot + QStringLiteral("/partial-catalog.json");
    QFile partialFile(partialCatalog);
    check(partialFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              partialFile.write(
                  R"JSON({"schemaVersion":2,"marketplace":{"id":"partial-market"},"packages":[{"id":"maya-theme","version":"1.0.0","manifest":"packages/maya-theme/manifest.json"},{"id":"escape","version":"1.0.0","manifest":"../escape.json"}]})JSON") > 0,
          "partially invalid catalog fixture is written");
    partialFile.close();
    QSettings partialSettings(
        temporary.filePath(QStringLiteral("partial.ini")),
        QSettings::IniFormat);
    forkmesh::market::PackageRegistry partial(&partialSettings,
                                              partialCatalog);
    check(partial.isReady() &&
              partial.package(QStringLiteral("maya-theme")) &&
              !partial.package(QStringLiteral("escape")) &&
              partial.warningString().contains(QStringLiteral("unsafe path")),
          "one invalid package is quarantined without blanking valid listings");

    QSettings statusSettings(temporary.filePath(QStringLiteral("status.ini")),
                             QSettings::IniFormat);
    forkmesh::market::PackageRegistry statusRegistry(&statusSettings);
    const QString statusPageId = QStringLiteral("status-page");
    const QString apiCheckId = QStringLiteral("status-check-api");
    const forkmesh::market::PackageManifest *statusParent =
        statusRegistry.package(statusPageId);
    const forkmesh::market::PackageManifest *apiCheck =
        statusRegistry.package(apiCheckId);
    check(statusParent && statusParent->accountSync &&
              statusParent->dependencies.isEmpty() &&
              statusParent->settings.size() == 2 &&
              statusParent->setting(QStringLiteral("onlyProblems")) &&
              statusParent->setting(QStringLiteral("onlyProblems"))->type ==
                  QStringLiteral("boolean") &&
              statusParent->setting(QStringLiteral("refreshMinutes")) &&
              statusParent->setting(QStringLiteral("refreshMinutes"))
                      ->options.size() == 4,
          "Status Page declares account sync and its two settings");
    check(apiCheck && apiCheck->accountSync &&
              apiCheck->dependencies == QStringList{statusPageId} &&
              apiCheck->statusMonitors.size() == 1 &&
              apiCheck->statusMonitors.first().systems ==
                  QStringList{QStringLiteral("api")},
          "a status check depends on Status Page and names its system");
    int statusChecks = 0;
    for (const forkmesh::market::PackageManifest &manifest :
         statusRegistry.packages()) {
        if (manifest.dependencies.contains(statusPageId) &&
            !manifest.statusMonitors.isEmpty() &&
            statusRegistry.isEffectivelyInstalled(manifest.id))
            ++statusChecks;
    }
    check(statusChecks == 17 &&
              !statusRegistry.isEffectivelyInstalled(
                  QStringLiteral("status-check-mirrors")),
          "every status check except Mirrors is installed by default");
    check(statusRegistry.declaredSetting(statusPageId,
                                         QStringLiteral("onlyProblems")) ==
                  QVariant(false) &&
              statusRegistry.declaredSetting(statusPageId,
                                             QStringLiteral("refreshMinutes")) ==
                  QVariant(QStringLiteral("15")),
          "declared settings fall back to their manifest defaults");
    statusRegistry.setPackageSetting(statusPageId,
                                     QStringLiteral("refreshMinutes"),
                                     QStringLiteral("7"));
    statusRegistry.setPackageSetting(statusPageId,
                                     QStringLiteral("onlyProblems"), true);
    check(statusRegistry.declaredSetting(statusPageId,
                                         QStringLiteral("refreshMinutes")) ==
                  QVariant(QStringLiteral("15")),
          "a stored choice outside the declared options reads as the default");
    statusRegistry.setPackageSetting(statusPageId,
                                     QStringLiteral("refreshMinutes"),
                                     QStringLiteral("5"));
    {
        forkmesh::market::PackageRegistry reread(&statusSettings);
        check(reread.declaredSetting(statusPageId,
                                     QStringLiteral("onlyProblems")) ==
                      QVariant(true) &&
                  reread.declaredSetting(statusPageId,
                                         QStringLiteral("refreshMinutes")) ==
                      QVariant(QStringLiteral("5")),
              "stored declared settings survive a restart with their types");
    }
    check(statusRegistry.statusMonitorShows(QStringLiteral("api")) &&
              statusRegistry.statusMonitorShows(QStringLiteral("core:n1")) &&
              statusRegistry.statusMonitorShows(
                  QStringLiteral("desktop_website")) &&
              !statusRegistry.statusMonitorShows(
                  QStringLiteral("not_a_system")),
          "installed checks cover their systems and prefixes");

    QString statusError;
    QStringList statusChanged;
    check(statusRegistry.uninstall(statusPageId, &statusError,
                                   &statusChanged) &&
              statusError.isEmpty() && statusChanged.size() == 18 &&
              statusChanged.last() == statusPageId &&
              statusChanged.contains(apiCheckId) &&
              !statusRegistry.isInstalled(statusPageId) &&
              !statusRegistry.isInstalled(apiCheckId) &&
              !statusRegistry.statusMonitorShows(QStringLiteral("api")) &&
              !statusRegistry.hasPackageData(statusPageId),
          "removing Status Page removes every check that depends on it");
    check(statusRegistry.missingDependencies(apiCheckId) ==
              QStringList{statusPageId},
          "a removed parent is reported as a missing dependency");
    statusChanged.clear();
    check(statusRegistry.install(apiCheckId, &statusError, &statusChanged) &&
              statusChanged ==
                  (QStringList{statusPageId, apiCheckId}) &&
              statusRegistry.isEffectivelyInstalled(apiCheckId) &&
              statusRegistry.statusMonitorShows(QStringLiteral("api")) &&
              !statusRegistry.isInstalled(QStringLiteral("status-check-cores")) &&
              !statusRegistry.statusMonitorShows(QStringLiteral("core:n1")),
          "installing one check brings Status Page back but not the other checks");
    check(statusRegistry.setInstalledState(statusPageId, false,
                                           &statusError) &&
              statusRegistry.isInstalled(apiCheckId) &&
              !statusRegistry.isEffectivelyInstalled(apiCheckId) &&
              !statusRegistry.statusMonitorShows(QStringLiteral("api")),
          "account state is applied without cascading, and a check without "
          "its parent is not effective");
    check(statusRegistry.setInstalledState(statusPageId, true,
                                           &statusError) &&
              statusRegistry.isEffectivelyInstalled(apiCheckId),
          "restoring the parent makes the check effective again");
    const QStringList nestedChecks = statusRegistry.nestedPackages(statusPageId);
    check(nestedChecks.size() == 18 && nestedChecks.contains(apiCheckId) &&
              statusRegistry.marketParent(apiCheckId) == statusPageId &&
              statusRegistry.marketParent(statusPageId).isEmpty(),
          "every status check is nested under Status Page in the Market");
    statusChanged.clear();
    check(statusRegistry.uninstall(statusPageId, &statusError,
                                   &statusChanged) &&
              statusRegistry.install(statusPageId, &statusError,
                                     &statusChanged),
          "Status Page can be removed and added again");
    int restoredChecks = 0;
    for (const QString &nestedCheck : nestedChecks)
        restoredChecks += statusRegistry.isInstalled(nestedCheck) ? 1 : 0;
    check(restoredChecks == 17 && statusRegistry.isInstalled(apiCheckId) &&
              statusRegistry.isInstalled(QStringLiteral("status-check-cores")) &&
              !statusRegistry.isInstalled(
                  QStringLiteral("status-check-mirrors")),
          "adding Status Page brings back every check that is on by default");

    const QString chainRoot = temporary.filePath(QStringLiteral("chain-market"));
    const auto writeChainPackage = [&](const QString &id,
                                       const QString &dependencies,
                                       const QString &extra) {
        const QString root = chainRoot + QStringLiteral("/packages/") + id;
        QDir().mkpath(root + QStringLiteral("/preview"));
        QFile preview(root + QStringLiteral("/preview/market.png"));
        bool ok = preview.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  preview.write("preview") > 0;
        preview.close();
        QFile manifest(root + QStringLiteral("/manifest.json"));
        ok = ok && manifest.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
             manifest.write(QStringLiteral(R"JSON({
  "schemaVersion": 1,
  "id": "%1",
  "name": "Chain %1",
  "creator": {"id": "did:forkmesh:maya", "handle": "maya", "displayName": "Maya"},
  "type": "extension.native",
  "version": "1.0.0",
  "summary": "A dependency fixture for the package registry test.",
  "license": {"model": "free-open-source", "spdx": "MIT", "creatorRetainsOwnership": true},
  "price": {"amount": 0, "currency": "USD"},
  "entrypoints": {"settings": "builtin:package"},
  "lifecycle": {"migrations": []},
  "permissions": [],
  "dependencies": [%2],
  %3
  "screenshots": [{"path": "preview/market.png", "alt": "Chain preview"}]
})JSON")
                                .arg(id, dependencies, extra)
                                .toUtf8()) > 0;
        manifest.close();
        return ok;
    };
    check(writeChainPackage(QStringLiteral("chain-a"), QString(),
                            QStringLiteral(R"("install": {"bundled": true},)")) &&
              writeChainPackage(QStringLiteral("chain-b"),
                                QStringLiteral(R"({"id": "chain-a"})"),
                                QStringLiteral(R"("install": {"bundled": true},)")) &&
              writeChainPackage(QStringLiteral("chain-c"),
                                QStringLiteral(R"("chain-b")"),
                                QStringLiteral(R"("install": {"bundled": true},)")) &&
              writeChainPackage(QStringLiteral("chain-orphan"),
                                QStringLiteral(R"("chain-missing")"),
                                QStringLiteral(R"("install": {"bundled": true},)")) &&
              writeChainPackage(
                  QStringLiteral("chain-bad-setting"), QString(),
                  QStringLiteral(
                      R"("settings": [{"key": "mode", "type": "choice", "label": "Mode", "default": "z", "options": [{"value": "a", "label": "A"}]}],)")),
          "dependency chain fixtures are written");
    QFile chainCatalog(chainRoot + QStringLiteral("/catalog.json"));
    check(chainCatalog.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              chainCatalog.write(
                  R"JSON({"schemaVersion":2,"marketplace":{"id":"chain-market"},"packages":[{"id":"chain-a","version":"1.0.0","manifest":"packages/chain-a/manifest.json"},{"id":"chain-b","version":"1.0.0","manifest":"packages/chain-b/manifest.json"},{"id":"chain-c","version":"1.0.0","manifest":"packages/chain-c/manifest.json"},{"id":"chain-orphan","version":"1.0.0","manifest":"packages/chain-orphan/manifest.json"},{"id":"chain-bad-setting","version":"1.0.0","manifest":"packages/chain-bad-setting/manifest.json"}]})JSON") > 0,
          "dependency chain catalog is written");
    chainCatalog.close();
    QSettings chainSettings(temporary.filePath(QStringLiteral("chain.ini")),
                            QSettings::IniFormat);
    forkmesh::market::PackageRegistry chain(
        &chainSettings, chainRoot + QStringLiteral("/catalog.json"));
    check(chain.isReady() && chain.package(QStringLiteral("chain-c")) &&
              !chain.package(QStringLiteral("chain-bad-setting")) &&
              chain.warningString().contains(QStringLiteral("invalid settings")),
          "a package with an invalid declared setting is quarantined");
    QString chainError;
    QStringList chainChanged;
    check(chain.missingDependencies(QStringLiteral("chain-c")) ==
              (QStringList{QStringLiteral("chain-a"), QStringLiteral("chain-b")}),
          "missing dependencies are listed dependency-first");
    check(chain.install(QStringLiteral("chain-c"), &chainError, &chainChanged) &&
              chainChanged == (QStringList{QStringLiteral("chain-a"),
                                           QStringLiteral("chain-b"),
                                           QStringLiteral("chain-c")}) &&
              chain.isEffectivelyInstalled(QStringLiteral("chain-c")),
          "installing a package installs its dependencies transitively first");
    chainSettings.setValue(chain.packageDataPrefix(QStringLiteral("chain-c")) +
                               QStringLiteral("/note"),
                           QStringLiteral("kept until removal"));
    check(chain.installedDependents(QStringLiteral("chain-a")) ==
              (QStringList{QStringLiteral("chain-b"), QStringLiteral("chain-c")}),
          "installed dependents are found transitively");
    chainChanged.clear();
    check(chain.uninstall(QStringLiteral("chain-a"), &chainError,
                          &chainChanged) &&
              chainChanged == (QStringList{QStringLiteral("chain-c"),
                                           QStringLiteral("chain-b"),
                                           QStringLiteral("chain-a")}) &&
              !chain.isInstalled(QStringLiteral("chain-b")) &&
              !chain.isInstalled(QStringLiteral("chain-c")) &&
              !chain.hasPackageData(QStringLiteral("chain-c")),
          "removing a package removes every package that depends on it, "
          "with their data");
    chainChanged.clear();
    check(chain.install(QStringLiteral("chain-b"), &chainError,
                        &chainChanged) &&
              chainChanged == (QStringList{QStringLiteral("chain-a"),
                                           QStringLiteral("chain-b")}) &&
              !chain.isInstalled(QStringLiteral("chain-c")),
          "reinstalling a middle package does not bring back its dependents");
    check(!chain.install(QStringLiteral("chain-orphan"), &chainError) &&
              chainError.contains(QStringLiteral("chain-missing")) &&
              !chain.isInstalled(QStringLiteral("chain-orphan")),
          "a dependency missing from the catalog fails the install closed");

    const QDateTime beforeCutoff(QDate(2026, 10, 10), QTime(23, 30),
                                 QTimeZone::UTC);
    const QDateTime afterCutoff(QDate(2026, 10, 11), QTime(0, 0),
                                QTimeZone::UTC);
    check(writeChainPackage(
              QStringLiteral("before-a"), QString(),
              QStringLiteral(
                  R"("install": {"bundled": true, "default": false, "installedBefore": "2026-10-11"},)")) &&
              writeChainPackage(
                  QStringLiteral("before-child"),
                  QStringLiteral(R"({"id": "before-a", "version": "^1.0.0"})"),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": false, "installedBefore": "2026-10-11"},)")) &&
              writeChainPackage(
                  QStringLiteral("before-off"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": false},)")) &&
              writeChainPackage(
                  QStringLiteral("before-bad"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "installedBefore": "soon"},)")),
          "installedBefore fixtures are written");
    QFile beforeCatalog(chainRoot + QStringLiteral("/before-catalog.json"));
    check(beforeCatalog.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              beforeCatalog.write(
                  R"JSON({"schemaVersion":2,"marketplace":{"id":"before-market"},"packages":[{"id":"before-a","version":"1.0.0","manifest":"packages/before-a/manifest.json"},{"id":"before-child","version":"1.0.0","manifest":"packages/before-child/manifest.json"},{"id":"before-off","version":"1.0.0","manifest":"packages/before-off/manifest.json"},{"id":"before-bad","version":"1.0.0","manifest":"packages/before-bad/manifest.json"}]})JSON") > 0,
          "installedBefore catalog is written");
    beforeCatalog.close();
    QSettings beforeSettings(temporary.filePath(QStringLiteral("before.ini")),
                             QSettings::IniFormat);
    forkmesh::market::PackageRegistry before(
        &beforeSettings, chainRoot + QStringLiteral("/before-catalog.json"));
    const forkmesh::market::PackageManifest *beforeA =
        before.package(QStringLiteral("before-a"));
    check(before.isReady() && beforeA &&
              beforeA->installedBefore == QDate(2026, 10, 11) &&
              !beforeA->installedByDefault &&
              !before.package(QStringLiteral("before-off"))
                   ->installedBefore.isValid(),
          "install.installedBefore is parsed as a date");
    check(!before.package(QStringLiteral("before-bad")) &&
              before.warningString().contains(QStringLiteral("installedBefore")),
          "a malformed installedBefore date quarantines the package");
    check(!before.projectCreated().isValid() && before.isLegacyProject() &&
              before.isInstalled(QStringLiteral("before-a")) &&
              before.isEffectivelyInstalled(QStringLiteral("before-child")) &&
              !before.isInstalled(QStringLiteral("before-off")),
          "a project with no creation date keeps every installedBefore "
          "package and nothing that is only default off");
    before.setProjectCreated(beforeCutoff);
    check(before.isLegacyProject() &&
              before.isInstalled(QStringLiteral("before-a")) &&
              before.isInstalled(QStringLiteral("before-child")),
          "a project created before the date starts with the package");
    before.setProjectCreated(afterCutoff);
    check(!before.isLegacyProject() &&
              !before.isInstalled(QStringLiteral("before-a")) &&
              !before.isInstalled(QStringLiteral("before-child")) &&
              !before.isInstalled(QStringLiteral("before-off")),
          "a project created on or after the date starts without it");
    QStringList beforeChanged;
    check(before.install(QStringLiteral("before-child"), &chainError,
                         &beforeChanged) &&
              beforeChanged == (QStringList{QStringLiteral("before-a"),
                                            QStringLiteral("before-child")}) &&
              before.isEffectivelyInstalled(QStringLiteral("before-child")),
          "installing a nested package in a new project installs its parent");
    before.setProjectCreated(beforeCutoff);
    check(before.uninstall(QStringLiteral("before-a"), &chainError) &&
              !before.isInstalled(QStringLiteral("before-a")) &&
              !before.isInstalled(QStringLiteral("before-child")),
          "an explicit removal wins over the legacy fallback");
    before.setProjectCreated(afterCutoff);
    check(before.setInstalledState(QStringLiteral("before-a"), true) &&
              before.isInstalled(QStringLiteral("before-a")),
          "the account answer wins over the new-project fallback");

    QSettings embeddedNewSettings(
        temporary.filePath(QStringLiteral("embedded-new.ini")),
        QSettings::IniFormat);
    forkmesh::market::PackageRegistry embeddedNew(&embeddedNewSettings);
    embeddedNew.setProjectCreated(afterCutoff);
    const forkmesh::market::PackageManifest *agents =
        embeddedNew.package(QStringLiteral("agents"));
    check(agents && agents->accountSync && !agents->installedByDefault &&
              agents->installedBefore == QDate(2026, 10, 11) &&
              agents->desktopEntrypoint == QStringLiteral("builtin:agents"),
          "the Agents core plugin is account-synced and default off");
    bool coreOffInNewProject = true;
    for (const QString &id :
         {QStringLiteral("agents"), QStringLiteral("chat"),
          QStringLiteral("inbox"), QStringLiteral("issues"),
          QStringLiteral("pull-requests"), QStringLiteral("special-agents"),
          QStringLiteral("notes"), QStringLiteral("ledger"),
          QStringLiteral("timers")})
        coreOffInNewProject =
            coreOffInNewProject && embeddedNew.package(id) &&
            !embeddedNew.isEffectivelyInstalled(id);
    check(coreOffInNewProject,
          "a new project starts without the core plugins");
    check(embeddedNew.marketParent(QStringLiteral("pull-requests")) ==
                  QStringLiteral("branches") &&
              embeddedNew.marketParent(QStringLiteral("special-agents")) ==
                  QStringLiteral("agents"),
          "pull requests nest under branches and special agents under agents");
    embeddedNew.setProjectCreated(QDateTime());
    check(embeddedNew.isEffectivelyInstalled(QStringLiteral("pull-requests")) &&
              embeddedNew.isEffectivelyInstalled(QStringLiteral("chat")) &&
              !embeddedNew.isInstalled(QStringLiteral("timers")),
          "the home project keeps the core plugins it had");

    check(writeChainPackage(
              QStringLiteral("place-parent"), QString(),
              QStringLiteral(
                  R"("install": {"bundled": true, "default": true, "sync": "account"},)")) &&
              writeChainPackage(
                  QStringLiteral("place-child"),
                  QStringLiteral(R"("place-parent")"),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": true, "sync": "account", "places": ["web", "desktop"]},)")) &&
              writeChainPackage(
                  QStringLiteral("place-phone"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": true, "sync": "account", "places": ["mobile"]},)")) &&
              writeChainPackage(
                  QStringLiteral("place-device"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": true, "places": ["web"]},)")) &&
              writeChainPackage(
                  QStringLiteral("place-hook"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": false, "sync": "account"},)")) &&
              writeChainPackage(
                  QStringLiteral("place-late"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "default": false, "sync": "account", "installedBefore": "2026-10-14"},)")) &&
              writeChainPackage(
                  QStringLiteral("place-empty"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "sync": "account", "places": []},)")) &&
              writeChainPackage(
                  QStringLiteral("place-unknown"), QString(),
                  QStringLiteral(
                      R"("install": {"bundled": true, "sync": "account", "places": ["watch"]},)")),
          "places fixtures are written");
    QFile placesCatalog(chainRoot + QStringLiteral("/places-catalog.json"));
    check(placesCatalog.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              placesCatalog.write(
                  R"JSON({"schemaVersion":2,"marketplace":{"id":"places-market"},"packages":[{"id":"place-parent","version":"1.0.0","manifest":"packages/place-parent/manifest.json"},{"id":"place-child","version":"1.0.0","manifest":"packages/place-child/manifest.json"},{"id":"place-phone","version":"1.0.0","manifest":"packages/place-phone/manifest.json"},{"id":"place-device","version":"1.0.0","manifest":"packages/place-device/manifest.json"},{"id":"place-hook","version":"1.0.0","manifest":"packages/place-hook/manifest.json"},{"id":"place-late","version":"1.0.0","manifest":"packages/place-late/manifest.json"},{"id":"place-empty","version":"1.0.0","manifest":"packages/place-empty/manifest.json"},{"id":"place-unknown","version":"1.0.0","manifest":"packages/place-unknown/manifest.json"}]})JSON") > 0,
          "places catalog is written");
    placesCatalog.close();
    QSettings placesSettings(temporary.filePath(QStringLiteral("places.ini")),
                             QSettings::IniFormat);
    forkmesh::market::PackageRegistry placed(
        &placesSettings, chainRoot + QStringLiteral("/places-catalog.json"));
    const QStringList everywhere{QStringLiteral("mobile"),
                                 QStringLiteral("desktop"),
                                 QStringLiteral("web")};
    check(placed.isReady() && placed.package(QStringLiteral("place-parent")) &&
              placed.package(QStringLiteral("place-parent"))->places ==
                  everywhere &&
              placed.package(QStringLiteral("place-child"))->places ==
                  (QStringList{QStringLiteral("desktop"),
                               QStringLiteral("web")}),
          "install.places is parsed in a fixed order and defaults to every "
          "place");
    check(!placed.package(QStringLiteral("place-empty")) &&
              !placed.package(QStringLiteral("place-unknown")) &&
              placed.warningString().contains(QStringLiteral("install.places")),
          "an empty or unknown install.places quarantines the package");
    check(placed.places(QStringLiteral("place-parent")) == everywhere &&
              placed.isShownHere(QStringLiteral("place-parent")) &&
              placed.isShownHere(QStringLiteral("place-child")),
          "without a stored choice a package shows everywhere it can");
    check(!placed.isPlacedHere(QStringLiteral("place-phone")) &&
              placed.isInstalled(QStringLiteral("place-phone")) &&
              !placed.isInstalledHere(QStringLiteral("place-phone")) &&
              !placed.isShownHere(QStringLiteral("place-phone")),
          "an installed package that can only show on phones is hidden on "
          "the desktop");
    check(placed.isShownHere(QStringLiteral("place-device")) &&
              !placed.setPlaces(QStringLiteral("place-device"),
                                {QStringLiteral("mobile")}) &&
              placed.isShownHere(QStringLiteral("place-device")),
          "a package that is not account-synced ignores places");
    check(placed.setPlaces(QStringLiteral("place-parent"),
                           {QStringLiteral("web"), QStringLiteral("mobile")}) &&
              placed.places(QStringLiteral("place-parent")) ==
                  (QStringList{QStringLiteral("mobile"),
                               QStringLiteral("web")}) &&
              placed.isInstalled(QStringLiteral("place-parent")) &&
              !placed.isShownHere(QStringLiteral("place-parent")),
          "taking Desktop out of a package's places hides it here but keeps "
          "it installed");
    check(placed.isShownHere(QStringLiteral("place-child")),
          "a dependency only needs to be installed, not placed on the "
          "desktop");
    check(!placed.setPlaces(QStringLiteral("place-parent"),
                            {QStringLiteral("mobile"), QStringLiteral("web")}),
          "storing the same places again is not a change");
    placesSettings.sync();
    const QString placesFile = placesSettings.fileName();
    const QFileDevice::Permissions placesPermissions =
        QFile::permissions(placesFile);
    const bool placesLocked =
        QFile::setPermissions(placesFile, QFileDevice::ReadOwner);
    const bool unchangedPlacesQuiet =
        !placed.setPlaces(QStringLiteral("place-parent"),
                          {QStringLiteral("web"), QStringLiteral("mobile")}) &&
        !placed.setPlaces(QStringLiteral("place-device"),
                          {QStringLiteral("mobile")}) &&
        placesSettings.status() == QSettings::NoError;
    QFile::setPermissions(placesFile, placesPermissions);
    check(placesLocked && unchangedPlacesQuiet,
          "an account answer with unchanged places does not write the "
          "settings file");
    check(placed.setPlaces(QStringLiteral("place-child"),
                           {QStringLiteral("mobile")}) &&
              placed.places(QStringLiteral("place-child")).isEmpty() &&
              !placed.isShownHere(QStringLiteral("place-child")),
          "a stored place the package cannot show in is dropped");
    check(placed.setPlaces(QStringLiteral("place-parent"), {}) &&
              placed.places(QStringLiteral("place-parent")) == everywhere &&
              placed.isShownHere(QStringLiteral("place-parent")),
          "an account entry without places means every place");
    check(placed.setPlaces(QStringLiteral("place-parent"),
                           {QStringLiteral("desktop")}) &&
              placed.uninstall(QStringLiteral("place-parent")) &&
              !placed.isShownHere(QStringLiteral("place-parent")) &&
              placed.install(QStringLiteral("place-parent")) &&
              placed.places(QStringLiteral("place-parent")) ==
                  QStringList{QStringLiteral("desktop")} &&
              placed.isShownHere(QStringLiteral("place-parent")),
          "removing and installing again keeps the chosen places");
    const forkmesh::market::PackageManifest *hook =
        placed.package(QStringLiteral("place-hook"));
    const forkmesh::market::PackageManifest *late =
        placed.package(QStringLiteral("place-late"));
    placed.setProjectCreated(QDateTime());
    check(hook && late && placed.legacyEntryInstalled(*hook) &&
              placed.legacyEntryInstalled(*late) &&
              !placed.installedWithoutChoice(*hook),
          "a legacy account entry installs a package without installedBefore "
          "in the home project");
    placed.setProjectCreated(beforeCutoff);
    check(hook && late && placed.legacyEntryInstalled(*hook) &&
              placed.legacyEntryInstalled(*late),
          "a legacy account entry installs in a project from before the core "
          "plugins");
    const QDateTime beforeWorkflowsCutoff(QDate(2026, 10, 13), QTime(9, 0),
                                          QTimeZone::UTC);
    placed.setProjectCreated(beforeWorkflowsCutoff);
    check(hook && late && !placed.legacyEntryInstalled(*hook) &&
              placed.legacyEntryInstalled(*late),
          "a project made between the two dates keeps a package dated later "
          "but not one without a date");
    placed.setProjectCreated(
        QDateTime(QDate(2026, 10, 14), QTime(0, 0), QTimeZone::UTC));
    check(hook && late && !placed.legacyEntryInstalled(*hook) &&
              !placed.legacyEntryInstalled(*late),
          "a legacy account entry does not install in a new project");

    QSettings ownedSettings(temporary.filePath(QStringLiteral("owned.ini")),
                            QSettings::IniFormat);
    forkmesh::market::PackageRegistry owned(
        &ownedSettings, chainRoot + QStringLiteral("/places-catalog.json"));
    const forkmesh::market::PackageManifest *ownedHook =
        owned.package(QStringLiteral("place-hook"));
    const forkmesh::market::PackageManifest *ownedParent =
        owned.package(QStringLiteral("place-parent"));
    owned.setProjectCreated(beforeCutoff);
    check(ownedHook && ownedParent && !owned.projectOwnsInstalls() &&
              owned.accountAnswerInstalls(*ownedHook, true, false) &&
              !owned.accountAnswerInstalls(*ownedParent, false, false) &&
              owned.accountAnswerInstalls(*ownedHook, false, true),
          "a legacy project follows the account answer");
    owned.setProjectCreated(afterCutoff);
    check(ownedHook && owned.projectOwnsInstalls() &&
              !owned.isInstalled(QStringLiteral("place-hook")) &&
              !owned.accountAnswerInstalls(*ownedHook, true, false),
          "an account install does not turn a package on in a new project");
    check(ownedHook && !owned.accountAnswerInstalls(*ownedHook, true, true) &&
              !owned.accountAnswerInstalls(*ownedHook, false, true),
          "a legacy account entry does not turn a package on in a new "
          "project");
    check(ownedParent && owned.isInstalled(QStringLiteral("place-parent")) &&
              owned.accountAnswerInstalls(*ownedParent, true, false) &&
              owned.accountAnswerInstalls(*ownedParent, false, true),
          "a package installed in a new project stays on while the account "
          "has it");
    check(ownedParent && !owned.accountAnswerInstalls(*ownedParent, false,
                                                      false),
          "removing a package from the account removes it from a new "
          "project");
    check(ownedHook && owned.install(QStringLiteral("place-hook")) &&
              owned.accountAnswerInstalls(*ownedHook, true, false) &&
              owned.accountAnswerInstalls(*ownedHook, true, true),
          "a package installed in this new project keeps its own install");
    check(ownedParent && owned.uninstall(QStringLiteral("place-parent")) &&
              !owned.accountAnswerInstalls(*ownedParent, true, false) &&
              !owned.accountAnswerInstalls(*ownedParent, true, true),
          "a package removed from a new project stays off while other "
          "projects keep it");
    check(ownedHook && ownedParent &&
              owned.hasStoredInstall(QStringLiteral("place-hook")) &&
              owned.accountAnswerInstalls(*ownedHook, false, false, true) &&
              !owned.hasStoredInstall(QStringLiteral("place-parent")) &&
              !owned.accountAnswerInstalls(*ownedParent, false, false, true),
          "a desktop install made before the account ever chose the package "
          "is kept so it can be carried to the account");
    owned.setProjectCreated(QDateTime());
    check(!owned.projectOwnsInstalls() && ownedParent &&
              owned.accountAnswerInstalls(*ownedParent, true, false),
          "the home project follows the account");

    QSettings workflowsSettings(
        temporary.filePath(QStringLiteral("workflows.ini")),
        QSettings::IniFormat);
    forkmesh::market::PackageRegistry workflows(&workflowsSettings);
    const forkmesh::market::PackageManifest *workflowsPackage =
        workflows.package(QStringLiteral("workflows"));
    check(workflowsPackage && workflowsPackage->accountSync &&
              !workflowsPackage->installedByDefault &&
              workflowsPackage->installedBefore == QDate(2026, 10, 14) &&
              workflowsPackage->desktopEntrypoint ==
                  QStringLiteral("builtin:workflows") &&
              workflowsPackage->places == everywhere,
          "Workflows is an account-synced package, off by default, dated "
          "2026-10-14");
    workflows.setProjectCreated(beforeWorkflowsCutoff);
    check(workflows.isShownHere(QStringLiteral("workflows")),
          "a project created before 2026-10-14 keeps Workflows");
    workflows.setProjectCreated(
        QDateTime(QDate(2026, 10, 14), QTime(0, 0), QTimeZone::UTC));
    bool workflowsFamilyOff = !workflows.isShownHere(QStringLiteral("workflows"));
    for (const QString &nested :
         workflows.nestedPackages(QStringLiteral("workflows")))
        workflowsFamilyOff =
            workflowsFamilyOff && !workflows.isEffectivelyInstalled(nested);
    check(workflowsFamilyOff &&
              !workflows.nestedPackages(QStringLiteral("workflows")).isEmpty(),
          "a new project starts without Workflows and the packages nested "
          "under it");
    const forkmesh::market::PackageManifest *foodGuide =
        workflows.package(QStringLiteral("food-guide"));
    check(foodGuide &&
              foodGuide->places == QStringList{QStringLiteral("mobile")} &&
              !workflows.isPlacedHere(QStringLiteral("food-guide")),
          "Food Guide only shows on phones");

    const QString projectDir = temporary.filePath(QStringLiteral("project"));
    QDir().mkpath(projectDir);
    check(!forkmesh::market::PackageRegistry::readProjectCreated(projectDir)
               .isValid(),
          "a project folder without project.json has no creation date");
    QFile projectFile(projectDir + QStringLiteral("/project.json"));
    check(projectFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              projectFile.write(R"({"schema":1,"slug":"p","name":"P"})") > 0,
          "a project.json without created is written");
    projectFile.close();
    check(!forkmesh::market::PackageRegistry::readProjectCreated(projectDir)
               .isValid(),
          "a project.json without created counts as an old project");
    check(projectFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              projectFile.write(
                  R"({"schema":1,"slug":"p","name":"P","created":"2026-10-12T08:15:00Z"})") > 0,
          "a project.json with created is written");
    projectFile.close();
    check(forkmesh::market::PackageRegistry::readProjectCreated(projectDir) ==
              QDateTime(QDate(2026, 10, 12), QTime(8, 15), QTimeZone::UTC),
          "project.json created is read as UTC");

    return failures == 0 ? 0 : 1;
}
