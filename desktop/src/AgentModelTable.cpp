#include "AgentModelTable.h"

#include "AgentToolEfficiencyBoard.h"
#include "AgentToolUsageTable.h"

#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QLocale>

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

const QString kDash = QStringLiteral("—");

QString count(qint64 value)
{
    return value > 0 ? AgentToolEfficiencyBoard::compactNumber(value) : kDash;
}

QString percent(double rate, bool measured)
{
    return measured ? QStringLiteral("%1%").arg(rate * 100.0, 0, 'f', 0) : kDash;
}

QString usd(double value)
{
    return value > 0 ? AgentToolEfficiencyBoard::compactUsd(value) : kDash;
}

QString decimal(double value)
{
    return value > 0 ? QString::number(value, 'f', value < 10 ? 1 : 0) : kDash;
}

QString lastUsedText(qint64 ms)
{
    if (ms <= 0)
        return kDash;
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    return at.date() == QDate::currentDate()
               ? at.toString(QStringLiteral("HH:mm"))
               : QLocale().toString(at.date(), QLocale::ShortFormat);
}

QString modelTooltip(const AgentModelStatsRow &row)
{
    QStringList lines;
    lines << QStringLiteral("%1 · %2").arg(row.label, row.agentName);
    if (!row.model.isEmpty())
        lines << QStringLiteral("Model id: %1").arg(row.model);
    lines << QStringLiteral("Runs: %1 (%2 from deleted agents)")
                 .arg(row.runs)
                 .arg(row.retiredRuns);
    lines << QStringLiteral("Merged by the desktop: %1 · credited in git history: %2")
                 .arg(row.merged)
                 .arg(row.gitMerges);
    lines << QStringLiteral("Cost: %1 reported by the runtime, %2 estimated from tokens")
                 .arg(AgentToolEfficiencyBoard::compactUsd(row.reportedUsd),
                      AgentToolEfficiencyBoard::compactUsd(row.estimatedUsd));
    if (row.peakContextTokens > 0)
        lines << QStringLiteral("Largest context: %1 tokens")
                     .arg(AgentToolEfficiencyBoard::compactNumber(row.peakContextTokens));
    if (row.toolCalls > 0)
        lines << QStringLiteral("Tools: %1 calls, %2 errors, %3 average, %4 tokens saved")
                     .arg(row.toolCalls)
                     .arg(row.toolErrors)
                     .arg(AgentToolUsageTable::durationText(row.averageToolLatencyMs()),
                          AgentToolEfficiencyBoard::compactNumber(row.toolSavedTokens));
    return lines.join(QLatin1Char('\n'));
}

} // namespace

AgentModelTable::AgentModelTable(QWidget *parent) : QTableWidget(parent)
{
    setObjectName(QStringLiteral("agentModelTable"));
    setColumnCount(ColumnCount);
    const QList<QPair<QString, QString>> headers = {
        {QStringLiteral("Show"), QStringLiteral("Tick to list the model in the prompt "
                                                "composer's dropdown")},
        {QStringLiteral("Model"), QStringLiteral("The model, with the face its agents wear")},
        {QStringLiteral("Agent"), QStringLiteral("The CLI or API that runs it")},
        {QStringLiteral("Runs"), QStringLiteral("Agent runs on this desktop, deleted ones included")},
        {QStringLiteral("Live"), QStringLiteral("Running, waiting or queued now")},
        {QStringLiteral("OK"), QStringLiteral("Runs that finished successfully or merged")},
        {QStringLiteral("Fail"), QStringLiteral("Runs that failed")},
        {QStringLiteral("Stop"), QStringLiteral("Runs stopped or cleared by hand")},
        {QStringLiteral("OK%"), QStringLiteral("Successful share of finished runs")},
        {QStringLiteral("Fail%"), QStringLiteral("Failed share of finished runs")},
        {QStringLiteral("Merged"), QStringLiteral("Runs the desktop merged, deleted ones included")},
        {QStringLiteral("Mrg%"), QStringLiteral("Merged share of all runs")},
        {QStringLiteral("Git"), QStringLiteral("Merges backfilled from the repositories' commit "
                                               "messages: each merge goes to the model most of "
                                               "its commits name in Co-Authored-By or "
                                               "ForkMesh-Agent trailers")},
        {QStringLiteral("Commits"), QStringLiteral("Commits that name the model in a trailer")},
        {QStringLiteral("PRs"), QStringLiteral("Runs that opened a pull request")},
        {QStringLiteral("≈Cost"), QStringLiteral("Approximate spend: the runtime's reported cost, "
                                                 "else tokens at list price")},
        {QStringLiteral("$/run"), QStringLiteral("Approximate spend per stored run")},
        {QStringLiteral("$/merge"), QStringLiteral("Approximate spend per merged run")},
        {QStringLiteral("In"), QStringLiteral("Prompt tokens")},
        {QStringLiteral("Out"), QStringLiteral("Completion tokens")},
        {QStringLiteral("Turns"), QStringLiteral("Turns per run")},
        {QStringLiteral("Time"), QStringLiteral("Average time spent working per run")},
        {QStringLiteral("Tools"), QStringLiteral("Tool calls recorded by the tool-efficiency ledger")},
        {QStringLiteral("T/run"), QStringLiteral("Tool calls per stored run")},
        {QStringLiteral("T.err"), QStringLiteral("Tool calls that returned an error")},
        {QStringLiteral("Tok/T"), QStringLiteral("Context each tool call costs: arguments plus "
                                                 "result, estimated at four bytes per token")},
        {QStringLiteral("Last"), QStringLiteral("When the model last ran or merged")},
    };
    QStringList labels;
    for (const auto &header : headers)
        labels << header.first;
    setHorizontalHeaderLabels(labels);
    for (int column = 0; column < ColumnCount; ++column)
        horizontalHeaderItem(column)->setToolTip(headers.value(column).second);

    QFont compact = font();
    compact.setPointSizeF(qMax(7.0, compact.pointSizeF() - 1.0));
    setFont(compact);
    horizontalHeader()->setFont(compact);
    setIconSize(QSize(16, 16));
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setFrameShape(QFrame::NoFrame);
    verticalHeader()->hide();
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(20);
    horizontalHeader()->setMinimumSectionSize(24);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSortIndicatorShown(true);
    horizontalHeader()->setSortIndicator(GitMergesColumn, Qt::DescendingOrder);
    setSortingEnabled(true);
    connect(horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
            [this] { fitColumns(); });
    connect(this, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *cell) {
        if (m_filling || !cell || cell->column() != ShowColumn)
            return;
        const QString key = cell->data(Qt::UserRole + 1).toString();
        if (key.isEmpty())
            return;
        const bool shown = cell->checkState() == Qt::Checked;
        m_filling = true;
        cell->setData(Qt::UserRole, shown ? 1.0 : 0.0);
        cell->setToolTip(shown ? QStringLiteral("Listed in the composer dropdown")
                               : QStringLiteral("Hidden from the composer dropdown"));
        m_filling = false;
        if (composerShowChanged)
            composerShowChanged(key, shown);
    });
}

