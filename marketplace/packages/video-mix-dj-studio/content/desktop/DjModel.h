#pragma once

#include "DjAutoDj.h"
#include "DjCore.h"
#include "DjCrate.h"
#include "DjJam.h"
#include "DjPlayer.h"
#include "DjSmart.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include <array>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QTimer;

namespace forkmesh::djstudio {

struct DjEffect {
    int id = 0;
    DjReaction reaction;
    QString sender;
    double born = 0;
};

enum class DjFxSlot { Echo, Reverb, Flanger };

struct DjDeck {
    int index = 0;
    QString videoId;
    QString title;
    QString author;
    double duration = 0;
    double time = 0;
    double tickAt = 0;
    bool playing = false;
    bool keepsPlaying = false;
    bool advancing = false;
    bool counted = false;
    bool failed = false;
    bool routed = false;
    std::optional<double> doubtedEnd;
    double settleUntil = 0;
    double cue = 0;
    double level = 1;
    double rate = 1;
    double bend = 0;
    double brake = 0;
    double brakeStart = 0;
    double bpm = 0;
    double pitchRange = studio::pitchRange;
    bool loop = false;
    double loopStart = 0;
    double loopLength = 8;
    int loopSize = 7;
    bool repeats = false;
    bool keylock = false;
    bool scratching = false;
    bool resumeAfterScratch = false;
    double scratchStamp = 0;
    double scratchSpeed = 0;
    double scratchSent = 0;
    bool spinning = false;
    bool resumeAfterSpin = false;
    DjSampleStep sample;
    std::array<double, 8> hotcues{-1, -1, -1, -1, -1, -1, -1, -1};
    double gainDb = 0;
    std::array<double, 3> eq{0, 0, 0};
    std::array<bool, 3> kill{false, false, false};
    double filter = 0;
    std::array<double, 3> fx{0.5, 0.5, 0.5};
    std::array<bool, 3> fxOn{false, false, false};
    int assign = 0;
    double meter = 0;
    double meterHold = 0;
    bool showsRemaining = true;
    QVector<DjWaveBar> bars;
    double loudest = 0;
    QVector<qint16> pcm;
    QVector<char> heard;
    bool bpmQueued = false;
    bool waveDirty = false;
    double waveSaved = 0;
    DjSpin spin;
    DjTapTempo tap;

    bool loaded() const { return !videoId.isEmpty(); }
    double effectiveRate() const { return rate * (1 + bend) * (1 - brake); }
    double shownBpm() const { return bpm * effectiveRate(); }
    bool moving() const { return playing && !scratching; }
    double position(double now) const;
    double beatSeconds() const;
    double coverage() const;
};

struct DjNoticeEntry {
    QString text;
    double at = 0;
};

class DjModel : public QObject
{
public:
    static constexpr int pcmRate = 11025;
    static constexpr int barsPerSecond = 20;
    static constexpr double longestPcm = 15 * 60;
    static const QList<double> &loopBeats();

    struct Host {
        std::function<void(const QByteArray &, const QString &, const QJsonObject &,
                           std::function<void(bool, const QJsonObject &, const QString &)>)>
            request;
        QNetworkAccessManager *network = nullptr;
        std::function<QString()> accountName;
        std::function<QStringList()> userDirectory;
        QString dataDir;
    };

    DjModel(const Host &host, DjPlayer *player, QObject *parent = nullptr);
    ~DjModel() override;

    static double now();

    std::function<void()> changed;
    std::function<void(int)> deckChanged;
    std::function<void()> libraryChanged;
    std::function<void()> jamChanged;
    std::function<void(const QString &)> noticed;
    std::function<void()> searchChanged;
    std::function<void()> similarChanged;
    std::function<void()> effectsChanged;

    DjPlayer *player() const { return m_player; }
    DjDeck &deck(int index) { return m_decks[index]; }
    const DjDeck &deck(int index) const { return m_decks[index]; }
    int deckCount() const { return m_fourDecks ? 4 : 2; }
    bool fourDecks() const { return m_fourDecks; }
    void setFourDecks(bool on);

