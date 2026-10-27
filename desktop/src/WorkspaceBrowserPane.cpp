#include "WorkspaceBrowserPane.h"

#include "AppPaths.h"
#include "BrowserPrivacy.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <utility>

#ifdef FORKMESH_HAVE_WEBENGINE
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QStandardPaths>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#endif

namespace forkmesh::ui {

namespace {

#ifdef FORKMESH_HAVE_WEBENGINE
bool headlessPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") ||
           platform == QLatin1String("minimal");
}

class GoogleServiceInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (browser::isGoogleServiceHost(info.requestUrl().host()))
            info.block(true);
    }
};

class PanePage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override
    {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("file"))
            return false;
        if (!isMainFrame)
            return true;
        return scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
               scheme == QLatin1String("about") || scheme == QLatin1String("data") ||
               scheme == QLatin1String("blob");
    }

    QWebEnginePage *createWindow(WebWindowType) override
    {
        return this;
    }
};

void configureUngoogledSettings(QWebEngineSettings *settings)
{
    settings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    settings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
    settings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    settings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);
}

QWebEngineProfile *ungoogledProfile()
{
    static QPointer<QWebEngineProfile> shared;
    if (shared)
        return shared;
    auto *profile = new QWebEngineProfile(QStringLiteral("forkmesh-browser"),
                                          QCoreApplication::instance());
    profile->setPersistentStoragePath(
        paths::pluginDataDir(QStringLiteral("browser")) + QStringLiteral("/profile"));
    profile->setCachePath(paths::cacheRoot() + QStringLiteral("/browser"));
    profile->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
    profile->setHttpUserAgent(browser::ungoogledUserAgent(profile->httpUserAgent()));
    profile->setSpellCheckEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    profile->setPushServiceEnabled(false);
#endif
    profile->setUrlRequestInterceptor(new GoogleServiceInterceptor(profile));
    configureUngoogledSettings(profile->settings());
    QObject::connect(profile, &QWebEngineProfile::downloadRequested, profile,
                     [](QWebEngineDownloadRequest *download) {
                         const QString folder = QStandardPaths::writableLocation(
                             QStandardPaths::DownloadLocation);
                         if (folder.isEmpty() || !QDir().mkpath(folder)) {
                             download->cancel();
                             return;
                         }
                         download->setDownloadDirectory(folder);
                         download->accept();
                     });
    shared = profile;
    return profile;
}
#endif

QPushButton *toolButton(const QString &name, const QString &tip)
{
    auto *button = new QPushButton;
    button->setObjectName(name);
    button->setProperty("workspaceIconButton", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(28, 28);
    button->setIconSize(QSize(14, 14));
    button->setFlat(true);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
}

}

