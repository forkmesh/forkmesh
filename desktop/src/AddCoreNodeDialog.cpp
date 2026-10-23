#include "AddCoreNodeDialog.h"
#include "ProcessReaper.h"
#include "Theme.h"

#include "FleetAutoscaleModel.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace forkmesh::fleet {

namespace {

constexpr int kMaxLogChars = 400 * 1024;
constexpr int kReadTimeoutMs = 120 * 1000;

QLabel *mutedLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e;")));
    return label;
}

} // namespace

AddCoreNodeController::AddCoreNodeController(const QString &script, QObject *parent)
    : QObject(parent), m_script(script)
{
}

AddCoreNodeController::~AddCoreNodeController()
{
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
}

bool AddCoreNodeController::isAvailable() const
{
    return !m_script.isEmpty() && QFileInfo(m_script).isFile();
}

bool AddCoreNodeController::isRunning() const
{
    return !m_unit.isEmpty() || (m_process && m_process->state() != QProcess::NotRunning);
}

QString AddCoreNodeController::currentFile() const
{
    return m_runDirectory.isEmpty() ? QString()
                                    : QDir(m_runDirectory).filePath(QStringLiteral("current.json"));
}

bool AddCoreNodeController::launchDetached(const QStringList &arguments)
{
    if (m_runDirectory.isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("systemd-run")).isEmpty())
        return false;
    if (!QDir().mkpath(m_runDirectory))
        return false;
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString unit = addCoreUnitName(stamp);
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
        QJsonObject object{{QStringLiteral("unit"), unit}, {QStringLiteral("log"), logPath}};
        current.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    appendLog(QStringLiteral("(systemd user unit %1; log %2)\n").arg(unit, logPath));
    attach(unit, logPath);
    return true;
}

bool AddCoreNodeController::reattach()
{
    if (isRunning() || currentFile().isEmpty())
        return false;
    QFile current(currentFile());
    if (!current.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(current.readAll()).object();
    const QString unit = object.value(QStringLiteral("unit")).toString();
    const QString logPath = object.value(QStringLiteral("log")).toString();
    if (!unit.startsWith(QLatin1String("fmnext-add-core-")) || logPath.isEmpty())
        return false;
    m_run.reset();
    m_log.clear();
    attach(unit, logPath);
    return true;
}

void AddCoreNodeController::attach(const QString &unit, const QString &logPath)
{
    m_unit = unit;
    m_logPath = logPath;
    m_logOffset = 0;
    m_pollTicks = 0;
    if (!m_poll) {
        m_poll = new QTimer(this);
        m_poll->setInterval(500);
        connect(m_poll, &QTimer::timeout, this, &AddCoreNodeController::pollDetached);
    }
    m_poll->start();
    emit runChanged();
    pollDetached();
}

void AddCoreNodeController::readDetachedLog()
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

void AddCoreNodeController::pollDetached()
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

void AddCoreNodeController::finishDetached(int exitStatus)
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

QProcess *AddCoreNodeController::spawn(const QStringList &arguments, bool merged)
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

void AddCoreNodeController::loadOptions()
{
    if (m_optionsLoading || !isAvailable())
        return;
    m_optionsLoading = true;
    QProcess *process = spawn(addCoreOptionsArguments(m_script, m_provider, m_replace), false);
    auto *deadline = new QTimer(process);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
        const QByteArray out = process->readAllStandardOutput();
        const QByteArray err = process->readAllStandardError();
        process->deleteLater();
        m_optionsLoading = false;
        if (m_reloadOptions) {
            m_reloadOptions = false;
            loadOptions();
            return;
        }
        m_options = parseAddCoreOptions(out);
        if (!m_options.valid) {
            const QString detail = QString::fromUtf8(err).trimmed().split(QLatin1Char('\n')).last();
            m_options.error = QStringLiteral("add-core-options failed (%1): %2")
                                  .arg(exitCode)
                                  .arg(detail.isEmpty() ? QStringLiteral("no output") : detail.left(300));
        }
        m_run.setDefaultSteps(AddCoreRun::stepsFromJson(out));
        emit optionsChanged();
        emit runChanged();
        if (m_run.state() == AddCoreRun::State::Idle)
            loadStatus();
        else
            m_statusLoaded = true;
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_optionsLoading = false;
        m_options = AddCoreOptions();
        m_options.error = QStringLiteral("could not start %1").arg(process->program());
        m_statusLoaded = true;
        emit optionsChanged();
    });
    deadline->start(kReadTimeoutMs);
    process->start();
}

