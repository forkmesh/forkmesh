#include "DjDeckView.h"
#include "DjModel.h"
#include "DjWidgets.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QVBoxLayout>

#include <cmath>

namespace forkmesh::djstudio {

namespace {

const char songMime[] = "application/x-forkmesh-dj-song";

QLabel *label(const QString &text, int pixels, const QColor &color, bool bold = false, QWidget *parent = nullptr)
{
    auto *l = new QLabel(text, parent);
    l->setFont(theme::font(pixels, bold));
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, color);
    l->setPalette(p);
    return l;
}

void setColor(QLabel *l, const QColor &color)
{
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, color);
    l->setPalette(p);
}

DjButton *small(const QString &text, QWidget *parent, int width = 0)
{
    auto *b = new DjButton(text, parent);
    b->setFontSize(9);
    b->setFixedHeight(20);
    if (width)
        b->setFixedWidth(width);
    return b;
}

QString loopName(double beats)
{
    if (beats >= 1)
        return QString::number(int(beats));
    return QStringLiteral("1/%1").arg(int(std::round(1 / beats)));
}

QString precise(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    const int whole = int(seconds);
    return QStringLiteral("%1:%2.%3").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0')).arg(int((seconds - whole) * 10));
}

}

DjDeckView::DjDeckView(DjModel *model, int deck, bool mirrored, QWidget *parent)
    : QWidget(parent), m_model(model), m_deck(deck), m_mirrored(mirrored)
{
    setAcceptDrops(true);
    setAutoFillBackground(false);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(6);
    QWidget *info = buildInfo();
    QWidget *platter = buildPlatter();
    QWidget *pitch = buildPitch();
    if (mirrored) {
        row->addWidget(pitch);
        row->addWidget(platter, 1);
        row->addWidget(info, 2);
    } else {
        row->addWidget(info, 2);
        row->addWidget(platter, 1);
        row->addWidget(pitch);
    }
    refresh();
}

