#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentUsageResetClock.h"

#include <QAction>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
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

class AgentUsageHoverCard final : public QFrame
{
public:
    explicit AgentUsageHoverCard(QWidget *parent)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("agentUsageHoverCard"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(6);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        m_watch.setInterval(120);
        QObject::connect(&m_watch, &QTimer::timeout, this, [this] { hideIfLeft(); });
    }

    QPointer<QWidget> anchor;
    QString provider;
    QString signature;

    void place()
    {
        if (!anchor)
            return;
        const QRect zone(anchor->mapToGlobal(QPoint()), anchor->size());
        QScreen *screen = QGuiApplication::screenAt(zone.center());
        if (!screen)
            screen = anchor->screen();
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        const QSize size = sizeHint();
        const bool below = zone.center().y() < area.center().y();
        int y = below ? zone.bottom() + 6 : zone.top() - size.height() - 6;
        if (y < area.top() || y + size.height() > area.bottom() + 1)
            y = below ? zone.top() - size.height() - 6 : zone.bottom() + 6;
        y = qBound(area.top(), y, qMax(area.top(), area.bottom() - size.height() + 1));
        const int x = qBound(area.left(), zone.left(),
                             qMax(area.left(), area.right() - size.width() + 1));
        setGeometry(QRect(QPoint(x, y), size));
    }

    void present()
    {
        place();
        m_misses = 0;
        show();
        raise();
        m_watch.start();
    }

