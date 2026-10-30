#include "FleetAutoscalePanel.h"

#include "CoreNodeLogs.h"
#include "FontSizing.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace as = forkmesh::autoscale;

namespace {

QSpinBox *spin(int min, int max, const QString &suffix, const QString &name)
{
    auto *box = new QSpinBox;
    box->setRange(min, max);
    box->setSuffix(suffix);
    box->setObjectName(name);
    return box;
}

QDoubleSpinBox *doubleSpin(double min, double max, double step, int decimals,
                           const QString &prefix, const QString &name)
{
    auto *box = new QDoubleSpinBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setPrefix(prefix);
    box->setObjectName(name);
    return box;
}

QLabel *muted(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

}

FleetAutoscalePanel::FleetAutoscalePanel(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("fleetAutoscalePanel"));
    setFrameShape(QFrame::StyledPanel);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 10, 12, 10);
    outer->setSpacing(8);

    auto *titleRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Fleet autoscaling"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont = forkmesh::resizedFont(titleFont, 1);
    title->setFont(titleFont);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    m_refresh = new QPushButton(QStringLiteral("Refresh"));
    m_refresh->setObjectName(QStringLiteral("fleetAutoscaleRefresh"));
    m_refresh->setCursor(Qt::PointingHandCursor);
    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    titleRow->addWidget(m_refresh);
    m_save = new QPushButton(QStringLiteral("Save settings"));
    m_save->setObjectName(QStringLiteral("fleetAutoscaleSave"));
    m_save->setCursor(Qt::PointingHandCursor);
    m_save->setToolTip(QStringLiteral(
        "Validate both pools, sign the settings with the operator key and "
        "write them where the scaler service reads them. The scaler refuses "
        "unsigned or altered settings."));
    connect(m_save, &QPushButton::clicked, this, [this] { save(); });
    titleRow->addWidget(m_save);
    outer->addLayout(titleRow);

    m_killSwitch = new QCheckBox(QStringLiteral(
        "Kill switch: the cores take no scaling action at all"));
    m_killSwitch->setObjectName(QStringLiteral("fleetAutoscaleKillSwitch"));
    QFont killFont = m_killSwitch->font();
    killFont.setBold(true);
    m_killSwitch->setFont(killFont);
    m_killSwitch->setToolTip(QStringLiteral(
        "On: every core stops creating and destroying servers, whatever the "
        "pools say, until it is switched off again. Takes effect when saved."));
    connect(m_killSwitch, &QCheckBox::toggled, this, [this] {
        if (m_formLoaded)
            m_formDirty = true;
    });
    outer->addWidget(m_killSwitch);

    outer->addWidget(muted(QStringLiteral(
        "Adds and removes ForkMesh servers on Vultr as load changes. App "
        "capacity adds read-replica app servers (fmnext-auto-N, shown aN) "
        "behind the shared app and world names; burst runners "
        "(fmnext-burst-N, shown bN) take queued agent jobs. The three core "
        "servers and the always-on runner are never scaled. The scaler runs "
        "leaderless on the core servers, so it keeps working while this app "
        "and this computer are off; this panel edits its signed settings and "
        "shows its status. Every action is logged and Pinged to the admins; "
        "each pool makes at most one change per cooldown and stops at its "
        "monthly cap.")));

    m_summary = muted();
    m_summary->setObjectName(QStringLiteral("fleetAutoscaleSummary"));
    outer->addWidget(m_summary);
    m_settingsState = muted();
    m_settingsState->setObjectName(QStringLiteral("fleetAutoscaleSettingsState"));
    outer->addWidget(m_settingsState);

    m_app.runner = false;
    m_runner.runner = true;
    auto *pools = new QVBoxLayout;
    pools->setSpacing(12);
    pools->addWidget(buildPool(m_app, QStringLiteral("App capacity")), 1);
    pools->addWidget(buildPool(m_runner, QStringLiteral("Burst runners")), 1);
    outer->addLayout(pools);

    m_recent = muted();
    m_recent->setObjectName(QStringLiteral("fleetAutoscaleRecent"));
    outer->addWidget(m_recent);

    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("fleetAutoscaleLog"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setMinimumHeight(120);
    m_log->setVisible(false);
    outer->addWidget(m_log);

    m_timer = new QTimer(this);
    m_timer->setInterval(60 * 1000);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (isVisible())
            refresh();
    });

    applySettings(as::defaultSettings());
    m_formLoaded = false;
    m_formDirty = false;
    m_summary->setText(QStringLiteral("Status not loaded yet."));
}