QWidget *DjDeckView::buildInfo()
{
    const QColor color = theme::deck(m_deck);
    auto *box = new QWidget(this);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);

    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    m_art = new QLabel(box);
    m_art->setFixedSize(40, 40);
    m_art->setStyleSheet(QStringLiteral("background:#0c0d0f;border:1px solid #3c3e43;"));
    m_art->setCursor(Qt::PointingHandCursor);
    m_art->setToolTip(QStringLiteral("Load a song on this deck"));
    auto *names = new QVBoxLayout;
    names->setSpacing(0);
    m_title = label(QString(), 13, theme::text(), true, box);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_title->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_title, &QLabel::customContextMenuRequested, this, [this](const QPoint &point) {
        if (menuRequested)
            menuRequested(m_deck, m_title->mapToGlobal(point));
    });
    m_artist = label(QString(), 11, color, true, box);
    m_artist->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    names->addWidget(m_title);
    names->addWidget(m_artist);
    auto *bpmBox = new QVBoxLayout;
    bpmBox->setSpacing(1);
    m_bpm = label(QStringLiteral("---"), 20, color, true, box);
    m_bpm->setFont(theme::mono(20, true));
    m_bpm->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_bpm->setMinimumWidth(78);
    m_bpm->setToolTip(QStringLiteral("BPM, found from the audio as the song plays (or tapped). Right-click to detect again or clear."));
    m_bpm->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_bpm, &QLabel::customContextMenuRequested, this, [this](const QPoint &point) {
        if (menuRequested)
            menuRequested(m_deck, m_bpm->mapToGlobal(point));
    });
    auto *bpmRow = new QHBoxLayout;
    bpmRow->setSpacing(3);
    auto *bpmCaption = label(QStringLiteral("BPM"), 9, theme::dim(), true, box);
    m_tap = small(QStringLiteral("TAP"), box, 34);
    m_tap->setToolTip(QStringLiteral("Tap on the beat to set the BPM"));
    m_tap->clicked = [this]() { m_model->tapTempo(m_deck); };
    bpmRow->addStretch();
    bpmRow->addWidget(bpmCaption);
    bpmRow->addWidget(m_tap);
    bpmBox->addWidget(m_bpm);
    bpmBox->addLayout(bpmRow);
    head->addWidget(m_art);
    head->addLayout(names, 1);
    head->addLayout(bpmBox);
    column->addLayout(head);

    auto *times = new QHBoxLayout;
    times->setSpacing(6);
    auto caption = [&](const QString &text) { times->addWidget(label(text, 9, theme::dim(), true, box)); };
    caption(QStringLiteral("ELAPSED"));
    m_elapsed = label(QStringLiteral("0:00.0"), 12, color, true, box);
    m_elapsed->setFont(theme::mono(12, true));
    times->addWidget(m_elapsed);
    caption(QStringLiteral("REMAIN"));
    m_remain = label(QStringLiteral("0:00.0"), 12, color, true, box);
    m_remain->setFont(theme::mono(12, true));
    m_remain->setCursor(Qt::PointingHandCursor);
    times->addWidget(m_remain);
    times->addStretch();
    m_gain = label(QStringLiteral("0.0 dB"), 10, color, true, box);
    m_gain->setToolTip(QStringLiteral("Deck gain (the GAIN knob on the mixer)"));
    times->addWidget(m_gain);
    m_key = label(QString(), 9, theme::dim(), true, box);
    m_key->hide();
    column->addLayout(times);

    m_overview = new DjOverview(m_model, m_deck, box);
    m_overview->setFixedHeight(22);
    column->addWidget(m_overview);

    m_fxRow = new QWidget(box);
    auto *fx = new QHBoxLayout(m_fxRow);
    fx->setContentsMargins(0, 0, 0, 0);
    fx->setSpacing(3);
    auto *loopCaption = label(QStringLiteral("LOOP"), 9, theme::dim(), true, m_fxRow);
    fx->addWidget(loopCaption);
    auto *smaller = small(QStringLiteral("◀"), m_fxRow, 20);
    smaller->setToolTip(QStringLiteral("Halve the loop size"));
    smaller->clicked = [this]() { m_model->setLoopSize(m_deck, m_model->deck(m_deck).loopSize - 1); };
    m_loopLabel = label(QStringLiteral("16"), 11, color, true, m_fxRow);
    m_loopLabel->setAlignment(Qt::AlignCenter);
    m_loopLabel->setFixedWidth(28);
    m_loopLabel->setStyleSheet(QStringLiteral("background:#0c0d0f;border:1px solid #3c3e43;border-radius:3px;"));
    auto *bigger = small(QStringLiteral("▶"), m_fxRow, 20);
    bigger->setToolTip(QStringLiteral("Double the loop size"));
    bigger->clicked = [this]() { m_model->setLoopSize(m_deck, m_model->deck(m_deck).loopSize + 1); };
    m_loopIn = small(QStringLiteral("IN"), m_fxRow, 26);
    m_loopIn->clicked = [this]() { m_model->loopIn(m_deck); };
    m_loopOut = small(QStringLiteral("OUT"), m_fxRow, 30);
    m_loopOut->clicked = [this]() { m_model->loopOut(m_deck); };
    m_loop = small(QStringLiteral("LOOP"), m_fxRow, 38);
    m_loop->setColor(QColor(0x3d, 0xd6, 0x5c));
    m_loop->setToolTip(QStringLiteral("Loop the chosen number of beats from here (8 seconds without a BPM)"));
    m_loop->clicked = [this]() { m_model->toggleLoop(m_deck); };
    fx->addWidget(smaller);
    fx->addWidget(m_loopLabel);
    fx->addWidget(bigger);
    fx->addWidget(m_loopIn);
    fx->addWidget(m_loopOut);
    fx->addWidget(m_loop);
    fx->addStretch();
    column->addWidget(m_fxRow);
    auto *effects = new QWidget(box);
    auto *fxLine = new QHBoxLayout(effects);
    fxLine->setContentsMargins(0, 0, 0, 0);
    fxLine->setSpacing(3);
    fxLine->addWidget(label(QStringLiteral("FX"), 9, theme::dim(), true, effects));
    fx = fxLine;
    const QStringList names3{QStringLiteral("ECHO"), QStringLiteral("VERB"), QStringLiteral("FLANG")};
    for (int slot = 0; slot < 3; ++slot) {
        auto *button = small(names3.at(slot), effects, 38);
        button->setColor(color);
        button->setToolTip(QStringLiteral("Switch the %1 effect on or off; the knob next to it sets how much").arg(names3.at(slot).toLower()));
        button->clicked = [this, slot]() { m_model->toggleFx(m_deck, slot); };
        auto *knob = new DjKnob(QString(), false, effects);
        knob->setFixedSize(20, 20);
        knob->setColor(color);
        knob->setDefault(0.5);
        knob->changed = [this, slot](double value) { m_model->setFx(m_deck, slot, value); };
        m_fxButtons.append(button);
        m_fxKnobs.append(knob);
        fx->addWidget(button);
        fx->addWidget(knob);
    }
    fx->addStretch();
    column->addWidget(effects);

    m_padRow = new QWidget(box);
    auto *pads = new QHBoxLayout(m_padRow);
    pads->setContentsMargins(0, 0, 0, 0);
    pads->setSpacing(4);
    m_padModeButton = small(QStringLiteral("HOT CUE"), m_padRow, 62);
    m_padModeButton->setToolTip(QStringLiteral("Pad mode: hot cues, loop rolls, the sampler or beat jumps. Right-click a hot cue to clear it."));
    m_padModeButton->clicked = [this]() {
        m_padMode = (m_padMode + 1) % 4;
        refreshPads();
    };
    auto *grid = new QGridLayout;
    grid->setSpacing(3);
    for (int pad = 0; pad < 8; ++pad) {
        auto *button = new DjButton(QString::number(pad + 1), m_padRow);
        button->setFontSize(10);
        button->setMinimumHeight(20);
        button->setColor(color);
        button->setFilled(true);
        button->pressed = [this, pad]() { padPressed(pad, true); };
        button->released = [this, pad]() { padPressed(pad, false); };
        button->rightClicked = [this, pad]() {
            if (m_padMode == 0)
                m_model->deleteHotCue(m_deck, pad);
            else if (m_padMode == 2 && pad < 4)
                m_model->clearSection(pad);
        };
        m_pads.append(button);
        grid->addWidget(button, pad / 4, pad % 4);
    }
    pads->addWidget(m_padModeButton, 0, Qt::AlignTop);
    pads->addLayout(grid, 1);
    column->addWidget(m_padRow);
    column->addStretch();
    refreshPads();
    return box;
}

