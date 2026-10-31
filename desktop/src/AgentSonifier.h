#pragma once

// Listening to an agent: the run's raw transcript, played.
//
// Every event the CLI streams becomes one note. Work going out - a sentence,
// a file read, a shell command - climbs the scale; data coming back falls
// again, so one turn sounds like a phrase: the agent reaches for something and
// the answer settles underneath it. A run that finishes rings a cadence, the
// only figure in the piece that lands twice on the root, so "done" is
// unmistakable even from the next room.
//
// Several agents can be heard at once, and that is the constraint that shapes
// the music. Every voice draws from one pentatonic scale in one key, so any
// two notes that happen to sound together are consonant no matter what the
// agents are doing, and every note is quantised onto one shared eighth-note
// grid so two busy runs play in time with each other instead of smearing.
//
// Within that, each agent is its own player. Its instrument, register, stereo
// seat, home degree of the scale and whether it plays on or between the beats
// all come from the agent's own number and title, so #281 sounds like #281
// every time you switch it on and two runs are very unlikely to be confusable.
// The voice then moves with the work: every few turns its line steps up the
// scale, and the tokens it has burned fill its harmonics out, so a run you
// have been hearing since this morning is audibly further along than one that
// started a minute ago.
//
// Everything below the player is pure Qt Core - the scale, the event-to-note
// mapping and the mixer are all unit-testable with no audio device
// (tests/test_agent_sonifier.cpp). The player is the only part that needs one,
// and it has three backends: Qt Multimedia when the module is present, a raw
// PCM pipe into the system player when it is not, and a silent one the window
// tests run so the schedule can be checked without hardware.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>

class QProcess;
class QThread;
class QTimer;

namespace forkmesh::sonify {

inline constexpr int kSampleRate = 44100;
inline constexpr int kChannels = 2; // stereo: agents sit at different angles

// The grid every voice lands on: eighth notes at 100 BPM. Slow enough that a
// busy agent still gets one note per event, quick enough that the melody
// follows the work rather than trailing it.
inline constexpr double kSlotSeconds = 0.3;

// A note wanted while the grid is already this far ahead is dropped instead of
// queued. A tool that returns two hundred events in a second must not leave
// the music minutes behind the run it is describing.
inline constexpr double kMaxLeadSeconds = 1.8;

// D, as in D major pentatonic (D E F# A B). Low enough to leave three octaves
// of headroom above it for the agents stacked on top.
inline constexpr double kTonicHz = 146.832; // D3

// What an agent is doing shapes the note: how long it is held, how bright it
// is, how hard it is struck. It is the same gesture whoever plays it.
enum class Timbre {
    Mallet, // short wooden strike: the agent saying something
    Bell,   // ringing partials, slow decay: arrivals and the done cadence
    Glass,  // thin and bright: reading, searching
    Wood,   // dry, low, quick: shell commands
    Pad,    // soft and slow: thinking
    Chirp,  // swoops up into the note: something launched, something new
    Thud,   // a low drum, bent hard down: a build or an install grinding
    Trill,  // two pitches shaken fast: tests running, a retry
    Buzz,   // rough, bright, trembling: an error, a refusal
    Swell,  // breathed in backwards, rising: waiting, compacting
    Ping,   // near-pure, short and high: a listing, a seal
    Twang,  // a string sagging under the finger: git, a skill, a hook
};

// Which agent is playing it. Instruments are what make two runs tellable apart
// with your eyes shut: register and stereo seat separate them, but a marimba
// and a chime are different the moment they sound, even in unison. Each is a
// set of partials and an attack; the Timbre above then shapes whichever one
// this voice holds.
enum class Instrument {
    Marimba, // warm, wooden, quick
    Celesta, // bright, metallic, ringing
    Pluck,   // a string, many partials, dry
    Kalimba, // hollow and soft
    Chime,   // long inharmonic partials
    Reed,    // breathy, sustained, with a slow vibrato
};

// One sounded note, already placed on the mixer's clock.
struct Note {
    double startSeconds = 0; // on the mixer clock, not the wall clock
    double seconds = 0.3;    // how long it is held before its release
    double frequency = 0;    // Hz
    double gain = 0.2;       // 0..1, before the master mix
    double pan = 0.5;        // 0 hard left .. 1 hard right
    Timbre timbre = Timbre::Mallet;         // what the agent is doing
    Instrument instrument = Instrument::Marimba; // which agent is doing it
    // How much work this agent has done by the time this note sounds: 0 is a
    // run that has just started, 1 one that has been going for hours. Richer
    // partials and a longer ring, so a veteran run sounds fuller than a new one.
    double bloom = 0;
    // What the event was, hashed: the file, the command, the words. It shades
    // the note - brighter or darker, a longer or shorter ring, a little bend,
    // a stretched partial - so the same command always sounds the same and no
    // two different ones quite do. 0 is the plain instrument.
    quint32 seed = 0;

