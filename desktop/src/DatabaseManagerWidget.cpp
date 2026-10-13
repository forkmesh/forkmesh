#include "DatabaseManagerWidget.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace forkmesh {

namespace {

constexpr int kCellPreviewChars = 160;
constexpr int kTooltipChars = 4000;
constexpr int kExportMaxPages = 2000;

QString elided(const QString &text, int limit)
{
    if (text.size() <= limit)
        return text;
    return text.left(limit) + QString::fromUtf8("\xE2\x80\xA6");
}

QString storageLabel(const QString &storage)
{
    if (storage == QLatin1String("sealed"))
        return QStringLiteral("Sealed blob");
    if (storage == QLatin1String("sealed+indexed"))
        return QStringLiteral("Sealed + blind index");
    if (storage == QLatin1String("indexed"))
        return QStringLiteral("Blind index + plaintext");
    if (storage == QLatin1String("plaintext"))
        return QStringLiteral("Plaintext");
    return storage;
}

QString errorText(int status, const QJsonObject &body)
{
    QString error = body.value(QStringLiteral("error")).toString();
    const QString detail = body.value(QStringLiteral("detail")).toString();
    if (error == QLatin1String("unauthorized"))
        error = QStringLiteral("The relay did not accept this administrator "
                               "signature");
    if (error.isEmpty())
        error = status > 0 ? QStringLiteral("HTTP %1").arg(status)
                           : QStringLiteral("The relay is unreachable");
    return detail.isEmpty() ? error
                            : error + QStringLiteral(": ") + detail;
}

QPushButton *toolButton(const DatabaseManagerHooks &hooks, const QString &text,
                        const QString &icon, const QString &tooltip)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tooltip);
    if (hooks.decorateButton)
        hooks.decorateButton(button, icon);
    return button;
}

// One cell's display text and, when it was cut, the full text for a tooltip.
QPair<QString, QString> cellText(const QJsonObject &row, const QString &column)
{
    if (column == QLatin1String("data") &&
        row.contains(QStringLiteral("dataMode"))) {
        const QString mode = row.value(QStringLiteral("dataMode")).toString();
        const QJsonValue data = row.value(QStringLiteral("data"));
        QString full = data.isUndefined()
                           ? QStringLiteral("(cannot be decoded)")
                           : d1DatabaseCellText(data);
        const QString prefix = QStringLiteral("[%1] ").arg(mode);
        QString tooltip;
        if (!data.isUndefined()) {
            const QJsonDocument pretty =
                data.isArray() ? QJsonDocument(data.toArray())
                               : QJsonDocument(data.toObject());
            tooltip = elided(QString::fromUtf8(pretty.toJson()), kTooltipChars);
        }
        return {prefix + elided(full, kCellPreviewChars), tooltip};
    }
    const QJsonValue value = row.value(QStringLiteral("values"))
                                 .toObject()
                                 .value(column);
    const QString full = d1DatabaseCellText(value);
    return {elided(full, kCellPreviewChars),
            full.size() > kCellPreviewChars ? elided(full, kTooltipChars)
                                            : QString()};
}

} // namespace

