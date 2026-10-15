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
        ShowColumn,
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
    std::function<QString(const AgentModelStatsRow &)> composerKeyFor;
    std::function<bool(const QString &)> composerShows;
    std::function<void(const QString &, bool)> composerShowChanged;

    void refresh(const QVector<AgentModelStatsRow> &rows);
    void setHideUnused(bool hide);
    int visibleRowCount() const;
    QString cellText(const QString &label, int column) const;

private:
    void applyFilter();
    void fitColumns();

    bool m_hideUnused = false;
    bool m_filling = false;
};
