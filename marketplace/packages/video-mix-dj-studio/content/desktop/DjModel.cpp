#include "DjModel.h"
#include "DjBpm.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

constexpr int stateEnded = 0;
constexpr int statePlaying = 1;
constexpr int statePaused = 2;
constexpr int stateBuffering = 3;

QString deckName(int index) { return QStringLiteral("Deck %1").arg(index + 1); }

double clampDouble(double value, double low, double high, double fallback)
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, low, high);
}

}

double DjDeck::position(double now) const
{
    if (!advancing || scratching)
        return time;
    const double moved = time + (now - tickAt) * effectiveRate();
    return duration > 0 ? std::min(duration, moved) : moved;
}

double DjDeck::beatSeconds() const { return bpm > 0 ? 60.0 / bpm : 0.5; }

double DjDeck::coverage() const
{
    if (heard.isEmpty())
        return 0;
    const auto filled = std::count(heard.begin(), heard.end(), char(1));
    return double(filled) / heard.size();
}

const QList<double> &DjModel::loopBeats()
{
    static const QList<double> beats{0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32};
    return beats;
}

double DjModel::now()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return clock.nsecsElapsed() / 1e9;
}

DjModel::DjModel(const Host &host, DjPlayer *player, QObject *parent)
    : QObject(parent), m_host(host), m_player(player)
{
    for (int i = 0; i < 4; ++i) {
        m_decks[i].index = i;
        m_decks[i].assign = i % 2;
    }
    for (int i = 0; i < studio::sectionCount; ++i)
        m_sections.append(std::nullopt);
    m_automixTimer = new QTimer(this);
    m_automixTimer->setInterval(500);
    connect(m_automixTimer, &QTimer::timeout, this, [this]() { automixStep(); });
    m_fadeTimer = new QTimer(this);
    m_fadeTimer->setInterval(16);
    m_fadeTimer->setTimerType(Qt::PreciseTimer);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() { write(); });
    m_jamTimer = new QTimer(this);
    m_jamTimer->setSingleShot(true);
    connect(m_jamTimer, &QTimer::timeout, this, [this]() { poll(); });
    auto *loopTimer = new QTimer(this);
    loopTimer->setInterval(12);
    loopTimer->setTimerType(Qt::PreciseTimer);
    connect(loopTimer, &QTimer::timeout, this, [this]() { frame(now()); });
    loopTimer->start();
    if (m_player) {
        m_player->onEvent = [this](const QString &type, const QJsonObject &message) { handle(type, message); };
        m_player->onReload = [this]() {
            const double at = now();
            for (int i = 0; i < 4; ++i) {
                DjDeck &deck = m_decks[i];
                if (!deck.loaded())
                    continue;
                deck.time = deck.position(at);
                deck.tickAt = at;
                deck.advancing = false;
                deck.playing = deck.keepsPlaying;
                m_player->load(i, deck.videoId, deck.time, deck.effectiveRate(), deck.keepsPlaying);
                refreshSound(i);
            }
        };
    }
    restore();
}

DjModel::~DjModel()
{
    flush();
}

void DjModel::emitChanged()
{
    if (changed)
        changed();
}

void DjModel::emitDeck(int index)
{
    if (deckChanged)
        deckChanged(index);
}

void DjModel::emitLibrary()
{
    if (libraryChanged)
        libraryChanged();
}

void DjModel::show(const QString &text)
{
    m_notices.append({text, now()});
    while (m_notices.size() > 40)
        m_notices.removeFirst();
    if (noticed)
        noticed(text);
}

void DjModel::setFourDecks(bool on)
{
    if (m_fourDecks == on)
        return;
    if (!on) {
        for (int i = 2; i < 4; ++i) {
            if (m_decks[i].playing || m_decks[i].keepsPlaying)
                pause(i);
        }
    }
    m_fourDecks = on;
    applyMix();
    save();
    emitChanged();
}

void DjModel::setVideoMode(const QString &mode)
{
    m_videoMode = mode;
    applyMix();
    save();
    emitChanged();
}

void DjModel::setCrossfaderCurve(int curve)
{
    m_curve = std::clamp(curve, 0, 2);
    applyMix();
    save();
    emitChanged();
}

void DjModel::setAutomixLength(double seconds)
{
    m_automixLength = std::clamp(seconds, 2.0, 30.0);
    save();
    emitChanged();
}

double DjModel::crossfadeGain(int index) const
{
    const DjDeck &deck = m_decks[index];
    if (deck.assign == 2)
        return 1;
    const double p = std::clamp(m_crossfader, -1.0, 1.0);
    double a = 1;
    double b = 1;
    switch (m_curve) {
    case 1:
        a = std::min(1.0, 1 - p);
        b = std::min(1.0, 1 + p);
        break;
    case 2:
        a = std::clamp((1 - p) / 0.12, 0.0, 1.0);
        b = std::clamp((1 + p) / 0.12, 0.0, 1.0);
        break;
    default: {
        const auto both = studio::gains(p);
        a = both.first;
        b = both.second;
    }
    }
    return deck.assign == 0 ? a : b;
}

double DjModel::audibleGain(int index) const
{
    if (index >= deckCount())
        return 0;
    return crossfadeGain(index) * m_decks[index].level * m_master;
}

void DjModel::refreshSound(int index)
{
    if (!m_player)
        return;
    const DjDeck &deck = m_decks[index];
    DjDeckSound sound;
    sound.gain = std::round(audibleGain(index) * 1000) / 1000;
    sound.trim = std::pow(10.0, deck.gainDb / 20);
    for (int band = 0; band < 3; ++band) {
        const double v = deck.eq[band];
        sound.eq[band] = deck.kill[band] ? -40 : v < 0 ? v * 26 : v * 8;
    }
    sound.filter = deck.filter;
    for (int slot = 0; slot < 3; ++slot)
        sound.fx[slot] = deck.fxOn[slot] ? deck.fx[slot] : 0;
    sound.beat = deck.beatSeconds() / std::max(0.25, deck.effectiveRate());
    sound.keylock = deck.keylock;
    m_player->setSound(index, sound);
}

void DjModel::applyMix()
{
    for (int i = 0; i < 4; ++i)
        refreshSound(i);
    if (!m_player)
        return;
    QVector<DjVideoSlot> places(4);
    const int count = deckCount();
    if (m_videoMode == QLatin1String("split")) {
        const int columns = 2;
        const int rows = count > 2 ? 2 : 1;
        for (int i = 0; i < count; ++i)
            places[i] = {QRectF(double(i % columns) / columns, double(i / columns) / rows, 1.0 / columns, 1.0 / rows), 1, 1};
    } else if (m_videoMode.startsWith(QLatin1String("deck"))) {
        const int only = m_videoMode.mid(4).toInt();
        if (only >= 0 && only < count)
            places[only] = {QRectF(0, 0, 1, 1), 1, 1};
    } else {
        QVector<int> order;
        for (int i = 0; i < count; ++i) {
            if (m_decks[i].loaded())
                order.append(i);
        }
        std::sort(order.begin(), order.end(), [this](int l, int r) { return audibleGain(l) > audibleGain(r); });
        if (!order.isEmpty()) {
            const double first = audibleGain(order.first());
            const double second = order.size() > 1 ? audibleGain(order.at(1)) : 0;
            if (order.size() > 1) {
                places[order.at(1)] = {QRectF(0, 0, 1, 1), 1, 1};
                places[order.first()] = {QRectF(0, 0, 1, 1), first + second > 0.001 ? first / (first + second) : 0.5, 2};
            } else {
                places[order.first()] = {QRectF(0, 0, 1, 1), 1, 2};
            }
        }
    }
    m_player->layout(places);
}

void DjModel::settle(int index, double at) { m_decks[index].settleUntil = at + 0.6; }

void DjModel::rebase(int index)
{
    DjDeck &deck = m_decks[index];
    const double at = now();
    deck.time = deck.position(at);
    deck.tickAt = at;
}

double DjModel::shown(int index, double at)
{
    DjDeck &deck = m_decks[index];
    const double target = deck.position(at);
    if (!deck.moving() && !deck.spinning) {
        deck.spin.hold(target, at);
        return target;
    }
    const double goal = deck.advancing ? deck.effectiveRate() : deck.playing ? 0.35 * deck.effectiveRate() : 0;
    return deck.spin.step(target, goal, deck.playing ? deck.effectiveRate() : 0, at);
}

double DjModel::turned(int index, double at)
{
    shown(index, at);
    return m_decks[index].spin.turn();
}

void DjModel::frame(double at)
{
    for (int i = 0; i < 4; ++i) {
        DjDeck &deck = m_decks[i];
        if (deck.meterHold > 0)
            deck.meterHold = std::max(0.0, deck.meterHold - 0.008);
        if (!deck.loop || !deck.playing || deck.scratching || at < deck.settleUntil - 0.45)
            continue;
        const double position = deck.position(at);
        if (position >= deck.loopStart + deck.loopLength || position < deck.loopStart - 0.25) {
            m_player->seek(i, deck.loopStart);
            deck.time = deck.loopStart + std::max(0.0, position - deck.loopStart - deck.loopLength);
            deck.tickAt = at;
            deck.settleUntil = at + 0.3;
        }
    }
}

