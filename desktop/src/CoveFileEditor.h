#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace forkmesh::cove {

class SpreadsheetEditor final : public QWidget
{
public:
    explicit SpreadsheetEditor(const QString &csv, bool readOnly,
                               QWidget *parent = nullptr);

    QString body() const;
    bool isReadOnly() const { return m_readOnly; }
    bool isModified() const { return m_modified; }
    void setModified(bool modified);
    void setChangeHandler(std::function<void(bool)> handler);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void addRow();
    void addColumn();
    void deleteSelectedRows();
    void deleteSelectedColumns();
    void copySelection() const;
    void pasteSelection();
    void clearSelection();
    void markModified();
    void updateStatus();
    static QString columnLabel(int column);

    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_addRowButton = nullptr;
    QPushButton *m_addColumnButton = nullptr;
    QPushButton *m_deleteRowButton = nullptr;
    QPushButton *m_deleteColumnButton = nullptr;
    bool m_readOnly = false;
    bool m_modified = false;
    bool m_loading = false;
    std::function<void(bool)> m_changeHandler;
};

class PipelineEditor final : public QWidget
{
public:
    explicit PipelineEditor(const QString &json, const QString &actor,
                            bool readOnly, QWidget *parent = nullptr);

    QString body() const;
    bool isReadOnly() const { return m_readOnly; }
    bool isModified() const { return m_modified; }
    void setModified(bool modified);
    void setChangeHandler(std::function<void(bool)> handler);

private:
    void load(const QJsonObject &pipeline);
    void addLead();
    void deleteSelectedLeads();
    void addStage();
    void deleteSelectedStage();
    void appendHistory(int leadRow, const QString &fromStage,
                       const QString &toStage, const QString &action);
    void refreshStageCombos();
    void refreshReports();
    void markModified();
    void updateLeadTimestamp(int row);
    QString stageName(const QString &id) const;
    QString stageColor(const QString &id) const;
    QString firstStageId() const;
    int leadRowForWidget(const QWidget *widget) const;
    double leadValue(int row) const;

    QJsonObject m_root;
    QJsonArray m_history;
    QString m_actor;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_leads = nullptr;
    QTableWidget *m_stages = nullptr;
    QTableWidget *m_summary = nullptr;
    QTableWidget *m_historyTable = nullptr;
    QLabel *m_reportHeadline = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_addLeadButton = nullptr;
    QPushButton *m_deleteLeadButton = nullptr;
    QPushButton *m_addStageButton = nullptr;
    QPushButton *m_deleteStageButton = nullptr;
    bool m_readOnly = false;
    bool m_modified = false;
    bool m_loading = false;
    std::function<void(bool)> m_changeHandler;
};

} // namespace forkmesh::cove