FleetAutoscalePanel::~FleetAutoscalePanel()
{
    for (QProcess *process : {m_statusProcess.data(), m_actionProcess.data()}) {
        if (process) {
            process->disconnect(this);
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QWidget *FleetAutoscalePanel::buildPool(PoolWidgets &pool, const QString &title)
{
    const QString key = pool.runner ? QStringLiteral("Runner") : QStringLiteral("App");
    auto *box = new QFrame;
    box->setObjectName(QStringLiteral("fleetAutoscale%1Pool").arg(key));
    box->setFrameShape(QFrame::StyledPanel);
    auto *col = new QVBoxLayout(box);
    col->setContentsMargins(10, 8, 10, 8);
    col->setSpacing(6);
    auto *heading = new QLabel(title);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    col->addWidget(heading);

    auto *switches = new QHBoxLayout;
    pool.enabled = new QCheckBox(QStringLiteral("Enabled"));
    pool.enabled->setObjectName(QStringLiteral("fleetAutoscale%1Enabled").arg(key));
    pool.enabled->setToolTip(QStringLiteral(
        "Off: the scaler observes and decides but never changes the fleet."));
    pool.dryRun = new QCheckBox(QStringLiteral("Dry run only"));
    pool.dryRun->setObjectName(QStringLiteral("fleetAutoscale%1DryRun").arg(key));
    pool.dryRun->setToolTip(QStringLiteral(
        "Log and Ping what the scaler would do, without creating or destroying "
        "anything."));
    switches->addWidget(pool.enabled);
    switches->addWidget(pool.dryRun);
    switches->addStretch(1);
    col->addLayout(switches);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(5);
    pool.min = spin(0, as::kMaxNodesPerPool, QString(),
                    QStringLiteral("fleetAutoscale%1Min").arg(key));
    pool.max = spin(0, as::kMaxNodesPerPool, QString(),
                    QStringLiteral("fleetAutoscale%1Max").arg(key));
    auto *counts = new QHBoxLayout;
    counts->addWidget(new QLabel(QStringLiteral("min")));
    counts->addWidget(pool.min);
    counts->addWidget(new QLabel(QStringLiteral("max")));
    counts->addWidget(pool.max);
    counts->addStretch(1);
    form->addRow(pool.runner ? QStringLiteral("Burst runners")
                             : QStringLiteral("Extra app servers"),
                 counts);
    if (pool.runner) {
        pool.upQueueWait = spin(1, 240, QStringLiteral(" min"),
                                QStringLiteral("fleetAutoscaleRunnerQueueWait"));
        pool.upQueueDepth = spin(1, 100, QStringLiteral(" job(s)"),
                                 QStringLiteral("fleetAutoscaleRunnerQueueDepth"));
        auto *up = new QHBoxLayout;
        up->addWidget(pool.upQueueDepth);
        up->addWidget(new QLabel(QStringLiteral("waiting longer than")));
        up->addWidget(pool.upQueueWait);
        up->addStretch(1);
        form->addRow(QStringLiteral("Scale up when"), up);
        pool.downIdle = spin(1, 1440, QStringLiteral(" min"),
                             QStringLiteral("fleetAutoscaleRunnerIdle"));
        form->addRow(QStringLiteral("Destroy after idle"), pool.downIdle);
    } else {
        pool.upP95 = spin(0, 60000, QStringLiteral(" ms"),
                          QStringLiteral("fleetAutoscaleAppP95"));
        pool.upLoopLag = spin(0, 60000, QStringLiteral(" ms"),
                              QStringLiteral("fleetAutoscaleAppLoopLag"));
        pool.upLoad = doubleSpin(0, 16, 0.05, 2, QString(),
                                 QStringLiteral("fleetAutoscaleAppLoad"));
        pool.upRps = spin(0, 100000, QStringLiteral(" req/s"),
                          QStringLiteral("fleetAutoscaleAppRps"));
        pool.upFor = spin(1, 240, QStringLiteral(" min"),
                          QStringLiteral("fleetAutoscaleAppUpFor"));
        const QString zeroOff = QStringLiteral("0 turns this signal off.");
        pool.upP95->setToolTip(QStringLiteral("p95 request latency on the "
                                              "busiest serving node. ") + zeroOff);
        pool.upLoopLag->setToolTip(QStringLiteral("App event-loop lag (p95). ") +
                                   zeroOff);
        pool.upLoad->setToolTip(QStringLiteral(
            "One-minute load average divided by CPU count. ") + zeroOff);
        pool.upRps->setToolTip(QStringLiteral(
            "Requests per second per serving node. ") + zeroOff);
        form->addRow(QStringLiteral("Scale up: p95 above"), pool.upP95);
        form->addRow(QStringLiteral("or loop lag above"), pool.upLoopLag);
        form->addRow(QStringLiteral("or load per CPU above"), pool.upLoad);
        form->addRow(QStringLiteral("or requests above"), pool.upRps);
        form->addRow(QStringLiteral("sustained for"), pool.upFor);
        pool.downFraction = doubleSpin(0.05, 0.95, 0.05, 2, QString(),
                                       QStringLiteral("fleetAutoscaleAppDownFraction"));
        pool.downFraction->setToolTip(QStringLiteral(
            "Scale down when every signal stays under this fraction of its "
            "scale-up threshold."));
        pool.downFor = spin(1, 1440, QStringLiteral(" min"),
                            QStringLiteral("fleetAutoscaleAppDownFor"));
        auto *down = new QHBoxLayout;
        down->addWidget(new QLabel(QStringLiteral("all signals under")));
        down->addWidget(pool.downFraction);
        down->addWidget(new QLabel(QStringLiteral("× threshold for")));
        down->addWidget(pool.downFor);
        down->addStretch(1);
        form->addRow(QStringLiteral("Scale down when"), down);
    }
    pool.snapshot = new QLineEdit;
    pool.snapshot->setObjectName(QStringLiteral("fleetAutoscale%1Snapshot").arg(key));
    pool.snapshot->setPlaceholderText(QStringLiteral(
        "Vultr snapshot id (empty: install from Debian, slower)"));
    form->addRow(QStringLiteral("Boot snapshot"), pool.snapshot);
    pool.snapshotBuilt = new QLabel(QStringLiteral("—"));
    pool.snapshotBuilt->setObjectName(
        QStringLiteral("fleetAutoscale%1SnapshotBuilt").arg(key));
    pool.snapshotBuilt->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Snapshot built"), pool.snapshotBuilt);
    pool.cooldown = spin(as::kMinCooldownMinutes, 1440, QStringLiteral(" min"),
                         QStringLiteral("fleetAutoscale%1Cooldown").arg(key));
    form->addRow(QStringLiteral("Cooldown between changes"), pool.cooldown);
    pool.plan = new QLineEdit;
    pool.plan->setObjectName(QStringLiteral("fleetAutoscale%1Plan").arg(key));
    pool.region = new QLineEdit;
    pool.region->setObjectName(QStringLiteral("fleetAutoscale%1Region").arg(key));
    pool.region->setMaximumWidth(90);
    auto *placement = new QHBoxLayout;
    placement->addWidget(pool.plan, 1);
    placement->addWidget(new QLabel(QStringLiteral("in")));
    placement->addWidget(pool.region);
    form->addRow(QStringLiteral("Vultr plan"), placement);
    pool.cap = doubleSpin(0, as::kMaxMonthlyCapCents / 100.0, 1, 2,
                          QStringLiteral("$"),
                          QStringLiteral("fleetAutoscale%1Cap").arg(key));
    pool.cap->setToolTip(QStringLiteral(
        "Spend cap for this pool's scaled servers each calendar month (UTC). "
        "At the cap the scaler creates nothing more and destroys what it "
        "added."));
    form->addRow(QStringLiteral("Monthly spend cap"), pool.cap);
    col->addLayout(form);

    pool.status = muted();
    pool.status->setObjectName(QStringLiteral("fleetAutoscale%1Status").arg(key));
    col->addWidget(pool.status);

    pool.nodes = new QTableWidget(0, 5);
    pool.nodes->setObjectName(QStringLiteral("fleetAutoscale%1Nodes").arg(key));
    pool.nodes->setHorizontalHeaderLabels(
        {QStringLiteral("Node"), QStringLiteral("Label"), QStringLiteral("Address"),
         QStringLiteral("State"), QStringLiteral("Age")});
    pool.nodes->verticalHeader()->setVisible(false);
    pool.nodes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pool.nodes->setSelectionBehavior(QAbstractItemView::SelectRows);
    pool.nodes->setShowGrid(false);
    pool.nodes->horizontalHeader()->setStretchLastSection(true);
    pool.nodes->setMinimumHeight(90);
    pool.nodes->setMaximumHeight(150);
    col->addWidget(pool.nodes);

    auto *actions = new QHBoxLayout;
    pool.scaleUp = new QPushButton(QStringLiteral("Scale up now"));
    pool.scaleUp->setObjectName(QStringLiteral("fleetAutoscale%1ScaleUp").arg(key));
    pool.scaleUp->setCursor(Qt::PointingHandCursor);
    pool.scaleDown = new QPushButton(QStringLiteral("Scale down now"));
    pool.scaleDown->setObjectName(
        QStringLiteral("fleetAutoscale%1ScaleDown").arg(key));
    pool.scaleDown->setCursor(Qt::PointingHandCursor);
    const bool runner = pool.runner;
    connect(pool.scaleUp, &QPushButton::clicked, this,
            [this, runner] { scale(runner, true); });
    connect(pool.scaleDown, &QPushButton::clicked, this,
            [this, runner] { scale(runner, false); });
    actions->addWidget(pool.scaleUp);
    actions->addWidget(pool.scaleDown);
    actions->addStretch(1);
    col->addLayout(actions);

    const auto markDirty = [this] {
        if (m_formLoaded)
            m_formDirty = true;
    };
    for (QSpinBox *box : box->findChildren<QSpinBox *>())
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, markDirty);
    for (QDoubleSpinBox *box : box->findChildren<QDoubleSpinBox *>())
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                markDirty);
    for (QCheckBox *check : box->findChildren<QCheckBox *>())
        connect(check, &QCheckBox::toggled, this, markDirty);
    for (QLineEdit *edit : box->findChildren<QLineEdit *>()) {
        if (!qobject_cast<QAbstractSpinBox *>(edit->parentWidget()))
            connect(edit, &QLineEdit::textEdited, this, markDirty);
    }
    return box;
}

void FleetAutoscalePanel::fillPool(PoolWidgets &pool, const as::PoolSettings &settings)
{
    pool.enabled->setChecked(settings.enabled);
    pool.dryRun->setChecked(settings.dryRun);
    pool.min->setValue(settings.min);
    pool.max->setValue(settings.max);
    if (pool.runner) {
        pool.upQueueWait->setValue(settings.upQueueWaitMinutes);
        pool.upQueueDepth->setValue(settings.upQueueDepth);
        pool.downIdle->setValue(settings.downIdleMinutes);
    } else {
        pool.upP95->setValue(settings.upP95Ms);
        pool.upLoopLag->setValue(settings.upLoopLagMs);
        pool.upLoad->setValue(settings.upLoadPerCpu);
        pool.upRps->setValue(settings.upRps);
        pool.upFor->setValue(settings.upForMinutes);
        pool.downFraction->setValue(settings.downFraction);
        pool.downFor->setValue(settings.downForMinutes);
    }
    pool.snapshot->setText(settings.snapshotId);
    pool.cooldown->setValue(settings.cooldownMinutes);
    pool.plan->setText(settings.plan);
    pool.region->setText(settings.region);
    pool.cap->setValue(settings.monthlyCapCents / 100.0);
}

as::PoolSettings FleetAutoscalePanel::readPool(const PoolWidgets &pool) const
{
    as::PoolSettings settings =
        pool.runner ? as::defaultSettings().runner : as::defaultSettings().app;
    settings.enabled = pool.enabled->isChecked();
    settings.dryRun = pool.dryRun->isChecked();
    settings.min = pool.min->value();
    settings.max = pool.max->value();
    if (pool.runner) {
        settings.upQueueWaitMinutes = pool.upQueueWait->value();
        settings.upQueueDepth = pool.upQueueDepth->value();
        settings.downIdleMinutes = pool.downIdle->value();
    } else {
        settings.upP95Ms = pool.upP95->value();
        settings.upLoopLagMs = pool.upLoopLag->value();
        settings.upLoadPerCpu = pool.upLoad->value();
        settings.upRps = pool.upRps->value();
        settings.upForMinutes = pool.upFor->value();
        settings.downFraction = pool.downFraction->value();
        settings.downForMinutes = pool.downFor->value();
    }
    settings.snapshotId = pool.snapshot->text().trimmed().toLower();
    settings.cooldownMinutes = pool.cooldown->value();
    settings.plan = pool.plan->text().trimmed().toLower();
    settings.region = pool.region->text().trimmed().toLower();
    settings.monthlyCapCents = int(std::lround(pool.cap->value() * 100.0));
    return settings;
}

void FleetAutoscalePanel::applySettings(const as::Settings &settings)
{
    m_formLoaded = false;
    m_killSwitch->setChecked(settings.killSwitch);
    fillPool(m_app, settings.app);
    fillPool(m_runner, settings.runner);
    m_formLoaded = true;
    m_formDirty = false;
}

as::Settings FleetAutoscalePanel::settingsFromForm() const
{
    as::Settings settings = as::defaultSettings();
    settings.killSwitch = m_killSwitch->isChecked();
    settings.app = readPool(m_app);
    settings.runner = readPool(m_runner);
    return settings;
}

void FleetAutoscalePanel::showPoolStatus(PoolWidgets &pool,
                                         const as::PoolStatus &status,
                                         const as::PoolSettings &settings)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString mode = !settings.enabled ? QStringLiteral("off")
                   : settings.dryRun ? QStringLiteral("dry run")
                                     : QStringLiteral("live");
    pool.status->setText(
        QStringLiteral("%1 · %2 node(s) · spent %3 of %4 this month · %5\n%6")
            .arg(mode)
            .arg(status.nodes.size())
            .arg(as::formatDollars(status.spentCents),
                 as::formatDollars(settings.monthlyCapCents),
                 as::formatSignals(pool.runner, status.signalValues),
                 as::decisionLine(status, now)));
    pool.snapshotBuilt->setText(
        status.snapshotId.isEmpty()
            ? QStringLiteral("no snapshot reported")
            : QStringLiteral("%1, built %2")
                  .arg(status.snapshotId,
                       status.snapshotBuiltMs > 0
                           ? QDateTime::fromMSecsSinceEpoch(status.snapshotBuiltMs)
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                           : QStringLiteral("at an unknown time")));
    pool.nodes->setRowCount(status.nodes.size());
    for (int row = 0; row < status.nodes.size(); ++row) {
        const as::NodeRow &node = status.nodes.at(row);
        const QStringList cells{node.name, node.label, node.ip, node.state,
                                as::formatAge(node.createdMs, now)};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            if (column == 1 && !node.instanceId.isEmpty())
                item->setToolTip(QStringLiteral("Vultr instance ") + node.instanceId);
            pool.nodes->setItem(row, column, item);
        }
    }
}

