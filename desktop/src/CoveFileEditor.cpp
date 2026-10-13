#include "CoveFileEditor.h"
#include "Theme.h"

#include "CoveFileSupport.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::cove {
namespace {

QPushButton *toolbarButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QTableWidgetItem *cell(const QString &text = QString())
{
    return new QTableWidgetItem(text);
}

QString normalizedMoney(QString value)
{
    value.remove(QLatin1Char('$'));
    value.remove(QLatin1Char(','));
    return value.trimmed();
}

} // namespace

SpreadsheetEditor::SpreadsheetEditor(const QString &csv, bool readOnly,
                                     QWidget *parent)
    : QWidget(parent), m_readOnly(readOnly)
{
    setObjectName(QStringLiteral("coveSpreadsheetEditor"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(8, 6, 8, 0);
    toolbar->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("Spreadsheet"));
    title->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    toolbar->addWidget(title);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("statusLine"));
    toolbar->addWidget(m_status);
    toolbar->addStretch();
    m_addRowButton = toolbarButton(QStringLiteral("+ Row"), this);
    m_addColumnButton = toolbarButton(QStringLiteral("+ Column"), this);
    m_deleteRowButton = toolbarButton(QStringLiteral("Delete row"), this);
    m_deleteColumnButton = toolbarButton(QStringLiteral("Delete column"), this);
    toolbar->addWidget(m_addRowButton);
    toolbar->addWidget(m_addColumnButton);
    toolbar->addWidget(m_deleteRowButton);
    toolbar->addWidget(m_deleteColumnButton);
    layout->addLayout(toolbar);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("coveSpreadsheetGrid"));
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setDefaultSectionSize(150);
    m_table->verticalHeader()->setDefaultSectionSize(28);
    m_table->installEventFilter(this);
    layout->addWidget(m_table, 1);

    const QList<QStringList> rows = parseCsv(csv);
    int columns = 1;
    for (const QStringList &row : rows)
        columns = qMax(columns, row.size());
    m_loading = true;
    m_table->setRowCount(qMax(1, rows.size()));
    m_table->setColumnCount(columns);
    QStringList headers;
    for (int column = 0; column < columns; ++column)
        headers << columnLabel(column);
    m_table->setHorizontalHeaderLabels(headers);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (int column = 0; column < columns; ++column) {
            const QString value = row < rows.size() && column < rows.at(row).size()
                                      ? rows.at(row).at(column)
                                      : QString();
            QTableWidgetItem *item = cell(value);
            if (row == 0) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
            m_table->setItem(row, column, item);
        }
    }
    m_loading = false;

    connect(m_table, &QTableWidget::itemChanged, this,
            [this] { markModified(); });
    connect(m_addRowButton, &QPushButton::clicked, this,
            [this] { addRow(); });
    connect(m_addColumnButton, &QPushButton::clicked, this,
            [this] { addColumn(); });
    connect(m_deleteRowButton, &QPushButton::clicked, this,
            [this] { deleteSelectedRows(); });
    connect(m_deleteColumnButton, &QPushButton::clicked, this,
            [this] { deleteSelectedColumns(); });

    const QAbstractItemView::EditTriggers editTriggers =
        readOnly ? QAbstractItemView::NoEditTriggers
                 : QAbstractItemView::DoubleClicked |
                       QAbstractItemView::EditKeyPressed |
                       QAbstractItemView::AnyKeyPressed;
    m_table->setEditTriggers(editTriggers);
    m_addRowButton->setEnabled(!readOnly);
    m_addColumnButton->setEnabled(!readOnly);
    m_deleteRowButton->setEnabled(!readOnly);
    m_deleteColumnButton->setEnabled(!readOnly);
    updateStatus();
}

QString SpreadsheetEditor::columnLabel(int column)
{
    QString label;
    for (int value = column + 1; value > 0; value = (value - 1) / 26)
        label.prepend(QChar(QLatin1Char('A').unicode() + (value - 1) % 26));
    return label;
}

QString SpreadsheetEditor::body() const
{
    QList<QStringList> rows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QStringList values;
        for (int column = 0; column < m_table->columnCount(); ++column) {
            const QTableWidgetItem *item = m_table->item(row, column);
            values << (item ? item->text() : QString());
        }
        rows << values;
    }
    return serializeCsv(rows);
}

