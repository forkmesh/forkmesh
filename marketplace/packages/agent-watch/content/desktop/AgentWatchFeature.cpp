#include "AgentWatchFeature.h"

#include "AgentActivityModel.h"
#include "AgentActivityView.h"
#include "AgentAnnouncer.h"
#include "AgentNetworkSampler.h"
#include "AgentTrafficWindow.h"
#include "ClaudeSessionScan.h"
#include "ControlNode.h"
#include "SystemStats.h"
#include "Theme.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

using forkmesh::agentvis::Body;
using forkmesh::agentvis::BlockRule;
using forkmesh::agentvis::BlockTarget;
using forkmesh::agentvis::LogLine;
using forkmesh::agentvis::Motion;
using forkmesh::agentvis::Scene;
using forkmesh::agentvis::State;
using forkmesh::agentvis::Threat;

namespace forkmesh::agentwatch {
namespace {

const QString kHiddenKindsSetting = QStringLiteral("agents/activityHiddenKinds");
const QString kMutedKindsSetting = QStringLiteral("agents/activityMutedKinds");
const QString kSpeakSetting = QStringLiteral("agents/activitySpeakProse");
const QString kWorkingOnlySetting = QStringLiteral("agents/activityWorkingOnly");
const QString kSoundsSetting = QStringLiteral("agents/activitySounds");
const QString kNetworkSetting = QStringLiteral("agents/activityNetwork");
const QString kProcessesSetting = QStringLiteral("agents/activityProcesses");
const QString kLogsSetting = QStringLiteral("agents/activityLogs");
const QString kHideOutputSetting = QStringLiteral("agents/activityLogsHideOutput");
const QString kLinksSetting = QStringLiteral("agents/activityLinks");
const QString kRingsSetting = QStringLiteral("agents/activityRings");
const QString kBackdropSetting = QStringLiteral("agents/activityBackdrop");
constexpr char kVisitorBlocksSetting[] = "agents/visitorBlocks";
constexpr qint64 kMirrorRereadMs = 30000;

const forkmesh::agentfeed::Host &host()
{
    return forkmesh::agentfeed::host();
}

bool hosted()
{
    return forkmesh::agentfeed::hasHost();
}

State stateFor(forkmesh::agentfeed::RunState state)
{
    using forkmesh::agentfeed::RunState;
    switch (state) {
    case RunState::Running:
        return State::Running;
    case RunState::Waiting:
        return State::Waiting;
    case RunState::Done:
        return State::Done;
    case RunState::Failed:
        return State::Failed;
    case RunState::Idle:
        break;
    }
    return State::Idle;
}

QString kindKey(Motion motion)
{
    return QString::fromLatin1(forkmesh::agentvis::styleFor(motion).label).toLower();
}

bool kindListed(const QString &setting, Motion motion)
{
    return QSettings().value(setting).toStringList().contains(kindKey(motion));
}

void setKindListed(const QString &setting, Motion motion, bool listed)
{
    QSettings settings;
    QStringList kinds = settings.value(setting).toStringList();
    kinds.removeAll(kindKey(motion));
    if (listed)
        kinds.append(kindKey(motion));
    settings.setValue(setting, kinds);
}

void markToggle(QPushButton *button)
{
    button->setProperty("activityToggle", true);
    const QString tip = button->toolTip();
    const auto paint = [button, tip](bool on) {
        const qreal ratio = button->devicePixelRatioF();
        const int side = 12;
        QPixmap lamp(QSize(side, side) * ratio);
        lamp.setDevicePixelRatio(ratio);
        lamp.fill(Qt::transparent);
        QPainter painter(&lamp);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF disc(1.5, 1.5, side - 3, side - 3);
        if (on) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::remapColor(QColor(QString::fromLatin1(Theme::kPrimary))));
        } else {
            painter.setPen(QPen(button->palette().color(QPalette::PlaceholderText), 1.4));
            painter.setBrush(Qt::NoBrush);
        }
        painter.drawEllipse(disc);
        painter.end();
        button->setIcon(QIcon(lamp));
        button->setToolTip(tip + (on ? QStringLiteral("\n\nOn - click to turn off.")
                                     : QStringLiteral("\n\nOff - click to turn on.")));
        button->setAccessibleDescription(on ? QStringLiteral("On") : QStringLiteral("Off"));
    };
    paint(button->isChecked());
    QObject::connect(button, &QPushButton::toggled, button, paint);
}

void showKindsOff(QToolButton *kinds, const AgentActivityView *view)
{
    int shownOff = 0;
    int heardOff = 0;
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion == Motion::Rest)
            continue;
        shownOff += view->motionShown(motion) ? 0 : 1;
        heardOff += view->motionHeard(motion) ? 0 : 1;
    }
    const int layersOff = (view->showLinks() ? 0 : 1) + (view->showRings() ? 0 : 1);
    const int off = shownOff + heardOff + layersOff;
    const auto plural = [](int n) { return n == 1 ? QString() : QStringLiteral("s"); };
    kinds->setText(off == 0 ? QStringLiteral("Visuals & sounds")
                            : QStringLiteral("Visuals & sounds · %1 off").arg(off));
    kinds->setAccessibleDescription(
        off == 0 ? QStringLiteral("Everything on")
                 : QStringLiteral("%1 figure%2 hidden, %3 sound%4 silenced, %5 layer%6 off")
                       .arg(shownOff).arg(plural(shownOff))
                       .arg(heardOff).arg(plural(heardOff))
                       .arg(layersOff).arg(plural(layersOff)));
}

QString speakerName(const Body &body)
{
    const QString task = forkmesh::announce::spokenTitle(body.task, 5);
    return !task.isEmpty() ? task : forkmesh::announce::spokenTitle(body.title, 3);
}

QString blockMode(const BlockRule &rule)
{
    return rule.target == BlockTarget::Country ? QStringLiteral("managed_challenge")
                                               : QStringLiteral("block");
}

QString blockVerb(const BlockRule &rule)
{
    return rule.target == BlockTarget::Country ? QStringLiteral("Challenge")
                                               : QStringLiteral("Block");
}

BlockRule ruleFromStored(const QJsonObject &stored)
{
    BlockRule rule;
    const QString target = stored.value(QStringLiteral("target")).toString();
    for (const BlockTarget t : {BlockTarget::Address, BlockTarget::Range, BlockTarget::Network,
                                BlockTarget::Country, BlockTarget::UserAgent}) {
        if (forkmesh::agentvis::blockTargetKey(t) == target)
            rule.target = t;
    }
    rule.value = stored.value(QStringLiteral("value")).toString();
    rule.label = stored.value(QStringLiteral("label")).toString();
    rule.reason = stored.value(QStringLiteral("reason")).toString();
    return rule;
}

QString socketAddressText(const QHostAddress &address)
{
    QString text = address.toString();
    if (text.startsWith(QLatin1String("::ffff:")))
        text = text.mid(7);
    return text;
}

template <typename Result, typename Work, typename Done>
void offThread(QObject *context, Work work, Done done)
{
    auto *watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context,
                     [watcher, done]() mutable {
                         Result result = watcher->result();
                         watcher->deleteLater();
                         done(std::move(result));
                     });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

QPushButton *toggleButton(QWidget *parent, const QString &text, const QString &name,
                          const QString &tip)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    return button;
}

} // namespace

AgentWatchFeature::AgentWatchFeature(const forkmesh::market::NativePackageHost &host,
                                     QWidget *parent)
    : NativePackageWidget(parent), m_scene(std::make_unique<Scene>()),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("agentWatchPage"));
    for (const QJsonValue &value : visitorBlocksStored())
        m_scene->noteBlocked(ruleFromStored(value.toObject()));
    buildPage();
    applyMutedKinds();
    forkmesh::agentfeed::addListener(this);
}

AgentWatchFeature::~AgentWatchFeature()
{
    forkmesh::agentfeed::removeListener(this);
    if (m_hearAll && hosted())
        host().listenToEverything(false);
    if (m_speakButton && m_speakButton->isChecked() && hosted())
        host().setReplySpeech(false, nullptr);
    if (hosted()) {
        for (const Motion motion : forkmesh::sonify::kMotions) {
            if (motion != Motion::Rest)
                host().setMuted(motion, false);
        }
    }
    const QList<QPointer<AgentTrafficWindow>> windows = std::exchange(m_trafficWindows, {});
    for (const QPointer<AgentTrafficWindow> &window : windows) {
        if (window)
            window->disconnect(this);
        delete window.data();
    }
    delete m_view;
    m_view = nullptr;
    delete m_fullWindow.data();
}

