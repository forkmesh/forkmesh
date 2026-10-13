#pragma once

#include "RemoveCoreNodeModel.h"

#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace forkmesh::fleet {

class RemoveCoreNodeController : public QObject {
    Q_OBJECT
public:
    explicit RemoveCoreNodeController(const QString &script, QObject *parent = nullptr);
    ~RemoveCoreNodeController() override;

    QString script() const { return m_script; }
    void setRunDirectory(const QString &directory) { m_runDirectory = directory; }
    QString runDirectory() const { return m_runDirectory; }
    bool reattach();
    bool isAvailable() const;
    bool isRunning() const;
    bool planLoading() const { return m_planLoading; }
    QString planName() const { return m_planName; }
    const RemoveCorePlan &plan() const { return m_plan; }
    const AddCoreRun &run() const { return m_run; }
    QString runName() const { return m_runName; }
    bool runDestroysInstance() const { return m_runDestroy; }
    QString log() const { return m_log; }
    QStringList lastArguments() const { return m_lastArguments; }

    void loadPlan(const QString &name, bool destroyInstance);
    void start(const QString &name, bool destroyInstance);
    void retry();
    void clearRun();

signals:
    void planChanged();
    void runChanged();
    void logAppended(const QString &text);
    void finished(bool ok);

private:
    QProcess *spawn(const QStringList &arguments, bool merged);
    void launch(const QStringList &arguments);
    bool launchDetached(const QStringList &arguments);
    void attach(const QString &unit, const QString &logPath);
    void pollDetached();
    void readDetachedLog();
    void finishDetached(int exitStatus);
    QString currentFile() const;
    void appendLog(const QString &text);

    QString m_script;
    RemoveCorePlan m_plan;
    QString m_planName;
    bool m_planDestroy = false;
    int m_planGeneration = 0;
    AddCoreRun m_run;
    QString m_runName;
    bool m_runDestroy = false;
    QString m_log;
    QStringList m_lastArguments;
    QPointer<QProcess> m_process;
    QString m_runDirectory;
    QString m_unit;
    QString m_logPath;
    qint64 m_logOffset = 0;
    QTimer *m_poll = nullptr;
    QPointer<QProcess> m_unitQuery;
    int m_pollTicks = 0;
    bool m_planLoading = false;
};

class RemoveCoreNodeDialog : public QDialog {
    Q_OBJECT
public:
    enum class Page { Plan, Progress };

    RemoveCoreNodeDialog(RemoveCoreNodeController *controller, const QString &name,
                         QWidget *parent = nullptr);

    Page page() const;
    QString nodeName() const { return m_name; }

private:
    QWidget *buildPlanPage();
    QWidget *buildProgressPage();
    void showPage(Page page);
    void refreshPlan();
    void refreshStartButton();
    void refreshRun();
    void appendLog(const QString &text);

    QPointer<RemoveCoreNodeController> m_controller;
    QString m_name;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_planText = nullptr;
    QLabel *m_quorum = nullptr;
    QLabel *m_blocker = nullptr;
    QCheckBox *m_destroy = nullptr;
    QLineEdit *m_confirm = nullptr;
    QPushButton *m_start = nullptr;
    QLabel *m_headline = nullptr;
    QListWidget *m_steps = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_notes = nullptr;
    QPushButton *m_retry = nullptr;
    QPushButton *m_close = nullptr;
};

} // namespace forkmesh::fleet
