#include "RunnersFeature.h"

#include <QAbstractButton>
#include <QAction>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::runnerspage {
namespace {

const QString kApiPath = QStringLiteral("/api/runners");
const QString kAttentionPath = QStringLiteral("/api/runners/attention");

QLabel *label(const QString &value, const char *objectName, bool wrap = true)
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QLatin1String(objectName));
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(wrap);
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return widget;
}

QLabel *badge(const QString &value, const QString &tone, bool outline = false)
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QStringLiteral("badge"));
    widget->setTextFormat(Qt::PlainText);
    widget->setAlignment(Qt::AlignCenter);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    widget->setProperty("tone", tone);
    if (outline)
        widget->setProperty("outline", true);
    return widget;
}

QPushButton *button(const QString &text, const char *objectName, const QString &tooltip)
{
    auto *widget = new QPushButton(text);
    widget->setObjectName(QLatin1String(objectName));
    widget->setCursor(Qt::PointingHandCursor);
    widget->setToolTip(tooltip);
    return widget;
}

class StatusDot final : public QWidget
{
public:
    explicit StatusDot(bool online) : m_online(online)
    {
        setObjectName(QStringLiteral("runnerStatusDot"));
        setProperty("online", online);
        setFixedSize(10, 10);
        setAccessibleName(online ? QStringLiteral("Online") : QStringLiteral("Offline"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor colour = m_online ? (dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37))
                                       : palette().color(QPalette::PlaceholderText);
        painter.setBrush(colour);
        painter.drawEllipse(QRectF(1, 1, width() - 2, height() - 2));
    }

private:
    bool m_online = false;
};

}