void AgentWatchFeature::refresh()
{
    if (!hosted())
        return;
    syncRoster();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

bool AgentWatchFeature::watching() const
{
    return m_watching;
}

void AgentWatchFeature::applyMutedKinds()
{
    if (!hosted())
        return;
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion != Motion::Rest)
            host().setMuted(motion, kindListed(kMutedKindsSetting, motion));
    }
}

void AgentWatchFeature::buildPage()
{
    m_view = new AgentActivityView(m_scene.get(), this);
    m_view->setObjectName(QStringLiteral("agentWatchView"));
    m_view->setShowFrameRate(true);
    m_view->setOnSelect([](int sessionId) {
        if (hosted())
            host().showAgent(sessionId);
    });
    m_view->setOnToggleListen([this](int sessionId) {
        if (!hosted())
            return;
        host().toggleListening(sessionId);
        syncRoster();
    });

    m_hearAllButton = toggleButton(
        this, QStringLiteral("Hear everything"), QStringLiteral("agentActivityHearAllButton"),
        QStringLiteral("Play every agent in the field, and every one that "
                       "joins it, so what you can see you can also hear. Each "
                       "agent keeps its own instrument, register and seat; the "
                       "log, background work and this desktop's own network "
                       "traffic play on a low chime of their own."));
    connect(m_hearAllButton, &QPushButton::toggled, this, [this](bool on) {
        setHearAll(on);
        const QSignalBlocker block(m_hearAllButton);
        m_hearAllButton->setChecked(m_hearAll);
    });

    m_speakButton = toggleButton(
        this, QStringLiteral("Speak"), QStringLiteral("agentActivitySpeakButton"),
        QStringLiteral("Read each agent's replies aloud as they come in - what shows "
                       "under its body as “said” - in a neural voice that runs "
                       "on this machine. Code and tables are skipped."));
    connect(m_speakButton, &QPushButton::toggled, this, [this](bool on) {
        if (!hosted())
            return;
        if (on == host().replySpeech())
            return;
        if (!on) {
            host().setReplySpeech(false, m_speakButton);
            QSettings().setValue(kSpeakSetting, false);
            return;
        }
        if (!host().setReplySpeech(true, m_speakButton)) {
            QTimer::singleShot(0, m_speakButton, [this] { m_speakButton->setChecked(false); });
            return;
        }
        QSettings().setValue(kSpeakSetting, true);
        const Body *latest = nullptr;
        for (const Body &body : m_scene->bodies()) {
            if (!body.said.isEmpty() && (!latest || body.saidSeconds > latest->saidSeconds))
                latest = &body;
        }
        if (latest)
            host().speakReply(latest->sessionId, speakerName(*latest), latest->said);
    });

    auto *workingOnly = toggleButton(
        this, QStringLiteral("Working only"), QStringLiteral("agentActivityWorkingOnlyButton"),
        QStringLiteral("Show only the agents that are working - running, or "
                       "active in the last half minute. The rest fade out, and "
                       "fade back in the moment they start again."));
    m_view->setWorkingOnly(QSettings().value(kWorkingOnlySetting, false).toBool());
    workingOnly->setChecked(m_view->workingOnly());
    connect(workingOnly, &QPushButton::toggled, m_view, [this](bool on) {
        m_view->setWorkingOnly(on);
        QSettings().setValue(kWorkingOnlySetting, on);
    });

    auto *sounds = toggleButton(
        this, QStringLiteral("Sounds"), QStringLiteral("agentActivitySoundsButton"),
        QStringLiteral("Every kind of sound, what it stands for, and a glow on its "
                       "row the moment one plays"));
    m_view->setShowSounds(QSettings().value(kSoundsSetting, true).toBool());
    sounds->setChecked(m_view->showSounds());
    connect(sounds, &QPushButton::toggled, m_view, [this](bool on) {
        m_view->setShowSounds(on);
        QSettings().setValue(kSoundsSetting, on);
    });

    m_fullScreenButton = toggleButton(
        this, QStringLiteral("Full screen"), QStringLiteral("agentActivityFullScreenButton"),
        QStringLiteral("Fill the screen with the field (F11; Esc to come back)"));
    connect(m_fullScreenButton, &QPushButton::toggled, this,
            [this](bool on) { setFullScreen(on); });
    auto *fullScreenKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullScreenKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(fullScreenKey, &QShortcut::activated, this, [this] { setFullScreen(true); });

    auto *network = toggleButton(
        this, QStringLiteral("Network"), QStringLiteral("agentActivityNetworkButton"),
        QStringLiteral("Every agent's network traffic, live: the hosts it talks "
                       "to along the top, the desktop's own at the bottom, and the "
                       "bytes travelling between them - warm going out, cool "
                       "coming in. Beside the desktop, everybody else on the relay: "
                       "web visitors, other desktops and phones, with every "
                       "websocket frame to and from them"));
    m_view->setShowNetwork(QSettings().value(kNetworkSetting, true).toBool());
    network->setChecked(m_view->showNetwork());
    connect(network, &QPushButton::toggled, m_view, [this](bool on) {
        m_view->setShowNetwork(on);
        QSettings().setValue(kNetworkSetting, on);
    });

    auto *processes = toggleButton(
        this, QStringLiteral("Processes"), QStringLiteral("agentActivityProcessesButton"),
        QStringLiteral("Every process an agent started - its shells, builds, test "
                       "runs and servers - as a tree off its body, lit while it "
                       "burns CPU"));
    m_view->setShowProcesses(QSettings().value(kProcessesSetting, true).toBool());
    processes->setChecked(m_view->showProcesses());
    connect(processes, &QPushButton::toggled, m_view, [this](bool on) {
        m_view->setShowProcesses(on);
        QSettings().setValue(kProcessesSetting, on);
    });

    auto *logs = toggleButton(
        this, QStringLiteral("Logs"), QStringLiteral("agentActivityLogsButton"),
        QStringLiteral("The desktop's log alongside the agents: a column of the "
                       "latest lines in their categories' colours, and a spark "
                       "off the agent a line mentions (or off this desktop). "
                       "Click the column for the full Logs page."));
    m_view->setShowLogs(QSettings().value(kLogsSetting, false).toBool());
    logs->setChecked(m_view->showLogs());
    auto *hideOutput = toggleButton(
        this, QStringLiteral("Hide output"), QStringLiteral("agentActivityHideOutputButton"),
        QStringLiteral("Keep the core nodes' and mirrors' forwarded output - their "
                       "journals and service logs - out of the Log column, so it "
                       "shows only what the desktop itself writes. The lines still "
                       "reach the Logs page."));
    m_view->setHideOutput(QSettings().value(kHideOutputSetting, false).toBool());
    hideOutput->setChecked(m_view->hideOutput());
    hideOutput->setEnabled(m_view->showLogs());
    connect(hideOutput, &QPushButton::toggled, m_view, [this](bool on) {
        m_view->setHideOutput(on);
        QSettings().setValue(kHideOutputSetting, on);
    });
    connect(logs, &QPushButton::toggled, m_view, [this, hideOutput](bool on) {
        m_view->setShowLogs(on);
        hideOutput->setEnabled(on);
        QSettings().setValue(kLogsSetting, on);
    });
    m_view->setOnOpenLogs([this] {
        setFullScreen(false);
        if (hosted())
            host().showLogs();
    });

    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion == Motion::Rest)
            continue;
        m_view->setMotionShown(motion, !kindListed(kHiddenKindsSetting, motion));
        m_view->setMotionHeard(motion, !kindListed(kMutedKindsSetting, motion));
    }
    m_kindsButton = new QToolButton(this);
    QToolButton *kinds = m_kindsButton;
    AgentActivityView *view = m_view;
    const auto setKind = [view, kinds](Motion motion, bool sound, bool on) {
        if (sound) {
            view->setMotionHeard(motion, on);
            setKindListed(kMutedKindsSetting, motion, !on);
            if (hosted())
                host().setMuted(motion, !on);
        } else {
            view->setMotionShown(motion, on);
            setKindListed(kHiddenKindsSetting, motion, !on);
        }
        showKindsOff(kinds, view);
    };
    m_view->setOnToggleKind(setKind);
    m_view->setShowLinks(QSettings().value(kLinksSetting, true).toBool());
    m_view->setShowRings(QSettings().value(kRingsSetting, true).toBool());

    kinds->setObjectName(QStringLiteral("agentActivityKindsButton"));
    kinds->setText(QStringLiteral("Visuals & sounds"));
    kinds->setCursor(Qt::PointingHandCursor);
    kinds->setPopupMode(QToolButton::InstantPopup);
    kinds->setToolTip(
        QStringLiteral("Switch each kind of figure and each kind of sound on or "
                       "off, and the links and history rings round the agents"));
    showKindsOff(kinds, view);
    auto *kindsMenu = new QMenu(kinds);
    kindsMenu->setObjectName(QStringLiteral("agentActivityKindsMenu"));
    kinds->setMenu(kindsMenu);
    connect(kindsMenu, &QMenu::aboutToShow, kindsMenu, [kindsMenu, kinds, view, setKind] {
        kindsMenu->clear();
        QAction *links = kindsMenu->addAction(QStringLiteral("Links between agents"));
        links->setCheckable(true);
        links->setChecked(view->showLinks());
        QObject::connect(links, &QAction::toggled, view, [view, kinds](bool on) {
            view->setShowLinks(on);
            QSettings().setValue(kLinksSetting, on);
            showKindsOff(kinds, view);
        });
        QAction *rings = kindsMenu->addAction(QStringLiteral("History rings"));
        rings->setCheckable(true);
        rings->setChecked(view->showRings());
        QObject::connect(rings, &QAction::toggled, view, [view, kinds](bool on) {
            view->setShowRings(on);
            QSettings().setValue(kRingsSetting, on);
            showKindsOff(kinds, view);
        });
        for (const bool sound : {false, true}) {
            QMenu *menu = kindsMenu->addMenu(sound ? QStringLiteral("Sounds")
                                                   : QStringLiteral("Figures"));
            QAction *all = menu->addAction(sound ? QStringLiteral("Hear all")
                                                 : QStringLiteral("Show all"));
            QAction *none = menu->addAction(sound ? QStringLiteral("Silence all")
                                                  : QStringLiteral("Hide all"));
            for (QAction *bulk : {all, none}) {
                const bool on = bulk == all;
                QObject::connect(bulk, &QAction::triggered, view, [setKind, sound, on] {
                    for (const Motion motion : forkmesh::sonify::kMotions) {
                        if (motion != Motion::Rest)
                            setKind(motion, sound, on);
                    }
                });
            }
            menu->addSeparator();
            for (const Motion motion : forkmesh::sonify::kMotions) {
                if (motion == Motion::Rest)
                    continue;
                QAction *action = menu->addAction(
                    QString::fromLatin1(forkmesh::agentvis::styleFor(motion).label));
                action->setCheckable(true);
                action->setChecked(sound ? view->motionHeard(motion) : view->motionShown(motion));
                action->setToolTip(
                    QString::fromLatin1(sound ? forkmesh::agentvis::styleFor(motion).sound
                                              : forkmesh::agentvis::styleFor(motion).description));
                QObject::connect(action, &QAction::toggled, view,
                                 [setKind, motion, sound](bool on) { setKind(motion, sound, on); });
            }
            menu->setToolTipsVisible(true);
        }
    });

    auto *backdrop = new QComboBox(this);
    backdrop->setObjectName(QStringLiteral("agentActivityBackdropCombo"));
    backdrop->setToolTip(QStringLiteral("What the field is drawn on"));
    backdrop->addItems(AgentActivityView::backdropNames());
    const int savedBackdrop = qBound(0, QSettings().value(kBackdropSetting, 1).toInt(),
                                     backdrop->count() - 1);
    backdrop->setCurrentIndex(savedBackdrop);
    m_view->setBackdrop(AgentActivityView::Backdrop(savedBackdrop));
    connect(backdrop, &QComboBox::currentIndexChanged, m_view, [this](int index) {
        if (index < 0)
            return;
        m_view->setBackdrop(AgentActivityView::Backdrop(index));
        QSettings().setValue(kBackdropSetting, index);
    });

    m_visitorsButton = new QPushButton(QStringLiteral("Visitors"), this);
    m_visitorsButton->setObjectName(QStringLiteral("agentActivityVisitorsButton"));
    m_visitorsButton->setCursor(Qt::PointingHandCursor);
    m_visitorsButton->setToolTip(
        QStringLiteral("The site's web visitors as threats: what looks like an attack "
                       "(an attack tool, a flood, probes for paths like /.env, a crowd "
                       "out of one range or network), the narrowest rule that stops each "
                       "one, and the blocks made at Cloudflare's edge. Click a visitor on "
                       "the field to block it alone."));
    connect(m_visitorsButton, &QPushButton::clicked, this, [this] { openVisitorBlocks(); });
    m_view->setOnClientClicked([this](const QString &id, const QPoint &at) {
        offerVisitorBlock(id, at);
    });
    m_view->setOnConnectionClicked([this](int sessionId, const QString &host) {
        openTraffic(sessionId, host);
    });

    for (QPushButton *toggle : {m_hearAllButton, m_speakButton, sounds, workingOnly, network,
                                processes, logs, hideOutput, m_fullScreenButton})
        markToggle(toggle);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(10, 8, 10, 8);
    top->setSpacing(8);
    top->addWidget(m_hearAllButton);
    top->addWidget(m_speakButton);
    top->addWidget(sounds);
    top->addWidget(workingOnly);
    top->addWidget(network);
    top->addWidget(processes);
    top->addWidget(logs);
    top->addWidget(hideOutput);
    top->addWidget(kinds);
    top->addWidget(backdrop);
    top->addWidget(m_fullScreenButton);
    top->addWidget(m_visitorsButton);
    top->addStretch(1);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addLayout(top);
    m_layout->addWidget(m_view, 1);

    m_rosterTimer = new QTimer(this);
    m_rosterTimer->setInterval(1000);
    connect(m_rosterTimer, &QTimer::timeout, this, [this] {
        syncRoster();
        syncRelaySocket();
        refreshVisitorBlockButton();
    });
    m_networkTimer = new QTimer(this);
    m_networkTimer->setInterval(400);
    connect(m_networkTimer, &QTimer::timeout, this, [this] { sampleNetwork(); });
}

