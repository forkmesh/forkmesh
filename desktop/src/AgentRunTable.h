#pragma once

#include "AgentRunEfficiencyLog.h"

#include <QWidget>

class QLabel;
class QTableWidget;

class AgentRunTable : public QWidget
{
public:
    enum ComparisonColumn {
        RuntimeColumn,
        ToolsColumn,
        RunsColumn,
        FailedColumn,
        TimeColumn,
        ModelCallsColumn,
        ContextColumn,
        ContextVsOnColumn,
        OutputColumn,
        ToolCallsColumn,
        ToolErrorsColumn,
        ToolTokensColumn,
        StakeColumn,
        EfficiencyColumn,
        CostColumn,
        ComparisonColumnCount
    };
    enum RunColumn {
        EndedColumn,
        AgentColumn,
        RunRuntimeColumn,
        RunToolsColumn,
        OutcomeColumn,
        RunTimeColumn,
        RunModelCallsColumn,
        RunContextColumn,
        RunOutputColumn,
        RunToolCallsColumn,
        RunErrorsColumn,
        RunToolTokensColumn,
        RunStakeColumn,
        RunEfficiencyColumn,
        RunCostColumn,
        RunColumnCount
    };
    static constexpr int kMaxRunRows = 500;

    explicit AgentRunTable(QWidget *parent = nullptr);

    void refresh(const AgentRunEfficiencyLog *log);

    QTableWidget *comparisonTable() const { return m_comparison; }
    QTableWidget *runTable() const { return m_runs; }

private:
    QLabel *m_summary = nullptr;
    QTableWidget *m_comparison = nullptr;
    QTableWidget *m_runs = nullptr;
};