void FleetAutoscalePanel::applyStatus(const as::Status &status)
{
    if (!status.parsed) {
        m_summary->setText(status.error.isEmpty()
                               ? QStringLiteral("The scaler printed no status.")
                               : status.error);
        return;
    }
    if (!m_formDirty)
        applySettings(status.settings);
    if (status.ok)
        forkmesh::corelogs::writeAutoscaledInstances(
            as::autoscaledInstancesJson(status));
    const QString service =
        as::serviceLine(status, QDateTime::currentMSecsSinceEpoch());
    m_summary->setText(status.ok || status.error.isEmpty()
                           ? service
                           : service + QStringLiteral(" ") + status.error);
    if (status.verified) {
        m_settingsState->setText(
            QStringLiteral("Settings are signed with the operator key and verified."));
    } else {
        m_settingsState->setText(
            QStringLiteral("Settings are NOT verified (%1): the cores use safe "
                           "defaults with both pools off until they are saved "
                           "and signed here.")
                .arg(status.settingsError.isEmpty() ? QStringLiteral("unsigned")
                                                    : status.settingsError));
    }
    showPoolStatus(m_app, status.app, status.settings.app);
    showPoolStatus(m_runner, status.runner, status.settings.runner);
    QStringList recent;
    for (const as::RecentEvent &event : status.recent.mid(
             qMax(0, int(status.recent.size()) - 5))) {
        recent.append(QStringLiteral("%1 %2 %3%4: %5")
                          .arg(QDateTime::fromMSecsSinceEpoch(event.ts)
                                   .toString(QStringLiteral("MM-dd HH:mm")),
                               event.pool, event.event,
                               event.ok ? QString() : QStringLiteral(" (failed)"),
                               event.detail));
    }
    m_recent->setText(recent.isEmpty()
                          ? QStringLiteral("No scaling events yet.")
                          : QStringLiteral("Recent: ") + recent.join(QLatin1Char('\n')));
}