void AgentWatchFeature::showEvent(QShowEvent *event)
{
    NativePackageWidget::showEvent(event);
    m_pageShown = true;
    updateWatching();
}

void AgentWatchFeature::hideEvent(QHideEvent *event)
{
    NativePackageWidget::hideEvent(event);
    m_pageShown = false;
    updateWatching();
}

bool AgentWatchFeature::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_fullWindow && event->type() == QEvent::Close) {
        QTimer::singleShot(0, this, [this] { setFullScreen(false); });
        return false;
    }
    return NativePackageWidget::eventFilter(watched, event);
}

void AgentWatchFeature::updateWatching()
{
    const bool now = m_pageShown || m_fullWindow || trafficWindows() > 0;
    if (now == m_watching)
        return;
    m_watching = now;
    if (now)
        beginWatching();
    else
        endWatching();
}

void AgentWatchFeature::beginWatching()
{
    forkmesh::agentfeed::setWatching(this, true);
    if (!hosted())
        return;
    if (!m_speechRestored) {
        m_speechRestored = true;
        if (QSettings().value(kSpeakSetting, false).toBool()
            && forkmesh::announce::locateVoice().ok()) {
            const QSignalBlocker block(m_speakButton);
            m_speakButton->setChecked(host().setReplySpeech(true, nullptr));
        }
    }
    refreshMirrors();
    syncRoster();
    m_scene->clearLogs();
    seedLogs();
    seedTasks();
    noteClients(false);
    syncRelaySocket();
    refreshVisitorBlockButton();
    if (m_networkNames.isEmpty())
        seedNetworkNames();
    m_rosterTimer->start();
    m_networkTimer->start();
}

void AgentWatchFeature::endWatching()
{
    forkmesh::agentfeed::setWatching(this, false);
    m_rosterTimer->stop();
    m_networkTimer->stop();
    if (m_ledger)
        m_ledger->reset();
}

int AgentWatchFeature::trafficWindows() const
{
    int open = 0;
    for (const QPointer<AgentTrafficWindow> &window : m_trafficWindows)
        open += window && window->isVisible() ? 1 : 0;
    return open;
}

void AgentWatchFeature::openTraffic(int sessionId, const QString &host)
{
    const QString key = AgentTrafficWindow::keyFor(sessionId, m_scene->hostNamed(host));
    m_trafficWindows.removeAll(QPointer<AgentTrafficWindow>());
    for (const QPointer<AgentTrafficWindow> &window : std::as_const(m_trafficWindows)) {
        if (window->key() != key)
            continue;
        window->show();
        window->raise();
        window->activateWindow();
        return;
    }
    auto *window = new AgentTrafficWindow(m_scene.get(), sessionId, host);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QObject::destroyed, this, [this] {
        m_trafficWindows.removeAll(QPointer<AgentTrafficWindow>());
        QTimer::singleShot(0, this, [this] { updateWatching(); });
    });
    m_trafficWindows.append(window);
    window->show();
    window->raise();
    window->activateWindow();
    updateWatching();
}

