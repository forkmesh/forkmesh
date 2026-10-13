#pragma once

// Announcer mode: somebody in the booth, calling the agents.
//
// Listen (AgentSonifier.h) turns one run into music. The announcer does the
// other thing you want from the next room: it watches every agent at once and
// says, out loud and in sentences, what is happening - who just started, who
// landed an edit, whose tests are running, who is waiting on you, who crossed
// the line. It talks like a sports caster because that is the register built
// for exactly this job: several competitors, continuous play, and a listener
// who is only half paying attention.
//
// Two halves, kept apart so the first can be tested with no audio at all:
//
// The Commentator is the writer. It is fed the same stream events the
// transcript gets plus a periodic look at each session's status and usage, and
// it keeps a short list of things worth saying. It is *pulled*, not pushed:
// the window asks for the next line only when the voice has finished the last
// one, and the commentator answers with the most important thing that is still
// true. A caster who falls behind does not read out the backlog, they skip to
// the play in front of them - so calls go stale and are dropped, a run of
// twenty file reads is one remark, and when the field goes quiet it fills with
// colour: the standings, who has gone silent, how long the leader has been out.
//
// The Speaker is the voice. It is Piper, a neural text-to-speech engine that
// runs entirely on this machine - nothing about what the agents are doing is
// sent anywhere to be spoken. One long-lived Piper process takes a line of
// text on stdin and streams raw PCM back, which is forwarded to the same
// system player the sonifier's pipe backend uses. The voice itself (the Piper
// binary and one voice model, ~90 MB) is fetched once, on request, by
// AgentAnnouncerVoice.h.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QProcess;
class QTimer;

namespace forkmesh::announce {

// ---------------------------------------------------------------------------
// Words. A speech engine reads what it is given, so what it is given has to
// be what a person would say: "two eighty-five", not "two hundred and
// eighty-five"; "Main Window Agents", not "MainWindowAgents dot c p p".

// A jersey number: 7 "seven", 42 "forty-two", 285 "two eighty-five",
// 205 "two oh five", 300 "three hundred", 1234 "twelve thirty-four".
QString spokenNumber(int number);
// A quantity, rounded the way a caster rounds: 950 "950", 45 200 "45 thousand",
// 1 240 000 "1.2 million".
QString spokenCount(qint64 count);
// "desktop/src/MainWindowAgents.cpp" -> "Main Window Agents".
QString spokenFileName(const QString &path);
// A session title cut down to something sayable in one breath.
QString spokenTitle(const QString &title, int maxWords = 6);
// 95 000 ms -> "a minute and a half"; 3 700 000 -> "just over an hour".
QString spokenDuration(qint64 milliseconds);
// Whatever the templates produced, made safe for one line of Piper's stdin:
// no newlines, no markup, no runs of punctuation it would read aloud.
QString speakable(const QString &line);
// An agent's reply (markdown) as something to read out: code blocks and
// tables left out, a link said by its words, a URL as "a link", a file path
// by its name, each list item and heading its own sentence, and cut at the
// end of a sentence near `maxChars`. Still to go through speakable().
QString spokenProse(const QString &markdown, int maxChars = 700);

// ---------------------------------------------------------------------------
// Plays: what one stream event amounts to, in the caster's vocabulary.

enum class PlayKind {
    None,
    KickOff,  // the run announcing itself
    Edit,     // a file written or changed
    Test,     // a test suite started
    Build,    // a compile started
    Commit,   // git commit
    Push,     // git push
    Shell,    // any other command
    Read,     // reading files
    Search,   // grep, glob
    Web,      // fetching or searching the web
    Delegate, // a sub-agent or skill called in
    Think,    // reasoning
    Speak,    // writing prose
    Coach,    // you, steering it
    Fumble,   // a tool call that came back as an error
};

struct Play {
    PlayKind kind = PlayKind::None;
    QString detail; // the file, already speakable; empty when there is none
};

Play playForEvent(const QJsonObject &event);
// What a shell command is, from its text: tests, a build, a commit, a push.
PlayKind playForCommand(const QString &command);

// How much a call matters. Decides what is said first and how long it stays
// worth saying.
enum class Priority {
    Colour,    // filler for a quiet field; never queued, made on demand
    Play,      // routine work: edits, commands, reading
    Highlight, // tests, builds, commits, errors, milestones
    Headline,  // a run starting, finishing, failing, or waiting on you
};

// How long a call of each priority stays true enough to say.
qint64 shelfLifeMs(Priority priority);

struct Call {
    QString text;
    Priority priority = Priority::Play;
    PlayKind kind = PlayKind::None;
    int sessionId = -1;
    qint64 atMs = 0;
};

// What the window knows about one session, handed over on every tick.
struct AgentFacts {
    int id = 0;
    QString title;
    QString status; // AgentStatus strings: queued, running, waiting, success...
    int turns = 0;
    qint64 tokens = 0;
    qint64 runningSinceMs = 0;
};

class Commentator
{
public:
    Commentator();

    // Same seed, same events, same commentary: what the tests rely on.
    void setSeed(quint32 seed);

    // The session as it stands now. Status changes and usage milestones
    // become calls; the first sight of a session is recorded silently unless
    // the broadcast is already under way.
    void updateAgent(const AgentFacts &facts, qint64 nowMs);
    void forgetAgent(int sessionId);
    // One streamed transcript event from any session.
    void handleEvent(int sessionId, const QJsonObject &event, qint64 nowMs);

