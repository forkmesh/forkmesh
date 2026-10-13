#include "SystemdTimersPanel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <utility>

namespace forkmesh::systemdtimers {

namespace {

class PanelWidget : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void()> onShown;

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (onShown)
            onShown();
    }
};

struct PanelState {
    PanelOptions options;
    QPointer<QWidget> panel;
    QPointer<QWidget> rowsHost;
    QPointer<QLabel> status;
    QPointer<QPushButton> refresh;
    QList<Timer> timers;
    QElapsedTimer sinceRefresh;
    QString notice;
    bool noticeIsProblem = false;
    bool busy = false;

    void say(const QString &text, bool problem = false)
    {
        status->setText(text);
        status->setStyleSheet(problem ? QStringLiteral("color:#f85149;") : QString());
    }

    void setBusy(bool value)
    {
        busy = value;
        refresh->setEnabled(!value);
        for (QPushButton *button : rowsHost->findChildren<QPushButton *>())
            button->setEnabled(!value && button->property("usable").toBool());
    }
};

using State = std::shared_ptr<PanelState>;

void refreshNow(const State &state);

QString stateText(const Timer &timer, qint64 now)
{
    QStringList parts;
    parts << (timer.enabled() ? QStringLiteral("On") : QStringLiteral("Off"));
    if (timer.serviceActiveState == QLatin1String("active")
        || timer.serviceActiveState == QLatin1String("activating"))
        parts << QStringLiteral("running now");
    if (timer.enabled()) {
        const QString next = relativeTime(timer.nextUsec, now);
        parts << (next.isEmpty() ? QStringLiteral("no next run scheduled")
                                 : QStringLiteral("next run %1").arg(next));
    }
    const QString last = relativeTime(timer.lastUsec, now);
    if (!last.isEmpty())
        parts << QStringLiteral("last run %1").arg(last);
    if (!timer.serviceResult.isEmpty() && timer.serviceResult != QLatin1String("success"))
        parts << QStringLiteral("last result: %1").arg(timer.serviceResult);
    return parts.join(QStringLiteral(" · "));
}

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void runCommands(const State &state, const QList<QStringList> &commands, const QString &doing,
                 const QString &done)
{
    if (state->busy)
        return;
    state->setBusy(true);
    state->say(doing);
    const Runner runner = state->options.runner;
    auto *watcher = new QFutureWatcher<QString>(state->panel);
    QObject::connect(watcher, &QFutureWatcherBase::finished, state->panel,
                     [state, watcher, done] {
                         const QString error = watcher->result();
                         watcher->deleteLater();
                         state->setBusy(false);
                         state->notice = error.isEmpty() ? done : error;
                         state->noticeIsProblem = !error.isEmpty();
                         state->say(state->notice, state->noticeIsProblem);
                         refreshNow(state);
                     });
    watcher->setFuture(QtConcurrent::run([runner, commands] {
        for (int i = 0; i < commands.size(); ++i) {
            const CommandResult result = runner(commands.at(i));
            if (i == 0 && !result.ok()) {
                return QStringLiteral("systemctl --user %1 failed: %2")
                    .arg(commands.at(i).join(QLatin1Char(' ')),
                         result.error.isEmpty() ? QStringLiteral("exit %1").arg(result.exitCode)
                                                : result.error);
            }
        }
        return QString();
    }));
}

QWidget *timerRow(const State &state, const Timer &timer, qint64 now)
{
    auto *row = new QFrame;
    row->setObjectName(QStringLiteral("timerRow"));
    row->setProperty("unit", timer.unit);
    row->setFrameShape(QFrame::StyledPanel);
    auto *column = new QVBoxLayout(row);
    column->setContentsMargins(10, 8, 10, 8);
    column->setSpacing(3);

    auto *top = new QHBoxLayout;
    auto *name = new QLabel(timer.unit);
    QFont nameFont = name->font();
    nameFont.setBold(true);
    name->setFont(nameFont);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(name);
    top->addStretch(1);

    auto *toggle = new QPushButton(timer.enabled() ? QStringLiteral("Turn off")
                                                   : QStringLiteral("Turn on"));
    toggle->setObjectName(QStringLiteral("timerToggle"));
    toggle->setProperty("unit", timer.unit);
    const bool canToggle = timer.installed() || !timer.sourcePath.isEmpty();
    toggle->setProperty("usable", canToggle);
    toggle->setEnabled(canToggle);
    toggle->setToolTip(timer.enabled()
                           ? QStringLiteral("Stops the timer and keeps it off after a restart "
                                            "(systemctl --user disable --now).")
                           : QStringLiteral("Starts the timer and keeps it on after a restart "
                                            "(systemctl --user enable --now)."));
    auto *runNow = new QPushButton(QStringLiteral("Run now"));
    runNow->setObjectName(QStringLiteral("timerRunNow"));
    runNow->setProperty("unit", timer.unit);
    const bool running = timer.serviceActiveState == QLatin1String("active")
                         || timer.serviceActiveState == QLatin1String("activating");
    const bool canRun = !timer.service.isEmpty() && !running && !timer.command.isEmpty();
    runNow->setProperty("usable", canRun);
    runNow->setEnabled(canRun);
    runNow->setToolTip(QStringLiteral("Starts %1 once, now, without waiting for the timer.")
                           .arg(timer.service));
    top->addWidget(runNow);
    top->addWidget(toggle);
    column->addLayout(top);

    if (!timer.description.isEmpty())
        column->addWidget(mutedLabel(timer.description));
    auto *stateLabel = new QLabel(stateText(timer, now));
    stateLabel->setObjectName(QStringLiteral("timerState"));
    stateLabel->setWordWrap(true);
    column->addWidget(stateLabel);
    if (!timer.command.isEmpty())
        column->addWidget(mutedLabel(QStringLiteral("Runs: %1").arg(timer.command)));
    if (!timer.dropIns.isEmpty())
        column->addWidget(mutedLabel(
            QStringLiteral("Overrides: %1").arg(timer.dropIns.join(QStringLiteral(", ")))));

    const Timer copy = timer;
    QObject::connect(toggle, &QPushButton::clicked, row, [state, copy] {
        if (copy.enabled())
            runCommands(state, disableCommands(copy),
                        QStringLiteral("Turning off %1…").arg(copy.unit),
                        QStringLiteral("%1 is off.").arg(copy.unit));
        else
            runCommands(state, enableCommands(copy),
                        QStringLiteral("Turning on %1…").arg(copy.unit),
                        QStringLiteral("%1 is on.").arg(copy.unit));
    });
    QObject::connect(runNow, &QPushButton::clicked, row, [state, copy] {
        runCommands(state, {runNowArguments(copy)},
                    QStringLiteral("Starting %1…").arg(copy.service),
                    QStringLiteral("%1 started.").arg(copy.service));
    });
    return row;
}

