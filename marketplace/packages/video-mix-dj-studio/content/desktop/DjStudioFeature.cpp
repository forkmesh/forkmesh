#include "DjStudioFeature.h"
#include "DjBrowserView.h"
#include "DjDeckView.h"
#include "DjDialogs.h"
#include "DjMixerView.h"
#include "DjModel.h"
#include "DjPlayer.h"
#include "DjWidgets.h"

#include "AppPaths.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::djstudio {

namespace {

const QString packageId = QStringLiteral("video-mix-dj-studio");
const QString webPath = QStringLiteral("/dashboard/video-mix-dj-studio");

DjButton *barButton(const QString &text, QWidget *parent, int width = 0)
{
    auto *b = new DjButton(text, parent);
    b->setFontSize(9);
    b->setFixedHeight(22);
    if (width)
        b->setFixedWidth(width);
    return b;
}

QUrl originFor(const market::NativePackageHost &host)
{
    if (host.publicPageUrl) {
        QUrl url(host.publicPageUrl(QStringLiteral("/")));
        if (url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty()) {
            url.setPath(QStringLiteral("/"));
            url.setQuery(QString());
            url.setFragment(QString());
            return url;
        }
    }
    return QUrl(QStringLiteral("https://forkmesh.com/"));
}

}

DjStudioFeature::DjStudioFeature(const market::NativePackageHost &host, QWidget *parent)
    : market::NativePackageWidget(parent), m_host(host)
{
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    m_player = new DjPlayer(originFor(host), this);
    DjModel::Host modelHost;
    modelHost.request = host.request;
    modelHost.network = host.network;
    modelHost.accountName = host.accountName;
    modelHost.userDirectory = host.userDirectory;
    modelHost.dataDir = paths::pluginDataDir(packageId);
    m_model = new DjModel(modelHost, m_player, this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    if (!DjPlayer::available()) {
        m_banner = new QWidget(this);
        m_banner->setStyleSheet(QStringLiteral("background:#3a1d0b;"));
        auto *row = new QHBoxLayout(m_banner);
        row->setContentsMargins(10, 6, 10, 6);
        auto *text = new QLabel(QStringLiteral("This build of ForkMesh has no Qt WebEngine, so YouTube cannot play inside the app. "
                                               "Install qt6-webengine-dev (Debian and Ubuntu) and rebuild the desktop, or use the web studio."),
                                m_banner);
        text->setWordWrap(true);
        text->setStyleSheet(QStringLiteral("color:#ffd9b8;font-size:12px;"));
        auto *open = new QPushButton(QStringLiteral("Open the Web Studio"), m_banner);
        connect(open, &QPushButton::clicked, this, [this]() {
            if (m_host.openWebPage)
                m_host.openWebPage(webPath);
        });
        row->addWidget(text, 1);
        row->addWidget(open);
        root->addWidget(m_banner);
    }
    root->addWidget(buildTopBar());

    m_waves = new QWidget(this);
    m_wavesLayout = new QVBoxLayout(m_waves);
    m_wavesLayout->setContentsMargins(4, 2, 4, 2);
    m_wavesLayout->setSpacing(2);
    root->addWidget(m_waves);

    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setHandleWidth(4);
    m_split->setStyleSheet(QStringLiteral("QSplitter::handle{background:#0d0e10;}"));
    auto *scroll = new QScrollArea(m_split);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#15161a;}"));
    m_deckArea = new QWidget(scroll);
    m_deckArea->setStyleSheet(QStringLiteral("background:#15161a;"));
    m_deckGrid = new QGridLayout(m_deckArea);
    m_deckGrid->setContentsMargins(4, 2, 4, 2);
    m_deckGrid->setSpacing(4);
    scroll->setWidget(m_deckArea);
    for (int i = 0; i < 4; ++i) {
        auto *view = new DjDeckView(m_model, i, i % 2 == 1, m_deckArea);
        view->menuRequested = [this](int deck, const QPoint &global) { deckMenu(deck, global); };
        view->fileRequested = [this](int deck, const QPoint &global) { fileMenu(deck, global); };
        view->similarRequested = [this](int deck) {
            if (!m_model->deck(deck).loaded())
                return;
            m_model->findSimilar(m_model->deck(deck).videoId);
            m_browser->showSource(QStringLiteral("similar"));
        };
        view->loadRequested = [this](int) { m_browser->focusSearch(); };
        m_decks.append(view);
    }
    m_mixer = new DjMixerView(m_model, m_player->view(), m_deckArea);
    m_mixer->focusMenuRequested = [this]() { focusMenu(QCursor::pos()); };
    m_browser = new DjBrowserView(m_model, m_split);
    m_browser->songEditRequested = [this](const QString &id) { openSong(id); };
    m_browser->exportRequested = [this](const DjFocus &focus) { exportList(focus); };
    m_browser->importRequested = [this]() { importFile(); };
    m_browser->pasteRequested = [this]() { pasteSongs(); };
    m_browser->shortcut = [this](QKeyEvent *event) { return shortcut(event, true); };
    m_split->addWidget(scroll);
    m_split->addWidget(m_browser);
    m_split->setStretchFactor(0, 3);
    m_split->setStretchFactor(1, 2);
    m_split->setSizes({470, 280});
    root->addWidget(m_split, 1);

    m_overlay = new DjReactionOverlay(m_model, this);
    m_notice = new QLabel(this);
    m_notice->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_notice->setAlignment(Qt::AlignCenter);
    m_notice->setWordWrap(true);
    m_notice->setStyleSheet(QStringLiteral("background:rgba(20,22,26,235);color:#e6e8eb;border:1px solid #23a6ff;"
                                           "border-radius:6px;padding:7px 14px;font-size:13px;font-weight:bold;"));
    m_notice->hide();
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    connect(m_noticeTimer, &QTimer::timeout, m_notice, &QWidget::hide);

    m_model->deckChanged = [this](int deck) {
        if (deck >= 0 && deck < m_decks.size())
            m_decks.at(deck)->refresh();
        m_mixer->refresh();
        railSoon();
    };
    m_model->changed = [this]() {
        m_mixer->refresh();
        m_automix->setLit(m_model->automixOn());
        m_automix->setBlink(m_model->automixing());
        m_autoDj->setLit(m_model->autoDjOn());
        m_autoDj->setBlink(m_model->autoDjFinding());
        m_browser->refreshIcons();
        if (m_model->deckCount() != m_scrolls.size())
            layoutDecks();
        railSoon();
    };
    m_model->libraryChanged = [this]() {
        m_browser->refreshSources();
        m_browser->refreshList();
        m_mixer->refresh();
        if (m_host.searchIndexChanged)
            m_host.searchIndexChanged();
    };
    m_model->searchChanged = [this]() { m_browser->refreshList(); };
    m_model->similarChanged = [this]() { m_browser->refreshList(); };
    m_model->jamChanged = [this]() {
        m_people->refresh();
        if (m_jamDialog)
            m_jamDialog->refresh();
        m_browser->refreshSources();
        if (m_host.navigationBadgeChanged)
            m_host.navigationBadgeChanged();
    };
    m_model->noticed = [this](const QString &text) { notice(text); };
    m_model->effectsChanged = [this]() { m_overlay->update(); };

    m_frame = new QTimer(this);
    m_frame->setInterval(16);
    m_frame->setTimerType(Qt::PreciseTimer);
    connect(m_frame, &QTimer::timeout, this, [this]() { tick(); });
    m_slow = new QTimer(this);
    m_slow->setInterval(500);
    connect(m_slow, &QTimer::timeout, this, [this]() { slowTick(); });
    m_slow->start();
    m_railTimer = new QTimer(this);
    m_railTimer->setSingleShot(true);
    m_railTimer->setInterval(400);
    connect(m_railTimer, &QTimer::timeout, this, [this]() {
        if (m_host.railChanged)
            m_host.railChanged();
    });
    layoutDecks();
}

DjStudioFeature::~DjStudioFeature()
{
    delete m_model;
    m_model = nullptr;
    delete m_player;
    m_player = nullptr;
}

QWidget *DjStudioFeature::buildTopBar()
{
    auto *bar = new QWidget(this);
    bar->setFixedHeight(34);
    bar->setStyleSheet(QStringLiteral("background:#1d1e21;"));
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(6);
    auto *logo = new QLabel(QStringLiteral("FORKMESH<span style='color:#fff;font-weight:400'> DJ</span>"), bar);
    logo->setTextFormat(Qt::RichText);
    logo->setStyleSheet(QStringLiteral("background:#c81e1e;color:#ffffff;font-size:14px;font-weight:900;padding:2px 8px;border-radius:2px;"));
    row->addWidget(logo);
    m_clock = new QLabel(bar);
    m_clock->setFont(theme::mono(13, true));
    m_clock->setStyleSheet(QStringLiteral("color:#e6e8eb;background:#0c0d0f;border:1px solid #3c3e43;padding:1px 6px;"));
    row->addWidget(m_clock);
    m_engine = new QLabel(bar);
    m_engine->setStyleSheet(QStringLiteral("color:#8c9199;font-size:10px;font-weight:bold;"));
    row->addWidget(m_engine);
    m_deckMode = barButton(QStringLiteral("2 DECKS"), bar, 66);
    m_deckMode->setToolTip(QStringLiteral("Switch between two and four decks"));
    m_deckMode->clicked = [this]() { m_model->setFourDecks(!m_model->fourDecks()); };
    row->addWidget(m_deckMode);
    row->addStretch();
    m_automix = barButton(QStringLiteral("AUTOMIX"), bar, 70);
    m_automix->setToolTip(QStringLiteral("AUTOMIX on or off (right-click: what it plays)"));
    m_automix->clicked = [this]() { m_model->toggleAutomix(); };
    m_automix->rightClicked = [this]() { focusMenu(QCursor::pos()); };
    m_autoDj = barButton(QStringLiteral("AUTO DJ"), bar, 66);
    m_autoDj->setToolTip(QStringLiteral("AUTO DJ: related songs people liked follow each song"));
    m_autoDj->clicked = [this]() { m_model->toggleAutoDj(); };
    auto *mix = barButton(QStringLiteral("MIX NOW"), bar, 66);
    mix->clicked = [this]() { m_model->mixNow(); };
    row->addWidget(m_automix);
    row->addWidget(m_autoDj);
    row->addWidget(mix);
    row->addStretch();
    m_people = new DjPeopleStrip(m_model, bar);
    m_people->clicked = [this]() { openJam(); };
    row->addWidget(m_people);
    auto *react = barButton(QStringLiteral("☺ REACT"), bar, 62);
    react->setToolTip(QStringLiteral("Send a reaction to everyone in the jam"));
    react->clicked = [this, react]() { reactionMenu(react->mapToGlobal(QPoint(0, react->height()))); };
    row->addWidget(react);
    auto *settings = barButton(QStringLiteral("⚙"), bar, 26);
    settings->setToolTip(QStringLiteral("Studio menu"));
    settings->clicked = [this, settings]() { settingsMenu(settings->mapToGlobal(QPoint(0, settings->height()))); };
    row->addWidget(settings);
    return bar;
}

void DjStudioFeature::layoutDecks()
{
    const int count = m_model->deckCount();
    while (QLayoutItem *item = m_deckGrid->takeAt(0))
        delete item;
    for (int i = 0; i < 4; ++i) {
        DjDeckView *view = m_decks.at(i);
        view->setVisible(i < count);
        view->setCompact(count == 4);
    }
    if (count == 4) {
        m_deckGrid->addWidget(m_decks.at(0), 0, 0);
        m_deckGrid->addWidget(m_decks.at(2), 1, 0);
        m_deckGrid->addWidget(m_mixer, 0, 1, 2, 1);
        m_deckGrid->addWidget(m_decks.at(1), 0, 2);
        m_deckGrid->addWidget(m_decks.at(3), 1, 2);
    } else {
        m_deckGrid->addWidget(m_decks.at(0), 0, 0);
        m_deckGrid->addWidget(m_mixer, 0, 1);
        m_deckGrid->addWidget(m_decks.at(1), 0, 2);
    }
    m_deckGrid->setColumnStretch(0, 1);
    m_deckGrid->setColumnStretch(1, 0);
    m_deckGrid->setColumnStretch(2, 1);
    m_mixer->setFixedWidth(count == 4 ? 300 : 250);
    for (DjScrollWave *wave : m_scrolls)
        wave->deleteLater();
    m_scrolls.clear();
    const QList<int> order = count == 4 ? QList<int>{2, 0, 1, 3} : QList<int>{0, 1};
    for (int deck : order) {
        auto *wave = new DjScrollWave(m_model, deck, m_waves);
        wave->setFixedHeight(count == 4 ? 24 : 42);
        m_wavesLayout->addWidget(wave);
        m_scrolls.append(wave);
    }
    m_deckMode->setLabel(count == 4 ? QStringLiteral("4 DECKS") : QStringLiteral("2 DECKS"));
    m_deckMode->setLit(count == 4);
    m_mixer->rebuildStrips();
    m_mixer->refresh();
    for (DjDeckView *view : m_decks)
        view->refresh();
    fitDecks();
}

void DjStudioFeature::fitDecks()
{
    const QList<int> sizes = m_split->sizes();
    const int total = sizes.value(0) + sizes.value(1);
    if (total <= 0)
        return;
    const int wanted = std::min(int(total * 0.72), m_deckArea->minimumSizeHint().height() + 12);
    if (sizes.value(0) < wanted)
        m_split->setSizes({wanted, total - wanted});
}

void DjStudioFeature::refresh()
{
    m_browser->refreshSources();
    m_browser->refreshList();
    m_mixer->refresh();
    for (DjDeckView *view : m_decks)
        view->refresh();
}

void DjStudioFeature::tick()
{
    for (DjScrollWave *wave : m_scrolls)
        wave->update();
    for (int i = 0; i < m_model->deckCount(); ++i)
        m_decks.at(i)->animate();
    m_mixer->animate();
    if (!m_model->effects().isEmpty())
        m_overlay->update();
}

void DjStudioFeature::slowTick()
{
    m_clock->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    if (!DjPlayer::available())
        m_engine->setText(QStringLiteral("● NO ENGINE"));
    else
        m_engine->setText(m_player->ready() ? QStringLiteral("<span style='color:#3dd65c'>●</span> ENGINE")
                                            : QStringLiteral("<span style='color:#ffc41c'>●</span> STARTING"));
    bool moving = false;
    for (int i = 0; i < 4; ++i)
        moving = moving || m_model->deck(i).playing || m_model->deck(i).scratching || m_model->deck(i).spinning;
    const bool wanted = isVisible() && (moving || !m_model->effects().isEmpty() || m_model->automixing());
    if (wanted && !m_frame->isActive())
        m_frame->start();
    else if (!wanted && m_frame->isActive()) {
        m_frame->stop();
        tick();
    }
    QStringList state;
    for (int i = 0; i < 4; ++i)
        state << QStringLiteral("%1%2").arg(m_model->deck(i).playing).arg(m_model->deck(i).videoId);
    const QString joined = state.join(QLatin1Char('|'));
    if (joined != m_railState) {
        m_railState = joined;
        railSoon();
    }
}

void DjStudioFeature::railSoon()
{
    if (!m_railTimer->isActive())
        m_railTimer->start();
}

void DjStudioFeature::notice(const QString &text)
{
    m_notice->setText(text);
    const int width = std::min(560, std::max(240, this->width() - 80));
    m_notice->setFixedWidth(width);
    m_notice->adjustSize();
    m_notice->move((this->width() - m_notice->width()) / 2, std::max(40, int(height() * 0.42)));
    m_notice->show();
    m_notice->raise();
    m_noticeTimer->start(2600);
}

void DjStudioFeature::showEvent(QShowEvent *)
{
    m_model->setOnScreen(true);
    m_model->applyMix();
    refresh();
    slowTick();
}

void DjStudioFeature::hideEvent(QHideEvent *)
{
    m_model->setOnScreen(false);
    m_model->save(true);
    m_frame->stop();
}

void DjStudioFeature::resizeEvent(QResizeEvent *)
{
    m_overlay->setGeometry(rect());
    m_overlay->raise();
    if (!m_fitted && m_split->height() > 0) {
        m_fitted = true;
        QTimer::singleShot(0, this, [this]() { fitDecks(); });
    }
    if (m_notice->isVisible())
        m_notice->move((width() - m_notice->width()) / 2, std::max(40, int(height() * 0.42)));
}

void DjStudioFeature::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme::background());
}