void AgentWatchFeature::setFullScreen(bool on)
{
    if (on == !m_fullWindow.isNull()) {
        if (on)
            m_fullWindow->raise();
        return;
    }
    if (on) {
        auto *window = new QWidget(nullptr, Qt::Window);
        window->setAttribute(Qt::WA_QuitOnClose, false);
        window->setObjectName(QStringLiteral("agentActivityWindow"));
        window->setWindowTitle(QStringLiteral("Agent activity"));
        auto *layout = new QVBoxLayout(window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_view);
        m_view->setGpuRendering(true);
        for (const QKeySequence &key : {QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_F11)}) {
            auto *leave = new QShortcut(key, window);
            connect(leave, &QShortcut::activated, this, [this] { setFullScreen(false); });
        }
        window->installEventFilter(this);
        m_fullWindow = window;
        window->showFullScreen();
        window->activateWindow();
    } else {
        QWidget *window = m_fullWindow.data();
        m_fullWindow.clear();
        m_view->setGpuRendering(false);
        m_layout->addWidget(m_view, 1);
        m_view->show();
        if (window) {
            window->removeEventFilter(this);
            window->hide();
            window->deleteLater();
        }
    }
    const QSignalBlocker block(m_fullScreenButton);
    m_fullScreenButton->setChecked(on);
    updateWatching();
}

void AgentWatchFeature::agentEvent(int sessionId, const QJsonObject &event)
{
    if (!hosted() || (sessionId < 0 && !host().isExternal(sessionId)))
        return;
    m_scene->tick();
    forkmesh::agentfeed::Agent agent;
    const bool known = host().agent(sessionId, &agent);
    const bool heard = host().listening(sessionId)
                       && !host().muted(forkmesh::sonify::motionForEvent(event));
    const bool speaking = host().replySpeech();
    const Body *before = speaking ? m_scene->body(sessionId) : nullptr;
    const QString said = before ? before->said : QString();
    m_scene->ingest(sessionId, known ? agent.name : QString(), event, heard);
    if (known)
        m_scene->setState(sessionId, stateFor(agent.state));
    if (speaking) {
        if (const Body *body = m_scene->body(sessionId); body && body->said != said)
            host().speakReply(sessionId, speakerName(*body), body->said);
    }
}

void AgentWatchFeature::noteScheduled(int sessionId, Motion motion, double delaySeconds,
                                      double gain, quint32 seed)
{
    m_scene->tick();
    if (sessionId != Scene::kDesktop) {
        forkmesh::agentfeed::Agent agent;
        const bool known = hosted() && host().agent(sessionId, &agent);
        m_scene->addAgent(sessionId, known ? agent.name : QString());
    }
    m_scene->note(sessionId, motion, delaySeconds, gain, /*heard=*/true, QString(), seed);
}

void AgentWatchFeature::logLine(const QString &storedLine)
{
    if (m_watching)
        noteLog(storedLine, 0);
}

void AgentWatchFeature::backgroundTask(const forkmesh::agentfeed::TaskEvent &task)
{
    if (!m_watching)
        return;
    m_scene->tick();
    if (!task.started) {
        int owner = Scene::kDesktop;
        QString word = task.word;
        for (const forkmesh::agentvis::Task &known : m_scene->tasks()) {
            if (known.id == task.id) {
                owner = known.sessionId;
                word = known.word;
                break;
            }
        }
        m_scene->taskFinished(task.id, task.failed);
        noteCue(owner, Motion::Task,
                QStringLiteral("%1 %2").arg(word, task.failed ? QStringLiteral("failed")
                                                              : QStringLiteral("done")),
                Scene::logSeed(QStringLiteral("task-end"), word));
        return;
    }
    int sessionId = Scene::kDesktop;
    if (task.sessionId > 0) {
        refreshState(task.sessionId);
        if (m_scene->working(task.sessionId))
            sessionId = task.sessionId;
    }
    m_scene->taskStarted(task.id, sessionId, task.word, task.detail);
    noteCue(sessionId, Motion::Task, QStringLiteral("%1 started").arg(task.word),
            Scene::logSeed(QStringLiteral("task"), task.word));
}

void AgentWatchFeature::relayRosterChanged()
{
    if (m_watching)
        noteClients(true);
}

void AgentWatchFeature::relayFrame(bool outgoing, const QString &type, const QString &peerId,
                                   qint64 bytes)
{
    if (!m_watching)
        return;
    m_scene->tick();
    m_scene->socketFrame(outgoing, type, peerId, bytes);
    constexpr qint64 kSocketCueSpacingMs = 250;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_socketCueMs < kSocketCueSpacingMs)
        return;
    m_socketCueMs = nowMs;
    QString who;
    if (const forkmesh::agentvis::Client *client = m_scene->client(peerId))
        who = client->info.name.isEmpty() ? client->info.nodeName : client->info.name;
    if (who.isEmpty())
        who = peerId.isEmpty() ? (outgoing ? QStringLiteral("the room")
                                           : QStringLiteral("the relay"))
                               : peerId.left(8);
    QString text = QStringLiteral("%1 %2 %3 %4")
                       .arg(outgoing ? QStringLiteral("↑") : QStringLiteral("↓"), type,
                            outgoing ? QStringLiteral("to") : QStringLiteral("from"), who);
    if (bytes > 0)
        text += QStringLiteral(" (%1 B)").arg(bytes);
    const quint32 seed = (Scene::logSeed(QStringLiteral("socket"), type) & ~1u)
                         | 2u | (outgoing ? 0u : 1u);
    noteCue(Scene::kDesktop, Motion::Socket, text, seed);
}

void AgentWatchFeature::webEvent(const forkmesh::control::CloudflareTailEvent &event)
{
    if (!m_watching)
        return;
    m_scene->tick();
    const QString object =
        event.entrypoint == QLatin1String("default") ? QString() : event.entrypoint;
    if (event.kind == QLatin1String("websocket")) {
        relayFrame(false,
                   object.isEmpty() ? QStringLiteral("relay socket")
                                    : QStringLiteral("relay socket · ") + object,
                   QString(), 0);
        return;
    }
    if (event.kind != QLatin1String("fetch") || !object.isEmpty())
        return;
    QString browser = forkmesh::agentvis::browserForUserAgent(event.userAgent);
    if (browser.isEmpty()) {
        browser = forkmesh::agentvis::toolForUserAgent(event.userAgent);
        if (browser.isEmpty() || event.clientAddress.isEmpty())
            return;
    }
    if (const QString siteHost = QUrl(event.url).host(); !siteHost.isEmpty())
        m_siteHost = siteHost;
    QStringList place;
    for (const QString &part : {event.city, event.country}) {
        if (!part.trimmed().isEmpty())
            place << part.trimmed();
    }
    const QString where = place.join(QStringLiteral(", "));
    const QString name = where.isEmpty() ? browser : QStringLiteral("%1 in %2").arg(browser, where);
    const QString key = !event.clientAddress.isEmpty()
                            ? event.clientAddress
                            : QStringList{event.country, event.city, event.asn, event.userAgent}
                                  .join(QLatin1Char('|'));
    QStringList detail;
    if (!event.clientAddress.isEmpty())
        detail << event.clientAddress;
    if (!event.region.isEmpty() && !where.contains(event.region))
        detail << event.region;
    if (!event.asOrganization.isEmpty())
        detail << (event.asn.isEmpty() ? event.asOrganization
                                       : QStringLiteral("AS%1 %2").arg(event.asn,
                                                                        event.asOrganization));
    if (!event.httpProtocol.isEmpty())
        detail << event.httpProtocol;
    if (!event.colo.isEmpty())
        detail << QStringLiteral("via ") + event.colo;
    const QString path = QUrl(event.url).path();
    QString request = QStringLiteral("%1 %2").arg(event.method.isEmpty() ? QStringLiteral("GET")
                                                                         : event.method,
                                                  path.isEmpty() ? QStringLiteral("/") : path);
    if (event.status > 0)
        request += QLatin1Char(' ') + QString::number(event.status);
    Scene::WebHit hit;
    hit.visitor = key;
    hit.name = name;
    hit.detail = detail.join(QStringLiteral(" · "));
    hit.request = request;
    hit.path = path;
    hit.status = event.status;
    hit.country = event.country;
    hit.address = event.clientAddress;
    hit.asn = event.asn;
    hit.asOrganization = event.asOrganization;
    hit.userAgent = event.userAgent;
    const Threat before = m_scene->siteThreat();
    const bool joined = m_scene->webRequest(hit);
    if (const Threat now = m_scene->siteThreat();
        now == Threat::Danger && before != now && hosted())
        host().log(QStringLiteral("Cloud: web visitors look like an attack: %1. Open Watch "
                                  "> Visitors to block them.")
                       .arg(m_scene->siteThreatReasons().join(QStringLiteral(", "))));
    if (joined)
        noteCue(Scene::kDesktop, Motion::Visitor,
                QStringLiteral("web visitor %1 arrived on %2")
                    .arg(name, path.isEmpty() ? QStringLiteral("/") : path),
                Scene::logSeed(QStringLiteral("client-in"), name));
}

