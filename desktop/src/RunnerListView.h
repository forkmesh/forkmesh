#pragma once

#include "RunnerLogin.h"

#include <QList>
#include <QString>

#include <functional>

class QMenu;
class QTableWidget;

namespace forkmesh::runners {

enum RunnerTableColumn {
    kRunnerNameColumn = 0,
    kRunnerStatusColumn,
    kRunnerLastSeenColumn,
    kRunnerClaudeColumn,
    kRunnerCodexColumn,
    kRunnerActionsColumn,
    kRunnerColumnCount
};

using RunnerSignInHandler =
    std::function<void(const QString &runner, const QString &provider)>;
using RunnerAccountHandler =
    std::function<void(const QString &runner, const QString &provider,
                       const RunnerAccountInfo &account,
                       const QString &action)>;

QString runnerLastSeenText(qint64 lastSeenAtMs, qint64 nowMs);
QString runnerSignInButtonText(const RunnerInfo &runner,
                               const QString &provider);
bool runnerSignInOffered(const RunnerInfo &runner, const QString &provider);
QString runnerSignInButtonName(const QString &provider);
QString runnerAccountsButtonName(const QString &provider);
QString runnerProviderCellText(const RunnerInfo &runner,
                               const QString &provider);
QString runnerProviderToolTip(const RunnerInfo &runner,
                              const QString &provider, qint64 nowMs);
void setUpRunnersTable(QTableWidget *table);
void fillRunnersTable(QTableWidget *table, const QList<RunnerInfo> &runners,
                      qint64 nowMs, const RunnerSignInHandler &signIn,
                      const RunnerAccountHandler &accountAction = {});
void fillRunnerAccountsMenu(QMenu *menu, const QList<RunnerInfo> &runners,
                            const QString &provider, qint64 nowMs,
                            const QString &emptyText,
                            const RunnerSignInHandler &signIn,
                            const RunnerAccountHandler &accountAction);

} // namespace forkmesh::runners
