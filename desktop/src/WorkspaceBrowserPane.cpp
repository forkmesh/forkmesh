#include "WorkspaceBrowserPane.h"

#include "AppPaths.h"
#include "BrowserPrivacy.h"
#include "WebEngine.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::ui {

namespace {

bool headlessPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") ||
           platform == QLatin1String("minimal");
}

bool paneNavigationAllowed(const QUrl &url, bool, bool isMainFrame)
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

QObject *ungoogledProfile()
{
    static QPointer<QObject> shared;
    if (shared)
        return shared;
    web::ProfileOptions options;
    options.storageName = QStringLiteral("forkmesh-browser");
    options.storagePath =
        paths::pluginDataDir(QStringLiteral("browser")) + QStringLiteral("/profile");
    options.cachePath = paths::cacheRoot() + QStringLiteral("/browser");
    options.persistentCookies = true;
    options.ungoogled = true;
    options.userAgent = [](const QString &agent) { return browser::ungoogledUserAgent(agent); };
    options.requestAllowed = [](const QUrl &url) {
        return !browser::isGoogleServiceHost(url.host());
    };
    options.downloadDirectory = [] {
        return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    };
    shared = web::createProfile(options, QCoreApplication::instance());
    return shared;
}

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
    connect(m_back, &QPushButton::clicked, this, [this] {
        if (web::View *view = web::viewOf(m_view))
            view->back();
    });
    connect(m_forward, &QPushButton::clicked, this, [this] {
        if (web::View *view = web::viewOf(m_view))
            view->forward();
    });
    connect(m_reload, &QPushButton::clicked, this, [this] {
        if (web::View *view = web::viewOf(m_view))
            view->reload();
        else if (m_url.isValid())
            navigate(m_url.toString());
    });

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
#ifdef FORKMESH_BROWSER_PANE_EMBEDS
    return web::available() && !headlessPlatform();
#else
    return false;
#endif
}

bool WorkspaceBrowserPane::embeddedInstallable()
{
    return !web::built();
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
    if (embeddedAvailable()) {
        ensureView();
        if (web::View *view = web::viewOf(m_view)) {
            m_stack->setCurrentWidget(m_view);
            view->load(url);
            syncNavigation();
            return;
        }
    }
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
    if (m_view)
        return;
    web::PageOptions options;
    options.acceptNavigation = &paneNavigationAllowed;
    options.newWindowsInPlace = true;
    web::View *view = web::createView(ungoogledProfile(), options, m_stack);
    if (!view)
        return;
    m_view = view->widget();
    m_view->setObjectName(QStringLiteral("workspaceBrowserView"));
    m_stack->addWidget(m_view);
    view->onUrlChanged(this, [this](const QUrl &url) {
        m_url = url;
        if (!m_address->hasFocus())
            m_address->setText(url.toString());
        syncNavigation();
    });
    view->onLoadFinished(this, [this](bool) { syncNavigation(); });
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
    if (web::View *view = web::viewOf(m_view)) {
        back = view->canGoBack();
        forward = view->canGoForward();
    }
    m_back->setEnabled(back);
    m_forward->setEnabled(forward);
    m_reload->setEnabled(m_url.isValid());
    m_external->setEnabled(m_url.isValid() ||
                           !m_address->text().trimmed().isEmpty());
}

}