WorkspaceBrowserPane::WorkspaceBrowserPane(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("workspaceBrowserPane"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget;
    bar->setObjectName(QStringLiteral("workspaceBrowserBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(4);
    m_back = toolButton(QStringLiteral("workspaceBrowserBack"),
                        QStringLiteral("Back"));
    m_forward = toolButton(QStringLiteral("workspaceBrowserForward"),
                           QStringLiteral("Forward"));
    m_reload = toolButton(QStringLiteral("workspaceBrowserReload"),
                          QStringLiteral("Reload"));
    m_external = toolButton(QStringLiteral("workspaceBrowserExternal"),
                            QStringLiteral("Open in your browser"));
    m_address = new QLineEdit;
    m_address->setObjectName(QStringLiteral("workspaceBrowserAddress"));
    m_address->setPlaceholderText(
        QStringLiteral("Search DuckDuckGo, enter a URL, or a port such as 3000"));
    m_address->setClearButtonEnabled(true);
    row->addWidget(m_back);
    row->addWidget(m_forward);
    row->addWidget(m_reload);
    row->addWidget(m_address, 1);
    row->addWidget(m_external);
    layout->addWidget(bar);

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("workspaceBrowserStack"));
    layout->addWidget(m_stack, 1);

    m_placeholder = new QWidget;
    m_placeholder->setObjectName(QStringLiteral("workspaceBrowserPlaceholder"));
    auto *holder = new QVBoxLayout(m_placeholder);
    holder->setContentsMargins(24, 24, 24, 24);
    holder->setSpacing(8);
    holder->addStretch(1);
    m_placeholderTitle = new QLabel;
    m_placeholderTitle->setObjectName(QStringLiteral("workspaceEmptyTitle"));
    m_placeholderTitle->setAlignment(Qt::AlignHCenter);
    m_placeholderBody = new QLabel;
    m_placeholderBody->setObjectName(QStringLiteral("workspaceEmptyBody"));
    m_placeholderBody->setAlignment(Qt::AlignHCenter);
    m_placeholderBody->setWordWrap(true);
    m_placeholderOpen = new QPushButton(QStringLiteral("Open in your browser"));
    m_placeholderOpen->setObjectName(QStringLiteral("workspaceGhostButton"));
    m_placeholderOpen->setCursor(Qt::PointingHandCursor);
    m_placeholderInstall = new QPushButton(QStringLiteral("Install embedded browser"));
    m_placeholderInstall->setObjectName(QStringLiteral("workspaceGhostButton"));
    m_placeholderInstall->setCursor(Qt::PointingHandCursor);
    m_placeholderInstall->setToolTip(
        QStringLiteral("Install Qt WebEngine with your package manager, then "
                       "rebuild and restart ForkMesh so pages load here"));
    m_placeholderInstall->hide();
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch(1);
    actions->addWidget(m_placeholderInstall);
    actions->addWidget(m_placeholderOpen);
    actions->addStretch(1);
    holder->addWidget(m_placeholderTitle);
    holder->addWidget(m_placeholderBody);
    holder->addSpacing(4);
    holder->addLayout(actions);
    holder->addStretch(1);
    m_stack->addWidget(m_placeholder);

    connect(m_address, &QLineEdit::returnPressed, this,
            [this] { navigate(m_address->text()); });
    connect(m_address, &QLineEdit::textChanged, this,
            [this] { syncNavigation(); });
    connect(m_external, &QPushButton::clicked, this,
            [this] { openExternally(); });
    connect(m_placeholderOpen, &QPushButton::clicked, this,
            [this] { openExternally(); });
    connect(m_placeholderInstall, &QPushButton::clicked, this, [this] {
        if (m_installHandler)
            m_installHandler();
    });
#ifdef FORKMESH_HAVE_WEBENGINE
    connect(m_back, &QPushButton::clicked, this, [this] {
        if (auto *view = qobject_cast<QWebEngineView *>(m_view))
            view->back();
    });
    connect(m_forward, &QPushButton::clicked, this, [this] {
        if (auto *view = qobject_cast<QWebEngineView *>(m_view))
            view->forward();
    });
    connect(m_reload, &QPushButton::clicked, this, [this] {
        if (auto *view = qobject_cast<QWebEngineView *>(m_view))
            view->reload();
        else if (m_url.isValid())
            navigate(m_url.toString());
    });
#else
    connect(m_reload, &QPushButton::clicked, this, [this] {
        if (m_url.isValid())
            navigate(m_url.toString());
    });
#endif

    showPlaceholder(
        QStringLiteral("Preview a page"),
        embeddedAvailable()
            ? QStringLiteral("Type an address or a search above to load it "
                             "here. A bare port such as 3000 opens "
                             "http://localhost:3000. Google's background "
                             "services are switched off.")
            : QStringLiteral("This build has no embedded browser. Type an "
                             "address above and it opens in your own browser."),
        false, embeddedInstallable());
    syncNavigation();
}

bool WorkspaceBrowserPane::embeddedAvailable()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    return !headlessPlatform();
#else
    return false;
#endif
}

bool WorkspaceBrowserPane::embeddedInstallable()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    return false;
#else
    return true;
#endif
}