void AgentWatchFeature::replySpeechChanged(bool on)
{
    if (m_speakButton && m_speakButton->isChecked() != on)
        m_speakButton->setChecked(on);
}

void AgentWatchFeature::syncRoster()
{
    if (!hosted())
        return;
    m_scene->tick();
    QSet<int> live;
    const QList<forkmesh::agentfeed::Agent> agents = host().agents();
    for (const forkmesh::agentfeed::Agent &agent : agents) {
        live.insert(agent.id);
        m_scene->addAgent(agent.id, agent.name);
        m_scene->setTitle(agent.id, agent.name);
        m_scene->setTask(agent.id, agent.task);
        if (const Body *body = m_scene->body(agent.id); body && body->said.isEmpty())
            m_scene->setSaid(agent.id, agent.lastReply);
        m_scene->setState(agent.id, stateFor(agent.state));
        m_scene->setUsage(agent.id, agent.turns, agent.tokens);
        m_scene->setPrompt(agent.id, agent.prompt);
        m_view->setFace(agent.id, agent.faceKey, agent.face);
        m_scene->setListening(agent.id, agent.listening);
        if (agent.listening)
            m_scene->setVoice(agent.id, agent.voice);
        if (m_hearAll && !agent.listening)
            host().listen(agent.id);
    }
    const QList<Body> bodies = m_scene->bodies();
    for (const Body &body : bodies) {
        if (!live.contains(body.sessionId)) {
            m_scene->removeAgent(body.sessionId);
            m_view->setFace(body.sessionId, QString(), QIcon());
        }
    }
    m_view->update();
}

void AgentWatchFeature::syncRelaySocket()
{
    if (!hosted())
        return;
    forkmesh::agentvis::RelaySocket socket;
    const QJsonObject row = host().relayDiagnostics();
    if (!row.isEmpty()) {
        const auto number = [&row](const char *key) {
            return qint64(row.value(QLatin1String(key)).toDouble());
        };
        socket.connected = row.value(QStringLiteral("connected")).toBool();
        socket.endpoint = row.value(QStringLiteral("endpoint")).toString();
        socket.room = row.value(QStringLiteral("durableObject")).toString();
        const qint64 since = number("connectedAtMs");
        if (socket.connected && since > 0)
            socket.connectedSeconds =
                qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - since) / 1000.0;
        socket.framesIn = number("rxFrames");
        socket.framesOut = number("txFrames");
        socket.bytesIn = number("rxBytes");
        socket.bytesOut = number("txBytes");
        socket.controlIn = number("rxControlFrames");
        socket.controlOut = number("txControlFrames");
        socket.buffered = number("bufferedBytes");
        socket.lastInType = row.value(QStringLiteral("lastRxType")).toString();
        socket.lastOutType = row.value(QStringLiteral("lastTxType")).toString();
    }
    m_scene->setRelaySocket(socket);
}

void AgentWatchFeature::refreshState(int sessionId)
{
    if (sessionId == Scene::kDesktop || !hosted())
        return;
    forkmesh::agentfeed::Agent agent;
    if (host().agent(sessionId, &agent))
        m_scene->setState(sessionId, stateFor(agent.state));
}

void AgentWatchFeature::noteLog(const QString &storedLine, double ageSeconds)
{
    if (!hosted())
        return;
    const forkmesh::agentfeed::LogStyle style = host().logStyle(storedLine);
    m_scene->tick();
    refreshState(m_scene->agentForLog(style.message));
    bool heard = false;
    if (ageSeconds <= 0) {
        heard = host().playCue(m_scene->ownerForLog(style.message),
                               forkmesh::sonify::motionForLogBadge(style.badge),
                               Scene::logSeed(style.badge, style.message));
    }
    m_scene->log(style.badge, style.accent, style.message, ageSeconds, heard);
}