void DjModel::handle(const QString &type, const QJsonObject &message)
{
    const int index = message.value(QStringLiteral("k")).toInt(-1);
    if (type == QLatin1String("api")) {
        for (int i = 0; i < 4; ++i)
            refreshSound(i);
        applyMix();
        emitChanged();
        return;
    }
    if (type == QLatin1String("tick")) {
        for (const QJsonValue &row : message.value(QStringLiteral("d")).toArray()) {
            const QJsonArray item = row.toArray();
            if (item.size() < 5)
                continue;
            const int k = item.at(0).toInt(-1);
            if (k < 0 || k >= 4)
                continue;
            handleTick(k, item.at(1).toDouble(), item.at(2).toDouble(), item.at(3).toInt(-1), item.at(4).toDouble());
        }
        return;
    }
    if (index < 0 || index >= 4)
        return;
    if (type == QLatin1String("ready")) {
        DjDeck &deck = m_decks[index];
        refreshSound(index);
        m_player->setRate(index, deck.effectiveRate());
        if (deck.playing)
            m_player->play(index);
    } else if (type == QLatin1String("state")) {
        handleState(index, message.value(QStringLiteral("s")).toInt(-1));
    } else if (type == QLatin1String("meta")) {
        handleMeta(index, message.value(QStringLiteral("title")).toString(),
                   message.value(QStringLiteral("author")).toString());
    } else if (type == QLatin1String("error")) {
        handleError(index, message.value(QStringLiteral("code")).toInt());
    } else if (type == QLatin1String("pcm")) {
        handlePcm(index, message.value(QStringLiteral("id")).toString(), message.value(QStringLiteral("t")).toDouble(),
                  QByteArray::fromBase64(message.value(QStringLiteral("d")).toString().toLatin1()));
    } else if (type == QLatin1String("spun")) {
        handleSpun(index, message.value(QStringLiteral("t")).toDouble());
    } else if (type == QLatin1String("routed")) {
        m_decks[index].routed = true;
        refreshSound(index);
        emitDeck(index);
    }
}

void DjModel::handleTick(int index, double time, double duration, int state, double level)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    deck.meter = level;
    deck.meterHold = std::max(deck.meterHold, level);
    if (!believes(state, index, duration))
        return;
    if (duration > 0 && std::abs(duration - deck.duration) > 0.5) {
        deck.duration = duration;
        const int bars = int(std::ceil(duration * barsPerSecond));
        if (deck.bars.size() < bars) {
            deck.bars.resize(bars);
            deck.heard.resize(bars, 0);
        }
        emitDeck(index);
    }
    if (deck.scratching || deck.spinning)
        return;
    if (state == stateEnded && replay(index))
        return;
    const double at = now();
    if (at < deck.settleUntil) {
        advance(state == statePlaying && deck.playing, index);
        return;
    }
    const bool wasPlaying = deck.playing;
    applyState(state, index);
    const bool continuous = deck.advancing && state == statePlaying;
    const double predicted = deck.position(at);
    const double tracked = studio::trackedPosition(predicted, time, continuous);
    deck.time = tracked;
    deck.tickAt = at;
    advance(state == statePlaying, index);
    aimRepeat(index, at);
    if (wasPlaying != deck.playing)
        emitDeck(index);
    if (deck.playing && at - m_savedAt > 5)
        save(true);
    if (deck.waveDirty && at - deck.waveSaved > 20)
        cacheWave(index);
}

void DjModel::handleState(int index, int state)
{
    DjDeck &deck = m_decks[index];
    if (deck.scratching || deck.spinning || !deck.loaded())
        return;
    if (!believes(state, index, 0))
        return;
    if (state == stateEnded && replay(index))
        return;
    applyState(state, index);
    if (state == statePlaying)
        advance(true, index);
    if (state == stateEnded) {
        cacheWave(index, true);
        holdAutomix();
    }
    emitDeck(index);
}

void DjModel::handleMeta(int index, const QString &title, const QString &author)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || (deck.title == title && deck.author == author))
        return;
    deck.title = title.left(300);
    deck.author = author.left(200);
    bool known = false;
    for (DjHistoryItem &item : m_history) {
        if (item.id == deck.videoId) {
            item.title = deck.title;
            item.author = deck.author;
            known = true;
        }
    }
    m_library = studio::retitled(m_library, deck.videoId, deck.title, deck.author);
    if (m_crate.track(deck.videoId)) {
        m_crate.keep(deck.videoId, deck.title, deck.author);
        saveCrate();
    } else {
        refreshSmart();
    }
    Q_UNUSED(known);
    save();
    emitDeck(index);
    emitLibrary();
}

void DjModel::handleError(int index, int code)
{
    fail(index);
    const QString name = deckName(index);
    switch (code) {
    case 2:
        show(name + QStringLiteral(": that YouTube link is not valid."));
        break;
    case 100:
        show(name + QStringLiteral(": that video was removed or is private."));
        break;
    case 101:
    case 150:
    case 152:
    case 153:
        show(name + QStringLiteral(": the owner does not allow this video to be embedded."));
        break;
    default:
        show(name + QStringLiteral(": YouTube could not play this video."));
    }
    emitDeck(index);
}

void DjModel::fail(int index)
{
    DjDeck &deck = m_decks[index];
    deck.playing = false;
    deck.keepsPlaying = false;
    deck.failed = true;
    deck.doubtedEnd.reset();
    if (deck.loaded())
        m_unplayable.insert(deck.videoId);
    advance(false, index);
}

bool DjModel::replay(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.repeats || !deck.keepsPlaying || !deck.loaded() || deck.failed || deck.scratching)
        return false;
    QVector<DjWaveBar> normal = deck.bars;
    for (int i = 0; i < normal.size(); ++i)
        normal[i].level = i < deck.heard.size() && deck.heard.at(i) && deck.loudest > 0 ? deck.bars.at(i).level / deck.loudest : 1;
    const auto span = repeat::span(normal, deck.duration);
    rewind(index, span ? span->start : 0);
    play(index);
    return true;
}

void DjModel::aimRepeat(int index, double at)
{
    DjDeck &deck = m_decks[index];
    const int token = ++m_timerGeneration;
    m_repeatTimers[index] = token;
    if (!deck.repeats || deck.loop || !deck.playing || !deck.keepsPlaying || !deck.advancing || deck.scratching ||
        deck.failed || at < deck.settleUntil)
        return;
    QVector<DjWaveBar> normal = deck.bars;
    for (int i = 0; i < normal.size(); ++i)
        normal[i].level = i < deck.heard.size() && deck.heard.at(i) && deck.loudest > 0 ? deck.bars.at(i).level / deck.loudest : 1;
    const auto span = repeat::span(normal, deck.duration);
    if (!span)
        return;
    const DjRepeatStep step = repeat::step(deck.position(at), *span, deck.effectiveRate());
    if (step.kind == DjRepeatStep::Kind::Wrap) {
        rewind(index, span->start);
        count(index);
    } else if (step.kind == DjRepeatStep::Kind::After) {
        const QString id = deck.videoId;
        QTimer::singleShot(int(step.after * 1000), this, [this, index, token, id]() {
            if (m_repeatTimers.value(index) != token || m_decks[index].videoId != id)
                return;
            aimRepeat(index, now());
        });
    }
}

bool DjModel::believes(int state, int index, double reported)
{
    DjDeck &deck = m_decks[index];
    if (state != stateEnded || deck.scratching) {
        deck.doubtedEnd.reset();
        return true;
    }
    const double at = now();
    const double length = reported > 0 ? reported : deck.duration;
    switch (end::judge(deck.position(at), length, deck.doubtedEnd, deck.keepsPlaying, at)) {
    case DjEndReport::Ended:
        deck.doubtedEnd.reset();
        return true;
    case DjEndReport::Doubted:
        if (!deck.doubtedEnd)
            deck.doubtedEnd = at;
        return false;
    case DjEndReport::Failed:
        fail(index);
        show(deckName(index) + QStringLiteral(": YouTube stopped this video at once, so it is skipped."));
        emitDeck(index);
        return false;
    }
    return true;
}

void DjModel::advance(bool advancing, int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.advancing == advancing || deck.scratching)
        return;
    rebase(index);
    deck.advancing = advancing;
}

void DjModel::applyState(int state, int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.scratching)
        return;
    const bool playing = state == statePlaying || state == stateBuffering;
    deck.playing = playing;
    if (state != statePlaying)
        advance(false, index);
    if (state == stateEnded) {
        deck.keepsPlaying = false;
        deck.counted = false;
        if (deck.duration > 0)
            deck.time = deck.duration;
    }
    Q_UNUSED(statePaused);
}

void DjModel::prepare(int index, const QString &id, const QString &title, const QString &author, double start,
                      bool keepSettings, bool playNow)
{
    DjDeck &deck = m_decks[index];
    if (deck.loaded() && deck.videoId != id)
        cacheWave(index, true);
    m_sectionTimers[index] = ++m_timerGeneration;
    deck.videoId = id;
    deck.title = title.left(300);
    deck.author = author.left(200);
    deck.bars.clear();
    deck.heard.clear();
    deck.pcm.clear();
    deck.loudest = 0;
    deck.waveDirty = false;
    deck.bpmQueued = false;
    deck.time = start;
    deck.tickAt = now();
    deck.duration = 0;
    deck.playing = playNow;
    deck.keepsPlaying = playNow;
    deck.advancing = false;
    deck.counted = false;
    deck.failed = false;
    deck.routed = false;
    deck.doubtedEnd.reset();
    deck.spinning = false;
    if (!keepSettings) {
        deck.cue = 0;
        deck.bpm = 0;
        deck.loop = false;
        deck.hotcues.fill(-1);
        deck.tap.clear();
    }
    readWave(index);
    m_player->load(index, id, start, deck.effectiveRate(), playNow);
    refreshSound(index);
    applyMix();
    artwork(id);
    emitDeck(index);
}