void DjStudioFeature::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    market::NativePackageWidget::mousePressEvent(event);
}

bool DjStudioFeature::shortcut(QKeyEvent *event, bool down)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        if (down && event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier)) {
            m_browser->focusSearch();
            return true;
        }
        return false;
    }
    const int key = event->key();
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    static const QString drums = QStringLiteral("ERTYUIOP");
    const QList<DjDrumPad> pads = drumPads();
    for (int i = 0; i < drums.size(); ++i) {
        if (key == Qt::Key_A + (drums.at(i).unicode() - 'A')) {
            if (down && !event->isAutoRepeat())
                m_model->playPad(drumPadKey(pads.at(i)));
            return true;
        }
    }
    if (event->isAutoRepeat() && key != Qt::Key_Left && key != Qt::Key_Right)
        return true;
    switch (key) {
    case Qt::Key_Z:
        if (down)
            m_model->cuePress(0);
        return true;
    case Qt::Key_X:
        if (down)
            m_model->togglePlay(0);
        return true;
    case Qt::Key_C:
        if (down)
            m_model->sync(0);
        return true;
    case Qt::Key_V:
        if (down)
            m_model->tapTempo(0);
        return true;
    case Qt::Key_M:
        if (down)
            m_model->cuePress(1);
        return true;
    case Qt::Key_Comma:
        if (down)
            m_model->togglePlay(1);
        return true;
    case Qt::Key_Period:
        if (down)
            m_model->sync(1);
        return true;
    case Qt::Key_Slash:
        if (down)
            m_model->tapTempo(1);
        return true;
    case Qt::Key_A:
        if (down)
            m_model->toggleAutomix();
        return true;
    case Qt::Key_D:
        if (down)
            m_model->toggleAutoDj();
        return true;
    case Qt::Key_N:
        if (down)
            m_model->mixNow();
        return true;
    case Qt::Key_G:
        if (down)
            m_model->toggleLoop(0);
        return true;
    case Qt::Key_H:
        if (down)
            m_model->toggleLoop(1);
        return true;
    case Qt::Key_Space:
        if (down)
            m_model->togglePlay(m_model->crossfader() <= 0 ? 0 : 1);
        return true;
    case Qt::Key_Left:
        if (down) {
            if (shift)
                m_model->slam(DjSide::A);
            else
                m_model->setCrossfader(m_model->crossfader() - 0.1);
        }
        return true;
    case Qt::Key_Right:
        if (down) {
            if (shift)
                m_model->slam(DjSide::B);
            else
                m_model->setCrossfader(m_model->crossfader() + 0.1);
        }
        return true;
    default:
        break;
    }
    if (key >= Qt::Key_1 && key <= Qt::Key_8) {
        if (down)
            m_model->hotCue(0, key - Qt::Key_1);
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F8) {
        if (down)
            m_model->hotCue(1, key - Qt::Key_F1);
        return true;
    }
    return false;
}