void AgentWatchFeature::seedLogs()
{
    const QStringList lines = host().recentLogLines();
    const QDateTime now = QDateTime::currentDateTime();
    const qsizetype from = qMax<qsizetype>(0, lines.size() - Scene::kLogSize);
    for (qsizetype i = from; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const QDateTime stamp = QDateTime::fromString(
            line.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const double age = stamp.isValid()
                               ? qMax(double(stamp.secsTo(now)), LogLine::kSparkSeconds)
                               : 600.0;
        noteLog(line, age);
    }
}

void AgentWatchFeature::seedTasks()
{
    m_scene->tick();
    m_scene->clearTasks();
    const QHash<quint64, QString> tasks = host().runningTasks();
    for (auto it = tasks.cbegin(); it != tasks.cend(); ++it)
        m_scene->taskStarted(it.key(), Scene::kDesktop, it.value());
}

void AgentWatchFeature::noteClients(bool announce)
{
    if (!hosted())
        return;
    m_scene->tick();
    QList<forkmesh::agentvis::ClientInfo> clients;
    for (const forkmesh::agentfeed::RelayClient &client : host().relayClients())
        clients.append({client.id, client.name, client.platform, client.accountKind,
                        client.nodeName, client.version});
    const QList<forkmesh::agentvis::ClientChange> changes = m_scene->setClients(clients, announce);
    for (const forkmesh::agentvis::ClientChange &change : changes) {
        const bool visitor = change.kind == forkmesh::agentvis::ClientKind::Visitor;
        QString name = change.info.name.trimmed();
        if (name.isEmpty())
            name = change.info.nodeName.trimmed();
        if (name.isEmpty())
            name = change.info.id.left(8);
        QString text = QStringLiteral("%1 %2 %3")
                           .arg(forkmesh::agentvis::clientKindLabel(change.kind), name,
                                change.joined ? (visitor ? QStringLiteral("arrived")
                                                         : QStringLiteral("joined"))
                                              : QStringLiteral("left"));
        if (change.joined && !change.info.platform.isEmpty())
            text += QStringLiteral(" (%1)").arg(change.info.platform);
        noteCue(Scene::kDesktop, visitor ? Motion::Visitor : Motion::Client, text,
                Scene::logSeed(change.joined ? QStringLiteral("client-in")
                                             : QStringLiteral("client-out"),
                               name));
    }
    m_view->update();
}

void AgentWatchFeature::noteCue(int sessionId, Motion motion, const QString &text, quint32 seed)
{
    m_scene->tick();
    const bool heard = hosted() && host().playCue(sessionId, motion, seed);
    m_scene->cue(sessionId, motion, text, seed, heard);
}

void AgentWatchFeature::setHearAll(bool on)
{
    if (!hosted())
        return;
    m_hearAll = host().listenToEverything(on) && on;
    syncRoster();
}

void AgentWatchFeature::refreshMirrors()
{
    if (!hosted())
        return;
    m_mirrorsReadMs = QDateTime::currentMSecsSinceEpoch();
    QList<forkmesh::agentvis::MirrorSeat> seats;
    for (const QJsonValue &value : host().savedHosts()) {
        const QJsonObject saved = value.toObject();
        const QString ip = saved.value(QStringLiteral("ip")).toString().trimmed();
        QString name = saved.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            name = ip;
        if (name.isEmpty())
            continue;
        forkmesh::agentvis::MirrorSeat seat{name, {}};
        const QString origin =
            QUrl(saved.value(QStringLiteral("publicOrigin")).toString().trimmed()).host();
        for (const QString &where :
             {ip, saved.value(QStringLiteral("dnsHostname")).toString().trimmed(), origin}) {
            if (where.isEmpty())
                continue;
            QHostAddress literal;
            if (literal.setAddress(where)) {
                seat.addresses.append(socketAddressText(literal));
                continue;
            }
            seat.addresses.append(m_mirrorResolved.value(where));
            if (m_mirrorLookups.contains(where))
                continue;
            m_mirrorLookups.insert(where);
            QHostInfo::lookupHost(where, this, [this, where](const QHostInfo &info) {
                QStringList addresses;
                for (const QHostAddress &address : info.addresses())
                    addresses.append(socketAddressText(address));
                if (addresses.isEmpty())
                    return;
                m_mirrorResolved.insert(where, addresses);
                refreshMirrors();
            });
        }
        seats.append(seat);
    }
    m_scene->setMirrors(seats);
}

void AgentWatchFeature::sampleNetwork()
{
    if (!m_watching || m_sampleInFlight || !hosted())
        return;
    if (!m_ledger)
        m_ledger = std::make_unique<forkmesh::agentnet::Ledger>();
    QList<forkmesh::agentfeed::ProcessRoot> agentRoots;
    QList<forkmesh::agentfeed::ExternalRun> externals;
    host().processRoots(&agentRoots, &externals);
    QList<forkmesh::agentnet::Root> roots;
    roots.append({Scene::kDesktop, QCoreApplication::applicationPid(),
                  /*withDescendants=*/false});
    for (const forkmesh::agentfeed::ProcessRoot &root : std::as_const(agentRoots))
        roots.append({root.sessionId, root.pid, root.withDescendants});
    m_sampleInFlight = true;
    offThread<forkmesh::agentnet::Sample>(
        this,
        [roots, externals]() mutable {
            const SystemStats::ProcessTable table = SystemStats::processTable();
            if (!externals.isEmpty()) {
                QList<qint64> claudePids;
                for (auto entry = table.entries.cbegin(); entry != table.entries.cend(); ++entry) {
                    if (entry->command == QLatin1String("claude") && !entry->zombie)
                        claudePids.append(entry.key());
                }
                for (const forkmesh::agentfeed::ExternalRun &external : std::as_const(externals)) {
                    const QList<qint64> pids = ClaudeSessionScan::findSessionPids(
                        external.uuid, external.cwd, claudePids);
                    if (!pids.isEmpty())
                        roots.append({external.sessionId, pids.first(), true});
                }
            }
            return forkmesh::agentnet::sample(roots, table);
        },
        [this](forkmesh::agentnet::Sample sample) {
            m_sampleInFlight = false;
            if (!m_watching || !m_ledger)
                return;
            const QList<forkmesh::agentnet::Delta> deltas = m_ledger->update(sample);
            m_scene->tick();
            if (QDateTime::currentMSecsSinceEpoch() - m_mirrorsReadMs > kMirrorRereadMs)
                refreshMirrors();
            QSet<int> sampled;
            for (auto tree = sample.trees.cbegin(); tree != sample.trees.cend(); ++tree) {
                QList<forkmesh::agentvis::ProcessInfo> processes;
                processes.reserve(tree->processes.size());
                for (const forkmesh::agentnet::Process &process : tree->processes)
                    processes.append({process.pid, process.parentPid, process.name,
                                      process.command, process.cpuTicks});
                m_scene->sampleProcesses(tree.key(), processes, tree->rootPid);
                sampled.insert(tree.key());
            }
            QSet<int> stale;
            for (const forkmesh::agentvis::Process &process : m_scene->processes()) {
                if (process.running() && !sampled.contains(process.sessionId))
                    stale.insert(process.sessionId);
            }
            for (const int sessionId : std::as_const(stale))
                m_scene->sampleProcesses(sessionId, {}, 0);
            for (const forkmesh::agentnet::Delta &delta : deltas) {
                const QString name = hostName(delta.peer);
                const bool joined = m_scene->network(delta.sessionId, name, delta.up,
                                                     delta.down, delta.peer, delta.port);
                if (hosted())
                    host().playNetwork(delta.sessionId, delta.up, delta.down);
                const QString mirror = m_scene->mirrorForAddress(delta.peer);
                const QString endpoint =
                    forkmesh::agentvis::describeEndpoint(delta.peer, delta.port);
                if (joined && !mirror.isEmpty())
                    noteCue(delta.sessionId, Motion::Connect,
                            QStringLiteral("reached mirror %1 (%2)").arg(mirror, endpoint),
                            Scene::logSeed(QStringLiteral("host"), mirror));
                else if (joined)
                    noteCue(delta.sessionId, Motion::Connect,
                            name == delta.peer
                                ? QStringLiteral("connected to %1").arg(endpoint)
                                : QStringLiteral("connected to %1 (%2)").arg(name, endpoint),
                            Scene::logSeed(QStringLiteral("host"), name));
            }
        });
}

QString AgentWatchFeature::hostName(const QString &peer)
{
    if (forkmesh::agentnet::isLoopback(peer))
        return QStringLiteral("this machine");
    const QString mirror = m_scene->mirrorForAddress(peer);
    if (!mirror.isEmpty())
        return mirror;
    const auto known = m_networkNames.constFind(peer);
    if (known != m_networkNames.constEnd())
        return known.value();
    if (!m_networkLookups.contains(peer)) {
        m_networkLookups.insert(peer);
        QHostInfo::lookupHost(peer, this, [this, peer](const QHostInfo &info) {
            const QString name = info.error() == QHostInfo::NoError ? info.hostName()
                                                                    : QString();
            if (m_networkNames.contains(peer))
                return;
            if (name.isEmpty() || name == peer) {
                lookupWhois(peer);
                return;
            }
            m_networkNames.insert(peer, name);
            m_scene->renameHost(peer, name);
        });
    }
    return peer;
}

void AgentWatchFeature::lookupWhois(const QString &peer)
{
    const auto remember = [this, peer](const QStringList &lines) {
        m_scene->setAddressNetwork(peer, lines);
    };
    const QString kind = forkmesh::agentvis::addressInfo(peer);
    if (kind.isEmpty() || !kind.endsWith(QLatin1String("public"))) {
        remember({QStringLiteral("network: ")
                  + (kind.isEmpty() ? QStringLiteral("unknown address") : kind)
                  + QStringLiteral(" (not in whois)")});
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("whois")).isEmpty()) {
        remember({QStringLiteral("network: ") + kind,
                  QStringLiteral("whois: not installed (install the whois package)")});
        return;
    }
    remember({QStringLiteral("whois: looking up…")});
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    QTimer::singleShot(20000, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this,
            [process, remember, kind](int, QProcess::ExitStatus status) {
                const QString text = QString::fromUtf8(process->readAll());
                process->deleteLater();
                QStringList lines = forkmesh::agentvis::parseWhois(text);
                if (lines.isEmpty())
                    lines = {QStringLiteral("network: ") + kind,
                             status == QProcess::NormalExit
                                 ? QStringLiteral("whois: no registration found")
                                 : QStringLiteral("whois: timed out")};
                remember(lines);
            });
    connect(process, &QProcess::errorOccurred, this,
            [process, remember, kind](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                remember({QStringLiteral("network: ") + kind,
                          QStringLiteral("whois: could not run")});
            });
    process->start(QStringLiteral("whois"), {peer});
}

void AgentWatchFeature::seedNetworkNames()
{
    static const char *const kUsualHosts[] = {
        "api.anthropic.com",   "claude.ai",          "statsig.anthropic.com",
        "console.anthropic.com", "api.openai.com",   "chatgpt.com",
        "github.com",          "api.github.com",     "registry.npmjs.org",
        "pypi.org",            "files.pythonhosted.org",
    };
    for (const char *usual : kUsualHosts) {
        const QString name = QString::fromLatin1(usual);
        QHostInfo::lookupHost(name, this, [this, name](const QHostInfo &info) {
            for (const QHostAddress &address : info.addresses()) {
                const QString text = socketAddressText(address);
                m_networkNames.insert(text, name);
                m_scene->renameHost(text, name);
            }
        });
    }
}

QJsonArray AgentWatchFeature::visitorBlocksStored() const
{
    return QJsonDocument::fromJson(QSettings().value(QLatin1String(kVisitorBlocksSetting))
                                       .toByteArray())
        .array();
}

void AgentWatchFeature::refreshVisitorBlockButton()
{
    if (!m_visitorsButton)
        return;
    const int flagged = m_scene->flaggedCount();
    const Threat threat = m_scene->siteThreat();
    const QString text = threat == Threat::None
                             ? QStringLiteral("Visitors")
                             : QStringLiteral("⚠ Visitors (%1)").arg(qMax(1, flagged));
    if (m_visitorsButton->text() != text)
        m_visitorsButton->setText(text);
    const QString style =
        threat == Threat::Danger  ? QStringLiteral("QPushButton { color: #f85149; font-weight: bold; }")
        : threat == Threat::Watch ? QStringLiteral("QPushButton { color: #d29922; font-weight: bold; }")
                                  : QString();
    if (m_visitorsButton->styleSheet() != style)
        m_visitorsButton->setStyleSheet(style);
}