DatabaseManagerWidget::DatabaseManagerWidget(DatabaseManagerHooks hooks,
                                             QWidget *parent)
    : QWidget(parent), m_hooks(std::move(hooks))
{
    setObjectName(QStringLiteral("logDatabasePage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    m_summary = new QLabel(QStringLiteral(
        "Every D1 table with its rows, storage model and what the D1 audit "
        "(#682) found. Rows outside the restricted tables can be created, "
        "edited, deleted and exported; each write is signed, single-use and "
        "recorded in the sensitive audit log."));
    m_summary->setObjectName(QStringLiteral("logFrequencySummary"));
    m_summary->setWordWrap(true);
    header->addWidget(m_summary, 1);
    m_cleanupButton = toolButton(
        m_hooks, QStringLiteral("Cleanup plan"), QStringLiteral("checklist"),
        QStringLiteral("The audit's drop, custody, catch-up migration and "
                       "encryption findings against the live tables"));
    m_refreshButton = toolButton(
        m_hooks, QStringLiteral("Refresh"), QStringLiteral("sync"),
        QStringLiteral("Re-read the table list and row counts"));
    header->addWidget(m_cleanupButton);
    header->addWidget(m_refreshButton);
    layout->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    m_tableSearch = new QLineEdit;
    m_tableSearch->setPlaceholderText(QStringLiteral("Filter tables"));
    m_tableSearch->setClearButtonEnabled(true);
    m_tableView = new QComboBox;
    m_tableView->addItem(QStringLiteral("All tables"), QStringLiteral("all"));
    m_tableView->addItem(QStringLiteral("Audit findings"),
                         QStringLiteral("findings"));
    m_tableView->addItem(QStringLiteral("Cleanup candidates"),
                         QStringLiteral("cleanup"));
    m_tableView->addItem(QStringLiteral("Encryption gaps"),
                         QStringLiteral("gaps"));
    m_tableView->addItem(QStringLiteral("Editable"),
                         QStringLiteral("writable"));
    m_tableView->addItem(QStringLiteral("Restricted"),
                         QStringLiteral("restricted"));
    m_tableList = new QTreeWidget;
    m_tableList->setObjectName(QStringLiteral("logDatabaseTables"));
    m_tableList->setColumnCount(4);
    m_tableList->setHeaderLabels({QStringLiteral("Table"),
                                  QStringLiteral("Rows"),
                                  QStringLiteral("Storage"),
                                  QStringLiteral("Audit")});
    m_tableList->setRootIsDecorated(false);
    m_tableList->setUniformRowHeights(true);
    m_tableList->setSortingEnabled(true);
    m_tableList->sortByColumn(0, Qt::AscendingOrder);
    m_tableList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    leftLayout->addWidget(m_tableSearch);
    leftLayout->addWidget(m_tableView);
    leftLayout->addWidget(m_tableList, 1);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);
    m_tableTitle = new QLabel;
    m_tableTitle->setTextFormat(Qt::RichText);
    m_tableDetails = new QLabel;
    m_tableDetails->setObjectName(QStringLiteral("logFrequencySummary"));
    m_tableDetails->setTextFormat(Qt::RichText);
    m_tableDetails->setWordWrap(true);
    m_tableDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    m_filterColumn = new QComboBox;
    m_filterColumn->setMinimumContentsLength(12);
    m_filterMode = new QComboBox;
    m_filterMode->addItem(QStringLiteral("equals"), QStringLiteral("equals"));
    m_filterMode->addItem(QStringLiteral("contains"),
                          QStringLiteral("contains"));
    m_filterMode->addItem(QStringLiteral("blind index of"),
                          QStringLiteral("blind"));
    m_filterMode->addItem(QStringLiteral("is NULL"), QStringLiteral("null"));
    m_filterMode->setToolTip(QStringLiteral(
        "\"blind index of\" hashes the value with the relay's key, so a "
        "*_bi column can be searched by the name or email it indexes"));
    m_filterValue = new QLineEdit;
    m_filterValue->setPlaceholderText(QStringLiteral("Value"));
    m_filterValue->setClearButtonEnabled(true);
    auto *applyFilter =
        toolButton(m_hooks, QStringLiteral("Apply"), QStringLiteral("filter"),
                   QStringLiteral("Show only rows that match"));
    auto *clearFilter =
        toolButton(m_hooks, QStringLiteral("Clear"), QStringLiteral("x"),
                   QStringLiteral("Show every row"));
    m_oldestFirst = new QCheckBox(QStringLiteral("Oldest first"));
    filterRow->addWidget(m_filterColumn);
    filterRow->addWidget(m_filterMode);
    filterRow->addWidget(m_filterValue, 1);
    filterRow->addWidget(applyFilter);
    filterRow->addWidget(clearFilter);
    filterRow->addWidget(m_oldestFirst);

    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    m_newButton = toolButton(m_hooks, QStringLiteral("New row"),
                             QStringLiteral("plus"),
                             QStringLiteral("Insert a row into this table"));
    m_editButton = toolButton(m_hooks, QStringLiteral("Edit"),
                              QStringLiteral("pencil"),
                              QStringLiteral("Edit the selected row"));
    m_deleteButton = toolButton(m_hooks, QStringLiteral("Delete"),
                                QStringLiteral("trash"),
                                QStringLiteral("Delete the selected rows"));
    m_exportButton = toolButton(
        m_hooks, QStringLiteral("Export"), QStringLiteral("download"),
        QStringLiteral("Save every matching row, decrypted, as JSON Lines"));
    m_schemaButton = toolButton(
        m_hooks, QStringLiteral("Schema"), QStringLiteral("code"),
        QStringLiteral("The table's CREATE statement, indexes and triggers"));
    m_previousButton = toolButton(m_hooks, QString(),
                                  QStringLiteral("chevron-left"),
                                  QStringLiteral("Previous page"));
    m_nextButton = toolButton(m_hooks, QString(),
                              QStringLiteral("chevron-right"),
                              QStringLiteral("Next page"));
    actionRow->addWidget(m_newButton);
    actionRow->addWidget(m_editButton);
    actionRow->addWidget(m_deleteButton);
    actionRow->addWidget(m_exportButton);
    actionRow->addWidget(m_schemaButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_previousButton);
    actionRow->addWidget(m_nextButton);

    m_rowsView = new QTableWidget;
    m_rowsView->setObjectName(QStringLiteral("logDatabaseRows"));
    m_rowsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rowsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rowsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_rowsView->setWordWrap(false);
    m_rowsView->verticalHeader()->setVisible(false);
    m_rowsView->horizontalHeader()->setStretchLastSection(true);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("logFrequencySummary"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    rightLayout->addWidget(m_tableTitle);
    rightLayout->addWidget(m_tableDetails);
    rightLayout->addLayout(filterRow);
    rightLayout->addLayout(actionRow);
    rightLayout->addWidget(m_rowsView, 1);
    rightLayout->addWidget(m_status);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 900});
    layout->addWidget(splitter, 1);

    connect(m_refreshButton, &QPushButton::clicked, this,
            [this] { loadInventory(true); });
    connect(m_cleanupButton, &QPushButton::clicked, this,
            [this] { showCleanupPlan(); });
    connect(m_tableSearch, &QLineEdit::textChanged, this,
            [this] { rebuildTableList(); });
    connect(m_tableView, &QComboBox::currentIndexChanged, this,
            [this] { rebuildTableList(); });
    connect(m_tableList, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item) {
                if (!item)
                    return;
                const QString name = item->data(0, Qt::UserRole).toString();
                if (name != m_table)
                    selectTable(name);
            });
    const auto apply = [this] {
        m_appliedFilterColumn = m_filterColumn->currentText();
        m_appliedFilterMode = m_filterMode->currentData().toString();
        m_appliedFilterValue = m_filterValue->text();
        resetPaging();
        loadPage();
    };
    connect(applyFilter, &QPushButton::clicked, this, apply);
    connect(m_filterValue, &QLineEdit::returnPressed, this, apply);
    connect(clearFilter, &QPushButton::clicked, this, [this] {
        m_filterValue->clear();
        m_appliedFilterColumn.clear();
        m_appliedFilterMode.clear();
        m_appliedFilterValue.clear();
        resetPaging();
        loadPage();
    });
    connect(m_oldestFirst, &QCheckBox::toggled, this, [this] {
        resetPaging();
        loadPage();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        if (m_nextCursor.isEmpty())
            return;
        m_pageCursors.append(m_nextCursor);
        loadPage();
    });
    connect(m_previousButton, &QPushButton::clicked, this, [this] {
        if (m_pageCursors.size() <= 1)
            return;
        m_pageCursors.removeLast();
        loadPage();
    });
    connect(m_newButton, &QPushButton::clicked, this, [this] { editRow(-1); });
    connect(m_editButton, &QPushButton::clicked, this, [this] {
        const QList<int> rows = selectedRowIndexes();
        if (rows.size() == 1)
            editRow(rows.first());
    });
    connect(m_rowsView, &QTableWidget::cellDoubleClicked, this,
            [this](int row) {
                const QTableWidgetItem *item = m_rowsView->item(row, 0);
                if (item)
                    editRow(item->data(Qt::UserRole).toInt());
            });
    connect(m_rowsView, &QTableWidget::itemSelectionChanged, this,
            [this] { updateActions(); });
    connect(m_deleteButton, &QPushButton::clicked, this,
            [this] { deleteSelectedRows(); });
    connect(m_exportButton, &QPushButton::clicked, this,
            [this] { exportTable(); });
    connect(m_schemaButton, &QPushButton::clicked, this,
            [this] { showSchema(); });

    renderDetails();
    updateActions();
}