    double crossfader() const { return m_crossfader; }
    double master() const { return m_master; }
    double samplerLevel() const { return m_samplerLevel; }
    int crossfaderCurve() const { return m_curve; }
    void setCrossfaderCurve(int curve);
    bool automixOn() const { return m_automixOn; }
    bool automixing() const { return m_automixing; }
    bool autoDjOn() const { return m_autoDjOn; }
    bool autoDjFinding() const { return m_autoDjFinding; }
    double automixLength() const { return m_automixLength; }
    void setAutomixLength(double seconds);
    const QList<std::optional<DjSection>> &sections() const { return m_sections; }
    const QList<DjHistoryItem> &history() const { return m_history; }
    const QList<DjPlayedSong> &library() const { return m_library; }
    DjCrate &crate() { return m_crate; }
    const DjCrate &crate() const { return m_crate; }
    QList<DjQueueItem> shownQueue() const;
    const std::optional<DjJam> &jam() const { return m_jam; }
    const QList<DjJamSummary> &jams() const { return m_jams; }
    const QList<DjJamPresence> &crowd() const { return m_crowd; }
    bool inJam() const;
    int pendingInvites() const;
    bool jamBusy() const { return m_jamBusy; }
    QString jamProblem() const { return m_jamProblem; }
    const QList<DjEffect> &effects() const { return m_effects; }
    const QList<DjNoticeEntry> &notices() const { return m_notices; }
    QString videoMode() const { return m_videoMode; }
    void setVideoMode(const QString &mode);
    std::optional<DjAutoDjCandidate> autoDjNext() const;
    QString diagnostics() const;

    double audibleGain(int index) const;
    double crossfadeGain(int index) const;
    void refreshSound(int index);
    void applyMix();
    void frame(double now);
    double shown(int index, double at);
    double turned(int index, double at);

    void show(const QString &text);
    void load(int index, const QString &id, const QString &title = QString(), const QString &author = QString());
    bool loadLink(int index, const QString &text);
    void eject(int index);
    void start(int index);
    void togglePlay(int index);
    void pauseDeck(int index);
    void stop(int index);
    void restart(int index);
    void setCue(int index);
    void jumpToCue(int index);
    void cueStop(int index);
    void cuePress(int index);
    void seek(int index, double seconds);
    void beatJump(int index, double beats);
    void setRate(int index, double rate);
    void resetPitch(int index);
    void setPitchRange(int index, double range);
    void touchBegan(int index);
    void touchEnded(int index);
    void bend(int index, double direction);
    void sync(int index);
    void tapTempo(int index);
    void clearBpm(int index);
    void detectBpm(int index, bool quietly = false);
    void toggleLoop(int index);
    void loopIn(int index);
    void loopOut(int index);
    void setLoopSize(int index, int size);
    void loopRoll(int index, int size, bool on);
    void toggleRepeat(int index);
    void toggleKeylock(int index);
    void setLevel(int index, double level);
    void setGain(int index, double db);
    void setEq(int index, int band, double value);
    void toggleKill(int index, int band);
    void setFilter(int index, double value);
    void setFx(int index, int slot, double amount);
    void toggleFx(int index, int slot);
    void brakeDeck(int index);
    void backspin(int index);
    void setAssign(int index, int assign);
    void hotCue(int index, int pad);
    void deleteHotCue(int index, int pad);
    void scratchBegan(int index);
    void scratchMoved(int index, double radians);
    void scratchEnded(int index);
    void toggleRemaining(int index);
    void setCrossfader(double value, bool cancelAutomix = true, bool saving = true);
    void setMaster(double value);
    void slam(DjSide side);
    void toggleAutomix();
    void setAutomix(bool on);
    void mixNow();
    void quickNext(int index);
    void deleteSongOn(int index);
    void sample(int index);
    void playSection(int slot);
    void clearSection(int slot);
    void playPad(const QString &name);
    void setSamplerLevel(double level);
    void resetAll();
    void stopAll();

