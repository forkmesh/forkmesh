#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentUsageResetClock.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

using namespace forkmesh::ui;

namespace {

class AgentUsagePage final : public QWidget
{
public:
    using QWidget::QWidget;
    std::function<void()> onShow;
    QWidget *body = nullptr;

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (onShow)
            onShow();
    }
};

QLabel *pageLabel(const QString &text, const char *objectName, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QPushButton *pageButton(const QString &objectName, const QString &text,
                        const QString &toolTip, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setProperty("agentUsagePageAction", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    return button;
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        else if (QLayout *child = item->layout())
            clearLayout(child);
        delete item;
    }
}

}

QWidget *MainWindow::buildAgentUsagePage(QWidget *parent)
{
    auto *page = new AgentUsagePage(parent);
    page->setObjectName(QStringLiteral("agentUsagePage"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(10);
    heading->addWidget(pageLabel(QStringLiteral("Usage"), "pageTitle", page));
    heading->addWidget(
        pageLabel(QStringLiteral("Claude Code and Codex accounts on this desktop"),
                  "pageLede", page),
        0, Qt::AlignBottom);
    heading->addStretch();
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), page);
    refresh->setObjectName(QStringLiteral("ghostButton"));
    refresh->setAccessibleName(QStringLiteral("Refresh usage"));
    refresh->setCursor(Qt::PointingHandCursor);
    refresh->setToolTip(QStringLiteral("Read each Claude Code account's usage again, one by one"));
    heading->addWidget(refresh);
    outer->addLayout(heading);

    auto *scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("agentUsagePageScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    page->body = new QWidget;
    page->body->setObjectName(QStringLiteral("agentUsagePageBody"));
    auto *bodyLayout = new QVBoxLayout(page->body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(6);
    scroll->setWidget(page->body);
    outer->addWidget(scroll, 1);

    const QPointer<AgentUsagePage> guarded(page);
    const auto readAgain = [this, guarded] {
        refreshClaudeCodeUsage();
        refreshCodexUsageRemaining();
        reconcileClaudeAccountLogins();
        refreshRunners();
        if (guarded)
            fillAgentUsagePage(guarded);
    };
    const QPointer<QPushButton> guardedRefresh(refresh);
    connect(refresh, &QPushButton::clicked, page, [this, guarded, guardedRefresh] {
        if (guardedRefresh) {
            guardedRefresh->setEnabled(false);
            guardedRefresh->setText(QString::fromUtf8("Refreshing\xE2\x80\xA6"));
        }
        reconcileClaudeAccountLogins();
        refreshRunners();
        refreshAgentUsageAccounts([this, guarded, guardedRefresh] {
            if (guardedRefresh) {
                guardedRefresh->setEnabled(true);
                guardedRefresh->setText(QStringLiteral("Refresh"));
            }
            if (guarded)
                fillAgentUsagePage(guarded);
        });
    });
    page->onShow = readAgain;

    m_agentUsagePages.removeAll(QPointer<QWidget>());
    m_agentUsagePages.append(page);
    fillAgentUsagePage(page);
    return page;
}

void MainWindow::scheduleAgentUsagePagesRefresh()
{
    m_agentUsagePages.removeAll(QPointer<QWidget>());
    if (m_agentUsagePages.isEmpty())
        return;
    if (!m_agentUsagePagesTimer) {
        m_agentUsagePagesTimer = new QTimer(this);
        m_agentUsagePagesTimer->setSingleShot(true);
        m_agentUsagePagesTimer->setInterval(150);
        connect(m_agentUsagePagesTimer, &QTimer::timeout, this, [this] {
            for (const QPointer<QWidget> &page : std::as_const(m_agentUsagePages)) {
                if (page && page->isVisible())
                    fillAgentUsagePage(page);
            }
        });
    }
    m_agentUsagePagesTimer->start();
}

void MainWindow::fillAgentUsagePage(QWidget *target)
{
    auto *page = static_cast<AgentUsagePage *>(target);
    if (!page || !page->body)
        return;
    QWidget *body = page->body;
    auto *layout = static_cast<QVBoxLayout *>(body->layout());
    clearLayout(layout);

    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QColor iconColor = body->palette().color(QPalette::PlaceholderText);
    QStringList providers{kClaudeCodeProvider, kCodexProvider};
    if (agentIsCodexProvider(defaultAgentProvider()))
        std::swap(providers[0], providers[1]);

    for (const QString &provider : providers) {
        const bool codex = agentIsCodexProvider(provider);
        const bool isDefault = provider == providers.first();
        const QString providerName =
            codex ? QStringLiteral("Codex") : QStringLiteral("Claude Code");
        const QString key = codex ? QStringLiteral("Codex") : QStringLiteral("Claude");
        const AgentAccountProfile active = activeAgentAccount(provider);
        const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
        QString slot = key;
        for (int i = 0; i < profiles.size(); ++i)
            if (profiles.at(i).id == active.id)
                slot = QStringLiteral("%1 %2").arg(key).arg(i + 1);

        auto *section = new QWidget(body);
        section->setObjectName(QStringLiteral("agentUsagePageProvider%1").arg(key));
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 8, 0, 8);
        sectionLayout->setSpacing(6);

        auto *header = new QHBoxLayout;
        header->setSpacing(10);
        header->addWidget(pageLabel(providerName, "groupHeading", section));
        if (isDefault) {
            header->addWidget(pageLabel(QStringLiteral("Used for new agents"),
                                        "statusLine", section),
                              0, Qt::AlignVCenter);
        } else {
            QPushButton *makeDefault = pageButton(
                QStringLiteral("agentUsagePageMakeDefault%1").arg(key),
                QStringLiteral("Use %1 for new agents").arg(providerName),
                QStringLiteral("Make %1 the default agent provider, and show its "
                               "usage on the Usage button")
                    .arg(providerName),
                section);
            connect(makeDefault, &QPushButton::clicked, this, [this, provider] {
                QSettings().setValue(kDefaultAgentProviderSetting, provider);
                if (m_defaultAgentProviderCombo) {
                    const QSignalBlocker blocker(m_defaultAgentProviderCombo);
                    selectDefaultAgentProvider(m_defaultAgentProviderCombo);
                }
                selectDefaultAgentProvider(m_quickAddAgentProvider);
                selectDefaultAgentProvider(m_issueAgentProvider);
                selectDefaultAgentProvider(m_issuePrioritizeAgentCombo);
                refreshAgentToolbarAccountButtons();
                scheduleAgentUsagePagesRefresh();
            });
            header->addWidget(makeDefault);
        }
        header->addStretch(1);
        sectionLayout->addLayout(header);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(6);
        QPushButton *terminal = pageButton(
            QStringLiteral("agentUsagePageTerminal%1").arg(key),
            QStringLiteral("Open %1 in a terminal").arg(slot),
            QStringLiteral("Start a new %1 agent in a terminal inside ForkMesh")
                .arg(providerName),
            section);
        terminal->setIcon(themedOcticon(QStringLiteral("terminal"), iconColor, 14));
        connect(terminal, &QPushButton::clicked, this, [this, codex] {
            if (codex)
                openAgentProviderTerminal(QStringLiteral("codex"),
                                          QStringLiteral("Codex"),
                                          QStringLiteral("agentCodexTerminalDialog"),
                                          QStringLiteral("agentCodexTerminal"));
            else
                openAgentProviderTerminal(QStringLiteral("claude"),
                                          QStringLiteral("Claude Code"),
                                          QStringLiteral("agentClaudeTerminalDialog"),
                                          QStringLiteral("agentClaudeTerminal"));
        });
        actions->addWidget(terminal);
        QPushButton *systemTerminal = pageButton(
            QStringLiteral("agentUsagePageSystemTerminal%1").arg(key),
            QStringLiteral("Launch in system terminal"),
            QStringLiteral("Launch %1 in a system terminal").arg(providerName),
            section);
        connect(systemTerminal, &QPushButton::clicked, this,
                [this, provider] { launchAgentSystemTerminal(provider); });
        actions->addWidget(systemTerminal);
        QPushButton *add = pageButton(
            QStringLiteral("agentUsagePageAddAccount%1").arg(key),
            QString::fromUtf8("Add account\xE2\x80\xA6"),
            QStringLiteral("Add another %1 account to this desktop").arg(providerName),
            section);
        add->setIcon(themedOcticon(QStringLiteral("person"), iconColor, 14));
        connect(add, &QPushButton::clicked, this,
                [this, provider] { addAgentAccount(provider); });
        actions->addWidget(add);
        auto *runners = new QToolButton(section);
        runners->setObjectName(QStringLiteral("agentUsagePageRunnerAccounts%1").arg(key));
        runners->setText(QStringLiteral("Runner accounts"));
        runners->setToolTip(
            QStringLiteral("Choose which %1 account each runner uses for cloud agents")
                .arg(providerName));
        runners->setCursor(Qt::PointingHandCursor);
        runners->setPopupMode(QToolButton::InstantPopup);
        auto *runnerMenu = new QMenu(runners);
        runnerMenu->setObjectName(QStringLiteral("agentUsagePageRunnerMenu%1").arg(key));
        connect(runnerMenu, &QMenu::aboutToShow, this, [this, runnerMenu, provider] {
            fillAgentRunnerAccountsMenu(runnerMenu, provider);
        });
        runners->setMenu(runnerMenu);
        actions->addWidget(runners);
        actions->addStretch(1);
        sectionLayout->addLayout(actions);

        for (int i = 0; i < profiles.size(); ++i) {
            const AgentAccountProfile &profile = profiles.at(i);
            const QString identity = agentAccountIdentity(provider, profile.configDir);
            const bool signedIn = agentAccountSignedIn(provider, profile.configDir);
            const bool inUse = profile.id == active.id;

            auto *account = new QWidget(section);
            account->setObjectName(QStringLiteral("agentUsagePageAccount_%1").arg(profile.id));
            auto *accountLayout = new QVBoxLayout(account);
            accountLayout->setContentsMargins(0, 6, 0, 2);
            accountLayout->setSpacing(4);

            auto *row = new QHBoxLayout;
            row->setSpacing(6);
            QString baseLabel = QStringLiteral("%1 %2").arg(key).arg(i + 1);
            if (!profile.label.trimmed().isEmpty())
                baseLabel += dot + profile.label.trimmed();
            const QString suffix =
                signedIn ? QString() : QString::fromUtf8(" \xC2\xB7 not signed in");
            QLabel *name = pageLabel(
                baseLabel + (identity.isEmpty() ? QString()
                                                : QStringLiteral(" — ") + identity) +
                    suffix,
                "agentUsagePageAccountName", account);
            QFont nameFont = name->font();
            nameFont.setBold(inUse);
            name->setFont(nameFont);
            row->addWidget(name);
            if (!codex && signedIn && identity.isEmpty()) {
                auto *probe = new QAction(baseLabel, name);
                connect(probe, &QAction::changed, name, [name, probe, suffix] {
                    name->setText(probe->text() + suffix);
                });
                probeClaudeAgentAccountIdentity(profile.configDir, probe, baseLabel);
            }
            if (inUse) {
                row->addWidget(pageLabel(QStringLiteral("Runs new agents"),
                                         "statusLine", account),
                               0, Qt::AlignVCenter);
            } else {
                QPushButton *use = pageButton(
                    QStringLiteral("agentUsagePageUse_%1").arg(profile.id),
                    QStringLiteral("Run new agents as this account"),
                    QStringLiteral("Use this %1 account for new agents").arg(providerName),
                    account);
                connect(use, &QPushButton::clicked, this,
                        [this, provider, id = profile.id] {
                            selectAgentAccount(provider, id);
                            scheduleAgentUsagePagesRefresh();
                        });
                row->addWidget(use);
            }
            row->addStretch(1);
            const auto addControl = [&](const QString &objectName, const QString &text,
                                        const QString &toolTip, bool enabled,
                                        std::function<void()> run) {
                auto *button = new QToolButton(account);
                button->setObjectName(objectName.arg(profile.id));
                button->setAutoRaise(true);
                button->setCursor(Qt::PointingHandCursor);
                button->setText(text);
                button->setToolTip(toolTip);
                button->setAccessibleName(toolTip);
                button->setEnabled(enabled);
                connect(button, &QToolButton::clicked, this,
                        [this, run = std::move(run)] {
                            run();
                            scheduleAgentUsagePagesRefresh();
                        });
                row->addWidget(button);
                return button;
            };
            addControl(QStringLiteral("agentUsagePageEdit_%1"),
                       QString::fromUtf8("Edit account\xE2\x80\xA6"),
                       QStringLiteral("Change this account's label"), true,
                       [this, provider, id = profile.id] {
                           editAgentAccount(provider, id);
                       });
            addControl(QStringLiteral("agentUsagePageLogin_%1"),
                       QString::fromUtf8("Log in\xE2\x80\xA6"),
                       QStringLiteral("Sign this %1 account in from a system terminal")
                           .arg(providerName),
                       true,
                       [this, provider, id = profile.id] {
                           launchAgentSystemTerminal(provider, QStringLiteral("login"), id);
                       });
            addControl(QStringLiteral("agentUsagePageLogout_%1"),
                       QString::fromUtf8("Log out\xE2\x80\xA6"),
                       QStringLiteral("Sign this %1 account out from a system terminal")
                           .arg(providerName),
                       signedIn,
                       [this, provider, id = profile.id] {
                           launchAgentSystemTerminal(provider, QStringLiteral("logout"), id);
                       });
            addControl(QStringLiteral("agentUsagePageDelete_%1"),
                       QString::fromUtf8("Delete\xE2\x80\xA6"),
                       profile.builtIn
                           ? QStringLiteral("The default %1 account is the one in "
                                            "your home folder and cannot be deleted")
                                 .arg(providerName)
                           : QStringLiteral("Delete this %1 account from this desktop")
                                 .arg(providerName),
                       !profile.builtIn,
                       [this, provider, id = profile.id] {
                           confirmDeleteAgentAccount(provider, id);
                       });
            QToolButton *launch = addControl(
                QStringLiteral("agentUsagePageAccountTerminal_%1"), QString(),
                QStringLiteral("Launch %1 in a system terminal as this account")
                    .arg(providerName),
                true, [this, provider, id = profile.id] {
                    launchAgentSystemTerminal(provider, QStringLiteral("agent"), id);
                });
            launch->setFixedSize(22, 22);
            launch->setIcon(themedOcticon(QStringLiteral("terminal"), iconColor, 14));
            accountLayout->addLayout(row);

            for (const AgentUsageMenuData &usage :
                 agentAccountUsageMenuData(provider, profile.id)) {
                auto *meter = new TokenUsageMenuRow(
                    usage.label, usage.value,
                    forkmesh::usageclock::noteWithResetClock(
                        usage.resetNote, usage.resetAtMs, QDateTime::currentDateTime()),
                    usage.usedPercent, account, usage.resetAtMs, usage.windowMs);
                const QString usageKey = QStringLiteral("%1_%2").arg(
                    profile.id, usage.label.toLower().replace('-', '_'));
                meter->setObjectName(QStringLiteral("agentUsagePageMeter_%1").arg(usageKey));
                if (m_timersFeature) {
                    auto *timerButton = new QToolButton;
                    timerButton->setObjectName(
                        QStringLiteral("agentUsagePageTimer_%1").arg(usageKey));
                    timerButton->setAutoRaise(true);
                    timerButton->setCursor(Qt::PointingHandCursor);
                    timerButton->setIcon(
                        themedOcticon(QStringLiteral("stopwatch"), iconColor, 14));
                    timerButton->setToolTip(
                        QStringLiteral("Add a countdown to the top bar for when %1 resets")
                            .arg(usage.label));
                    timerButton->setAccessibleName(
                        QStringLiteral("Add %1 reset timer").arg(usage.label));
                    QString timerLabel = QStringLiteral("%1 %2").arg(key, usage.label);
                    if (profiles.size() > 1 && !profile.label.trimmed().isEmpty())
                        timerLabel += dot + profile.label.trimmed();
                    const QPointer<TokenUsageMenuRow> guardedMeter(meter);
                    connect(timerButton, &QToolButton::clicked, this,
                            [this, guardedMeter, timerLabel] {
                                if (guardedMeter)
                                    addUsageResetCountdown(timerLabel,
                                                           guardedMeter->resetAtMs());
                            });
                    meter->setActionButton(timerButton);
                }
                accountLayout->addWidget(meter);
            }
            sectionLayout->addWidget(account);
        }

        layout->addWidget(section);
        if (provider == providers.first()) {
            auto *rule = new QFrame(body);
            rule->setObjectName(QStringLiteral("agentUsagePageRule"));
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Plain);
            layout->addWidget(rule);
        }
    }
    layout->addStretch(1);
}