void DjModel::load(int index, const QString &id, const QString &title, const QString &author)
{
    if (!studio::isVideoId(id) || index < 0 || index >= 4)
        return;
    const DjTrack known = song(id);
    prepare(index, id, title.isEmpty() ? known.title : title, author.isEmpty() ? known.author : author, 0, false);
    show(title.isEmpty() ? deckName(index) + QStringLiteral(" loaded.")
                         : deckName(index) + QStringLiteral(": ") + title);
    save();
    share(index);
}

bool DjModel::loadLink(int index, const QString &text)
{
    const QString id = studio::videoId(text);
    if (id.isEmpty())
        return false;
    load(index, id);
    return true;
}

void DjModel::eject(int index)
{
    DjDeck &deck = m_decks[index];
    cacheWave(index, true);
    m_sectionTimers[index] = ++m_timerGeneration;
    m_player->eject(index);
    deck.videoId.clear();
    deck.title.clear();
    deck.author.clear();
    deck.bars.clear();
    deck.heard.clear();
    deck.pcm.clear();
    deck.loudest = 0;
    deck.time = 0;
    deck.duration = 0;
    deck.playing = false;
    deck.keepsPlaying = false;
    deck.advancing = false;
    deck.cue = 0;
    deck.bpm = 0;
    deck.loop = false;
    deck.hotcues.fill(-1);
    deck.sample = {};
    for (auto &section : m_sections) {
        if (section && section->deck == index)
            section.reset();
    }
    applyMix();
    save();
    share(index);
    emitDeck(index);
}

void DjModel::start(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded()) {
        show(QStringLiteral("Load a song on %1 first: search below or drag a song onto it.").arg(deckName(index)));
        return;
    }
    if (!deck.playing)
        play(index);
}

void DjModel::togglePlay(int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.playing || deck.keepsPlaying)
        pauseDeck(index);
    else
        start(index);
}

void DjModel::pauseDeck(int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.playing || deck.keepsPlaying)
        pause(index);
    holdAutomix();
}

void DjModel::stop(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    m_sectionTimers[index] = ++m_timerGeneration;
    if (deck.playing || deck.keepsPlaying)
        pause(index);
    rewind(index);
    holdAutomix();
}

void DjModel::restart(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded()) {
        start(index);
        return;
    }
    m_sectionTimers[index] = ++m_timerGeneration;
    rewind(index);
    if (!deck.playing)
        play(index);
}

void DjModel::rewind(int index, double startAt)
{
    DjDeck &deck = m_decks[index];
    m_repeatTimers[index] = ++m_timerGeneration;
    deck.counted = false;
    m_player->seek(index, startAt);
    deck.time = startAt;
    deck.tickAt = now();
    settle(index, deck.tickAt);
    share(index);
    emitDeck(index);
}

void DjModel::play(int index)
{
    DjDeck &deck = m_decks[index];
    m_player->play(index);
    rebase(index);
    settle(index, now());
    deck.playing = true;
    deck.keepsPlaying = true;
    count(index);
    remember(index);
    share(index);
    save();
    emitDeck(index);
}

void DjModel::pause(int index)
{
    DjDeck &deck = m_decks[index];
    advance(false, index);
    settle(index, now());
    deck.playing = false;
    deck.keepsPlaying = false;
    m_player->pause(index);
    share(index);
    save();
    emitDeck(index);
}

void DjModel::remember(int index)
{
    const DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    const auto updated = studio::remember(m_history, deck.videoId, deck.title, deck.author);
    if (updated == m_history)
        return;
    m_history = updated;
    save();
    emitLibrary();
}

void DjModel::count(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || deck.counted)
        return;
    deck.counted = true;
    m_library = studio::played(m_library, deck.videoId, deck.title, deck.author,
                               QDateTime::currentMSecsSinceEpoch() / 1000.0);
    refreshSmart();
    emitLibrary();
}

void DjModel::setCue(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    deck.cue = deck.position(now());
    show(QStringLiteral("%1 cue set at %2.").arg(deckName(index), studio::clock(deck.cue)));
    save();
    emitDeck(index);
}

void DjModel::jumpToCue(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    m_player->seek(index, deck.cue);
    deck.time = deck.cue;
    deck.tickAt = now();
    settle(index, deck.tickAt);
    share(index);
    emitDeck(index);
}

void DjModel::cueStop(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    if (deck.playing || deck.keepsPlaying)
        pause(index);
    jumpToCue(index);
    holdAutomix();
}

void DjModel::cuePress(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    if (deck.playing || deck.keepsPlaying) {
        cueStop(index);
        return;
    }
    if (std::abs(deck.position(now()) - deck.cue) < 0.05) {
        jumpToCue(index);
        return;
    }
    setCue(index);
}

void DjModel::seek(int index, double seconds)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || deck.duration <= 0)
        return;
    const double target = std::clamp(seconds, 0.0, deck.duration);
    deck.time = target;
    deck.tickAt = now();
    settle(index, deck.tickAt);
    m_player->seek(index, target);
    if (deck.loop && (target < deck.loopStart || target > deck.loopStart + deck.loopLength))
        deck.loop = false;
    share(index);
    emitDeck(index);
}

void DjModel::beatJump(int index, double beats)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    seek(index, deck.position(now()) + beats * deck.beatSeconds());
}

void DjModel::setRate(int index, double rate)
{
    DjDeck &deck = m_decks[index];
    const double clamped = studio::clampRate(rate, deck.pitchRange);
    if (std::abs(clamped - deck.rate) <= 0.0001)
        return;
    rebase(index);
    deck.rate = clamped;
    sendRate(index);
    save();
    share(index);
    emitDeck(index);
}

void DjModel::resetPitch(int index)
{
    DjDeck &deck = m_decks[index];
    rebase(index);
    deck.rate = 1;
    sendRate(index);
    save();
    share(index);
    emitDeck(index);
}

void DjModel::setPitchRange(int index, double range)
{
    DjDeck &deck = m_decks[index];
    deck.pitchRange = std::clamp(range, 0.04, 0.5);
    if (inJam() && index < 2)
        deck.pitchRange = std::min(deck.pitchRange, studio::pitchRange);
    setRate(index, deck.rate);
    emitDeck(index);
}

void DjModel::touchBegan(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || !deck.playing || deck.scratching)
        return;
    m_brakeTimers[index] = ++m_timerGeneration;
    deck.brakeStart = now();
    rebase(index);
    deck.brake = studio::touchBrake;
    sendRate(index);
}

void DjModel::touchEnded(int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.brake == 0)
        return;
    const double left = studio::touchBrakeSeconds - (now() - deck.brakeStart);
    const int token = ++m_timerGeneration;
    m_brakeTimers[index] = token;
    QTimer::singleShot(std::max(0, int(left * 1000)), this, [this, index, token]() {
        if (m_brakeTimers.value(index) == token)
            releaseBrake(index);
    });
}

void DjModel::releaseBrake(int index)
{
    DjDeck &deck = m_decks[index];
    m_brakeTimers[index] = ++m_timerGeneration;
    if (deck.brake == 0)
        return;
    rebase(index);
    deck.brake = 0;
    sendRate(index);
}

void DjModel::bend(int index, double direction)
{
    DjDeck &deck = m_decks[index];
    rebase(index);
    deck.bend = direction * studio::bendAmount;
    sendRate(index);
    emitDeck(index);
}

void DjModel::sendRate(int index)
{
    if (m_rateTimers.value(index) > 0)
        return;
    m_rateTimers[index] = 1;
    QTimer::singleShot(30, this, [this, index]() {
        m_rateTimers[index] = 0;
        m_player->setRate(index, m_decks[index].effectiveRate());
        refreshSound(index);
    });
}

void DjModel::sync(int index)
{
    DjDeck &deck = m_decks[index];
    int other = -1;
    double best = -1;
    for (int i = 0; i < deckCount(); ++i) {
        if (i == index || !m_decks[i].loaded())
            continue;
        const double score = (m_decks[i].playing ? 2 : 0) + audibleGain(i);
        if (score > best) {
            best = score;
            other = i;
        }
    }
    if (!deck.loaded() || other < 0) {
        show(QStringLiteral("Load two decks to sync."));
        return;
    }
    const DjDeck &target = m_decks[other];
    rebase(index);
    deck.rate = studio::syncedRate(deck.bpm, target.bpm, target.effectiveRate(), deck.pitchRange);
    sendRate(index);
    share(index);
    show(deck.bpm > 0 && target.bpm > 0
             ? QStringLiteral("%1 matched to %2 BPM.").arg(deckName(index)).arg(target.shownBpm(), 0, 'f', 1)
             : QStringLiteral("%1 matched %2's tempo. Tap both BPM boxes to beat-match.").arg(deckName(index), deckName(other)));
    save();
    emitDeck(index);
}

void DjModel::tapTempo(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    if (const auto bpm = deck.tap.tap(now())) {
        deck.bpm = *bpm / std::max(0.01, deck.effectiveRate());
        updateLoopLength(index);
        save();
        emitDeck(index);
    } else if (deck.bpm == 0) {
        show(QStringLiteral("Keep tapping on the beat to set %1's BPM.").arg(deckName(index)));
    }
}

void DjModel::clearBpm(int index)
{
    m_decks[index].bpm = 0;
    m_decks[index].tap.clear();
    updateLoopLength(index);
    save();
    emitDeck(index);
}