    void search(const QString &query);
    const QList<DjSearchResult> &searchResults() const { return m_searchResults; }
    bool searching() const { return m_searching; }
    QString searchProblem() const { return m_searchProblem; }
    void findSimilar(const QString &id);
    QString similarSeed() const { return m_similarSeed; }
    const QList<DjSearchResult> &similarResults() const { return m_similarResults; }
    bool similarLoading() const { return m_similarLoading; }
    QString similarProblem() const { return m_similarProblem; }
    QList<DjSimilarSong> similarSongs(const QString &id) const;

    DjTrack song(const QString &id) const;
    QList<DjTrack> librarySongs() const;
    int plays(const QString &id) const;
    double lastPlayed(const QString &id) const;
    void forgetPlayed(const QString &id);
    void clearHistory();
    void refreshSmart();
    void toggleShuffle(const QString &id);
    void addTags(const QString &raw, const QString &id);
    void toggleTag(const QString &name, const QString &id);
    void renameTag(const QString &old, const QString &raw);
    void deleteTag(const QString &name);
    bool inPlaylist(const QString &id, const QString &playlist) const;
    void toggleInPlaylist(const QString &id, const QString &playlist);
    std::optional<QString> newPlaylist(const QString &name, const QStringList &ids = {});
    void renamePlaylist(const QString &id, const QString &name);
    void deletePlaylist(const QString &id);
    void removeFromPlaylist(const QString &song, const QString &playlist);
    void movePlaylistSong(const QString &playlist, int from, int to);
    void removeSong(const QString &id);
    void fileDeck(int index, const DjFocus &target);
    void setShuffle(bool on);
    void focusAutomix(const DjFocus &focus, bool startMix);
    void queueAll(const DjFocus &focus, bool shuffled);
    void saveSmart(const QString &id);
    QString importData(const QByteArray &data, const QString &name);
    void saveCrate();

    void enqueue(const QString &id, const QString &title, const QString &author);
    void enqueueAll(const QList<DjTrack> &songs);
    void playFromQueue(const DjQueueItem &item, int index);
    void removeFromQueue(const DjQueueItem &item);
    void moveQueueItem(int from, int to);
    void clearQueue();

    void toggleAutoDj();
    void setAutoDj(bool on, bool refresh = true);

    void refreshJams();
    void resumeJam();
    void openJam(const QString &id);
    void startJam();
    void joinJam(const QString &id);
    void leaveJam();
    void invite(const QStringList &names);
    void removeMember(const QString &name);
    DjPersonStatus status(const DjJamMember &member) const;
    void react(const DjReaction &reaction);
    void setOnScreen(bool on);

    QImage artwork(const QString &id);
    void save(bool now = false);
    void flush();

private:
    void restore();
    void write();
    QJsonObject savedState() const;
    void handle(const QString &type, const QJsonObject &message);
    void handleTick(int index, double time, double duration, int state, double level);
    void handleState(int index, int state);
    void handleMeta(int index, const QString &title, const QString &author);
    void handleError(int index, int code);
    void handlePcm(int index, const QString &id, double at, const QByteArray &data);
    void handleSpun(int index, double at);
    void prepare(int index, const QString &id, const QString &title, const QString &author, double start,
                 bool keepSettings, bool playNow = false);
    void fail(int index);
    bool replay(int index);
    void aimRepeat(int index, double now);
    bool believes(int state, int index, double reported);
    void advance(bool advancing, int index);
    void applyState(int state, int index);
    void rewind(int index, double start = 0);
    void play(int index);
    void pause(int index);
    void settle(int index, double now);
    void rebase(int index);
    void remember(int index);
    void count(int index);
    void releaseBrake(int index);
    void sendRate(int index);
    void sendScratch(int index);
    void holdAutomix();
    void automixStep();
    bool preload(int index);
    std::optional<DjPlayedSong> replayPick();
    void refocusAutomix();
    bool skipOut(int index);
    void mix(int from, int to, double seconds, bool replacing = false);
    DjAutomixDeck snapshot(int index) const;
    void fade(double end, double seconds, std::function<void()> then);
    void stopFade();
    bool playNow(const QString &id, const QString &title, const QString &author);
    void updateLoopLength(int index);
    void cacheWave(int index, bool force = false);
    void readWave(int index);
    void rebuildBars(int index, qsizetype from, qsizetype to);
    void share(int index);
    DjJamDeck deckState(int index) const;
    void jamRequest(const QByteArray &method, const QString &path, const QJsonObject &body,
                    std::function<void(std::optional<QJsonObject>)> done);
    void receive(const std::optional<QJsonObject> &reply, double latency = 0);
    void clearJam();
    void adopt(const DjJam &fresh, double latency);
    void hear(const QList<DjJamReaction> &reactions, const QString &me);
    void follow(DjSide side, const DjJam &jam);
    void poll();
    void spawn(const DjReaction &reaction, const QString &from);
    void stopAutoDj();
    std::optional<DjTrack> autoDjSeedTrack() const;
    void primeAutoDj();
    std::optional<DjAutoDjCandidate> takeAutoDj();
    void findAutoDj(const DjTrack &seed);
    void refreshAutoDj();
    void fetchInstances(std::function<void()> done);
    void fetchInvidious(std::function<QUrl(const QUrl &)> address,
                        std::function<void(std::optional<QPair<QByteArray, QUrl>>)> done, int attempt = 0);
    void fetchUrl(const QUrl &url, int timeout, std::function<void(bool, const QByteArray &)> done);
    void refreshSmartNow();
    void emitChanged();
    void emitDeck(int index);
    void emitLibrary();

