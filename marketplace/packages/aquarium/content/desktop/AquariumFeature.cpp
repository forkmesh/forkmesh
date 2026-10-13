#include "AquariumFeature.h"

#include "AgentActivityFeed.h"
#include "AquariumOverlay.h"

#include <QCheckBox>
#include <QDateTime>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMap>
#include <QPixmap>
#include <QMainWindow>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::aquarium {
namespace {

const QString kEnabledSetting = QStringLiteral("market/packages/aquarium/data/enabled");
const QString kDelaySetting = QStringLiteral("market/packages/aquarium/data/delaySeconds");
constexpr int kLatelyRows = 60;
constexpr int kMaxFish = 60;
constexpr int kPortrait = 56;

bool hasAudience()
{
#ifdef FORKMESH_WINDOW_TESTS
    return false;
#else
    const QString platform = QGuiApplication::platformName();
    return !platform.contains(QStringLiteral("offscreen"), Qt::CaseInsensitive) &&
           !platform.contains(QStringLiteral("minimal"), Qt::CaseInsensitive);
#endif
}

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QWidget *mainWindow(QWidget *page)
{
    if (auto *own = qobject_cast<QMainWindow *>(page->window()))
        return own;
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (auto *main = qobject_cast<QMainWindow *>(top); main && main->isVisible())
            return main;
    }
    return nullptr;
}

Mood moodOf(const forkmesh::agentfeed::Agent &agent)
{
    using forkmesh::agentfeed::RunState;
    switch (agent.state) {
    case RunState::Running:
        return Mood::Working;
    case RunState::Waiting:
        return Mood::Waiting;
    case RunState::Done:
        return agent.merged ? Mood::Merged : Mood::Done;
    case RunState::Failed:
        return agent.stopped ? Mood::Stopped : Mood::Failed;
    case RunState::Idle:
        break;
    }
    return agent.merged ? Mood::Merged : Mood::Queued;
}

int rank(Mood mood)
{
    switch (mood) {
    case Mood::Waiting:
    case Mood::Working:
        return 0;
    case Mood::Queued:
        return 1;
    case Mood::Failed:
        return 2;
    case Mood::Done:
    case Mood::Merged:
        return 3;
    case Mood::Stopped:
        return 4;
    }
    return 5;
}

QString describe(const Swimmer &swimmer)
{
    QStringList parts{Scene::moodName(swimmer.mood)};
    if (swimmer.files >= 0)
        parts << (swimmer.files == 1 ? QStringLiteral("1 changed file") : QStringLiteral("%1 changed files").arg(swimmer.files));
    if (swimmer.lines > 0)
        parts << QStringLiteral("%1 lines").arg(swimmer.lines);
    if (swimmer.conflicted)
        parts << QStringLiteral("merge conflict");
    return parts.join(QStringLiteral(", "));
}

QString trickList(int from, int count)
{
    QStringList names;
    for (int i = from; i < from + count; ++i)
        names << Scene::trickName(Trick(i));
    return names.join(QStringLiteral(", "));
}

} // namespace

bool AquariumFeature::enabled()
{
    return QSettings().value(kEnabledSetting, true).toBool();
}

int AquariumFeature::delaySeconds()
{
    return std::clamp(QSettings().value(kDelaySetting, 3).toInt(), 0, 600);
}

QList<Swimmer> AquariumFeature::roster(bool guests)
{
    using namespace forkmesh::agentfeed;
    QList<Swimmer> swimmers;
    if (hasHost() && host().agents) {
        for (const Agent &agent : host().agents()) {
            const int lines = agent.linesAdded >= 0 && agent.linesRemoved >= 0 ? agent.linesAdded + agent.linesRemoved : -1;
            swimmers.append({agent.id, agent.name, agent.faceKey, moodOf(agent), qint64(agent.turns) * 100000 + agent.tokens,
                             agent.files, lines, agent.conflicted});
        }
    }
    std::stable_sort(swimmers.begin(), swimmers.end(),
                     [](const Swimmer &a, const Swimmer &b) { return rank(a.mood) < rank(b.mood); });
    if (swimmers.size() > kMaxFish)
        swimmers.resize(kMaxFish);
    if (guests && swimmers.isEmpty()) {
        swimmers.append({-101, QStringLiteral("Guest goldie"), QStringLiteral("a"), Mood::Working, 0});
        swimmers.append({-102, QStringLiteral("Guest bubbles"), QStringLiteral("bb"), Mood::Working, 0});
        swimmers.append({-103, QStringLiteral("Guest finn"), QStringLiteral("c"), Mood::Working, 0});
    }
    return swimmers;
}