RunnersFeature::RunnersFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("runnersSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(8);
    heading->addWidget(label(QStringLiteral("Runners"), "pageTitle", false));
    heading->addStretch();
    m_refresh = button(QStringLiteral("Refresh"), "ghostButton",
                       QStringLiteral("Read your runners again"));
    m_add = button(QStringLiteral("Add runner"), "primaryButton",
                   QStringLiteral("Set up a new runner on the web"));
    m_add->setProperty("runnersAdd", true);
    heading->addWidget(m_refresh);
    heading->addWidget(m_add);
    outer->addLayout(heading);

    m_status = label(QString(), "statusLine");
    m_status->hide();
    outer->addWidget(m_status);

    m_error = new QFrame;
    m_error->setObjectName(QStringLiteral("inlineNotice"));
    m_error->setProperty("tone", QStringLiteral("danger"));
    m_error->setAttribute(Qt::WA_StyledBackground, true);
    auto *errorLayout = new QHBoxLayout(m_error);
    errorLayout->setContentsMargins(12, 8, 12, 8);
    m_errorText = label(QString(), "runnersError");
    errorLayout->addWidget(m_errorText, 1);
    m_error->hide();
    outer->addWidget(m_error);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("runnersScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_add, &QPushButton::clicked, this, [this] { openPage(addRunnerPath()); });
    render();
}

void RunnersFeature::request(const QByteArray &method, const QString &path,
                             const QJsonObject &body,
                             forkmesh::market::NativePackageReplyHandler handler)
{
    if (!m_host.request) {
        handler(false, {}, QStringLiteral("network access is unavailable"));
        return;
    }
    const QPointer<RunnersFeature> guard(this);
    m_host.request(method, path, body,
                   [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                         const QString &error) {
        if (guard)
            handler(ok, payload, error);
    });
}

void RunnersFeature::refresh()
{
    if (m_loading) {
        m_reloadPending = true;
        return;
    }
    m_loading = true;
    m_refresh->setEnabled(false);
    setStatus(m_loaded ? QString::fromUtf8("Refreshing\xE2\x80\xA6")
                       : QString::fromUtf8("Loading runners\xE2\x80\xA6"));
    request(QByteArrayLiteral("GET"), kApiPath, {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        m_refresh->setEnabled(true);
        if (std::exchange(m_reloadPending, false)) {
            refresh();
            return;
        }
        if (!ok) {
            m_notice.clear();
            setStatus(QString());
            m_signedOut = signedOutError(error);
            if (m_signedOut) {
                m_payload = {};
                m_loaded = false;
                setError(QString());
                setBadge(0);
            } else {
                setError(loadErrorText(error));
            }
            render();
            return;
        }
        m_signedOut = false;
        m_payload = payload;
        m_loaded = true;
        setError(QString());
        setStatus(std::exchange(m_notice, QString()));
        render();
        loadAttention();
    });
}

int RunnersFeature::navigationBadge() const
{
    return m_badge;
}

void RunnersFeature::prepareNavigationBadge()
{
    if (!m_attentionLoaded)
        loadAttention();
}

void RunnersFeature::loadAttention()
{
    if (m_attentionLoading)
        return;
    m_attentionLoading = true;
    request(QByteArrayLiteral("GET"), kAttentionPath, {},
            [this](bool ok, const QJsonObject &payload, const QString &) {
        m_attentionLoading = false;
        if (!ok)
            return;
        m_attentionLoaded = true;
        setBadge(attentionCount(payload));
    });
}

void RunnersFeature::setBadge(int count)
{
    if (count == m_badge)
        return;
    m_badge = count;
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void RunnersFeature::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void RunnersFeature::setError(const QString &text)
{
    m_errorText->setText(text);
    m_error->setVisible(!text.isEmpty());
}

void RunnersFeature::render()
{
    const int scrolled = m_scroll->verticalScrollBar()->value();
    if (QWidget *old = m_scroll->takeWidget()) {
        old->hide();
        old->setParent(nullptr);
        old->deleteLater();
    }
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("runnersBody"));
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(10);
    if (m_signedOut) {
        addState(layout, QStringLiteral("Sign in to see your runners"),
                 QStringLiteral("Runners are managed with your ForkMesh account session. Sign in "
                                "from the Account menu, then press Refresh."),
                 "runnersSignedOut");
    } else if (!m_loaded) {
        layout->addStretch();
    } else {
        const QList<RunnerRow> rows = runnerRows(m_payload, QDateTime::currentMSecsSinceEpoch());
        if (rows.isEmpty()) {
            addState(layout, QStringLiteral("No runners yet"),
                     QStringLiteral("A runner is a machine of yours that runs agent jobs with its "
                                    "own Claude Code and Codex sign-ins. Add one, then press "
                                    "Refresh."),
                     "runnersEmpty");
        } else {
            auto *card = new QFrame;
            card->setObjectName(QStringLiteral("card"));
            card->setAttribute(Qt::WA_StyledBackground, true);
            auto *cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(14, 2, 14, 2);
            cardLayout->setSpacing(0);
            for (int index = 0; index < rows.size(); ++index)
                cardLayout->addWidget(runnerWidget(rows.at(index), index == 0));
            layout->addWidget(card);
            layout->addStretch();
        }
    }
    m_scroll->setWidget(body);
    m_scroll->verticalScrollBar()->setValue(scrolled);
}

void RunnersFeature::addState(QVBoxLayout *layout, const QString &title, const QString &text,
                              const char *name)
{
    auto *empty = new QWidget;
    empty->setObjectName(QStringLiteral("emptyState"));
    empty->setProperty("runnersState", QLatin1String(name));
    auto *column = new QVBoxLayout(empty);
    column->setContentsMargins(24, 48, 24, 24);
    column->setSpacing(6);
    QLabel *heading = label(title, "emptyStateTitle");
    heading->setAlignment(Qt::AlignHCenter);
    column->addWidget(heading);
    QLabel *detail = label(text, "emptyStateBody");
    detail->setAlignment(Qt::AlignHCenter);
    column->addWidget(detail);
    layout->addWidget(empty);
    layout->addStretch();
}

QWidget *RunnersFeature::runnerWidget(const RunnerRow &row, bool first)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("settingsRow"));
    frame->setProperty("firstRow", first);
    frame->setProperty("runner", row.name);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 10, 0, 10);
    layout->setSpacing(4);

    const bool busy = m_busyRunner == row.name;
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    auto *dot = new StatusDot(row.online);
    dot->setToolTip(row.detail);
    top->addWidget(dot, 0, Qt::AlignVCenter);
    top->addWidget(label(row.name, "settingsRowTitle", false));
    if (!row.scope.isEmpty())
        top->addWidget(badge(row.scope, QStringLiteral("neutral"), true));
    if (!row.statusLabel.isEmpty()) {
        QLabel *status = badge(row.statusLabel, row.statusTone);
        status->setProperty("runnerStatus", row.status);
        top->addWidget(status);
    }
    top->addStretch();
    for (RunnerAction action : row.actions) {
        const bool revoke = action == RunnerAction::Revoke;
        QPushButton *control = button(
            actionLabel(action), revoke ? "dangerButton" : "ghostButton",
            revoke ? QStringLiteral("Stop %1 for good; it cannot be undone").arg(row.name)
            : action == RunnerAction::Pause
                ? QStringLiteral("Stop giving %1 new jobs; it finishes the ones it holds").arg(row.name)
                : QStringLiteral("Let %1 take new jobs again").arg(row.name));
        control->setProperty("runnerAction", actionName(action));
        control->setEnabled(!busy);
        const QString name = row.name;
        connect(control, &QPushButton::clicked, this, [this, action, name] {
            if (action == RunnerAction::Revoke)
                confirmRevoke(name);
            else
                runAction(action, name);
        });
        top->addWidget(control);
    }
    layout->addLayout(top);
    QLabel *detail = label(row.detail, "settingsRowDescription");
    detail->setContentsMargins(18, 0, 0, 0);
    layout->addWidget(detail);
    for (const ProviderRow &provider : row.providers)
        layout->addWidget(providerWidget(row, provider));
    return frame;
}