void AddCoreNodeController::loadStatus()
{
    if (isRunning() || !isAvailable())
        return;
    QProcess *process = spawn({m_script, QStringLiteral("add-core-status"),
                               QStringLiteral("--json")},
                              false);
    connect(process, &QProcess::finished, this, [this, process](int, QProcess::ExitStatus) {
        const QByteArray out = process->readAllStandardOutput();
        process->deleteLater();
        m_statusLoaded = true;
        if (isRunning())
            return;
        AddCoreRun loaded = m_run;
        if (loaded.loadStatus(out) && loaded.state() == AddCoreRun::State::Failed) {
            m_run = loaded;
            emit runChanged();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_statusLoaded = true;
        process->deleteLater();
    });
    process->start();
}

void AddCoreNodeController::clearRun()
{
    if (isRunning())
        return;
    m_run.reset();
    m_log.clear();
    emit runChanged();
    loadOptions();
}

void AddCoreNodeController::setProvider(const QString &provider)
{
    const QString chosen = provider.isEmpty() ? QStringLiteral("vultr") : provider;
    if (chosen == m_provider)
        return;
    m_provider = chosen;
    if (m_optionsLoading) {
        m_reloadOptions = true;
        return;
    }
    loadOptions();
}

void AddCoreNodeController::setReplaceTarget(const QString &name)
{
    if (isRunning() || name == m_replace)
        return;
    m_replace = name;
    if (m_optionsLoading) {
        m_reloadOptions = true;
        return;
    }
    loadOptions();
}

QString AddCoreNodeController::activeReplaceTarget() const
{
    if (m_run.state() != AddCoreRun::State::Idle && !m_run.replaces().isEmpty())
        return m_run.replaces();
    if (m_run.state() == AddCoreRun::State::Idle && !m_options.unfinishedReplaces.isEmpty())
        return m_options.unfinishedReplaces;
    return m_run.state() == AddCoreRun::State::Idle ? m_replace : QString();
}

void AddCoreNodeController::start(const QString &region, const QString &plan)
{
    launch(addCoreArguments(m_script, region, plan, m_options.name, false, m_provider,
                            m_replace));
}

void AddCoreNodeController::retry()
{
    const QString name = m_run.name().isEmpty() ? m_options.unfinishedName : m_run.name();
    launch(addCoreArguments(m_script, QString(), QString(), name, true));
}

void AddCoreNodeController::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    m_log += text;
    if (m_log.size() > kMaxLogChars)
        m_log = m_log.right(kMaxLogChars);
    emit logAppended(text);
}

