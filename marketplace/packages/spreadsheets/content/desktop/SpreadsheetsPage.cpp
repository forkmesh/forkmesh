#include "SpreadsheetsPage.h"

#include "DesignTokens.h"
#include "DesktopUi.h"
#include "SpreadsheetsWeb.h"
#include "WebEngine.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace forkmesh::spreadsheets {

namespace {

const QString kOpenCommand = QStringLiteral("webPage.open");
const QString kSaveFolderSetting = QStringLiteral("saveFolder");

}

SpreadsheetsPage::SpreadsheetsPage(const forkmesh::market::NativePackageHost &host,
                                   QWidget *parent)
    : NativePackageWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("spreadsheetsPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack);

    m_message = new QWidget(m_stack);
    m_message->setObjectName(QStringLiteral("spreadsheetsMessage"));
    auto *column = new QVBoxLayout(m_message);
    column->setContentsMargins(32, 32, 32, 32);
    column->setSpacing(10);
    column->addStretch(1);
    m_title = new QLabel(m_message);
    m_title->setObjectName(QStringLiteral("spreadsheetsMessageTitle"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    column->addWidget(m_title);
    m_body = new QLabel(m_message);
    m_body->setObjectName(QStringLiteral("spreadsheetsMessageBody"));
    m_body->setAlignment(Qt::AlignCenter);
    m_body->setWordWrap(true);
    m_body->setMaximumWidth(520);
    column->addWidget(m_body, 0, Qt::AlignHCenter);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch(1);
    m_reload = new QPushButton(QStringLiteral("Reload"), m_message);
    m_reload->setObjectName(QStringLiteral("spreadsheetsReload"));
    m_reload->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(m_reload);
    m_browser = new QPushButton(QStringLiteral("Open in your browser"), m_message);
    m_browser->setObjectName(QStringLiteral("spreadsheetsOpenBrowser"));
    m_browser->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(m_browser);
    buttons->addStretch(1);
    column->addLayout(buttons);
    column->addStretch(2);
    m_stack->addWidget(m_message);

    connect(m_reload, &QPushButton::clicked, this, [this] {
        drop();
        sync(true);
    });
    connect(m_browser, &QPushButton::clicked, this, [this] { openInBrowser(); });
    themeChanged();
    showMessage(QStringLiteral("Spreadsheets"), QStringLiteral("Opening your workbooks\xE2\x80\xA6"),
                false, false);
}

SpreadsheetsPage::~SpreadsheetsPage()
{
    delete m_view;
    m_view = nullptr;
    delete m_profile;
    m_profile = nullptr;
}

void SpreadsheetsPage::refresh()
{
    sync(true);
}

void SpreadsheetsPage::prepareNavigationBadge()
{
    sync(isVisible());
}

void SpreadsheetsPage::themeChanged()
{
    m_title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; color: %1;")
                               .arg(Theme::hex(Theme::Role::Fg)));
    m_body->setStyleSheet(
        QStringLiteral("color: %1;").arg(Theme::hex(Theme::Role::FgMuted)));
    if (web::View *view = web::viewOf(m_view))
        view->runJavaScript(themeScript(forkmesh::ui::currentThemeIsDark()), web::World::Main,
                            nullptr);
}

bool SpreadsheetsPage::acceptsHostCommand(const QString &command) const
{
    return command == kOpenCommand;
}

bool SpreadsheetsPage::hostCommand(const QString &command, const QJsonObject &args)
{
    if (command != kOpenCommand || !web::available())
        return false;
    const QUrl relative(args.value(QStringLiteral("path")).toString());
    if (!relative.isValid() || !relative.isRelative() || !relative.host().isEmpty() ||
        !ownsPath(relative.path(QUrl::FullyDecoded)))
        return false;
    m_pending = pathAndQuery(relative);
    if (m_host.showPage)
        m_host.showPage();
    else
        sync(true);
    return true;
}

void SpreadsheetsPage::showEvent(QShowEvent *event)
{
    NativePackageWidget::showEvent(event);
    sync(true);
}

QUrl SpreadsheetsPage::pageUrl(const QString &path) const
{
    return m_host.webPageUrl ? QUrl(m_host.webPageUrl(path)) : QUrl();
}

void SpreadsheetsPage::sync(bool create)
{
    const QUrl page = pageUrl(kPagePath);
    const QUrl origin = originOf(page);
    if (!origin.isValid()) {
        drop();
        showMessage(QStringLiteral("No ForkMesh host is set"),
                    QStringLiteral("Choose the host this desktop uses in Settings, then open "
                                   "Spreadsheets again."),
                    false, true);
        return;
    }
    const QString account = m_host.accountName ? m_host.accountName().trimmed() : QString();
    if (account.isEmpty() || !m_host.accountSignedIn || !m_host.accountSignedIn()) {
        drop();
        showMessage(QStringLiteral("Sign in to use Spreadsheets"),
                    QStringLiteral("Spreadsheets opens your workbooks with this desktop's "
                                   "ForkMesh account. Sign in, then reload."),
                    false, true);
        return;
    }
    const QString token =
        m_host.accountSessionToken ? m_host.accountSessionToken(page).trimmed() : QString();
    if (token.isEmpty()) {
        drop();
        requestSession();
        return;
    }
    if (!web::available()) {
        drop();
        showMessage(QStringLiteral("This build has no embedded browser"),
                    QStringLiteral("Spreadsheets runs in Qt WebEngine on the desktop. Without "
                                   "it, the editor opens in your own browser."),
                    true, false);
        return;
    }
    const QByteArray session = QCryptographicHash::hash(
        (origin.toString() + QLatin1Char('\n') + account + QLatin1Char('\n') + token).toUtf8(),
        QCryptographicHash::Sha256);
    if (m_view && session == m_session) {
        if (!m_pending.isEmpty()) {
            const QString path = m_pending;
            m_pending.clear();
            load(path);
        }
        return;
    }
    drop();
    if (create)
        build(origin, account, token, session);
}

void SpreadsheetsPage::requestSession()
{
    showMessage(QStringLiteral("Spreadsheets"), QStringLiteral("Opening your workbooks\xE2\x80\xA6"),
                false, false);
    if (m_sessionPending)
        return;
    if (!m_host.requestAccountSession) {
        showMessage(QStringLiteral("Spreadsheets could not open your account"),
                    QStringLiteral("This desktop cannot hand its account session to the "
                                   "editor. Open Spreadsheets in your own browser."),
                    true, true);
        return;
    }
    m_sessionPending = true;
    QPointer<SpreadsheetsPage> self(this);
    m_host.requestAccountSession([self](const QString &error) {
        if (!self)
            return;
        self->m_sessionPending = false;
        const QString token = self->m_host.accountSessionToken
                                  ? self->m_host.accountSessionToken(
                                        self->pageUrl(kPagePath)).trimmed()
                                  : QString();
        if (error.isEmpty() && !token.isEmpty()) {
            self->sync(self->isVisible());
            return;
        }
        self->showMessage(QStringLiteral("Spreadsheets could not open your account"),
                          error.isEmpty()
                              ? QStringLiteral("Your ForkMesh host did not open a session "
                                               "for this desktop. Reload to try again.")
                              : error,
                          true, true);
    });
}

void SpreadsheetsPage::build(const QUrl &origin, const QString &account, const QString &token,
                             const QByteArray &session)
{
    web::ProfileOptions profile;
    profile.downloadPath = [this](const QString &name) {
        return askSavePath(QStringLiteral("Save file"), name, QString());
    };
    profile.downloadFinished = [this](const QString &path, bool ok) { saved(path, ok); };
    m_profile = web::createProfile(profile, this);
    if (!m_profile) {
        showMessage(QStringLiteral("The embedded browser did not start"),
                    QStringLiteral("Open Spreadsheets in your own browser, or reload to try "
                                   "again."),
                    true, true);
        return;
    }
    web::setCookie(m_profile, origin, QStringLiteral("forkmesh_account"), token, true);

    web::PageOptions options;
    options.acceptNavigation = [this, origin](const QUrl &url, bool linkClicked, bool mainFrame) {
        if (navigation(origin, url, linkClicked, mainFrame) == Navigation::Load)
            return true;
        QTimer::singleShot(0, this, [this, url, linkClicked, mainFrame] {
            decide(url, linkClicked, mainFrame);
        });
        return false;
    };
    options.clipboard = true;
    options.onNewWindow = [this](const QUrl &url) {
        QTimer::singleShot(0, this, [this, url] { openLink(url); });
    };
    options.printPath = [this] {
        return askSavePath(QStringLiteral("Save as PDF"), QStringLiteral("Spreadsheet.pdf"),
                           QStringLiteral("PDF files (*.pdf)"));
    };
    options.printFinished = [this](const QString &path, bool ok) { saved(path, ok); };
    options.documentScriptName = QStringLiteral("forkmesh-spreadsheets-desktop");
    options.documentScript =
        documentScript(origin, account, token, forkmesh::ui::currentThemeIsDark());
    web::View *view = web::createView(m_profile, options, m_stack);
    if (!view) {
        delete m_profile;
        m_profile = nullptr;
        showMessage(QStringLiteral("The embedded browser did not start"),
                    QStringLiteral("Open Spreadsheets in your own browser, or reload to try "
                                   "again."),
                    true, true);
        return;
    }
    m_view = view->widget();
    m_view->setObjectName(QStringLiteral("spreadsheetsView"));
    m_stack->addWidget(m_view);
    view->onLoadFinished(this, [this](bool ok) {
        if (!ok && m_view && m_stack->currentWidget() == m_view &&
            !m_view->property("forkmeshLoaded").toBool())
            showMessage(QStringLiteral("Spreadsheets did not load"),
                        QStringLiteral("Check the connection to your ForkMesh host, then "
                                       "reload."),
                        true, true);
        else if (ok && m_view)
            m_view->setProperty("forkmeshLoaded", true);
    });
    m_origin = origin;
    m_session = session;
    const QString path = m_pending.isEmpty() ? kPagePath : m_pending;
    m_pending.clear();
    load(path);
}

void SpreadsheetsPage::drop()
{
    if (m_view) {
        m_stack->removeWidget(m_view);
        delete m_view;
        m_view = nullptr;
    }
    if (m_profile) {
        m_profile->deleteLater();
        m_profile = nullptr;
    }
    m_session.clear();
    m_origin = QUrl();
}

void SpreadsheetsPage::load(const QString &path)
{
    web::View *view = web::viewOf(m_view);
    const QUrl url = pageUrl(path);
    if (!view || !ownsUrl(m_origin, url))
        return;
    m_view->setProperty("forkmeshLoaded", false);
    m_stack->setCurrentWidget(m_view);
    view->load(url);
}

void SpreadsheetsPage::decide(const QUrl &url, bool linkClicked, bool mainFrame)
{
    switch (navigation(m_origin, url, linkClicked, mainFrame)) {
    case Navigation::OpenLink:
        openLink(url);
        return;
    case Navigation::OpenPage:
        if (m_host.openWebPage)
            m_host.openWebPage(pathAndQuery(url));
        return;
    case Navigation::Leave:
        showMessage(QStringLiteral("Spreadsheets was signed out"),
                    QStringLiteral("The page tried to leave Spreadsheets for %1. Reload to open "
                                   "it again; if this keeps happening, sign in to ForkMesh on "
                                   "this desktop again.")
                        .arg(url.toDisplayString(QUrl::RemoveQuery | QUrl::RemoveFragment)),
                    true, true);
        return;
    case Navigation::Load:
    case Navigation::Block:
        return;
    }
}

void SpreadsheetsPage::openLink(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (ownsUrl(m_origin, url)) {
        load(pathAndQuery(url));
        return;
    }
    if (sameOrigin(m_origin, url)) {
        if (m_host.openWebPage)
            m_host.openWebPage(pathAndQuery(url));
        return;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
        scheme == QLatin1String("mailto"))
        QDesktopServices::openUrl(url);
}

void SpreadsheetsPage::openInBrowser()
{
    QUrl url = pageUrl(kPagePath);
    if (web::View *view = web::viewOf(m_view)) {
        if (ownsUrl(m_origin, view->url()))
            url = view->url();
    }
    if (!url.isValid()) {
        if (m_host.flashMessage)
            m_host.flashMessage(
                QStringLiteral("No ForkMesh host is set, so there is no page to open."), true);
        return;
    }
    QDesktopServices::openUrl(url);
}

void SpreadsheetsPage::showMessage(const QString &title, const QString &body, bool offerBrowser,
                                   bool offerReload)
{
    m_title->setText(title);
    m_body->setText(body);
    m_browser->setVisible(offerBrowser);
    m_reload->setVisible(offerReload);
    m_stack->setCurrentWidget(m_message);
}

QString SpreadsheetsPage::askSavePath(const QString &title, const QString &name,
                                      const QString &filter)
{
    QString folder = m_host.setting
                         ? m_host.setting(kSaveFolderSetting, QString()).toString()
                         : QString();
    if (folder.isEmpty() || !QDir(folder).exists())
        folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString suggested = QFileInfo(name).fileName();
    const QString path = QFileDialog::getSaveFileName(
        window(), title,
        QDir(folder).filePath(suggested.isEmpty() ? QStringLiteral("Spreadsheet") : suggested),
        filter);
    if (!path.isEmpty() && m_host.setSetting)
        m_host.setSetting(kSaveFolderSetting, QFileInfo(path).absolutePath());
    return path;
}

void SpreadsheetsPage::saved(const QString &path, bool ok)
{
    if (!m_host.flashMessage)
        return;
    const QString shown = QDir::toNativeSeparators(path);
    m_host.flashMessage(ok ? QStringLiteral("Saved %1").arg(shown)
                           : QStringLiteral("Could not save %1").arg(shown),
                        !ok);
}

}

namespace {

const forkmesh::market::NativePackageRegistration spreadsheetsRegistration([] {
    forkmesh::market::NativePackageContribution contribution{
        QStringLiteral("builtin:spreadsheets"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Spreadsheets"),
        QStringLiteral("table"),
        QStringLiteral("/api/spreadsheets"),
        QStringLiteral("forkmesh-spreadsheets-request-v1"),
        [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
            return new forkmesh::spreadsheets::SpreadsheetsPage(host, parent);
        },
    };
    contribution.gpuSurface = true;
    return contribution;
}());

}
