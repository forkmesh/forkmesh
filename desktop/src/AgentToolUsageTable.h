#pragma once

#include "AgentToolEfficiencyLedger.h"

#include <QString>
#include <QTableWidget>

// Every runtime tool and shell command shape the ledger has seen, one row
// each, with the numbers that tell an efficient tool from a wasteful one:
// how often it runs, how often it fails, how long it takes and how much
// context it costs per call. Click a header to sort by that column. Plain
// QTableWidget so it needs no moc registration.
class AgentToolUsageTable : public QTableWidget
{
public:
    enum Column {
        ToolColumn,
        CommandColumn,
        CallsColumn,
        ErrorsColumn,
        ErrorRateColumn,
        AverageTimeColumn,
        SlowestColumn,
        TokensPerCallColumn,
        TokensColumn,
        SavedColumn,
        CostColumn,
        LastUsedColumn,
        ColumnCount
    };

    explicit AgentToolUsageTable(QWidget *parent = nullptr);

    // Rebuilds the rows from the ledger, keeping the sort and the filter.
    void refresh(const AgentToolEfficiencyLedger *ledger);
    // Shows only rows whose tool, command or family contains `text`.
    void setFilter(const QString &text);

    // "4.2 s", "380 ms", "—" for no samples.
    static QString durationText(double ms);

private:
    void applyFilter();

    QString m_filter;
};