void AgentWatchFeature::resolveVisitorZone(const QString &token,
                                           std::function<void(QString, QString)> onDone)
{
    if (token == m_visitorZoneToken && !m_visitorZoneId.isEmpty()) {
        onDone(m_visitorZoneId, QString());
        return;
    }
    const QString configured = host().cloudflareZoneName();
    QPointer<AgentWatchFeature> self(this);
    host().cloudflare(
        token, QStringLiteral("/zones?per_page=50"), QByteArrayLiteral("GET"), {},
        [self, token, configured, onDone](QJsonObject result, QString error) {
            if (!self)
                return;
            if (!error.isEmpty()) {
                onDone({}, error);
                return;
            }
            const QJsonArray zones = result.value(QStringLiteral("result")).toArray();
            const QString siteHost = self->m_siteHost.toLower();
            QString best;
            QString bestName;
            QString named;
            for (const QJsonValue &value : zones) {
                const QJsonObject zone = value.toObject();
                const QString id = zone.value(QStringLiteral("id")).toString();
                const QString name = zone.value(QStringLiteral("name")).toString().toLower();
                if (id.isEmpty() || name.isEmpty())
                    continue;
                if (!siteHost.isEmpty()
                    && (siteHost == name || siteHost.endsWith(QLatin1Char('.') + name))
                    && name.size() > bestName.size()) {
                    best = id;
                    bestName = name;
                }
                if (!configured.isEmpty() && name == configured.toLower())
                    named = id;
            }
            if (best.isEmpty())
                best = named;
            if (best.isEmpty() && zones.size() == 1)
                best = zones.first().toObject().value(QStringLiteral("id")).toString();
            if (best.isEmpty()) {
                onDone({}, siteHost.isEmpty()
                               ? QStringLiteral("this token sees %1 zones, and no request has "
                                                "said which one serves the site yet")
                                     .arg(zones.size())
                               : QStringLiteral("this token sees no zone serving %1").arg(siteHost));
                return;
            }
            self->m_visitorZoneId = best;
            self->m_visitorZoneToken = token;
            onDone(best, QString());
        });
}

void AgentWatchFeature::blockVisitors(const QList<BlockRule> &rules)
{
    if (rules.isEmpty() || !hosted())
        return;
    const QString token = host().cloudflareToken();
    if (token.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Block web visitors"),
            QStringLiteral("Blocking at the edge needs a Cloudflare API token with "
                           "“Firewall Services: Edit” on the site's zone. Add it "
                           "as CLOUDFLARE_API_TOKEN in Settings > Secrets & Coves."));
        return;
    }
    QPointer<AgentWatchFeature> self(this);
    resolveVisitorZone(token, [self, token, rules](QString zoneId, QString error) {
        if (!self || !hosted())
            return;
        if (!error.isEmpty()) {
            host().ping(QStringLiteral("Could not block: ") + error, true);
            host().log(QStringLiteral("Cloud: could not block web visitors: ") + error);
            return;
        }
        for (const BlockRule &rule : rules) {
            const bool agent = rule.target == BlockTarget::UserAgent;
            QString target = forkmesh::agentvis::blockTargetKey(rule.target);
            if (rule.target == BlockTarget::Address && rule.value.contains(QLatin1Char(':')))
                target = QStringLiteral("ip6");
            const QString note =
                QStringLiteral("ForkMesh desktop, Agent activity: %1").arg(rule.reason).left(500);
            QJsonObject body{
                {QStringLiteral("mode"), blockMode(rule)},
                {QStringLiteral("configuration"),
                 QJsonObject{{QStringLiteral("target"), agent ? QStringLiteral("ua") : target},
                             {QStringLiteral("value"), rule.value}}}};
            if (agent) {
                body.insert(QStringLiteral("description"), note);
                body.insert(QStringLiteral("paused"), false);
            } else {
                body.insert(QStringLiteral("notes"), note);
            }
            const QString path = QStringLiteral("/zones/%1/firewall/%2")
                                     .arg(zoneId, agent ? QStringLiteral("ua_rules")
                                                        : QStringLiteral("access_rules/rules"));
            host().cloudflare(
                token, path, QByteArrayLiteral("POST"), body,
                [self, rule, zoneId, agent](QJsonObject result, QString failure) {
                    if (!self || !hosted())
                        return;
                    const bool duplicate =
                        failure.contains(QLatin1String("duplicate"), Qt::CaseInsensitive)
                        || failure.contains(QLatin1String("already exists"), Qt::CaseInsensitive);
                    if (!failure.isEmpty() && !duplicate) {
                        host().ping(QStringLiteral("Could not block %1: %2")
                                        .arg(rule.label, failure),
                                    true);
                        host().log(QStringLiteral("Cloud: could not block %1: %2")
                                       .arg(rule.label, failure));
                        return;
                    }
                    const QString id = result.value(QStringLiteral("result"))
                                           .toObject()
                                           .value(QStringLiteral("id"))
                                           .toString();
                    QJsonArray stored = self->visitorBlocksStored();
                    stored.append(QJsonObject{
                        {QStringLiteral("kind"), agent ? QStringLiteral("ua")
                                                       : QStringLiteral("access")},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("zone"), zoneId},
                        {QStringLiteral("target"), forkmesh::agentvis::blockTargetKey(rule.target)},
                        {QStringLiteral("value"), rule.value},
                        {QStringLiteral("label"), rule.label},
                        {QStringLiteral("reason"), rule.reason},
                        {QStringLiteral("mode"), blockMode(rule)},
                        {QStringLiteral("createdMs"), QDateTime::currentMSecsSinceEpoch()}});
                    QSettings().setValue(QLatin1String(kVisitorBlocksSetting),
                                         QJsonDocument(stored).toJson(QJsonDocument::Compact));
                    self->m_scene->noteBlocked(rule);
                    const QString done =
                        QStringLiteral("%1 %2 at Cloudflare's edge%3")
                            .arg(rule.target == BlockTarget::Country ? QStringLiteral("Challenged")
                                                                     : QStringLiteral("Blocked"),
                                 rule.label,
                                 duplicate ? QStringLiteral(" (a rule was already there)")
                                           : QString());
                    host().ping(done, false);
                    host().log(QStringLiteral("Cloud: ") + done + QStringLiteral(" - ") + rule.reason);
                    self->refreshVisitorBlockButton();
                    self->m_view->update();
                });
        }
    });
}

void AgentWatchFeature::unblockVisitor(const QJsonObject &stored)
{
    if (!hosted())
        return;
    const QString id = stored.value(QStringLiteral("id")).toString();
    const QString zone = stored.value(QStringLiteral("zone")).toString();
    const QString token = host().cloudflareToken();
    const auto forget = [stored] {
        QJsonArray kept;
        const QJsonArray all = QJsonDocument::fromJson(
                                   QSettings().value(QLatin1String(kVisitorBlocksSetting))
                                       .toByteArray())
                                   .array();
        for (const QJsonValue &value : all) {
            if (value.toObject() != stored)
                kept.append(value);
        }
        QSettings().setValue(QLatin1String(kVisitorBlocksSetting),
                             QJsonDocument(kept).toJson(QJsonDocument::Compact));
    };
    if (id.isEmpty() || zone.isEmpty() || token.isEmpty()) {
        forget();
        return;
    }
    const bool agent = stored.value(QStringLiteral("kind")).toString() == QLatin1String("ua");
    const QString label = stored.value(QStringLiteral("label")).toString();
    host().cloudflare(
        token,
        QStringLiteral("/zones/%1/firewall/%2/%3")
            .arg(zone, agent ? QStringLiteral("ua_rules") : QStringLiteral("access_rules/rules"),
                 id),
        QByteArrayLiteral("DELETE"), {},
        [forget, label](QJsonObject, QString error) {
            if (!hosted())
                return;
            if (!error.isEmpty() && !error.contains(QLatin1String("HTTP 404"))) {
                host().ping(QStringLiteral("Could not lift the block on %1: %2")
                                .arg(label, error),
                            true);
                return;
            }
            forget();
            host().ping(QStringLiteral("Lifted the block on ") + label, false);
            host().log(QStringLiteral("Cloud: lifted the block on ") + label);
        });
}