AquariumFeature::AquariumFeature(const forkmesh::market::NativePackageHost &, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
{
    setObjectName(QStringLiteral("aquariumSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Aquarium"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Every agent swims as a fish whenever ForkMesh is in the background")));
    heading->addStretch();
    auto *show = new QPushButton(QStringLiteral("Show it now"));
    show->setObjectName(QStringLiteral("primaryButton"));
    heading->addWidget(show);
    outer->addLayout(heading);

    m_enabled = new QCheckBox(QStringLiteral("Fill the window with the aquarium when ForkMesh is not the active app"));
    m_enabled->setChecked(enabled());
    outer->addWidget(m_enabled);

    auto *delayRow = new QHBoxLayout;
    delayRow->addWidget(new QLabel(QStringLiteral("Start after")));
    m_delay = new QSpinBox;
    m_delay->setRange(0, 600);
    m_delay->setSuffix(QStringLiteral(" s"));
    m_delay->setValue(delaySeconds());
    delayRow->addWidget(m_delay);
    delayRow->addWidget(mutedLabel(QStringLiteral("in the background. Coming back to the window drains the tank at once.")));
    delayRow->addStretch();
    outer->addLayout(delayRow);

    outer->addWidget(sectionLabel(QStringLiteral("IN THE TANK")));
    m_tank = mutedLabel(QString());
    outer->addWidget(m_tank);
    m_fish = new QListWidget;
    m_fish->setObjectName(QStringLiteral("aquariumFish"));
    m_fish->setViewMode(QListView::IconMode);
    m_fish->setFlow(QListView::LeftToRight);
    m_fish->setWrapping(true);
    m_fish->setResizeMode(QListView::Adjust);
    m_fish->setMovement(QListView::Static);
    m_fish->setIconSize(QSize(kPortrait, kPortrait));
    m_fish->setGridSize(QSize(kPortrait + 52, kPortrait + 34));
    m_fish->setWordWrap(true);
    m_fish->setTextElideMode(Qt::ElideRight);
    m_fish->setSelectionMode(QAbstractItemView::NoSelection);
    m_fish->setFocusPolicy(Qt::NoFocus);
    m_fish->setMaximumHeight(2 * (kPortrait + 34) + 8);
    outer->addWidget(m_fish);

    outer->addWidget(sectionLabel(QStringLiteral("TRICKS")));
    outer->addWidget(mutedLabel(QStringLiteral("Around the window: %1.").arg(trickList(0, kElementTricks))));
    outer->addWidget(mutedLabel(QStringLiteral("With the spider: %1.").arg(trickList(kElementTricks, kPetTricks))));

    outer->addWidget(sectionLabel(QStringLiteral("LATELY")));
    m_lately = new QListWidget;
    m_lately->setObjectName(QStringLiteral("aquariumLately"));
    m_lately->setSelectionMode(QAbstractItemView::NoSelection);
    m_lately->setFocusPolicy(Qt::NoFocus);
    outer->addWidget(m_lately, 1);

    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(kEnabledSetting, on);
        if (!on) {
            m_wait->stop();
            if (m_overlay && !m_overlay->previewing())
                m_overlay->dismiss();
        }
    });
    connect(m_delay, &QSpinBox::valueChanged, this, [](int seconds) { QSettings().setValue(kDelaySetting, seconds); });
    connect(show, &QPushButton::clicked, this, [this] { preview(); });

    m_wait = new QTimer(this);
    m_wait->setSingleShot(true);
    connect(m_wait, &QTimer::timeout, this, [this] { wake(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) { stateChanged(state); });
    refresh();
}

AquariumFeature::~AquariumFeature()
{
    delete m_overlay.data();
}

void AquariumFeature::refresh()
{
    const QList<Swimmer> swimmers = roster(false);
    m_fish->clear();
    m_fish->setVisible(!swimmers.isEmpty());
    if (swimmers.isEmpty()) {
        m_tank->setText(QStringLiteral("There are no agents yet, so the tank is empty apart from the spider. "
                                       "Show it now brings three guest fish."));
        return;
    }
    QMap<int, int> counts;
    const qreal ratio = devicePixelRatioF();
    for (const Swimmer &swimmer : swimmers) {
        ++counts[int(swimmer.mood)];
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(Scene::portrait(swimmer, kPortrait, ratio))), swimmer.name);
        item->setToolTip(QStringLiteral("%1: %2").arg(swimmer.name, describe(swimmer)));
        item->setForeground(Scene::moodColor(swimmer.mood));
        m_fish->addItem(item);
    }
    QStringList tally;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        tally << QStringLiteral("%1 %2").arg(it.value()).arg(Scene::moodName(Mood(it.key())));
    m_tank->setText(QStringLiteral("%1 %2 in the tank: %3. Each fish shows its agent: the colour and how it swims "
                                   "follow the status, it grows with the files it changed, its fins with the lines, "
                                   "and a merge conflict tangles it in red line.")
                        .arg(swimmers.size())
                        .arg(swimmers.size() == 1 ? QStringLiteral("fish") : QStringLiteral("fish are"))
                        .arg(tally.join(QStringLiteral(", "))));
}

