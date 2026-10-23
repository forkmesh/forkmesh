#include "RemoveCoreNodeDialog.h"
#include "ProcessReaper.h"

#include "FleetAutoscaleModel.h"
#include "FontSizing.h"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace forkmesh::fleet {

namespace {

constexpr int kMaxLogChars = 400 * 1024;
constexpr int kPlanTimeoutMs = 120 * 1000;

QLabel *mutedLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#8b949e;"));
    return label;
}

QLabel *titleLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    font = forkmesh::scaledFont(font, 1.2);
    label->setFont(font);
    return label;
}

} // namespace

RemoveCoreNodeController::RemoveCoreNodeController(const QString &script, QObject *parent)
    : QObject(parent), m_script(script)
{
    configureRemoveRun(m_run);
}

RemoveCoreNodeController::~RemoveCoreNodeController()
{
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
}

bool RemoveCoreNodeController::isAvailable() const
{
    return !m_script.isEmpty() && QFileInfo(m_script).isFile();
}

bool RemoveCoreNodeController::isRunning() const
{
    return !m_unit.isEmpty() || (m_process && m_process->state() != QProcess::NotRunning);
}

QString RemoveCoreNodeController::currentFile() const
{
    return m_runDirectory.isEmpty() ? QString()
                                    : QDir(m_runDirectory).filePath(QStringLiteral("current.json"));
}

QProcess *RemoveCoreNodeController::spawn(const QStringList &arguments, bool merged)
{
    auto *process = new QProcess(this);
    process->setProcessChannelMode(merged ? QProcess::MergedChannels
                                          : QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(QFileInfo(m_script).absolutePath());
    process->setProgram(forkmesh::autoscale::fleetPython(m_script));
    process->setArguments(arguments);
    return process;
}

void RemoveCoreNodeController::loadPlan(const QString &name, bool destroyInstance)
{
    if (!isAvailable() || !isRemovableCoreName(name)) {
        m_plan = RemoveCorePlan();
        m_planName = name;
        m_planDestroy = destroyInstance;
        m_plan.error = isAvailable() ? QStringLiteral("%1 is not a core node").arg(name)
                                     : QStringLiteral("tools/next_fleet.py was not found");
        emit planChanged();
        return;
    }
    const int generation = ++m_planGeneration;
    m_planName = name;
    m_planDestroy = destroyInstance;
    m_planLoading = true;
    emit planChanged();
    QProcess *process = spawn(removeCorePlanArguments(m_script, name, destroyInstance), false);
    auto *deadline = new QTimer(process);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this,
            [this, process, generation](int exitCode, QProcess::ExitStatus) {
                const QByteArray out = process->readAllStandardOutput();
                const QByteArray err = process->readAllStandardError();
                process->deleteLater();
                if (generation != m_planGeneration)
                    return;
                m_planLoading = false;
                m_plan = parseRemoveCorePlan(out);
                if (!m_plan.valid) {
                    const QString detail =
                        QString::fromUtf8(err).trimmed().split(QLatin1Char('\n')).last();
                    m_plan.error = QStringLiteral("remove-core-plan failed (%1): %2")
                                       .arg(exitCode)
                                       .arg(detail.isEmpty() ? QStringLiteral("no output")
                                                             : detail.left(300));
                }
                if (m_run.state() == AddCoreRun::State::Idle && !m_plan.steps.isEmpty())
                    m_run.setDefaultSteps(m_plan.steps);
                emit planChanged();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                if (generation != m_planGeneration)
                    return;
                m_planLoading = false;
                m_plan = RemoveCorePlan();
                m_plan.error = QStringLiteral("could not start %1").arg(process->program());
                emit planChanged();
            });
    deadline->start(kPlanTimeoutMs);
    process->start();
}