void SpreadsheetEditor::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    if (m_changeHandler)
        m_changeHandler(m_modified);
}

void SpreadsheetEditor::setChangeHandler(std::function<void(bool)> handler)
{
    m_changeHandler = std::move(handler);
}

void SpreadsheetEditor::markModified()
{
    if (m_loading || m_readOnly)
        return;
    m_modified = true;
    updateStatus();
    if (m_changeHandler)
        m_changeHandler(true);
}

void SpreadsheetEditor::updateStatus()
{
    if (!m_status)
        return;
    m_status->setText(QStringLiteral("%1 row%2 × %3 column%4%5")
                          .arg(m_table->rowCount())
                          .arg(m_table->rowCount() == 1 ? QString() : QStringLiteral("s"))
                          .arg(m_table->columnCount())
                          .arg(m_table->columnCount() == 1 ? QString()
                                                          : QStringLiteral("s"))
                          .arg(m_readOnly ? QStringLiteral(" · read only")
                                          : QString()));
}

void SpreadsheetEditor::addRow()
{
    if (m_readOnly)
        return;
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int column = 0; column < m_table->columnCount(); ++column)
        m_table->setItem(row, column, cell());
    m_table->setCurrentCell(row, 0);
    markModified();
    updateStatus();
}

void SpreadsheetEditor::addColumn()
{
    if (m_readOnly)
        return;
    const int column = m_table->columnCount();
    m_table->insertColumn(column);
    m_table->setHorizontalHeaderItem(column, cell(columnLabel(column)));
    for (int row = 0; row < m_table->rowCount(); ++row)
        m_table->setItem(row, column, cell());
    m_table->setCurrentCell(0, column);
    markModified();
    updateStatus();
}

void SpreadsheetEditor::deleteSelectedRows()
{
    if (m_readOnly || m_table->rowCount() <= 1)
        return;
    QSet<int> selected;
    for (const QTableWidgetSelectionRange &range : m_table->selectedRanges()) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row)
            selected.insert(row);
    }
    if (selected.isEmpty())
        selected.insert(m_table->currentRow());
    QList<int> rows = selected.values();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        if (m_table->rowCount() > 1 && row >= 0)
            m_table->removeRow(row);
    }
    markModified();
    updateStatus();
}

void SpreadsheetEditor::deleteSelectedColumns()
{
    if (m_readOnly || m_table->columnCount() <= 1)
        return;
    QSet<int> selected;
    for (const QTableWidgetSelectionRange &range : m_table->selectedRanges()) {
        for (int column = range.leftColumn(); column <= range.rightColumn(); ++column)
            selected.insert(column);
    }
    if (selected.isEmpty())
        selected.insert(m_table->currentColumn());
    QList<int> columns = selected.values();
    std::sort(columns.begin(), columns.end(), std::greater<int>());
    for (int column : columns) {
        if (m_table->columnCount() > 1 && column >= 0)
            m_table->removeColumn(column);
    }
    QStringList headers;
    for (int column = 0; column < m_table->columnCount(); ++column)
        headers << columnLabel(column);
    m_table->setHorizontalHeaderLabels(headers);
    markModified();
    updateStatus();
}

