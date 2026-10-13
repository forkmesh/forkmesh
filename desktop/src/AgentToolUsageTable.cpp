#include "AgentToolUsageTable.h"

#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyBoard.h"

#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QLocale>

namespace {

// Sorts by the raw number in Qt::UserRole, not by the formatted text, so
// "1.2k" lands above "900" and "—" below every measured value.
class NumberItem : public QTableWidgetItem
{
public:
    NumberItem(const QString &text, double value) : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole).toDouble() < other.data(Qt::UserRole).toDouble();
    }
};

QString lastUsedText(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    return at.date() == QDate::currentDate()
               ? at.toString(QStringLiteral("HH:mm"))
               : QLocale().toString(at.date(), QLocale::ShortFormat);
}

} // namespace

AgentToolUsageTable::AgentToolUsageTable(QWidget *parent) : QTableWidget(parent)
{
    setObjectName(QStringLiteral("agentToolUsageTable"));
    setColumnCount(ColumnCount);
    setHorizontalHeaderLabels(
        {QStringLiteral("Tool"), QStringLiteral("Command"), QStringLiteral("Calls"),
         QStringLiteral("Errors"), QStringLiteral("Error rate"),
         QStringLiteral("Avg time"), QStringLiteral("Slowest"),
         QStringLiteral("Tokens / call"), QStringLiteral("Tokens"),
         QStringLiteral("Saved"), QStringLiteral("Cost"), QStringLiteral("Last used")});
    const QStringList tips = {
        QStringLiteral("The runtime tool that made the call"),
        QStringLiteral("For shell calls, the command the line is about, "
                       "ignoring setup such as cd and timeout"),
        QStringLiteral("Calls recorded on this device"),
        QStringLiteral("Calls whose result was an error"),
        QStringLiteral("Errors as a share of calls"),
        QStringLiteral("Mean time from call to result"),
        QStringLiteral("Longest time from call to result"),
        QStringLiteral("Context each call costs on average: arguments plus result, "
                       "estimated at four bytes per token"),
        QStringLiteral("Context all calls cost together"),
        QStringLiteral("Tokens saved against the naive alternative, where it can "
                       "be measured"),
        QStringLiteral("Estimated cost of that context at the model's list price"),
        QStringLiteral("When the tool last ran")};
    for (int column = 0; column < ColumnCount; ++column)
        horizontalHeaderItem(column)->setToolTip(tips.value(column));
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setFrameShape(QFrame::NoFrame);
    verticalHeader()->hide();
    horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(CommandColumn, QHeaderView::Stretch);
    horizontalHeader()->setSortIndicatorShown(true);
    horizontalHeader()->setSortIndicator(CallsColumn, Qt::DescendingOrder);
    setSortingEnabled(true);
}

QString AgentToolUsageTable::durationText(double ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(qRound(ms));
    if (ms < 60'000)
        return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 min").arg(ms / 60'000.0, 0, 'f', 1);
}

void AgentToolUsageTable::refresh(const AgentToolEfficiencyLedger *ledger)
{
    const int sortColumn = horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = horizontalHeader()->sortIndicatorOrder();
    QString selectedKey;
    if (const QTableWidgetItem *item = currentItem())
        selectedKey = this->item(item->row(), ToolColumn)->data(Qt::UserRole).toString();

    setSortingEnabled(false);
    const QVector<AgentToolUsageStats> rows =
        ledger ? ledger->usage() : QVector<AgentToolUsageStats>();
    setRowCount(rows.size());
    const QColor danger(QStringLiteral("#cf222e"));
    for (int r = 0; r < rows.size(); ++r) {
        const AgentToolUsageStats &u = rows.at(r);
        auto *tool = new QTableWidgetItem(u.tool);
        tool->setData(Qt::UserRole, u.key);
        const AgentToolFamily *family = agentToolFamily(u.family);
        tool->setToolTip(family ? QStringLiteral("%1 · %2").arg(u.tool, family->displayName)
                                : u.tool);
        setItem(r, ToolColumn, tool);
        auto *command = new QTableWidgetItem(u.command.isEmpty() ? QStringLiteral("—")
                                                                 : u.command);
        command->setData(Qt::UserRole + 1, family ? family->displayName : QString());
        setItem(r, CommandColumn, command);
        setItem(r, CallsColumn,
                new NumberItem(AgentToolEfficiencyBoard::compactNumber(u.calls), u.calls));
        setItem(r, ErrorsColumn,
                new NumberItem(AgentToolEfficiencyBoard::compactNumber(u.errors), u.errors));
        auto *rate = new NumberItem(
            QStringLiteral("%1%").arg(u.errorRate() * 100.0, 0, 'f', 0), u.errorRate());
        // A tool that fails one call in four is costing more than it saves.
        if (u.calls >= 4 && u.errorRate() >= 0.25)
            rate->setForeground(danger);
        setItem(r, ErrorRateColumn, rate);
        setItem(r, AverageTimeColumn,
                new NumberItem(durationText(u.averageLatencyMs()), u.averageLatencyMs()));
        setItem(r, SlowestColumn,
                new NumberItem(durationText(double(u.latencyMsMax)),
                               double(u.latencyMsMax)));
        setItem(r, TokensPerCallColumn,
                new NumberItem(AgentToolEfficiencyBoard::compactNumber(
                                   qRound64(u.tokensPerCall())),
                               u.tokensPerCall()));
        setItem(r, TokensColumn,
                new NumberItem(AgentToolEfficiencyBoard::compactNumber(u.consumedTokens()),
                               double(u.consumedTokens())));
        setItem(r, SavedColumn,
                new NumberItem(u.savedTokens > 0
                                   ? AgentToolEfficiencyBoard::compactNumber(u.savedTokens)
                                   : QStringLiteral("—"),
                               double(u.savedTokens)));
        setItem(r, CostColumn,
                new NumberItem(AgentToolEfficiencyBoard::compactUsd(u.consumedUsd),
                               u.consumedUsd));
        setItem(r, LastUsedColumn,
                new NumberItem(lastUsedText(u.lastCallMs), double(u.lastCallMs)));
    }
    setSortingEnabled(true);
    sortItems(sortColumn, sortOrder);
    applyFilter();
    if (!selectedKey.isEmpty()) {
        for (int r = 0; r < rowCount(); ++r)
            if (item(r, ToolColumn)->data(Qt::UserRole).toString() == selectedKey) {
                setCurrentCell(r, ToolColumn);
                break;
            }
    }
}

void AgentToolUsageTable::setFilter(const QString &text)
{
    m_filter = text.trimmed();
    applyFilter();
}

void AgentToolUsageTable::applyFilter()
{
    for (int r = 0; r < rowCount(); ++r) {
        const QTableWidgetItem *tool = item(r, ToolColumn);
        const QTableWidgetItem *command = item(r, CommandColumn);
        const bool match =
            m_filter.isEmpty() ||
            (tool && tool->text().contains(m_filter, Qt::CaseInsensitive)) ||
            (command && (command->text().contains(m_filter, Qt::CaseInsensitive) ||
                         command->data(Qt::UserRole + 1)
                             .toString()
                             .contains(m_filter, Qt::CaseInsensitive)));
        setRowHidden(r, !match);
    }
}