    // The next thing to say, or an empty text when silence is the right call.
    // Ask only when the voice is free: whatever is returned is taken off the
    // list and everything it made redundant goes with it.
    Call nextCall(qint64 nowMs);
    // The line the broadcast closes on when it is switched off.
    QString signOff();

    int pendingCalls() const { return m_pending.size(); }
    int runningAgents() const;
    int knownAgents() const { return m_agents.size(); }
    bool knows(int sessionId) const { return m_agents.contains(sessionId); }
    QList<int> agentIds() const { return m_agents.keys(); }

private:
    struct Agent {
        AgentFacts facts;
        bool introduced = false;
        qint64 firstSeenMs = 0;
        qint64 lastEventMs = 0;
        qint64 lastMentionMs = 0;
        QHash<int, qint64> lastKindMs; // PlayKind -> when it was last queued
        int studyStreak = 0;           // reads and searches since the last call
        int edits = 0;
        int fumbles = 0;
        int turnMark = 0;     // the last turn milestone called
        qint64 tokenMark = 0; // the last token milestone called
        qint64 quietCalledMs = 0;
    };

    Agent &agent(int sessionId, qint64 nowMs);
    QString nameFor(Agent &agent, bool allowIntroduction);
    // A call as it waits: the words are chosen when it is spoken, not when it
    // is queued, because how an agent is named depends on what was said last.
    struct Pending {
        QString pattern;
        QString detail;
        Priority priority = Priority::Play;
        PlayKind kind = PlayKind::None;
        int sessionId = -1;
        qint64 atMs = 0;
    };

    QString pick(const char *const *pool, int count);
    void queue(Agent &agent, PlayKind kind, Priority priority, const QString &pattern,
               const QString &detail, qint64 nowMs);
    QString render(const QString &pattern, Agent &agent, const QString &detail,
                   bool mayLink);
    void callStatusChange(Agent &agent, const QString &from, qint64 nowMs);
    void callMilestones(Agent &agent, qint64 nowMs);
    QString openingLine();
    QString colourLine(qint64 nowMs);

    QHash<int, Agent> m_agents;
    QVector<Pending> m_pending;
    QRandomGenerator m_random;
    QHash<quintptr, int> m_lastPick; // template pool -> the index used last
    bool m_opened = false;
    bool m_signedOff = false;
    bool m_lastWasColour = false;
    int m_lastSpokenSession = -1;
    qint64 m_lastCallMs = 0;
    int m_colourTurn = 0;
    int m_finishes = 0;
    int m_failures = 0;
};

// ---------------------------------------------------------------------------
// The voice.

// Where a usable Piper and voice model are, if anywhere.
struct VoicePaths {
    QString piper; // the executable
    QString model; // the .onnx; its .onnx.json sits beside it
    int sampleRate = 22050;
    bool ok() const { return !piper.isEmpty() && !model.isEmpty(); }
};

// The directory the app installs its own voice into.
QString voiceDirectory();
// FORKMESH_ANNOUNCER_PIPER / FORKMESH_ANNOUNCER_MODEL first, then the app's own
// install, then a `piper` on PATH with a voice in the usual shared places.
VoicePaths locateVoice();
// The model's native sample rate, from the .onnx.json beside it.
int voiceSampleRate(const QString &modelPath);

// Mono int16 at `fromRate` to the sonifier's format (interleaved stereo at
// sonify::kSampleRate), so both features share one set of player arguments.
// `carry` holds the last input sample between chunks so the join is seamless.
QByteArray resampleToPlayerFormat(const QByteArray &mono, int fromRate, qint16 *carry);

class Speaker : public QObject
{
public:
    enum class Backend {
        None,   // no voice here: announcing would be silent, so it is refused
        Piper,  // neural speech, piped into the system player
        Silent, // records what it was asked to say: the tests
    };

    explicit Speaker(QObject *parent = nullptr);
    ~Speaker() override;

    // False when there is no voice or no way to play one; nothing is started.
    bool start();
    void stop();
    bool isRunning() const { return m_backend != Backend::None; }
    Backend backend() const { return m_backend; }
    QString backendName() const;

    // Speak one line. Lines are not queued here on purpose: the caller asks
    // busy() first, so what is said is always chosen at the last moment.
    void say(const QString &line);
    // Still synthesising, or the audio already handed to the player has not
    // finished playing.
    bool busy(qint64 nowMs) const;

    // Everything say() was given, oldest first. Kept short; for tests and the
    // button's tooltip.
    QStringList recentLines() const { return m_recent; }
    // Called once if the voice dies and cannot be brought back.
    std::function<void(const QString &reason)> onFailed;

private:
    bool startProcesses();
    void onSpeech();
    void onPiperLog();
    void fail(const QString &reason);

    Backend m_backend = Backend::None;
    VoicePaths m_voice;
    QProcess *m_piper = nullptr;
    QProcess *m_player = nullptr;
    QTimer *m_synthesisGuard = nullptr;
    QStringList m_recent;
    QByteArray m_logTail;
    qint16 m_carry = 0;
    bool m_oddByte = false;
    char m_heldByte = 0;
    bool m_synthesising = false;
    qint64 m_playEndMs = 0;
    int m_restarts = 0;
    bool m_stopping = false;
};

} // namespace forkmesh::announce
