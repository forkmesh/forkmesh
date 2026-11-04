#include "WebEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QWebEngineCookieStore>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEngineFrame>
#include <QWebEnginePermission>
#endif

#include <utility>

namespace forkmesh::web {

namespace {

class Interceptor : public QWebEngineUrlRequestInterceptor
{
public:
    Interceptor(std::function<bool(const QUrl &)> allowed, QObject *parent)
        : QWebEngineUrlRequestInterceptor(parent), m_allowed(std::move(allowed))
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (!m_allowed(info.requestUrl()))
            info.block(true);
    }

private:
    std::function<bool(const QUrl &)> m_allowed;
};

class Page : public QWebEnginePage
{
public:
    Page(QWebEngineProfile *profile, const PageOptions &options, QObject *parent)
        : QWebEnginePage(profile, parent), m_options(options)
    {
        if (m_options.requestAllowed)
            setUrlRequestInterceptor(new Interceptor(m_options.requestAllowed, this));
        if (m_options.onNewWindow && !m_options.newWindowsInPlace)
            connect(this, &QWebEnginePage::newWindowRequested, this,
                    [open = m_options.onNewWindow](QWebEngineNewWindowRequest &request) {
                        open(request.requestedUrl());
                    });
        if (m_options.printPath) {
            connect(this, &QWebEnginePage::printRequested, this, [this] {
                const QString path = m_options.printPath();
                if (!path.isEmpty())
                    printToPdf(path);
            });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            connect(this, &QWebEnginePage::printRequestedByFrame, this,
                    [this](QWebEngineFrame frame) {
                        const QString path = m_options.printPath();
                        if (!path.isEmpty())
                            frame.printToPdf(path);
                    });
#endif
        }
        if (m_options.printFinished)
            connect(this, &QWebEnginePage::pdfPrintingFinished, this,
                    [done = m_options.printFinished](const QString &path, bool ok) {
                        done(path, ok);
                    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (m_options.clipboard)
            connect(this, &QWebEnginePage::permissionRequested, this,
                    [](QWebEnginePermission permission) {
                        if (permission.permissionType() ==
                            QWebEnginePermission::PermissionType::ClipboardReadWrite)
                            permission.grant();
                    });
#endif
    }

    QJsonArray consoleMessages() const
    {
        QJsonArray out;
        for (const QJsonObject &message : m_console)
            out.append(message);
        return out;
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message,
                                  int lineNumber, const QString &sourceID) override
    {
        if (m_options.onConsole)
            m_options.onConsole(message);
        if (m_options.consoleLimit <= 0)
            return;
        static const char *const names[] = {"info", "warning", "error"};
        const int index = qBound(0, int(level), 2);
        m_console.append(QJsonObject{
            {QStringLiteral("level"), QString::fromLatin1(names[index])},
            {QStringLiteral("message"), message.left(2000)},
            {QStringLiteral("source"), sourceID.left(500)},
            {QStringLiteral("line"), lineNumber},
        });
        while (m_console.size() > m_options.consoleLimit)
            m_console.removeFirst();
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        if (!m_options.acceptNavigation)
            return false;
        return m_options.acceptNavigation(url, type == NavigationTypeLinkClicked, isMainFrame);
    }

    QWebEnginePage *createWindow(WebWindowType) override
    {
        return m_options.newWindowsInPlace ? this : nullptr;
    }

private:
    PageOptions m_options;
    QList<QJsonObject> m_console;
};

class EngineView : public QWebEngineView, public View
{
public:
    EngineView(QWebEngineProfile *profile, const PageOptions &options, QWidget *parent)
        : QWebEngineView(parent)
    {
        m_page = new Page(profile, options, this);
        if (options.restricted) {
            QWebEngineSettings *settings = m_page->settings();
            settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
            settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
            settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
            settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
            settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        }
        if (options.clipboard) {
            QWebEngineSettings *settings = m_page->settings();
            settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);
            settings->setAttribute(QWebEngineSettings::JavascriptCanPaste, true);
        }
        if (!options.documentScript.isEmpty()) {
            QWebEngineScript script;
            script.setName(options.documentScriptName);
            script.setInjectionPoint(QWebEngineScript::DocumentCreation);
            script.setWorldId(QWebEngineScript::MainWorld);
            script.setRunsOnSubFrames(false);
            script.setSourceCode(options.documentScript);
            m_page->scripts().insert(script);
        }
        setPage(m_page);
    }

    QWidget *widget() override { return this; }
    void load(const QUrl &url) override { QWebEngineView::load(url); }
    void setHtml(const QString &html, const QUrl &baseUrl) override { m_page->setHtml(html, baseUrl); }
    void back() override { QWebEngineView::back(); }
    void forward() override { QWebEngineView::forward(); }
    void reload() override { QWebEngineView::reload(); }
    bool canGoBack() const override { return history()->canGoBack(); }
    bool canGoForward() const override { return history()->canGoForward(); }
    QUrl url() const override { return QWebEngineView::url(); }
    QString title() const override { return QWebEngineView::title(); }
    QJsonArray consoleMessages() const override { return m_page->consoleMessages(); }

    void runJavaScript(const QString &script, World world,
                       std::function<void(const QVariant &)> done) override
    {
        const quint32 worldId = world == World::Main ? QWebEngineScript::MainWorld
                                                     : QWebEngineScript::ApplicationWorld;
        if (done)
            m_page->runJavaScript(script, worldId, std::move(done));
        else
            m_page->runJavaScript(script, worldId);
    }

    void onUrlChanged(QObject *context, std::function<void(const QUrl &)> handler) override
    {
        QObject::connect(this, &QWebEngineView::urlChanged, context, std::move(handler));
    }

    void onTitleChanged(QObject *context, std::function<void(const QString &)> handler) override
    {
        QObject::connect(this, &QWebEngineView::titleChanged, context, std::move(handler));
    }

    void onLoadFinished(QObject *context, std::function<void(bool)> handler) override
    {
        QObject::connect(this, &QWebEngineView::loadFinished, context, std::move(handler));
    }

private:
    Page *m_page = nullptr;
};

void ungoogle(QWebEngineSettings *settings)
{
    settings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    settings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
    settings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    settings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);
}

QObject *makeProfile(const ProfileOptions &options, QObject *parent)
{
    auto *profile = options.storageName.isEmpty()
                        ? new QWebEngineProfile(parent)
                        : new QWebEngineProfile(options.storageName, parent);
    if (!options.storagePath.isEmpty())
        profile->setPersistentStoragePath(options.storagePath);
    if (!options.cachePath.isEmpty())
        profile->setCachePath(options.cachePath);
    if (options.storageName.isEmpty())
        profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile->setPersistentCookiesPolicy(options.persistentCookies
                                            ? QWebEngineProfile::AllowPersistentCookies
                                            : QWebEngineProfile::NoPersistentCookies);
    if (options.userAgent)
        profile->setHttpUserAgent(options.userAgent(profile->httpUserAgent()));
    if (options.ungoogled) {
        profile->setSpellCheckEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        profile->setPushServiceEnabled(false);
#endif
        ungoogle(profile->settings());
    }
    if (options.requestAllowed)
        profile->setUrlRequestInterceptor(new Interceptor(options.requestAllowed, profile));
    QObject::connect(profile, &QWebEngineProfile::downloadRequested, profile,
                     [folderFor = options.downloadDirectory, pathFor = options.downloadPath,
                      finished = options.downloadFinished](QWebEngineDownloadRequest *download) {
                         if (pathFor) {
                             const QString path = pathFor(download->downloadFileName());
                             if (path.isEmpty()) {
                                 download->cancel();
                                 return;
                             }
                             const QFileInfo target(path);
                             download->setDownloadDirectory(target.absolutePath());
                             download->setDownloadFileName(target.fileName());
                         } else {
                             const QString folder = folderFor ? folderFor() : QString();
                             if (folder.isEmpty() || !QDir().mkpath(folder)) {
                                 download->cancel();
                                 return;
                             }
                             download->setDownloadDirectory(folder);
                         }
                         if (finished)
                             QObject::connect(
                                 download, &QWebEngineDownloadRequest::isFinishedChanged, download,
                                 [download, finished] {
                                     if (!download->isFinished())
                                         return;
                                     finished(QDir(download->downloadDirectory())
                                                  .filePath(download->downloadFileName()),
                                              download->state() ==
                                                  QWebEngineDownloadRequest::DownloadCompleted);
                                 });
                         download->accept();
                     });
    return profile;
}

bool setProfileCookie(QObject *profile, const QUrl &origin, const QString &name,
                      const QString &value, bool httpOnly)
{
    auto *engineProfile = qobject_cast<QWebEngineProfile *>(profile);
    if (!engineProfile || !origin.isValid() || origin.host().isEmpty() || name.isEmpty())
        return false;
    QNetworkCookie cookie(name.toUtf8(), value.toUtf8());
    cookie.setPath(QStringLiteral("/"));
    cookie.setHttpOnly(httpOnly);
    cookie.setSecure(origin.scheme() == QLatin1String("https"));
    cookie.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
    if (value.isEmpty())
        engineProfile->cookieStore()->deleteCookie(cookie, origin);
    else
        engineProfile->cookieStore()->setCookie(cookie, origin);
    return true;
}

View *makeView(QObject *profile, const PageOptions &options, QWidget *parent)
{
    auto *engineProfile = qobject_cast<QWebEngineProfile *>(profile);
    if (!engineProfile)
        return nullptr;
    return new EngineView(engineProfile, options, parent);
}

[[maybe_unused]] const bool kRegistered = registerEngine(Engine{&makeProfile, &makeView, &setProfileCookie});

}

}