void DatabaseManagerWidget::activate()
{
    if (m_activated)
        return;
    m_activated = true;
    loadInventory(false);
}

void DatabaseManagerWidget::setStatus(const QString &text, bool error)
{
    m_status->setText(error ? QStringLiteral("<span style='color:#cf222e'>%1"
                                             "</span>")
                                  .arg(text.toHtmlEscaped())
                            : text.toHtmlEscaped());
}

void DatabaseManagerWidget::read(const D1DatabaseReadRequest &request,
                                 bool refresh, QObject *context, Reply done)
{
    QString reason;
    const QString node =
        m_hooks.administrator ? m_hooks.administrator(&reason) : QString();
    QNetworkAccessManager *network = m_hooks.network ? m_hooks.network() : nullptr;
    if (node.isEmpty() || !network) {
        done(0, QJsonObject{{QStringLiteral("error"),
                             reason.isEmpty()
                                 ? QStringLiteral("This desktop cannot sign "
                                                  "administrator requests")
                                 : reason}});
        return;
    }
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString signature =
        m_hooks.sign(d1DatabaseReadCanonical(node, ts, request));
    QUrl url = m_hooks.endpoint();
    url.setQuery(QString::fromLatin1(d1DatabaseReadQueryString(
                     request, node, ts, signature, refresh)),
                 QUrl::StrictMode);
    QNetworkRequest networkRequest(url);
    networkRequest.setTransferTimeout(60'000);
    QNetworkReply *reply = network->get(networkRequest);
    connect(reply, &QNetworkReply::finished, context,
            [reply, done = std::move(done)] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject body =
                    QJsonDocument::fromJson(reply->readAll()).object();
                done(status, body);
            });
    // A reply whose context died never reaches the lambda above.
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void DatabaseManagerWidget::write(const QJsonObject &payload, QObject *context,
                                  Reply done)
{
    QString reason;
    const QString node =
        m_hooks.administrator ? m_hooks.administrator(&reason) : QString();
    QNetworkAccessManager *network = m_hooks.network ? m_hooks.network() : nullptr;
    if (node.isEmpty() || !network) {
        done(0, QJsonObject{{QStringLiteral("error"), reason}});
        return;
    }
    const QByteArray payloadBytes =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString requestId = newD1DatabaseRequestId();
    const QJsonObject body{
        {QStringLiteral("node"), node},
        {QStringLiteral("ts"), ts},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("payload"), QString::fromUtf8(payloadBytes)},
        {QStringLiteral("sig"),
         m_hooks.sign(
             d1DatabaseWriteCanonical(node, ts, requestId, payloadBytes))},
    };
    QNetworkRequest networkRequest(m_hooks.endpoint());
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    networkRequest.setTransferTimeout(60'000);
    QNetworkReply *reply = network->post(
        networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, context,
            [reply, done = std::move(done)] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject response =
                    QJsonDocument::fromJson(reply->readAll()).object();
                done(status, response);
            });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void DatabaseManagerWidget::loadInventory(bool refresh)
{
    if (m_inventoryInFlight)
        return;
    m_inventoryInFlight = true;
    m_refreshButton->setEnabled(false);
    setStatus(QStringLiteral("Reading the table list%1")
                  .arg(QString::fromUtf8("\xE2\x80\xA6")));
    D1DatabaseReadRequest request;
    request.op = QStringLiteral("inventory");
    read(request, refresh, this, [this](int status, const QJsonObject &body) {
        m_inventoryInFlight = false;
        m_refreshButton->setEnabled(true);
        if (status != 200 || !body.value(QStringLiteral("ok")).toBool()) {
            setStatus(errorText(status, body), true);
            return;
        }
        m_inventory = body;
        m_tables = parseD1DatabaseInventory(body);
        rebuildTableList();
        qint64 rows = 0;
        int findings = 0;
        int gaps = 0;
        for (const D1DatabaseTable &table : m_tables) {
            rows += table.rows;
            findings += table.auditCategory.isEmpty() ? 0 : 1;
            gaps += table.gaps.isEmpty() ? 0 : 1;
        }
        setStatus(QStringLiteral("%1 tables, %2 rows. %3 carry an audit "
                                 "finding; %4 have a live encryption gap.")
                      .arg(m_tables.size())
                      .arg(rows)
                      .arg(findings)
                      .arg(gaps));
        if (!m_table.isEmpty())
            selectTable(m_table);
    });
}

void DatabaseManagerWidget::rebuildTableList()
{
    const QString search = m_tableSearch->text().trimmed();
    const QString view = m_tableView->currentData().toString();
    const QSignalBlocker blocker(m_tableList);
    m_tableList->setSortingEnabled(false);
    m_tableList->clear();
    QTreeWidgetItem *selected = nullptr;
    const QBrush muted = palette().brush(QPalette::Disabled, QPalette::Text);
    for (const D1DatabaseTable &table : m_tables) {
        if (!search.isEmpty() &&
            !table.name.contains(search, Qt::CaseInsensitive))
            continue;
        const bool keep =
            view == QLatin1String("findings")
                ? !table.auditCategory.isEmpty() || !table.gaps.isEmpty()
            : view == QLatin1String("cleanup")
                ? d1DatabaseAuditIsDropCandidate(table.auditCategory) ||
                      table.auditCategory == QLatin1String("custody")
            : view == QLatin1String("gaps")     ? !table.gaps.isEmpty()
            : view == QLatin1String("writable") ? table.writable
            : view == QLatin1String("restricted") ? table.restricted
                                                  : true;
        if (!keep)
            continue;
        auto *item = new QTreeWidgetItem;
        item->setData(0, Qt::UserRole, table.name);
        item->setText(0, table.name);
        item->setData(1, Qt::DisplayRole, table.rows);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(2, storageLabel(table.storage));
        QString audit = d1DatabaseAuditCategoryLabel(table.auditCategory);
        if (!table.gaps.isEmpty())
            audit = audit.isEmpty() ? QStringLiteral("Encryption gap")
                                    : audit + QStringLiteral(" + gap");
        item->setText(3, audit);
        QStringList tooltip;
        if (table.restricted)
            tooltip << QStringLiteral("Restricted: rows are served only by "
                                      "this table's own API.");
        if (!table.auditNote.isEmpty())
            tooltip << table.auditNote;
        tooltip << table.gaps;
        item->setToolTip(0, tooltip.join(QLatin1Char('\n')));
        item->setToolTip(3, tooltip.join(QLatin1Char('\n')));
        if (table.restricted)
            for (int column = 0; column < 4; ++column)
                item->setForeground(column, muted);
        m_tableList->addTopLevelItem(item);
        if (table.name == m_table)
            selected = item;
    }
    m_tableList->setSortingEnabled(true);
    if (selected)
        m_tableList->setCurrentItem(selected);
}

const D1DatabaseTable *DatabaseManagerWidget::currentTable() const
{
    for (const D1DatabaseTable &table : m_tables)
        if (table.name == m_table)
            return &table;
    return nullptr;
}

void DatabaseManagerWidget::selectTable(const QString &name)
{
    const bool sameTable = name == m_table;
    m_table = name;
    const D1DatabaseTable *table = currentTable();
    m_columns = table ? table->columns : QList<D1DatabaseColumn>();
    m_tableHasRowid = table ? table->rowid : true;
    if (!sameTable) {
        m_appliedFilterColumn.clear();
        m_appliedFilterMode.clear();
        m_appliedFilterValue.clear();
        m_filterValue->clear();
    }
    const QSignalBlocker blocker(m_filterColumn);
    m_filterColumn->clear();
    if (m_tableHasRowid)
        m_filterColumn->addItem(QStringLiteral("rowid"));
    for (const D1DatabaseColumn &column : m_columns)
        m_filterColumn->addItem(column.name);
    if (!m_appliedFilterColumn.isEmpty())
        m_filterColumn->setCurrentText(m_appliedFilterColumn);
    m_rows = QJsonArray();
    m_nextCursor.clear();
    resetPaging();
    renderRows();
    renderDetails();
    if (table && table->restricted) {
        setStatus(QStringLiteral("Restricted: the relay serves this table's "
                                 "rows only through its own audited API."));
        updateActions();
        return;
    }
    loadPage();
}

void DatabaseManagerWidget::resetPaging()
{
    m_pageCursors = {QString()};
    m_nextCursor.clear();
}

D1DatabaseReadRequest
DatabaseManagerWidget::pageRequest(const QString &cursor) const
{
    D1DatabaseReadRequest request;
    request.op = QStringLiteral("rows");
    request.table = m_table;
    request.cursor = cursor;
    request.filterColumn = m_appliedFilterColumn;
    request.filterMode = m_appliedFilterMode;
    request.filterValue = m_appliedFilterValue;
    if (m_appliedFilterMode.isEmpty() && m_appliedFilterValue.isEmpty())
        request.filterColumn.clear();
    request.order = m_oldestFirst->isChecked() ? QStringLiteral("asc")
                                               : QString();
    return request;
}

void DatabaseManagerWidget::loadPage()
{
    if (m_table.isEmpty())
        return;
    const quint64 generation = ++m_pageGeneration;
    setStatus(QStringLiteral("Reading %1%2")
                  .arg(m_table, QString::fromUtf8("\xE2\x80\xA6")));
    read(pageRequest(m_pageCursors.isEmpty() ? QString()
                                             : m_pageCursors.last()),
         false, this,
         [this, generation](int status, const QJsonObject &body) {
             if (generation != m_pageGeneration)
                 return;
             if (status != 200 || !body.value(QStringLiteral("ok")).toBool()) {
                 m_rows = QJsonArray();
                 renderRows();
                 updateActions();
                 setStatus(errorText(status, body), true);
                 return;
             }
             m_rows = body.value(QStringLiteral("rows")).toArray();
             m_columns = parseD1DatabaseColumns(
                 body.value(QStringLiteral("columns")).toArray());
             m_tableHasRowid = body.value(QStringLiteral("rowid")).toBool(true);
             m_nextCursor = body.value(QStringLiteral("nextCursor")).toString();
             renderRows();
             updateActions();
             setStatus(QStringLiteral("Page %1: %2 row%3%4")
                           .arg(m_pageCursors.size())
                           .arg(m_rows.size())
                           .arg(m_rows.size() == 1 ? QString()
                                                   : QStringLiteral("s"))
                           .arg(m_nextCursor.isEmpty()
                                    ? QString()
                                    : QStringLiteral(", more on the next page")));
         });
}

void DatabaseManagerWidget::renderRows()
{
    const QSignalBlocker blocker(m_rowsView);
    m_rowsView->clear();
    QStringList headers;
    if (m_tableHasRowid)
        headers << QStringLiteral("rowid");
    for (const D1DatabaseColumn &column : m_columns)
        headers << column.name;
    m_rowsView->setColumnCount(headers.size());
    m_rowsView->setHorizontalHeaderLabels(headers);
    m_rowsView->setRowCount(m_rows.size());
    for (int index = 0; index < m_rows.size(); ++index) {
        const QJsonObject row = m_rows.at(index).toObject();
        int column = 0;
        if (m_tableHasRowid) {
            auto *item = new QTableWidgetItem(
                d1DatabaseCellText(row.value(QStringLiteral("rowid"))));
            m_rowsView->setItem(index, column++, item);
        }
        for (const D1DatabaseColumn &info : m_columns) {
            const auto [text, tooltip] = cellText(row, info.name);
            auto *item = new QTableWidgetItem(text);
            if (!tooltip.isEmpty())
                item->setToolTip(tooltip);
            m_rowsView->setItem(index, column++, item);
        }
        if (QTableWidgetItem *first = m_rowsView->item(index, 0))
            first->setData(Qt::UserRole, index);
    }
    m_rowsView->resizeColumnsToContents();
    for (int column = 0; column < m_rowsView->columnCount(); ++column)
        m_rowsView->setColumnWidth(
            column, qMin(m_rowsView->columnWidth(column), 320));
}

void DatabaseManagerWidget::renderDetails()
{
    const D1DatabaseTable *table = currentTable();
    if (!table) {
        m_tableTitle->setText(QStringLiteral("<b>Select a table</b>"));
        m_tableDetails->setText(QString());
        return;
    }
    m_tableTitle->setText(
        QStringLiteral("<b>%1</b> &nbsp;%2 rows &middot; %3")
            .arg(table->name.toHtmlEscaped())
            .arg(table->rows)
            .arg(storageLabel(table->storage).toHtmlEscaped()));
    QStringList lines;
    if (!table->auditCategory.isEmpty())
        lines << QStringLiteral("<b>Audit:</b> %1 &mdash; %2")
                     .arg(d1DatabaseAuditCategoryLabel(table->auditCategory)
                              .toHtmlEscaped(),
                          table->auditNote.toHtmlEscaped());
    for (const QString &gap : table->gaps)
        lines << QStringLiteral("<b>Encryption gap:</b> %1")
                     .arg(gap.toHtmlEscaped());
    if (!table->writable && !table->writeBlockedReason.isEmpty())
        lines << QStringLiteral("<b>Read-only:</b> %1")
                     .arg(table->writeBlockedReason.toHtmlEscaped());
    const QJsonArray queries = d1DatabaseTableQueryInsights(
        m_hooks.d1UsageReport ? m_hooks.d1UsageReport() : QJsonObject(),
        table->name);
    for (int i = 0; i < queries.size() && i < 3; ++i) {
        const QJsonObject query = queries.at(i).toObject();
        lines << QStringLiteral("<b>Hot query today:</b> %1 runs, %2 rows "
                                "read &mdash; <code>%3</code>")
                     .arg(static_cast<qint64>(
                         query.value(QStringLiteral("runs")).toDouble()))
                     .arg(static_cast<qint64>(
                         query.value(QStringLiteral("totalRowsRead"))
                             .toDouble()))
                     .arg(elided(query.value(QStringLiteral("query"))
                                     .toString(),
                                 140)
                              .toHtmlEscaped());
    }
    m_tableDetails->setText(lines.join(QStringLiteral("<br>")));
}

void DatabaseManagerWidget::updateActions()
{
    const D1DatabaseTable *table = currentTable();
    const bool readable = table && !table->restricted;
    const bool writable = table && table->writable && m_tableHasRowid;
    const int selected = selectedRowIndexes().size();
    m_newButton->setEnabled(writable);
    m_editButton->setEnabled(writable && selected == 1);
    m_deleteButton->setEnabled(writable && selected > 0);
    m_exportButton->setEnabled(readable && !m_exporting);
    m_schemaButton->setEnabled(table != nullptr);
    m_previousButton->setEnabled(readable && m_pageCursors.size() > 1);
    m_nextButton->setEnabled(readable && !m_nextCursor.isEmpty());
    const QString reason = table && !table->writable
                               ? table->writeBlockedReason
                               : QString();
    m_newButton->setToolTip(
        reason.isEmpty() ? QStringLiteral("Insert a row into this table")
                         : reason);
    m_editButton->setToolTip(
        reason.isEmpty() ? QStringLiteral("Edit the selected row") : reason);
    m_deleteButton->setToolTip(
        reason.isEmpty() ? QStringLiteral("Delete the selected rows") : reason);
}

QList<int> DatabaseManagerWidget::selectedRowIndexes() const
{
    QList<int> indexes;
    const QList<QTableWidgetSelectionRange> ranges =
        m_rowsView->selectedRanges();
    for (const QTableWidgetSelectionRange &range : ranges)
        for (int row = range.topRow(); row <= range.bottomRow(); ++row)
            if (const QTableWidgetItem *item = m_rowsView->item(row, 0))
                indexes.append(item->data(Qt::UserRole).toInt());
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

void DatabaseManagerWidget::editRow(int rowIndex)
{
    const D1DatabaseTable *table = currentTable();
    if (!table || !table->writable || !m_tableHasRowid)
        return;
    const bool inserting = rowIndex < 0;
    const QJsonObject row =
        inserting ? QJsonObject() : m_rows.at(rowIndex).toObject();
    const QJsonObject values = row.value(QStringLiteral("values")).toObject();

    QDialog dialog(this);
    dialog.setWindowTitle(
        inserting ? QStringLiteral("New row in %1").arg(m_table)
                  : QStringLiteral("Edit %1 row %2")
                        .arg(m_table,
                             d1DatabaseCellText(
                                 row.value(QStringLiteral("rowid")))));
    dialog.resize(640, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *formHost = new QWidget;
    auto *form = new QFormLayout(formHost);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    scroll->setWidget(formHost);
    layout->addWidget(scroll, 1);

    struct Field {
        D1DatabaseColumn column;
        QLineEdit *edit = nullptr;
        QCheckBox *unset = nullptr;
        QPlainTextEdit *json = nullptr;
        QString original;
        bool originalNull = false;
    };
    auto fields = std::make_shared<QList<Field>>();
    for (const D1DatabaseColumn &column : m_columns) {
        Field field;
        field.column = column;
        const QString label =
            column.name +
            (column.type.isEmpty() ? QString()
                                   : QStringLiteral(" (%1)").arg(column.type));
        if (column.name == QLatin1String("data")) {
            field.json = new QPlainTextEdit;
            field.json->setMinimumHeight(180);
            const QString mode = row.value(QStringLiteral("dataMode")).toString();
            const QJsonValue data = row.value(QStringLiteral("data"));
            const bool frozen =
                row.value(QStringLiteral("walletKeys")).toBool() ||
                mode == QLatin1String("unreadable");
            if (data.isObject())
                field.original = QString::fromUtf8(
                    QJsonDocument(data.toObject()).toJson());
            else if (data.isArray())
                field.original = QString::fromUtf8(
                    QJsonDocument(data.toArray()).toJson());
            field.json->setPlainText(field.original);
            field.json->setPlaceholderText(
                inserting ? QStringLiteral("{} (blank leaves data unset)")
                          : QString());
            field.json->setReadOnly(frozen);
            const QString note =
                frozen ? QStringLiteral("Read-only: holds legacy wallet "
                                        "custody material or does not decrypt.")
                : table->plaintextData || mode == QLatin1String("plaintext")
                    ? QStringLiteral("Stored as plaintext JSON, like the rest "
                                     "of this table.")
                    : QStringLiteral("Sealed with the relay's data key on "
                                     "save.");
            auto *box = new QWidget;
            auto *boxLayout = new QVBoxLayout(box);
            boxLayout->setContentsMargins(0, 0, 0, 0);
            boxLayout->addWidget(field.json);
            auto *hint = new QLabel(note);
            hint->setObjectName(QStringLiteral("logFrequencySummary"));
            hint->setWordWrap(true);
            boxLayout->addWidget(hint);
            form->addRow(label, box);
        } else if (column.name.endsWith(QLatin1String("_bi"))) {
            field.edit = new QLineEdit;
            field.edit->setPlaceholderText(
                inserting ? QStringLiteral("Plaintext to blind-index (blank "
                                           "leaves it unset)")
                          : QStringLiteral("Plaintext to re-index (blank keeps "
                                           "the stored index)"));
            field.edit->setToolTip(
                QStringLiteral("Stored: %1").arg(d1DatabaseCellText(
                    values.value(column.name))));
            form->addRow(label, field.edit);
        } else {
            const QJsonValue value = values.value(column.name);
            field.originalNull = !inserting && value.isNull();
            field.original = value.isNull() ? QString()
                                            : d1DatabaseCellText(value);
            field.edit = new QLineEdit(field.original);
            field.unset = new QCheckBox(inserting ? QStringLiteral("default")
                                                  : QStringLiteral("NULL"));
            field.unset->setToolTip(
                inserting ? QStringLiteral("Leave the column out so SQLite "
                                           "applies its default")
                          : QStringLiteral("Store NULL"));
            field.unset->setChecked(
                inserting ? (column.hasDefault || !column.notNull ||
                             column.primaryKey)
                          : field.originalNull);
            field.edit->setEnabled(!field.unset->isChecked());
            QLineEdit *edit = field.edit;
            connect(field.unset, &QCheckBox::toggled, edit,
                    [edit](bool on) { edit->setEnabled(!on); });
            auto *box = new QWidget;
            auto *boxLayout = new QHBoxLayout(box);
            boxLayout->setContentsMargins(0, 0, 0, 0);
            boxLayout->addWidget(field.edit, 1);
            boxLayout->addWidget(field.unset);
            form->addRow(label, box);
        }
        fields->append(field);
    }

    auto *error = new QLabel;
    error->setWordWrap(true);
    error->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#cf222e")));
    layout->addWidget(error);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QPushButton *save = buttons->button(QDialogButtonBox::Save);
    connect(save, &QPushButton::clicked, &dialog, [&, fields, inserting] {
        QJsonObject payload{
            {QStringLiteral("op"), inserting ? QStringLiteral("insert")
                                             : QStringLiteral("update")},
            {QStringLiteral("table"), m_table},
        };
        if (!inserting) {
            payload.insert(QStringLiteral("rowid"),
                           row.value(QStringLiteral("rowid")));
            payload.insert(QStringLiteral("digest"),
                           row.value(QStringLiteral("digest")));
        }
        QJsonObject changed;
        QJsonObject blind;
        for (const Field &field : *fields) {
            if (field.json) {
                const QString text = field.json->toPlainText();
                if (field.json->isReadOnly() || text.trimmed().isEmpty() ||
                    text == field.original)
                    continue;
                QJsonParseError parseError;
                const QJsonDocument document =
                    QJsonDocument::fromJson(text.toUtf8(), &parseError);
                if (parseError.error != QJsonParseError::NoError ||
                    document.isNull()) {
                    error->setText(QStringLiteral("data is not valid JSON: %1")
                                       .arg(parseError.errorString()));
                    return;
                }
                if (!inserting && document ==
                                      QJsonDocument::fromJson(
                                          field.original.toUtf8()))
                    continue;
                payload.insert(QStringLiteral("data"),
                               document.isArray()
                                   ? QJsonValue(document.array())
                                   : QJsonValue(document.object()));
            } else if (field.column.name.endsWith(QLatin1String("_bi"))) {
                if (!field.edit->text().isEmpty())
                    blind.insert(field.column.name, field.edit->text());
            } else if (field.unset->isChecked()) {
                if (!inserting && !field.originalNull)
                    changed.insert(field.column.name, QJsonValue::Null);
            } else if (inserting || field.originalNull ||
                       field.edit->text() != field.original) {
                changed.insert(field.column.name,
                               d1DatabaseEditorValue(field.edit->text(),
                                                     field.column.type));
            }
        }
        if (!changed.isEmpty())
            payload.insert(QStringLiteral("values"), changed);
        if (!blind.isEmpty())
            payload.insert(QStringLiteral("blind"), blind);
        if (!inserting && changed.isEmpty() && blind.isEmpty() &&
            !payload.contains(QStringLiteral("data"))) {
            error->setText(QStringLiteral("Nothing changed."));
            return;
        }
        save->setEnabled(false);
        error->setText(QStringLiteral("Saving%1").arg(
            QString::fromUtf8("\xE2\x80\xA6")));
        QPointer<QDialog> guard(&dialog);
        write(payload, &dialog,
              [guard, save, error](int status, const QJsonObject &body) {
                  if (!guard)
                      return;
                  if (status == 200 && body.value(QStringLiteral("ok")).toBool()) {
                      guard->accept();
                      return;
                  }
                  save->setEnabled(true);
                  error->setText(errorText(status, body));
              });
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    setStatus(inserting ? QStringLiteral("Row inserted into %1.").arg(m_table)
                        : QStringLiteral("Row saved."));
    if (inserting)
        resetPaging();
    loadPage();
}

void DatabaseManagerWidget::deleteSelectedRows()
{
    const D1DatabaseTable *table = currentTable();
    const QList<int> indexes = selectedRowIndexes();
    if (!table || !table->writable || indexes.isEmpty())
        return;
    QJsonArray rowids;
    for (int index : indexes)
        rowids.append(m_rows.at(index).toObject().value(QStringLiteral("rowid")));
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, QStringLiteral("Delete rows"),
        QStringLiteral("Permanently delete %1 row%2 from %3? D1 keeps no undo; "
                       "the sensitive audit log records a digest of what was "
                       "removed, not the rows.")
            .arg(indexes.size())
            .arg(indexes.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(m_table),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    QJsonObject payload{
        {QStringLiteral("op"), QStringLiteral("delete")},
        {QStringLiteral("table"), m_table},
        {QStringLiteral("rowids"), rowids},
    };
    if (indexes.size() == 1)
        payload.insert(QStringLiteral("digest"),
                       m_rows.at(indexes.first())
                           .toObject()
                           .value(QStringLiteral("digest")));
    m_deleteButton->setEnabled(false);
    setStatus(QStringLiteral("Deleting%1").arg(QString::fromUtf8("\xE2\x80\xA6")));
    write(payload, this, [this](int status, const QJsonObject &body) {
        if (status != 200 || !body.value(QStringLiteral("ok")).toBool()) {
            setStatus(errorText(status, body), true);
            updateActions();
            return;
        }
        const qint64 count = static_cast<qint64>(
            body.value(QStringLiteral("rowCount")).toDouble());
        loadPage();
        setStatus(QStringLiteral("Deleted %1 row%2.")
                      .arg(count)
                      .arg(count == 1 ? QString() : QStringLiteral("s")));
    });
}

void DatabaseManagerWidget::exportTable()
{
    const D1DatabaseTable *table = currentTable();
    if (!table || table->restricted || m_exporting)
        return;
    const QString suggested =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) +
        QStringLiteral("/%1-%2.jsonl")
            .arg(m_table, QDateTime::currentDateTimeUtc().toString(
                              QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export %1").arg(m_table), suggested,
        QStringLiteral("JSON Lines (*.jsonl)"));
    if (path.isEmpty())
        return;
    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("Cannot write %1: %2")
                      .arg(path, file->errorString()),
                  true);
        return;
    }
    m_exporting = true;
    updateActions();
    const D1DatabaseReadRequest base = pageRequest(QString());
    auto rows = std::make_shared<qint64>(0);
    auto pages = std::make_shared<int>(0);
    auto step = std::make_shared<std::function<void(const QString &)>>();
    // Only the in-flight page's callback owns the step (it locks the weak
    // pointer when the request is sent), so the chain frees itself when the
    // last page lands or the widget, and with it the connection, goes away.
    std::weak_ptr<std::function<void(const QString &)>> weakStep = step;
    const QString tableName = m_table;
    auto finish = [this, file, rows, path](const QString &failure) {
        file->close();
        m_exporting = false;
        updateActions();
        if (failure.isEmpty())
            setStatus(QStringLiteral("Exported %1 rows to %2")
                          .arg(*rows)
                          .arg(path));
        else
            setStatus(QStringLiteral("Export stopped after %1 rows: %2")
                          .arg(*rows)
                          .arg(failure),
                      true);
    };
    *step = [this, base, file, rows, pages, weakStep, finish,
             tableName](const QString &cursor) {
        D1DatabaseReadRequest request = base;
        request.cursor = cursor;
        read(request, false, this,
             [this, file, rows, pages, again = weakStep.lock(), finish,
              tableName](int status, const QJsonObject &body) {
                 if (status != 200 ||
                     !body.value(QStringLiteral("ok")).toBool()) {
                     finish(errorText(status, body));
                     return;
                 }
                 for (const QJsonValue &value :
                      body.value(QStringLiteral("rows")).toArray()) {
                     QJsonObject row = value.toObject();
                     row.remove(QStringLiteral("digest"));
                     row.insert(QStringLiteral("table"), tableName);
                     file->write(QJsonDocument(row).toJson(
                         QJsonDocument::Compact));
                     file->write("\n");
                     ++*rows;
                 }
                 const QString next =
                     body.value(QStringLiteral("nextCursor")).toString();
                 ++*pages;
                 setStatus(QStringLiteral("Exporting %1: %2 rows%3")
                               .arg(tableName)
                               .arg(*rows)
                               .arg(QString::fromUtf8("\xE2\x80\xA6")));
                 if (next.isEmpty() || !again || !*again ||
                     *pages >= kExportMaxPages) {
                     finish(next.isEmpty() || *pages < kExportMaxPages
                                ? QString()
                                : QStringLiteral("page limit reached"));
                     return;
                 }
                 (*again)(next);
             });
    };
    (*step)(QString());
}

void DatabaseManagerWidget::showSchema()
{
    if (m_table.isEmpty())
        return;
    D1DatabaseReadRequest request;
    request.op = QStringLiteral("schema");
    request.table = m_table;
    read(request, false, this, [this](int status, const QJsonObject &body) {
        if (status != 200 || !body.value(QStringLiteral("ok")).toBool()) {
            setStatus(errorText(status, body), true);
            return;
        }
        QStringList statements;
        for (const QJsonValue &value :
             body.value(QStringLiteral("objects")).toArray())
            statements << value.toObject().value(QStringLiteral("sql"))
                              .toString() + QLatin1Char(';');
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Schema: %1").arg(m_table));
        dialog.resize(720, 480);
        auto *layout = new QVBoxLayout(&dialog);
        auto *text = new QPlainTextEdit(statements.join(QStringLiteral("\n\n")));
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(text, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, &dialog,
                &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    });
}

void DatabaseManagerWidget::showCleanupPlan()
{
    if (m_tables.isEmpty()) {
        setStatus(QStringLiteral("Load the table list first."), true);
        return;
    }
    const QStringList order = {
        QStringLiteral("drop"),          QStringLiteral("drop-after-readers"),
        QStringLiteral("drop-after-export"), QStringLiteral("custody"),
        QStringLiteral("runtime-only"),  QStringLiteral("encryption-gap"),
        QStringLiteral("hot"),           QStringLiteral("plugin"),
        QStringLiteral("keep"),
    };
    QString html = QStringLiteral(
        "<p>Findings of the D1 audit (%1) joined to the live tables. The audit "
        "orders the work: remove a table's readers and its schema.py DDL in "
        "one deploy, drop it in the next, and export rows first where a "
        "table still holds any.</p>")
                       .arg(m_inventory.value(QStringLiteral("audit"))
                                .toString()
                                .toHtmlEscaped());
    QStringList dropTables;
    for (const QString &category : order) {
        QString rows;
        for (const D1DatabaseTable &table : m_tables) {
            if (table.auditCategory != category)
                continue;
            if (d1DatabaseAuditIsDropCandidate(category))
                dropTables << table.name;
            rows += QStringLiteral("<tr><td><code>%1</code></td>"
                                   "<td align='right'>%2</td><td>%3</td></tr>")
                        .arg(table.name.toHtmlEscaped())
                        .arg(table.rows)
                        .arg(table.auditNote.toHtmlEscaped());
        }
        if (rows.isEmpty())
            continue;
        html += QStringLiteral("<h3>%1</h3><table cellpadding='3' "
                               "width='100%'>%2</table>")
                    .arg(d1DatabaseAuditCategoryLabel(category).toHtmlEscaped(),
                         rows);
    }
    QString gapRows;
    for (const D1DatabaseTable &table : m_tables)
        for (const QString &gap : table.gaps)
            gapRows += QStringLiteral("<tr><td><code>%1</code></td><td>%2</td>"
                                      "</tr>")
                           .arg(table.name.toHtmlEscaped(),
                                gap.toHtmlEscaped());
    if (!gapRows.isEmpty())
        html += QStringLiteral("<h3>Live encryption checks</h3><table "
                               "cellpadding='3' width='100%'>%1</table>")
                    .arg(gapRows);
    QStringList landed;
    for (const QJsonValue &value :
         m_inventory.value(QStringLiteral("droppedSinceAudit")).toArray())
        landed << QStringLiteral("<code>%1</code>")
                      .arg(value.toString().toHtmlEscaped());
    if (!landed.isEmpty())
        html += QStringLiteral("<h3>Already dropped</h3><p>%1</p>")
                    .arg(landed.join(QStringLiteral(", ")));

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("D1 cleanup plan"));
    dialog.resize(820, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *browser = new QTextBrowser;
    browser->setOpenExternalLinks(false);
    browser->setHtml(html);
    layout->addWidget(browser, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(QStringLiteral("Copy drop migration"),
                                    QDialogButtonBox::ActionRole);
    copy->setEnabled(!dropTables.isEmpty());
    copy->setToolTip(QStringLiteral(
        "Copies DROP TABLE statements for the drop candidates that still "
        "exist, for a migration file. Nothing runs from here."));
    connect(copy, &QPushButton::clicked, &dialog, [dropTables, copy] {
        QApplication::clipboard()->setText(d1DatabaseDropMigration(dropTables));
        copy->setText(QStringLiteral("Copied"));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace forkmesh