void SpreadsheetEditor::copySelection() const
{
    const QList<QTableWidgetSelectionRange> ranges = m_table->selectedRanges();
    if (ranges.isEmpty())
        return;
    const QTableWidgetSelectionRange range = ranges.first();
    QStringList lines;
    for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
        QStringList values;
        for (int column = range.leftColumn(); column <= range.rightColumn(); ++column) {
            const QTableWidgetItem *item = m_table->item(row, column);
            values << (item ? item->text() : QString());
        }
        lines << values.join(QLatin1Char('\t'));
    }
    if (QApplication::clipboard())
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void SpreadsheetEditor::pasteSelection()
{
    if (m_readOnly || !QApplication::clipboard())
        return;
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    QList<QStringList> rows;
    if (text.contains(QLatin1Char('\t'))) {
        QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        for (QString line : std::as_const(lines)) {
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            rows << line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        }
    } else {
        rows = parseCsv(text);
    }
    if (rows.isEmpty())
        return;
    const int startRow = qMax(0, m_table->currentRow());
    const int startColumn = qMax(0, m_table->currentColumn());
    int requiredColumns = startColumn + 1;
    for (const QStringList &row : rows)
        requiredColumns = qMax(requiredColumns, startColumn + row.size());
    m_loading = true;
    while (m_table->rowCount() < startRow + rows.size())
        m_table->insertRow(m_table->rowCount());
    while (m_table->columnCount() < requiredColumns)
        m_table->insertColumn(m_table->columnCount());
    QStringList headers;
    for (int column = 0; column < m_table->columnCount(); ++column)
        headers << columnLabel(column);
    m_table->setHorizontalHeaderLabels(headers);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (int column = 0; column < m_table->columnCount(); ++column) {
            if (!m_table->item(row, column))
                m_table->setItem(row, column, cell());
        }
    }
    for (int row = 0; row < rows.size(); ++row) {
        for (int column = 0; column < rows.at(row).size(); ++column)
            m_table->item(startRow + row, startColumn + column)
                ->setText(rows.at(row).at(column));
    }
    m_loading = false;
    markModified();
    updateStatus();
}

void SpreadsheetEditor::clearSelection()
{
    if (m_readOnly)
        return;
    m_loading = true;
    for (QTableWidgetItem *item : m_table->selectedItems())
        item->setText(QString());
    m_loading = false;
    markModified();
}

bool SpreadsheetEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy)) {
            copySelection();
            return true;
        }
        if (key->matches(QKeySequence::Paste)) {
            pasteSelection();
            return true;
        }
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            clearSelection();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