void render(const State &state)
{
    QWidget *host = state->rowsHost;
    for (QWidget *child : host->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        child->deleteLater();
    auto *layout = static_cast<QVBoxLayout *>(host->layout());
    while (QLayoutItem *item = layout->takeAt(0))
        delete item;
    const qint64 now = state->options.nowUsec();
    for (const Timer &timer : state->timers)
        layout->addWidget(timerRow(state, timer, now));
}

void refreshNow(const State &state)
{
    if (state->busy)
        return;
    state->setBusy(true);
    state->sinceRefresh.start();
    const Runner runner = state->options.runner;
    const QString directory = state->options.unitSourceDirectory
                                  ? state->options.unitSourceDirectory()
                                  : QString();
    auto *watcher = new QFutureWatcher<Snapshot>(state->panel);
    QObject::connect(watcher, &QFutureWatcherBase::finished, state->panel, [state, watcher] {
        const Snapshot snapshot = watcher->result();
        watcher->deleteLater();
        const QString notice = std::exchange(state->notice, QString());
        if (!snapshot.error.isEmpty()) {
            state->setBusy(false);
            state->say(snapshot.error, true);
            return;
        }
        state->timers = snapshot.timers;
        render(state);
        state->setBusy(false);
        int on = 0;
        for (const Timer &timer : snapshot.timers)
            on += timer.enabled() ? 1 : 0;
        if (!notice.isEmpty())
            state->say(notice, state->noticeIsProblem);
        else
            state->say(snapshot.timers.isEmpty()
                           ? QStringLiteral("No ForkMesh timers on this computer.")
                           : QStringLiteral("%1 timers, %2 on.")
                                 .arg(snapshot.timers.size())
                                 .arg(on));
    });
    watcher->setFuture(
        QtConcurrent::run([runner, directory] { return collect(runner, directory); }));
}

} // namespace

QWidget *buildSettingsPanel(const PanelOptions &options, QWidget *parent)
{
    auto *panel = new PanelWidget(parent);
    panel->setObjectName(QStringLiteral("timersSettingsPanel"));
    auto state = std::make_shared<PanelState>();
    state->options = options;
    if (!state->options.runner)
        state->options.runner = systemctlRunner();
    if (!state->options.nowUsec)
        state->options.nowUsec = [] { return QDateTime::currentMSecsSinceEpoch() * 1000; };
    state->panel = panel;

    auto *column = new QVBoxLayout(panel);
    column->setContentsMargins(2, 12, 2, 12);
    column->setSpacing(8);
    auto *intro = new QLabel(QStringLiteral(
        "Background jobs this computer runs on a schedule: the systemd user timers named "
        "forkmesh-* and fmnext-*. Turning one off stops it and keeps it off after a "
        "restart; turning it on starts it again."));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("mutedLabel"));
    column->addWidget(intro);

    auto *header = new QHBoxLayout;
    state->refresh = new QPushButton(QStringLiteral("Refresh"));
    state->refresh->setObjectName(QStringLiteral("timersRefresh"));
    state->status = new QLabel;
    state->status->setObjectName(QStringLiteral("timersStatus"));
    state->status->setWordWrap(true);
    state->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(state->refresh);
    header->addWidget(state->status, 1);
    column->addLayout(header);

    state->rowsHost = new QWidget;
    state->rowsHost->setObjectName(QStringLiteral("timersRows"));
    auto *rows = new QVBoxLayout(state->rowsHost);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);
    column->addWidget(state->rowsHost);
    column->addStretch(1);

    QObject::connect(state->refresh, &QPushButton::clicked, panel,
                     [state] { refreshNow(state); });
    panel->onShown = [state] {
        if (!state->sinceRefresh.isValid()
            || state->sinceRefresh.hasExpired(state->options.refreshAfterShowMs))
            refreshNow(state);
    };
    return panel;
}

} // namespace forkmesh::systemdtimers
