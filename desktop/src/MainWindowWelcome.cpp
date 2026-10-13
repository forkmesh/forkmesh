#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

namespace forkmesh::ui {

namespace {

const char kWelcomeSeenSetting[] = "onboarding/welcomeSignInSeenV1";
const char kWelcomeStateName[] = "nextWelcomeState";
constexpr int kWelcomeCardWidth = 440;

enum class WelcomePage { Home, Account, Credentials, Browser };

struct WelcomeHooks {
    std::function<void()> signIn;
    std::function<bool()> signedIn;
    std::function<void()> createAccount;
    std::function<bool()> canLinkNode;
    std::function<void()> linkNode;
    std::function<void()> afterClose;
    std::function<QString()> instanceHost;
    std::function<QString(const QString &)> instanceError;
    std::function<bool(const QString &, const QString &, const QString &,
                       const QString &, bool *)>
        verify;
};

QLabel *welcomeLabel(const char *name, const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(name));
    label->setWordWrap(true);
    return label;
}

QPushButton *welcomeButton(const char *name, const char *action,
                           const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    if (name)
        button->setObjectName(QLatin1String(name));
    button->setProperty("welcomeAction", QString::fromLatin1(action));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

kit::Spinner *welcomeSpinner(QWidget *parent)
{
    auto *spinner = new kit::Spinner(16, parent);
    spinner->setObjectName(QStringLiteral("welcomeSpinner"));
    spinner->setStyleSheet(
        QStringLiteral("#welcomeSpinner { background: transparent; }"));
    spinner->stop();
    return spinner;
}

class WelcomeOverlay final : public QWidget
{
public:
    explicit WelcomeOverlay(QWidget *parent);

    WelcomeHooks hooks;

    void attachTo(QWidget *parent);
    bool isOpen() const;
    bool flowActive() const;
    void openHome(bool firstRun);
    void openBrowserWait(const QString &host);
    bool promptAccount(const QString &suggestedAccount,
                       const QString &suggestedInstance, QString *account,
                       QString *instance);
    bool runCredentials(const QString &accountName);
    bool showNotice(const QString &text, bool error);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QVBoxLayout *addPage();
    void addActionRow(QVBoxLayout *layout, QPushButton *back,
                      kit::Spinner *spinner, QPushButton *primary);
    void buildHome();
    void buildAccount();
    void buildCredentials();
    void buildBrowser();
    void showPage(WelcomePage page);
    void openPage(WelcomePage page);
    void focusPage();
    void refreshHome();
    void setBusy(bool busy);
    bool runLoop(WelcomePage page);
    void finishLoop(bool accepted);
    void closeOverlay();
    void finish();
    void dismiss();
    void beginSignIn();
    void submitAccount();
    void leaveAccount();
    void submitCredentials();
    void leaveCredentials();
    void activatePrimary();
    void activateBack();

    QFrame *m_card = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    QList<QWidget *> m_pages;

    QLabel *m_homeTitle = nullptr;
    QLabel *m_homeLede = nullptr;
    QLabel *m_homeFootnote = nullptr;
    QPushButton *m_homeSignIn = nullptr;
    QPushButton *m_homeDismiss = nullptr;
    QPushButton *m_homeLink = nullptr;

    QLineEdit *m_accountEdit = nullptr;
    QLineEdit *m_instanceEdit = nullptr;
    kit::InlineNotice *m_accountNotice = nullptr;
    kit::Spinner *m_accountSpinner = nullptr;
    QPushButton *m_accountContinue = nullptr;
    QPushButton *m_accountBack = nullptr;

    QLabel *m_credTitle = nullptr;
    QLineEdit *m_emailEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_totpEdit = nullptr;
    kit::InlineNotice *m_credNotice = nullptr;
    kit::Spinner *m_credSpinner = nullptr;
    QPushButton *m_credSubmit = nullptr;
    QPushButton *m_credBack = nullptr;

    QLabel *m_browserLede = nullptr;
    QPushButton *m_browserSignIn = nullptr;
    QPushButton *m_browserBack = nullptr;

    WelcomePage m_page = WelcomePage::Home;
    WelcomePage m_loopPage = WelcomePage::Home;
    QEventLoop *m_loop = nullptr;
    QString m_loginAccount;
    int m_accountPrompts = 0;
    bool m_loopAccepted = false;
    bool m_firstRun = false;
    bool m_driving = false;
    bool m_busy = false;
    bool m_awaitingAck = false;
    bool m_returnToAccount = false;
    bool m_accountNoticeSticky = false;
};

WelcomeOverlay::WelcomeOverlay(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("welcomeScrim"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    hide();

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("welcomeCard"));
    m_card->setFrameShape(QFrame::NoFrame);
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setFixedWidth(kWelcomeCardWidth);
    m_cardLayout = new QVBoxLayout(m_card);
    m_cardLayout->setContentsMargins(32, 28, 32, 28);
    m_cardLayout->setSpacing(0);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addStretch(1);
    row->addWidget(m_card);
    row->addStretch(1);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(0);
    outer->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(1);

    buildHome();
    buildAccount();
    buildCredentials();
    buildBrowser();

    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }
}

void WelcomeOverlay::attachTo(QWidget *parent)
{
    if (!parent || parent == parentWidget())
        return;
    if (QWidget *previous = parentWidget())
        previous->removeEventFilter(this);
    setParent(parent);
    parent->installEventFilter(this);
    setGeometry(parent->rect());
    hide();
}

bool WelcomeOverlay::isOpen() const
{
    return !isHidden();
}

bool WelcomeOverlay::flowActive() const
{
    return m_loop != nullptr || m_driving;
}

QVBoxLayout *WelcomeOverlay::addPage()
{
    auto *page = new QWidget(m_card);
    page->setObjectName(QStringLiteral("welcomePage"));
    page->setStyleSheet(
        QStringLiteral("#welcomePage { background: transparent; }"));
    page->hide();
    m_cardLayout->addWidget(page);
    m_pages.append(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return layout;
}

void WelcomeOverlay::addActionRow(QVBoxLayout *layout, QPushButton *back,
                                  kit::Spinner *spinner, QPushButton *primary)
{
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(10);
    buttons->addWidget(back);
    buttons->addStretch(1);
    buttons->addWidget(spinner);
    buttons->addWidget(primary);
    layout->addLayout(buttons);
}

void WelcomeOverlay::buildHome()
{
    QVBoxLayout *layout = addPage();
    QWidget *page = layout->parentWidget();

    layout->addWidget(
        welcomeLabel("welcomeBrand", QStringLiteral("FORKMESH"), page));
    layout->addSpacing(12);
    m_homeTitle = welcomeLabel("welcomeTitle", QString(), page);
    layout->addWidget(m_homeTitle);
    layout->addSpacing(8);
    m_homeLede = welcomeLabel("welcomeLede", QString(), page);
    layout->addWidget(m_homeLede);
    layout->addSpacing(24);

    m_homeSignIn = welcomeButton("primaryButton", "signIn",
                                 QStringLiteral("Sign in"), page);
    layout->addWidget(m_homeSignIn);
    layout->addSpacing(8);
    QPushButton *create = welcomeButton(
        nullptr, "createAccount", QStringLiteral("Create account"), page);
    layout->addWidget(create);
    layout->addSpacing(16);

    auto *links = new QVBoxLayout;
    links->setContentsMargins(0, 0, 0, 0);
    links->setSpacing(6);
    m_homeDismiss = welcomeButton("welcomeLink", "dismiss", QString(), page);
    links->addWidget(m_homeDismiss, 0, Qt::AlignLeft);
    m_homeLink = welcomeButton(
        "welcomeLink", "linkNode",
        QStringLiteral("Link this node in your browser"), page);
    links->addWidget(m_homeLink, 0, Qt::AlignLeft);
    layout->addLayout(links);
    layout->addSpacing(20);

    auto *footnote = new QFrame(page);
    footnote->setObjectName(QStringLiteral("welcomeFootnote"));
    footnote->setFrameShape(QFrame::NoFrame);
    footnote->setAttribute(Qt::WA_StyledBackground, true);
    auto *footnoteLayout = new QVBoxLayout(footnote);
    footnoteLayout->setContentsMargins(12, 10, 12, 10);
    footnoteLayout->setSpacing(0);
    m_homeFootnote = new QLabel(footnote);
    m_homeFootnote->setWordWrap(true);
    footnoteLayout->addWidget(m_homeFootnote);
    layout->addWidget(footnote);

    QObject::connect(m_homeSignIn, &QPushButton::clicked, this,
                     [this] { beginSignIn(); });
    QObject::connect(create, &QPushButton::clicked, this, [this] {
        if (!flowActive() && hooks.createAccount)
            hooks.createAccount();
    });
    QObject::connect(m_homeDismiss, &QPushButton::clicked, this,
                     [this] { dismiss(); });
    QObject::connect(m_homeLink, &QPushButton::clicked, this, [this] {
        if (flowActive())
            return;
        if (hooks.linkNode)
            hooks.linkNode();
        dismiss();
    });
}

void WelcomeOverlay::buildAccount()
{
    QVBoxLayout *layout = addPage();
    QWidget *page = layout->parentWidget();

    layout->addWidget(
        welcomeLabel("welcomeTitle", QStringLiteral("Sign in"), page));
    layout->addSpacing(8);
    layout->addWidget(welcomeLabel(
        "welcomeLede",
        QStringLiteral("Use an account from this or another ForkMesh "
                       "instance. The instance is saved with the account for "
                       "quick switching."),
        page));
    layout->addSpacing(20);

    m_accountEdit = new QLineEdit(page);
    m_accountEdit->setObjectName(QStringLiteral("welcomeAccountEdit"));
    m_accountEdit->setPlaceholderText(QStringLiteral("username"));
    layout->addWidget(kit::labeledField(QStringLiteral("ForkMesh username"),
                                        m_accountEdit, QString(), page));
    layout->addSpacing(14);
    m_instanceEdit = new QLineEdit(page);
    m_instanceEdit->setObjectName(QStringLiteral("welcomeInstanceEdit"));
    m_instanceEdit->setPlaceholderText(serverHostDisplay(kDefaultServerUrl));
    layout->addWidget(kit::labeledField(QStringLiteral("Instance"),
                                        m_instanceEdit, QString(), page));
    layout->addSpacing(12);

    m_accountNotice = new kit::InlineNotice(page);
    layout->addWidget(m_accountNotice);
    layout->addSpacing(12);

    m_accountContinue = welcomeButton("primaryButton", "accountContinue",
                                      QStringLiteral("Continue"), page);
    m_accountBack =
        welcomeButton(nullptr, "accountBack", QStringLiteral("Back"), page);
    m_accountSpinner = welcomeSpinner(page);
    addActionRow(layout, m_accountBack, m_accountSpinner, m_accountContinue);

    QObject::connect(m_accountContinue, &QPushButton::clicked, this,
                     [this] { submitAccount(); });
    QObject::connect(m_accountBack, &QPushButton::clicked, this,
                     [this] { leaveAccount(); });
}

void WelcomeOverlay::buildCredentials()
{
    QVBoxLayout *layout = addPage();
    QWidget *page = layout->parentWidget();

    m_credTitle = welcomeLabel("welcomeTitle", QString(), page);
    layout->addWidget(m_credTitle);
    layout->addSpacing(8);
    layout->addWidget(welcomeLabel(
        "welcomeLede",
        QStringLiteral("Log in with your email and password to join the "
                       "network."),
        page));
    layout->addSpacing(20);

    m_emailEdit = new QLineEdit(page);
    m_emailEdit->setObjectName(QStringLiteral("welcomeEmailEdit"));
    m_emailEdit->setPlaceholderText(QStringLiteral("you@example.com"));
    layout->addWidget(kit::labeledField(QStringLiteral("Email"), m_emailEdit,
                                        QString(), page));
    layout->addSpacing(14);
    m_passwordEdit = new QLineEdit(page);
    m_passwordEdit->setObjectName(QStringLiteral("welcomePasswordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    layout->addWidget(kit::labeledField(QStringLiteral("Password"),
                                        m_passwordEdit, QString(), page));
    layout->addSpacing(14);
    m_totpEdit = new QLineEdit(page);
    m_totpEdit->setObjectName(QStringLiteral("welcomeTotpEdit"));
    m_totpEdit->setPlaceholderText(
        QStringLiteral("6-digit code, only if you enabled 2FA"));
    m_totpEdit->setMaxLength(6);
    layout->addWidget(kit::labeledField(QStringLiteral("2FA code"), m_totpEdit,
                                        QString(), page));
    layout->addSpacing(12);

    m_credNotice = new kit::InlineNotice(page);
    layout->addWidget(m_credNotice);
    layout->addSpacing(12);

    m_credSubmit = welcomeButton("primaryButton", "credentialsSubmit",
                                 QStringLiteral("Log in"), page);
    m_credBack = welcomeButton(nullptr, "credentialsBack",
                               QStringLiteral("Back"), page);
    m_credSpinner = welcomeSpinner(page);
    addActionRow(layout, m_credBack, m_credSpinner, m_credSubmit);

    QObject::connect(m_credSubmit, &QPushButton::clicked, this,
                     [this] { submitCredentials(); });
    QObject::connect(m_credBack, &QPushButton::clicked, this,
                     [this] { leaveCredentials(); });
}

void WelcomeOverlay::buildBrowser()
{
    QVBoxLayout *layout = addPage();
    QWidget *page = layout->parentWidget();

    layout->addWidget(welcomeLabel(
        "welcomeTitle", QStringLiteral("Finish in your browser"), page));
    layout->addSpacing(8);
    m_browserLede = welcomeLabel("welcomeLede", QString(), page);
    layout->addWidget(m_browserLede);
    layout->addSpacing(24);

    m_browserSignIn = welcomeButton("primaryButton", "browserSignIn",
                                    QStringLiteral("Sign in"), page);
    layout->addWidget(m_browserSignIn);
    layout->addSpacing(14);
    m_browserBack = welcomeButton("welcomeLink", "browserBack",
                                  QStringLiteral("Back"), page);
    layout->addWidget(m_browserBack, 0, Qt::AlignLeft);

    QObject::connect(m_browserSignIn, &QPushButton::clicked, this,
                     [this] { beginSignIn(); });
    QObject::connect(m_browserBack, &QPushButton::clicked, this, [this] {
        if (flowActive())
            return;
        showPage(WelcomePage::Home);
        focusPage();
    });
}

void WelcomeOverlay::showPage(WelcomePage page)
{
    m_page = page;
    if (page == WelcomePage::Home)
        refreshHome();
    const int current = static_cast<int>(page);
    if (QWidget *widget = m_pages.value(current))
        widget->show();
    for (int index = 0; index < m_pages.size(); ++index) {
        if (index != current)
            m_pages.at(index)->hide();
    }
}

void WelcomeOverlay::openPage(WelcomePage page)
{
    showPage(page);
    if (QWidget *parent = parentWidget())
        setGeometry(parent->rect());
    show();
    raise();
    focusPage();
}

void WelcomeOverlay::focusPage()
{
    switch (m_page) {
    case WelcomePage::Home:
        m_homeSignIn->setFocus(Qt::OtherFocusReason);
        break;
    case WelcomePage::Account:
        m_accountEdit->setFocus(Qt::OtherFocusReason);
        m_accountEdit->selectAll();
        break;
    case WelcomePage::Credentials:
        if (m_emailEdit->text().trimmed().isEmpty())
            m_emailEdit->setFocus(Qt::OtherFocusReason);
        else
            m_passwordEdit->setFocus(Qt::OtherFocusReason);
        break;
    case WelcomePage::Browser:
        m_browserSignIn->setFocus(Qt::OtherFocusReason);
        break;
    }
}

void WelcomeOverlay::refreshHome()
{
    m_homeTitle->setText(m_firstRun ? QStringLiteral("Welcome to ForkMesh")
                                    : QStringLiteral("Sign in to ForkMesh"));
    m_homeLede->setText(
        m_firstRun
            ? QStringLiteral("Sign in to use your ForkMesh account on this "
                             "computer, or continue as a guest and sign in "
                             "later.")
            : QStringLiteral("Sign in to use your ForkMesh account on this "
                             "computer."));
    m_homeDismiss->setText(m_firstRun ? QStringLiteral("Continue as guest")
                                      : QStringLiteral("Not now"));
    m_homeLink->setVisible(hooks.canLinkNode && hooks.canLinkNode());
    const QString host = hooks.instanceHost ? hooks.instanceHost() : QString();
    m_homeFootnote->setText(
        host.isEmpty()
            ? QStringLiteral("You can sign in later from the top bar.")
            : QStringLiteral("This computer uses the ForkMesh instance at "
                             "%1. You can sign in later from the top bar.")
                  .arg(host));
}

void WelcomeOverlay::setBusy(bool busy)
{
    m_busy = busy;
    if (busy)
        setFocus(Qt::OtherFocusReason);
    const QList<QWidget *> controls = {
        m_accountEdit,  m_instanceEdit, m_accountContinue,
        m_accountBack,  m_emailEdit,    m_passwordEdit,
        m_totpEdit,     m_credSubmit,   m_credBack};
    for (QWidget *control : controls)
        control->setEnabled(!busy);
    if (busy && m_page == WelcomePage::Account)
        m_accountSpinner->start();
    else
        m_accountSpinner->stop();
    if (busy && m_page == WelcomePage::Credentials)
        m_credSpinner->start();
    else
        m_credSpinner->stop();
}

bool WelcomeOverlay::runLoop(WelcomePage page)
{
    QEventLoop loop;
    const QPointer<WelcomeOverlay> self(this);
    QObject::connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
    if (QWidget *top = window()) {
        if (top != this)
            QObject::connect(top, &QObject::destroyed, &loop,
                             &QEventLoop::quit);
    }
    QList<QPointer<QWidget>> blocked;
    const auto block = [&blocked](QWidget *widget) {
        if (!widget || !widget->isEnabled() ||
            widget->testAttribute(Qt::WA_ForceDisabled))
            return;
        widget->setEnabled(false);
        blocked.append(widget);
    };
    if (QWidget *host = parentWidget()) {
        for (QWidget *sibling :
             host->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
            if (sibling != this && !sibling->isWindow())
                block(sibling);
        }
    }
    if (auto *mainWindow = qobject_cast<QMainWindow *>(window())) {
        if (mainWindow->menuWidget())
            block(mainWindow->menuWidget());
    }
    m_loop = &loop;
    m_loopPage = page;
    m_loopAccepted = false;
    loop.exec();
    for (const QPointer<QWidget> &widget : blocked) {
        if (widget)
            widget->setEnabled(true);
    }
    if (!self)
        return false;
    m_loop = nullptr;
    return m_loopAccepted;
}

void WelcomeOverlay::finishLoop(bool accepted)
{
    m_loopAccepted = accepted;
    if (m_loop)
        m_loop->quit();
}

void WelcomeOverlay::closeOverlay()
{
    setBusy(false);
    m_passwordEdit->clear();
    m_totpEdit->clear();
    if (!m_driving)
        m_firstRun = false;
    hide();
}

void WelcomeOverlay::finish()
{
    const bool followUp = m_firstRun;
    QSettings().setValue(QLatin1String(kWelcomeSeenSetting), true);
    closeOverlay();
    if (followUp && hooks.afterClose)
        hooks.afterClose();
}

void WelcomeOverlay::dismiss()
{
    if (flowActive())
        return;
    finish();
}

void WelcomeOverlay::openHome(bool firstRun)
{
    if (flowActive()) {
        raise();
        return;
    }
    m_firstRun = firstRun || (isOpen() && m_firstRun);
    openPage(WelcomePage::Home);
}

void WelcomeOverlay::openBrowserWait(const QString &host)
{
    if (flowActive())
        return;
    m_browserLede->setText(
        host.isEmpty()
            ? QStringLiteral("Create your account in the browser, then come "
                             "back and sign in.")
            : QStringLiteral("Create your account on %1, then come back and "
                             "sign in.")
                  .arg(host));
    if (!isOpen())
        m_firstRun = false;
    openPage(WelcomePage::Browser);
}

void WelcomeOverlay::beginSignIn()
{
    if (flowActive() || !hooks.signIn)
        return;
    const QPointer<WelcomeOverlay> self(this);
    const std::function<void()> signIn = hooks.signIn;
    m_driving = true;
    m_returnToAccount = false;
    m_accountNoticeSticky = false;
    for (;;) {
        const int prompts = m_accountPrompts;
        signIn();
        if (!self)
            return;
        if (!m_returnToAccount || m_accountPrompts == prompts)
            break;
    }
    m_driving = false;
    m_returnToAccount = false;
    m_accountNoticeSticky = false;
    setBusy(false);
    if (hooks.signedIn && hooks.signedIn()) {
        finish();
        return;
    }
    openPage(WelcomePage::Home);
}

bool WelcomeOverlay::promptAccount(const QString &suggestedAccount,
                                   const QString &suggestedInstance,
                                   QString *account, QString *instance)
{
    if (m_loop)
        return false;
    ++m_accountPrompts;
    const bool keepInput = m_driving && m_returnToAccount;
    m_returnToAccount = false;
    if (!keepInput) {
        m_accountEdit->setText(suggestedAccount);
        m_instanceEdit->setText(suggestedInstance);
    }
    if (!keepInput || !m_accountNoticeSticky)
        m_accountNotice->clear();
    m_accountNoticeSticky = false;
    m_accountBack->setText(m_driving ? QStringLiteral("Back")
                                     : QStringLiteral("Cancel"));
    setBusy(false);
    openPage(WelcomePage::Account);

    const QPointer<WelcomeOverlay> self(this);
    const bool accepted = runLoop(WelcomePage::Account);
    if (!self)
        return false;
    if (!accepted) {
        if (!m_driving)
            closeOverlay();
        return false;
    }
    if (account)
        *account = m_accountEdit->text().trimmed().toLower();
    if (instance)
        *instance = m_instanceEdit->text();
    if (m_driving)
        setBusy(true);
    else
        closeOverlay();
    return true;
}

bool WelcomeOverlay::runCredentials(const QString &accountName)
{
    if (m_loop || !hooks.verify)
        return false;
    m_loginAccount = accountName;
    m_credTitle->setText(QStringLiteral("Log in to @%1").arg(accountName));
    m_passwordEdit->clear();
    m_totpEdit->clear();
    m_credNotice->clear();
    m_awaitingAck = false;
    m_credSubmit->setText(QStringLiteral("Log in"));
    m_credBack->setText(m_driving ? QStringLiteral("Back")
                                  : QStringLiteral("Cancel"));
    m_credBack->show();
    setBusy(false);
    openPage(WelcomePage::Credentials);

    const QPointer<WelcomeOverlay> self(this);
    const bool accepted = runLoop(WelcomePage::Credentials);
    if (!self)
        return false;
    const bool loggedIn = accepted || m_awaitingAck;
    m_awaitingAck = false;
    m_passwordEdit->clear();
    m_totpEdit->clear();
    if (loggedIn || !m_driving)
        closeOverlay();
    return loggedIn;
}

bool WelcomeOverlay::showNotice(const QString &text, bool error)
{
    if (!isOpen() || text.isEmpty())
        return false;
    const kit::Tone tone = error ? kit::Tone::Danger : kit::Tone::Accent;
    if (m_page == WelcomePage::Credentials) {
        m_credNotice->showMessage(text, tone);
        return true;
    }
    if (m_page == WelcomePage::Account) {
        m_accountNotice->showMessage(text, tone);
        if (error && m_driving && !m_loop) {
            m_returnToAccount = true;
            m_accountNoticeSticky = true;
        }
        return true;
    }
    return false;
}

void WelcomeOverlay::submitAccount()
{
    if (m_busy || !m_loop || m_loopPage != WelcomePage::Account)
        return;
    const QString name = m_accountEdit->text().trimmed().toLower();
    if (!isValidNodeName(name)) {
        m_accountNotice->showMessage(
            QStringLiteral("Enter a valid username (lowercase letters, "
                           "numbers and hyphens; start with a letter)."),
            kit::Tone::Danger);
        m_accountEdit->setFocus(Qt::OtherFocusReason);
        return;
    }
    const QString problem = hooks.instanceError
                                ? hooks.instanceError(m_instanceEdit->text())
                                : QString();
    if (!problem.isEmpty()) {
        m_accountNotice->showMessage(problem, kit::Tone::Danger);
        m_instanceEdit->setFocus(Qt::OtherFocusReason);
        return;
    }
    m_accountNotice->clear();
    finishLoop(true);
}

void WelcomeOverlay::leaveAccount()
{
    if (m_busy)
        return;
    if (m_loop && m_loopPage == WelcomePage::Account) {
        finishLoop(false);
        return;
    }
    if (!flowActive()) {
        showPage(WelcomePage::Home);
        focusPage();
    }
}

void WelcomeOverlay::submitCredentials()
{
    if (m_busy || !m_loop || m_loopPage != WelcomePage::Credentials)
        return;
    if (m_awaitingAck) {
        finishLoop(true);
        return;
    }
    const QString email = m_emailEdit->text().trimmed();
    if (!email.contains(QLatin1Char('@'))) {
        m_credNotice->showMessage(QStringLiteral("Enter a valid email."),
                                  kit::Tone::Danger);
        m_emailEdit->setFocus(Qt::OtherFocusReason);
        return;
    }
    const QString password = m_passwordEdit->text();
    const QString totp = m_totpEdit->text().trimmed();
    m_credNotice->clear();
    setBusy(true);

    const QPointer<WelcomeOverlay> self(this);
    const auto verify = hooks.verify;
    const QString account = m_loginAccount;
    bool fatal = false;
    const bool loggedIn = verify(email, password, totp, account, &fatal);
    if (!self)
        return;
    setBusy(false);

    if (loggedIn) {
        if (m_credNotice->text().isEmpty()) {
            finishLoop(true);
            return;
        }
        m_awaitingAck = true;
        m_emailEdit->setEnabled(false);
        m_passwordEdit->setEnabled(false);
        m_totpEdit->setEnabled(false);
        m_credBack->hide();
        m_credSubmit->setText(QStringLiteral("Continue"));
        m_credSubmit->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (m_credNotice->text().isEmpty()) {
        m_credNotice->showMessage(
            QStringLiteral("Could not log in. Check your details and try "
                           "again."),
            kit::Tone::Danger);
    }
    if (fatal && m_driving) {
        m_accountNotice->showMessage(m_credNotice->text(), kit::Tone::Danger);
        m_accountNoticeSticky = true;
        m_returnToAccount = true;
        finishLoop(false);
        return;
    }
    m_passwordEdit->setFocus(Qt::OtherFocusReason);
    m_passwordEdit->selectAll();
}

void WelcomeOverlay::leaveCredentials()
{
    if (m_busy || !m_loop || m_loopPage != WelcomePage::Credentials)
        return;
    if (m_awaitingAck) {
        finishLoop(true);
        return;
    }
    if (m_driving)
        m_returnToAccount = true;
    finishLoop(false);
}

void WelcomeOverlay::activatePrimary()
{
    if (m_busy)
        return;
    if (auto *focused =
            qobject_cast<QPushButton *>(QApplication::focusWidget())) {
        if (isAncestorOf(focused) && focused->isEnabled()) {
            focused->click();
            return;
        }
    }
    switch (m_page) {
    case WelcomePage::Home:
        m_homeSignIn->click();
        break;
    case WelcomePage::Account:
        m_accountContinue->click();
        break;
    case WelcomePage::Credentials:
        m_credSubmit->click();
        break;
    case WelcomePage::Browser:
        m_browserSignIn->click();
        break;
    }
}

void WelcomeOverlay::activateBack()
{
    if (m_busy)
        return;
    switch (m_page) {
    case WelcomePage::Home:
        dismiss();
        break;
    case WelcomePage::Account:
        leaveAccount();
        break;
    case WelcomePage::Credentials:
        leaveCredentials();
        break;
    case WelcomePage::Browser:
        m_browserBack->click();
        break;
    }
}

bool WelcomeOverlay::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Escape || key == Qt::Key_Return ||
            key == Qt::Key_Enter) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

bool WelcomeOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

bool WelcomeOverlay::focusNextPrevChild(bool next)
{
    if (isHidden())
        return QWidget::focusNextPrevChild(next);
    QList<QWidget *> chain;
    if (QWidget *page = m_pages.value(static_cast<int>(m_page))) {
        const QList<QWidget *> widgets = page->findChildren<QWidget *>();
        for (QWidget *widget : widgets) {
            if ((widget->focusPolicy() & Qt::TabFocus) &&
                widget->isEnabled() && widget->isVisibleTo(this)) {
                chain.append(widget);
            }
        }
    }
    if (chain.isEmpty()) {
        setFocus(Qt::OtherFocusReason);
        return true;
    }
    const qsizetype count = chain.size();
    qsizetype index = chain.indexOf(QApplication::focusWidget());
    if (index < 0)
        index = next ? 0 : count - 1;
    else
        index = (index + (next ? 1 : count - 1)) % count;
    chain.at(index)->setFocus(next ? Qt::TabFocusReason
                                   : Qt::BacktabFocusReason);
    return true;
}

void WelcomeOverlay::keyPressEvent(QKeyEvent *event)
{
    event->accept();
    if (event->isAutoRepeat())
        return;
    const int key = event->key();
    if (key == Qt::Key_Escape)
        activateBack();
    else if (key == Qt::Key_Return || key == Qt::Key_Enter)
        activatePrimary();
}

void WelcomeOverlay::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void WelcomeOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void WelcomeOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
}

void WelcomeOverlay::wheelEvent(QWheelEvent *event)
{
    event->accept();
}

void WelcomeOverlay::contextMenuEvent(QContextMenuEvent *event)
{
    event->accept();
}

class WelcomeState final : public QObject
{
public:
    explicit WelcomeState(QObject *parent) : QObject(parent)
    {
        setObjectName(QLatin1String(kWelcomeStateName));
    }