PipelineEditor::PipelineEditor(const QString &json, const QString &actor,
                               bool readOnly, QWidget *parent)
    : QWidget(parent), m_actor(actor.trimmed()), m_readOnly(readOnly)
{
    setObjectName(QStringLiteral("covePipelineEditor"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(10, 7, 10, 2);
    auto *title = new QLabel(QStringLiteral("Lead pipeline"));
    title->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    header->addWidget(title);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("statusLine"));
    header->addWidget(m_status);
    header->addStretch();
    layout->addLayout(header);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);

    auto *leadsPage = new QWidget;
    auto *leadsLayout = new QVBoxLayout(leadsPage);
    leadsLayout->setContentsMargins(8, 8, 8, 8);
    auto *leadActions = new QHBoxLayout;
    m_addLeadButton = toolbarButton(QStringLiteral("+ Lead"), leadsPage);
    m_addLeadButton->setObjectName(QStringLiteral("covePipelineAddLead"));
    m_deleteLeadButton = toolbarButton(QStringLiteral("Delete lead"), leadsPage);
    leadActions->addWidget(m_addLeadButton);
    leadActions->addWidget(m_deleteLeadButton);
    leadActions->addStretch();
    leadsLayout->addLayout(leadActions);
    m_leads = new QTableWidget(0, 5, leadsPage);
    m_leads->setObjectName(QStringLiteral("covePipelineLeads"));
    m_leads->setHorizontalHeaderLabels(
        {QStringLiteral("Lead"), QStringLiteral("Company"),
         QStringLiteral("Value"), QStringLiteral("Owner"),
         QStringLiteral("Stage")});
    m_leads->setAlternatingRowColors(true);
    m_leads->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_leads->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_leads->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_leads->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_leads->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_leads->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    // A QHeaderView in ResizeToContents mode measures the table model, but the
    // stage is an index widget rather than a QTableWidgetItem.  The resulting
    // section was only wide enough for the "Stage" header, while the default
    // row was shorter than the styled combo.  Size both from the populated
    // combos in refreshStageCombos().
    m_leads->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    m_leads->setColumnWidth(4, 150);
    m_leads->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_leads->verticalHeader()->setMinimumSectionSize(40);
    leadsLayout->addWidget(m_leads, 1);
    m_tabs->addTab(leadsPage, QStringLiteral("Leads"));

    auto *stagesPage = new QWidget;
    auto *stagesLayout = new QVBoxLayout(stagesPage);
    stagesLayout->setContentsMargins(8, 8, 8, 8);
    auto *stageActions = new QHBoxLayout;
    m_addStageButton = toolbarButton(QStringLiteral("+ Stage"), stagesPage);
    m_addStageButton->setObjectName(QStringLiteral("covePipelineAddStage"));
    m_deleteStageButton = toolbarButton(QStringLiteral("Delete stage"), stagesPage);
    stageActions->addWidget(m_addStageButton);
    stageActions->addWidget(m_deleteStageButton);
    stageActions->addStretch();
    stagesLayout->addLayout(stageActions);
    m_stages = new QTableWidget(0, 2, stagesPage);
    m_stages->setObjectName(QStringLiteral("covePipelineStages"));
    m_stages->setHorizontalHeaderLabels(
        {QStringLiteral("Stage"), QStringLiteral("Color")});
    m_stages->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_stages->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_stages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    stagesLayout->addWidget(m_stages, 1);
    auto *stageHint = new QLabel(QStringLiteral(
        "Stage names remain linked to their history even when renamed."));
    stageHint->setObjectName(QStringLiteral("statusLine"));
    stagesLayout->addWidget(stageHint);
    m_tabs->addTab(stagesPage, QStringLiteral("Stages"));

    auto *reportsPage = new QWidget;
    auto *reportsLayout = new QVBoxLayout(reportsPage);
    reportsLayout->setContentsMargins(8, 8, 8, 8);
    m_reportHeadline = new QLabel;
    m_reportHeadline->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    reportsLayout->addWidget(m_reportHeadline);
    m_summary = new QTableWidget(0, 4, reportsPage);
    m_summary->setObjectName(QStringLiteral("covePipelineReport"));
    m_summary->setHorizontalHeaderLabels(
        {QStringLiteral("Stage"), QStringLiteral("Leads"),
         QStringLiteral("Value"), QStringLiteral("Share")});
    m_summary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_summary->setSelectionMode(QAbstractItemView::NoSelection);
    m_summary->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_summary->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_summary->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_summary->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    reportsLayout->addWidget(m_summary);
    auto *historyTitle = new QLabel(QStringLiteral("Movement history"));
    historyTitle->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600; margin-top:6px;")));
    reportsLayout->addWidget(historyTitle);
    m_historyTable = new QTableWidget(0, 5, reportsPage);
    m_historyTable->setObjectName(QStringLiteral("covePipelineHistory"));
    m_historyTable->setHorizontalHeaderLabels(
        {QStringLiteral("When"), QStringLiteral("Lead"),
         QStringLiteral("From"), QStringLiteral("To"),
         QStringLiteral("By")});
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_historyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_historyTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_historyTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    reportsLayout->addWidget(m_historyTable, 1);
    m_tabs->addTab(reportsPage, QStringLiteral("Reports & history"));
    layout->addWidget(m_tabs, 1);

    QJsonObject pipeline;
    QString error;
    if (!parsePipeline(json, &pipeline, &error))
        pipeline = defaultPipelineObject();
    load(pipeline);

    connect(m_addLeadButton, &QPushButton::clicked, this,
            [this] { addLead(); });
    connect(m_deleteLeadButton, &QPushButton::clicked, this,
            [this] { deleteSelectedLeads(); });
    connect(m_addStageButton, &QPushButton::clicked, this,
            [this] { addStage(); });
    connect(m_deleteStageButton, &QPushButton::clicked, this,
            [this] { deleteSelectedStage(); });
    connect(m_leads, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (!item || m_loading)
                    return;
                updateLeadTimestamp(item->row());
                markModified();
                refreshReports();
            });
    connect(m_stages, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (!item || m_loading)
                    return;
                if (item->column() == 1) {
                    const QColor color(item->text());
                    QSignalBlocker blocker(m_stages);
                    item->setBackground(color.isValid() ? color
                                                        : QColor(Qt::transparent));
                }
                refreshStageCombos();
                markModified();
                refreshReports();
            });

    const QAbstractItemView::EditTriggers editTriggers =
        readOnly ? QAbstractItemView::NoEditTriggers
                 : QAbstractItemView::DoubleClicked |
                       QAbstractItemView::EditKeyPressed |
                       QAbstractItemView::AnyKeyPressed;
    m_leads->setEditTriggers(editTriggers);
    m_stages->setEditTriggers(editTriggers);
    m_addLeadButton->setEnabled(!readOnly);
    m_deleteLeadButton->setEnabled(!readOnly);
    m_addStageButton->setEnabled(!readOnly);
    m_deleteStageButton->setEnabled(!readOnly);
    refreshStageCombos();
    refreshReports();
}