    bool isRest() const { return frequency <= 0 || seconds <= 0 || gain <= 0; }
};

// Where one agent sits in the mix, and what it sounds like. Chosen by the
// order sessions were switched on, so the first agent you listen to always
// sounds the same. Five things separate any two of the first six: the
// instrument, the register, the stereo seat, the degree of the scale their
// line is centred on, and whether they play on the beat or between the beats.
// That is enough to follow three or four runs at once by ear.
struct Voice {
    int index = 0; // the session this voice belongs to
    Instrument instrument = Instrument::Marimba;
    int octave = 0;        // registers above the tonic
    int degreeOffset = 0;  // the scale degree this voice calls home
    double pan = 0.5;      // stereo position
    double slotOffset = 0; // 0 on the beat, 0.5 between them
    double detune = 1.0;   // a few cents off centre, so unisons shimmer

    // What the run has spent so far, which is what makes an agent's voice move
    // over its life rather than stay a fixed tone: turns push its line up the
    // scale, tokens fill out its harmonics.
    int drift = 0;    // scale degrees added by the turns taken
    double bloom = 0; // 0..1 from the tokens burned

    int home() const { return degreeOffset + drift; }
};

// The voice for a session: derived from the agent's number and its title, so
// agent #281 "fix the worker" sounds the same every time you listen to it, and
// two different agents are very unlikely to sound alike. `salt` re-rolls it,
// which is how a collision between two agents being heard together is settled.
Voice voiceForSession(int sessionId, const QString &title, int salt = 0);

// Folds a run's usage into its voice: `turns` walks the line up the scale a
// degree at a time, `tokens` opens the instrument out.
void applyUsage(Voice *voice, int turns, qint64 tokens);

// "celesta, two octaves up, hard left" - what the Listen button says an agent
// sounds like, so several running at once can be told apart on the screen too.
QString voiceDescription(const Voice &voice);

// A voice's running position in the scale, carried between events: the melody
// walks rather than jumping, which is what makes it a tune and not an alarm.
struct Melody {
    int degree = 0;       // scale degrees above the voice's tonic
    double lastSlot = -1; // the grid slot this voice last used
    // The quarter-slot the voice's last grace note took. Grace notes are the
    // quiet ones between the beats: the log's bookkeeping, and the events the
    // melody had no room for. They never move the melody.
    double lastGrace = -1;
    // How many notes this voice has played, melodic and grace: what stirs the
    // choice of step and ornament, so the same kind of event twice running
    // does not ring the same two pitches back and forth all afternoon.
    int count = 0;
    int graceCount = 0;
    int graceRun = 0; // grace notes back to back: a flurry thins out as it goes
    // The network is sampled a few times a second; heard that often at one
    // pitch it becomes a drone. Each direction sounds at most every
    // kNetworkSpacing seconds and carries the bytes held since.
    double lastSent = -1e9;
    double lastReceived = -1e9;
    qint64 heldSent = 0;
    qint64 heldReceived = 0;
};

// The shortest gap between two network notes of one direction for one voice.
inline constexpr double kNetworkSpacing = 1.2;
// How long a packet takes to cross the activity window's field. A received
// packet's note waits this long, so it sounds as the packet lands on the body.
inline constexpr double kPacketFlightSeconds = 1.1;

// Scale degree (may be negative, and wraps octaves every five steps) to Hz.
double degreeFrequency(int degree, const Voice &voice);

// What an event does to the melody. Outgoing work rises, returning data falls.
enum class Motion {
    Rest,     // nothing worth a note
    Start,    // the run announcing itself
    Speak,    // the agent writing prose
    Think,    // reasoning, under its breath
    Read,     // reading a file, fetching a page
    Search,   // grep, glob, find: looking through the tree
    Web,      // a page fetched, the web searched
    Write,    // an edit landing
    Run,      // a shell command
    Delegate, // handing work to another agent or skill
    Plan,     // a todo list written, a plan proposed
    Arrive,   // a tool result coming back
    Prompt,   // you, steering it
    Notice,   // a local warning, a protocol error, a refusal
    Finish,   // the turn's result: the phrase landing back on the root
    Scan,     // a folder listed, a glob expanded
    Create,   // a new file written from nothing
    Test,     // a test suite run
    Build,    // a compile, a bundle, a make
    Git,      // git or gh: history read, a commit, a push
    Install,  // a package manager fetching dependencies
    Spawn,    // a shell command sent to the background
    Wait,     // polling background work, sleeping on it
    Ask,      // the agent asking you a question
    Skill,    // a skill called in
    Mcp,      // an MCP server's tool
    Todo,     // the todo list or task list updated
    Fumble,   // a tool call that came back as an error
    Denied,   // a tool call refused: permission, a hook, the user
    Image,    // a picture coming back: a screenshot, an image read
    Compact,  // the context compacted to make room
    Retry,    // an API call failed and is being retried
    Fail,     // the turn ended in an error
    // The log's bookkeeping, each kind its own grace note - never a tune, and
    // never waking the agent, but never lumped together either.
    Frame,    // a message or content block opening and closing in the stream
    Compose,  // a tool call's arguments arriving piece by piece
    Usage,    // token counts and rate limits being reported
    System,   // hooks, status changes, compaction, retries
    Tick,     // anything else the log records: heartbeats, control frames
    Close,    // a message or content block closing in the stream
    Seal,     // a thinking block's signature, a citation: sealing what was said
    Hook,     // a hook starting or answering
    Network,  // bytes crossing the network. Never returned by motionForEvent:
              // the sampler, not the transcript, sounds it.
    // The rest of the field, which no transcript carries: the sampler seeing
    // a host for the first time, the desktop's background work, and every
    // line of its own log, one kind per family of the Logs page's badges
    // (motionForLogBadge). None is ever returned by motionForEvent either.
    Connect,    // a host joins the field: the first bytes to somewhere new
    Task,       // the desktop's background work starting or finishing
    LogError,   // ERROR
    LogStall,   // STALL, BGBLOCK: the interface held up, work turned away
    LogSync,    // MIRROR, SYNC, PIN, PEER, NODE, FORKED, STATUS: the mesh
    LogGit,     // GIT, PULL, MERGE, PUBLISH, ISSUE, BOUNTY, ACTIONS
    LogCloud,   // CLOUD, NETWORK, DB and the fleet nodes' groups
    LogTask,    // BGTASK, BGNOTE: background work reported
    LogAccount, // SESSION, ACCOUNT, IDENTITY, CRYPTO, ADMIN, PROMPT
    LogInfo,    // INFO, and any badge nobody has given a family yet
    // Who else is on the relay with this desktop, and what crosses the socket
    // to them. Cues too, never returned by motionForEvent.
    Visitor,    // a web visitor arriving or leaving: a browser on the site
    Client,     // another client joining or leaving: a desktop node, a phone
    Socket,     // a websocket frame crossing the relay: chat, presence, typing
};

// Every Motion, in order, so a caller that has to cover all of them - the
// visualiser's colour table - can be checked against this list rather than
// against a number someone has to remember to change.
inline constexpr Motion kMotions[] = {
    Motion::Rest,   Motion::Start,  Motion::Speak,    Motion::Think,
    Motion::Read,   Motion::Search, Motion::Web,      Motion::Write,
    Motion::Run,    Motion::Delegate, Motion::Plan,   Motion::Arrive,
    Motion::Prompt, Motion::Notice,
    Motion::Finish, Motion::Scan,   Motion::Create,   Motion::Test,
    Motion::Build,  Motion::Git,    Motion::Install,  Motion::Spawn,
    Motion::Wait,   Motion::Ask,    Motion::Skill,    Motion::Mcp,
    Motion::Todo,   Motion::Fumble, Motion::Denied,   Motion::Image,
    Motion::Compact, Motion::Retry, Motion::Fail,
    Motion::Frame,  Motion::Compose,  Motion::Usage,
    Motion::System, Motion::Tick,   Motion::Close,    Motion::Seal,
    Motion::Hook,   Motion::Network,
    Motion::Connect, Motion::Task,  Motion::LogError, Motion::LogStall,
    Motion::LogSync, Motion::LogGit, Motion::LogCloud, Motion::LogTask,
    Motion::LogAccount, Motion::LogInfo,
    Motion::Visitor, Motion::Client, Motion::Socket,
};
inline constexpr int kMotionCount = int(std::size(kMotions));

Motion motionForEvent(const QJsonObject &event);

// A stable hash of what an event is about - the tool and its target, the
// command, the first words of a sentence or a prompt - never 0. The same file
// read twice hashes the same; a different file does not. Both the note and the
// mark are varied by it, so the sound and the picture shade together.
quint32 eventSeed(const QJsonObject &event);
// 0..1 from byte `index` (0-3) of a seed: one independent knob per use.
inline double seedUnit(quint32 seed, int index)
{
    return double((seed >> (8 * (index & 3))) & 0xffu) / 255.0;
}

// The log's bookkeeping - Frame, Compose, Usage, System, Tick, Close, Seal,
// Hook: always a grace note, and never work (it does not wake a body, join two
// agents or write a line of the feed).
inline constexpr bool isBookkeeping(Motion motion)
{
    return motion == Motion::Frame || motion == Motion::Compose || motion == Motion::Usage
           || motion == Motion::System || motion == Motion::Tick || motion == Motion::Close
           || motion == Motion::Seal || motion == Motion::Hook;
}
// What a shell command is, from its text: a test run, a build, git, an
// install - or plain Run. `background` is a command sent off to run on its own.
Motion motionForCommand(const QString &command, bool background = false);
// Bookkeeping and the network: the small marks and the quiet notes.
inline constexpr bool isFaint(Motion motion)
{
    return isBookkeeping(motion) || motion == Motion::Network;
}

// A line of the desktop's own log, by the badge the Logs page gives it.
inline constexpr bool isLog(Motion motion)
{
    return motion >= Motion::LogError && motion <= Motion::LogInfo;
}
// Who is connected to the relay, and the frames crossing to them.
inline constexpr bool isRelay(Motion motion)
{
    return motion == Motion::Visitor || motion == Motion::Client || motion == Motion::Socket;
}
// What the field hears outside any transcript: the log, a new host, the
// desktop's background work, the relay's clients and frames. Played by
// AgentSonifier::handleCue, always as grace notes, never moving a melody.
inline constexpr bool isCue(Motion motion)
{
    return isLog(motion) || isRelay(motion) || motion == Motion::Connect
           || motion == Motion::Task;
}
// The kind a Logs page badge sounds as: "ERROR" is LogError, "MIRROR" and
// "PEER" are LogSync, and so on. Anything unknown is LogInfo.
Motion motionForLogBadge(const QString &badge);

// Roughly how much text an event carries, capped so a huge tool result costs
// no more to weigh than a small one. Louder notes mean more data moved.
int payloadBytes(const QJsonObject &event);

// The next note for this voice, quantised onto the shared grid. Every event in
// the log sounds: work takes the next slot of the melody; bookkeeping (Tick),
// and work arriving faster than the melody can take it, becomes a grace note -
// quiet, short, on the next free quarter of a slot, leaving the melody where
// it was. A rest only for an event with no type at all, or when even the
// grace notes are a whole lead ahead of the run. Advances the melody only when
// a melodic note is produced.
Note noteForEvent(const QJsonObject &event, const Voice &voice, Melody *melody,
                  double nowSeconds);

// Bytes crossing the network, as a little packet sound: a high chirp as data
// leaves, a lower ping as data lands - kPacketFlightSeconds later, when the
// activity window's packet reaches the body - a touch louder the more it
// moved. A rest when the voice's grace notes are already a lead ahead.
Note networkNote(const Voice &voice, Melody *melody, double nowSeconds, bool sent,
                 qint64 bytes);

// The one voice for what belongs to no agent: the desktop's own log, its
// background work and its own traffic. A low chime dead centre, under the
// agents' registers, so it is the floor the fleet plays over.
Voice desktopVoice();

// A cue (isCue) as a grace note: each kind its own figure - an error a rough
// buzz falling, a sync a glass note, a new host a chirp swooping up - shaded
// by `seed` (what the line said, which host) so two different lines of one
// kind never sound quite alike. A rest when the grace notes are a lead ahead.
Note cueNote(Motion motion, const Voice &voice, Melody *melody, double nowSeconds,
             quint32 seed);

// The figure that says a run is over: a rising arpeggio resolving onto the
// root for a clean finish, a falling one sinking onto it for a failure or a
// stop. Louder and longer than anything the stream itself plays.
QVector<Note> doneCadence(const Voice &voice, bool success, double startSeconds);

// The notes in flight, and the samples they add up to. Locked, because the
// audio backend renders on its own thread while the UI thread schedules.
class Mixer
{
public:
    // False when the note was a rest or the mixer is full and dropped it.
    bool addNote(const Note &note);
    void addNotes(const QVector<Note> &notes);
    void clear();