    void dismiss()
    {
        m_watch.stop();
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor background = palette().color(QPalette::Window);
        background.setAlpha(255);
        QColor edge = palette().color(QPalette::WindowText);
        edge.setAlpha(60);
        painter.fillRect(rect(), background);
        painter.setPen(edge);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    void hideIfLeft()
    {
        if (!anchor || !anchor->isVisible() || !anchor->window()->isVisible()) {
            dismiss();
            return;
        }
        const QPoint pointer = QCursor::pos();
        const QRect zone(anchor->mapToGlobal(QPoint()), anchor->size());
        if (zone.adjusted(-4, -4, 4, 4).contains(pointer) ||
            geometry().adjusted(-8, -8, 8, 8).contains(pointer)) {
            m_misses = 0;
            return;
        }
        if (++m_misses >= 3)
            dismiss();
    }

    QTimer m_watch;
    int m_misses = 0;
};

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
    const bool hoverCardShown =
        m_agentUsageHoverCard && m_agentUsageHoverCard->isVisible();
    if (m_agentUsagePages.isEmpty() && !hoverCardShown)
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
            if (m_agentUsageHoverCard && m_agentUsageHoverCard->isVisible())
                fillAgentUsageHoverCard();
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

void MainWindow::attachAgentUsageHoverCard(QWidget *anchor,
                                           std::function<QString()> provider)
{
    if (!anchor)
        return;
    auto *delay = new QTimer(anchor);
    delay->setSingleShot(true);
    delay->setInterval(350);
    const QPointer<QWidget> guarded(anchor);
    connect(delay, &QTimer::timeout, this, [this, guarded, provider] {
        if (guarded && guarded->underMouse())
            showAgentUsageHoverCard(guarded, provider());
    });
    anchor->installEventFilter(new RailEventHook(
        [this, delay](QObject *, QEvent *event) {
            switch (event->type()) {
            case QEvent::Enter:
                delay->start();
                break;
            case QEvent::Leave:
                delay->stop();
                break;
            case QEvent::ToolTip:
                return true;
            case QEvent::MouseButtonPress:
            case QEvent::Hide:
                delay->stop();
                hideAgentUsageHoverCard();
                break;
            default:
                break;
            }
            return false;
        },
        anchor));
}

void MainWindow::showAgentUsageHoverCard(QWidget *anchor, const QString &provider)
{
    if (!anchor)
        return;
    auto *card = static_cast<AgentUsageHoverCard *>(m_agentUsageHoverCard.data());
    if (!card) {
        card = new AgentUsageHoverCard(this);
        m_agentUsageHoverCard = card;
    }
    if (card->anchor != anchor || card->provider != provider)
        card->signature.clear();
    card->anchor = anchor;
    card->provider = provider;
    fillAgentUsageHoverCard();
    card->present();
}

void MainWindow::hideAgentUsageHoverCard()
{
    if (auto *card = static_cast<AgentUsageHoverCard *>(m_agentUsageHoverCard.data()))
        card->dismiss();
}

void MainWindow::fillAgentUsageHoverCard()
{
    auto *card = static_cast<AgentUsageHoverCard *>(m_agentUsageHoverCard.data());
    if (!card)
        return;
    const QString provider = card->provider;
    const bool codex = agentIsCodexProvider(provider);
    const QString providerName =
        codex ? QStringLiteral("Codex") : QStringLiteral("Claude Code");
    const QString key = codex ? QStringLiteral("Codex") : QStringLiteral("Claude");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const bool isDefault =
        agentIsCodexProvider(defaultAgentProvider()) == codex;
    const AgentAccountProfile active = activeAgentAccount(provider);
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    const QList<AgentUsageMenuData> usage =
        agentAccountUsageMenuData(provider, active.id);
    const auto *chart = static_cast<const TokenUsageMiniChart *>(
        codex ? m_navCodexUsage : m_navTokenUsage);
    const QString stats = chart ? chart->stats() : QString();
    const QString identity = agentAccountIdentity(provider, active.configDir);
    const bool signedIn = agentAccountSignedIn(provider, active.configDir);

    QString slot = key;
    QStringList signature{provider, active.id, QString::number(isDefault),
                          identity, QString::number(signedIn), stats,
                          QString::number(m_timersFeature != nullptr)};
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles.at(i).id == active.id)
            slot = QStringLiteral("%1 %2").arg(key).arg(i + 1);
        signature << profiles.at(i).id << profiles.at(i).label;
    }
    for (const AgentUsageMenuData &row : usage)
        signature << row.detail << QString::number(row.usedPercent)
                  << QString::number(row.resetAtMs);
    const QString joined = signature.join(QLatin1Char('\n'));
    if (card->signature == joined) {
        card->place();
        return;
    }
    card->signature = joined;

    auto *layout = static_cast<QVBoxLayout *>(card->layout());
    clearLayout(layout);
    const QColor iconColor = card->palette().color(QPalette::PlaceholderText);
    const QPointer<QWidget> guardedCard(card);
    const auto hideFirst = [this](std::function<void()> run) {
        return [this, run = std::move(run)] {
            hideAgentUsageHoverCard();
            run();
        };
    };
    const auto addTool = [&](QBoxLayout *row, const QString &objectName,
                             const QString &text, const QString &toolTip,
                             const QString &icon, bool enabled,
                             std::function<void()> run) {
        auto *button = new QToolButton(card);
        button->setObjectName(objectName);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setText(text);
        button->setToolTip(toolTip);
        button->setAccessibleName(toolTip);
        button->setEnabled(enabled);
        if (!icon.isEmpty()) {
            button->setIcon(themedOcticon(icon, iconColor, 14));
            button->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly
                                                      : Qt::ToolButtonTextBesideIcon);
        }
        connect(button, &QToolButton::clicked, this, std::move(run));
        row->addWidget(button);
        return button;
    };

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    QString title = providerName + dot + slot;
    if (!active.label.trimmed().isEmpty())
        title += dot + active.label.trimmed();
    if (!identity.isEmpty())
        title += QStringLiteral(" — ") + identity;
    QLabel *heading = pageLabel(title, "agentUsageHoverTitle", card);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    header->addWidget(heading);
    header->addStretch(1);
    addTool(header, QStringLiteral("agentUsageHoverRefresh"), QString(),
            QStringLiteral("Read this account's usage again"),
            QStringLiteral("sync"), true, [this, codex] {
                if (codex)
                    refreshCodexUsageRemaining();
                else
                    refreshClaudeCodeUsage();
            });
    addTool(header, QStringLiteral("agentUsageHoverOpenPage"), QString(),
            QStringLiteral("Open the Usage page with every account"),
            QStringLiteral("graph"), true,
            hideFirst([this] { openAgentUsagePage(); }));
    layout->addLayout(header);

    QStringList status;
    status << (isDefault ? QStringLiteral("Runs new agents")
                         : QStringLiteral("Runs new %1 agents").arg(providerName));
    if (!signedIn)
        status << QStringLiteral("not signed in");
    layout->addWidget(pageLabel(status.join(dot), "statusLine", card));

    for (const AgentUsageMenuData &row : usage) {
        auto *meter = new TokenUsageMenuRow(
            row.label, row.value,
            forkmesh::usageclock::noteWithResetClock(row.resetNote, row.resetAtMs,
                                                     QDateTime::currentDateTime()),
            row.usedPercent, card, row.resetAtMs, row.windowMs);
        const QString usageKey = row.label.toLower().replace('-', '_').replace(' ', '_');
        meter->setObjectName(QStringLiteral("agentUsageHoverMeter_%1").arg(usageKey));
        if (m_timersFeature) {
            auto *timerButton = new QToolButton;
            timerButton->setObjectName(
                QStringLiteral("agentUsageHoverTimer_%1").arg(usageKey));
            timerButton->setAutoRaise(true);
            timerButton->setCursor(Qt::PointingHandCursor);
            timerButton->setFocusPolicy(Qt::NoFocus);
            timerButton->setIcon(themedOcticon(QStringLiteral("stopwatch"), iconColor, 14));
            timerButton->setToolTip(
                QStringLiteral("Add a countdown to the top bar for when %1 resets")
                    .arg(row.label));
            timerButton->setAccessibleName(
                QStringLiteral("Add %1 reset timer").arg(row.label));
            QString timerLabel = QStringLiteral("%1 %2").arg(key, row.label);
            if (profiles.size() > 1 && !active.label.trimmed().isEmpty())
                timerLabel += dot + active.label.trimmed();
            const QPointer<TokenUsageMenuRow> guardedMeter(meter);
            connect(timerButton, &QToolButton::clicked, this,
                    [this, guardedMeter, timerLabel] {
                        if (guardedMeter &&
                            addUsageResetCountdown(timerLabel, guardedMeter->resetAtMs()))
                            hideAgentUsageHoverCard();
                    });
            meter->setActionButton(timerButton);
        }
        layout->addWidget(meter);
    }

    if (!stats.isEmpty()) {
        QLabel *statsLabel = pageLabel(stats, "statusLine", card);
        statsLabel->setObjectName(QStringLiteral("agentUsageHoverStats"));
        statsLabel->setWordWrap(true);
        layout->addWidget(statsLabel);
    }

    auto *rule = new QFrame(card);
    rule->setObjectName(QStringLiteral("agentUsageHoverRule"));
    rule->setFixedHeight(1);
    const QColor ruleColor = card->palette().color(QPalette::WindowText);
    rule->setStyleSheet(QStringLiteral("QFrame{border:none;background:rgba(%1,%2,%3,40);}")
                            .arg(ruleColor.red())
                            .arg(ruleColor.green())
                            .arg(ruleColor.blue()));
    layout->addWidget(rule);

    auto *terminals = new QHBoxLayout;
    terminals->setSpacing(2);
    addTool(terminals, QStringLiteral("agentUsageHoverTerminal"),
            QStringLiteral("Open %1 in a terminal").arg(slot),
            QStringLiteral("Start a new %1 agent in a terminal inside ForkMesh")
                .arg(providerName),
            QStringLiteral("terminal"), true, hideFirst([this, codex] {
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
            }));
    addTool(terminals, QStringLiteral("agentUsageHoverSystemTerminal"),
            QStringLiteral("Launch in system terminal"),
            QStringLiteral("Launch %1 in a system terminal as this account")
                .arg(providerName),
            QString(), true, hideFirst([this, provider, id = active.id] {
                launchAgentSystemTerminal(provider, QStringLiteral("agent"), id);
            }));
    terminals->addStretch(1);
    layout->addLayout(terminals);

    auto *account = new QHBoxLayout;
    account->setSpacing(2);
    addTool(account, QStringLiteral("agentUsageHoverEdit"),
            QString::fromUtf8("Edit account\xE2\x80\xA6"),
            QStringLiteral("Change this account's label"), QString(), true,
            hideFirst([this, provider, id = active.id] { editAgentAccount(provider, id); }));
    addTool(account, QStringLiteral("agentUsageHoverLogin"),
            QString::fromUtf8("Log in\xE2\x80\xA6"),
            QStringLiteral("Sign this %1 account in from a system terminal")
                .arg(providerName),
            QString(), true, hideFirst([this, provider, id = active.id] {
                launchAgentSystemTerminal(provider, QStringLiteral("login"), id);
            }));
    addTool(account, QStringLiteral("agentUsageHoverLogout"),
            QString::fromUtf8("Log out\xE2\x80\xA6"),
            QStringLiteral("Sign this %1 account out from a system terminal")
                .arg(providerName),
            QString(), signedIn, hideFirst([this, provider, id = active.id] {
                launchAgentSystemTerminal(provider, QStringLiteral("logout"), id);
            }));
    addTool(account, QStringLiteral("agentUsageHoverAddAccount"),
            QString::fromUtf8("Add account\xE2\x80\xA6"),
            QStringLiteral("Add another %1 account to this desktop").arg(providerName),
            QStringLiteral("person"), true,
            hideFirst([this, provider] { addAgentAccount(provider); }));
    account->addStretch(1);
    layout->addLayout(account);

    if (profiles.size() > 1) {
        auto *switcher = new QHBoxLayout;
        switcher->setSpacing(2);
        switcher->addWidget(pageLabel(QStringLiteral("Switch to"), "statusLine", card));
        for (int i = 0; i < profiles.size(); ++i) {
            const AgentAccountProfile &profile = profiles.at(i);
            if (profile.id == active.id)
                continue;
            QString label = QStringLiteral("%1 %2").arg(key).arg(i + 1);
            if (!profile.label.trimmed().isEmpty())
                label += dot + profile.label.trimmed();
            addTool(switcher, QStringLiteral("agentUsageHoverUse_%1").arg(profile.id),
                    label,
                    QStringLiteral("Run new %1 agents as this account").arg(providerName),
                    QString(), true, [this, provider, id = profile.id] {
                        selectAgentAccount(provider, id);
                        scheduleAgentUsagePagesRefresh();
                    });
        }
        switcher->addStretch(1);
        layout->addLayout(switcher);
    }

    auto *more = new QHBoxLayout;
    more->setSpacing(2);
    QToolButton *runners = addTool(
        more, QStringLiteral("agentUsageHoverRunnerAccounts"),
        QString::fromUtf8("Runner accounts\xE2\x80\xA6"),
        QStringLiteral("Choose which %1 account each runner uses for cloud agents")
            .arg(providerName),
        QString(), true, [] {});
    const QPointer<QToolButton> guardedRunners(runners);
    connect(runners, &QToolButton::clicked, this, [this, provider, guardedRunners] {
        const QPoint at = guardedRunners
                              ? guardedRunners->mapToGlobal(
                                    QPoint(0, guardedRunners->height()))
                              : QCursor::pos();
        hideAgentUsageHoverCard();
        auto *menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("agentUsageHoverRunnerMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        fillAgentRunnerAccountsMenu(menu, provider);
        refreshRunners();
        menu->popup(at);
    });
    if (!isDefault) {
        addTool(more, QStringLiteral("agentUsageHoverMakeDefault"),
                QStringLiteral("Use %1 for new agents").arg(providerName),
                QStringLiteral("Make %1 the default agent provider").arg(providerName),
                QString(), true, [this, provider] {
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
    }
    more->addStretch(1);
    layout->addLayout(more);

    if (guardedCard) {
        card->adjustSize();
        card->place();
    }
}