void PipelineEditor::load(const QJsonObject &pipeline)
{
    m_loading = true;
    m_root = pipeline;
    m_history = pipeline.value(QStringLiteral("history")).toArray();
    m_stages->setRowCount(0);
    for (const QJsonValue &value : pipeline.value(QStringLiteral("stages")).toArray()) {
        const QJsonObject stage = value.toObject();
        const int row = m_stages->rowCount();
        m_stages->insertRow(row);
        QTableWidgetItem *name = cell(stage.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, stage.value(QStringLiteral("id")).toString());
        QTableWidgetItem *color =
            cell(stage.value(QStringLiteral("color")).toString(QStringLiteral("#58a6ff")));
        const QColor parsed(color->text());
        if (parsed.isValid())
            color->setBackground(parsed);
        m_stages->setItem(row, 0, name);
        m_stages->setItem(row, 1, color);
    }

    m_leads->setRowCount(0);
    for (const QJsonValue &value : pipeline.value(QStringLiteral("leads")).toArray()) {
        const QJsonObject lead = value.toObject();
        const int row = m_leads->rowCount();
        m_leads->insertRow(row);
        QTableWidgetItem *name = cell(lead.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole,
                      lead.value(QStringLiteral("id")).toString(
                          QUuid::createUuid().toString(QUuid::WithoutBraces)));
        name->setData(Qt::UserRole + 2,
                      lead.value(QStringLiteral("updatedAt")).toString());
        m_leads->setItem(row, 0, name);
        m_leads->setItem(row, 1,
                         cell(lead.value(QStringLiteral("company")).toString()));
        const QJsonValue valueField = lead.value(QStringLiteral("value"));
        m_leads->setItem(
            row, 2,
            cell(valueField.isDouble()
                     ? QString::number(valueField.toDouble(), 'f', 2)
                     : valueField.toString()));
        m_leads->setItem(row, 3,
                         cell(lead.value(QStringLiteral("owner")).toString()));
        auto *stage = new QComboBox(m_leads);
        stage->setProperty("stageId", lead.value(QStringLiteral("stage")).toString());
        m_leads->setCellWidget(row, 4, stage);
    }
    m_loading = false;
}

QString PipelineEditor::body() const
{
    QJsonObject root = m_root;
    QJsonArray stages;
    for (int row = 0; row < m_stages->rowCount(); ++row) {
        const QTableWidgetItem *name = m_stages->item(row, 0);
        const QTableWidgetItem *color = m_stages->item(row, 1);
        stages.append(QJsonObject{
            {QStringLiteral("id"), name ? name->data(Qt::UserRole).toString() : QString()},
            {QStringLiteral("name"), name ? name->text().trimmed() : QString()},
            {QStringLiteral("color"), color ? color->text().trimmed() : QString()}});
    }
    QJsonArray leads;
    for (int row = 0; row < m_leads->rowCount(); ++row) {
        const QTableWidgetItem *name = m_leads->item(row, 0);
        const auto *stage = qobject_cast<QComboBox *>(m_leads->cellWidget(row, 4));
        leads.append(QJsonObject{
            {QStringLiteral("id"), name ? name->data(Qt::UserRole).toString() : QString()},
            {QStringLiteral("name"), name ? name->text().trimmed() : QString()},
            {QStringLiteral("company"), m_leads->item(row, 1)->text().trimmed()},
            {QStringLiteral("value"), leadValue(row)},
            {QStringLiteral("owner"), m_leads->item(row, 3)->text().trimmed()},
            {QStringLiteral("stage"), stage ? stage->currentData().toString() : QString()},
            {QStringLiteral("updatedAt"),
             name ? name->data(Qt::UserRole + 2).toString() : QString()}});
    }
    root.insert(QStringLiteral("stages"), stages);
    root.insert(QStringLiteral("leads"), leads);
    root.insert(QStringLiteral("history"), m_history);
    root.insert(QStringLiteral("version"), 1);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void PipelineEditor::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    if (m_changeHandler)
        m_changeHandler(m_modified);
}