    // How much of the dotted ping-pong echo is mixed back in. The room is what
    // makes an agent's sparse events a piece of music rather than a row of
    // beeps, but it is exactly wrong for an interface sound: a click wants to
    // be over when the mouse is, not repeat twice behind itself. 0 is dry.
    void setEchoDepth(double depth);

    // Seconds of audio rendered so far: the clock every note is placed on.
    double now() const;
    // Notes still ringing or still waiting for their slot.
    int pendingNotes() const;
    bool idle() const { return pendingNotes() == 0; }

    // Interleaved stereo int16, `frames` frames. Always fills the buffer -
    // silence when there is nothing to play - and never clips.
    void render(qint16 *out, int frames);

private:
    mutable QMutex m_mutex;
    qint64 m_frame = 0;
    QVector<Note> m_notes;
    double m_echoDepth = 1.0;
    // A dotted-note ping-pong echo, allocated on the first render: what turns
    // a handful of events into a room rather than a row of beeps.
    std::vector<float> m_echoLeft;
    std::vector<float> m_echoRight;
    int m_echoIndex = 0;
};

// The audio device, when there is one. Pulls from the mixer in real time.
//
// Rendering is arithmetic on every sample, and a fleet of agents can have
// dozens of notes ringing at once, so none of it happens on the UI thread:
// Qt Multimedia pulls on its own audio thread, and the pipe backend renders on
// a thread of its own. The mixer is locked; the UI thread only ever adds notes.
class Player : public QObject
{
public:
    enum class Backend {
        None,  // nothing available: listening would be silent, so it is refused
        Qt,    // QAudioSink, when Qt Multimedia is built in
        Pipe,  // raw PCM into paplay/aplay
        Silent // renders and throws away: the window tests
    };

