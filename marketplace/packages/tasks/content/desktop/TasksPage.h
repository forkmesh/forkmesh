#pragma once

#include "ListViewMode.h"
#include "NativePackageExtension.h"
#include "OrganizationTaskStore.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QTextBrowser;
class QTimer;
class QVBoxLayout;

namespace forkmesh::tasks {

class TasksPage : public forkmesh::market::NativePackageWidget
{
public:
    TasksPage(const forkmesh::market::NativePackageHost &host, QWidget *parent);

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(
        const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;
    int navigationBadge() const override;
    bool acceptsHostCommand(const QString &command) const override;
    bool hostCommand(const QString &command, const QJsonObject &args) override;
    void agentSessionsChanged() override;
    void accountPicturesChanged() override;
    void themeChanged() override;

    void openOrganizationTask(const QString &taskId);
    void openCreatedOrganizationTask(const QJsonObject &task);
    void applyOrganizationTasks(const QJsonObject &payload);
    void setOrganizationTaskListView(forkmesh::ui::ListViewMode view);
    forkmesh::ui::ListViewMode organizationTaskListView() const
    {
        return m_organizationTaskListView;
    }
    void reloadOrganizationTaskListView();
    void refreshOrganizationTaskAgentRows();
    void tickOrganizationTaskSpinners();
    QPixmap organizationTaskAgentFacePicture(const QString &taskId) const;
    QPixmap organizationTaskCreatorPicture(const QString &taskId) const;
    QPixmap organizationTaskAssigneePicture(const QString &taskId) const;
    bool organizationTaskAgentWorking(const QString &taskId) const;
    bool organizationTaskSpinTimerActive() const;
    QImage organizationTaskRowImage(const QString &taskId) const;

private:
    struct LocalAgent {
        int id = 0;
        QString orgTaskId;
        qint64 createdAtMs = 0;
        QString provider;
        QString providerName;
        QString model;
        QString status;
        QString branch;
        bool merged = false;
    };

    void ensureBuilt();
    QWidget *buildOrganizationTasksSection();
    void refreshOrganizationTasks();
    void updateOrganizationTaskRefreshAge();
    void onCatalogLoaded(const QJsonObject &payload);
    void onCatalogFailed(const QString &error);
    bool replaceOrganizationTask(const QJsonObject &task);
    bool settleOrganizationTaskEdit(const QString &taskId,
                                    const QJsonObject &previous, bool ok,
                                    const QJsonObject &payload,
                                    const QString &error, bool *updated);
    bool organizationTaskCheckboxToggleable(int index) const;
    void toggleOrganizationTaskCheckbox(int index);
    void overlayOrganizationTaskLocalEdits();
    void renderOrganizationTaskRows(const QString &selectTaskId = QString());
    void appendOrganizationTaskRows(int count);
    void refreshOrganizationTaskRowThumbnails(const QString &taskId);
    void updateOrganizationTaskPageLabel();
    void revealOrganizationTaskStatus(const QJsonObject &task);
    void setOrganizationTaskBadge(int openCount);
    void renderOrganizationTaskDetail();
    void updateOrganizationTaskActions();
    void createOrganizationTask();
    void composeOrganizationTask(const QString &windowTitle,
                                 const QString &prefillTitle,
                                 const QString &prefillDetails,
                                 std::function<void()> onCreated);
    void createOrganizationTaskFollowUp();
    void moveOrganizationTaskToIssue();
    void addOrganizationTaskToPrompt();
    void startOrganizationTaskAgentFromPrompt();
    void attachExistingWorktreeToOrganizationTask();
    QList<LocalAgent> localAgents() const;
    const LocalAgent *localAgentForTask(const QString &taskId) const;
    int localAgentSessionIdForTask(const QString &taskId) const;
    QString localAgentRunLabelForTask(const QString &taskId) const;
    QString localAgentBranchForTask(const QString &taskId) const;
    QPixmap accountPicture(const QString &account) const;
    bool organizationTaskAgentFace(const QJsonObject &task, QPixmap *face,
                                   bool *working) const;
    void applyOrganizationTaskAgentFace(QTableWidgetItem *item,
                                        const QJsonObject &task);
    QJsonObject organizationTaskForRow(int row) const;
    int organizationTaskRowForId(const QString &taskId) const;
    void applyOrganizationTaskPeopleFaces(QTableWidgetItem *item);
    void refreshOrganizationTaskPeopleFaces();
    void rebuildOrganizationTaskPeopleStrip();
    void updateOrganizationTaskPeopleStrip(
        const QHash<QString, int> &openByAssignee);
    void setOrganizationTaskAssigneeFilter(const QString &filter);
    bool organizationTaskRowsHaveWorkingAgent() const;
    void updateOrganizationTaskSpinTimer();
    void editOrganizationTask();
    void assignOrganizationTaskToAgent();
    void queueOrganizationTaskAgent(const QJsonObject &task);
    void runOrganizationTaskAction(const QString &action);
    void sendOrganizationTaskAction(const QString &taskId,
                                    const QString &action,
                                    const QJsonObject &body);
    void patchOrganizationTask(const QJsonObject &task,
                               const QJsonObject &body);
    void syncOrganizationTaskProperties(const QJsonObject &task);
    void commitOrganizationTaskProperty(const QString &field);
    void commentOnOrganizationTask();
    void postOrganizationTaskComment(const QString &body);
    void submitOrganizationTaskQuickComment();
    void checkInOnOrganizationTask();
    void remindOrganizationTaskAssignee();
    void loadOrganizationTaskHistory(const QString &taskId);
    void deleteOrganizationTask();
    void showWorkPlanForDelete(const QString &id, const QString &title,
                               const QJsonObject &plan);
    void sendOrganizationTaskDelete(const QString &taskId, int cleanupMode,
                                    bool force = false);
    void cleanupOrganizationTaskWork(const QString &taskId,
                                     const QJsonArray &workItems, int mode);
    void deleteClosedOrganizationTasks();
    void deleteNextClosedOrganizationTask();
    void finishClosedOrganizationTaskDelete();
    void publishRepositoryLogos();
    void requestOrganizationTasks(const QByteArray &method, const QString &path,
                                  const QJsonObject &body,
                                  TaskReplyHandler handler,
                                  const QList<int> &expectedHttpStatuses = {});
    void requestOrganizationTaskAttachment(const QString &taskId,
                                           const QString &attachmentId,
                                           TaskAttachmentHandler handler);
    void flashMessage(const QString &text, bool error = false);
    void reportError(const QString &text);
    void showStatus(const QString &text);
    void showPage();
    bool windowCommand(const QString &command, const QJsonObject &args);
    QJsonValue windowQuery(const QString &command, const QJsonObject &args) const;

    forkmesh::market::NativePackageHost m_host;
    QPointer<OrganizationTaskStore> m_store;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_section = nullptr;
    mutable QList<LocalAgent> m_localAgents;
    mutable bool m_localAgentsStale = true;
    QHash<QString, int> m_deleteRetries;

    QTableWidget *m_organizationTasksTable = nullptr;
    QString m_organizationTasksSearchMirror;
    QTextBrowser *m_organizationTaskDetail = nullptr;
    QTextBrowser *m_organizationTaskFields = nullptr;
    QWidget *m_organizationTaskSide = nullptr;
    QWidget *m_organizationTaskProperties = nullptr;
    QComboBox *m_organizationTaskStatusEdit = nullptr;
    QSpinBox *m_organizationTaskPriorityEdit = nullptr;
    QSpinBox *m_organizationTaskProgressEdit = nullptr;
    QComboBox *m_organizationTaskAssigneeEdit = nullptr;
    QComboBox *m_organizationTaskDepartmentEdit = nullptr;
    QLineEdit *m_organizationTaskTeamEdit = nullptr;
    QLineEdit *m_organizationTaskRepositoryEdit = nullptr;
    QLineEdit *m_organizationTaskParentEdit = nullptr;
    QString m_organizationTaskPropertiesTaskId;
    QPlainTextEdit *m_organizationTaskQuickComment = nullptr;
    QPushButton *m_organizationTaskQuickCommentSend = nullptr;
    QString m_organizationTaskQuickCommentTaskId;
    QHash<QString, QString> m_organizationTaskCommentDrafts;
    QPushButton *m_organizationTaskRefreshButton = nullptr;
    QLabel *m_organizationTasksStatus = nullptr;
    QLabel *m_organizationTasksSummary = nullptr;
    QPushButton *m_organizationTaskNewButton = nullptr;
    QPushButton *m_organizationTaskDeleteClosedButton = nullptr;
    QPushButton *m_organizationTaskSortButton = nullptr;
    QPushButton *m_organizationTaskViewButton = nullptr;
    forkmesh::ui::ListViewMode m_organizationTaskListView =
        forkmesh::ui::ListViewMode::Full;
    QPushButton *m_organizationTaskMineFilterButton = nullptr;
    QWidget *m_organizationTaskPeopleStrip = nullptr;
    QHBoxLayout *m_organizationTaskPeopleLayout = nullptr;
    QButtonGroup *m_organizationTaskPeopleGroup = nullptr;
    QStringList m_organizationTaskPeopleKeys;
    QPushButton *m_organizationTaskOpenFilterButton = nullptr;
    QPushButton *m_organizationTaskClosedFilterButton = nullptr;
    QPushButton *m_organizationTaskAllFilterButton = nullptr;
    QPushButton *m_organizationTaskFollowUpButton = nullptr;
    QPushButton *m_organizationTaskEditButton = nullptr;
    QPushButton *m_organizationTaskAgentButton = nullptr;
    QPushButton *m_organizationTaskStartButton = nullptr;
    QPushButton *m_organizationTaskCompleteButton = nullptr;
    QPushButton *m_organizationTaskUndoButton = nullptr;
    QPushButton *m_organizationTaskQaButton = nullptr;
    QPushButton *m_organizationTaskReturnButton = nullptr;
    QPushButton *m_organizationTaskDeleteButton = nullptr;
    QPushButton *m_organizationTaskAttachWorktreeButton = nullptr;
    QPushButton *m_organizationTaskPromptButton = nullptr;
    QPushButton *m_organizationTaskIssueButton = nullptr;
    QPushButton *m_organizationTaskStartAgentButton = nullptr;
    QPushButton *m_organizationTaskCommentButton = nullptr;
    QPushButton *m_organizationTaskCheckinButton = nullptr;
    QPushButton *m_organizationTaskRemindButton = nullptr;
    QLabel *m_organizationTaskPageLabel = nullptr;
    QList<int> m_organizationTaskMatches;
    int m_organizationTaskShownRows = 0;
    QTimer *m_organizationTaskSpinTimer = nullptr;
    qreal m_organizationTaskSpinAngle = 0.0;
    QString m_organizationTaskSortKey = QStringLiteral("created-desc");
    QString m_organizationTaskStatusFilter = QStringLiteral("open");
    QString m_organizationTaskAssigneeFilter = QStringLiteral("all");
    QJsonArray m_organizationTasks;
    QHash<QString, QJsonObject> m_organizationTaskLocalEdits;
    QHash<QString, int> m_organizationTaskEditsInFlight;
    QString m_pendingOrganizationTaskSelection;
    QStringList m_organizationTaskMembers;
    QHash<QString, QJsonArray> m_organizationTaskHistory;
    QHash<QString, qint64> m_organizationTaskHistoryAt;
    QString m_organizationTaskHistoryInFlight;
    QStringList m_organizationTaskDepartments;
    QString m_organizationTaskActor;
    bool m_organizationTasksCanManage = false;
    bool m_organizationTaskIssueMoveInFlight = false;
    bool m_organizationTaskClosedDeleteInFlight = false;
    QStringList m_organizationTaskClosedDeleteIds;
    int m_organizationTaskClosedDeleteTotal = 0;
    int m_organizationTaskClosedDeleteCount = 0;
    int m_organizationTaskClosedDeleteRetries = 0;
    int m_organizationTaskClosedDeleteKept = 0;
    bool m_organizationTaskClosedDeleteForce = false;
    QStringList m_organizationTaskClosedDeleteBusy;
    QStringList m_organizationTaskClosedDeleteReasons;
    bool m_organizationTasksLoaded = false;
    qint64 m_organizationTasksLoadedAt = 0;
    QHash<QString, QByteArray> m_organizationTaskImageData;
    QHash<QString, QString> m_organizationTaskImageMime;
    QSet<QString> m_organizationTaskImageLoads;
};

}
