#pragma once

#include "AgentModelStats.h"

#include <QIcon>
#include <QString>
#include <QTableWidget>
#include <QVector>

#include <functional>

class AgentModelTable : public QTableWidget
{
public:
    enum Column {
        ModelColumn,
        AgentColumn,
        RunsColumn,
        LiveColumn,
        SucceededColumn,
        FailedColumn,
        StoppedColumn,
        SuccessRateColumn,
        FailureRateColumn,
        MergedColumn,
        MergeRateColumn,
        GitMergesColumn,
        CommitsColumn,
        PullRequestsColumn,
        CostColumn,
        CostPerRunColumn,
        CostPerMergeColumn,
        InputTokensColumn,
        OutputTokensColumn,
        TurnsPerRunColumn,
        AverageTimeColumn,
        ToolCallsColumn,
        ToolCallsPerRunColumn,
        ToolErrorRateColumn,
        TokensPerToolCallColumn,
        LastUsedColumn,
        ColumnCount
    };

    explicit AgentModelTable(QWidget *parent = nullptr);

    std::function<QIcon(const AgentModelStatsRow &)> iconFor;

    void refresh(const QVector<AgentModelStatsRow> &rows);
    void setHideUnused(bool hide);
    int visibleRowCount() const;
    QString cellText(const QString &label, int column) const;

private:
    void applyFilter();
    void fitColumns();

    bool m_hideUnused = false;
};