void DjModel::detectBpm(int index, bool quietly)
{
    DjDeck &deck = m_decks[index];
    const QString id = deck.videoId;
    if (id.isEmpty() || deck.heard.isEmpty()) {
        if (!quietly)
            show(QStringLiteral("%1 has not been heard long enough yet: let it play, or tap the BPM box on the beat.").arg(deckName(index)));
        return;
    }
    const int perBar = pcmRate / barsPerSecond;
    int bestStart = 0;
    int bestLength = 0;
    int runStart = -1;
    for (int i = 0; i <= deck.heard.size(); ++i) {
        const bool on = i < deck.heard.size() && deck.heard.at(i);
        if (on && runStart < 0)
            runStart = i;
        if (!on && runStart >= 0) {
            if (i - runStart > bestLength) {
                bestLength = i - runStart;
                bestStart = runStart;
            }
            runStart = -1;
        }
    }
    if (bestLength < barsPerSecond * 12) {
        if (!quietly)
            show(QStringLiteral("%1 needs about 15 seconds of playing to find its BPM.").arg(deckName(index)));
        return;
    }
    const qsizetype from = qsizetype(bestStart) * perBar;
    const qsizetype length = std::min<qsizetype>(qsizetype(bestLength) * perBar, deck.pcm.size() - from);
    if (length <= 0)
        return;
    const QVector<qint16> samples = deck.pcm.mid(from, length);
    deck.bpmQueued = true;
    QtConcurrent::run([samples]() { return bpm::detect(samples, pcmRate); })
        .then(this, [this, index, id, quietly](std::optional<double> found) {
            DjDeck &current = m_decks[index];
            if (current.videoId != id)
                return;
            if (quietly && current.bpm > 0)
                return;
            if (!found) {
                if (!quietly)
                    show(QStringLiteral("No steady beat found on %1: tap the BPM box on the beat instead.").arg(deckName(index)));
                return;
            }
            current.bpm = *found;
            current.tap.clear();
            current.waveDirty = true;
            updateLoopLength(index);
            refreshSound(index);
            save();
            emitDeck(index);
        });
}

void DjModel::updateLoopLength(int index)
{
    DjDeck &deck = m_decks[index];
    const double beats = loopBeats().value(deck.loopSize, 16);
    deck.loopLength = deck.bpm > 0 ? beats * 60 / deck.bpm : beats * 0.5;
}

void DjModel::toggleLoop(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    deck.loop = !deck.loop;
    if (deck.loop) {
        updateLoopLength(index);
        deck.loopStart = deck.position(now());
    }
    show(deck.loop ? QStringLiteral("%1 loops %2 from %3.")
                         .arg(deckName(index),
                              deck.bpm > 0 ? QStringLiteral("%1 beats").arg(loopBeats().value(deck.loopSize))
                                           : QStringLiteral("%1 s").arg(deck.loopLength, 0, 'f', 1),
                              studio::clock(deck.loopStart))
                   : QStringLiteral("%1 loop off.").arg(deckName(index)));
    save();
    emitDeck(index);
}

void DjModel::loopIn(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    deck.loopStart = deck.position(now());
    deck.loop = false;
    show(QStringLiteral("%1 loop starts at %2. Press OUT to close it.").arg(deckName(index), studio::clock(deck.loopStart)));
    emitDeck(index);
}

void DjModel::loopOut(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    const double end = deck.position(now());
    if (deck.loop) {
        deck.loop = false;
        emitDeck(index);
        return;
    }
    if (end - deck.loopStart < 0.05) {
        show(QStringLiteral("Press IN first, then OUT a little later."));
        return;
    }
    deck.loopLength = end - deck.loopStart;
    deck.loop = true;
    seek(index, deck.loopStart);
    deck.loop = true;
    emitDeck(index);
}

void DjModel::setLoopSize(int index, int size)
{
    DjDeck &deck = m_decks[index];
    deck.loopSize = std::clamp(size, 0, int(loopBeats().size()) - 1);
    updateLoopLength(index);
    save();
    emitDeck(index);
}

void DjModel::loopRoll(int index, int size, bool on)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || !deck.playing)
        return;
    static QHash<int, QPair<double, double>> rolls;
    const double at = now();
    if (on) {
        rolls.insert(index, {deck.position(at), at});
        const double beats = loopBeats().value(std::clamp(size, 0, int(loopBeats().size()) - 1));
        deck.loopStart = deck.position(at);
        deck.loopLength = beats * deck.beatSeconds();
        deck.loop = true;
    } else if (rolls.contains(index)) {
        const auto roll = rolls.take(index);
        deck.loop = false;
        updateLoopLength(index);
        seek(index, roll.first + (at - roll.second) * deck.effectiveRate());
    }
    emitDeck(index);
}

void DjModel::toggleRepeat(int index)
{
    DjDeck &deck = m_decks[index];
    deck.repeats = !deck.repeats;
    show(deck.repeats ? QStringLiteral("%1 repeats its song.").arg(deckName(index))
                      : QStringLiteral("%1 repeat off.").arg(deckName(index)));
    save();
    emitDeck(index);
}

void DjModel::toggleKeylock(int index)
{
    DjDeck &deck = m_decks[index];
    deck.keylock = !deck.keylock;
    refreshSound(index);
    show(deck.keylock ? QStringLiteral("%1 key lock on: tempo changes keep the pitch.").arg(deckName(index))
                      : QStringLiteral("%1 key lock off: tempo changes the pitch, like vinyl.").arg(deckName(index)));
    save();
    emitDeck(index);
}

void DjModel::setLevel(int index, double level)
{
    m_decks[index].level = std::clamp(level, 0.0, 1.0);
    applyMix();
    save();
    emitDeck(index);
}

void DjModel::setGain(int index, double db)
{
    m_decks[index].gainDb = std::clamp(db, -12.0, 12.0);
    refreshSound(index);
    save();
    emitDeck(index);
}

void DjModel::setEq(int index, int band, double value)
{
    if (band < 0 || band > 2)
        return;
    m_decks[index].eq[band] = std::clamp(value, -1.0, 1.0);
    refreshSound(index);
    save();
    emitDeck(index);
}

void DjModel::toggleKill(int index, int band)
{
    if (band < 0 || band > 2)
        return;
    m_decks[index].kill[band] = !m_decks[index].kill[band];
    refreshSound(index);
    emitDeck(index);
}

void DjModel::setFilter(int index, double value)
{
    m_decks[index].filter = std::abs(value) < 0.03 ? 0 : std::clamp(value, -1.0, 1.0);
    refreshSound(index);
    save();
    emitDeck(index);
}

void DjModel::setFx(int index, int slot, double amount)
{
    if (slot < 0 || slot > 2)
        return;
    m_decks[index].fx[slot] = std::clamp(amount, 0.0, 1.0);
    refreshSound(index);
    save();
    emitDeck(index);
}

void DjModel::toggleFx(int index, int slot)
{
    if (slot < 0 || slot > 2)
        return;
    m_decks[index].fxOn[slot] = !m_decks[index].fxOn[slot];
    refreshSound(index);
    emitDeck(index);
}

void DjModel::brakeDeck(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || !deck.playing || deck.scratching || deck.spinning)
        return;
    deck.spinning = true;
    m_player->brake(index);
    emitDeck(index);
}

void DjModel::backspin(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || !deck.playing || deck.scratching || deck.spinning)
        return;
    if (!deck.routed) {
        brakeDeck(index);
        return;
    }
    rebase(index);
    deck.spinning = true;
    m_player->backspin(index);
    emitDeck(index);
}

void DjModel::handleSpun(int index, double at)
{
    DjDeck &deck = m_decks[index];
    if (!deck.spinning)
        return;
    deck.spinning = false;
    deck.time = std::max(0.0, at);
    deck.tickAt = now();
    pause(index);
    m_player->seek(index, deck.time);
    holdAutomix();
    emitDeck(index);
}

void DjModel::setAssign(int index, int assign)
{
    m_decks[index].assign = std::clamp(assign, 0, 2);
    applyMix();
    save();
    emitDeck(index);
}

void DjModel::hotCue(int index, int pad)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || pad < 0 || pad >= 8)
        return;
    if (deck.hotcues[pad] < 0) {
        deck.hotcues[pad] = deck.position(now());
        deck.waveDirty = true;
        show(QStringLiteral("%1 hot cue %2 set at %3.").arg(deckName(index)).arg(pad + 1).arg(studio::clock(deck.hotcues[pad])));
        save();
        emitDeck(index);
        return;
    }
    seek(index, deck.hotcues[pad]);
    if (!deck.playing)
        play(index);
}

void DjModel::deleteHotCue(int index, int pad)
{
    if (pad < 0 || pad >= 8)
        return;
    m_decks[index].hotcues[pad] = -1;
    m_decks[index].waveDirty = true;
    save();
    emitDeck(index);
}

void DjModel::scratchBegan(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || deck.duration <= 0 || deck.scratching || deck.spinning)
        return;
    releaseBrake(index);
    rebase(index);
    deck.resumeAfterScratch = deck.playing;
    deck.advancing = false;
    deck.scratching = true;
    const double at = now();
    deck.scratchStamp = at;
    deck.scratchSent = 0;
    deck.scratchSpeed = 0;
    m_player->scratch(index, deck.time);
    emitDeck(index);
}

void DjModel::scratchMoved(int index, double radians)
{
    DjDeck &deck = m_decks[index];
    if (!deck.scratching)
        return;
    const double seconds = studio::scratchSeconds(radians);
    const double at = now();
    deck.time = std::clamp(deck.time + seconds, 0.0, deck.duration);
    deck.scratchSpeed = studio::scratchSpeed(deck.scratchSpeed, seconds, at - deck.scratchStamp);
    deck.scratchStamp = at;
    sendScratch(index);
}

