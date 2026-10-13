#pragma once

#include "D1DatabaseAdmin.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class QUrl;

namespace forkmesh {

// What the Database tab borrows from the window: the network, the relay
// endpoint, and the administrator's signing key. `administrator` returns the
// signing account, or an empty string with `*reason` explaining why this
// desktop cannot sign.
struct DatabaseManagerHooks {
    std::function<QNetworkAccessManager *()> network;
    std::function<QUrl()> endpoint;
    std::function<QString(QString *reason)> administrator;
    std::function<QString(const QByteArray &canonical)> sign;
    // Today's D1 usage report, for the queries that touch a table.
    std::function<QJsonObject()> d1UsageReport;
    std::function<void(QPushButton *, const QString &icon)> decorateButton;
};

// The Logs page's Database tab: every D1 table with its row count, storage
// model and #682 audit finding; paged rows with sealed `data` decrypted; and
// create, edit, delete and export for tables outside the restricted set.
// Holds no Q_OBJECT, so it needs no moc registration.
class DatabaseManagerWidget : public QWidget {
public:
    explicit DatabaseManagerWidget(DatabaseManagerHooks hooks,
                                   QWidget *parent = nullptr);

    // Loads the inventory the first time the tab is shown.
    void activate();

private:
    using Reply = std::function<void(int status, const QJsonObject &body)>;

    void read(const D1DatabaseReadRequest &request, bool refresh,
              QObject *context, Reply done);
    void write(const QJsonObject &payload, QObject *context, Reply done);

    void loadInventory(bool refresh);
    void rebuildTableList();
    void selectTable(const QString &name);
    void resetPaging();
    void loadPage();
    void renderRows();
    void renderDetails();
    void updateActions();
    const D1DatabaseTable *currentTable() const;
    D1DatabaseReadRequest pageRequest(const QString &cursor) const;
    QList<int> selectedRowIndexes() const;

    void editRow(int rowIndex); // -1 inserts
    void deleteSelectedRows();
    void exportTable();
    void showSchema();
    void showCleanupPlan();
    void setStatus(const QString &text, bool error = false);

    DatabaseManagerHooks m_hooks;

    QLabel *m_summary = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_cleanupButton = nullptr;
    QLineEdit *m_tableSearch = nullptr;
    QComboBox *m_tableView = nullptr;
    QTreeWidget *m_tableList = nullptr;
    QLabel *m_tableTitle = nullptr;
    QLabel *m_tableDetails = nullptr;
    QComboBox *m_filterColumn = nullptr;
    QComboBox *m_filterMode = nullptr;
    QLineEdit *m_filterValue = nullptr;
    QCheckBox *m_oldestFirst = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_schemaButton = nullptr;
    QTableWidget *m_rowsView = nullptr;
    QLabel *m_status = nullptr;

    QJsonObject m_inventory;
    QList<D1DatabaseTable> m_tables;
    QString m_table;
    QList<D1DatabaseColumn> m_columns;
    bool m_tableHasRowid = true;
    QJsonArray m_rows;
    // The cursor that produced each page read so far; the last is showing.
    QStringList m_pageCursors;
    QString m_nextCursor;
    // Filter applied to the pages, captured when Apply is pressed.
    QString m_appliedFilterColumn;
    QString m_appliedFilterMode;
    QString m_appliedFilterValue;
    quint64 m_pageGeneration = 0;
    bool m_activated = false;
    bool m_inventoryInFlight = false;
    bool m_exporting = false;
};

} // namespace forkmesh