QWidget *RunnersFeature::providerWidget(const RunnerRow &runner, const ProviderRow &provider)
{
    auto *line = new QWidget;
    line->setObjectName(QStringLiteral("runnerProviderLine"));
    line->setProperty("provider", provider.provider);
    auto *layout = new QHBoxLayout(line);
    layout->setContentsMargins(18, 2, 0, 0);
    layout->setSpacing(8);
    QLabel *title = label(provider.title, "settingsRowDescription", false);
    title->setMinimumWidth(90);
    layout->addWidget(title);
    QLabel *state = badge(provider.state, provider.tone);
    state->setProperty("providerState", provider.provider);
    layout->addWidget(state);
    if (!provider.account.isEmpty())
        layout->addWidget(label(provider.account, "settingsRowDescription", false));
    layout->addStretch();
    const bool busy = m_busyRunner == runner.name;
    const QString name = runner.name;
    const QString providerId = provider.provider;
    if (provider.signInOffered) {
        QPushButton *signInButton = button(
            provider.signInText, "ghostButton",
            QStringLiteral("Finish the %1 sign-in for %2 in your browser. The credential stays "
                           "on the runner.")
                .arg(provider.title, runner.name));
        signInButton->setProperty("runnerSignIn", providerId);
        signInButton->setEnabled(!busy);
        connect(signInButton, &QPushButton::clicked, this,
                [this, name, providerId] { signIn(name, providerId); });
        layout->addWidget(signInButton);
    }
    if (provider.accountsOffered) {
        QPushButton *accounts = button(
            QStringLiteral("Accounts"), "ghostButton",
            QStringLiteral("Switch the account every %1 job on %2 uses, or remove one")
                .arg(provider.title, runner.name));
        accounts->setProperty("runnerAccounts", providerId);
        accounts->setEnabled(!busy);
        auto *menu = new QMenu(accounts);
        menu->setObjectName(QStringLiteral("runnerAccountsMenu"));
        for (const AccountChoice &account : provider.accounts) {
            QAction *use = menu->addAction(account.pending
                                               ? account.name + QString::fromUtf8(" \xC2\xB7 switching\xE2\x80\xA6")
                                               : account.name);
            use->setObjectName(QStringLiteral("runnerAccountUse-") + account.id);
            use->setCheckable(true);
            use->setChecked(account.active);
            use->setEnabled(!account.active && !account.pending);
            connect(use, &QAction::triggered, this, [this, name, providerId, account] {
                changeAccount(name, providerId, account, QStringLiteral("activate"));
            });
        }
        menu->addSeparator();
        for (const AccountChoice &account : provider.accounts) {
            QAction *remove = menu->addAction(
                QStringLiteral("Remove %1").arg(account.name) + QString::fromUtf8("\xE2\x80\xA6"));
            remove->setObjectName(QStringLiteral("runnerAccountRemove-") + account.id);
            remove->setEnabled(!account.pending);
            connect(remove, &QAction::triggered, this, [this, name, providerId, account] {
                confirmRemoveAccount(name, providerId, account);
            });
        }
        accounts->setMenu(menu);
        layout->addWidget(accounts);
    }
    return line;
}