    WelcomeHooks hooks;
    QPointer<WelcomeOverlay> overlay;
};

WelcomeState *welcomeState(const MainWindow &window)
{
    return dynamic_cast<WelcomeState *>(window.findChild<QObject *>(
        QLatin1String(kWelcomeStateName), Qt::FindDirectChildrenOnly));
}

WelcomeOverlay *existingWelcome(const MainWindow &window)
{
    const WelcomeState *state = welcomeState(window);
    return state ? state->overlay.data() : nullptr;
}

WelcomeOverlay *ensureWelcome(MainWindow &window)
{
    WelcomeState *state = welcomeState(window);
    if (!state) {
        NextAccess::installWelcome(window);
        state = welcomeState(window);
    }
    if (!state)
        return nullptr;
    QWidget *parent = NextAccess::overlayParent(window);
    if (!parent)
        return state->overlay.data();
    if (!state->overlay) {
        state->overlay = new WelcomeOverlay(parent);
        state->overlay->hooks = state->hooks;
    } else if (state->overlay->parentWidget() != parent &&
               !state->overlay->flowActive()) {
        state->overlay->attachTo(parent);
    }
    return state->overlay.data();
}

}

void NextAccess::installWelcome(MainWindow &window)
{
    if (!Theme::isNext() || welcomeState(window))
        return;
    auto *state = new WelcomeState(&window);
    MainWindow *target = &window;
    state->hooks.signIn = [target] {
        const QPointer<MainWindow> guard(target);
        target->loginToUserAccount();
        if (!guard)
            return;
        target->updateUserSwitcher();
        target->updateSignInButton();
    };
    state->hooks.signedIn = [target] {
        return target->hasActiveAccountSession();
    };
    state->hooks.createAccount = [target] {
        NextAccess::openCreateAccount(*target);
    };
    state->hooks.canLinkNode = [target] {
        const QString owner = target->accountOwner();
        return !owner.isEmpty() && target->hasOwnerSigningCapability(owner);
    };
    state->hooks.linkNode = [target] { target->openLinkNodeInBrowser(); };
    state->hooks.afterClose = [target] { target->showFirstRunWelcome(); };
    state->hooks.instanceHost = [target] {
        return serverHostDisplay(target->currentAccountInstanceUrl());
    };
    state->hooks.instanceError = [target](const QString &text) -> QString {
        const QUrl url =
            target->accountApiUrlForInstance(text, QStringLiteral("login"));
        if (!url.isValid() || url.host().isEmpty())
            return QStringLiteral("Enter a valid ForkMesh instance host.");
        const bool web = url.scheme() == QLatin1String("http") ||
                         url.scheme() == QLatin1String("https");
        if (!web || !url.userName().isEmpty() || !url.password().isEmpty())
            return QStringLiteral(
                "Choose a valid ws:// or wss:// ForkMesh instance.");
        return QString();
    };
    state->hooks.verify = [target](const QString &email,
                                   const QString &password,
                                   const QString &totp,
                                   const QString &account, bool *fatal) {
        return target->verifyTotpLogin(email, password, totp, account, fatal);
    };
}