void PipelineEditor::setChangeHandler(std::function<void(bool)> handler)
{
    m_changeHandler = std::move(handler);
}

void PipelineEditor::markModified()
{
    if (m_loading || m_readOnly)
        return;
    m_modified = true;
    if (m_changeHandler)
        m_changeHandler(true);
}

QString PipelineEditor::firstStageId() const
{
    const QTableWidgetItem *item = m_stages->item(0, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString PipelineEditor::stageName(const QString &id) const
{
    for (int row = 0; row < m_stages->rowCount(); ++row) {
        const QTableWidgetItem *item = m_stages->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == id)
            return item->text().trimmed();
    }
    return id;
}

QString PipelineEditor::stageColor(const QString &id) const
{
    for (int row = 0; row < m_stages->rowCount(); ++row) {
        const QTableWidgetItem *item = m_stages->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == id) {
            const QTableWidgetItem *color = m_stages->item(row, 1);
            return color ? color->text() : QString();
        }
    }
    return {};
}

void PipelineEditor::refreshStageCombos()
{
    int stageColumnWidth = 140;
    for (int row = 0; row < m_leads->rowCount(); ++row) {
        auto *combo = qobject_cast<QComboBox *>(m_leads->cellWidget(row, 4));
        if (!combo) {
            combo = new QComboBox(m_leads);
            m_leads->setCellWidget(row, 4, combo);
        }
        combo->setObjectName(QStringLiteral("covePipelineStageCombo"));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        if (!combo->property("covePipelineConnected").toBool()) {
            combo->setProperty("covePipelineConnected", true);
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, combo](int) {
                        if (m_loading)
                            return;
                        const int leadRow = leadRowForWidget(combo);
                        if (leadRow < 0)
                            return;
                        const QString from = combo->property("stageId").toString();
                        const QString to = combo->currentData().toString();
                        if (from == to)
                            return;
                        appendHistory(leadRow, from, to, QStringLiteral("moved"));
                        combo->setProperty("stageId", to);
                        updateLeadTimestamp(leadRow);
                        markModified();
                        refreshReports();
                    });
        }
        QString selected = combo->property("stageId").toString();
        if (selected.isEmpty())
            selected = combo->currentData().toString();
        QSignalBlocker blocker(combo);
        combo->clear();
        for (int stageRow = 0; stageRow < m_stages->rowCount(); ++stageRow) {
            const QTableWidgetItem *name = m_stages->item(stageRow, 0);
            if (!name)
                continue;
            combo->addItem(name->text(), name->data(Qt::UserRole));
            const QColor color(stageColor(name->data(Qt::UserRole).toString()));
            if (color.isValid())
                combo->setItemData(combo->count() - 1, color, Qt::DecorationRole);
        }
        int selectedIndex = combo->findData(selected);
        if (selectedIndex < 0)
            selectedIndex = 0;
        combo->setCurrentIndex(selectedIndex);
        combo->setProperty("stageId", combo->currentData().toString());
        combo->setEnabled(!m_readOnly);
        stageColumnWidth = qMax(stageColumnWidth, combo->sizeHint().width() + 6);
    }
    // Bound pathological stage names while leaving enough room for every
    // normal label, its color swatch, and the native drop-down affordance.
    m_leads->setColumnWidth(4, qMin(stageColumnWidth, 320));
    m_leads->resizeRowsToContents();
}

int PipelineEditor::leadRowForWidget(const QWidget *widget) const
{
    for (int row = 0; row < m_leads->rowCount(); ++row) {
        if (m_leads->cellWidget(row, 4) == widget)
            return row;
    }
    return -1;
}