QWidget *DjDeckView::buildPlatter()
{
    const QColor color = theme::deck(m_deck);
    auto *box = new QWidget(this);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    auto *top = new QHBoxLayout;
    top->setSpacing(3);
    m_assign = small(m_mirrored ? QStringLiteral("RIGHT") : QStringLiteral("LEFT"), box, 44);
    m_assign->setColor(color);
    m_assign->setToolTip(QStringLiteral("Which side of the crossfader this deck is on (left, right or through)"));
    m_assign->clicked = [this]() { m_model->setAssign(m_deck, (m_model->deck(m_deck).assign + 1) % 3); };
    auto *number = new DjButton(QString::number(m_deck + 1), box);
    number->setFixedSize(22, 20);
    number->setColor(color);
    number->setLit(true);
    number->setFilled(true);
    number->clicked = [this]() {
        if (loadRequested)
            loadRequested(m_deck);
    };
    number->setToolTip(QStringLiteral("Deck %1: load a song").arg(m_deck + 1));
    m_keylock = small(QStringLiteral("KEY"), box, 32);
    m_keylock->setToolTip(QStringLiteral("Key lock: keep the pitch when the tempo changes"));
    m_keylock->clicked = [this]() { m_model->toggleKeylock(m_deck); };
    m_repeat = small(QStringLiteral("⟳1"), box, 28);
    m_repeat->setToolTip(QStringLiteral("Repeat this song"));
    m_repeat->clicked = [this]() { m_model->toggleRepeat(m_deck); };
    if (m_mirrored) {
        top->addWidget(m_keylock);
        top->addWidget(m_repeat);
        top->addStretch();
        top->addWidget(number);
        top->addWidget(m_assign);
    } else {
        top->addWidget(m_assign);
        top->addWidget(number);
        top->addStretch();
        top->addWidget(m_repeat);
        top->addWidget(m_keylock);
    }
    column->addLayout(top);
    m_platter = new DjPlatter(m_model, m_deck, box);
    column->addWidget(m_platter, 1);
    auto *tools = new QHBoxLayout;
    tools->setSpacing(3);
    auto *backspin = small(QStringLiteral("SPIN"), box);
    backspin->setToolTip(QStringLiteral("Backspin: spin the record backwards to a stop"));
    backspin->clicked = [this]() { m_model->backspin(m_deck); };
    auto *brake = small(QStringLiteral("BRAKE"), box);
    brake->setToolTip(QStringLiteral("Brake: slow the record to a stop like a turntable switched off"));
    brake->clicked = [this]() { m_model->brakeDeck(m_deck); };
    m_sample = small(QStringLiteral("SAMPLE"), box);
    m_sample->setToolTip(QStringLiteral("First press records a sample, the second arms it, the third plays it"));
    m_sample->clicked = [this]() { m_model->sample(m_deck); };
    auto *next = small(QStringLiteral("NEXT"), box);
    next->setToolTip(QStringLiteral("Load the next song: the queue first, then what AUTOMIX would play"));
    next->clicked = [this]() { m_model->quickNext(m_deck); };
    auto *similar = small(QStringLiteral("✦"), box, 22);
    similar->setToolTip(QStringLiteral("Find songs like this one"));
    similar->clicked = [this]() {
        if (similarRequested)
            similarRequested(m_deck);
    };
    auto *file = small(QStringLiteral("☰"), box, 22);
    file->setToolTip(QStringLiteral("File this song in a playlist or tag"));
    file->clicked = [this, file]() {
        if (fileRequested)
            fileRequested(m_deck, file->mapToGlobal(QPoint(0, file->height())));
    };
    m_backspin = backspin;
    m_brake = brake;
    tools->addWidget(m_sample);
    tools->addWidget(next);
    tools->addWidget(similar);
    tools->addWidget(file);
    column->addLayout(tools);
    auto *transport = new QHBoxLayout;
    transport->setSpacing(3);
    m_cue = new DjButton(QStringLiteral("CUE"), box);
    m_cue->setFixedHeight(26);
    m_cue->setColor(QColor(0xff, 0xd0, 0x30));
    m_cue->setToolTip(QStringLiteral("Stopped: set the cue here (or go back to it). Playing: stop and return to the cue."));
    m_cue->clicked = [this]() { m_model->cuePress(m_deck); };
    m_play = new DjButton(QStringLiteral("▶ ❚❚"), box);
    m_play->setFixedHeight(26);
    m_play->setColor(QColor(0x3d, 0xd6, 0x5c));
    m_play->clicked = [this]() { m_model->togglePlay(m_deck); };
    m_stop = new DjButton(QStringLiteral("■"), box);
    m_stop->setFixedHeight(26);
    m_stop->setToolTip(QStringLiteral("Stop and go back to the beginning"));
    m_stop->clicked = [this]() { m_model->stop(m_deck); };
    m_restart = new DjButton(QStringLiteral("⏮"), box);
    m_restart->setFixedHeight(26);
    m_restart->setToolTip(QStringLiteral("Start the song over"));
    m_restart->clicked = [this]() { m_model->restart(m_deck); };
    m_sync = new DjButton(QStringLiteral("SYNC"), box);
    m_sync->setFixedHeight(26);
    m_sync->setFontSize(9);
    m_sync->setColor(color);
    m_sync->setToolTip(QStringLiteral("Match this deck's tempo to the other playing deck"));
    m_sync->clicked = [this]() { m_model->sync(m_deck); };
    transport->addWidget(m_cue);
    transport->addWidget(m_play);
    transport->addWidget(m_stop);
    transport->addWidget(m_restart);
    transport->addWidget(m_sync);
    column->addLayout(transport);
    return box;
}

