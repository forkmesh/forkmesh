#include "RunnerListView.h"

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QWidget>

namespace forkmesh::runners {

namespace {

QColor stateColor(const RunnerProviderState &state)
{
    if (state.signedIn())
        return QColor(0x3f, 0xb9, 0x50);
    if (state.reported && state.binaryFound)
        return QColor(0xd2, 0x99, 0x22);
    return QColor(0x8b, 0x94, 0x9e);
}

} // namespace

QString runnerProviderCellText(const RunnerInfo &runner,
                               const QString &provider)
{
    const RunnerProviderState state = runner.provider(provider);
    QString text = runnerProviderStateText(state);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    if (const RunnerAccountInfo *active = state.activeAccount()) {
        text += dot + active->displayName();
        if (state.accounts.size() > 1)
            text += QStringLiteral(" (1 of %1)").arg(state.accounts.size());
        if (!active->usage.isEmpty()) {
            QStringList windows;
            for (const RunnerUsageWindow &window : active->usage)
                windows.append(QStringLiteral("%1 %2%")
                                   .arg(window.label)
                                   .arg(qRound(window.usedPercent)));
            text += QStringLiteral("\n") + windows.join(dot);
        }
    }
    if (!runner.openLoginId(provider).isEmpty())
        text += dot + QStringLiteral("sign-in open");
    return text;
}

QString runnerProviderToolTip(const RunnerInfo &runner,
                              const QString &provider, qint64 nowMs)
{
    const RunnerProviderState state = runner.provider(provider);
    QStringList lines;
    if (!state.source.isEmpty())
        lines.append(QStringLiteral("Credential source: %1").arg(state.source));
    for (const RunnerAccountInfo &account : state.accounts) {
        QString line = account.displayName();
        if (!account.plan.isEmpty())
            line += QStringLiteral(" [%1]").arg(account.plan);
        if (account.active)
            line += QStringLiteral(" (active)");
        if (runner.accountRequestPending(provider, account.id))
            line += QString::fromUtf8(" (change pending\xE2\x80\xA6)");
        line += QStringLiteral(": ") + runnerAccountUsageText(account, nowMs);
        lines.append(line);
    }
    for (const RunnerAccountRequestInfo &request : runner.accountRequests)
        if (request.provider == provider &&
            request.state == QLatin1String("failed") &&
            !request.message.isEmpty())
            lines.append(QStringLiteral("Last change failed: %1")
                             .arg(request.message));
    return lines.join(QLatin1Char('\n'));
}

QString runnerLastSeenText(qint64 lastSeenAtMs, qint64 nowMs)
{
    if (lastSeenAtMs <= 0)
        return QStringLiteral("never");
    const qint64 seconds = qMax<qint64>(0, (nowMs - lastSeenAtMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("%1 s ago").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    return QStringLiteral("%1 d ago").arg(seconds / 86400);
}

bool runnerSignInOffered(const RunnerInfo &runner, const QString &provider)
{
    if (runner.status != QLatin1String("active"))
        return false;
    const RunnerProviderState state = runner.provider(provider);
    return !state.reported || state.binaryFound;
}

QString runnerSignInButtonText(const RunnerInfo &runner,
                               const QString &provider)
{
    const QString label = runnerProviderLabel(provider);
    if (!runner.openLoginId(provider).isEmpty())
        return QStringLiteral("Open %1 sign-in").arg(label);
    if (!runner.provider(provider).accounts.isEmpty())
        return QStringLiteral("Add %1 account").arg(label);
    if (runner.provider(provider).signedIn())
        return QStringLiteral("Re-sign in %1").arg(label);
    return QStringLiteral("Sign in %1").arg(label);
}

QString runnerSignInButtonName(const QString &provider)
{
    return QStringLiteral("runnerSignInButton-") + provider;
}

QString runnerAccountsButtonName(const QString &provider)
{
    return QStringLiteral("runnerAccountsButton-") + provider;
}

void setUpRunnersTable(QTableWidget *table)
{
    table->setColumnCount(kRunnerColumnCount);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Runner"), QStringLiteral("Status"),
         QStringLiteral("Last seen"), QStringLiteral("Claude"),
         QStringLiteral("Codex"), QString()});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->horizontalHeader()->setSectionResizeMode(
        kRunnerStatusColumn, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(
        kRunnerClaudeColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        kRunnerCodexColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        kRunnerActionsColumn, QHeaderView::ResizeToContents);
}

void fillRunnersTable(QTableWidget *table, const QList<RunnerInfo> &runners,
                      qint64 nowMs, const RunnerSignInHandler &signIn,
                      const RunnerAccountHandler &accountAction)
{
    table->setRowCount(runners.size());
    const QStringList providers = runnerProviders();
    const int providerColumns[] = {kRunnerClaudeColumn, kRunnerCodexColumn};
    for (int row = 0; row < runners.size(); ++row) {
        const RunnerInfo &runner = runners.at(row);
        table->setItem(row, kRunnerNameColumn,
                       new QTableWidgetItem(runner.name));
        QString status = runner.status.isEmpty() ? QStringLiteral("unknown")
                                                 : runner.status;
        if (!runner.scope.isEmpty())
            status += QString::fromUtf8(" \xC2\xB7 ") + runner.scope;
        auto *statusItem = new QTableWidgetItem(status);
        if (!runner.registeredBy.isEmpty())
            statusItem->setToolTip(
                QStringLiteral("Registered by %1").arg(runner.registeredBy));
        table->setItem(row, kRunnerStatusColumn, statusItem);
        table->setItem(row, kRunnerLastSeenColumn,
                       new QTableWidgetItem(
                           runnerLastSeenText(runner.lastSeenAtMs, nowMs)));
        for (int i = 0; i < providers.size(); ++i) {
            const QString &provider = providers.at(i);
            auto *item = new QTableWidgetItem(
                runnerProviderCellText(runner, provider));
            item->setForeground(stateColor(runner.provider(provider)));
            item->setToolTip(runnerProviderToolTip(runner, provider, nowMs));
            table->setItem(row, providerColumns[i], item);
        }
        auto *actions = new QWidget;
        auto *layout = new QHBoxLayout(actions);
        layout->setContentsMargins(2, 0, 2, 0);
        layout->setSpacing(4);
        for (const QString &provider : providers) {
            if (!runnerSignInOffered(runner, provider))
                continue;
            auto *button =
                new QPushButton(runnerSignInButtonText(runner, provider));
            button->setObjectName(runnerSignInButtonName(provider));
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(
                QStringLiteral("Run the %1 CLI's own sign-in on %2 and relay "
                               "its terminal here. The credential stays on "
                               "the runner.")
                    .arg(runnerProviderLabel(provider), runner.name));
            const QString name = runner.name;
            QObject::connect(button, &QPushButton::clicked, button,
                             [signIn, name, provider] {
                                 if (signIn)
                                     signIn(name, provider);
                             });
            layout->addWidget(button);
            const QList<RunnerAccountInfo> accounts =
                runner.provider(provider).accounts;
            if (accounts.isEmpty() || !accountAction)
                continue;
            auto *accountsButton = new QPushButton(
                QStringLiteral("%1 accounts").arg(runnerProviderLabel(provider)));
            accountsButton->setObjectName(runnerAccountsButtonName(provider));
            accountsButton->setCursor(Qt::PointingHandCursor);
            accountsButton->setToolTip(
                QStringLiteral("Switch the account every %1 job on %2 uses, "
                               "or remove a saved account.")
                    .arg(runnerProviderLabel(provider), runner.name));
            auto *menu = new QMenu(accountsButton);
            for (const RunnerAccountInfo &account : accounts) {
                QAction *use = menu->addAction(
                    account.active
                        ? QStringLiteral("%1 (active)").arg(account.displayName())
                        : QStringLiteral("Use %1").arg(account.displayName()));
                use->setObjectName(QStringLiteral("runnerAccountUse-") +
                                   account.id);
                use->setEnabled(!account.active &&
                                !runner.accountRequestPending(provider,
                                                              account.id));
                use->setToolTip(runnerAccountUsageText(account, nowMs));
                const QString name = runner.name;
                QObject::connect(use, &QAction::triggered, accountsButton,
                                 [accountAction, name, provider, account] {
                                     accountAction(name, provider, account,
                                                   QStringLiteral("activate"));
                                 });
            }
            menu->addSeparator();
            for (const RunnerAccountInfo &account : accounts) {
                QAction *remove = menu->addAction(
                    QStringLiteral("Remove %1").arg(account.displayName()));
                remove->setObjectName(QStringLiteral("runnerAccountRemove-") +
                                      account.id);
                remove->setEnabled(
                    !runner.accountRequestPending(provider, account.id));
                const QString name = runner.name;
                QObject::connect(remove, &QAction::triggered, accountsButton,
                                 [accountAction, name, provider, account] {
                                     accountAction(name, provider, account,
                                                   QStringLiteral("remove"));
                                 });
            }
            accountsButton->setMenu(menu);
            accountsButton->setEnabled(runner.status ==
                                       QLatin1String("active"));
            layout->addWidget(accountsButton);
        }
        layout->addStretch(1);
        table->setCellWidget(row, kRunnerActionsColumn, actions);
    }
    table->resizeRowsToContents();
}

void fillRunnerAccountsMenu(QMenu *menu, const QList<RunnerInfo> &runners,
                            const QString &provider, qint64 nowMs,
                            const QString &emptyText,
                            const RunnerSignInHandler &signIn,
                            const RunnerAccountHandler &accountAction)
{
    if (!menu)
        return;
    menu->clear();
    const QString providerLabel = runnerProviderLabel(provider);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    bool listed = false;
    for (const RunnerInfo &runner : runners) {
        if (runner.status != QLatin1String("active"))
            continue;
        const RunnerProviderState state = runner.provider(provider);
        if (!state.binaryFound && state.accounts.isEmpty())
            continue;
        if (listed)
            menu->addSeparator();
        listed = true;
        QAction *heading = menu->addAction(runner.name);
        heading->setObjectName(QStringLiteral("runnerAccountsHeading-") +
                               runner.name);
        heading->setEnabled(false);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
        const QString name = runner.name;
        for (const RunnerAccountInfo &account : state.accounts) {
            const bool pending =
                runner.accountRequestPending(provider, account.id);
            QString text = account.displayName();
            if (!account.plan.isEmpty())
                text += dot + account.plan;
            if (pending)
                text += dot + QString::fromUtf8("switching\xE2\x80\xA6");
            QAction *use = menu->addAction(text);
            use->setObjectName(
                QStringLiteral("runnerAccount-%1-%2").arg(name, account.id));
            use->setCheckable(true);
            use->setChecked(account.active);
            use->setEnabled(!pending);
            use->setToolTip(
                (account.active
                     ? QStringLiteral("Every %1 job on %2 runs as this account")
                     : QStringLiteral("Use this account for every %1 job on %2"))
                    .arg(providerLabel, name));
            QObject::connect(use, &QAction::triggered, menu,
                             [accountAction, name, provider, account] {
                                 if (!account.active && accountAction)
                                     accountAction(name, provider, account,
                                                   QStringLiteral("activate"));
                             });
            QAction *usage =
                menu->addAction(QStringLiteral("    ") +
                                runnerAccountUsageText(account, nowMs));
            usage->setObjectName(
                QStringLiteral("runnerAccountUsage-%1-%2").arg(name, account.id));
            usage->setEnabled(false);
        }
        const bool add = !state.accounts.isEmpty();
        QAction *signInAction = menu->addAction(
            (add ? QString::fromUtf8("Add %1 account on %2\xE2\x80\xA6")
                 : QString::fromUtf8("Sign in %1 on %2\xE2\x80\xA6"))
                .arg(providerLabel, name));
        signInAction->setObjectName(QStringLiteral("runnerAccountAdd-") + name);
        signInAction->setEnabled(state.binaryFound);
        QObject::connect(signInAction, &QAction::triggered, menu,
                         [signIn, name, provider] {
                             if (signIn)
                                 signIn(name, provider);
                         });
    }
    if (!listed) {
        QAction *empty = menu->addAction(emptyText);
        empty->setObjectName(QStringLiteral("runnerAccountsEmpty"));
        empty->setEnabled(false);
    }
}

} // namespace forkmesh::runners