void DjModel::sendScratch(int index)
{
    DjDeck &deck = m_decks[index];
    const double at = now();
    if (at - deck.scratchSent < 0.012)
        return;
    deck.scratchSent = at;
    m_player->scratch(index, deck.time);
}

void DjModel::scratchEnded(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.scratching)
        return;
    deck.scratchSpeed = 0;
    m_player->scratch(index, std::nullopt);
    m_player->seek(index, deck.time);
    deck.scratching = false;
    deck.tickAt = now();
    settle(index, deck.tickAt);
    deck.playing = deck.resumeAfterScratch;
    deck.keepsPlaying = deck.resumeAfterScratch;
    if (deck.resumeAfterScratch)
        m_player->play(index);
    else
        m_player->pause(index);
    share(index);
    emitDeck(index);
}

void DjModel::toggleRemaining(int index)
{
    m_decks[index].showsRemaining = !m_decks[index].showsRemaining;
    emitDeck(index);
}

void DjModel::setCrossfader(double value, bool cancelAutomix, bool saving)
{
    if (cancelAutomix && m_automixing)
        stopFade();
    const double clamped = std::clamp(value, -1.0, 1.0);
    if (clamped == m_crossfader)
        return;
    m_crossfader = clamped;
    applyMix();
    if (saving)
        save();
    emitChanged();
}

void DjModel::setMaster(double value)
{
    m_master = std::clamp(value, 0.0, 1.25);
    applyMix();
    save();
    emitChanged();
}

void DjModel::slam(DjSide side)
{
    stopFade();
    fade(side == DjSide::A ? -1 : 1, 0.18, nullptr);
}

void DjModel::stopFade()
{
    ++m_fadeGeneration;
    m_fadeTimer->stop();
    m_fadeTimer->disconnect();
    m_automixing = false;
}

void DjModel::fade(double end, double seconds, std::function<void()> then)
{
    m_fadeTimer->stop();
    m_fadeTimer->disconnect();
    const int token = ++m_fadeGeneration;
    const double from = m_crossfader;
    const double began = now();
    connect(m_fadeTimer, &QTimer::timeout, this, [this, token, from, end, seconds, began, then]() {
        if (token != m_fadeGeneration)
            return;
        const double progress = std::min(1.0, (now() - began) / std::max(0.01, seconds));
        const double eased = progress * progress * (3 - 2 * progress);
        setCrossfader(from + (end - from) * eased, false, false);
        if (progress >= 1) {
            m_fadeTimer->stop();
            m_fadeTimer->disconnect();
            save();
            if (then)
                then();
        }
    });
    m_fadeTimer->start();
}

void DjModel::toggleAutomix()
{
    const bool autoDj = m_autoDjOn;
    setAutomix(!m_automixOn);
    const QString focus = m_crate.label(m_crate.focus) + (m_crate.shuffle ? QStringLiteral(", shuffled") : QStringLiteral(" in order"));
    if (!m_automixOn)
        show(autoDj ? QStringLiteral("AUTOMIX and AUTO DJ off.") : QStringLiteral("AUTOMIX off."));
    else if (!m_crate.focus.isEverything())
        show(shownQueue().isEmpty() ? QStringLiteral("AUTOMIX on: playing %1.").arg(focus)
                                    : QStringLiteral("AUTOMIX on: the queue, then %1.").arg(focus));
    else if (!shownQueue().isEmpty())
        show(QStringLiteral("AUTOMIX on: the queue plays one song after another."));
    else if (m_library.isEmpty() && m_crate.tracks.isEmpty())
        show(QStringLiteral("AUTOMIX on: queue songs and they play one after another."));
    else
        show(QStringLiteral("AUTOMIX on: with the queue empty it shuffles the songs you have played."));
}

void DjModel::setAutomix(bool on)
{
    m_automixTimer->stop();
    if (!on) {
        stopFade();
        stopAutoDj();
    }
    m_automixOn = on;
    if (on)
        m_automixTimer->start();
    save();
    emitChanged();
}

void DjModel::holdAutomix()
{
    if (!m_automixOn || m_decks[0].playing || m_decks[1].playing)
        return;
    const bool autoDj = m_autoDjOn;
    setAutomix(false);
    show(autoDj ? QStringLiteral("AUTOMIX and AUTO DJ off.") : QStringLiteral("AUTOMIX off."));
}

DjAutomixDeck DjModel::snapshot(int index) const
{
    const DjDeck &deck = m_decks[index];
    DjAutomixDeck out;
    out.loaded = deck.loaded() && !deck.failed;
    out.playing = deck.playing;
    out.position = deck.position(now());
    out.duration = deck.duration;
    out.rate = deck.effectiveRate();
    out.repeats = deck.repeats;
    return out;
}

void DjModel::automixStep()
{
    if (!m_automixOn || m_automixing || m_decks[0].scratching || m_decks[1].scratching)
        return;
    const DjSide favoured = m_crossfader <= 0 ? DjSide::A : DjSide::B;
    if (m_autoDjOn)
        primeAutoDj();
    const bool picked = m_autoDjOn && autoDjNext().has_value();
    const bool pooled = !(m_autoDjOn && m_autoDjFinding) && !m_crate.pool(m_library, m_unplayable).isEmpty();
    DjAutomixDeck a = snapshot(0);
    DjAutomixDeck b = snapshot(1);
    const double scale = studio::automixSeconds / m_automixLength;
    if (a.duration > 0 && a.playing)
        a.rate *= scale;
    if (b.duration > 0 && b.playing)
        b.rate *= scale;
    const DjAutomixStep step = automix::step(a, b, favoured, !shownQueue().isEmpty() || picked || pooled);
    const int from = step.side == DjSide::A ? 0 : 1;
    const int to = step.to == DjSide::A ? 0 : 1;
    switch (step.kind) {
    case DjAutomixStep::Kind::Wait:
        break;
    case DjAutomixStep::Kind::Preload:
        preload(from);
        break;
    case DjAutomixStep::Kind::Start:
        play(from);
        fade(from == 0 ? -1 : 1, 0.4, nullptr);
        break;
    case DjAutomixStep::Kind::Mix:
        mix(from, to, std::min(m_automixLength, step.seconds / scale));
        break;
    }
}

bool DjModel::preload(int index)
{
    const QList<DjQueueItem> queued = shownQueue();
    if (!queued.isEmpty()) {
        playFromQueue(queued.first(), index);
    } else if (m_autoDjOn) {
        if (const auto pick = takeAutoDj()) {
            load(index, pick->id, pick->title, pick->author);
            show(QStringLiteral("AUTO DJ: %1 is up next.").arg(pick->title));
        } else if (const auto song = replayPick()) {
            load(index, song->id, song->title, song->author);
        } else {
            return false;
        }
    } else if (const auto song = replayPick()) {
        load(index, song->id, song->title, song->author);
    } else {
        return false;
    }
    return true;
}

std::optional<DjPlayedSong> DjModel::replayPick()
{
    QSet<QString> loaded;
    QSet<QString> playing;
    for (int i = 0; i < deckCount(); ++i) {
        if (!m_decks[i].loaded())
            continue;
        loaded.insert(m_decks[i].videoId);
        if (m_decks[i].playing)
            playing.insert(m_decks[i].videoId);
    }
    const auto song = m_crate.next(m_library, loaded, playing, QRandomGenerator::global()->generate64(), m_unplayable);
    if (song && !m_crate.focus.isEverything())
        saveCrate();
    return song;
}

void DjModel::refocusAutomix()
{
    if (!m_automixOn || m_automixing || !shownQueue().isEmpty() || m_crate.focus.isEverything())
        return;
    QSet<QString> pool;
    for (const DjPlayedSong &song : m_crate.pool(m_library, m_unplayable))
        pool.insert(song.id);
    const int favoured = m_crossfader <= 0 ? 0 : 1;
    for (int index : {favoured, 1 - favoured}) {
        const DjDeck &deck = m_decks[index];
        if (deck.playing || deck.scratching || pool.contains(deck.videoId))
            continue;
        if (pool.size() > 1 || index == favoured || m_decks[0].playing || m_decks[1].playing)
            preload(index);
    }
}

bool DjModel::skipOut(int index)
{
    if (m_automixing || m_decks[index].scratching || index > 1)
        return false;
    const int other = 1 - index;
    if (!m_decks[index].playing) {
        if (!preload(index))
            eject(index);
        return true;
    }
    if (m_decks[other].playing) {
        pause(index);
        fade(other == 0 ? -1 : 1, 0.4, nullptr);
        if (!preload(index))
            eject(index);
        return true;
    }
    QSet<QString> pool;
    for (const DjPlayedSong &song : m_crate.pool(m_library, m_unplayable))
        pool.insert(song.id);
    if (!(snapshot(other).fresh() && pool.contains(m_decks[other].videoId)) && !preload(other)) {
        pause(index);
        return false;
    }
    mix(index, other, 2, true);
    return true;
}

void DjModel::mix(int from, int to, double seconds, bool replacing)
{
    m_automixing = true;
    if (!m_decks[to].playing)
        play(to);
    m_fadeTimer->stop();
    m_fadeTimer->disconnect();
    const int token = ++m_fadeGeneration;
    const double began = now();
    auto waiter = std::make_shared<std::function<void()>>();
    *waiter = [this, token, began, from, to, seconds, replacing, waiter]() {
        if (token != m_fadeGeneration)
            return;
        if (!m_decks[to].advancing && now() - began < automix::audible) {
            QTimer::singleShot(20, this, *waiter);
            return;
        }
        const double left = snapshot(from).remaining().value_or(seconds);
        fade(to == 1 ? 1 : -1, std::max(2.0, std::min(seconds, left)), [this, from, replacing]() {
            m_automixing = false;
            if (m_decks[from].playing)
                pause(from);
            if (replacing && !preload(from))
                eject(from);
            emitChanged();
        });
    };
    QTimer::singleShot(20, this, *waiter);
    emitChanged();
}

