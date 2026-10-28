#include "WebEngine.h"

#include <QDir>
#include <QJsonObject>
#include <QList>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

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
                     [folderFor = options.downloadDirectory](QWebEngineDownloadRequest *download) {
                         const QString folder = folderFor ? folderFor() : QString();
                         if (folder.isEmpty() || !QDir().mkpath(folder)) {
                             download->cancel();
                             return;
                         }
                         download->setDownloadDirectory(folder);
                         download->accept();
                     });
    return profile;
}

View *makeView(QObject *profile, const PageOptions &options, QWidget *parent)
{
    auto *engineProfile = qobject_cast<QWebEngineProfile *>(profile);
    if (!engineProfile)
        return nullptr;
    return new EngineView(engineProfile, options, parent);
}

[[maybe_unused]] const bool kRegistered = registerEngine(Engine{&makeProfile, &makeView});

}

}
