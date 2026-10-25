#pragma once

#include "NativePackageExtension.h"
#include "WorkflowsCards.h"
#include "WorkflowsModel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QIcon;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;
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
    void repositoryWorkflowsChanged() override;

protected:
    void changeEvent(QEvent *event) override;

private:
    using LocalAction = std::function<void(const QString &, const QString &, const QString &,
                                           const QJsonObject &, ReplyHandler)>;

    Catalog &catalog();
    bool localAvailable() const;
    void loadLocal();
    QJsonObject localSummary(const QString &repo, const QString &id) const;
    void localAction(const QString &repo, const QString &id, const QString &action, const QJsonObject &payload,
                     ReplyHandler done);
    void openLocalWorkflow(const QString &repo, const QString &id);
    void loadLocalRuns();
    void runLocal(const QJsonObject &summary, const QString &assignment = QString());
    void showPinMenu(const QJsonObject &entry, const QPoint &globalPos);
    void startLocalDraft(const QString &kind, const QString &repo);
    QIcon faceIcon(const QString &base64) const;
    QJsonObject localRunRow(const QJsonObject &run, const QString &repo, const QString &workflowName) const;
    void showLocalRunDetail(QTableWidget *steps, QLabel *summary, const QJsonObject &run);
    QJsonArray repositoryShortcuts(const QString &repo) const;
    QString uniqueLocalId(const QString &repo, const QString &name) const;
    void renderLocalEditorBar();
    void rebuildRunFilter();

    void request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler);
    void setStatus(const QString &text, bool error = false);
    QString failureText(const QJsonObject &payload, const QString &error) const;
    QWidget *buildDashboard();
    QWidget *buildCollection();
    void rebuildEntries();
    void renderCollection();
    QJsonObject entryFor(const QString &key) const;
    QPixmap entryFace(const QJsonObject &entry) const;
    void entryAction(const QString &key, const QString &action, QWidget *anchor);
    void showEntryMenu(const QJsonObject &entry, const QPoint &globalPos);
    void runEntry(const QJsonObject &entry, const QString &assignment = QString());
    void askAssignment(const QJsonObject &entry);
    void toggleEntry(const QJsonObject &entry);
    void showEntryOutput(const QJsonObject &entry);
    void deleteEntry(const QJsonObject &entry);
    void addAgentStep(const QJsonObject &entry);
    void setViewMode(ViewMode mode);
    void applyTheme();
    void syncAccountPins();
    QWidget *buildEditor();
    void loadCatalog(std::function<void()> done);
    void loadWorkflows();
    void loadAllRuns();
    void pumpRunQueue();
    void renderDashboard();
    void renderRunTable();
    void showRunDetail(QTableWidget *steps, QLabel *summary, const QString &workflowId, const QString &runId,
                       QPlainTextEdit *output = nullptr);
    QJsonObject selectedDashboardRun() const;
    void showSelectedRun();
    void pollLiveRun();
    void followRun(const QString &workflowKey, const QString &runId = QString());
    void stopFollowing();
    void fetchWorkflow(const QString &id, std::function<void(const QJsonObject &)> done);
    void openWorkflow(const QString &id);
    void openDraft(const Draft &draft, bool dirty);
    bool leaveEditor();
    void showDashboard();
    void toggleActive(const QJsonObject &summary);
    void duplicateWorkflow(const QJsonObject &summary);
    void deleteWorkflow(const QString &id, const QString &name);
    void runWorkflow(const QString &id, const QJsonObject &body = {});
    void runSummary(const QJsonObject &summary, const QString &assignment = QString());

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
    std::function<QJsonObject()> m_repositoryWorkflows;
    LocalAction m_repositoryWorkflowAction;

    Catalog m_localCatalog;
    QJsonObject m_local;
    QJsonArray m_localWorkflows;
    QJsonArray m_localRuns;
    QJsonArray m_entries;
    QString m_selectedKey;
    QString m_scope;
    QString m_sort = QStringLiteral("name");
    ViewMode m_viewMode = ViewMode::Thumbnails;
    Tones m_tones;
    QString m_appliedSheet;
    bool m_theming = false;
    QString m_pendingAgentStep;
    CardGrid *m_grid = nullptr;
    QScrollArea *m_collection = nullptr;
    QLabel *m_collectionEmpty = nullptr;
    QLabel *m_collectionCount = nullptr;
    QComboBox *m_sortCombo = nullptr;
    QList<QToolButton *> m_scopeButtons;
    QList<QToolButton *> m_viewButtons;
    QPushButton *m_editorPinButton = nullptr;
    QPushButton *m_editorAgentButton = nullptr;
    QComboBox *m_iconCombo = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLineEdit *m_description = nullptr;

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
    QComboBox *m_runWorkflowFilter = nullptr;
    QComboBox *m_runStatusFilter = nullptr;
    QTableWidget *m_runTable = nullptr;
    QLabel *m_runSummary = nullptr;
    QTableWidget *m_runSteps = nullptr;
    QPlainTextEdit *m_runOutput = nullptr;
    QSplitter *m_dashboardSplitter = nullptr;
    QTimer *m_liveTimer = nullptr;
    QString m_shownRunId;
    QString m_shownStatus;
    QString m_followWorkflow;
    QString m_followRun;
    qint64 m_followSince = 0;
    QJsonObject m_pendingRun;

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
