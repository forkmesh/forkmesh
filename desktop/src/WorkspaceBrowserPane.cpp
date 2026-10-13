#include "WorkspaceBrowserPane.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>

#ifdef FORKMESH_HAVE_WEBENGINE
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineSettings>
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
        QStringLiteral("Enter a URL, or a port such as 3000"));
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
    holder->addWidget(m_placeholderTitle);
    holder->addWidget(m_placeholderBody);
    holder->addSpacing(4);
    holder->addWidget(m_placeholderOpen, 0, Qt::AlignHCenter);
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
            ? QStringLiteral("Type an address above to load it here. A bare "
                             "port such as 3000 opens http://localhost:3000.")
            : QStringLiteral("This build has no embedded browser. Type an "
                             "address above and it opens in your own browser."),
        false);
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

QUrl WorkspaceBrowserPane::urlFromInput(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QUrl();
    static const QRegularExpression port(QStringLiteral("^:?(\\d{2,5})(/.*)?$"));
    const QRegularExpressionMatch match = port.match(trimmed);
    if (match.hasMatch())
        return QUrl(QStringLiteral("http://localhost:%1%2")
                        .arg(match.captured(1), match.captured(2)));
    static const QRegularExpression local(
        QStringLiteral("^(localhost|127\\.0\\.0\\.1|\\[::1\\])(:\\d+)?(/.*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (local.match(trimmed).hasMatch())
        return QUrl(QStringLiteral("http://") + trimmed);
    const QUrl url = QUrl::fromUserInput(trimmed);
    if (!url.isValid())
        return QUrl();
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QUrl();
    return url;
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
                    true);
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

void WorkspaceBrowserPane::ensureView()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    if (m_view)
        return;
    auto *view = new QWebEngineView(m_stack);
    view->setObjectName(QStringLiteral("workspaceBrowserView"));
    view->settings()->setUnknownUrlSchemePolicy(
        QWebEngineSettings::DisallowUnknownUrlSchemes);
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
                                           bool offerExternal)
{
    m_placeholderTitle->setText(title);
    m_placeholderBody->setText(body);
    m_placeholderOpen->setVisible(offerExternal);
    m_stack->setCurrentWidget(m_placeholder);
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