    explicit Player(Mixer *mixer, QObject *parent = nullptr);
    ~Player() override;

    bool start();
    void stop();
    bool isRunning() const;
    Backend backend() const { return m_backend; }
    QString backendName() const;
    // Roughly how far behind the mixer's clock the speaker is: the pump stays
    // one buffer ahead of the wall clock and the device holds a little more.
    // What the picture adds so a mark lands when its note is heard, not when
    // it was rendered.
    double latencySeconds() const;

    // The system player and the arguments that make it read raw PCM from
    // stdin in this mixer's format. Empty when the program is not one we know.
    static QStringList pipeArgumentsFor(const QString &program);
    // The candidates, best first.
    static QStringList pipePrograms();
    static QStringList availablePipePrograms();
    static void warmPipePrograms();

private:
    void shutdown(bool waitForPump);

    Mixer *m_mixer = nullptr;
    Backend m_backend = Backend::None;
    bool m_running = false;
    QStringList m_pipePrograms;
    std::shared_ptr<std::atomic<int>> m_opened;
    class Pump;          // the render loop and the player process it feeds
    Pump *m_pump = nullptr; // lives on m_thread, not here
    QThread *m_thread = nullptr;
    struct QtBackend; // QAudioSink + its pull device, when compiled in
    std::unique_ptr<QtBackend> m_qt;
};

// What the window drives: which sessions are being listened to, the voice each
// was given, and the one player they all share.
class AgentSonifier : public QObject
{
public:
    explicit AgentSonifier(QObject *parent = nullptr);
    ~AgentSonifier() override;