bool RemoveCoreNodeController::launchDetached(const QStringList &arguments)
{
    if (m_runDirectory.isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("systemd-run")).isEmpty())
        return false;
    if (!QDir().mkpath(m_runDirectory))
        return false;
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString unit = removeCoreUnitName(stamp);
    const QString logPath = QDir(m_runDirectory).filePath(stamp + QStringLiteral(".log"));
    QList<QPair<QString, QString>> environment;
    const QProcessEnvironment system = forkmesh::processreaper::releasedEnvironment();
    for (const QString &key : system.keys()) {
        if (key == QLatin1String("PATH") || key.startsWith(QLatin1String("FMNEXT_")) ||
            key.startsWith(QLatin1String("FORKMESH_")))
            environment.append({key, system.value(key)});
    }
    QProcess launcher;
    launcher.setProcessChannelMode(QProcess::MergedChannels);
    launcher.start(QStringLiteral("systemd-run"),
                   systemdRunArguments(unit, logPath, QFileInfo(m_script).absolutePath(),
                                       forkmesh::autoscale::fleetPython(m_script), arguments,
                                       environment));
    if (!launcher.waitForFinished(30000) || launcher.exitStatus() != QProcess::NormalExit ||
        launcher.exitCode() != 0) {
        appendLog(QStringLiteral("systemd-run could not start the run (%1); running it "
                                 "inside the desktop instead\n")
                      .arg(QString::fromUtf8(launcher.readAll()).trimmed()));
        return false;
    }
    QFile current(currentFile());
    if (current.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonObject object{{QStringLiteral("unit"), unit},
                           {QStringLiteral("log"), logPath},
                           {QStringLiteral("name"), m_runName},
                           {QStringLiteral("destroy_instance"), m_runDestroy}};
        current.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    appendLog(QStringLiteral("(systemd user unit %1; log %2)\n").arg(unit, logPath));
    attach(unit, logPath);
    return true;
}

bool RemoveCoreNodeController::reattach()
{
    if (isRunning() || currentFile().isEmpty())
        return false;
    QFile current(currentFile());
    if (!current.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(current.readAll()).object();
    const QString unit = object.value(QStringLiteral("unit")).toString();
    const QString logPath = object.value(QStringLiteral("log")).toString();
    if (!unit.startsWith(QLatin1String("fmnext-remove-core-")) || logPath.isEmpty())
        return false;
    m_run.reset();
    m_log.clear();
    m_runName = object.value(QStringLiteral("name")).toString();
    m_runDestroy = object.value(QStringLiteral("destroy_instance")).toBool();
    attach(unit, logPath);
    return true;
}

void RemoveCoreNodeController::attach(const QString &unit, const QString &logPath)
{
    m_unit = unit;
    m_logPath = logPath;
    m_logOffset = 0;
    m_pollTicks = 0;
    if (!m_poll) {
        m_poll = new QTimer(this);
        m_poll->setInterval(500);
        connect(m_poll, &QTimer::timeout, this, &RemoveCoreNodeController::pollDetached);
    }
    m_poll->start();
    emit runChanged();
    pollDetached();
}

void RemoveCoreNodeController::readDetachedLog()
{
    QFile file(m_logPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    if (file.size() < m_logOffset)
        m_logOffset = 0;
    if (!file.seek(m_logOffset))
        return;
    const QByteArray data = file.readAll();
    m_logOffset += data.size();
    if (data.isEmpty())
        return;
    appendLog(m_run.consume(QString::fromUtf8(data)));
    emit runChanged();
}

void RemoveCoreNodeController::pollDetached()
{
    if (m_unit.isEmpty())
        return;
    readDetachedLog();
    if (m_unitQuery || (m_pollTicks++ % 4) != 0)
        return;
    auto *query = new QProcess(this);
    m_unitQuery = query;
    connect(query, &QProcess::finished, this, [this, query](int, QProcess::ExitStatus) {
        const UnitState state = parseUnitState(query->readAllStandardOutput());
        query->deleteLater();
        if (m_unit.isEmpty() || state.running)
            return;
        finishDetached(state.exitStatus);
    });
    connect(query, &QProcess::errorOccurred, this, [query](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            query->deleteLater();
    });
    query->start(QStringLiteral("systemctl"),
                 {QStringLiteral("--user"), QStringLiteral("show"),
                  QStringLiteral("--property=ActiveState,LoadState,ExecMainStatus"), m_unit});
}

void RemoveCoreNodeController::finishDetached(int exitStatus)
{
    readDetachedLog();
    appendLog(m_run.flush());
    m_run.markProcessEnded(exitStatus, false);
    m_unit.clear();
    if (m_poll)
        m_poll->stop();
    if (!currentFile().isEmpty())
        QFile::remove(currentFile());
    emit runChanged();
    emit finished(m_run.state() == AddCoreRun::State::Succeeded);
}

void RemoveCoreNodeController::start(const QString &name, bool destroyInstance)
{
    if (!isRemovableCoreName(name))
        return;
    m_runName = name;
    m_runDestroy = destroyInstance;
    launch(removeCoreArguments(m_script, name, destroyInstance));
}

void RemoveCoreNodeController::retry()
{
    if (m_runName.isEmpty())
        return;
    launch(removeCoreArguments(m_script, m_runName, m_runDestroy));
}

void RemoveCoreNodeController::clearRun()
{
    if (isRunning())
        return;
    m_run.reset();
    m_log.clear();
    emit runChanged();
}

void RemoveCoreNodeController::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    m_log += text;
    if (m_log.size() > kMaxLogChars)
        m_log = m_log.right(kMaxLogChars);
    emit logAppended(text);
}

void RemoveCoreNodeController::launch(const QStringList &arguments)
{
    if (isRunning() || !isAvailable())
        return;
    m_run.reset();
    m_log.clear();
    m_lastArguments = arguments;
    if (launchDetached(arguments))
        return;
    QProcess *process = spawn(arguments, true);
    m_process = process;
    appendLog(QStringLiteral("$ %1 %2\n").arg(QFileInfo(process->program()).fileName(),
                                               arguments.join(QLatin1Char(' '))));
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        appendLog(m_run.consume(QString::fromUtf8(process->readAllStandardOutput())));
        emit runChanged();
    });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
                appendLog(m_run.consume(QString::fromUtf8(process->readAllStandardOutput())));
                appendLog(m_run.flush());
                m_run.markProcessEnded(exitCode, exitStatus != QProcess::NormalExit);
                process->deleteLater();
                emit runChanged();
                emit finished(m_run.state() == AddCoreRun::State::Succeeded);
            });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        appendLog(QStringLiteral("Could not start %1.\n").arg(process->program()));
        m_run.markProcessEnded(-1, true);
        process->deleteLater();
        emit runChanged();
        emit finished(false);
    });
    emit runChanged();
    process->start();
}

