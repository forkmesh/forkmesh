#pragma once

#include <tuple>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <cstdint>
#include <optional>

namespace forkmesh::djstudio {

enum class DjSide { A, B };

inline DjSide otherSide(DjSide side) { return side == DjSide::A ? DjSide::B : DjSide::A; }
inline QString sideLabel(DjSide side) { return side == DjSide::A ? QStringLiteral("A") : QStringLiteral("B"); }
inline QString sideKey(DjSide side) { return side == DjSide::A ? QStringLiteral("a") : QStringLiteral("b"); }

struct DjWaveBar {
    double level = 0;
    double low = 0;
    double mid = 0;
    double high = 0;
    bool operator==(const DjWaveBar &o) const { return std::tie(level, low, mid, high) == std::tie(o.level, o.low, o.mid, o.high); }
    bool operator!=(const DjWaveBar &o) const { return !(*this == o); }
};

struct DjSearchResult {
    QString id;
    QString title;
    QString author;
    double seconds = 0;
    bool operator==(const DjSearchResult &o) const { return std::tie(id, title, author, seconds) == std::tie(o.id, o.title, o.author, o.seconds); }
    bool operator!=(const DjSearchResult &o) const { return !(*this == o); }
};

struct DjHistoryItem {
    QString id;
    QString title;
    QString author;
    bool operator==(const DjHistoryItem &o) const { return std::tie(id, title, author) == std::tie(o.id, o.title, o.author); }
    bool operator!=(const DjHistoryItem &o) const { return !(*this == o); }
};

struct DjPlayedSong {
    QString id;
    QString title;
    QString author;
    int plays = 1;
    double last = 0;
    bool operator==(const DjPlayedSong &o) const { return std::tie(id, title, author, plays, last) == std::tie(o.id, o.title, o.author, o.plays, o.last); }
    bool operator!=(const DjPlayedSong &o) const { return !(*this == o); }
};

struct DjTrack {
    QString id;
    QString title;
    QString author;
    QStringList tags;
    bool noShuffle = false;
    bool operator==(const DjTrack &o) const { return std::tie(id, title, author, tags, noShuffle) == std::tie(o.id, o.title, o.author, o.tags, o.noShuffle); }
    bool operator!=(const DjTrack &o) const { return !(*this == o); }
    QString shownTitle() const { return title.isEmpty() ? QStringLiteral("YouTube video") : title; }
};

struct DjQueueItem {
    QString id;
    QString videoId;
    QString title;
    QString author;
    QString addedBy;
    qint64 addedAt = 0;
    bool operator==(const DjQueueItem &o) const { return std::tie(id, videoId, title, author, addedBy, addedAt) == std::tie(o.id, o.videoId, o.title, o.author, o.addedBy, o.addedAt); }
    bool operator!=(const DjQueueItem &o) const { return !(*this == o); }
    QJsonObject toJson() const;
    static std::optional<DjQueueItem> fromJson(const QJsonObject &object);
};

struct DjSection {
    int deck = 0;
    double start = 0;
    double duration = 8;
    bool operator==(const DjSection &o) const { return std::tie(deck, start, duration) == std::tie(o.deck, o.start, o.duration); }
    bool operator!=(const DjSection &o) const { return !(*this == o); }
};

struct DjRandom {
    explicit DjRandom(quint64 seed) : state(seed) {}
    quint64 next();
    double unit();
    quint64 state;
};

namespace studio {

inline constexpr double secondsPerTurn = 1.8;
inline constexpr double pitchRange = 0.16;
inline constexpr double bendAmount = 0.04;
inline constexpr double loopSeconds = 8;
inline constexpr double sectionSeconds = 8;
inline constexpr int sectionCount = 4;
inline constexpr double automixSeconds = 8;
inline constexpr double rateLow = 0.25;
inline constexpr double rateHigh = 2;
inline constexpr int historyLimit = 40;
inline constexpr int libraryLimit = 500;
inline constexpr int queueLimit = 100;
inline constexpr double touchBrake = 0.08;
inline constexpr double touchBrakeSeconds = 0.35;
inline constexpr double scratchThreshold = 14;
inline constexpr double shortestSample = 0.25;

bool isVideoId(const QString &value);
QString videoId(const QString &value);
QStringList sharedVideoIds(const QStringList &urls, const QString &text);
QUrl watchUrl(const QString &id);
QUrl thumbnailUrl(const QString &id, const QString &quality = QStringLiteral("hqdefault"));
QPair<double, double> gains(double crossfader);
QString clock(double seconds);
double clampRate(double rate, double range = pitchRange);
double syncedRate(double ownBpm, double otherBpm, double otherRate, double range = pitchRange);
double scratchSeconds(double radians);
double scratchSpeed(double previous, double seconds, double elapsed);
bool isScratch(double dx, double dy, double threshold = scratchThreshold);
std::optional<DjSection> sampleSection(int deck, double start, double end);
double trackedPosition(double predicted, double reported, bool continuous);
DjWaveBar amplitude(const QVector<DjWaveBar> &bars, double time, double duration);
QList<DjHistoryItem> remember(const QList<DjHistoryItem> &history, const QString &id,
                              const QString &title, const QString &author,
                              int limit = historyLimit);
QList<DjPlayedSong> played(const QList<DjPlayedSong> &library, const QString &id,
                           const QString &title, const QString &author, double now,
                           int limit = libraryLimit);
QList<DjPlayedSong> retitled(const QList<DjPlayedSong> &library, const QString &id,
                             const QString &title, const QString &author);
QString playsLabel(int plays);
QList<DjPlayedSong> mostPlayed(const QList<DjPlayedSong> &library, int limit = 25);
double turnDegrees(double seconds);
QList<QUrl> searchInstances(const QByteArray &data, int limit = 3);
QUrl searchUrl(const QUrl &instance, const QString &query);
QUrl similarUrl(const QUrl &instance, const QString &id);
QList<DjSearchResult> similarResults(const QByteArray &data);
QList<DjSearchResult> searchResults(const QByteArray &data);
QList<DjSearchResult> results(const QJsonArray &rows);

QJsonObject playedJson(const DjPlayedSong &song);
std::optional<DjPlayedSong> playedFromJson(const QJsonObject &object);
QJsonObject historyJson(const DjHistoryItem &item);
std::optional<DjHistoryItem> historyFromJson(const QJsonObject &object);

}

class DjTapTempo
{
public:
    std::optional<double> tap(double time);
    void clear() { m_taps.clear(); }
    int count() const { return int(m_taps.size()); }
    bool counting(double time) const { return !m_taps.isEmpty() && time - m_taps.last() <= 2; }

private:
    QVector<double> m_taps;
};

class DjSpin
{
public:
    static constexpr double spinUp = 0.09;
    static constexpr double spinDown = 0.18;
    static constexpr double follow = 0.2;
    static constexpr double jump = 0.6;
    static constexpr double gap = 0.1;
    static constexpr double hitch = 0.5;