void PipelineEditor::updateLeadTimestamp(int row)
{
    if (QTableWidgetItem *name = m_leads->item(row, 0))
        name->setData(Qt::UserRole + 2,
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
}

void PipelineEditor::appendHistory(int leadRow, const QString &fromStage,
                                   const QString &toStage, const QString &action)
{
    const QTableWidgetItem *lead = m_leads->item(leadRow, 0);
    if (!lead)
        return;
    m_history.append(QJsonObject{
        {QStringLiteral("at"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("leadId"), lead->data(Qt::UserRole).toString()},
        {QStringLiteral("lead"), lead->text().trimmed()},
        {QStringLiteral("from"), fromStage},
        {QStringLiteral("to"), toStage},
        {QStringLiteral("by"), m_actor},
        {QStringLiteral("action"), action}});
}

void PipelineEditor::addLead()
{
    if (m_readOnly)
        return;
    const int row = m_leads->rowCount();
    m_loading = true;
    m_leads->insertRow(row);
    QTableWidgetItem *name = cell(QStringLiteral("New lead"));
    name->setData(Qt::UserRole,
                  QUuid::createUuid().toString(QUuid::WithoutBraces));
    name->setData(Qt::UserRole + 2,
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    m_leads->setItem(row, 0, name);
    m_leads->setItem(row, 1, cell());
    m_leads->setItem(row, 2, cell(QStringLiteral("0.00")));
    m_leads->setItem(row, 3, cell(m_actor));
    auto *stage = new QComboBox(m_leads);
    stage->setProperty("stageId", firstStageId());
    m_leads->setCellWidget(row, 4, stage);
    m_loading = false;
    refreshStageCombos();
    appendHistory(row, QString(), firstStageId(), QStringLiteral("created"));
    m_leads->selectRow(row);
    m_leads->editItem(name);
    markModified();
    refreshReports();
}

void PipelineEditor::deleteSelectedLeads()
{
    if (m_readOnly)
        return;
    QSet<int> selected;
    for (const QModelIndex &index : m_leads->selectionModel()->selectedRows())
        selected.insert(index.row());
    if (selected.isEmpty() && m_leads->currentRow() >= 0)
        selected.insert(m_leads->currentRow());
    QList<int> rows = selected.values();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        const auto *combo = qobject_cast<QComboBox *>(m_leads->cellWidget(row, 4));
        appendHistory(row, combo ? combo->currentData().toString() : QString(),
                      QString(), QStringLiteral("deleted"));
        m_leads->removeRow(row);
    }
    if (!rows.isEmpty())
        markModified();
    refreshReports();
}

void PipelineEditor::addStage()
{
    if (m_readOnly)
        return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New pipeline stage"),
                              QStringLiteral("Stage name:"), QLineEdit::Normal,
                              QStringLiteral("Follow up"), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    static const QStringList colors{QStringLiteral("#58a6ff"),
                                    QStringLiteral("#a371f7"),
                                    QStringLiteral("#d29922"),
                                    QStringLiteral("#3fb950"),
                                    QStringLiteral("#db6d28")};
    const int row = m_stages->rowCount();
    m_loading = true;
    m_stages->insertRow(row);
    QTableWidgetItem *stageNameItem = cell(name);
    stageNameItem->setData(Qt::UserRole,
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
    QTableWidgetItem *color = cell(colors.at(row % colors.size()));
    color->setBackground(QColor(color->text()));
    m_stages->setItem(row, 0, stageNameItem);
    m_stages->setItem(row, 1, color);
    m_loading = false;
    refreshStageCombos();
    m_stages->selectRow(row);
    m_stages->editItem(stageNameItem);
    markModified();
    refreshReports();
}

void PipelineEditor::deleteSelectedStage()
{
    if (m_readOnly || m_stages->rowCount() <= 1)
        return;
    const int row = m_stages->currentRow();
    if (row < 0)
        return;
    const QTableWidgetItem *item = m_stages->item(row, 0);
    const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    const QString label = item ? item->text() : QStringLiteral("this stage");
    if (QMessageBox::question(
            this, QStringLiteral("Delete pipeline stage"),
            QStringLiteral("Delete \"%1\"? Leads in it will move to another stage.")
                .arg(label),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString fallback =
        row == 0 ? m_stages->item(1, 0)->data(Qt::UserRole).toString()
                 : m_stages->item(0, 0)->data(Qt::UserRole).toString();
    for (int leadRow = 0; leadRow < m_leads->rowCount(); ++leadRow) {
        auto *combo = qobject_cast<QComboBox *>(m_leads->cellWidget(leadRow, 4));
        if (!combo || combo->currentData().toString() != id)
            continue;
        appendHistory(leadRow, id, fallback, QStringLiteral("moved"));
        combo->setProperty("stageId", fallback);
        updateLeadTimestamp(leadRow);
    }
    m_stages->removeRow(row);
    refreshStageCombos();
    markModified();
    refreshReports();
}

double PipelineEditor::leadValue(int row) const
{
    bool ok = false;
    const QTableWidgetItem *item = m_leads->item(row, 2);
    const double value = item ? normalizedMoney(item->text()).toDouble(&ok) : 0.0;
    return ok ? value : 0.0;
}

void PipelineEditor::refreshReports()
{
    QHash<QString, int> counts;
    QHash<QString, double> values;
    double totalValue = 0.0;
    for (int row = 0; row < m_leads->rowCount(); ++row) {
        const auto *combo = qobject_cast<QComboBox *>(m_leads->cellWidget(row, 4));
        const QString stage = combo ? combo->currentData().toString() : QString();
        const double value = leadValue(row);
        ++counts[stage];
        values[stage] += value;
        totalValue += value;
    }

    m_summary->setRowCount(m_stages->rowCount());
    for (int row = 0; row < m_stages->rowCount(); ++row) {
        const QTableWidgetItem *stage = m_stages->item(row, 0);
        const QString id = stage ? stage->data(Qt::UserRole).toString() : QString();
        const int count = counts.value(id);
        QTableWidgetItem *name = cell(stage ? stage->text() : QString());
        const QColor color(stageColor(id));
        if (color.isValid())
            name->setData(Qt::DecorationRole, color);
        m_summary->setItem(row, 0, name);
        m_summary->setItem(row, 1, cell(QString::number(count)));
        m_summary->setItem(
            row, 2, cell(QStringLiteral("$%1").arg(values.value(id), 0, 'f', 2)));
        const double share = m_leads->rowCount() > 0
                                 ? (100.0 * count / m_leads->rowCount())
                                 : 0.0;
        m_summary->setItem(
            row, 3, cell(QStringLiteral("%1%").arg(share, 0, 'f', 0)));
    }

    m_historyTable->setRowCount(m_history.size());
    for (int displayRow = 0; displayRow < m_history.size(); ++displayRow) {
        const QJsonObject event =
            m_history.at(m_history.size() - 1 - displayRow).toObject();
        const QDateTime at = QDateTime::fromString(
            event.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs);
        const QString action = event.value(QStringLiteral("action")).toString();
        QString from = stageName(event.value(QStringLiteral("from")).toString());
        QString to = stageName(event.value(QStringLiteral("to")).toString());
        if (action == QLatin1String("created"))
            from = QStringLiteral("Created");
        else if (action == QLatin1String("deleted"))
            to = QStringLiteral("Deleted");
        m_historyTable->setItem(
            displayRow, 0,
            cell(at.isValid() ? at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                              : event.value(QStringLiteral("at")).toString()));
        m_historyTable->setItem(
            displayRow, 1, cell(event.value(QStringLiteral("lead")).toString()));
        m_historyTable->setItem(displayRow, 2, cell(from));
        m_historyTable->setItem(displayRow, 3, cell(to));
        m_historyTable->setItem(
            displayRow, 4, cell(event.value(QStringLiteral("by")).toString()));
    }
    const int leads = m_leads->rowCount();
    m_reportHeadline->setText(
        QStringLiteral("%1 lead%2 · $%3 total · %4 history event%5")
            .arg(leads)
            .arg(leads == 1 ? QString() : QStringLiteral("s"))
            .arg(totalValue, 0, 'f', 2)
            .arg(m_history.size())
            .arg(m_history.size() == 1 ? QString() : QStringLiteral("s")));
    m_status->setText(
        QStringLiteral("%1 stage%2 · %3 lead%4%5")
            .arg(m_stages->rowCount())
            .arg(m_stages->rowCount() == 1 ? QString() : QStringLiteral("s"))
            .arg(leads)
            .arg(leads == 1 ? QString() : QStringLiteral("s"))
            .arg(m_readOnly ? QStringLiteral(" · read only") : QString()));
}

} // namespace forkmesh::cove