RemoveCoreNodeDialog::RemoveCoreNodeDialog(RemoveCoreNodeController *controller,
                                           const QString &name, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_name(name)
{
    setObjectName(QStringLiteral("removeCoreNodeDialog"));
    setWindowTitle(QStringLiteral("Remove core node %1").arg(name));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(720, 600);
    auto *layout = new QVBoxLayout(this);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildPlanPage());
    m_pages->addWidget(buildProgressPage());
    layout->addWidget(m_pages);
    if (!controller)
        return;
    connect(controller, &RemoveCoreNodeController::planChanged, this,
            &RemoveCoreNodeDialog::refreshPlan);
    connect(controller, &RemoveCoreNodeController::runChanged, this,
            &RemoveCoreNodeDialog::refreshRun);
    connect(controller, &RemoveCoreNodeController::logAppended, this,
            &RemoveCoreNodeDialog::appendLog);
    const bool ownRun = controller->runName() == name &&
                        controller->run().state() != AddCoreRun::State::Idle;
    if (ownRun) {
        m_log->setPlainText(controller->log());
        m_destroy->setChecked(controller->runDestroysInstance());
    }
    refreshRun();
    showPage(ownRun || (controller->isRunning() && controller->runName() == name)
                 ? Page::Progress
                 : Page::Plan);
    if (page() == Page::Plan) {
        if (!controller->isRunning())
            controller->clearRun();
        controller->loadPlan(name, false);
    }
    refreshPlan();
}