void AgentModelTable::fitColumns()
{
    const QFontMetrics cells(font());
    const QFontMetrics headers(horizontalHeader()->font());
    const int sorted = horizontalHeader()->sortIndicatorSection();
    for (int column = 0; column < ColumnCount; ++column) {
        const QTableWidgetItem *head = horizontalHeaderItem(column);
        int width = headers.horizontalAdvance(head ? head->text() : QString()) +
                    (column == sorted ? 22 : 10);
        if (column == ShowColumn)
            width = qMax(width, 28);
        for (int r = 0; r < rowCount(); ++r)
            if (const QTableWidgetItem *cell = item(r, column))
                width = qMax(width, cells.horizontalAdvance(cell->text()) + 10 +
                                        (cell->icon().isNull() ? 0 : iconSize().width() + 6));
        horizontalHeader()->resizeSection(column, width);
    }
}

void AgentModelTable::refresh(const QVector<AgentModelStatsRow> &rows)
{
    const int sortColumn = horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = horizontalHeader()->sortIndicatorOrder();
    QString selectedKey;
    if (const QTableWidgetItem *current = currentItem())
        if (const QTableWidgetItem *model = item(current->row(), ModelColumn))
            selectedKey = model->data(Qt::UserRole + 1).toString();

    m_filling = true;
    setSortingEnabled(false);
    clearContents();
    setRowCount(rows.size());
    const QColor danger(QStringLiteral("#cf222e"));
    const QColor good(QStringLiteral("#1a7f37"));
    for (int r = 0; r < rows.size(); ++r) {
        const AgentModelStatsRow &row = rows.at(r);
        auto *model = new QTableWidgetItem(row.label);
        model->setData(Qt::UserRole, row.label.toLower());
        model->setData(Qt::UserRole + 1, row.key);
        model->setData(Qt::UserRole + 2, row.hasActivity());
        model->setToolTip(modelTooltip(row));
        if (iconFor)
            model->setIcon(iconFor(row));
        setItem(r, ModelColumn, model);
        const QString composerKey = composerKeyFor ? composerKeyFor(row) : QString();
        const bool offered = !composerKey.isEmpty();
        const bool shown = offered && (!composerShows || composerShows(composerKey));
        auto *show = new NumberItem(QString(), offered ? (shown ? 1.0 : 0.0) : -1.0);
        show->setTextAlignment(Qt::AlignCenter);
        if (offered) {
            show->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable |
                           Qt::ItemIsSelectable);
            show->setData(Qt::UserRole + 1, composerKey);
            show->setCheckState(shown ? Qt::Checked : Qt::Unchecked);
            show->setToolTip(shown ? QStringLiteral("Listed in the composer dropdown")
                                   : QStringLiteral("Hidden from the composer dropdown"));
        } else {
            show->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            show->setToolTip(QStringLiteral("Seen in past runs; not offered in the "
                                            "composer dropdown"));
        }
        setItem(r, ShowColumn, show);
        auto *agent = new QTableWidgetItem(row.agentName);
        agent->setToolTip(row.provider);
        setItem(r, AgentColumn, agent);

        const auto number = [&](int column, const QString &text, double value) {
            auto *cell = new NumberItem(text, value);
            setItem(r, column, cell);
            return cell;
        };
        number(RunsColumn, count(row.runs), row.runs);
        number(LiveColumn, count(row.active), row.active);
        number(SucceededColumn, count(row.succeeded), row.succeeded);
        number(FailedColumn, count(row.failed), row.failed);
        number(StoppedColumn, count(row.stopped), row.stopped);
        const bool finished = row.finished() > 0;
        auto *success = number(SuccessRateColumn, percent(row.successRate(), finished),
                               finished ? row.successRate() : -1);
        if (row.finished() >= 4 && row.successRate() >= 0.75)
            success->setForeground(good);
        auto *failure = number(FailureRateColumn, percent(row.failureRate(), finished),
                               finished ? row.failureRate() : -1);
        if (row.finished() >= 4 && row.failureRate() >= 0.25)
            failure->setForeground(danger);
        number(MergedColumn, count(row.merged), row.merged);
        number(MergeRateColumn, percent(row.mergeRate(), row.runs > 0),
               row.runs > 0 ? row.mergeRate() : -1);
        number(GitMergesColumn, count(row.gitMerges), row.gitMerges);
        number(CommitsColumn, count(row.gitCommits), row.gitCommits);
        number(PullRequestsColumn, count(row.pullRequests), row.pullRequests);
        auto *cost = number(CostColumn, usd(row.costUsd()), row.costUsd());
        if (row.estimatedUsd > 0 && row.reportedUsd <= 0)
            cost->setText(QStringLiteral("~") + cost->text());
        number(CostPerRunColumn, usd(row.costPerRun()), row.costPerRun());
        number(CostPerMergeColumn, usd(row.costPerMerge()), row.costPerMerge());
        number(InputTokensColumn, count(row.promptTokens), double(row.promptTokens));
        number(OutputTokensColumn, count(row.completionTokens),
               double(row.completionTokens));
        number(TurnsPerRunColumn, decimal(row.turnsPerRun()), row.turnsPerRun());
        const double averageMs = row.averageRunMs();
        number(AverageTimeColumn,
               averageMs > 0 ? AgentToolUsageTable::durationText(averageMs) : kDash,
               averageMs);
        number(ToolCallsColumn, count(row.toolCalls), double(row.toolCalls));
        number(ToolCallsPerRunColumn, decimal(row.toolCallsPerRun()),
               row.toolCallsPerRun());
        auto *toolErrors =
            number(ToolErrorRateColumn, percent(row.toolErrorRate(), row.toolCalls > 0),
                   row.toolCalls > 0 ? row.toolErrorRate() : -1);
        if (row.toolCalls >= 20 && row.toolErrorRate() >= 0.1)
            toolErrors->setForeground(danger);
        number(TokensPerToolCallColumn, count(qRound64(row.tokensPerToolCall())),
               row.tokensPerToolCall());
        number(LastUsedColumn, lastUsedText(row.lastUsedMs), double(row.lastUsedMs));
    }
    setSortingEnabled(true);
    sortItems(sortColumn, sortOrder);
    m_filling = false;
    applyFilter();
    fitColumns();
    if (!selectedKey.isEmpty())
        for (int r = 0; r < rowCount(); ++r)
            if (item(r, ModelColumn)->data(Qt::UserRole + 1).toString() == selectedKey) {
                setCurrentCell(r, ModelColumn);
                break;
            }
}

void AgentModelTable::setHideUnused(bool hide)
{
    m_hideUnused = hide;
    applyFilter();
}

int AgentModelTable::visibleRowCount() const
{
    int visible = 0;
    for (int r = 0; r < rowCount(); ++r)
        if (!isRowHidden(r))
            ++visible;
    return visible;
}

QString AgentModelTable::cellText(const QString &label, int column) const
{
    for (int r = 0; r < rowCount(); ++r)
        if (const QTableWidgetItem *model = item(r, ModelColumn);
            model && model->text() == label)
            if (const QTableWidgetItem *cell = item(r, column))
                return cell->text();
    return QString();
}

void AgentModelTable::applyFilter()
{
    for (int r = 0; r < rowCount(); ++r) {
        const QTableWidgetItem *model = item(r, ModelColumn);
        setRowHidden(r, m_hideUnused && model &&
                            !model->data(Qt::UserRole + 2).toBool());
    }
}