    // Adds a session to the mix, opening the device on the first one. False
    // means there is no way to make sound here and nothing was started.
    bool listen(int sessionId, const QString &title = QString());
    void stopListening(int sessionId);
    void stopAll();
    bool isListening(int sessionId) const { return m_voices.contains(sessionId); }
    int listenerCount() const { return m_voices.size(); }
    QList<int> listeningSessions() const { return m_voices.keys(); }
    Voice voiceFor(int sessionId) const;
    // What this agent sounds like, in words, or empty if it is not being heard.
    QString voiceDescriptionFor(int sessionId) const;

    // The run's usage so far. Cheap, and safe to call on a timer: it only
    // touches the voice when the figures have actually moved.
    void updateUsage(int sessionId, int turns, qint64 tokens);

    // One streamed transcript event, for a session that may or may not be
    // being listened to. Cheap and safe to call for every event of every run.
    void handleEvent(int sessionId, const QJsonObject &event);
    // The run reached a terminal state. Rings the cadence once, then keeps the
    // session in the mix - a follow-up turn on the same session plays on.
    void handleFinished(int sessionId, bool success);
    // A kind of note switched off: nothing of it is scheduled for any
    // session (the melody does not move for it either). Everything sounds by
    // default.
    void setMuted(Motion motion, bool muted);
    bool isMuted(Motion motion) const;