QWidget *DjDeckView::buildPitch()
{
    const QColor color = theme::deck(m_deck);
    auto *box = new QWidget(this);
    box->setFixedWidth(46);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    m_pitchLabel = label(QStringLiteral("+0.0"), 11, theme::text(), true, box);
    m_pitchLabel->setFont(theme::mono(11, true));
    m_pitchLabel->setAlignment(Qt::AlignCenter);
    m_pitchLabel->setStyleSheet(QStringLiteral("background:#0c0d0f;border:1px solid #3c3e43;border-radius:3px;"));
    m_range = small(QStringLiteral("±16"), box);
    m_range->setToolTip(QStringLiteral("Pitch fader range"));
    m_range->clicked = [this]() {
        const double range = m_model->deck(m_deck).pitchRange;
        m_model->setPitchRange(m_deck, range < 0.1 ? 0.16 : range < 0.3 ? 0.5 : 0.08);
    };
    m_pitch = new DjFader(Qt::Vertical, true, box);
    m_pitch->setColor(color);
    m_pitch->setTicks(16);
    m_pitch->setToolTip(QStringLiteral("Tempo: up is faster. Double-click to reset."));
    m_pitch->changed = [this](double value) {
        const DjDeck &deck = m_model->deck(m_deck);
        m_model->setRate(m_deck, 1 + value * deck.pitchRange);
    };
    m_reset = small(QStringLiteral("0"), box);
    m_reset->setToolTip(QStringLiteral("Put the tempo back to the middle"));
    m_reset->clicked = [this]() { m_model->resetPitch(m_deck); };
    auto *bends = new QHBoxLayout;
    bends->setSpacing(2);
    auto *slower = small(QStringLiteral("−"), box);
    slower->setToolTip(QStringLiteral("Hold to slow down a little"));
    slower->pressed = [this]() { m_model->bend(m_deck, -1); };
    slower->released = [this]() { m_model->bend(m_deck, 0); };
    auto *faster = small(QStringLiteral("+"), box);
    faster->setToolTip(QStringLiteral("Hold to speed up a little"));
    faster->pressed = [this]() { m_model->bend(m_deck, 1); };
    faster->released = [this]() { m_model->bend(m_deck, 0); };
    bends->addWidget(slower);
    bends->addWidget(faster);
    column->addWidget(m_pitchLabel);
    column->addWidget(m_range);
    column->addWidget(m_pitch, 1);
    column->addWidget(m_reset);
    if (m_backspin && m_brake) {
        m_backspin->setParent(box);
        m_brake->setParent(box);
        m_brake->setLabel(QStringLiteral("BRK"));
        column->addWidget(m_brake);
        column->addWidget(m_backspin);
    }
    column->addLayout(bends);
    return box;
}