void RunnersFeature::runAction(RunnerAction action, const QString &runner)
{
    if (!m_busyRunner.isEmpty())
        return;
    m_busyRunner = runner;
    setError(QString());
    setStatus(actionProgressText(action, runner));
    render();
    request(QByteArrayLiteral("POST"), actionPath(action), actionBody(runner),
            [this, action, runner](bool ok, const QJsonObject &, const QString &error) {
        m_busyRunner.clear();
        if (!ok) {
            setStatus(QString());
            if (signedOutError(error)) {
                m_signedOut = true;
                render();
                return;
            }
            setError(actionErrorText(error));
            render();
            return;
        }
        m_notice = actionDoneText(action, runner);
        refresh();
    });
}

void RunnersFeature::confirmRevoke(const QString &runner)
{
    auto *box = new QMessageBox(QMessageBox::Warning, revokeTitle(runner), revokeQuestion(runner),
                                QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("runnerRevokeConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *revoke = box->addButton(QStringLiteral("Revoke"), QMessageBox::DestructiveRole);
    revoke->setObjectName(QStringLiteral("dangerButton"));
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::buttonClicked, this, [this, revoke, runner](QAbstractButton *clicked) {
        if (clicked == revoke)
            runAction(RunnerAction::Revoke, runner);
    });
    box->open();
}

void RunnersFeature::changeAccount(const QString &runner, const QString &provider,
                                   const AccountChoice &account, const QString &action)
{
    if (!m_busyRunner.isEmpty())
        return;
    m_busyRunner = runner;
    setError(QString());
    render();
    request(QByteArrayLiteral("POST"), accountsPath(runner), accountBody(provider, account.id, action),
            [this, runner, account, action](bool ok, const QJsonObject &, const QString &error) {
        m_busyRunner.clear();
        if (!ok) {
            if (signedOutError(error)) {
                m_signedOut = true;
                render();
                return;
            }
            setError(actionErrorText(error));
            render();
            return;
        }
        m_notice = accountDoneText(action, account.name, runner);
        refresh();
    });
}

void RunnersFeature::confirmRemoveAccount(const QString &runner, const QString &provider,
                                          const AccountChoice &account)
{
    auto *box = new QMessageBox(QMessageBox::Question,
                                QStringLiteral("Remove %1 account").arg(providerTitle(provider)),
                                removeAccountQuestion(account.name, runner), QMessageBox::Cancel,
                                this);
    box->setObjectName(QStringLiteral("runnerAccountRemoveConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *remove = box->addButton(QStringLiteral("Remove"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("dangerButton"));
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::buttonClicked, this,
            [this, remove, runner, provider, account](QAbstractButton *clicked) {
        if (clicked == remove)
            changeAccount(runner, provider, account, QStringLiteral("remove"));
    });
    box->open();
}

void RunnersFeature::signIn(const QString &runner, const QString &provider)
{
    openPage(connectPath(runner, provider));
    setStatus(QStringLiteral("Finish the %1 sign-in for %2 in your browser, then press Refresh.")
                  .arg(providerTitle(provider), runner));
}

void RunnersFeature::openPage(const QString &path)
{
    if (m_host.openWebPage)
        m_host.openWebPage(path);
    else
        setError(QStringLiteral("This desktop cannot open web pages from plugins. Open %1 on "
                                "your ForkMesh host in a browser.")
                     .arg(path));
}

}

namespace {
const forkmesh::market::NativePackageRegistration runnersRegistration({
    QStringLiteral("builtin:runners"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Runners"),
    QStringLiteral("server"),
    QStringLiteral("/api/runners"),
    QStringLiteral("forkmesh-runners-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::runnerspage::RunnersFeature(host, parent);
    },
});
}