void DjModel::mixNow()
{
    if (m_automixing) {
        stopFade();
        emitChanged();
        return;
    }
    const int target = m_crossfader <= 0 ? 1 : 0;
    const int outgoing = 1 - target;
    if (!m_decks[target].loaded()) {
        if (!preload(target)) {
            show(QStringLiteral("Load %1 to mix into it.").arg(deckName(target)));
            return;
        }
    }
    if (!m_decks[target].playing)
        play(target);
    m_automixing = true;
    fade(target == 1 ? 1 : -1, m_automixLength, [this, outgoing]() {
        m_automixing = false;
        if (m_decks[outgoing].playing)
            pause(outgoing);
        emitChanged();
    });
    emitChanged();
}

void DjModel::quickNext(int index)
{
    DjDeck &deck = m_decks[index];
    if (deck.scratching)
        return;
    bool resume = deck.playing || deck.keepsPlaying;
    if (m_automixing && index < 2) {
        stopFade();
        const int other = 1 - index;
        if (m_decks[other].playing) {
            resume = false;
            fade(other == 0 ? -1 : 1, 0.4, nullptr);
        }
    }
    if (!preload(index)) {
        show(m_crate.focus.isEverything() ? QStringLiteral("Nothing to load next: queue a song or play a few first.")
                                          : QStringLiteral("Nothing else in %1 to load next.").arg(m_crate.label(m_crate.focus)));
        return;
    }
    if (resume)
        play(index);
}

void DjModel::deleteSongOn(int index)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    const QString id = deck.videoId;
    const QString name = deck.title.isEmpty() ? QStringLiteral("the song") : deck.title;
    removeSong(id);
    if (!inJam()) {
        const auto before = m_queue.size();
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [&](const DjQueueItem &item) { return item.videoId == id; }), m_queue.end());
        if (m_queue.size() != before)
            save();
    }
    if (m_automixOn && deck.playing && skipOut(index)) {
        show(QStringLiteral("Deleted %1. Mixing into the next song.").arg(name));
        return;
    }
    const bool resume = deck.playing || deck.keepsPlaying;
    if (preload(index)) {
        if (resume)
            play(index);
        show(QStringLiteral("Deleted %1. Loaded the next song.").arg(name));
    } else {
        eject(index);
        show(QStringLiteral("Deleted %1.").arg(name));
    }
}

void DjModel::sample(int index)
{
    DjDeck &deck = m_decks[index];
    switch (deck.sample.kind) {
    case DjSampleStepKind::Idle:
        if (!deck.loaded() || deck.duration <= 0) {
            show(QStringLiteral("Load %1 before sampling it.").arg(deckName(index)));
            return;
        }
        deck.sample = {DjSampleStepKind::Recording, deck.position(now()), 0};
        show(QStringLiteral("Sampling %1. Press SAMPLE again to stop.").arg(deckName(index)));
        break;
    case DjSampleStepKind::Recording: {
        const auto section = studio::sampleSection(index, deck.sample.start, deck.position(now()));
        if (!section) {
            deck.sample = {};
            show(QStringLiteral("Nothing was sampled. Play %1 while sampling.").arg(deckName(index)));
            break;
        }
        const int slot = m_sectionCursor;
        m_sections[slot] = section;
        m_sectionCursor = (slot + 1) % studio::sectionCount;
        deck.sample = {DjSampleStepKind::Armed, 0, slot};
        show(QStringLiteral("Sample armed: %1 s from %2. Press again to play it.")
                 .arg(section->duration, 0, 'f', 1)
                 .arg(studio::clock(section->start)));
        save();
        break;
    }
    case DjSampleStepKind::Armed: {
        const int slot = deck.sample.slot;
        deck.sample = {};
        playSection(slot);
        break;
    }
    }
    emitDeck(index);
    emitChanged();
}

void DjModel::playSection(int slot)
{
    if (slot < 0 || slot >= m_sections.size() || !m_sections.at(slot))
        return;
    const DjSection section = *m_sections.at(slot);
    const int index = section.deck;
    if (index < 0 || index >= 4 || !m_decks[index].loaded())
        return;
    const bool keepPlaying = m_decks[index].playing;
    m_player->seek(index, section.start);
    m_decks[index].time = section.start;
    m_decks[index].tickAt = now();
    play(index);
    const int token = ++m_timerGeneration;
    m_sectionTimers[index] = token;
    QTimer::singleShot(int(section.duration * 1000), this, [this, index, token, keepPlaying, section]() {
        if (m_sectionTimers.value(index) != token || keepPlaying || !m_decks[index].playing)
            return;
        if (std::abs(m_decks[index].position(now()) - section.start - section.duration) < 2)
            pause(index);
    });
}

void DjModel::clearSection(int slot)
{
    if (slot < 0 || slot >= m_sections.size())
        return;
    m_sections[slot].reset();
    m_sectionCursor = slot;
    for (DjDeck &deck : m_decks) {
        if (deck.sample.kind == DjSampleStepKind::Armed && deck.sample.slot == slot)
            deck.sample = {};
    }
    save();
    emitChanged();
}

void DjModel::playPad(const QString &name)
{
    m_player->pad(name);
}

void DjModel::setSamplerLevel(double level)
{
    m_samplerLevel = std::clamp(level, 0.0, 1.0);
    m_player->setSamplerLevel(m_samplerLevel);
    save();
    emitChanged();
}

void DjModel::stopAll()
{
    setAutomix(false);
    for (int i = 0; i < 4; ++i) {
        if (m_decks[i].playing || m_decks[i].keepsPlaying)
            pause(i);
    }
}

void DjModel::resetAll()
{
    setAutomix(false);
    ++m_searchGeneration;
    m_sectionTimers.clear();
    m_player->reset();
    const double at = now();
    for (int i = 0; i < 4; ++i) {
        cacheWave(i, true);
        DjDeck fresh;
        fresh.index = i;
        fresh.assign = i % 2;
        fresh.tickAt = at;
        m_decks[i] = fresh;
    }
    m_crossfader = 0;
    m_master = 1;
    m_curve = 0;
    m_samplerLevel = 0.8;
    m_player->setSamplerLevel(m_samplerLevel);
    for (auto &section : m_sections)
        section.reset();
    m_sectionCursor = 0;
    m_searchResults.clear();
    m_searching = false;
    m_searchProblem.clear();
    save(true);
    show(QStringLiteral("Studio reset. Load a song on each deck to start fresh."));
    for (int i = 0; i < 4; ++i)
        emitDeck(i);
    emitChanged();
    if (searchChanged)
        searchChanged();
}

void DjModel::handlePcm(int index, const QString &id, double at, const QByteArray &data)
{
    DjDeck &deck = m_decks[index];
    if (deck.videoId != id || data.size() < 2 || !std::isfinite(at) || at < 0)
        return;
    const qsizetype count = data.size() / 2;
    const qsizetype start = qsizetype(std::llround(at * pcmRate));
    const qsizetype limit = qsizetype(longestPcm * pcmRate);
    if (start >= limit)
        return;
    const qsizetype end = std::min(limit, start + count);
    if (deck.pcm.size() < end) {
        const qsizetype wanted = std::max<qsizetype>(end, qsizetype(deck.duration * pcmRate) + pcmRate);
        deck.pcm.resize(std::min(limit, wanted));
    }
    const auto *raw = reinterpret_cast<const uchar *>(data.constData());
    for (qsizetype i = 0; i < end - start; ++i)
        deck.pcm[start + i] = qint16(raw[2 * i] | (raw[2 * i + 1] << 8));
    rebuildBars(index, start, end);
    deck.waveDirty = true;
    if (deck.bpm <= 0 && !deck.bpmQueued) {
        int run = 0;
        int best = 0;
        for (char on : deck.heard) {
            run = on ? run + 1 : 0;
            best = std::max(best, run);
        }
        if (best >= barsPerSecond * 25)
            detectBpm(index, true);
    }
}

void DjModel::rebuildBars(int index, qsizetype from, qsizetype to)
{
    DjDeck &deck = m_decks[index];
    const double perBar = double(pcmRate) / barsPerSecond;
    const int first = int(from / perBar);
    const int last = int(std::ceil(to / perBar));
    if (deck.bars.size() < last) {
        deck.bars.resize(last);
        deck.heard.resize(last, 0);
    }
    const double lowAlpha = 1 - std::exp(-2 * M_PI * 250.0 / pcmRate);
    const double highAlpha = 1 - std::exp(-2 * M_PI * 2500.0 / pcmRate);
    for (int bar = first; bar < last; ++bar) {
        const qsizetype a = qsizetype(bar * perBar);
        const qsizetype b = std::min<qsizetype>(qsizetype((bar + 1) * perBar), deck.pcm.size());
        if (a >= b || a < from - qsizetype(perBar) || b > to + qsizetype(perBar))
            continue;
        if (a < from || b > to) {
            if (!(bar < deck.heard.size() && deck.heard.at(bar)) && (b - std::max(a, from)) < (b - a) / 2)
                continue;
        }
        double sum = 0;
        double lowSum = 0;
        double highSum = 0;
        double low = 0;
        double smooth = 0;
        for (qsizetype i = a; i < b; ++i) {
            const double x = deck.pcm.at(i) / 32768.0;
            sum += x * x;
            low += lowAlpha * (x - low);
            smooth += highAlpha * (x - smooth);
            const double high = x - smooth;
            lowSum += low * low;
            highSum += high * high;
        }
        const double n = double(b - a);
        const double rms = std::sqrt(sum / n);
        const double total = sum;
        const double lowShare = total > 0 ? std::min(1.0, lowSum / total) : 0;
        const double highShare = total > 0 ? std::min(1.0, highSum / total) : 0;
        DjWaveBar &out = deck.bars[bar];
        out.level = rms;
        out.low = lowShare;
        out.high = highShare;
        out.mid = std::max(0.0, 1 - lowShare - highShare);
        deck.heard[bar] = 1;
        deck.loudest = std::max(deck.loudest, rms);
    }
}