    // Bytes this session moved over the network since the last sample. The
    // activity window draws them as packets, and they sound as they leave and
    // land: in a heard agent's own voice, otherwise on the desktop voice while
    // everything is heard. Scene::kDesktop's own traffic plays on that voice.
    void handleNetwork(int sessionId, qint64 sent, qint64 received);

    // The desktop's voice, for everything no agent owns: its log, its
    // background work, its traffic. Opens the device like listen() does.
    static constexpr int kDesktop = -2; // the same id Scene::kDesktop uses
    bool listenDesktop();
    void stopDesktop();
    bool isListeningDesktop() const { return m_desktopOn; }
    // Whether handleCue would play this kind for this session: its own voice
    // if it is heard, the desktop's otherwise.
    bool hearsCue(int sessionId, Motion motion) const;
    // One cue (isCue): a log line, a new host, background work. It plays on
    // the agent's voice when that agent is heard - a line about #281 sounds
    // in #281's voice - and on the desktop's otherwise. The observer is told
    // which one, so the mark lands where the note sounds. False when nothing
    // was scheduled.
    bool handleCue(int sessionId, Motion motion, quint32 seed);

    QString backendName() const;
    int pendingNotes() const { return m_mixer.pendingNotes(); }

    // Told about every note this schedules for a session, with how many
    // seconds from now it will actually be heard. The activity window draws
    // from this, so a heard agent's marks land on its notes to the beat.
    using NoteObserver = std::function<void(int sessionId, Motion motion,
                                            const Note &note, double delaySeconds)>;
    void setNoteObserver(NoteObserver observer) { m_observer = std::move(observer); }
    Mixer &mixer() { return m_mixer; }

private:
    struct Listener {
        Voice voice;
        Melody melody;
        bool cadenceRung = false;
    };

    void schedule(int sessionId, Motion motion, const Note &note);
    Listener *listenerFor(int sessionId);
    void closeWhenQuiet();

    Mixer m_mixer;
    Listener m_desktop;
    bool m_desktopOn = false;
    std::unique_ptr<Player> m_player;
    QHash<int, Listener> m_voices;
    NoteObserver m_observer;
    bool m_muted[kMotionCount] = {};
};

} // namespace forkmesh::sonify
