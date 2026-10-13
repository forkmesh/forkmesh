#include "AgentRunTable.h"

#include "AgentToolEfficiencyBoard.h"
#include "AgentToolUsageTable.h"

#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

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

QString tokens(double value)
{
    return value > 0 ? AgentToolEfficiencyBoard::compactNumber(qRound64(value))
                     : QStringLiteral("—");
}

QString percent(double ratio)
{
    return QStringLiteral("%1%").arg(ratio * 100.0, 0, 'f', 0);
}

QString endedText(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    return at.date() == QDate::currentDate()
               ? at.toString(QStringLiteral("HH:mm"))
               : QLocale().toString(at, QLocale::ShortFormat);
}

QTableWidget *makeTable(const QString &objectName, const QStringList &labels,
                        const QStringList &tips)
{
    auto *table = new QTableWidget;
    table->setObjectName(objectName);
    table->setColumnCount(int(labels.size()));
    table->setHorizontalHeaderLabels(labels);
    for (int column = 0; column < labels.size(); ++column)
        table->horizontalHeaderItem(column)->setToolTip(tips.value(column));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSortIndicatorShown(true);
    return table;
}

QColor efficiencyColor(double efficiency)
{
    if (efficiency < 0.6)
        return QColor(QStringLiteral("#cf222e"));
    if (efficiency < 0.85)
        return QColor(QStringLiteral("#9a6700"));
    return QColor(QStringLiteral("#1a7f37"));
}

} // namespace

AgentRunTable::AgentRunTable(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("agentRunTable"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("agentRunSummary"));
    m_summary->setWordWrap(true);
    m_comparison = makeTable(
        QStringLiteral("agentRunComparisonTable"),
        {QStringLiteral("Runtime"), QStringLiteral("Tools"), QStringLiteral("Runs"),
         QStringLiteral("Failed"), QStringLiteral("Avg time"),
         QStringLiteral("Model calls / run"), QStringLiteral("Context / run"),
         QStringLiteral("vs all on"), QStringLiteral("Output / run"),
         QStringLiteral("Tool calls / run"), QStringLiteral("Tool errors"),
         QStringLiteral("Tool tokens / run"), QStringLiteral("At stake / run"),
         QStringLiteral("Efficiency"), QStringLiteral("Est. cost / run")},
        {QStringLiteral("The agent runtime"),
         QStringLiteral("Tool families switched off when the runs launched"),
         QStringLiteral("Finished runs (one prompt to its result)"),
         QStringLiteral("Runs that ended in an error, were interrupted or never reported a result"),
         QStringLiteral("Mean wall time of a run"),
         QStringLiteral("Model responses per run"),
         QStringLiteral("Input context the model read per run, cached input included"),
         QStringLiteral("Context per run against the same runtime with every tool on"),
         QStringLiteral("Tokens the model wrote per run"),
         QStringLiteral("Tool calls per run"),
         QStringLiteral("Share of tool calls that returned an error"),
         QStringLiteral("Tool arguments plus results per run, at four bytes per token"),
         QStringLiteral("Tokens per run the improvement log flagged: oversized results, "
                        "whole-file reads, failed and repeated calls"),
         QStringLiteral("Share of tool tokens not flagged by the improvement log"),
         QStringLiteral("Estimated at the model's list price")});
    m_comparison->setSortingEnabled(false);
    m_runs = makeTable(
        QStringLiteral("agentRunListTable"),
        {QStringLiteral("Ended"), QStringLiteral("Agent"), QStringLiteral("Runtime"),
         QStringLiteral("Tools"), QStringLiteral("Outcome"), QStringLiteral("Time"),
         QStringLiteral("Model calls"), QStringLiteral("Context"), QStringLiteral("Output"),
         QStringLiteral("Tool calls"), QStringLiteral("Errors"), QStringLiteral("Tool tokens"),
         QStringLiteral("At stake"), QStringLiteral("Efficiency"), QStringLiteral("Est. cost")},
        {QStringLiteral("When the run ended"), QStringLiteral("The agent and its session number"),
         QStringLiteral("Runtime and model"),
         QStringLiteral("Tool families switched off when the run launched"),
         QStringLiteral("How the run ended"), QStringLiteral("Wall time of the run"),
         QStringLiteral("Model responses"), QStringLiteral("Input context, cached input included"),
         QStringLiteral("Tokens the model wrote"), QStringLiteral("Tool calls"),
         QStringLiteral("Tool calls that returned an error"),
         QStringLiteral("Tool arguments plus results, at four bytes per token"),
         QStringLiteral("Tokens the improvement log flagged"),
         QStringLiteral("Share of tool tokens not flagged"),
         QStringLiteral("Estimated at the model's list price")});
    m_runs->horizontalHeader()->setSectionResizeMode(AgentColumn, QHeaderView::Stretch);
    m_runs->horizontalHeader()->setSortIndicator(EndedColumn, Qt::DescendingOrder);
    m_runs->setSortingEnabled(true);
    layout->addWidget(m_summary);
    layout->addWidget(m_comparison, 1);
    layout->addWidget(m_runs, 2);
}