void AddCoreNodeController::launch(const QStringList &arguments)
{
    if (isRunning() || !isAvailable())
        return;
    m_run.reset();
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

AddCoreNodeDialog::AddCoreNodeDialog(AddCoreNodeController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setObjectName(QStringLiteral("addCoreNodeDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(720, 560);
    auto *layout = new QVBoxLayout(this);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildChoosePage());
    m_pages->addWidget(buildConfirmPage());
    m_pages->addWidget(buildProgressPage());
    layout->addWidget(m_pages);
    if (!controller)
        return;
    connect(controller, &AddCoreNodeController::optionsChanged, this,
            &AddCoreNodeDialog::refreshOptions);
    connect(controller, &AddCoreNodeController::runChanged, this, &AddCoreNodeDialog::refreshRun);
    connect(controller, &AddCoreNodeController::logAppended, this, &AddCoreNodeDialog::appendLog);
    m_log->setPlainText(controller->log());
    refreshTitle();
    refreshOptions();
    refreshRun();
    const AddCoreRun::State state = controller->run().state();
    showPage(state == AddCoreRun::State::Idle ? Page::Choose : Page::Progress);
    if (!controller->isRunning())
        controller->loadOptions();
}

QWidget *AddCoreNodeDialog::buildChoosePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_chooseTitle = new QLabel(QStringLiteral("Add a core node to the ForkMesh fleet"));
    m_chooseTitle->setObjectName(QStringLiteral("addCoreTitle"));
    QFont font = m_chooseTitle->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.2);
    m_chooseTitle->setFont(font);
    layout->addWidget(m_chooseTitle);
    m_chooseIntro = mutedLabel(QStringLiteral("Loading the fleet and the plan list…"));
    m_chooseIntro->setObjectName(QStringLiteral("addCoreIntro"));
    layout->addWidget(m_chooseIntro);
    auto *form = new QFormLayout;
    m_provider = new QComboBox;
    m_provider->setObjectName(QStringLiteral("addCoreProvider"));
    m_provider->addItem(QStringLiteral("Vultr"), QStringLiteral("vultr"));
    m_provider->addItem(QStringLiteral("AWS"), QStringLiteral("aws"));
    m_region = new QComboBox;
    m_region->setObjectName(QStringLiteral("addCoreRegion"));
    m_plan = new QComboBox;
    m_plan->setObjectName(QStringLiteral("addCorePlan"));
    m_cost = new QLabel;
    m_cost->setObjectName(QStringLiteral("addCoreCost"));
    m_time = new QLabel;
    m_time->setObjectName(QStringLiteral("addCoreTime"));
    m_role = mutedLabel();
    m_role->setObjectName(QStringLiteral("addCoreRole"));
    form->addRow(QStringLiteral("Cloud"), m_provider);
    form->addRow(QStringLiteral("Region"), m_region);
    form->addRow(QStringLiteral("Plan"), m_plan);
    form->addRow(QStringLiteral("Estimated cost"), m_cost);
    form->addRow(QStringLiteral("Estimated time"), m_time);
    form->addRow(QStringLiteral("rqlite"), m_role);
    layout->addLayout(form);
    m_chooseError = new QLabel;
    m_chooseError->setObjectName(QStringLiteral("addCoreError"));
    m_chooseError->setWordWrap(true);
    m_chooseError->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#f85149;")));
    m_chooseError->hide();
    layout->addWidget(m_chooseError);
    layout->addStretch(1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::close);
    m_next = new QPushButton(QStringLiteral("Next"));
    m_next->setObjectName(QStringLiteral("addCoreNext"));
    m_next->setDefault(true);
    m_next->setEnabled(false);
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        m_confirmText->setText(confirmationText(m_controller->options(), selectedRegion(),
                                                selectedPlan()));
        const QString newName = m_controller->options().name.isEmpty()
                                    ? QStringLiteral("the core")
                                    : m_controller->options().name;
        const QString replacing = m_controller->options().replaces;
        m_create->setText(replacing.isEmpty()
                              ? QStringLiteral("Create and join %1").arg(newName)
                              : QStringLiteral("Create %1 and remove %2").arg(newName, replacing));
        showPage(Page::Confirm);
    });
    buttons->addWidget(cancel);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this] {
        if (m_plansFilling || !m_controller)
            return;
        m_next->setEnabled(false);
        m_chooseIntro->setText(QStringLiteral("Loading the %1 plan list…")
                                   .arg(m_provider->currentText()));
        m_controller->setProvider(selectedProvider());
    });
    connect(m_region, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_plansFilling)
            refreshPlans();
    });
    connect(m_plan, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_plansFilling)
            refreshEstimate();
    });
    return page;
}

QWidget *AddCoreNodeDialog::buildConfirmPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *title = new QLabel(QStringLiteral("Confirm"));
    QFont font = title->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.2);
    title->setFont(font);
    layout->addWidget(title);
    m_confirmText = new QLabel;
    m_confirmText->setObjectName(QStringLiteral("addCoreConfirmText"));
    m_confirmText->setWordWrap(true);
    m_confirmText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_confirmText);
    layout->addStretch(1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *back = new QPushButton(QStringLiteral("Back"));
    back->setObjectName(QStringLiteral("addCoreBack"));
    connect(back, &QPushButton::clicked, this, [this] { showPage(Page::Choose); });
    m_create = new QPushButton(QStringLiteral("Create and join"));
    m_create->setObjectName(QStringLiteral("addCoreCreate"));
    m_create->setDefault(true);
    connect(m_create, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        m_log->clear();
        showPage(Page::Progress);
        m_controller->start(selectedRegion(), selectedPlan());
    });
    buttons->addWidget(back);
    buttons->addWidget(m_create);
    layout->addLayout(buttons);
    return page;
}