void AgentWatchFeature::offerVisitorBlock(const QString &clientId, const QPoint &globalPos)
{
    const forkmesh::agentvis::Client *client = m_scene->client(clientId);
    if (!client)
        return;
    QMenu menu(m_view);
    menu.setObjectName(QStringLiteral("agentVisitorBlockMenu"));
    menu.setToolTipsVisible(true);
    QString name = client->info.name.trimmed();
    if (name.isEmpty())
        name = client->info.nodeName.trimmed();
    menu.addSection(name.isEmpty() ? client->info.id.left(16) : name);
    const QList<BlockRule> rules = m_scene->blockRules(clientId);
    if (!client->web || rules.isEmpty()) {
        QAction *none = menu.addAction(
            client->web ? QStringLiteral("The Worker's log gave no address to block")
                        : QStringLiteral("In the relay room, not the Worker's log: no "
                                         "address to block"));
        none->setEnabled(false);
    } else if (client->blocked) {
        menu.addAction(QStringLiteral("Already blocked at the edge"))->setEnabled(false);
    } else {
        for (const BlockRule &rule : rules) {
            QString text = QStringLiteral("%1 %2 (%3 now)")
                               .arg(blockVerb(rule), rule.label)
                               .arg(rule.visitors);
            if (rule.recommended)
                text = QStringLiteral("★ ") + text + QStringLiteral(" - recommended");
            QAction *action = menu.addAction(text);
            action->setToolTip(rule.reason);
            connect(action, &QAction::triggered, this, [this, rule] {
                const QString question =
                    QStringLiteral("%1 %2 at Cloudflare's edge?\n\n%3.\n\nIt catches %4 of the "
                                   "visitors on the field now, %5 of them flagged. The rule "
                                   "stays until you lift it under Visitors.")
                        .arg(blockVerb(rule), rule.label, rule.reason)
                        .arg(rule.visitors)
                        .arg(rule.flagged);
                if (QMessageBox::question(m_view, QStringLiteral("Block web visitors"), question)
                    == QMessageBox::Yes)
                    blockVisitors({rule});
            });
        }
    }
    menu.addSeparator();
    if (!client->info.address.isEmpty()) {
        const QString address = client->info.address;
        connect(menu.addAction(QStringLiteral("Copy address")), &QAction::triggered, this,
                [address] { QGuiApplication::clipboard()->setText(address); });
    }
    if (!client->info.userAgent.isEmpty()) {
        const QString agent = client->info.userAgent;
        connect(menu.addAction(QStringLiteral("Copy user agent")), &QAction::triggered, this,
                [agent] { QGuiApplication::clipboard()->setText(agent); });
    }
    connect(menu.addAction(QStringLiteral("Every threat and block…")), &QAction::triggered,
            this, [this] { openVisitorBlocks(); });
    menu.exec(globalPos);
}

void AgentWatchFeature::openVisitorBlocks()
{
    Scene *scene = m_scene.get();
    QDialog dialog(m_view);
    dialog.setObjectName(QStringLiteral("agentVisitorBlocksDialog"));
    dialog.setWindowTitle(QStringLiteral("Web visitors: threats and blocks"));
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);

    const Threat threat = scene->siteThreat();
    auto *summary = new QLabel(&dialog);
    summary->setWordWrap(true);
    summary->setText(
        threat == Threat::None
            ? QStringLiteral("Nothing on the field looks like an attack right now: %1 web "
                             "visitor%2, %3 newcomer%4 in the last minute.")
                  .arg(scene->clientCount(forkmesh::agentvis::ClientKind::Visitor))
                  .arg(scene->clientCount(forkmesh::agentvis::ClientKind::Visitor) == 1
                           ? QString()
                           : QStringLiteral("s"))
                  .arg(scene->arrivalRate())
                  .arg(scene->arrivalRate() == 1 ? QString() : QStringLiteral("s"))
            : QStringLiteral("<b>⚠ %1:</b> %2")
                  .arg(forkmesh::agentvis::threatLabel(threat),
                       scene->siteThreatReasons().join(QStringLiteral(" · ")).toHtmlEscaped()));
    layout->addWidget(summary);

    layout->addWidget(new QLabel(
        QStringLiteral("Suggested blocks - the narrowest rule that stops each attack:"), &dialog));
    auto *suggested = new QTreeWidget(&dialog);
    suggested->setObjectName(QStringLiteral("agentVisitorSuggestedBlocks"));
    suggested->setHeaderLabels({QStringLiteral("Rule"), QStringLiteral("Catches"),
                                QStringLiteral("Why")});
    suggested->setRootIsDecorated(false);
    const QList<BlockRule> rules = scene->suggestedBlocks();
    for (int i = 0; i < rules.size(); ++i) {
        const BlockRule &rule = rules.at(i);
        auto *item = new QTreeWidgetItem(suggested);
        item->setText(0, QStringLiteral("%1 %2").arg(blockVerb(rule), rule.label));
        item->setText(1, QStringLiteral("%1 (%2 flagged)").arg(rule.visitors).arg(rule.flagged));
        item->setText(2, rule.reason);
        item->setToolTip(2, rule.reason);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, rule.target == BlockTarget::Country ? Qt::Unchecked : Qt::Checked);
        item->setData(0, Qt::UserRole, i);
    }
    if (rules.isEmpty()) {
        auto *item = new QTreeWidgetItem(suggested);
        item->setText(0, QStringLiteral("Nothing flagged - click a visitor on the field to "
                                        "block it by hand"));
        item->setFlags(Qt::NoItemFlags);
    }
    suggested->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    suggested->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(suggested, 1);
    auto *blockChecked = new QPushButton(QStringLiteral("Block checked"), &dialog);
    blockChecked->setObjectName(QStringLiteral("agentVisitorBlockCheckedButton"));
    blockChecked->setEnabled(!rules.isEmpty());
    layout->addWidget(blockChecked, 0, Qt::AlignRight);
    connect(blockChecked, &QPushButton::clicked, &dialog, [this, &dialog, suggested, rules] {
        QList<BlockRule> chosen;
        for (int row = 0; row < suggested->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = suggested->topLevelItem(row);
            if (item->checkState(0) == Qt::Checked)
                chosen << rules.value(item->data(0, Qt::UserRole).toInt());
        }
        if (chosen.isEmpty())
            return;
        if (QMessageBox::question(&dialog, QStringLiteral("Block web visitors"),
                                  QStringLiteral("Make %1 rule%2 at Cloudflare's edge? They stay "
                                                 "until you lift them here.")
                                      .arg(chosen.size())
                                      .arg(chosen.size() == 1 ? QString() : QStringLiteral("s")))
            != QMessageBox::Yes)
            return;
        blockVisitors(chosen);
        dialog.accept();
    });

    layout->addWidget(new QLabel(QStringLiteral("Made from this desktop:"), &dialog));
    auto *made = new QTreeWidget(&dialog);
    made->setObjectName(QStringLiteral("agentVisitorMadeBlocks"));
    made->setHeaderLabels({QStringLiteral("Rule"), QStringLiteral("Mode"), QStringLiteral("Made")});
    made->setRootIsDecorated(false);
    made->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const QJsonArray stored = visitorBlocksStored();
    for (int i = 0; i < stored.size(); ++i) {
        const QJsonObject entry = stored.at(i).toObject();
        auto *item = new QTreeWidgetItem(made);
        item->setText(0, entry.value(QStringLiteral("label")).toString());
        item->setToolTip(0, entry.value(QStringLiteral("reason")).toString());
        item->setText(1, entry.value(QStringLiteral("mode")).toString());
        item->setText(2, QDateTime::fromMSecsSinceEpoch(
                             qint64(entry.value(QStringLiteral("createdMs")).toDouble()))
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setData(0, Qt::UserRole, i);
    }
    made->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(made, 1);
    auto *lift = new QPushButton(QStringLiteral("Lift selected"), &dialog);
    lift->setObjectName(QStringLiteral("agentVisitorLiftButton"));
    lift->setEnabled(false);
    connect(made, &QTreeWidget::itemSelectionChanged, lift,
            [made, lift] { lift->setEnabled(!made->selectedItems().isEmpty()); });
    connect(lift, &QPushButton::clicked, &dialog, [this, &dialog, made, stored] {
        for (QTreeWidgetItem *item : made->selectedItems())
            unblockVisitor(stored.at(item->data(0, Qt::UserRole).toInt()).toObject());
        dialog.accept();
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->addButton(lift, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QList<forkmesh::market::NativePackageSearchHit>
AgentWatchFeature::searchHits(const QString &, int) const
{
    return {};
}

void AgentWatchFeature::activateSearchHit(const QString &)
{
}

} // namespace forkmesh::agentwatch

namespace {
const forkmesh::market::NativePackageRegistration agentWatchRegistration({
    QStringLiteral("builtin:agent-watch"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Watch"),
    QStringLiteral("broadcast"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::agentwatch::AgentWatchFeature(host, parent);
    },
});
} // namespace