void AgentRunTable::refresh(const AgentRunEfficiencyLog *log)
{
    const QVector<AgentRunRecord> runs = log ? log->runs() : QVector<AgentRunRecord>();
    const QVector<AgentRunComparison> groups =
        log ? log->comparison() : QVector<AgentRunComparison>();
    m_summary->setText(
        runs.isEmpty()
            ? QStringLiteral("No finished runs yet. Every agent run is logged to "
                             "agent-runs.jsonl with its model tokens, tool calls and "
                             "the tool families it launched with; switch families off "
                             "on the Families view to compare.")
            : QStringLiteral("%1 runs in %2 configurations. Model tokens are what the "
                             "runtime reported; tool tokens are four-bytes-per-token "
                             "estimates. Runs differ in what they were asked, so compare "
                             "groups with several runs each.")
                  .arg(runs.size())
                  .arg(groups.size()));

    QHash<QString, double> baseline;
    for (const AgentRunComparison &group : groups)
        if (group.allToolsOn())
            baseline.insert(group.provider, group.perRun(group.contextTokens));
    m_comparison->setRowCount(int(groups.size()));
    for (int r = 0; r < groups.size(); ++r) {
        const AgentRunComparison &g = groups.at(r);
        m_comparison->setItem(r, RuntimeColumn, new QTableWidgetItem(g.provider));
        m_comparison->setItem(r, ToolsColumn, new QTableWidgetItem(g.config));
        m_comparison->setItem(r, RunsColumn, new NumberItem(QString::number(g.runs), g.runs));
        m_comparison->setItem(r, FailedColumn,
                              new NumberItem(QString::number(g.failedRuns), g.failedRuns));
        m_comparison->setItem(r, TimeColumn,
                              new NumberItem(AgentToolUsageTable::durationText(
                                                 g.perRun(g.durationMs)),
                                             g.perRun(g.durationMs)));
        m_comparison->setItem(r, ModelCallsColumn,
                              new NumberItem(QString::number(g.perRun(g.modelCalls), 'f', 1),
                                             g.perRun(g.modelCalls)));
        const double context = g.perRun(g.contextTokens);
        m_comparison->setItem(r, ContextColumn, new NumberItem(tokens(context), context));
        const double reference = baseline.value(g.provider);
        QString versus = QStringLiteral("—");
        double change = 0.0;
        if (!g.allToolsOn() && reference > 0) {
            change = context / reference - 1.0;
            versus = QStringLiteral("%1%2%").arg(change >= 0 ? QStringLiteral("+") : QString())
                         .arg(change * 100.0, 0, 'f', 0);
        }
        auto *versusItem = new NumberItem(versus, change);
        if (!g.allToolsOn() && reference > 0)
            versusItem->setForeground(efficiencyColor(change <= 0 ? 1.0 : 0.0));
        m_comparison->setItem(r, ContextVsOnColumn, versusItem);
        m_comparison->setItem(r, OutputColumn,
                              new NumberItem(tokens(g.perRun(g.outputTokens)),
                                             g.perRun(g.outputTokens)));
        m_comparison->setItem(r, ToolCallsColumn,
                              new NumberItem(QString::number(g.perRun(g.toolCalls), 'f', 1),
                                             g.perRun(g.toolCalls)));
        m_comparison->setItem(r, ToolErrorsColumn,
                              new NumberItem(percent(g.toolErrorRate()), g.toolErrorRate()));
        m_comparison->setItem(r, ToolTokensColumn,
                              new NumberItem(tokens(g.perRun(g.toolTokens)),
                                             g.perRun(g.toolTokens)));
        m_comparison->setItem(r, StakeColumn,
                              new NumberItem(tokens(g.perRun(g.tokensAtStake)),
                                             g.perRun(g.tokensAtStake)));
        auto *efficiency = new NumberItem(percent(g.efficiency()), g.efficiency());
        efficiency->setForeground(efficiencyColor(g.efficiency()));
        m_comparison->setItem(r, EfficiencyColumn, efficiency);
        m_comparison->setItem(r, CostColumn,
                              new NumberItem(AgentToolEfficiencyBoard::compactUsd(g.usdPerRun()),
                                             g.usdPerRun()));
    }

    const int sortColumn = m_runs->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_runs->horizontalHeader()->sortIndicatorOrder();
    m_runs->setSortingEnabled(false);
    const int shown = int(qMin<qsizetype>(runs.size(), kMaxRunRows));
    m_runs->setRowCount(shown);
    for (int r = 0; r < shown; ++r) {
        const AgentRunRecord &run = runs.at(runs.size() - 1 - r);
        m_runs->setItem(r, EndedColumn, new NumberItem(endedText(run.endedMs), run.endedMs));
        auto *agent = new QTableWidgetItem(
            run.title.isEmpty() ? QStringLiteral("#%1").arg(run.sessionId)
                                : QStringLiteral("%1 · #%2").arg(run.title).arg(run.sessionId));
        m_runs->setItem(r, AgentColumn, agent);
        m_runs->setItem(r, RunRuntimeColumn,
                        new QTableWidgetItem(run.model.isEmpty()
                                                 ? run.provider
                                                 : QStringLiteral("%1 · %2").arg(run.provider,
                                                                                 run.model)));
        m_runs->setItem(r, RunToolsColumn, new QTableWidgetItem(run.configLabel()));
        auto *outcome = new QTableWidgetItem(run.outcome);
        if (run.failed())
            outcome->setForeground(efficiencyColor(0.0));
        m_runs->setItem(r, OutcomeColumn, outcome);
        m_runs->setItem(r, RunTimeColumn,
                        new NumberItem(AgentToolUsageTable::durationText(double(run.durationMs)),
                                       double(run.durationMs)));
        m_runs->setItem(r, RunModelCallsColumn,
                        new NumberItem(QString::number(run.modelCalls), run.modelCalls));
        m_runs->setItem(r, RunContextColumn,
                        new NumberItem(tokens(run.contextTokens()), run.contextTokens()));
        m_runs->setItem(r, RunOutputColumn,
                        new NumberItem(tokens(run.outputTokens), run.outputTokens));
        m_runs->setItem(r, RunToolCallsColumn,
                        new NumberItem(QString::number(run.toolCalls), run.toolCalls));
        m_runs->setItem(r, RunErrorsColumn,
                        new NumberItem(QString::number(run.toolErrors), run.toolErrors));
        m_runs->setItem(r, RunToolTokensColumn,
                        new NumberItem(tokens(run.toolTokens()), run.toolTokens()));
        m_runs->setItem(r, RunStakeColumn,
                        new NumberItem(tokens(run.tokensAtStake), run.tokensAtStake));
        auto *efficiency = new NumberItem(percent(run.efficiency()), run.efficiency());
        efficiency->setForeground(efficiencyColor(run.efficiency()));
        m_runs->setItem(r, RunEfficiencyColumn, efficiency);
        m_runs->setItem(r, RunCostColumn,
                        new NumberItem(AgentToolEfficiencyBoard::compactUsd(run.estimatedUsd),
                                       run.estimatedUsd));
    }
    m_runs->setSortingEnabled(true);
    m_runs->sortItems(sortColumn, sortOrder);
}