QWidget *AddCoreNodeDialog::buildProgressPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_headline = new QLabel;
    m_headline->setObjectName(QStringLiteral("addCoreHeadline"));
    m_headline->setWordWrap(true);
    QFont font = m_headline->font();
    font.setBold(true);
    m_headline->setFont(font);
    layout->addWidget(m_headline);
    m_steps = new QListWidget;
    m_steps->setObjectName(QStringLiteral("addCoreSteps"));
    m_steps->setSelectionMode(QAbstractItemView::NoSelection);
    m_steps->setFocusPolicy(Qt::NoFocus);
    m_steps->setMinimumHeight(180);
    layout->addWidget(m_steps, 2);
    m_notes = mutedLabel();
    m_notes->setObjectName(QStringLiteral("addCoreNotes"));
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_notes->hide();
    layout->addWidget(m_notes);
    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("addCoreLog"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setMaximumBlockCount(20000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(m_log, 3);
    auto *buttons = new QHBoxLayout;
    auto *hint = mutedLabel(QStringLiteral(
        "Closing this window does not stop the run; reopen it from Add core node or "
        "the core's Replace… button."));
    buttons->addWidget(hint, 1);
    m_retry = new QPushButton(QStringLiteral("Retry"));
    m_retry->setObjectName(QStringLiteral("addCoreRetry"));
    m_retry->hide();
    connect(m_retry, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        m_log->clear();
        m_controller->retry();
    });
    m_again = new QPushButton(QStringLiteral("Add another core"));
    m_again->setObjectName(QStringLiteral("addCoreAgain"));
    m_again->hide();
    connect(m_again, &QPushButton::clicked, this, [this] {
        if (!m_controller)
            return;
        m_log->clear();
        m_controller->clearRun();
        showPage(Page::Choose);
    });
    buttons->addWidget(m_again);
    m_close = new QPushButton(QStringLiteral("Close"));
    m_close->setObjectName(QStringLiteral("addCoreClose"));
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_close);
    layout->addLayout(buttons);
    return page;
}

AddCoreNodeDialog::Page AddCoreNodeDialog::page() const
{
    return static_cast<Page>(m_pages->currentIndex());
}

QString AddCoreNodeDialog::selectedRegion() const
{
    return m_region->currentData().toString();
}

QString AddCoreNodeDialog::selectedPlan() const
{
    return m_plan->currentData().toString();
}

QString AddCoreNodeDialog::selectedProvider() const
{
    return m_provider->currentData().toString();
}

void AddCoreNodeDialog::refreshTitle()
{
    const QString replacing = m_controller ? m_controller->activeReplaceTarget() : QString();
    setWindowTitle(replacing.isEmpty() ? QStringLiteral("Add core node")
                                       : QStringLiteral("Replace core node %1").arg(replacing));
    if (m_chooseTitle)
        m_chooseTitle->setText(
            replacing.isEmpty()
                ? QStringLiteral("Add a core node to the ForkMesh fleet")
                : QStringLiteral("Replace %1 with a new core node").arg(replacing));
}

void AddCoreNodeDialog::showPage(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
}

void AddCoreNodeDialog::refreshOptions()
{
    if (!m_controller)
        return;
    refreshTitle();
    const AddCoreOptions &options = m_controller->options();
    if (m_controller->optionsLoading() && !options.valid)
        return;
    QStringList problems;
    if (!options.error.isEmpty())
        problems.append(options.error);
    if (!options.unfinishedName.isEmpty() && m_controller->run().state() == AddCoreRun::State::Idle)
        problems.append(QStringLiteral("%1 is half added (stopped at %2); retry it instead of "
                                       "starting another core.")
                            .arg(options.unfinishedName, options.unfinishedFailedStep));
    if (options.valid) {
        QString intro =
            QStringLiteral("The new core will be %1 (fmnext-core-%2), at release %3. Region "
                           "and plan default to the existing cores'.")
                .arg(options.name, options.name.mid(1),
                     options.rev.isEmpty() ? QStringLiteral("(unknown)") : options.rev);
        if (!options.replaces.isEmpty())
            intro += QStringLiteral(" Once %1 serves, %2 is taken out of the fleet in the "
                                    "same run; its instance is kept.")
                         .arg(options.name, options.replaces);
        m_chooseIntro->setText(intro);
    }
    m_role->setText(options.voterReason.isEmpty()
                        ? QString()
                        : QStringLiteral("%1: %2").arg(options.voter ? QStringLiteral("voter")
                                                                      : QStringLiteral("non-voter"),
                                                       options.voterReason));
    m_plansFilling = true;
    m_provider->setCurrentIndex(qMax(0, m_provider->findData(m_controller->provider())));
    m_region->clear();
    for (const QString &region : options.regions)
        m_region->addItem(region, region);
    if (m_region->count() == 0 && !options.region.isEmpty())
        m_region->addItem(options.region, options.region);
    m_region->setCurrentIndex(qMax(0, m_region->findData(options.region)));
    m_plansFilling = false;
    refreshPlans();
    if (!options.plansError.isEmpty() && options.provider == QLatin1String("aws"))
        problems.append(QStringLiteral("AWS credentials for the fleet tools: %1. Enter them in "
                                       "Settings > Cloud and press \"Write aws.env for "
                                       "the fleet tools\".")
                            .arg(options.plansError));
    else if (!options.plansError.isEmpty())
        problems.append(QStringLiteral("%1 plan list unavailable: %2")
                            .arg(providerDisplayName(options.provider), options.plansError));
    m_chooseError->setText(problems.join(QLatin1Char('\n')));
    m_chooseError->setVisible(!problems.isEmpty());
    m_next->setEnabled(options.valid && options.error.isEmpty() && !options.name.isEmpty() &&
                       options.unfinishedName.isEmpty() && m_plan->count() > 0 &&
                       (options.provider != QLatin1String("aws") ||
                        options.plansError.isEmpty()));
    if (!options.unfinishedName.isEmpty() && m_controller->run().state() != AddCoreRun::State::Idle)
        showPage(Page::Progress);
}

