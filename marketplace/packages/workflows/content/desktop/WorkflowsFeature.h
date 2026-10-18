#pragma once

#include "NativePackageExtension.h"
#include "WorkflowsModel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QWidget;

namespace forkmesh::workflows {

class WorkflowsFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler = forkmesh::market::NativePackageReplyHandler;
    using Request = forkmesh::market::NativePackageRequest;

    explicit WorkflowsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    int navigationBadge() const override;

private:
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler);
    void setStatus(const QString &text, bool error = false);
    QString failureText(const QJsonObject &payload, const QString &error) const;
    QWidget *buildDashboard();
    QWidget *buildEditor();
    void loadCatalog(std::function<void()> done);
    void loadWorkflows();
    void loadAllRuns();
    void pumpRunQueue();
    void renderDashboard();
    void renderWorkflowTable();
    void renderRunTable();
    void showRunDetail(QTableWidget *steps, QLabel *summary, const QString &workflowId, const QString &runId);
    QJsonObject selectedSummary() const;
    QJsonObject selectedDashboardRun() const;
    void fetchWorkflow(const QString &id, std::function<void(const QJsonObject &)> done);
    void openWorkflow(const QString &id);
    void openDraft(const Draft &draft, bool dirty);
    bool leaveEditor();
    void showDashboard();
    void toggleActive(const QJsonObject &summary);
    void duplicateWorkflow(const QJsonObject &summary);
    void deleteWorkflow(const QString &id, const QString &name);
    void runWorkflow(const QString &id);
    void runSummary(const QJsonObject &summary);

    void renderEditor();
    void renderStepList();
    void renderStepForm();
    void renderEditorRuns();
    void loadEditorRuns();
    void setDirty(bool dirty);
    void saveDraft(std::function<void(bool)> done = {});
    void runDraft();
    void runOnDesktop(const Draft &draft);
    void runDesktopStep(int index);
    void finishDesktopRun();
    void exportDraft();
    void deleteDraft();
    void rotateHook();
    QString selectedStepId() const;
    void selectStep(const QString &id);
    void updateSelectedNode(const std::function<void(QJsonObject &)> &change);
    QWidget *fieldEditor(const QJsonObject &field, const QJsonObject &node);
    void addInputRow(QFormLayout *form, const QJsonObject &node);
    QString desktopCheckout() const;

    void newWorkflowDialog();
    void importDocument(const QJsonObject &body, const QString &path);
    void addStepDialog();
    void credentialsDialog(const QString &integration = QString());
    void loadCredentials(std::function<void()> done = {});

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    std::function<void()> m_navigationBadgeChanged;
    std::function<QList<forkmesh::market::NativePackageRepository>()> m_repositories;

    Catalog m_catalog;
    bool m_catalogLoaded = false;
    QJsonArray m_templates;
    QJsonArray m_workflows;
    QJsonArray m_credentials;
    bool m_credentialsLoaded = false;
    QHash<QString, QJsonArray> m_runsByWorkflow;
    QJsonArray m_allRuns;
    QStringList m_runQueue;
    int m_runsInFlight = 0;
    int m_serial = 0;
    bool m_loading = false;
    bool m_ready = false;
    QString m_hostQuery;

    Draft m_draft;
    bool m_dirty = false;
    bool m_busy = false;
    QString m_selectedStep;
    QJsonArray m_editorRuns;

    struct DesktopRun {
        Draft draft;
        QStringList order;
        QJsonObject report;
        QString checkout;
        QString python;
        QString failed;
        qint64 startedAt = 0;
        double freed = 0;
        double free = -1;
        double total = -1;
    };
    DesktopRun m_desktopRun;

    QLabel *m_status = nullptr;
    QStackedWidget *m_pages = nullptr;
    QPushButton *m_newButton = nullptr;

    QList<QLabel *> m_kpiValues;
    QList<QLabel *> m_kpiNotes;
    QLineEdit *m_filter = nullptr;
    QTableWidget *m_workflowTable = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QLabel *m_emptyNote = nullptr;
    QComboBox *m_runWorkflowFilter = nullptr;
    QComboBox *m_runStatusFilter = nullptr;
    QTableWidget *m_runTable = nullptr;
    QLabel *m_runSummary = nullptr;
    QTableWidget *m_runSteps = nullptr;

    QLineEdit *m_name = nullptr;
    QCheckBox *m_active = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_runDraftButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_deleteDraftButton = nullptr;
    QLabel *m_editorNote = nullptr;
    QListWidget *m_steps = nullptr;
    QPushButton *m_addStepButton = nullptr;
    QPushButton *m_removeStepButton = nullptr;
    QWidget *m_formHost = nullptr;
    QTableWidget *m_editorRunTable = nullptr;
    QLabel *m_editorRunSummary = nullptr;
    QTableWidget *m_editorRunSteps = nullptr;
};

} // namespace forkmesh::workflows