bool NextAccess::maybeShowWelcome(MainWindow &window)
{
    if (!Theme::isNext())
        return false;
    if (const WelcomeOverlay *open = existingWelcome(window);
        open && open->isOpen())
        return true;
    if (QSettings().value(QLatin1String(kWelcomeSeenSetting), false).toBool())
        return false;
    if (window.hasActiveAccountSession() ||
        !window.nodeOwnerDisplayName().trimmed().isEmpty())
        return false;
    showWelcome(window);
    const WelcomeOverlay *overlay = existingWelcome(window);
    return overlay && overlay->isOpen();
}

void NextAccess::showWelcome(MainWindow &window)
{
    if (WelcomeOverlay *overlay = ensureWelcome(window))
        overlay->openHome(true);
}

void NextAccess::openSignIn(MainWindow &window)
{
    if (WelcomeOverlay *overlay = ensureWelcome(window))
        overlay->openHome(false);
}

void NextAccess::openCreateAccount(MainWindow &window)
{
    QUrl url = window.catalogApiUrl();
    url.setPath(QStringLiteral("/signup"));
    QDesktopServices::openUrl(url);
    window.logSystem(QStringLiteral(
        "Account: opened the browser to create a ForkMesh account."));
    if (WelcomeOverlay *overlay = ensureWelcome(window))
        overlay->openBrowserWait(serverHostDisplay(url.toString()));
}

bool NextAccess::promptInstanceAccount(MainWindow &window,
                                       const QString &suggestedAccount,
                                       const QString &suggestedInstance,
                                       QString *account, QString *instance)
{
    WelcomeOverlay *overlay = ensureWelcome(window);
    return overlay && overlay->promptAccount(suggestedAccount,
                                             suggestedInstance, account,
                                             instance);
}

bool NextAccess::runLoginPanel(MainWindow &window, const QString &accountName)
{
    WelcomeOverlay *overlay = ensureWelcome(window);
    return overlay && overlay->runCredentials(accountName);
}

bool NextAccess::accountNotice(MainWindow &window, const QString &title,
                               const QString &text, bool error)
{
    Q_UNUSED(title);
    if (!Theme::isNext())
        return false;
    WelcomeOverlay *overlay = existingWelcome(window);
    return overlay && overlay->showNotice(text, error);
}

}