Overlay *AquariumFeature::overlay()
{
    QWidget *host = mainWindow(this);
    if (!host)
        return nullptr;
    if (m_overlay && m_overlay->parentWidget() == host)
        return m_overlay;
    delete m_overlay.data();
    m_overlay = new Overlay(host);
    m_overlay->swimmers = [](bool preview) { return roster(preview); };
    m_overlay->noted = [this](const Note &note) { this->note(note); };
    m_overlay->picked = [](int fishId) {
        namespace feed = forkmesh::agentfeed;
        if (!feed::hasHost() || !feed::host().showAgent || !feed::host().agents)
            return;
        const QList<feed::Agent> agents = feed::host().agents();
        if (std::any_of(agents.cbegin(), agents.cend(), [fishId](const feed::Agent &agent) { return agent.id == fishId; }))
            feed::host().showAgent(fishId);
    };
    return m_overlay;
}

void AquariumFeature::stateChanged(Qt::ApplicationState state)
{
    if (state == Qt::ApplicationActive) {
        m_wait->stop();
        if (m_overlay)
            m_overlay->activated();
        return;
    }
    if (m_overlay && m_overlay->previewing()) {
        m_overlay->settle();
        return;
    }
    if (QWidget *host = mainWindow(this))
        host->installEventFilter(this);
    wait();
}

void AquariumFeature::wait()
{
    if (!enabled() || !hasAudience() || QGuiApplication::applicationState() == Qt::ApplicationActive ||
        (m_overlay && m_overlay->opened()))
        return;
    m_wait->start(std::max(50, delaySeconds() * 1000));
}

bool AquariumFeature::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Leave && watched == mainWindow(this))
        wait();
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

void AquariumFeature::wake()
{
    if (!enabled() || QGuiApplication::applicationState() == Qt::ApplicationActive)
        return;
    QWidget *host = mainWindow(this);
    if (!host || !host->isVisible() || host->isMinimized() || host->underMouse())
        return;
    if (Overlay *tank = overlay())
        tank->open(false);
}

void AquariumFeature::preview()
{
    if (Overlay *tank = overlay())
        tank->open(true);
}

void AquariumFeature::note(const Note &note)
{
    const QString time = QLocale().toString(QTime::currentTime(), QStringLiteral("h:mm AP"));
    m_lately->insertItem(0, QStringLiteral("%1   %2 %3").arg(time, note.fish, note.text));
    while (m_lately->count() > kLatelyRows)
        delete m_lately->takeItem(m_lately->count() - 1);
}

} // namespace forkmesh::aquarium

namespace {
const forkmesh::market::NativePackageRegistration aquariumRegistration({
    QStringLiteral("builtin:aquarium"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Aquarium"),
    QStringLiteral("fish"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::aquarium::AquariumFeature(host, parent);
    },
});
} // namespace