void DjDeckView::setCompact(bool compact)
{
    m_compact = compact;
    m_reset->setVisible(!compact);
    refresh();
}

void DjDeckView::refreshPads()
{
    static const QStringList modes{QStringLiteral("HOT CUE"), QStringLiteral("ROLL"), QStringLiteral("SAMPLER"), QStringLiteral("JUMP")};
    m_padModeButton->setLabel(modes.at(m_padMode));
    const DjDeck &deck = m_model->deck(m_deck);
    const auto &sections = m_model->sections();
    const auto &effects = soundEffects();
    static const QStringList jumps{QStringLiteral("-8"), QStringLiteral("-4"), QStringLiteral("-2"), QStringLiteral("-1"),
                                   QStringLiteral("+1"), QStringLiteral("+2"), QStringLiteral("+4"), QStringLiteral("+8")};
    for (int pad = 0; pad < m_pads.size(); ++pad) {
        DjButton *button = m_pads.at(pad);
        switch (m_padMode) {
        case 0:
            button->setLabel(QString::number(pad + 1));
            button->setLit(deck.hotcues[pad] >= 0);
            button->setToolTip(deck.hotcues[pad] >= 0 ? QStringLiteral("Hot cue %1 at %2 (right-click to clear)").arg(pad + 1).arg(studio::clock(deck.hotcues[pad]))
                                                       : QStringLiteral("Set hot cue %1 here").arg(pad + 1));
            break;
        case 1: {
            const double beats = DjModel::loopBeats().value(pad);
            button->setLabel(loopName(beats));
            button->setLit(false);
            button->setToolTip(QStringLiteral("Hold to roll a %1-beat loop; the song carries on underneath").arg(loopName(beats)));
            break;
        }
        case 2:
            if (pad < 4) {
                const bool has = pad < sections.size() && sections.at(pad).has_value();
                button->setLabel(QStringLiteral("S%1").arg(pad + 1));
                button->setLit(has);
                button->setToolTip(has ? QStringLiteral("Sample %1 from deck %2 (right-click to clear)").arg(pad + 1).arg(sections.at(pad)->deck + 1)
                                       : QStringLiteral("Empty sample slot: use SAMPLE to record one"));
            } else {
                const DjSoundEffect &effect = effects.at(pad - 4);
                button->setLabel(effect.name.section(QLatin1Char(' '), 0, 0).left(7).toUpper());
                button->setLit(true);
                button->setToolTip(effect.name);
            }
            break;
        default:
            button->setLabel(jumps.at(pad));
            button->setLit(false);
            button->setToolTip(QStringLiteral("Jump %1 beats").arg(jumps.at(pad)));
        }
    }
}