QString FleetAutoscalePanel::statusText() const
{
    return m_summary->text() + QLatin1Char('\n') + m_settingsState->text() +
           QLatin1Char('\n') + m_app.status->text() + QLatin1Char('\n') +
           m_runner.status->text();
}

int FleetAutoscalePanel::nodeRowCount(bool runnerPool) const
{
    return (runnerPool ? m_runner : m_app).nodes->rowCount();
}

void FleetAutoscalePanel::setScriptPath(const QString &path)
{
    m_script = path;
}

QString FleetAutoscalePanel::python() const
{
    return as::fleetPython(m_script);
}

void FleetAutoscalePanel::refresh()
{
    if (m_script.isEmpty()) {
        m_summary->setText(QStringLiteral(
            "tools/next_autoscale.py was not found next to this app or in its "
            "source checkout."));
        return;
    }
    if (m_statusProcess)
        return;
    auto *process = new QProcess(this);
    m_statusProcess = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto *deadline = new QTimer(process);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
                const QByteArray out = process->readAllStandardOutput();
                const QByteArray err = process->readAllStandardError();
                process->deleteLater();
                as::Status status = as::parseStatus(out);
                if (!status.parsed) {
                    const QString detail =
                        QString::fromUtf8(err).trimmed().split(QLatin1Char('\n')).last();
                    status.error = QStringLiteral("Status failed (%1): %2")
                                       .arg(exitStatus == QProcess::NormalExit
                                                ? QString::number(exitCode)
                                                : QStringLiteral("killed"),
                                            detail.isEmpty()
                                                ? QStringLiteral("no output")
                                                : detail.left(300));
                }
                applyStatus(status);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                m_summary->setText(QStringLiteral("Could not start %1.").arg(python()));
            });
    deadline->start(90 * 1000);
    process->start(python(), {m_script, QStringLiteral("status"), QStringLiteral("--json")});
}