    Host m_host;
    DjPlayer *m_player = nullptr;
    std::array<DjDeck, 4> m_decks;
    bool m_fourDecks = false;
    double m_crossfader = 0;
    double m_master = 1;
    int m_curve = 0;
    double m_samplerLevel = 0.8;
    QList<std::optional<DjSection>> m_sections;
    int m_sectionCursor = 0;
    QList<DjHistoryItem> m_history;
    QList<DjPlayedSong> m_library;
    QList<DjQueueItem> m_queue;
    DjCrate m_crate;
    QSet<QString> m_unplayable;
    bool m_automixOn = false;
    bool m_automixing = false;
    double m_automixLength = studio::automixSeconds;
    QTimer *m_automixTimer = nullptr;
    QTimer *m_fadeTimer = nullptr;
    int m_fadeGeneration = 0;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_jamTimer = nullptr;
    double m_savedAt = 0;
    bool m_restoring = false;
    QString m_videoMode = QStringLiteral("mix");
    bool m_onScreen = false;
    bool m_resumed = false;
    bool m_resumeAutomix = false;
    bool m_resumeAutoDj = false;
    QString m_resumeJam;

    bool m_autoDjOn = false;
    bool m_autoDjFinding = false;
    bool m_autoDjOwnsAutomix = false;
    bool m_autoDjRefresh = false;
    QString m_autoDjSeed;
    QList<DjAutoDjCandidate> m_autoDjChoices;
    QStringList m_autoDjPlayed;
    int m_autoDjGeneration = 0;

    QList<QUrl> m_instances;
    QList<DjSearchResult> m_searchResults;
    bool m_searching = false;
    QString m_searchProblem;
    int m_searchGeneration = 0;
    QString m_similarSeed;
    QList<DjSearchResult> m_similarResults;
    bool m_similarLoading = false;
    QString m_similarProblem;
    int m_similarGeneration = 0;

    std::optional<DjJam> m_jam;
    QList<DjJamSummary> m_jams;
    QList<DjJamPresence> m_crowd;
    bool m_jamBusy = false;
    QString m_jamProblem;
    bool m_applyingRemote = false;
    QHash<int, qint64> m_applied;
    QHash<int, QTimer *> m_shareTimers;
    QSet<int> m_sharing;
    QStringList m_seenReactions;
    QList<double> m_sentReactions;
    bool m_polling = false;
    QList<DjEffect> m_effects;
    int m_effectId = 0;
    QList<DjNoticeEntry> m_notices;

    QHash<QString, QImage> m_art;
    QSet<QString> m_artLoading;
    QHash<int, int> m_sectionTimers;
    QHash<int, int> m_repeatTimers;
    QHash<int, int> m_rateTimers;
    QHash<int, int> m_brakeTimers;
    int m_timerGeneration = 0;
};

}