void DjDeckView::padPressed(int pad, bool down)
{
    switch (m_padMode) {
    case 0:
        if (down)
            m_model->hotCue(m_deck, pad);
        break;
    case 1:
        m_model->loopRoll(m_deck, pad, down);
        break;
    case 2:
        if (!down)
            break;
        if (pad < 4)
            m_model->playSection(pad);
        else
            m_model->playPad(soundEffects().at(pad - 4).id);
        break;
    default:
        if (down) {
            static const double beats[] = {-8, -4, -2, -1, 1, 2, 4, 8};
            m_model->beatJump(m_deck, beats[pad]);
        }
    }
    refreshPads();
}

void DjDeckView::refresh()
{
    DjDeck &deck = m_model->deck(m_deck);
    const QColor color = theme::deck(m_deck);
    const QString title = deck.loaded() ? (deck.title.isEmpty() ? QStringLiteral("Loading…") : deck.title)
                                        : QStringLiteral("Empty: drop or double-click a song");
    m_title->setText(m_title->fontMetrics().elidedText(title, Qt::ElideRight, std::max(60, m_title->width())));
    m_title->setToolTip(deck.title);
    m_artist->setText(m_artist->fontMetrics().elidedText(deck.author, Qt::ElideRight, std::max(60, m_artist->width())));
    if (deck.videoId != m_artFor) {
        const QImage art = m_model->artwork(deck.videoId);
        if (!art.isNull() && art.width() > 1) {
            m_art->setPixmap(QPixmap::fromImage(art.scaled(40, 40, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
            m_artFor = deck.videoId;
        } else {
            m_art->setPixmap(QPixmap());
            if (!deck.loaded())
                m_artFor.clear();
        }
    }
    m_bpm->setText(deck.bpm > 0 ? QString::number(deck.shownBpm(), 'f', 2)
                                : deck.loaded() && std::abs(deck.effectiveRate() - 1) > 0.0005
                                    ? QStringLiteral("%1%").arg((deck.effectiveRate() - 1) * 100, 0, 'f', 1)
                                    : QStringLiteral("---"));
    const double pitch = (deck.rate - 1) * 100;
    m_pitchLabel->setText(QStringLiteral("%1%2").arg(pitch >= 0 ? QStringLiteral("+") : QString()).arg(pitch, 0, 'f', 1));
    m_pitch->setValue((deck.rate - 1) / deck.pitchRange);
    m_range->setLabel(QStringLiteral("±%1").arg(int(std::round(deck.pitchRange * 100))));
    m_reset->setEnabled(std::abs(deck.rate - 1) > 0.0001);
    m_gain->setText(QStringLiteral("%1%2 dB").arg(deck.gainDb >= 0 ? QStringLiteral("+") : QString()).arg(deck.gainDb, 0, 'f', 1));
    m_key->setText(deck.keylock ? QStringLiteral("KEY LOCK") : QString());
    m_loopLabel->setText(loopName(DjModel::loopBeats().value(deck.loopSize)));
    m_loop->setLit(deck.loop);
    m_repeat->setLit(deck.repeats);
    m_keylock->setLit(deck.keylock);
    m_play->setLit(deck.playing || deck.keepsPlaying);
    m_play->setBlink(deck.keepsPlaying && !deck.advancing && !deck.scratching);
    m_cue->setLit(deck.loaded() && !deck.playing && std::abs(deck.position(DjModel::now()) - deck.cue) < 0.05);
    m_stop->setLit(deck.loaded() && !deck.playing && deck.position(DjModel::now()) < 0.05);
    m_sample->setLabel(deck.sample.kind == DjSampleStepKind::Recording ? QStringLiteral("REC")
                       : deck.sample.kind == DjSampleStepKind::Armed  ? QStringLiteral("ARMED")
                                                                      : QStringLiteral("SAMPLE"));
    m_sample->setColor(deck.sample.kind == DjSampleStepKind::Recording ? QColor(0xff, 0x3d, 0x3d) : theme::accent());
    m_sample->setLit(deck.sample.kind != DjSampleStepKind::Idle);
    static const QStringList sides{QStringLiteral("LEFT"), QStringLiteral("RIGHT"), QStringLiteral("THRU")};
    m_assign->setLabel(sides.at(deck.assign));
    m_assign->setLit(deck.assign != 2);
    for (int slot = 0; slot < 3; ++slot) {
        m_fxButtons.at(slot)->setLit(deck.fxOn[slot]);
        m_fxKnobs.at(slot)->setValue(deck.fx[slot]);
    }
    setColor(m_elapsed, color);
    refreshPads();
    animate();
}

void DjDeckView::animate()
{
    DjDeck &deck = m_model->deck(m_deck);
    const double at = DjModel::now();
    const double position = deck.position(at);
    m_elapsed->setText(precise(position));
    m_remain->setText(deck.duration > 0 ? QStringLiteral("-") + precise(deck.duration - position) : QStringLiteral("-0:00.0"));
    const bool ending = deck.playing && deck.duration > 0 && deck.duration - position < 30;
    setColor(m_remain, ending ? QColor(0xff, 0x3d, 0x3d) : theme::deck(m_deck));
    m_platter->update();
    m_overview->update();
    if (deck.keepsPlaying && !deck.advancing)
        m_play->update();
}

void DjDeckView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refresh();
}

void DjDeckView::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasFormat(QString::fromLatin1(songMime)) || !studio::videoId(mime->text()).isEmpty() ||
        (mime->hasUrls() && !mime->urls().isEmpty() && !studio::videoId(mime->urls().first().toString()).isEmpty())) {
        m_dropping = true;
        update();
        event->acceptProposedAction();
    }
}

