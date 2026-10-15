#include "CrashHandler.h"
#include "PackageModules.h"
#include "ProcessReaper.h"
#include "AgentCliLocator.h"
#include "AgentResourceGovernor.h"
#include "AppPaths.h"
#include "ControlNode.h"
#include "DirectorySizeScan.h"
#include "DesktopDisplayReadiness.h"
#if defined(FORKMESH_HAVE_TERM)
#include "ForkMeshTerm.h"
#endif
#include "ForkMeshIdentity.h"
#include "MainWindow.h"
#include "CoreMirrorActions.h"
#include "MirrorActionsConfiguration.h"
#include "PlatformLogFilter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "SingleInstance.h"
#include "TrayMode.h"
#include "SystemOpenFolder.h"
#include "SplitterHairline.h"
#include "StartupSplash.h"
#include "StartupTrace.h"
#include "SystemStats.h"
#include "Theme.h"
#include "UiSounds.h"
#include "UserActivityLog.h"
#include "VirtualMachineRuntime.h"

#if __has_include("ForkMeshVersion.h")
#include "ForkMeshVersion.h"
#endif
#if __has_include("ForkMeshBuildCommit.h")
#include "ForkMeshBuildCommit.h"
#endif
#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif
#ifndef FORKMESH_BUILD_COMMIT
#define FORKMESH_BUILD_COMMIT "unknown"
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QAbstractButton>
#include <QPushButton>
#include <QMessageBox>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>
#include <cstdlib>
#include <utility>

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <unistd.h>
#endif