void FleetAutoscalePanel::save()
{
    const as::Settings settings = settingsFromForm();
    const QString invalid = as::validateSettings(settings);
    if (!invalid.isEmpty()) {
        m_settingsState->setText(QStringLiteral("Not saved: ") + invalid);
        return;
    }
    if (m_script.isEmpty() || m_actionProcess)
        return;
    const bool goingLive =
        !settings.killSwitch &&
        ((settings.app.enabled && !settings.app.dryRun) ||
         (settings.runner.enabled && !settings.runner.dryRun));
    if (goingLive &&
        QMessageBox::question(
            this, QStringLiteral("Enable live autoscaling?"),
            QStringLiteral("A pool is enabled without dry run: the cores will "
                           "create and destroy Vultr servers on their own, "
                           "within the limits and caps shown. Save?")) !=
            QMessageBox::Yes)
        return;
    auto *process = new QProcess(this);
    m_actionProcess = process;
    setBusy(true);
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus) {
                const QByteArray out = process->readAllStandardOutput();
                const QByteArray err = process->readAllStandardError();
                process->deleteLater();
                setBusy(false);
                QByteArray body = out.trimmed();
                const int brace = body.lastIndexOf("\n{");
                if (brace >= 0)
                    body = body.mid(brace + 1);
                const QJsonObject reply = QJsonDocument::fromJson(body).object();
                if (exitCode == 0 && reply.value(QStringLiteral("ok")).toBool()) {
                    m_formDirty = false;
                    m_settingsState->setText(QStringLiteral("Saved and signed."));
                    refresh();
                    return;
                }
                QString error = reply.value(QStringLiteral("error")).toString();
                if (error.isEmpty())
                    error = QString::fromUtf8(err).trimmed().left(300);
                m_settingsState->setText(QStringLiteral("Not saved: ") +
                                         (error.isEmpty() ? QStringLiteral("unknown error")
                                                          : error));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                setBusy(false);
                m_settingsState->setText(QStringLiteral("Not saved: could not start %1.")
                                             .arg(python()));
            });
    process->start(python(), {m_script, QStringLiteral("settings"), QStringLiteral("set")});
    process->write(QJsonDocument(as::settingsToJson(settings)).toJson(QJsonDocument::Compact));
    process->closeWriteChannel();
}