QUrl WorkspaceBrowserPane::urlFromInput(const QString &text)
{
    return browser::urlFromInput(text);
}

void WorkspaceBrowserPane::navigate(const QString &text)
{
    const QUrl url = urlFromInput(text);
    if (!url.isValid()) {
        showPlaceholder(QStringLiteral("That address cannot be opened"),
                        QStringLiteral("Only http and https pages load here."),
                        false);
        return;
    }
    m_url = url;
    m_address->setText(url.toString());
#ifdef FORKMESH_HAVE_WEBENGINE
    if (embeddedAvailable()) {
        ensureView();
        if (auto *view = qobject_cast<QWebEngineView *>(m_view)) {
            m_stack->setCurrentWidget(view);
            view->load(url);
            syncNavigation();
            return;
        }
    }
#endif
    showPlaceholder(url.host().isEmpty() ? url.toString() : url.host(),
                    QStringLiteral("This build has no embedded browser, so the "
                                   "page opens in your own browser."),
                    true, embeddedInstallable());
    syncNavigation();
}

QUrl WorkspaceBrowserPane::currentUrl() const
{
    return m_url;
}

bool WorkspaceBrowserPane::showsEmbeddedView() const
{
    return m_view && m_stack->currentWidget() == m_view;
}

void WorkspaceBrowserPane::focusAddress()
{
    m_address->setFocus(Qt::OtherFocusReason);
    m_address->selectAll();
}

void WorkspaceBrowserPane::setIcons(const Icons &icons)
{
    m_back->setIcon(icons.back);
    m_forward->setIcon(icons.forward);
    m_reload->setIcon(icons.reload);
    m_external->setIcon(icons.external);
}

void WorkspaceBrowserPane::setInstallHandler(std::function<void()> handler)
{
    m_installHandler = std::move(handler);
    syncInstallButton();
}

void WorkspaceBrowserPane::ensureView()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    if (m_view)
        return;
    auto *view = new QWebEngineView(m_stack);
    view->setObjectName(QStringLiteral("workspaceBrowserView"));
    view->setPage(new PanePage(ungoogledProfile(), view));
    m_view = view;
    m_stack->addWidget(view);
    connect(view, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
        m_url = url;
        if (!m_address->hasFocus())
            m_address->setText(url.toString());
        syncNavigation();
    });
    connect(view, &QWebEngineView::loadFinished, this,
            [this](bool) { syncNavigation(); });
#endif
}

void WorkspaceBrowserPane::openExternally()
{
    QUrl url = m_url;
    if (!url.isValid())
        url = urlFromInput(m_address->text());
    const QString scheme = url.scheme().toLower();
    if (url.isValid() &&
        (scheme == QLatin1String("http") || scheme == QLatin1String("https")))
        QDesktopServices::openUrl(url);
}

void WorkspaceBrowserPane::showPlaceholder(const QString &title,
                                           const QString &body,
                                           bool offerExternal,
                                           bool offerInstall)
{
    m_placeholderTitle->setText(title);
    m_placeholderBody->setText(body);
    m_placeholderOpen->setVisible(offerExternal);
    m_placeholderOffersInstall = offerInstall;
    syncInstallButton();
    m_stack->setCurrentWidget(m_placeholder);
}

void WorkspaceBrowserPane::syncInstallButton()
{
    m_placeholderInstall->setVisible(m_placeholderOffersInstall &&
                                     static_cast<bool>(m_installHandler));
}

void WorkspaceBrowserPane::syncNavigation()
{
    bool back = false;
    bool forward = false;
#ifdef FORKMESH_HAVE_WEBENGINE
    if (auto *view = qobject_cast<QWebEngineView *>(m_view)) {
        back = view->history()->canGoBack();
        forward = view->history()->canGoForward();
    }
#endif
    m_back->setEnabled(back);
    m_forward->setEnabled(forward);
    m_reload->setEnabled(m_url.isValid());
    m_external->setEnabled(m_url.isValid() ||
                           !m_address->text().trimmed().isEmpty());
}

}