namespace {

QStringList launchArguments(int argc, char *argv[]);
void configureApplicationIdentity(QCoreApplication &app, const QStringList &arguments);

QJsonObject readBoundedHelperRequest(const char *label, bool *ok)
{
    if (ok)
        *ok = false;
    QByteArray input;
    char buffer[4096];
    while (!std::feof(stdin) && input.size() <= 128 * 1024) {
        const std::size_t read =
            std::fread(buffer, 1, sizeof(buffer), stdin);
        if (read > 0)
            input.append(buffer, int(read));
        if (std::ferror(stdin))
            break;
    }
    if (input.isEmpty() || input.size() > 128 * 1024) {
        std::fprintf(stderr, "%s: invalid request size\n", label);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(input, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        std::fprintf(stderr, "%s: invalid JSON request\n", label);
        return {};
    }
    if (ok)
        *ok = true;
    return document.object();
}

int runSizeMapScan(const QString &requestPath)
{
    QFile request(requestPath);
    if (!request.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "size-map-scan: cannot read request\n");
        return 2;
    }
    const QByteArray payload = request.read(16 * 1024 * 1024);
    QString path;
    forkmesh::DirectorySizeScanOptions options;
    if (!forkmesh::decodeScanRequest(payload, &path, &options)) {
        std::fprintf(stderr, "size-map-scan: invalid request\n");
        return 2;
    }
    if (!QFileInfo(path).isDir()) {
        std::fprintf(stderr, "size-map-scan: not a directory\n");
        return 2;
    }
    const auto progress =
        [](const forkmesh::DirectorySizeScanProgressUpdate &update) {
            const QByteArray line = forkmesh::encodeScanProgress(update);
            std::fwrite(line.constData(), 1, std::size_t(line.size()), stderr);
            std::fflush(stderr);
        };
    const QByteArray result = forkmesh::encodeScanResult(
        forkmesh::scanDirectorySizes(path, options, progress));
    std::fwrite(result.constData(), 1, std::size_t(result.size()), stdout);
    std::fflush(stdout);
    return 0;
}

void writeHelperResponse(const QJsonObject &response)
{
    const QByteArray bytes =
        QJsonDocument(response).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int runPublicMirrorMaterializer(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    configureApplicationIdentity(app, launchArguments(argc, argv));
    bool parsed = false;
    const QJsonObject request =
        readBoundedHelperRequest("public mirror materializer", &parsed);
    ForkMeshIdentity identity;
    if (!parsed || !identity.load() || !identity.isValid()) {
        std::fputs(
            "public mirror materializer: local identity unavailable\n",
            stderr);
        return 2;
    }
    const QByteArray canonical =
        QByteArrayLiteral("forkmesh-public-age-vault-unlock-v1\n") +
        identity.publicKey().toUtf8();
    QByteArray vaultSecret = QByteArray::fromBase64(
        identity.signData(canonical).toLatin1(),
        QByteArray::Base64UrlEncoding);
    const QString dataRoot =
        forkmesh::paths::dataRoot();
    QString error;
    const QJsonObject response =
        PublicMirrorRuntime::materializeGatewayRequest(
            request,
            QDir(dataRoot).filePath(
                QStringLiteral("public-mirror-archives")),
            QDir(dataRoot).filePath(
                QStringLiteral("identity/public-age-vault.json")),
            vaultSecret, PublicMirrorRuntime::Tools(), &error);
    vaultSecret.fill('\0');
    vaultSecret.clear();
    if (response.isEmpty()) {
        std::fputs(
            "public mirror materializer: request rejected\n", stderr);
        return 3;
    }
    writeHelperResponse(response);
    return 0;
}

bool isCloudflareSetupLink(const QString &value)
{
    const QUrl url(value);
    if (!url.isValid() || url.scheme() != QLatin1String("forkmesh") ||
        url.host() != QLatin1String("control") ||
        url.path() != QLatin1String("/cloudflare") ||
        !url.userInfo().isEmpty() || !url.fragment().isEmpty())
        return false;
    const QUrlQuery query(url);
    static const QRegularExpression prohibited(
        QStringLiteral(
            "(?:token|secret|password|private|credential|api[_-]?key)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (prohibited.match(item.first).hasMatch())
            return false;
    }
    return value.size() <= 4096;
}

bool repositoryForkLinkParts(const QString &value, QString *owner = nullptr,
                             QString *repository = nullptr)
{
    const QUrl url(value);
    if (!url.isValid() || value.size() > 512 ||
        url.scheme() != QLatin1String("forkmesh") ||
        url.host() != QLatin1String("fork") || !url.userInfo().isEmpty() ||
        url.port(-1) != -1 || !url.query().isEmpty() ||
        !url.fragment().isEmpty()) {
        return false;
    }
    const QStringList parts =
        url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    static const QRegularExpression segment(
        QStringLiteral("^[A-Za-z0-9._:-]{1,80}$"));
    if (parts.size() != 2 || !segment.match(parts.at(0)).hasMatch() ||
        !segment.match(parts.at(1)).hasMatch()) {
        return false;
    }
    if (owner)
        *owner = parts.at(0).toLower();
    if (repository)
        *repository = parts.at(1);
    return true;
}

bool agentLoginLinkParts(const QString &value, QString *node = nullptr,
                         QString *provider = nullptr)
{
    const QUrl url(value);
    if (!url.isValid() || value.size() > 512 ||
        url.scheme() != QLatin1String("forkmesh") ||
        url.host() != QLatin1String("agent-login") ||
        !url.userInfo().isEmpty() || url.port(-1) != -1 ||
        !url.fragment().isEmpty()) {
        return false;
    }
    const QStringList parts =
        url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    static const QRegularExpression nodePattern(
        QStringLiteral("^[A-Za-z0-9._:-]{1,80}$"));
    const QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    if (parts.size() != 1 || !nodePattern.match(parts.first()).hasMatch() ||
        items.size() != 1 || items.first().first != QLatin1String("provider") ||
        (items.first().second != QLatin1String("claude-code") &&
         items.first().second != QLatin1String("codex"))) {
        return false;
    }
    if (node)
        *node = parts.first().toLower();
    if (provider)
        *provider = items.first().second;
    return true;
}

bool isLocalActionLink(const QString &value)
{
    return isCloudflareSetupLink(value) || repositoryForkLinkParts(value) ||
           agentLoginLinkParts(value);
}

QStringList launchArguments(int argc, char *argv[])
{
    QStringList arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    return arguments;
}

// The one place the application names are set, for the desktop and for every
// helper mode, followed by the layout that decides where this process's files
// live. Helpers inherit FORKMESH_PROJECT from the desktop that spawned them.
void configureApplicationIdentity(QCoreApplication &app, const QStringList &arguments)
{
    app.setApplicationName(QStringLiteral("ForkMesh"));
    app.setOrganizationName(QStringLiteral("ForkMesh"));
    forkmesh::paths::activate(arguments);
}

const char *moveOutcomeText(forkmesh::paths::MoveResult::Outcome outcome)
{
    using Outcome = forkmesh::paths::MoveResult::Outcome;
    switch (outcome) {
    case Outcome::Moved:
        return "move";
    case Outcome::AlreadyDone:
        return "done";
    case Outcome::Skipped:
        return "SKIP";
    case Outcome::Failed:
        return "FAIL";
    }
    return "";
}

// forkmesh --migrate-layout [--dry-run] [--project <name>]
// Prints where every root resolves and moves the legacy locations into the
// projects layout. The desktop does the same on its own at startup; this mode
// exists to preview it and to run it on a headless box.
int runLayoutMigration(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = launchArguments(argc, argv);
    const bool dryRun = arguments.contains(QStringLiteral("--dry-run"));
    app.setApplicationName(QStringLiteral("ForkMesh"));
    app.setOrganizationName(QStringLiteral("ForkMesh"));
    forkmesh::paths::activate(arguments, /*createFolders=*/!dryRun);
    const forkmesh::paths::Layout &before = forkmesh::paths::layout();
    std::printf("project   %s\nfolder    %s\n", qPrintable(before.projectSlug),
                qPrintable(before.projectDir));
    if (!before.rerootAllowed) {
        std::printf("blocked   %s\n", qPrintable(before.rerootBlockedReason));
        return 3;
    }
    if (!dryRun && !forkmesh::acquireSingleInstance()) {
        std::fputs("ForkMesh is running for this project. It migrates on its own "
                   "the next time it starts; nothing was moved.\n",
                   stderr);
        return 4;
    }
    int failures = 0;
    for (const forkmesh::paths::MoveResult &result :
         forkmesh::paths::migrateNow(dryRun)) {
        using Outcome = forkmesh::paths::MoveResult::Outcome;
        failures += result.outcome == Outcome::Failed ? 1 : 0;
        std::printf("%s  %s\n      -> %s%s%s\n", moveOutcomeText(result.outcome),
                    qPrintable(result.move.from), qPrintable(result.move.to),
                    result.detail.isEmpty() ? "" : "   ",
                    qPrintable(result.detail));
    }
    if (!dryRun) {
        forkmesh::rehomeSingleInstanceLock();
        forkmesh::releaseSingleInstance();
    }
    const forkmesh::paths::Layout &after = forkmesh::paths::layout();
    std::printf("data      %s\nconfig    %s\ncache     %s\nstate     %s\n"
                "logs      %s\nssh       %s\n",
                qPrintable(after.data), qPrintable(after.config),
                qPrintable(after.cache), qPrintable(after.state),
                qPrintable(after.logs), qPrintable(after.ssh));
    return failures == 0 ? 0 : 1;
}

class ForkMeshApplication : public QApplication
{
public:
    using QApplication::QApplication;

    void setLocalLinkHandler(std::function<void(const QString &)> handler)
    {
        m_localLinkHandler = std::move(handler);
        if (!m_pendingLocalLink.isEmpty() && m_localLinkHandler) {
            const QString pending = std::exchange(m_pendingLocalLink, QString());
            m_localLinkHandler(pending);
        }
    }

    bool event(QEvent *event) override
    {
        if (event && event->type() == QEvent::FileOpen) {
            const auto *open = static_cast<QFileOpenEvent *>(event);
            const QString link = open->url().toString(QUrl::FullyEncoded);
            const QString target =
                isLocalActionLink(link)
                    ? link
                    : forkmesh::systemopen::folderFromArgument(link);
            if (!target.isEmpty()) {
                if (m_localLinkHandler)
                    m_localLinkHandler(target);
                else
                    m_pendingLocalLink = target;
                return true;
            }
        }
        return QApplication::event(event);
    }

    bool notify(QObject *receiver, QEvent *event) override
    {
        forkmesh::ui::QtEventDeliveryProbe diagnosticProbe(receiver, event);
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception &e) {
            forkmesh::logCaughtFault(describe(receiver, event),
                                     QString::fromUtf8(e.what()));
        } catch (...) {
            forkmesh::logCaughtFault(describe(receiver, event),
                                     QStringLiteral("(non-std exception)"));
        }
        return false;
    }

private:
    std::function<void(const QString &)> m_localLinkHandler;
    QString m_pendingLocalLink;

    static QString describe(QObject *receiver, QEvent *event)
    {
        const QString cls = receiver && receiver->metaObject()
                                ? QString::fromLatin1(receiver->metaObject()->className())
                                : QStringLiteral("(null)");
        const QString name = receiver ? receiver->objectName() : QString();
        const int type = event ? int(event->type()) : -1;
        return QStringLiteral("event delivery to %1%2 (event type %3)")
            .arg(cls,
                 name.isEmpty() ? QString() : QStringLiteral(" \"%1\"").arg(name))
            .arg(type);
    }
};

// External signer used by tools/cloudflare_bootstrap.py. The bootstrapper sends
// a public mirror-manifest request on stdin; this short-lived mode validates it,
// signs the exact canonical payload with the existing local node identity, and
// writes only the public key + detached signature to stdout. The private key
// never crosses the process boundary, enters argv, or reaches Cloudflare.
int runMirrorManifestSigner(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    configureApplicationIdentity(app, launchArguments(argc, argv));

    QByteArray input;
    char buffer[4096];
    while (!std::feof(stdin) && input.size() <= 128 * 1024) {
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (read > 0)
            input.append(buffer, int(read));
        if (std::ferror(stdin))
            break;
    }
    if (input.isEmpty() || input.size() > 128 * 1024) {
        std::fputs("manifest signer: invalid request size\n", stderr);
        return 2;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(input, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        std::fputs("manifest signer: invalid JSON request\n", stderr);
        return 2;
    }

    ForkMeshIdentity identity;
    if (!identity.load() || !identity.isValid()) {
        std::fputs("manifest signer: local identity is unavailable\n", stderr);
        return 3;
    }
    QString validationError;
    const QByteArray payload = forkmesh::control::mirrorManifestSigningPayload(
        document.object(), identity.publicKey(), &validationError);
    if (payload.isEmpty()) {
        const QByteArray safe =
            QStringLiteral("manifest signer: %1\n")
                .arg(validationError)
                .toUtf8();
        std::fwrite(safe.constData(), 1, std::size_t(safe.size()), stderr);
        return 2;
    }
    const QString signature = identity.signData(payload);
    if (signature.isEmpty()) {
        std::fputs("manifest signer: signing failed\n", stderr);
        return 3;
    }
    const QByteArray response =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("publicKey"), identity.publicKey()},
                          {QStringLiteral("signature"), signature},
                      })
            .toJson(QJsonDocument::Compact);
    std::fwrite(response.constData(), 1, std::size_t(response.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

// Shown once the main window is up when startup could not recover a mirror
// Actions configuration transaction. Non-modal: the app is fully usable
// behind it, only mirror Actions stay suspended until Retry or Reset works.
void showMirrorActionsRecoveryWarning(QWidget *parent)
{
    namespace ma = forkmesh::mirror_actions;
    const ma::StartupRecoveryStatus status = ma::startupRecoveryStatus();
    if (!status.attempted || status.ok)
        return;
    auto *box = new QMessageBox(
        QMessageBox::Warning, QStringLiteral("ForkMesh"),
        QStringLiteral(
            "Mirror Actions are paused: ForkMesh could not recover an "
            "interrupted mirror Actions configuration (%1).\n\n"
            "Everything else works normally. Retry once any other ForkMesh "
            "process has exited, or reset the Actions configuration to clear "
            "the stuck lock and journal files.")
            .arg(status.errorCode),
        QMessageBox::NoButton, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setDetailedText(QStringLiteral("Lock file: %1\nJournal file: %2")
                             .arg(status.lockPath, status.journalPath));
    QAbstractButton *retry =
        box->addButton(QStringLiteral("Retry"), QMessageBox::AcceptRole);
    QAbstractButton *reset = box->addButton(
        QStringLiteral("Reset Actions configuration…"),
        QMessageBox::DestructiveRole);
    box->addButton(QStringLiteral("Continue without Actions"),
                   QMessageBox::RejectRole);
    const auto retryNow = [parent] {
        QSettings settings;
        const ma::StartupRecoveryStatus again = ma::recoverAtStartup(settings);
        if (again.ok) {
            qInfo().noquote() << "Mirror Actions recovery succeeded on retry; "
                                 "mirror Actions resumed.";
            return;
        }
        qWarning().noquote() << "Mirror Actions recovery retry failed:"
                             << again.errorCode;
        QTimer::singleShot(0, parent, [parent] {
            showMirrorActionsRecoveryWarning(parent);
        });
    };
    QObject::connect(
        box, &QMessageBox::buttonClicked, parent,
        [parent, retry, reset, retryNow, status](QAbstractButton *clicked) {
            if (clicked == retry) {
                QTimer::singleShot(0, parent, retryNow);
                return;
            }
            if (clicked != reset)
                return;
            auto *confirm = new QMessageBox(
                QMessageBox::Question, QStringLiteral("ForkMesh"),
                QStringLiteral(
                    "Reset the mirror Actions configuration?\n\nThis deletes "
                    "only these two files (if present) and does not touch "
                    "your account, keys, repositories or settings:\n\n%1\n%2\n\n"
                    "An interrupted configuration change will not be rolled "
                    "back; re-apply it from the mirror's Actions settings.")
                    .arg(status.lockPath, status.journalPath),
                QMessageBox::Yes | QMessageBox::Cancel, parent);
            confirm->setAttribute(Qt::WA_DeleteOnClose);
            confirm->setWindowModality(Qt::NonModal);
            confirm->setDefaultButton(QMessageBox::Cancel);
            QObject::connect(
                confirm, &QMessageBox::finished, parent,
                [parent, retryNow](int result) {
                    if (result != QMessageBox::Yes)
                        return;
                    QSettings settings;
                    QString error;
                    const QStringList removed =
                        ma::resetConfigurationRecoveryFiles(settings, &error);
                    qInfo().noquote()
                        << "Mirror Actions reset removed"
                        << (removed.isEmpty() ? QStringLiteral("nothing")
                                              : removed.join(QStringLiteral(", ")))
                        << (error.isEmpty() ? QString()
                                            : QStringLiteral("(%1)").arg(error));
                    QTimer::singleShot(0, parent, retryNow);
                });
            confirm->show();
        });
    box->show();
}


} // namespace

int main(int argc, char *argv[])
{
#if defined(FORKMESH_HAVE_TERM)
    if (argc > 0 && (qstrcmp(argv[0], "forkmesh-term") == 0 ||
                     QByteArray(argv[0]).endsWith("/forkmesh-term")))
        return forkmesh::term::main(argc, argv);
    if (argc > 1 && qstrcmp(argv[1], "term") == 0)
        return forkmesh::term::main(argc, argv);
#endif
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--version") == 0) {
            std::printf("ForkMesh %s\n", FORKMESH_VERSION);
            return 0;
        }
        if (qstrcmp(argv[i], "--tray") == 0)
            return forkmesh::tray::run(argc, argv);
        if (qstrcmp(argv[i], "--headless") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            qputenv("FORKMESH_NO_SPLASH", "1");
            forkmesh::setBackgroundInstance();
        }
        if (qstrcmp(argv[i], "--build-commit") == 0) {
            std::printf("%s\n", FORKMESH_BUILD_COMMIT);
            static const QRegularExpression exactCommit(
                QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
            return exactCommit.match(QStringLiteral(FORKMESH_BUILD_COMMIT))
                       .hasMatch()
                       ? 0
                       : 1;
        }
    }

    // Before any thread exists: the PATH a service or launcher hands us may
    // not reach the user's own install directories (~/.local/bin holds
    // `claude`), and everything this process starts inherits it.
    const QStringList pathAdded =
        forkmesh::agentcli::addUserInstallDirsToProcessPath();
    forkmesh::processreaper::initializeInstance();

    forkmesh::beginStartupTrace();
    if (!pathAdded.isEmpty())
        forkmesh::logStartupTrace(
            QStringLiteral("added to PATH: %1")
                .arg(pathAdded.join(QDir::listSeparator())));
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--size-map-scan") == 0)
            return runSizeMapScan(QString::fromLocal8Bit(argv[i + 1]));
    }

    {
        forkmesh::StartupTraceStep step(QStringLiteral("raise open file limit"));
        SystemStats::raiseOpenFileLimit();
    }

    {
        forkmesh::StartupTraceStep step(QStringLiteral("install crash handler"));
        forkmesh::installCrashHandler(
            QString(),
            forkmesh::paths::earlyMainLogPath(argc, argv));
    }
    forkmesh::logStartupTrace(
        QStringLiteral("crash handler installed; parsing launch arguments"));

    const QStringList rawArgs = launchArguments(argc, argv);
    QString localActionLink;
    for (const QString &argument : std::as_const(rawArgs)) {
        if (isLocalActionLink(argument)) {
            localActionLink = argument;
            break;
        }
    }
    if (localActionLink.isEmpty())
        localActionLink = forkmesh::systemopen::folderFromLaunchArguments(rawArgs);

    if (rawArgs.contains(QStringLiteral("--migrate-layout")))
        return runLayoutMigration(argc, argv);
    if (rawArgs.contains(QStringLiteral("--sign-mirror-manifest")))
        return runMirrorManifestSigner(argc, argv);
    if (rawArgs.contains(QStringLiteral("--materialize-public-mirror")))
        return runPublicMirrorMaterializer(argc, argv);
    if (rawArgs.contains(
            QStringLiteral("--configure-mirror-actions-stdin"))) {
        forkmesh::coremirroractions::install();
        return forkmesh::mirror_actions::runConfigurationStdin(argc, argv);
    }

    forkmesh::logStartupTrace(
        QStringLiteral("launch mode resolved: desktop; Qt platform=%1")
            .arg(qEnvironmentVariable("QT_QPA_PLATFORM",
                                      QStringLiteral("automatic"))));

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    constexpr int kDesktopDisplayWaitMs = 30'000;
    constexpr int kDesktopDisplayPollMs = 100;
    const QProcessEnvironment startupEnvironment =
        QProcessEnvironment::systemEnvironment();
    forkmesh::DesktopDisplayReadiness display =
        forkmesh::inspectDesktopDisplay(startupEnvironment);
    if (display.state == forkmesh::DesktopDisplayState::WaitingForEndpoint) {
        forkmesh::logStartupTrace(
            QStringLiteral("desktop session is starting; waiting up to %1ms (%2)")
                .arg(kDesktopDisplayWaitMs)
                .arg(display.detail));
        display = forkmesh::waitForDesktopDisplay(
            startupEnvironment, kDesktopDisplayWaitMs, kDesktopDisplayPollMs);
    }
    if (display.state != forkmesh::DesktopDisplayState::Ready) {
        const QString message =
            QStringLiteral("ForkMesh cannot start its desktop UI: %1")
                .arg(display.detail);
        forkmesh::logStartupTrace(message);
        qCritical().noquote() << message;
        const QByteArray diagnostic = message.toUtf8() + '\n';
        std::fwrite(diagnostic.constData(), 1,
                    static_cast<std::size_t>(diagnostic.size()), stderr);
        std::fflush(stderr);
        return 3;
    }
    forkmesh::logStartupTrace(
        QStringLiteral("desktop display ready after %1ms (%2)")
            .arg(display.waitedMs)
            .arg(display.detail));
#endif

    forkmesh::installPlatformLogFilter();
    forkmesh::logStartupTrace(QStringLiteral("Qt platform log filter installed"));

#ifdef FORKMESH_HAVE_WEBENGINE
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif
    forkmesh::logStartupTrace(QStringLiteral("BEGIN construct QApplication"));
    QElapsedTimer applicationConstruction;
    applicationConstruction.start();
    ForkMeshApplication app(argc, argv);
    forkmesh::logStartupTrace(
        QStringLiteral("DONE  construct QApplication (%1ms)")
            .arg(applicationConstruction.elapsed()));
    forkmesh::market::modules::install();
    {
        forkmesh::StartupTraceStep applicationSetup(
            QStringLiteral("configure QApplication metadata, icon and style"));
        configureApplicationIdentity(app, rawArgs);
        app.setWindowIcon(QIcon(QStringLiteral(":/app/forkmesh.png")));
        QGuiApplication::setDesktopFileName(QStringLiteral("forkmesh"));
        app.setStyle(QStyleFactory::create("Fusion"));
        forkmesh::ui::SplitterHairline::install(&app);
    }

    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("resolve work runtime (KVM/host)"));
        forkmesh::vm::initialize(
            QSettings().value(forkmesh::vm::kEnabledSetting, false).toBool());
    }
    forkmesh::logStartupTrace(
        QStringLiteral("work runtime: %1; launch directory=%2")
            .arg(forkmesh::vm::active() ? QStringLiteral("KVM")
                                        : QStringLiteral("host"),
                 forkmesh::vm::workspaceRoot()));

    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("acquire the single-instance lock"));
        bool takingOver = false;
        const auto waitForBackgroundInstance = [&takingOver] {
            if (!takingOver) {
                takingOver = true;
                forkmesh::logStartupTrace(QStringLiteral(
                    "ForkMesh runs without a window; asked it to hand over"));
                forkmesh::ui::showStartupSplash(QStringLiteral(FORKMESH_VERSION),
                                                QStringLiteral(FORKMESH_BUILD_COMMIT));
                forkmesh::ui::startupStep(QStringLiteral(
                    "Taking over from ForkMesh running without a window"));
            }
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        };
        if (!forkmesh::acquireSingleInstance(localActionLink,
                                             waitForBackgroundInstance)) {
            qInfo().noquote()
                << "ForkMesh is already running; focusing the existing window.";
            return 0;
        }
    }

    {
        forkmesh::StartupTraceStep step(QStringLiteral("show startup splash"));
        forkmesh::ui::showStartupSplash(QStringLiteral(FORKMESH_VERSION),
                                        QStringLiteral(FORKMESH_BUILD_COMMIT));
    }

    // A project folder that is itself a git checkout would have application
    // data committed along with the code. Only the home project can wait that
    // out on its legacy locations; any other project has nowhere else to go.
    if (!forkmesh::paths::layout().rerootAllowed &&
        !forkmesh::paths::layout().isHomeProject()) {
        forkmesh::ui::dismissStartupSplash();
        QMessageBox::critical(
            nullptr, QStringLiteral("ForkMesh"),
            QStringLiteral("This project cannot be opened.\n\n%1")
                .arg(forkmesh::paths::layout().rerootBlockedReason));
        return 2;
    }

    // Sole instance proven: nothing else has the legacy folders open by name,
    // so they can move into the projects layout. Renames only, so this costs
    // milliseconds regardless of size, and every entry is safe to repeat.
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("move legacy locations into the projects layout"));
        for (const forkmesh::paths::MoveResult &result :
             forkmesh::paths::migrateNow()) {
            using Outcome = forkmesh::paths::MoveResult::Outcome;
            if (result.outcome == Outcome::AlreadyDone)
                continue;
            const QString line =
                QStringLiteral("Storage layout: %1 %2 -> %3 %4")
                    .arg(QLatin1String(moveOutcomeText(result.outcome)),
                         result.move.from, result.move.to, result.detail);
            if (result.outcome == Outcome::Moved)
                qInfo().noquote() << line;
            else
                qWarning().noquote() << line;
        }
        forkmesh::rehomeSingleInstanceLock();
        // Saved paths follow what moved, so nothing depends on the links left
        // at the old locations. The settings file is re-read afterwards: it was
        // already opened once, to decide the work runtime.
        if (const int repointed = forkmesh::paths::repointSettingsFile()) {
            QSettings().sync();
            qInfo().noquote() << "Storage layout: repointed" << repointed
                              << "saved path(s) at the projects layout.";
        }
        if (!forkmesh::paths::layout().rerootAllowed) {
            qInfo().noquote() << "Storage layout: legacy locations kept."
                              << forkmesh::paths::layout().rerootBlockedReason;
        }
    }
    forkmesh::ui::startupStep(
        QStringLiteral("Recovering interrupted mirror configuration"));

    // Repository materialization can be larger than a RAM-backed /tmp.
    // An abrupt reboot cannot run the Qt/Python temporary-directory destructors,
    // so sweep exact ForkMesh mirror artifacts only after proving this is the
    // sole application instance. This keeps crash debris from growing without
    // bound across gateway and encrypted-mirror restarts.
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("sweep stale mirror temporary directories"));
        QString cleanupError;
        const int removed =
            PublicMirrorRuntime::cleanupStaleTemporaryDirectories(
                QDir::tempPath(), &cleanupError);
        if (removed < 0) {
            qWarning().noquote()
                << "Mirror temporary cleanup skipped:" << cleanupError;
        } else if (removed > 0) {
            qInfo().noquote()
                << "Mirror temporary cleanup removed" << removed
                << "stale director" << (removed == 1 ? "y." : "ies.");
        }
    }

    // A mirror Actions helper can be killed during its local settings
    // transaction. Revert any owner-only journal before MainWindow reads
    // repository records or secrets. A failure is NOT startup-fatal: mirror
    // Actions stay suspended for this run (MainWindow checks
    // actionsSuspendedByRecoveryFailure) and, once the window is up, a warning
    // offers Retry and Reset. Everything else in the app keeps working.
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("recover interrupted mirror Actions configuration"));
        QSettings recoverySettings;
        const auto recovery =
            forkmesh::mirror_actions::recoverAtStartup(recoverySettings);
        if (!recovery.ok) {
            qCritical().noquote()
                << QStringLiteral(
                       "Mirror Actions recovery failed (%1); mirror Actions "
                       "are suspended for this run. Lock: %2 Journal: %3")
                       .arg(recovery.errorCode, recovery.lockPath,
                            recovery.journalPath);
            forkmesh::ui::startupStepFailed(
                QStringLiteral("Mirror Actions recovery failed — %1 "
                               "(continuing without mirror Actions)")
                    .arg(recovery.errorCode));
        }
    }

    {
        forkmesh::ui::startupStep(
            QStringLiteral("Choosing the interface font"));
        forkmesh::StartupTraceStep step(
            QStringLiteral("choose the interface font family"));
        QFont base = app.font();

        QStringList installed;
        {
            forkmesh::StartupTraceStep families(
                QStringLiteral("populate the system font database"));
            installed = QFontDatabase::families();
        }
        // This is the UI face for the whole app: Theme.h's global QWidget rule
        // deliberately sets no font-family (it would beat every monospace
        // setFont()), so the order here is the one that rule used to carry.
        for (const QString &preferred : {QStringLiteral("Segoe UI"),
                                         QStringLiteral("Inter"),
                                         QStringLiteral("Cantarell"),
                                         QStringLiteral("SF Pro Text"),
                                         QStringLiteral("Roboto"),
                                         QStringLiteral("Noto Sans"),
                                         QStringLiteral("Ubuntu")}) {
            if (installed.contains(preferred)) {
                base.setFamily(preferred);
                break;
            }
        }

        QStringList families{base.family()};
        bool colourEmoji = false;
        for (const QString &emoji : {QStringLiteral("Noto Color Emoji"),
                                     QStringLiteral("Apple Color Emoji"),
                                     QStringLiteral("Segoe UI Emoji")}) {
            colourEmoji = colourEmoji || installed.contains(emoji);
            if (!families.contains(emoji))
                families << emoji;
        }
        if (!colourEmoji)
            qWarning().noquote() << QStringLiteral(
                "No colour emoji font is installed; emoji draw in monochrome. "
                "Install your distribution's Noto Color Emoji package.");
        base.setFamilies(families);
        app.setFont(base);
        forkmesh::ui::startupDetail(
            QStringLiteral("UI family: %1").arg(base.family()));
    }

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (geteuid() == 0 || getuid() == 0) {
        forkmesh::ui::dismissStartupSplash();
        QMessageBox::critical(
            nullptr, "ForkMesh",
            "ForkMesh must not be run as root. It runs git and workflow "
            "commands with your privileges, so running as root is unsafe.\n\n"
            "Start it again as your normal user.");
        return 1;
    }