void DjDeckView::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropping = false;
    update();
}

void DjDeckView::dropEvent(QDropEvent *event)
{
    m_dropping = false;
    update();
    const QMimeData *mime = event->mimeData();
    if (mime->hasFormat(QString::fromLatin1(songMime))) {
        const QJsonObject song = QJsonDocument::fromJson(mime->data(QString::fromLatin1(songMime))).object();
        const QString queueId = song.value(QStringLiteral("queue")).toString();
        const QString id = song.value(QStringLiteral("id")).toString();
        if (!queueId.isEmpty()) {
            for (const DjQueueItem &item : m_model->shownQueue()) {
                if (item.id == queueId) {
                    m_model->playFromQueue(item, m_deck);
                    event->acceptProposedAction();
                    return;
                }
            }
        }
        m_model->load(m_deck, id, song.value(QStringLiteral("title")).toString(), song.value(QStringLiteral("author")).toString());
        event->acceptProposedAction();
        return;
    }
    QString text = mime->text();
    if (mime->hasUrls() && !mime->urls().isEmpty())
        text = mime->urls().first().toString();
    if (m_model->loadLink(m_deck, text))
        event->acceptProposedAction();
}

void DjDeckView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme::panel());
    const QColor color = theme::deck(m_deck);
    painter.setPen(QPen(m_dropping ? color : theme::line(), m_dropping ? 2 : 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.fillRect(m_mirrored ? QRect(width() - 3, 0, 3, height()) : QRect(0, 0, 3, height()), color);
}

}