QWidget *RemoveCoreNodeDialog::buildPlanPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(titleLabel(QStringLiteral("Remove %1 from the ForkMesh fleet").arg(m_name)));
    m_quorum = mutedLabel(QStringLiteral("Checking the fleet and the rqlite cluster…"));
    m_quorum->setObjectName(QStringLiteral("removeCoreQuorum"));
    m_quorum->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_quorum);
    m_planText = new QLabel;
    m_planText->setObjectName(QStringLiteral("removeCorePlanText"));
    m_planText->setWordWrap(true);
    m_planText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_planText);
    layout->addStretch(1);
    m_destroy = new QCheckBox(QStringLiteral("Also delete the cloud instance (irreversible)"));
    m_destroy->setObjectName(QStringLiteral("removeCoreDestroy"));
    m_destroy->setToolTip(QStringLiteral(
        "Deletes the instance at the cloud provider after the core has left the fleet. "
        "Everything on it is gone. This is the only step that uses the provider API key."));
    connect(m_destroy, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_controller && !m_controller->isRunning())
            m_controller->loadPlan(m_name, checked);
        refreshPlan();
    });
    layout->addWidget(m_destroy);
    auto *confirmRow = new QHBoxLayout;
    confirmRow->addWidget(new QLabel(QStringLiteral("Type %1 to confirm").arg(m_name)));
    m_confirm = new QLineEdit;
    m_confirm->setObjectName(QStringLiteral("removeCoreConfirm"));
    m_confirm->setPlaceholderText(m_name);
    connect(m_confirm, &QLineEdit::textChanged, this, &RemoveCoreNodeDialog::refreshStartButton);
    confirmRow->addWidget(m_confirm, 1);
    layout->addLayout(confirmRow);
    m_blocker = new QLabel;
    m_blocker->setObjectName(QStringLiteral("removeCoreBlocker"));
    m_blocker->setWordWrap(true);
    layout->addWidget(m_blocker);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setObjectName(QStringLiteral("removeCoreCancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::close);
    m_start = new QPushButton(QStringLiteral("Remove %1").arg(m_name));
    m_start->setObjectName(QStringLiteral("removeCoreStart"));
    m_start->setEnabled(false);
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        const RemoveCorePlan &plan = m_controller->plan();
        if (m_controller->planName() != m_name ||
            !removeCoreBlocker(plan, m_controller->planLoading(), m_controller->isRunning(),
                               m_destroy->isChecked(), m_confirm->text())
                 .isEmpty())
            return;
        m_log->clear();
        showPage(Page::Progress);
        m_controller->start(m_name, m_destroy->isChecked());
    });
    buttons->addWidget(cancel);
    buttons->addWidget(m_start);
    layout->addLayout(buttons);
    return page;
}

QWidget *RemoveCoreNodeDialog::buildProgressPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_headline = new QLabel;
    m_headline->setObjectName(QStringLiteral("removeCoreHeadline"));
    m_headline->setWordWrap(true);
    QFont font = m_headline->font();
    font.setBold(true);
    m_headline->setFont(font);
    layout->addWidget(m_headline);
    m_steps = new QListWidget;
    m_steps->setObjectName(QStringLiteral("removeCoreSteps"));
    m_steps->setSelectionMode(QAbstractItemView::NoSelection);
    m_steps->setFocusPolicy(Qt::NoFocus);
    m_steps->setMinimumHeight(180);
    layout->addWidget(m_steps, 2);
    m_notes = mutedLabel();
    m_notes->setObjectName(QStringLiteral("removeCoreNotes"));
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_notes->hide();
    layout->addWidget(m_notes);
    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("removeCoreLog"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setMaximumBlockCount(20000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(m_log, 3);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(mutedLabel(QStringLiteral(
                           "Closing this window does not stop the run; reopen it from the "
                           "node's Remove button.")),
                       1);
    m_retry = new QPushButton(QStringLiteral("Retry"));
    m_retry->setObjectName(QStringLiteral("removeCoreRetry"));
    m_retry->hide();
    connect(m_retry, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        m_log->clear();
        m_controller->retry();
    });
    m_close = new QPushButton(QStringLiteral("Close"));
    m_close->setObjectName(QStringLiteral("removeCoreClose"));
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_close);
    layout->addLayout(buttons);
    return page;
}