void DjStudioFeature::keyPressEvent(QKeyEvent *event)
{
    if (!shortcut(event, true))
        market::NativePackageWidget::keyPressEvent(event);
}

void DjStudioFeature::keyReleaseEvent(QKeyEvent *event)
{
    if (!shortcut(event, false))
        market::NativePackageWidget::keyReleaseEvent(event);
}

void DjStudioFeature::deckMenu(int deck, const QPoint &global)
{
    const DjDeck &state = m_model->deck(deck);
    QMenu menu(this);
    menu.addAction(QStringLiteral("Load a Song…"), this, [this]() { m_browser->focusSearch(); });
    menu.addAction(QStringLiteral("Paste a YouTube Link"), this, [this, deck]() {
        if (!m_model->loadLink(deck, QApplication::clipboard()->text()))
            notice(QStringLiteral("The clipboard has no YouTube link."));
    });
    menu.addAction(QStringLiteral("Load the Next Song"), this, [this, deck]() { m_model->quickNext(deck); });
    if (state.loaded()) {
        menu.addAction(QStringLiteral("Find Songs Like This"), this, [this, deck]() {
            m_model->findSimilar(m_model->deck(deck).videoId);
            m_browser->showSource(QStringLiteral("similar"));
        });
        menu.addAction(QStringLiteral("Tags and Playlists…"), this, [this, deck]() { openSong(m_model->deck(deck).videoId); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Detect the BPM Again"), this, [this, deck]() { m_model->detectBpm(deck); });
        menu.addAction(QStringLiteral("Clear the BPM"), this, [this, deck]() { m_model->clearBpm(deck); });
        menu.addAction(QStringLiteral("Clear the Hot Cues"), this, [this, deck]() {
            for (int pad = 0; pad < 8; ++pad)
                m_model->deleteHotCue(deck, pad);
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Open on YouTube"), this, [id = state.videoId]() { QDesktopServices::openUrl(studio::watchUrl(id)); });
        menu.addAction(QStringLiteral("Copy Link"), this, [id = state.videoId]() { QApplication::clipboard()->setText(studio::watchUrl(id).toString()); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Eject"), this, [this, deck]() { m_model->eject(deck); });
        menu.addAction(QStringLiteral("Delete from Library"), this, [this, deck]() { m_model->deleteSongOn(deck); });
    }
    menu.exec(global);
}

void DjStudioFeature::fileMenu(int deck, const QPoint &global)
{
    const DjDeck &state = m_model->deck(deck);
    if (!state.loaded())
        return;
    const QString id = state.videoId;
    const DjCrate &crate = m_model->crate();
    QMenu menu(this);
    menu.addSection(QStringLiteral("Playlists"));
    for (const DjPlaylist &playlist : crate.playlists) {
        QAction *action = menu.addAction(playlist.name, this, [this, deck, focus = DjFocus::playlist(playlist.id)]() { m_model->fileDeck(deck, focus); });
        action->setCheckable(true);
        action->setChecked(playlist.songs.contains(id));
    }
    menu.addAction(QStringLiteral("New Playlist…"), this, [this, deck]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New Playlist"), QStringLiteral("Name"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        if (const auto created = m_model->newPlaylist(name))
            m_model->fileDeck(deck, DjFocus::playlist(*created));
    });
    menu.addSection(QStringLiteral("Tags"));
    const auto track = crate.track(id);
    for (const DjTagCount &tag : crate.tagCounts()) {
        QAction *action = menu.addAction(QStringLiteral("#") + tag.name, this, [this, deck, focus = DjFocus::tag(tag.name)]() { m_model->fileDeck(deck, focus); });
        action->setCheckable(true);
        action->setChecked(track && track->tags.contains(tag.name));
    }
    menu.addAction(QStringLiteral("New Tag…"), this, [this, id]() {
        bool ok = false;
        const QString tags = QInputDialog::getText(this, QStringLiteral("New Tag"), QStringLiteral("Tags, separated by commas"), QLineEdit::Normal, QString(), &ok);
        if (ok)
            m_model->addTags(tags, id);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Tags and Playlists…"), this, [this, id]() { openSong(id); });
    QAction *shuffle = menu.addAction(QStringLiteral("Don't Shuffle"), this, [this, id]() { m_model->toggleShuffle(id); });
    shuffle->setCheckable(true);
    shuffle->setChecked(!crate.shuffles(id));
    if (crate.focus.isFiled()) {
        bool in = false;
        for (const DjTrack &t : crate.songs(crate.focus))
            in = in || t.id == id;
        if (in)
            menu.addAction(QStringLiteral("Take Out of %1").arg(crate.label(crate.focus)), this,
                           [this, deck, focus = crate.focus]() { m_model->fileDeck(deck, focus); });
    }
    menu.addAction(QStringLiteral("Delete from Library"), this, [this, deck]() { m_model->deleteSongOn(deck); });
    menu.exec(global);
}

void DjStudioFeature::focusMenu(const QPoint &global)
{
    const DjCrate &crate = m_model->crate();
    QMenu menu(this);
    menu.addSection(QStringLiteral("With the queue empty, AUTOMIX plays"));
    auto option = [&](QMenu *into, const QString &label, const DjFocus &focus) {
        QAction *action = into->addAction(label, this, [this, focus]() { m_model->focusAutomix(focus, false); });
        action->setCheckable(true);
        action->setChecked(crate.focus == focus);
    };
    option(&menu, QStringLiteral("Everything"), DjFocus::everything());
    if (!crate.playlists.isEmpty()) {
        QMenu *lists = menu.addMenu(QStringLiteral("A Playlist"));
        for (const DjPlaylist &playlist : crate.playlists)
            option(lists, playlist.name, DjFocus::playlist(playlist.id));
    }
    const auto tags = crate.tagCounts();
    if (!tags.isEmpty()) {
        QMenu *tagMenu = menu.addMenu(QStringLiteral("A Tag"));
        for (const DjTagCount &tag : tags)
            option(tagMenu, QStringLiteral("#") + tag.name, DjFocus::tag(tag.name));
    }
    if (!crate.smart.isEmpty()) {
        QMenu *smartMenu = menu.addMenu(QStringLiteral("A Smart Playlist"));
        for (DjSmartKind kind : smart::kinds()) {
            QMenu *group = nullptr;
            for (const DjSmartList &list : crate.smart) {
                if (list.kind != kind)
                    continue;
                if (!group)
                    group = smartMenu->addMenu(smart::kindTitle(kind));
                option(group, list.name, DjFocus::smart(list.id));
            }
        }
    }
    menu.addSeparator();
    QAction *shuffle = menu.addAction(QStringLiteral("Shuffle"), this, [this]() { m_model->setShuffle(!m_model->crate().shuffle); });
    shuffle->setCheckable(true);
    shuffle->setChecked(crate.shuffle);
    menu.addAction(QStringLiteral("Mix to the Other Deck Now"), this, [this]() { m_model->mixNow(); });
    menu.exec(global);
}

void DjStudioFeature::reactionMenu(const QPoint &global)
{
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral("QMenu{font-size:18px;}"));
    int count = 0;
    QMenu *current = &menu;
    for (const DjReaction &reaction : DjReaction::all()) {
        if (count == 15)
            current = menu.addMenu(QStringLiteral("More…"));
        current->addAction(QStringLiteral("%1  %2").arg(reaction.emoji, reaction.name), this, [this, reaction]() { m_model->react(reaction); });
        ++count;
    }
    menu.exec(global);
}

void DjStudioFeature::settingsMenu(const QPoint &global)
{
    QMenu menu(this);
    menu.addAction(m_model->fourDecks() ? QStringLiteral("Use Two Decks") : QStringLiteral("Use Four Decks"), this,
                   [this]() { m_model->setFourDecks(!m_model->fourDecks()); });
    menu.addAction(QStringLiteral("Jam Session…"), this, [this]() { openJam(); });
    menu.addAction(QStringLiteral("Stop the Music"), this, [this]() { m_model->stopAll(); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Import Playlists…"), this, [this]() { importFile(); });
    menu.addAction(QStringLiteral("Paste Songs from the Clipboard"), this, [this]() { pasteSongs(); });
    menu.addAction(QStringLiteral("Export Everything…"), this, [this]() { exportList(DjFocus::everything()); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Keyboard Shortcuts"), this, [this]() { openHelp(); });
    menu.addAction(QStringLiteral("Studio Diagnostics…"), this, [this]() { openDiagnostics(); });
    menu.addAction(QStringLiteral("Open the Web Studio"), this, [this]() {
        if (m_host.openWebPage)
            m_host.openWebPage(webPath);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Reset All…"), this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("Reset All"),
                                  QStringLiteral("Stop and unload every deck and put cues, loops, tempo, BPM, EQ, levels, the crossfader and "
                                                 "samples back to their defaults? Recently Played, playlists and tags are kept.")) == QMessageBox::Yes)
            m_model->resetAll();
    });
    menu.exec(global);
}

void DjStudioFeature::openJam()
{
    if (!m_jamDialog) {
        m_jamDialog = new DjJamDialog(m_model, m_host.userDirectory, this);
        m_jamDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_model->refreshJams();
    m_jamDialog->refresh();
    m_jamDialog->show();
    m_jamDialog->raise();
}

void DjStudioFeature::openSong(const QString &id)
{
    if (!studio::isVideoId(id))
        return;
    auto *dialog = new DjSongDialog(m_model, id, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void DjStudioFeature::openDiagnostics()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Studio Diagnostics"));
    dialog->resize(720, 420);
    auto *column = new QVBoxLayout(dialog);
    auto *text = new QPlainTextEdit(dialog);
    text->setReadOnly(true);
    text->setFont(theme::mono(11));
    text->setPlainText(m_model->diagnostics());
    column->addWidget(text, 1);
    auto *buttons = new QHBoxLayout;
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), dialog);
    auto *copy = new QPushButton(QStringLiteral("Copy"), dialog);
    auto *close = new QPushButton(QStringLiteral("Close"), dialog);
    buttons->addWidget(refresh);
    buttons->addWidget(copy);
    buttons->addStretch();
    buttons->addWidget(close);
    column->addLayout(buttons);
    connect(refresh, &QPushButton::clicked, dialog, [this, text]() { text->setPlainText(m_model->diagnostics()); });
    connect(copy, &QPushButton::clicked, dialog, [text]() { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->show();
}

void DjStudioFeature::openHelp()
{
    QMessageBox::information(
        this, QStringLiteral("Keyboard Shortcuts"),
        QStringLiteral("Deck 1: Z cue, X play or pause, C sync, V tap tempo, G loop, 1–8 hot cues\n"
                       "Deck 2: M cue, comma play or pause, period sync, slash tap tempo, H loop, F1–F8 hot cues\n"
                       "Space: play or pause the deck the crossfader favours\n"
                       "Left and Right: move the crossfader (Shift slams it)\n"
                       "A: AUTOMIX, D: AUTO DJ, N: mix into the other deck now\n"
                       "E R T Y U I O P: drum pads\n"
                       "Ctrl+F: search\n\n"
                       "Mouse: drag a record to scratch, Shift-drag or scroll on it to nudge the tempo; drag songs from the "
                       "browser onto a deck; double-click a knob or fader to reset it; right-click an EQ knob to kill the band."));
}

void DjStudioFeature::exportList(const DjFocus &focus)
{
    const DjCrate &crate = m_model->crate();
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(crate.fileName(focus));
    QString selected;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Playlists"), suggested,
                                                      QStringLiteral("ForkMesh playlists (*.json);;M3U playlist (*.m3u)"), &selected);
    if (path.isEmpty())
        return;
    const bool m3u = path.endsWith(QLatin1String(".m3u"), Qt::CaseInsensitive) || selected.contains(QLatin1String("m3u"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        notice(QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()));
        return;
    }
    file.write(m3u ? crate.m3u(focus) : crate.exported(focus));
    if (file.commit())
        notice(QStringLiteral("Exported to %1.").arg(QFileInfo(path).fileName()));
}

void DjStudioFeature::importFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Playlists"), QString(),
                                                      QStringLiteral("Playlists (*.json *.m3u *.m3u8 *.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (QFileInfo(path).size() > crate::importLimit || !file.open(QIODevice::ReadOnly)) {
        notice(QStringLiteral("That file is too large or cannot be read."));
        return;
    }
    m_model->importData(file.readAll(), QFileInfo(path).completeBaseName());
}

void DjStudioFeature::pasteSongs()
{
    const QString text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        notice(QStringLiteral("The clipboard is empty."));
        return;
    }
    m_model->importData(text.toUtf8(), QStringLiteral("Pasted Songs"));
}

void DjStudioFeature::setHostSearchQuery(const QString &query)
{
    m_query = query;
    m_browser->setQuery(query);
}

QList<market::NativePackageSearchHit> DjStudioFeature::searchHits(const QString &query, int limit) const
{
    QList<market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed().toLower();
    if (needle.size() < 2)
        return hits;
    for (const DjTrack &track : m_model->librarySongs()) {
        if (hits.size() >= limit)
            break;
        if (!(track.title + QLatin1Char(' ') + track.author).toLower().contains(needle))
            continue;
        hits.append({track.id, track.shownTitle(),
                     track.author.isEmpty() ? QStringLiteral("Video Mix DJ song") : track.author + QStringLiteral(" · Video Mix DJ")});
    }
    return hits;
}

void DjStudioFeature::activateSearchHit(const QString &id)
{
    if (!studio::isVideoId(id))
        return;
    const int deck = m_browser->bestDeck();
    const DjTrack track = m_model->song(id);
    if (deck >= 0)
        m_model->load(deck, id, track.title, track.author);
    else
        m_model->enqueue(id, track.title, track.author);
}

int DjStudioFeature::navigationBadge() const { return m_model ? m_model->pendingInvites() : 0; }

QString DjStudioFeature::railTitle() const { return QStringLiteral("Video Mix DJ"); }

QList<market::NativePackageRailTile> DjStudioFeature::railTiles() const
{
    QList<market::NativePackageRailTile> tiles;
    if (!m_model)
        return tiles;
    QStringList playing;
    QStringList loaded;
    for (int i = 0; i < 4; ++i) {
        const DjDeck &deck = m_model->deck(i);
        if (!deck.loaded())
            continue;
        const QString line = QStringLiteral("Deck %1: %2").arg(i + 1).arg(deck.title.isEmpty() ? QStringLiteral("YouTube video") : deck.title);
        loaded << line;
        if (deck.playing)
            playing << line;
    }
    if (loaded.isEmpty())
        return tiles;
    market::NativePackageRailTile tile;
    tile.id = QStringLiteral("now");
    tile.label = playing.isEmpty() ? QStringLiteral("Decks paused") : QStringLiteral("Now playing");
    tile.count = QString::number(playing.size());
    tile.countLabel = playing.size() == 1 ? QStringLiteral("deck playing") : QStringLiteral("decks playing");
    tile.tone = playing.isEmpty() ? market::NativePackageRailTone::Paused : market::NativePackageRailTone::Good;
    tile.stateLabel = playing.isEmpty() ? QStringLiteral("Paused") : QStringLiteral("Playing");
    tile.details = playing.isEmpty() ? loaded : playing;
    if (!playing.isEmpty())
        tile.steps.append({QStringLiteral("stop"), QStringLiteral("Stop the music")});
    tile.steps.append({QStringLiteral("next"), QStringLiteral("Mix into the next song")});
    tile.opensPage = true;
    tiles.append(tile);
    return tiles;
}

void DjStudioFeature::activateRailStep(const QString &, const QString &step)
{
    if (step == QLatin1String("stop"))
        m_model->stopAll();
    else if (step == QLatin1String("next"))
        m_model->mixNow();
}

}

namespace {

const forkmesh::market::NativePackageRegistration videoMixDjStudioRegistration({
    QStringLiteral("builtin:video-mix-dj-studio"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Video Mix DJ Studio"),
    QStringLiteral("play"),
    QStringLiteral("/api/video-mix-dj-studio"),
    QStringLiteral("forkmesh-video-mix-dj-studio-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::djstudio::DjStudioFeature(host, parent);
    },
    QString(),
});

}