    void hold(double target, double now);
    double step(double target, double speed, std::optional<double> motor, double now);
    double position() const { return m_position; }
    double speed() const { return m_speed; }
    double turn() const { return m_turn; }
    double motor() const { return m_motor; }

private:
    double m_position = 0;
    double m_speed = 0;
    double m_turn = 0;
    double m_motor = 0;
    std::optional<double> m_stamp;
};

struct DjAutomixDeck {
    bool loaded = false;
    bool playing = false;
    double position = 0;
    double duration = 0;
    double rate = 1;
    bool repeats = false;

    std::optional<double> remaining() const;
    bool fresh() const;
};

struct DjAutomixStep {
    enum class Kind { Wait, Preload, Start, Mix };
    Kind kind = Kind::Wait;
    DjSide side = DjSide::A;
    DjSide to = DjSide::B;
    double seconds = 0;
    bool operator==(const DjAutomixStep &o) const { return std::tie(kind, side, to, seconds) == std::tie(o.kind, o.side, o.to, o.seconds); }
    bool operator!=(const DjAutomixStep &o) const { return !(*this == o); }
};

namespace automix {

inline constexpr double lead = 2;
inline constexpr double audible = 3;

DjAutomixStep step(const DjAutomixDeck &a, const DjAutomixDeck &b, DjSide favoured, bool queued);
DjSide playNow(const DjAutomixDeck &a, const DjAutomixDeck &b, double crossfader, bool *none = nullptr);
std::optional<DjPlayedSong> next(const QList<DjPlayedSong> &library, const QSet<QString> &loaded,
                                 const QSet<QString> &playing, quint64 seed);

}

enum class DjEndReport { Ended, Doubted, Failed };

namespace end {

inline constexpr double slack = 5;
inline constexpr double patience = 3;

DjEndReport judge(double position, double duration, std::optional<double> doubtedSince,
                  bool intended, double now);

}

struct DjRepeatSpan {
    double start = 0;
    double end = 0;
    bool operator==(const DjRepeatSpan &o) const { return std::tie(start, end) == std::tie(o.start, o.end); }
    bool operator!=(const DjRepeatSpan &o) const { return !(*this == o); }
};

struct DjRepeatStep {
    enum class Kind { Wait, Wrap, After };
    Kind kind = Kind::Wait;
    double after = 0;
};

namespace repeat {

inline constexpr double quiet = 0.07;
inline constexpr double tail = 0.25;
inline constexpr double shortest = 2;
inline constexpr double ahead = 0.6;

std::optional<DjRepeatSpan> span(const QVector<DjWaveBar> &bars, double duration);
DjRepeatStep step(double position, const DjRepeatSpan &span, double rate);

}

enum class DjReactionStyle { Rise, Rain, Burst, Zoom, Spin, Orbit, Wave, Confetti, Fireworks, Flash };

struct DjReaction {
    QString id;
    QString emoji;
    QString name;
    DjReactionStyle style = DjReactionStyle::Rise;
    double hue = 0;
    double seconds() const;
    static const QList<DjReaction> &all();
    static const DjReaction *find(const QString &id);
};

enum class DjSampleStepKind { Idle, Recording, Armed };

struct DjSampleStep {
    DjSampleStepKind kind = DjSampleStepKind::Idle;
    double start = 0;
    int slot = 0;
};

enum class DjDrumPad { Kick, Snare, Hat, Clap, Tom, Perc, Riser, Stab };

QString drumPadKey(DjDrumPad pad);
QString drumPadLabel(DjDrumPad pad);
QList<DjDrumPad> drumPads();

struct DjSoundEffect {
    QString id;
    QString name;
    double seconds = 1;
};

const QList<DjSoundEffect> &soundEffects();

}