#endif

    forkmesh::ui::startupStep(QStringLiteral("Applying the theme"));
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("apply the application stylesheet"));
        MainWindow::applyTheme();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, &app,
                     [](Qt::ColorScheme) { MainWindow::applyTheme(); });
#endif

    QElapsedTimer startup;
    startup.start();
    qInfo().noquote() << QStringLiteral("[startup +%1ms] constructing MainWindow")
                             .arg(startup.elapsed(), 5);
    forkmesh::ui::startupStep(QStringLiteral("Starting the application window"));
    auto *window = new MainWindow;
    const auto openLocalAction = [window](const QString &target) {
        if (isCloudflareSetupLink(target))
            window->openCloudflareSetupFromSystemLink(target);
        else if (repositoryForkLinkParts(target))
            window->openRepositoryForkFromSystemLink(target);
        else {
            QString node, provider;
            if (agentLoginLinkParts(target, &node, &provider))
                window->openAgentLoginFromSystemLink(node, provider);
            else if (const QString folder =
                         forkmesh::systemopen::folderFromArgument(target);
                     !folder.isEmpty())
                window->openFolderFromSystem(folder);
        }
    };
    app.setLocalLinkHandler(openLocalAction);
    forkmesh::ui::attachStartupSplashTo(window);
    qInfo().noquote() << QStringLiteral("[startup +%1ms] MainWindow constructed")
                             .arg(startup.elapsed(), 5);
    forkmesh::onSingleInstanceActivation([window, openLocalAction](
                                             const QString &target) {
        openLocalAction(target);
        window->setWindowState((window->windowState() & ~Qt::WindowMinimized) |
                              Qt::WindowActive);
        window->show();
        window->raise();
        window->activateWindow();
    });
    forkmesh::ui::startupStep(QStringLiteral("Showing the main window"));
    {
        forkmesh::StartupTraceStep step(QStringLiteral("show the main window"));
        window->show();
    }
    // Interface sounds, if they are switched on: installs the one event filter
    // that hears clicks. After the first show, so the window appearing is not
    // itself the first sound the app makes.
    if (forkmesh::mirror_actions::actionsSuspendedByRecoveryFailure())
        QTimer::singleShot(0, window, [window] {
            showMirrorActionsRecoveryWarning(window);
        });
    forkmesh::sonify::UiSounds::instance();
    forkmesh::activity::UserActivityLog::instance();
    if (!localActionLink.isEmpty()) {
        // A deep link may build a heavy destination. Let the already-built
        // shell paint once before constructing that page-specific surface.
        QTimer::singleShot(50, window,
                           [openLocalAction, localActionLink] {
                               openLocalAction(localActionLink);
                           });
    }
    qInfo().noquote() << QStringLiteral("[startup +%1ms] window shown; entering event loop")
                             .arg(startup.elapsed(), 5);
    // Logged before the sink swap below: once the app log sink owns qInfo(),
    // startup lines stop reaching stdout, so this is the last pre-event-loop
    // marker a launch harness can read from the console.
    forkmesh::logStartupTrace(
        QStringLiteral("entering Qt event loop (pre-loop window closed)"));
    forkmesh::setAppLogSink(
        [window](QtMsgType type, const QString &message,
                 const QString &sourceFile, int sourceLine) {
            QMetaObject::invokeMethod(
                window, [window, type, message, sourceFile, sourceLine] {
                    window->logCapturedMessage(type, message, sourceFile,
                                               sourceLine);
                },
                Qt::QueuedConnection);
        },
        false);

    forkmesh::enableGracefulTerminationShutdown(&app, [&app, window]() {
        qInfo().noquote() << QStringLiteral(
            "Stop requested (SIGTERM/SIGINT): shutting down.");
        window->close();
        app.quit();
    });
    forkmesh::onSingleInstanceYield([&app, window]() {
        qInfo().noquote() << QStringLiteral(
            "ForkMesh without a window is handing over to a window: shutting down.");
        window->close();
        app.quit();
    });

    forkmesh::logStartupTrace(
        QStringLiteral("entering Qt event loop; deferred startup remains visible "
                       "in subsequent startup entries"));

    forkmesh::agentresources::Governor agentMemoryGovernor;
    agentMemoryGovernor.start();

    const int exitCode = app.exec();
    forkmesh::clearAppLogSink();
    delete window;
    const forkmesh::processreaper::Report reaped =
        forkmesh::processreaper::reapOnQuit();
    if (!reaped.terminated.isEmpty() || !reaped.stoppedUnits.isEmpty())
        qInfo().noquote() << forkmesh::processreaper::describe(reaped);
    return exitCode;
}