void AddCoreNodeDialog::refreshPlans()
{
    if (!m_controller)
        return;
    const AddCoreOptions &options = m_controller->options();
    const QString region = selectedRegion();
    const QString keep = m_plan->currentData().toString();
    m_plansFilling = true;
    m_plan->clear();
    for (const AddCorePlan &plan : plansInRegion(options, region))
        m_plan->addItem(planLabel(plan), plan.id);
    if (m_plan->count() == 0 && !options.plan.isEmpty())
        m_plan->addItem(QStringLiteral("%1 · price unknown").arg(options.plan), options.plan);
    int index = m_plan->findData(keep.isEmpty() ? options.plan : keep);
    if (index < 0)
        index = m_plan->findData(options.plan);
    m_plan->setCurrentIndex(qMax(0, index));
    m_plansFilling = false;
    refreshEstimate();
}

void AddCoreNodeDialog::refreshEstimate()
{
    if (!m_controller)
        return;
    const AddCoreOptions &options = m_controller->options();
    const AddCorePlan *plan = findPlan(options, selectedPlan());
    m_cost->setText(plan ? formatMonthlyCost(plan->monthlyCost) +
                               (options.priceNote.isEmpty() ? QString()
                                                            : QStringLiteral(" (estimate)"))
                         : QStringLiteral("unknown (no plan list)"));
    m_cost->setToolTip(options.priceNote);
    m_time->setText(QStringLiteral("%1, most of it rolling the peer update over the "
                                   "existing cores one at a time")
                        .arg(formatDuration(options.estimateSeconds)));
}

void AddCoreNodeDialog::refreshRun()
{
    if (!m_controller)
        return;
    refreshTitle();
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
        headline += QStringLiteral("\nMerge %1 (%2) into main so autodeploy and roll include it.")
                        .arg(run.branch(), run.commit().left(12));
    m_headline->setText(headline);
    QString headlineColor = QStringLiteral("inherit");
    if (run.state() == AddCoreRun::State::Succeeded)
        headlineColor = QStringLiteral("#3fb950");
    else if (run.state() == AddCoreRun::State::Failed)
        headlineColor = QStringLiteral("#f85149");
    m_headline->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;").arg(headlineColor)));
    m_notes->setText(run.notes().join(QLatin1Char('\n')));
    m_notes->setVisible(!run.notes().isEmpty());
    m_retry->setVisible(!running && run.state() == AddCoreRun::State::Failed);
    m_again->setVisible(!running && run.state() == AddCoreRun::State::Succeeded);
    m_close->setText(running ? QStringLiteral("Close (keeps running)") : QStringLiteral("Close"));
    if (running || run.state() != AddCoreRun::State::Idle)
        showPage(Page::Progress);
}

void AddCoreNodeDialog::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    QScrollBar *bar = m_log->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 4;
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(text);
    if (atEnd)
        bar->setValue(bar->maximum());
}

} // namespace forkmesh::fleet
