#pragma once

#include "AddCoreNodeModel.h"

#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace forkmesh::fleet {

class AddCoreNodeController : public QObject {
    Q_OBJECT
public:
    explicit AddCoreNodeController(const QString &script, QObject *parent = nullptr);
    ~AddCoreNodeController() override;

    QString script() const { return m_script; }
    void setRunDirectory(const QString &directory) { m_runDirectory = directory; }
    QString runDirectory() const { return m_runDirectory; }
    QString unitName() const { return m_unit; }
    QString logPath() const { return m_logPath; }
    bool reattach();
    bool isAvailable() const;
    bool isRunning() const;
    bool optionsLoading() const { return m_optionsLoading; }
    bool statusLoaded() const { return m_statusLoaded; }
    const AddCoreOptions &options() const { return m_options; }
    const AddCoreRun &run() const { return m_run; }
    QString log() const { return m_log; }
    QStringList lastArguments() const { return m_lastArguments; }
    QString provider() const { return m_provider; }
    void setProvider(const QString &provider);
    QString replaceTarget() const { return m_replace; }
    void setReplaceTarget(const QString &name);
    QString activeReplaceTarget() const;

    void loadOptions();
    void loadStatus();
    void start(const QString &region, const QString &plan);
    void retry();
    void clearRun();

signals:
    void optionsChanged();
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
    AddCoreOptions m_options;
    AddCoreRun m_run;
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
    bool m_optionsLoading = false;
    bool m_reloadOptions = false;
    bool m_statusLoaded = false;
    QString m_provider = QStringLiteral("vultr");
    QString m_replace;
};

class AddCoreNodeDialog : public QDialog {
    Q_OBJECT
public:
    enum class Page { Choose, Confirm, Progress };

    explicit AddCoreNodeDialog(AddCoreNodeController *controller, QWidget *parent = nullptr);

    Page page() const;
    QString selectedRegion() const;
    QString selectedPlan() const;
    QString selectedProvider() const;

private:
    QWidget *buildChoosePage();
    QWidget *buildConfirmPage();
    QWidget *buildProgressPage();
    void showPage(Page page);
    void refreshOptions();
    void refreshPlans();
    void refreshEstimate();
    void refreshRun();
    void refreshTitle();
    void appendLog(const QString &text);

    QPointer<AddCoreNodeController> m_controller;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_chooseTitle = nullptr;
    QLabel *m_chooseIntro = nullptr;
    QComboBox *m_provider = nullptr;
    QComboBox *m_region = nullptr;
    QComboBox *m_plan = nullptr;
    QLabel *m_cost = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_role = nullptr;
    QLabel *m_chooseError = nullptr;
    QPushButton *m_next = nullptr;
    QLabel *m_confirmText = nullptr;
    QPushButton *m_create = nullptr;
    QLabel *m_headline = nullptr;
    QListWidget *m_steps = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_notes = nullptr;
    QPushButton *m_retry = nullptr;
    QPushButton *m_again = nullptr;
    QPushButton *m_close = nullptr;
    bool m_plansFilling = false;
};

} // namespace forkmesh::fleet