RemoveCoreNodeDialog::Page RemoveCoreNodeDialog::page() const
{
    return static_cast<Page>(m_pages->currentIndex());
}

void RemoveCoreNodeDialog::showPage(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
}

void RemoveCoreNodeDialog::refreshPlan()
{
    if (!m_controller)
        return;
    const RemoveCorePlan &plan = m_controller->plan();
    const bool mine = m_controller->planName() == m_name;
    if (!mine || m_controller->planLoading()) {
        m_quorum->setText(QStringLiteral("Checking the fleet and the rqlite cluster…"));
    } else if (plan.valid) {
        m_quorum->setText(plan.quorum.isEmpty() ? plan.blocker : plan.quorum);
        m_planText->setText(removeCoreSummaryText(plan, m_destroy->isChecked()));
    } else {
        m_quorum->setText(plan.error);
        m_planText->clear();
    }
    m_start->setText(m_destroy->isChecked()
                         ? QStringLiteral("Remove %1 and delete its instance").arg(m_name)
                         : QStringLiteral("Remove %1").arg(m_name));
    refreshStartButton();
}

void RemoveCoreNodeDialog::refreshStartButton()
{
    if (!m_controller)
        return;
    const bool mine = m_controller->planName() == m_name;
    const RemoveCorePlan plan = mine ? m_controller->plan() : RemoveCorePlan();
    const bool loading = !mine || m_controller->planLoading();
    const QString blocker = removeCoreBlocker(plan, loading, m_controller->isRunning(),
                                              m_destroy->isChecked(), m_confirm->text());
    m_start->setEnabled(blocker.isEmpty());
    m_blocker->setText(blocker);
    m_blocker->setVisible(!blocker.isEmpty());
    const bool refused = !loading && (!plan.valid || !plan.allowed);
    m_blocker->setStyleSheet(refused ? QStringLiteral("color:#f85149;")
                                     : QStringLiteral("color:#8b949e;"));
    m_confirm->setEnabled(!refused);
}

void RemoveCoreNodeDialog::refreshRun()
{
    if (!m_controller)
        return;
    const AddCoreRun &run = m_controller->run();
    m_steps->clear();
    for (const AddCoreStep &step : run.steps()) {
        auto *item = new QListWidgetItem(stepDisplayText(step));
        item->setData(Qt::UserRole, step.id);
        if (step.status == AddCoreStep::Status::Failed)
            item->setForeground(QColor(QStringLiteral("#f85149")));
        else if (step.status == AddCoreStep::Status::Done)
            item->setForeground(QColor(QStringLiteral("#3fb950")));
        else if (step.status == AddCoreStep::Status::Running)
            item->setForeground(QColor(QStringLiteral("#d29922")));
        m_steps->addItem(item);
    }
    const bool running = m_controller->isRunning();
    QString headline = run.headline();
    if (run.state() == AddCoreRun::State::Succeeded && !run.commit().isEmpty())
        headline += QStringLiteral("\nMerge %1 (%2) into main so autodeploy and roll leave it "
                                   "out.")
                        .arg(run.branch(), run.commit().left(12));
    m_headline->setText(headline);
    QString color = QStringLiteral("inherit");
    if (run.state() == AddCoreRun::State::Succeeded)
        color = QStringLiteral("#3fb950");
    else if (run.state() == AddCoreRun::State::Failed)
        color = QStringLiteral("#f85149");
    m_headline->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    m_notes->setText(run.notes().join(QLatin1Char('\n')));
    m_notes->setVisible(!run.notes().isEmpty());
    m_retry->setVisible(!running && run.state() == AddCoreRun::State::Failed);
    m_close->setText(running ? QStringLiteral("Close (keeps running)") : QStringLiteral("Close"));
    refreshStartButton();
}

void RemoveCoreNodeDialog::appendLog(const QString &text)
{
    QScrollBar *bar = m_log->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(text);
    if (atBottom)
        bar->setValue(bar->maximum());
}

} // namespace forkmesh::fleet