void FleetAutoscalePanel::scale(bool runnerPool, bool up)
{
    if (m_script.isEmpty() || m_actionProcess)
        return;
    const as::PoolSettings settings = readPool(runnerPool ? m_runner : m_app);
    const QString pool = runnerPool ? QStringLiteral("burst runner")
                                    : QStringLiteral("app replica");
    const QString text =
        up ? QStringLiteral("Create one %1 now?\n\n%2\n\nThe request is "
                            "queued on the cluster and runs on a core; the "
                            "pool maximum, the spend cap, the cooldown and the "
                            "kill switch still apply.")
                 .arg(pool, as::scaleUpCostNote(settings))
           : QStringLiteral("Remove the newest %1 now? It is drained first "
                            "(taken out of DNS or its job queue), then "
                            "destroyed on Vultr, which stops its billing. The "
                            "request is queued on the cluster and runs on a "
                            "core; the pool minimum still applies.")
                 .arg(pool);
    if (QMessageBox::question(this,
                              up ? QStringLiteral("Scale up now")
                                 : QStringLiteral("Scale down now"),
                              text) != QMessageBox::Yes)
        return;
    auto *process = new QProcess(this);
    m_actionProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_log->setVisible(true);
    appendLog(QStringLiteral("$ next_autoscale.py scale %1 %2 --execute\n")
                  .arg(runnerPool ? QStringLiteral("runner") : QStringLiteral("app"),
                       up ? QStringLiteral("up") : QStringLiteral("down")));
    setBusy(true);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        appendLog(QString::fromUtf8(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
                appendLog(QString::fromUtf8(process->readAllStandardOutput()));
                appendLog(exitStatus == QProcess::NormalExit && exitCode == 0
                              ? QStringLiteral("Queued; progress shows in the "
                                               "status above.\n")
                              : QStringLiteral("Failed (exit %1).\n").arg(exitCode));
                process->deleteLater();
                setBusy(false);
                refresh();
                QTimer::singleShot(20 * 1000, this, [this] {
                    if (isVisible())
                        refresh();
                });
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                appendLog(QStringLiteral("Could not start %1.\n").arg(python()));
                process->deleteLater();
                setBusy(false);
            });
    process->start(python(), as::scaleArguments(m_script, runnerPool, up, true));
}

void FleetAutoscalePanel::setBusy(bool busy)
{
    for (QPushButton *button : {m_save, m_app.scaleUp, m_app.scaleDown,
                                m_runner.scaleUp, m_runner.scaleDown})
        button->setEnabled(!busy);
}

void FleetAutoscalePanel::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(text);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void FleetAutoscalePanel::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);
    refresh();
    m_timer->start();
}

void FleetAutoscalePanel::hideEvent(QHideEvent *event)
{
    QFrame::hideEvent(event);
    m_timer->stop();
}