void DjModel::cacheWave(int index, bool force)
{
    DjDeck &deck = m_decks[index];
    if (!deck.loaded() || (!deck.waveDirty && !force) || deck.heard.isEmpty() || m_host.dataDir.isEmpty())
        return;
    if (!deck.waveDirty)
        return;
    deck.waveDirty = false;
    deck.waveSaved = now();
    QByteArray bytes;
    bytes.reserve(deck.bars.size() * 4);
    for (int i = 0; i < deck.bars.size(); ++i) {
        const DjWaveBar &bar = deck.bars.at(i);
        const bool on = i < deck.heard.size() && deck.heard.at(i);
        bytes.append(char(on && deck.loudest > 0 ? std::clamp(int(std::lround(bar.level / deck.loudest * 254)), 0, 254) : 255));
        bytes.append(char(std::clamp(int(std::lround(bar.low * 255)), 0, 255)));
        bytes.append(char(std::clamp(int(std::lround(bar.mid * 255)), 0, 255)));
        bytes.append(char(std::clamp(int(std::lround(bar.high * 255)), 0, 255)));
    }
    QJsonArray cues;
    for (double cue : deck.hotcues)
        cues.append(cue);
    const QJsonObject object{{QStringLiteral("v"), 1},
                             {QStringLiteral("duration"), deck.duration},
                             {QStringLiteral("bpm"), deck.bpm},
                             {QStringLiteral("loudest"), deck.loudest},
                             {QStringLiteral("hotcues"), cues},
                             {QStringLiteral("bars"), QString::fromLatin1(bytes.toBase64())}};
    const QString dir = m_host.dataDir + QStringLiteral("/waveforms");
    QDir().mkpath(dir);
    QSaveFile file(dir + QLatin1Char('/') + deck.videoId + QStringLiteral(".json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void DjModel::readWave(int index)
{
    DjDeck &deck = m_decks[index];
    if (m_host.dataDir.isEmpty() || !studio::isVideoId(deck.videoId))
        return;
    QFile file(m_host.dataDir + QStringLiteral("/waveforms/") + deck.videoId + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    const QByteArray bytes = QByteArray::fromBase64(object.value(QStringLiteral("bars")).toString().toLatin1());
    const int count = int(bytes.size() / 4);
    deck.bars.resize(count);
    deck.heard.resize(count, 0);
    for (int i = 0; i < count; ++i) {
        const auto level = uchar(bytes.at(4 * i));
        DjWaveBar &bar = deck.bars[i];
        bar.level = level == 255 ? 0 : level / 254.0;
        bar.low = uchar(bytes.at(4 * i + 1)) / 255.0;
        bar.mid = uchar(bytes.at(4 * i + 2)) / 255.0;
        bar.high = uchar(bytes.at(4 * i + 3)) / 255.0;
        deck.heard[i] = level == 255 ? 0 : 1;
    }
    deck.loudest = 1;
    if (deck.bpm <= 0)
        deck.bpm = clampDouble(object.value(QStringLiteral("bpm")).toDouble(), 0, 250, 0);
    const double duration = object.value(QStringLiteral("duration")).toDouble();
    if (duration > 0 && deck.duration <= 0)
        deck.duration = duration;
    const QJsonArray cues = object.value(QStringLiteral("hotcues")).toArray();
    bool any = false;
    for (double cue : deck.hotcues)
        any = any || cue >= 0;
    if (!any) {
        for (int i = 0; i < 8 && i < cues.size(); ++i)
            deck.hotcues[i] = cues.at(i).toDouble(-1);
    }
    updateLoopLength(index);
}

QJsonObject DjModel::savedState() const
{
    QJsonArray decks;
    const double at = now();
    for (const DjDeck &deck : m_decks) {
        QJsonArray cues;
        for (double cue : deck.hotcues)
            cues.append(cue);
        QJsonArray eq;
        for (double band : deck.eq)
            eq.append(band);
        QJsonArray fx;
        for (double amount : deck.fx)
            fx.append(amount);
        decks.append(QJsonObject{{QStringLiteral("videoId"), deck.videoId},
                                 {QStringLiteral("title"), deck.title},
                                 {QStringLiteral("author"), deck.author},
                                 {QStringLiteral("cue"), deck.cue},
                                 {QStringLiteral("level"), deck.level},
                                 {QStringLiteral("rate"), deck.rate},
                                 {QStringLiteral("bpm"), deck.bpm},
                                 {QStringLiteral("repeats"), deck.repeats},
                                 {QStringLiteral("keylock"), deck.keylock},
                                 {QStringLiteral("position"), deck.position(at)},
                                 {QStringLiteral("playing"), deck.keepsPlaying},
                                 {QStringLiteral("pitchRange"), deck.pitchRange},
                                 {QStringLiteral("loopSize"), deck.loopSize},
                                 {QStringLiteral("hotcues"), cues},
                                 {QStringLiteral("gain"), deck.gainDb},
                                 {QStringLiteral("eq"), eq},
                                 {QStringLiteral("filter"), deck.filter},
                                 {QStringLiteral("fx"), fx},
                                 {QStringLiteral("assign"), deck.assign},
                                 {QStringLiteral("remaining"), deck.showsRemaining}});
    }
    QJsonArray sections;
    for (const auto &section : m_sections) {
        if (section)
            sections.append(QJsonObject{{QStringLiteral("deck"), section->deck},
                                        {QStringLiteral("start"), section->start},
                                        {QStringLiteral("duration"), section->duration}});
        else
            sections.append(QJsonValue());
    }
    QJsonArray history;
    for (const DjHistoryItem &item : m_history)
        history.append(studio::historyJson(item));
    QJsonArray library;
    for (const DjPlayedSong &song : m_library)
        library.append(studio::playedJson(song));
    QJsonArray queue;
    for (const DjQueueItem &item : m_queue)
        queue.append(item.toJson());
    QJsonArray played;
    for (const QString &id : m_autoDjPlayed)
        played.append(id);
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("decks"), decks},
            {QStringLiteral("fourDecks"), m_fourDecks},
            {QStringLiteral("crossfader"), m_crossfader},
            {QStringLiteral("master"), m_master},
            {QStringLiteral("curve"), m_curve},
            {QStringLiteral("samplerLevel"), m_samplerLevel},
            {QStringLiteral("sections"), sections},
            {QStringLiteral("history"), history},
            {QStringLiteral("library"), library},
            {QStringLiteral("queue"), queue},
            {QStringLiteral("automix"), m_automixOn},
            {QStringLiteral("autoDj"), m_automixOn && m_autoDjOn},
            {QStringLiteral("automixLength"), m_automixLength},
            {QStringLiteral("videoMode"), m_videoMode},
            {QStringLiteral("autoDjPlayed"), played},
            {QStringLiteral("jam"), m_jam ? m_jam->id : QString()}};
}

void DjModel::save(bool immediately)
{
    if (m_restoring)
        return;
    if (immediately) {
        m_saveTimer->stop();
        write();
        return;
    }
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

void DjModel::write()
{
    m_savedAt = now();
    if (m_host.dataDir.isEmpty())
        return;
    QDir().mkpath(m_host.dataDir);
    QSaveFile file(m_host.dataDir + QStringLiteral("/studio.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(savedState()).toJson(QJsonDocument::Compact));
    file.commit();
}

void DjModel::saveCrate()
{
    refreshSmart();
    if (!m_host.dataDir.isEmpty()) {
        QDir().mkpath(m_host.dataDir);
        QSaveFile file(m_host.dataDir + QStringLiteral("/crate.json"));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(m_crate.encoded());
            file.commit();
        }
    }
    emitLibrary();
}

void DjModel::flush()
{
    if (m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
        write();
    }
    for (int i = 0; i < 4; ++i)
        cacheWave(i, true);
}

void DjModel::restore()
{
    m_restoring = true;
    if (!m_host.dataDir.isEmpty()) {
        QFile crate(m_host.dataDir + QStringLiteral("/crate.json"));
        if (crate.open(QIODevice::ReadOnly))
            m_crate = DjCrate::decode(crate.readAll());
    }
    QJsonObject saved;
    if (!m_host.dataDir.isEmpty()) {
        QFile file(m_host.dataDir + QStringLiteral("/studio.json"));
        if (file.open(QIODevice::ReadOnly))
            saved = QJsonDocument::fromJson(file.readAll()).object();
    }
    m_fourDecks = saved.value(QStringLiteral("fourDecks")).toBool();
    m_crossfader = clampDouble(saved.value(QStringLiteral("crossfader")).toDouble(), -1, 1, 0);
    m_master = clampDouble(saved.value(QStringLiteral("master")).toDouble(1), 0, 1.25, 1);
    m_curve = std::clamp(saved.value(QStringLiteral("curve")).toInt(), 0, 2);
    m_samplerLevel = clampDouble(saved.value(QStringLiteral("samplerLevel")).toDouble(0.8), 0, 1, 0.8);
    m_automixLength = clampDouble(saved.value(QStringLiteral("automixLength")).toDouble(studio::automixSeconds), 2, 30, studio::automixSeconds);
    m_videoMode = saved.value(QStringLiteral("videoMode")).toString(QStringLiteral("mix"));
    const QJsonArray sections = saved.value(QStringLiteral("sections")).toArray();
    for (int i = 0; i < studio::sectionCount && i < sections.size(); ++i) {
        const QJsonObject object = sections.at(i).toObject();
        if (object.isEmpty())
            continue;
        DjSection section;
        section.deck = std::clamp(object.value(QStringLiteral("deck")).toInt(), 0, 3);
        section.start = clampDouble(object.value(QStringLiteral("start")).toDouble(), 0, 864000, 0);
        section.duration = clampDouble(object.value(QStringLiteral("duration")).toDouble(8), 1, 30, 8);
        m_sections[i] = section;
    }
    m_sectionCursor = 0;
    for (int i = 0; i < m_sections.size(); ++i) {
        if (!m_sections.at(i)) {
            m_sectionCursor = i;
            break;
        }
    }
    QSet<QString> seen;
    for (const QJsonValue &value : saved.value(QStringLiteral("history")).toArray()) {
        const auto item = studio::historyFromJson(value.toObject());
        if (item && !seen.contains(item->id) && m_history.size() < studio::historyLimit) {
            seen.insert(item->id);
            m_history.append(*item);
        }
    }
    seen.clear();
    if (saved.contains(QStringLiteral("library"))) {
        for (const QJsonValue &value : saved.value(QStringLiteral("library")).toArray()) {
            const auto song = studio::playedFromJson(value.toObject());
            if (song && !seen.contains(song->id) && m_library.size() < studio::libraryLimit) {
                seen.insert(song->id);
                m_library.append(*song);
            }
        }
    } else {
        for (const DjHistoryItem &item : m_history)
            m_library.append({item.id, item.title, item.author, 1, 0});
    }
    seen.clear();
    for (const QJsonValue &value : saved.value(QStringLiteral("queue")).toArray()) {
        const auto item = DjQueueItem::fromJson(value.toObject());
        if (item && !seen.contains(item->id) && m_queue.size() < studio::queueLimit) {
            seen.insert(item->id);
            m_queue.append(*item);
        }
    }
    const QJsonArray decks = saved.value(QStringLiteral("decks")).toArray();
    for (int i = 0; i < 4 && i < decks.size(); ++i) {
        const QJsonObject stored = decks.at(i).toObject();
        DjDeck &deck = m_decks[i];
        deck.cue = clampDouble(stored.value(QStringLiteral("cue")).toDouble(), 0, 864000, 0);
        deck.level = clampDouble(stored.value(QStringLiteral("level")).toDouble(1), 0, 1, 1);
        deck.pitchRange = clampDouble(stored.value(QStringLiteral("pitchRange")).toDouble(studio::pitchRange), 0.04, 0.5, studio::pitchRange);
        deck.rate = studio::clampRate(clampDouble(stored.value(QStringLiteral("rate")).toDouble(1), 0.25, 2, 1), deck.pitchRange);
        deck.bpm = clampDouble(stored.value(QStringLiteral("bpm")).toDouble(), 0, 250, 0);
        deck.repeats = stored.value(QStringLiteral("repeats")).toBool();
        deck.keylock = stored.value(QStringLiteral("keylock")).toBool();
        deck.loopSize = std::clamp(stored.value(QStringLiteral("loopSize")).toInt(7), 0, int(loopBeats().size()) - 1);
        deck.gainDb = clampDouble(stored.value(QStringLiteral("gain")).toDouble(), -12, 12, 0);
        const QJsonArray eq = stored.value(QStringLiteral("eq")).toArray();
        for (int b = 0; b < 3 && b < eq.size(); ++b)
            deck.eq[b] = clampDouble(eq.at(b).toDouble(), -1, 1, 0);
        deck.filter = clampDouble(stored.value(QStringLiteral("filter")).toDouble(), -1, 1, 0);
        const QJsonArray fx = stored.value(QStringLiteral("fx")).toArray();
        for (int s = 0; s < 3 && s < fx.size(); ++s)
            deck.fx[s] = clampDouble(fx.at(s).toDouble(0.5), 0, 1, 0.5);
        deck.assign = std::clamp(stored.value(QStringLiteral("assign")).toInt(i % 2), 0, 2);
        deck.showsRemaining = stored.value(QStringLiteral("remaining")).toBool(true);
        const QJsonArray cues = stored.value(QStringLiteral("hotcues")).toArray();
        for (int c = 0; c < 8 && c < cues.size(); ++c)
            deck.hotcues[c] = cues.at(c).toDouble(-1);
        const QString id = stored.value(QStringLiteral("videoId")).toString();
        if (studio::isVideoId(id)) {
            const double position = clampDouble(stored.value(QStringLiteral("position")).toDouble(deck.cue), 0, 864000, deck.cue);
            prepare(i, id, stored.value(QStringLiteral("title")).toString(), stored.value(QStringLiteral("author")).toString(),
                    position, true);
            deck.keepsPlaying = stored.value(QStringLiteral("playing")).toBool();
            deck.playing = false;
            deck.counted = deck.keepsPlaying || position > deck.cue + 1;
        }
        updateLoopLength(i);
    }
    for (const QJsonValue &value : saved.value(QStringLiteral("autoDjPlayed")).toArray())
        m_autoDjPlayed.append(value.toString());
    m_resumeAutomix = saved.value(QStringLiteral("automix")).toBool();
    m_resumeAutoDj = saved.value(QStringLiteral("autoDj")).toBool();
    m_resumeJam = saved.value(QStringLiteral("jam")).toString();
    m_player->setSamplerLevel(m_samplerLevel);
    refreshSmartNow();
    m_restoring = false;
}

void DjModel::setOnScreen(bool on)
{
    m_onScreen = on;
    if (on && !m_resumed) {
        m_resumed = true;
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            DjDeck &deck = m_decks[i];
            if (deck.keepsPlaying && deck.loaded() && !deck.playing && i < deckCount()) {
                play(i);
                any = true;
            }
        }
        if (any && m_resumeAutomix && !m_automixOn)
            setAutomix(true);
        if (any && m_resumeAutoDj && m_automixOn)
            setAutoDj(true, false);
        if (!m_resumeJam.isEmpty())
            resumeJam();
        else
            refreshJams();
    }
    if (on && inJam() && !m_polling)
        poll();
}

QString DjModel::diagnostics() const
{
    QStringList lines;
    const double at = now();
    lines << QStringLiteral("Video Mix DJ Studio (desktop) diagnostics");
    lines << QStringLiteral("WebEngine: %1, player ready: %2, scripts sent: %3")
                 .arg(DjPlayer::available() ? QStringLiteral("yes") : QStringLiteral("no"),
                      m_player && m_player->ready() ? QStringLiteral("yes") : QStringLiteral("no"))
                 .arg(m_player ? m_player->scriptsSent() : 0);
    lines << QStringLiteral("Decks shown: %1, crossfader %2, curve %3, master %4")
                 .arg(deckCount()).arg(m_crossfader, 0, 'f', 2).arg(m_curve).arg(m_master, 0, 'f', 2);
    lines << QStringLiteral("AUTOMIX %1%2, AUTO DJ %3%4, focus %5")
                 .arg(m_automixOn ? QStringLiteral("on") : QStringLiteral("off"),
                      m_automixing ? QStringLiteral(" (mixing)") : QString(),
                      m_autoDjOn ? QStringLiteral("on") : QStringLiteral("off"),
                      m_autoDjFinding ? QStringLiteral(" (finding)") : QString(), m_crate.label(m_crate.focus));
    for (int i = 0; i < 4; ++i) {
        const DjDeck &deck = m_decks[i];
        lines << QStringLiteral("Deck %1: %2 \"%3\" %4/%5 s, playing %6, intent %7, advancing %8, rate %9, bpm %10, routed %11, heard %12%, failed %13")
                     .arg(i + 1)
                     .arg(deck.videoId.isEmpty() ? QStringLiteral("empty") : deck.videoId, deck.title.left(60))
                     .arg(deck.position(at), 0, 'f', 1)
                     .arg(deck.duration, 0, 'f', 1)
                     .arg(deck.playing)
                     .arg(deck.keepsPlaying)
                     .arg(deck.advancing)
                     .arg(deck.effectiveRate(), 0, 'f', 3)
                     .arg(deck.bpm, 0, 'f', 1)
                     .arg(deck.routed)
                     .arg(int(deck.coverage() * 100))
                     .arg(deck.failed);
    }
    lines << QStringLiteral("Queue %1, history %2, library %3, saved songs %4, playlists %5, unplayable %6")
                 .arg(shownQueue().size()).arg(m_history.size()).arg(m_library.size())
                 .arg(m_crate.tracks.size()).arg(m_crate.playlists.size()).arg(m_unplayable.size());
    lines << QStringLiteral("Jam: %1").arg(m_jam ? m_jam->id + QStringLiteral(" (") + m_jam->myStatus + QLatin1Char(')') : QStringLiteral("none"));
    lines << QStringLiteral("Recent notices:");
    for (const DjNoticeEntry &entry : m_notices.mid(std::max<qsizetype>(0, m_notices.size() - 12)))
        lines << QStringLiteral("  %1 s ago: %2").arg(at - entry.at, 0, 'f', 1).arg(entry.text);
    return lines.join(QLatin1Char('\n'));
}

}
