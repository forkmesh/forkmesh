#pragma once

// Watching the fleet: the same events AgentSonifier plays, drawn.
//
// Every agent is a body in a field, seated in a row by what it is doing:
// working along the top, waiting on you under it, idle and finished along
// the bottom. Within a row agents read left to right in the order they sit in
// the stereo mix, so the one you hear furthest left is the leftmost of its
// row. Its colour comes from the instrument you would hear it on, so the
// picture reads the same in both senses, and it swells with the tokens it has
// burned exactly as its instrument fills out.
//
// Every event is a mark off its body, each kind of work its own figure - a
// bolt for a shell command, a sweep for a search, a comet for your prompt. The
// classification is not a second
// opinion: it is sonify::motionForEvent, the one the music is written from, so
// nothing can be visible and silent or audible and invisible. An action that
// makes a note makes a ripple; one that makes no note - usage bookkeeping,
// a delta between two frames of the same sentence - makes none. Work going out
// ripples outwards and data coming back falls inwards, the same way the melody
// climbs for one and settles for the other.
//
// Two agents that touch the same file (or page) are joined: a link is drawn
// between their bodies, in both their colours. Only a real shared subject
// joins them - two runs that merely act in the same second are not connected,
// and the picture used to claim they were.
//
// Your own mirror nodes keep a seat along the top of the field whether or not
// anything is crossing to them (setMirrors), so an agent reaching one - over
// git, ssh or the gateway - is drawn as a pipe to a place that was already
// there, and the moment it first does is a cue like any other.
//
// Everybody else on the relay sits in a row beside the desktop's node
// (setClients): web visitors, other desktops, phones. Each arrival and
// departure is a cue, and every websocket frame between one of them and this
// desktop (socketFrame) is a bead on its way.
//
// Qt Core only, no painting and no clock of its own - the caller advances it -
// so the whole model is unit-testable (tests/test_agent_activity.cpp).
// AgentActivityView draws it; MainWindowAgentVisual.cpp feeds it.

#include "AgentSonifier.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <limits>
#include <utility>

namespace forkmesh::agentvis {

using forkmesh::sonify::Motion;
using forkmesh::sonify::Voice;

// How one kind of work looks. One entry per Motion, and the tests check that
// it is one entry per Motion: an action the ear has a note for and the eye has
// no mark for is exactly the bug this table exists to make impossible.
struct MotionStyle {
    double hue = 0;      // 0..1 around the wheel, the mark's own colour
    double reach = 1.6;  // how far the ripple travels, in body radii
    double seconds = 1.0;// how long it lives
    double weight = 1.0; // stroke width, relative
    int spokes = 0;      // 0 a plain ring, n an n-armed star
    bool inbound = false;// data coming back falls in; work flies out
    bool dashed = false; // a searching, unsettled mark
    const char *label = "";        // the sound list's word for it
    const char *description = "";  // what it stands for in the log
    const char *sound = "";        // what you hear for it
};

// Rest has a style too - an empty one - so the table can be indexed by the
// enum without a hole in it. styleFor(Rest).label is empty, and nothing draws.
const MotionStyle &styleFor(Motion motion);

// The colour an agent is drawn in: its instrument picks the family, the degree
// of the scale it calls home picks the shade within it. Same inputs as the
// voice, so what you are looking at is what you would be hearing.
double hueForVoice(const Voice &voice);

// Where a voice sits in the mix: x is the stereo field, y is the register
// with the climb folded in (0 is the top). The field orders each row by seatX.
double seatX(const Voice &voice);
double seatY(const Voice &voice);

// The rows the field seats agents in, top to bottom. Every state lands in
// exactly one.
enum class Band { Working, Waiting, Resting };
constexpr int kBandCount = 3;

// What an event is, in a few words: "Edit MainWindow.cpp", "$ ninja -C build",
// "grep kSlotSeconds", "thinking", "result in, 212 lines". What a mark says
// when the cursor is on it. Empty for bookkeeping.
QString actionForEvent(const QJsonObject &event);
// The same, for every event: bookkeeping says which piece of it it was
// ("log: message_start", "log: usage"), so no mark is ever wordless.
QString labelForEvent(const QJsonObject &event);
// The file or place an event touches, when it names one (a path, a URL): two
// agents on the same one are joined, and the link says which.
QString subjectForEvent(const QJsonObject &event);
// "MainWindow.cpp" from a path, "example.com" from a URL: short enough to
// print on a link.
QString shortSubject(const QString &subject);
// The words of a prompt event (you, steering it), whole up to `limit`.
QString promptForEvent(const QJsonObject &event, int limit = 600);
// The words of a finished reply (an assistant turn's text, a Codex agent
// message), whole up to `limit`; empty for anything else, streaming deltas
// included.
QString proseForEvent(const QJsonObject &event, int limit = 2000);
// Prose as one plain line, its markdown taken off: what a display can show.
QString plainProse(const QString &text, int limit = 240);

// What a run is doing, in the only four states a picture can carry. The caller
// maps its own status strings onto this; the model stays out of AgentStore.
enum class State { Idle, Running, Waiting, Done, Failed };

Band bandFor(State state);
// "working", "waiting on you", "idle or finished": what a row is called.
QString bandLabel(Band band);

// One row of agents in the same band, in field units (0..1): where the
// view writes the band's name, and how many sit in it.
struct BandRow {
    Band band = Band::Working;
    double y = 0;      // the row's centre
    double height = 0; // the row's own room; a band's first has kBandHeader over it
    int count = 0;     // bodies in the whole band, on its first row; 0 after
};

struct Body {
    int sessionId = 0;
    QString title; // the project it works in ("forkmesh"); seeds its voice
    QString task;  // the task it was given, as the agent list titles it
    Voice voice;
    State state = State::Idle;
    bool listening = false; // in the mix, so this one can be heard as well as seen
    double hue = 0;
    double x = 0.5;
    double y = 0.5;
    double size = 0;   // 0..1 from the tokens burned
    double energy = 0; // 0..1, jumps on every event and decays
    double bornSeconds = 0;
    double lastEventSeconds = -1e9;
    int events = 0;
    int turns = 0;
    qint64 tokens = 0;
    Motion lastMotion = Motion::Rest;
    QString doing;            // actionForEvent() of its last event that said anything
    double doingSeconds = -1e9;
    QString subject;          // subjectForEvent() of the last event that named one
    double subjectSeconds = -1e9;
    // What it was asked to do: the session's own prompt, and the latest one
    // it was sent while the field watched (a follow-up, a steer).
    QString prompt;
    QString lastPrompt;
    double lastPromptSeconds = -1e9;
    // What it last said to you: its latest reply, and when that came in.
    QString said;
    double saidSeconds = -1e9;

    // Where it is drawn, as an offset from its seat (x, y) in field units.
    // A body holds still on its seat: nothing moves it but the room it makes
    // for a neighbour seated too close, eased in so the two part rather than
    // jump.
    double dx = 0;
    double dy = 0;
    // The width of its place in its row, in field widths: how much room its
    // name has under it.
    double slot = 0.2;
    bool seated = false; // placed once already: a new seat is glided to
    double sepX = 0;
    double sepY = 0;
    double lastNoteSeconds = -1e9;
    // What it has moved over the network, and how fast right now.
    qint64 netUp = 0;
    qint64 netDown = 0;
    double netUpRecent = 0;
    double netDownRecent = 0;
    double lastNetSeconds = -1e9;
    // The field's own copy of the melody, for an agent nobody is listening
    // to: its marks land on the grid the music would have put them on.
    forkmesh::sonify::Melody melody;
    // Every mark it has made, by kind: what the cursor's key counts.
    std::array<int, forkmesh::sonify::kMotionCount> kinds{};

    bool active(double now) const { return now - lastEventSeconds < 2.0; }
    // Worth showing when the window is set to working agents only: running,
    // or it did something recently enough that its ripples are still settling.
    bool working(double now) const
    {
        return state == State::Running || now - lastEventSeconds < 30.0
               || now - lastNoteSeconds < 30.0 || now - lastNetSeconds < 30.0;
    }
};

struct Pulse {
    int sessionId = 0;
    Motion motion = Motion::Rest;
    double bornSeconds = 0;
    double seconds = 1.0;
    double strength = 0.5; // 0..1, from how much data the event moved
    double angle = 0;      // 0..1 around its body, fanned so two never overlay
    // A mark scheduled for a note that has not sounded yet is born in the
    // future; it strikes (lights its body) when the note does.
    bool struck = false;
    // Heard: the sonifier placed it, so a note really sounds when it lands.
    // Otherwise it is the field's own copy of a note nobody is listening to.
    bool heard = false;
    double createdSeconds = 0; // when it was asked for, on the scene clock
    QString text;              // what happened, for the cursor
    // The event's hash (sonify::eventSeed): the same seed that shades its
    // note shades its figure - its size, its turn, its colour, how many
    // sparks fly off it - so no two different events look quite alike.
    quint32 seed = 0;

    bool pending(double now) const { return now < bornSeconds; }
    double age(double now) const { return now - bornSeconds; }
    double life(double now) const
    {
        const double t = seconds > 0 ? age(now) / seconds : 1.0;
        return t < 0 ? 0 : (t > 1 ? 1 : t);
    }
    bool expired(double now) const { return age(now) >= seconds; }
};

// Two agents on the same file or page. Held by session id rather than by
// index so a body leaving the field cannot silently re-point a link.
struct Join {
    int a = 0; // the lower session id, always
    int b = 0;
    double strength = 0;    // 0..1, up on every overlap, decaying between them
    double lastSeconds = 0; // when they last overlapped
    int meetings = 0;       // how many times, over the field's whole life
    // What connected them, in words, for the cursor: "both on
    // AgentSonifier.cpp".
    QString why;
    bool shared = true; // always, now: only a shared subject joins two agents
};

// What kind of address a peer is, for the cursor: "IPv4, public", "IPv6,
// link-local", "IPv4, private (LAN)". Empty for text that is not an address.
QString addressInfo(const QString &peer);
// The service usually behind a TCP port, or empty: 443 -> "https".
QString portService(quint16 port);
// One connection's network details on a line: "140.82.112.3:443 · IPv4,
// public · TCP https". IPv6 addresses are bracketed before the port.
QString describeEndpoint(const QString &peer, quint16 port);
QString formatRate(double bytesPerSecond);
QString formatBytes(qint64 bytes);
// The registration behind an address with no name, from `whois` output, one
// fact a line: "network: GOOGLE", "range: 8.8.8.0 - 8.8.8.255", "owner:
// Google LLC", "country: US", "AS: AS15169", "abuse: ...". The client prints
// the registry's answer after any referral, so the last value of each field
// (the most specific block) wins. Empty when nothing was recognised.
QStringList parseWhois(const QString &text);

// One remote end a host was reached on: the address and port the kernel's
// socket table reported, whatever name the host is shown under. Kept on the
// host and on each flow so the cursor can say where a connection really went.
struct Endpoint {
    QString address;
    quint16 port = 0;
    qint64 up = 0;
    qint64 down = 0;
    double lastSeconds = 0;
};

// The network, drawn. Every agent's (and the desktop's own) traffic to each
// host it talks to, sampled from the kernel's socket table a few times a
// second: hosts sit along the top of the field, the desktop at the bottom,
// and bytes travel between them and the bodies as packets - out towards the
// host for data sent, in towards the body for data received.
struct Host {
    QString name;
    double x = 0.5;        // across the top of the field, eased to its slot
    double slot = 0.5;
    double bornSeconds = 0;
    double lastSeconds = 0;
    double upRecent = 0;   // bytes, decaying: the rate is this * ln2 / half-life
    double downRecent = 0;
    qint64 up = 0;         // totals since it appeared
    qint64 down = 0;
    QList<Endpoint> endpoints; // most recently used first, capped
    // One of your own mirror nodes (setMirrors): held along the top of the
    // field whether or not anyone is talking to it, so an agent reaching one
    // is seen as a pipe to a place that was already there.
    bool mirror = false;
    // A mirror is a source in its own right, not only a place agents reach:
    // its journal (the MIRROR lines of the desktop's log, which every saved
    // host streams in) marks off its seat the way an agent's work marks off
    // its body. This is the negative id those marks are filed under -
    // Scene::isMirrorSource(source) - and what it has done under it.
    int source = 0;
    double energy = 0;     // lit as its lines land, decaying like a body's
    int lines = 0;         // journal lines since it took its seat
    QString lastLine;      // the newest of them, for the cursor
    double lastLineSeconds = -1e9;
    int marks = 0;         // marks thrown, fanning them round its seat
    forkmesh::sonify::Melody melody; // its own line, so its cues are its own
    std::array<int, forkmesh::sonify::kMotionCount> kinds = {};
};

// A mirror node the field keeps a seat for: its name on the Hosts page and
// every address it is reached on. Traffic to any of those addresses is that
// mirror's, whatever the address resolves to.
struct MirrorSeat {
    QString name;
    QStringList addresses;
};

struct Flow {
    int sessionId = 0;
    QString host;
    double upRecent = 0;
    double downRecent = 0;
    double lastSeconds = 0;
    QList<Endpoint> endpoints; // this session's connections to the host
};

struct Traffic {
    quint64 serial = 0;
    double seconds = 0;
    int sessionId = 0;
    QString host;
    QString address;
    quint16 port = 0;
    qint64 pid = 0;
    QString process;
    qint64 up = 0;
    qint64 down = 0;
};

struct Connection {
    int sessionId = 0;
    QString host;
    QString address;
    quint16 port = 0;
    qint64 pid = 0;
    QString process;
    qint64 up = 0;
    qint64 down = 0;
    double upRate = 0;
    double downRate = 0;
    double firstSeconds = 0;
    double lastSeconds = 0;
    int samples = 0;
};

struct Packet {
    int sessionId = 0;
    QString host;
    bool up = true;        // leaving the body (sent) or arriving at it (received)
    double bornSeconds = 0;
    double seconds = 1.1;
    double size = 0.5;     // 0..1 from the bytes it stands for

    bool pending(double now) const { return now < bornSeconds; }
    double life(double now) const
    {
        const double t = (now - bornSeconds) / seconds;
        return t < 0 ? 0 : (t > 1 ? 1 : t);
    }
};

// Who else is in the relay room with this desktop. A web visitor is a browser
// on the site - a guest in the public chat or the world, or somebody signed in
// on the web; a node is another desktop; a phone is the mobile app. They sit in
// a row beside the desktop's node at the foot of the field, and every frame the
// relay's websocket carries between one of them and this desktop is a bead
// between the two.
enum class ClientKind { Node, Phone, Visitor };

// A room member as the relay's roster reported it: what setClients() is fed.
struct ClientInfo {
    QString id;          // the peer id its frames are sent under
    QString name;
    QString platform;    // linux | macos | windows | android | ios | web | empty
    QString accountKind; // node | user | guest | empty
    QString nodeName;
    QString version;
    // A visitor seen only in the Worker's live log: where it is and what it
    // browses with, for the cursor.
    QString detail;
    // Where the edge placed it: an ISO 3166 code ("NO"), "T1" for Tor, or
    // empty when the edge did not say. Drawn as a flag on its seat.
    QString country;
    // What the Worker's log said of a site visitor: its address, its network
    // and the user agent it sent, word for word. What a block is made from.
    QString address;
    QString asn;            // "2119", no "AS"
    QString asOrganization;
    QString userAgent;
};

// Which of the three a member is. A browser advertises neither a platform nor
// a node name; the desktop and the phones always advertise a platform.
ClientKind clientKindFor(const ClientInfo &info);
// "web visitor", "desktop node", "phone".
QString clientKindLabel(ClientKind kind);
// "Firefox", "Chrome", "a crawler" from a request's user-agent; empty for
// anything that is not a browser - curl, a script, the desktop's own calls.
QString browserForUserAgent(const QString &userAgent);
// "curl", "python-requests", "sqlmap" for a script or a known attack tool;
// "no user agent" for a request that sent none. Empty for a browser and for
// anything else: the desktop's, the phones' and the mirrors' own calls among
// them. `attack` says whether it is a scanner or an exploit kit.
QString toolForUserAgent(const QString &userAgent, bool *attack = nullptr);
// "Windows", "macOS", "Linux", "Android", "iOS", "ChromeOS"; empty when the
// user agent does not say.
QString systemForUserAgent(const QString &userAgent);
// The flag for a country: its two regional-indicator letters, which an
// emoji font draws as the flag. Empty for anything but two letters A-Z, and
// for the codes that are no country ("XX", Tor's "T1").
QString flagForCountry(const QString &code);
// "Norway" for "NO", "Tor" for "T1"; the code itself when Qt has no name.
QString countryName(const QString &code);
// Whether a request's path is one a scanner asks for and this site never
// serves: "/.env", "/wp-login.php", "/.git/config", "/phpmyadmin/".
bool isProbePath(const QString &path);

// How worried the field is about a site visitor.
enum class Threat { None, Watch, Danger };
// "watch", "danger"; empty for None.
QString threatLabel(Threat threat);

// What a block is aimed at. Cloudflare's IP Access Rules take the first four;
// a user agent is matched word for word.
enum class BlockTarget { Address, Range, Network, Country, UserAgent };
// "ip", "ip_range", "asn", "country", "user_agent": the rule's target.
QString blockTargetKey(BlockTarget target);

// One way to block a visitor: what to match, why, and whom else it catches.
struct BlockRule {
    BlockTarget target = BlockTarget::Address;
    QString value;       // "203.0.113.7", "203.0.113.0/24", "AS14061", "NO", a user agent
    QString label;       // "the address 203.0.113.7", "AS14061 DigitalOcean"
    QString reason;      // why this is the one to use
    int visitors = 1;    // present site visitors it would catch
    int flagged = 0;     // how many of those are flagged
    bool recommended = false;
};

struct Client {
    ClientInfo info;
    ClientKind kind = ClientKind::Node;
    double bornSeconds = 0;
    double leftSeconds = -1; // when it left; it fades out and then goes
    double energy = 0;       // lit by every frame to or from it, decaying
    int framesIn = 0;        // frames it sent this desktop since it arrived
    int framesOut = 0;       // frames this desktop sent it, or the whole room
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    QString lastType;        // its newest frame's type: "chat", "presence", "typing"
    double lastFrameSeconds = -1e9;
    // Seen in the Worker's live log (webRequest) rather than in the relay's
    // roster: it has no presence to lose, so it leaves once it goes quiet.
    bool web = false;
    int requests = 0;
    // Its frames by type, both ways: "presence" 14, "chat" 2. For a site
    // visitor, its requests by status class: "2xx", "4xx".
    QMap<QString, int> types;
    // A site visitor's latest requests, newest first, with when each came.
    QVector<std::pair<QString, double>> recent;
    static constexpr int kRecentRequests = 6;
    // When its requests came in, over the last minute: its rate.
    QVector<double> requestTimes;
    int refused = 0;      // requests answered 4xx
    int failed = 0;       // requests answered 5xx
    int probes = 0;       // requests for a path scanners ask for (isProbePath)
    QString lastProbe;    // the latest such path
    QString tool;         // toolForUserAgent: empty for a browser
    bool attackTool = false;
    // Scene::assess's verdict, and every reason for it.
    Threat threat = Threat::None;
    QStringList threatReasons;
    bool blocked = false; // a rule the desktop made matches it

    bool present() const { return leftSeconds < 0; }
    // Requests in the minute up to `now`.
    int requestRate(double now) const;
};

// Who setClients() found had come or gone.
struct ClientChange {
    ClientInfo info;
    ClientKind kind = ClientKind::Node;
    bool joined = true;
};

// One websocket frame in flight between a client and the desktop's node, or
// between the room itself and the node when no client is named (a frame to
// the whole room, the relay's own announcements, a peer the roster has not
// caught up with yet).
struct Frame {
    QString client;  // Client id, or empty for the room
    bool up = false; // sent by this desktop, or received
    QString type;
    double bornSeconds = 0;
    double seconds = 0.9;
    double size = 0.4; // 0..1 from its bytes

    double life(double now) const
    {
        const double t = (now - bornSeconds) / seconds;
        return t < 0 ? 0 : (t > 1 ? 1 : t);
    }
};

// The relay websocket itself, as its own counters read: what the desktop's
// node says under the cursor.
struct RelaySocket {
    bool connected = false;
    QString endpoint;
    QString room;
    double connectedSeconds = 0; // how long this connection has been up
    qint64 framesIn = 0;
    qint64 framesOut = 0;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    qint64 controlIn = 0;  // pings, pongs, closes
    qint64 controlOut = 0;
    qint64 buffered = 0;   // bytes queued to send
    QString lastInType;
    QString lastOutType;
};

// Whose background work a Task is: the desktop's own (a footer ticket), or
// the agent's - a subagent it delegated to, or a shell it left running in the
// background - read off its transcript.
enum class TaskKind { Desktop, Subagent, Shell };

// One unit of background work while it runs, and for a moment after it ends:
// the desktop's own (BackgroundActivity: a git status, a fetch, a diff), or an
// agent's subagent or background shell. It orbits the agent it works for, or
// the desktop at the bottom of the field when it belongs to no agent.
struct Task {
    quint64 id = 0;
    TaskKind kind = TaskKind::Desktop;
    int sessionId = 0;   // the agent it works for, or Scene::kDesktop
    QString word;        // "git status", "fetch", ... what the footer calls it
    QString detail;      // the command line, for the cursor
    double bornSeconds = 0;
    double endSeconds = -1; // < 0 while it runs
    bool failed = false;
    double angle = 0;    // 0..1 around its owner, fanned so two never overlay

    bool running() const { return endSeconds < 0; }
};

// One process in an agent's tree, as the sampler read it off /proc: what
// sampleProcesses() is fed.
struct ProcessInfo {
    qint64 pid = 0;
    qint64 parentPid = 0;
    QString name;    // the kernel's short name: "bash", "ninja", "cc1plus"
    QString command; // the command line
    qint64 cpuTicks = 0; // utime + stime, monotonic
};

// A process an agent started - its shell, a build and the compilers under it,
// a test run, an MCP server - while it lives, and for a moment after. Drawn as
// a tree fanning out from its agent: each one hangs off its parent, lit while
// it burns CPU and dim while it sleeps.
struct Process {
    int sessionId = 0;
    qint64 pid = 0;
    qint64 parentPid = 0; // 0 when its parent is the agent itself
    QString name;
    QString command;
    int depth = 1;        // 1 for the agent's own children
    double angle = 0;     // 0..1 around its agent; children fan off their parent's
    double bornSeconds = 0;
    double endSeconds = -1;
    qint64 cpuTicks = 0;
    double cpu = 0;       // cores it used over the last sample
    double sampledSeconds = 0;

    bool running() const { return endSeconds < 0; }
    bool busy() const { return running() && cpu >= 0.05; }
};

// One mark that landed: every note of every agent, heard or not, kept for a
// minute and a half. The ring of history around each body is drawn from
// these.
struct Mark {
    int sessionId = 0;
    Motion motion = Motion::Rest;
    double seconds = 0;
    double strength = 0.5;
    bool heard = false;
    QString text;
};

// One line of the fleet's account: an agent, what it did, and when.
struct Action {
    int sessionId = 0;
    Motion motion = Motion::Rest;
    QString text;
    double seconds = 0;
    int repeats = 1; // the same thing again straight after is one line, counted
};

// One line of the desktop's own log (the Logs page), shown in the field with
// the agents: the column down the left of the window, and a spark off the
// body of the agent it names - "#281", "agent 281", "session 281" - or off
// the desktop at the bottom when it names none.
struct LogLine {
    Motion motion = Motion::LogInfo; // sonify::motionForLogBadge(badge)
    QString badge;  // the Logs page's category: "ERROR", "CLOUD", "AGENT", ...
    QString accent; // that category's colour, "#rrggbb", as the Logs page draws it
    QString text;
    int sessionId = 0; // the agent it is about, or Scene::kDesktop
    double seconds = 0;
    int repeats = 1;
    bool output = false;

    // The spark rises off its source for this long after the line arrives.
    static constexpr double kSparkSeconds = 2.4;
    bool sparking(double now) const { return now - seconds < kSparkSeconds; }
};

class Scene
{
public:
    // An agent counts as working this second if it did something inside this.
    static constexpr double kJoinWindow = 1.25;
    // A link left alone halves in this many seconds, and is dropped once there
    // is nothing left of it. A pair that keeps meeting keeps its link lit.
    static constexpr double kJoinHalfLife = 5.0;
    // A body's glow, likewise: bright the instant it does something.
    static constexpr double kEnergyHalfLife = 1.6;
    // Ripples in flight. A fleet in full cry makes a few hundred a minute and
    // each one lives a second or two; past this the oldest go.
    static constexpr int kMaxPulses = 512;
    // Bodies ease apart with this time constant rather than jumping.
    static constexpr double kGlideSeconds = 0.45;
    // No two bodies closer than this, in field heights (widths count for 1/1.6
    // of one): seats that land near each other are eased apart so every agent
    // stays readable, however much it has swollen.
    static constexpr double kPersonalSpace = 0.15;

    Scene();

    // The clock is the caller's, in seconds, monotonic. Every decay and expiry
    // happens here, so a scene nobody advances never changes under a test.
    void advanceTo(double seconds);
    // The same, off this scene's own monotonic clock: what the window calls
    // before it ingests an event and before it paints, so the marks and the
    // decay it draws them with are measured against one clock.
    void tick();
    double now() const { return m_now; }

    // Adds the agent if it is new, and returns it either way. The voice is the
    // one the sonifier would give it, so a run takes the same seat whether or
    // not anybody is listening yet; setVoice() re-seats it if the mixer had to
    // move it to keep two agents apart.
    Body &addAgent(int sessionId, const QString &title);
    void setVoice(int sessionId, const Voice &voice);
    void setTitle(int sessionId, const QString &title);
    void setTask(int sessionId, const QString &task);
    // Its latest reply, when the caller knows it better than the events the
    // field has seen (a run that spoke before the window opened).
    void setSaid(int sessionId, const QString &said);
    void setState(int sessionId, State state);
    void setListening(int sessionId, bool listening);
    // Turns walk the body up the field, tokens swell it: the same two numbers
    // that move the voice, doing the same two things to the picture.
    void setUsage(int sessionId, int turns, qint64 tokens);
    // The session's own prompt, for the cursor.
    void setPrompt(int sessionId, const QString &prompt);
    void removeAgent(int sessionId);
    void clear();

    // One transcript event, for an agent that may not be in the field yet.
    // True means it was work; bookkeeping (sonify::isBookkeeping) is false, but still
    // leaves its faint mark, because the sonifier still ticks for it. The mark itself lands on the note: on the shared grid, when the
    // music would sound it, and not at all when the music would drop it.
    //
    // `heard` is for an agent that is in the mix: the sonifier places its
    // notes, so the mark waits for note() instead of guessing the slot, and
    // what you see is what you are hearing to the beat.
    bool ingest(int sessionId, const QString &title, const QJsonObject &event,
                bool heard = false);

    // A note the sonifier scheduled for this agent, `delaySeconds` from now on
    // this scene's clock. The mark is born when it sounds, in the colour of the
    // work that asked for it, and it lights its body as it lands.
    // `heard` false is the field placing a note nobody is listening to.
    void note(int sessionId, Motion motion, double delaySeconds,
              double gain, bool heard = true,
              const QString &text = QString(), quint32 seed = 0);

    // The run ended. A ring in the ending's own colour, big enough to read
    // from across the room, the way the cadence is louder than the stream.
    void finish(int sessionId, bool success);

    // Something outside the transcripts (sonify::isCue): a log line, a host
    // joining, the desktop's background work. Its mark lands off the agent
    // it belongs to, or off the desktop's node when it belongs to none (or
    // was heard in the desktop's voice). `heard` is the sonifier having
    // placed the note already; otherwise the field works the note out itself
    // on the same melody, as ingest() does.
    void cue(int sessionId, Motion motion, const QString &text, quint32 seed,
             bool heard = false);

    const QList<Body> &bodies() const { return m_bodies; }
    // The rows the bodies are seated in, top to bottom; empty bands have none.
    const QVector<BandRow> &rows() const { return m_rows; }
    // Marks of this kind made by one agent, or by the desktop (kDesktop).
    int kindCount(int sessionId, Motion motion) const;
    // Where the rows sit: below the hosts' strip, above the desktop's node.
    static constexpr double kRowsTop = 0.17;
    static constexpr double kRowsBottom = 0.80;
    // Each band's name has a strip of this much over its first row.
    static constexpr double kBandHeader = 0.035;
    // Past this many to a row, a band takes another row.
    static constexpr int kRowSize = 8;
    const Body *body(int sessionId) const;
    // A body at work: running, or waiting on you. The desktop's own chores in
    // an agent's worktree, and its log lines naming the agent, are drawn on
    // the agent only while it is - stopping or merging one is not it working.
    bool working(int sessionId) const;
    const QVector<Pulse> &pulses() const { return m_pulses; }
    // Live links, faintest first, so a painter can lay the quiet ones under
    // the loud ones without sorting them itself.
    QVector<Join> joins() const;
    // The desktop itself as a network endpoint: its own traffic (the relay,
    // the API behind every page) flows from the bottom of the field.
    static constexpr int kDesktop = -2;
    // Rates are recent bytes decaying with this half-life.
    static constexpr double kRateHalfLife = 1.5;
    // A host nobody has talked to for this long leaves the top of the field.
    static constexpr double kHostLinger = 45.0;
    static constexpr int kMaxHosts = 10;
    static constexpr int kMaxPackets = 600;
    static constexpr int kMaxEndpoints = 6;

    // Bytes one agent (or kDesktop) moved to and from one host since the last
    // sample. Packets are launched for them at once: this is the real time.
    // True when the host is new to the field, or when this agent first
    // reaches one of your mirrors: a connection worth a cue.
    // Traffic to a mirror's address is the mirror's, whatever `named` says.
    // `address` and `port` are the connection's remote end, when known.
    bool network(int sessionId, const QString &named, qint64 up, qint64 down,
                 const QString &address = QString(), quint16 port = 0, qint64 pid = 0,
                 const QString &process = QString());
    static constexpr int kAnySession = std::numeric_limits<int>::min();
    static constexpr int kMaxTraffic = 5000;
    static constexpr double kConnectionRateSeconds = 3.0;
    QVector<Traffic> traffic(int sessionId, const QString &host, quint64 after = 0) const;
    QVector<Connection> connections(int sessionId, const QString &host) const;
    quint64 lastTraffic() const { return m_trafficSerial; }
    QString hostNamed(const QString &name) const;
    // A lookup came back: the host that was only an address has a name now.
    // A mirror keeps its own name: nothing renames it.
    void renameHost(const QString &from, const QString &to);
    // Your mirror nodes. Each takes a seat at the left of the hosts' strip
    // and keeps it while it is listed; one no longer listed is an ordinary
    // host again and leaves once it has been quiet for kHostLinger. Traffic
    // already drawn to one of a mirror's addresses is folded into it.
    void setMirrors(const QList<MirrorSeat> &mirrors);
    // Mirrors are sources of marks as well as places: each seated mirror has
    // a negative id of its own, under kMirrorFirst, that its journal's marks
    // and sparks are filed under. 0 when the name is not a seated mirror.
    int mirrorSource(const QString &name) const;
    // The mirror a source id belongs to, or nullptr.
    const Host *mirrorForSource(int sessionId) const;
    static constexpr int kMirrorFirst = -100;
    static constexpr int kMirrorSources = 800;
    static bool isMirrorSource(int sessionId)
    {
        return sessionId <= kMirrorFirst && sessionId > kMirrorFirst - kMirrorSources;
    }
    // Log lines this source (an agent, the desktop or a mirror) has had in
    // the last minute.
    int lineRate(int sessionId) const;
    // The mirror a log line is from: a MIRROR-badged line names the host it
    // was streamed from ("Mirror <node> <source>: …"). 0 when it names none.
    int mirrorForLog(const QString &badge, const QString &text) const;
    // The mirror an address belongs to, or empty.
    QString mirrorForAddress(const QString &address) const
    {
        return m_mirrorByAddress.value(address);
    }
    static constexpr int kMaxMirrors = 12;
    // What `whois` said about an address that has no name (parseWhois lines),
    // for the cursor; empty until the lookup comes back.
    void setAddressNetwork(const QString &address, const QStringList &lines);
    QStringList addressNetwork(const QString &address) const
    {
        return m_addressNetwork.value(address);
    }
    const QList<Host> &hosts() const { return m_hosts; }
    const Host *host(const QString &name) const;
    QVector<Flow> flows() const { return m_flows.values().toVector(); }
    const QVector<Packet> &packets() const { return m_packets; }
    // Bytes a second, the whole field: what the headline says.
    double upRate() const;
    double downRate() const;
    static double rateOf(double recent) { return recent * 0.6931 / kRateHalfLife; }

    // Background work, drawn round whoever it works for. A finished task stays
    // this long - a flash in its outcome's colour - and then leaves.
    static constexpr double kTaskLinger = 1.4;
    static constexpr int kMaxTasks = 48;
    // `sessionId` not in the field, or not working(), is the desktop's own work.
    void taskStarted(quint64 id, int sessionId, const QString &word,
                     const QString &detail = QString());
    void taskFinished(quint64 id, bool failed = false);
    const QVector<Task> &tasks() const { return m_tasks; }
    int runningTasks() const;
    // Only the desktop's own: the agents' come and go with their transcripts.
    void clearTasks();
    // An agent's own background work is read off its transcript by ingest():
    // an Agent (or Task) call is a subagent, running until its result comes
    // back; a Bash call with run_in_background is a shell, running until a
    // task notification, an output check or a kill says it has ended. A
    // subagent launched in the background likewise waits for its notification.
    // One the transcript never closes is dropped after this long.
    static constexpr double kAgentTaskLimit = 6 * 3600.0;

    // The agent's process tree, sampled: everything under `rootPid` (the
    // agent's own process). The agent runtime itself (claude, codex, the
    // sandbox wrapping it) is not drawn; what it started hangs off the body.
    // A process missing from the sample has ended. An empty sample ends them
    // all, which is what the caller sends for an agent that stopped running.
    void sampleProcesses(int sessionId, const QList<ProcessInfo> &processes,
                         qint64 rootPid);
    const QVector<Process> &processes() const { return m_processes; }
    const Process *process(int sessionId, qint64 pid) const;
    int runningProcesses() const;
    int busyProcesses() const;
    // Past this many at once, an agent's newest processes are not drawn (a
    // -j64 build is a crowd, not a tree), only counted.
    static constexpr int kMaxProcesses = 40;
    int hiddenProcesses(int sessionId) const { return m_hiddenProcesses.value(sessionId); }
    static constexpr double kProcessLinger = 1.2;
    // Kernel clock ticks a second, what cpuTicks is counted in.
    static constexpr double kClockTicks = 100.0;

    // Whether anything in the field is happening: an agent running or acting,
    // a mark or packet in flight, background work, traffic still settling.
    // Nothing on the field animates for its own sake - every movement is a
    // real event, a real number changing - so a field that is not moving is
    // painted once and left still.
    bool moving() const;
    // Below this many bytes a second the field counts as quiet.
    static constexpr double kQuietRate = 64.0;

    // How many agents did something inside the join window: the fleet's pulse.
    int activeCount() const;
    // How many would be shown with the window set to working agents only.
    int workingCount() const;
    // The last things the fleet did, oldest first. Capped at kFeedSize.
    const QVector<Action> &feed() const { return m_feed; }
    static constexpr int kFeedSize = 60;
    // Two agents on the same file within this many seconds of each other are
    // joined whether or not they happened to act in the same moment.
    static constexpr double kSharedWindow = 90.0;
    // Every mark that landed in the last kHistorySeconds, oldest first.
    const QVector<Mark> &history() const { return m_history; }
    static constexpr double kHistorySeconds = 90.0;
    static constexpr int kMaxHistory = 900;
    // When a note of this kind last sounded (heard) or last landed at all, and
    // how many have: what lights its row in the window's list of sounds.
    double lastSounded(Motion motion) const;
    double lastLanded(Motion motion) const;
    int soundCount(Motion motion) const;
    // The words of the latest mark of this kind that had any - "$ ninja -C
    // build", "Read MainWindow.cpp" - when it landed, and how many of the
    // kind have landed: what each row of the window's list says live.
    QString lastText(Motion motion) const;
    double lastTextSeconds(Motion motion) const;
    int landCount(Motion motion) const;
    // Events a minute over the last minute, counted from the marks themselves.
    int eventRate() const;
    qint64 totalEvents() const { return m_totalEvents; }

    // A line of the desktop's log. Returns the source it was put against: the
    // agent it names while that one is working(), the mirror it was streamed
    // from, or kDesktop.
    // `ageSeconds` is how long ago it was written: a backlog seeded
    // when the window opens passes its age, so only what is new sparks.
    // A new line (ageSeconds 0) also sounds as its badge's kind: see cue().
    int log(const QString &badge, const QString &accent, const QString &text,
            double ageSeconds = 0, bool heard = false);
    // The shade a line's note and mark take: its badge and words, numbers
    // aside. Never 0.
    static quint32 logSeed(const QString &badge, const QString &text);
    static bool outputLine(const QString &text);
    // The agent a log line is about: the first "#N", "agent N" or "session N"
    // in it that is a body in the field. kDesktop when there is none.
    int agentForLog(const QString &text) const;
    // Whom a line naming that agent is put against: it while working(), else
    // the desktop. The same answer log() gives, for the sonifier to match.
    int ownerForLog(const QString &text) const;
    // Oldest first, capped at kLogSize.
    const QVector<LogLine> &logs() const { return m_logs; }
    void clearLogs() { m_logs.clear(); }
    static constexpr int kLogSize = 200;
    // Log lines a minute over the last minute.
    int logRate() const;

    // The relay room's other members. Returns who arrived and who left since
    // the last call; one that left fades out over kClientLinger. `announce`
    // false seeds the row without anybody arriving (a window just opened on
    // a room already full).
    QList<ClientChange> setClients(const QList<ClientInfo> &clients, bool announce = true);
    // In the row's order: nodes nearest the desktop, then phones, then web
    // visitors, each in the order they arrived.
    const QList<Client> &clients() const { return m_clients; }
    const Client *client(const QString &id) const;
    // Present, not fading out.
    int clientCount(ClientKind kind) const;
    static constexpr double kClientLinger = 4.0;
    // How long a newcomer's arrival ring takes to open.
    static constexpr double kClientArrival = 1.6;
    static constexpr int kMaxClients = 200;
    // A frame on the relay's websocket. `client` is the peer it came from or
    // was sent to; empty for one sent to the whole room (it goes to every
    // client in it) or one no client in the row sent.
    void socketFrame(bool outgoing, const QString &type, const QString &client, qint64 bytes);
    const QVector<Frame> &frames() const { return m_frames; }
    static constexpr int kMaxFrames = 400;
    // A frame to the whole room is drawn to this many clients at most, the
    // nearest first: past that it is a crowd, and the counts say so.
    static constexpr int kRoomFanout = 24;
    // Frames a minute over the last minute, both ways.
    int frameRate() const;
    qint64 totalFrames() const { return m_totalFrames; }
    // A request to the site, from the Worker's live log: the browser that made
    // it (`visitor` identifies it - its address, or its place and browser)
    // takes a seat as a web visitor, and the request flies in as a frame.
    // True when the visitor is new to the row.
    bool webRequest(const QString &visitor, const QString &name, const QString &detail,
                    const QString &request, qint64 bytes = 0);
    // The same, with everything the Worker's log said of it: what the
    // attack checks and the block rules are made from.
    struct WebHit {
        QString visitor; // its address, or its place, network and browser
        QString name;    // "Firefox in Oslo, NO"
        QString detail;
        QString request; // "GET /docs 200"
        QString path;
        int status = 0;
        QString country;
        QString address;
        QString asn;
        QString asOrganization;
        QString userAgent;
        qint64 bytes = 0;
    };
    bool webRequest(const WebHit &hit);
    // A web visitor quiet this long has left the site.
    static constexpr double kWebVisitorIdle = 300.0;

    // Whether the site is under attack, visitor by visitor and as a whole.
    // Every site visitor is weighed after each request: its rate, what it
    // asks for, what it runs, and the crowd it came with - many addresses
    // out of one network or one range arriving together.
    static constexpr int kWatchRate = 30;    // requests a minute
    static constexpr int kDangerRate = 120;
    static constexpr int kCrowdWatch = 8;    // new visitors from one network in kCrowdSeconds
    static constexpr int kCrowdDanger = 20;
    static constexpr double kCrowdSeconds = 120.0;
    static constexpr int kSurgeWatch = 30;   // new visitors of every kind a minute
    static constexpr int kSurgeDanger = 90;
    // The worst verdict among the present visitors, or the arrivals' own:
    // a surge of newcomers is a warning even when none of them is.
    Threat siteThreat() const;
    QStringList siteThreatReasons() const;
    // Present visitors flagged watch or worse.
    int flaggedCount() const;
    // Clients of every kind that arrived in the last minute.
    int arrivalRate() const;
    // Every way to block this site visitor, narrowest first, the one that
    // catches the attack it is part of without catching more marked
    // recommended. Empty for anyone not seen in the Worker's log, or seen
    // without an address.
    QList<BlockRule> blockRules(const QString &clientId) const;
    // The best rule for every flagged visitor, merged: one range rule rather
    // than twenty address rules out of it. Most caught first.
    QList<BlockRule> suggestedBlocks() const;
    // A rule was made: every present visitor it matches is marked blocked,
    // and later requests matching it are marked too.
    void noteBlocked(const BlockRule &rule);
    // Whether a rule the desktop made matches this visitor.
    bool matchesBlock(const Client &client, const BlockRule &rule) const;
    const QList<BlockRule> &blocks() const { return m_blocks; }
    void setRelaySocket(const RelaySocket &socket) { m_socket = socket; }
    const RelaySocket &relaySocket() const { return m_socket; }
    // Whether any line is still sparking, for the view's frame rate.
    bool logSparking() const;

private:
    Body *find(int sessionId);
    // Seats every body in its band's rows, left to right by its place in the
    // stereo mix (then by session). A body that changes seat glides there.
    void relayout();
    void touchJoins(int sessionId);
    void record(int sessionId, Motion motion, const QString &text);
    Pulse &addPulse(int sessionId, Motion motion, double bornSeconds,
                    double strength);
    void land(const Pulse &pulse);
    // Words for the mark note() made for this event a moment ago.
    void caption(int sessionId, Motion motion, const QString &text);
    void strike(Pulse &pulse);
    // Eases bodies seated too close apart.
    void separate(double elapsed);

    QElapsedTimer m_clock;
    double m_now = 0;
    double m_lastAdvance = 0;
    bool m_separating = false; // a body is still easing clear of a neighbour
    QVector<BandRow> m_rows;
    double m_space = kPersonalSpace; // relayout()'s kPersonalSpace for the grid
    std::array<int, forkmesh::sonify::kMotionCount> m_desktopKinds{};
    QVector<Task> m_tasks;
    // Background work read off transcripts: "session|tool_use_id" for a call
    // in flight, "session|id" for the id it was launched under, and output
    // checks waiting on their result ("session|tool_use_id" -> "session|id").
    QHash<QString, quint64> m_agentTaskIds;
    QHash<QString, QString> m_outputChecks;
    void watchAgentWork(int sessionId, const QJsonObject &event);
    void startAgentTask(int sessionId, const QString &toolUseId, TaskKind kind,
                        const QString &word, const QString &detail);
    // Puts a task round `sessionId` as given (the desktop's when it is not in
    // the field): an agent's own work is its whatever its state.
    void placeTask(quint64 id, int sessionId, const QString &word, const QString &detail);
    void endAgentTask(const QString &key, bool failed);
    void forgetAgentTasks(int sessionId);
    QVector<Process> m_processes;
    QHash<int, int> m_hiddenProcesses;
    QList<Body> m_bodies;
    QVector<Pulse> m_pulses;
    QHash<qint64, Join> m_joins;
    QVector<double> m_eventTimes; // the last minute, for the rate
    QVector<Action> m_feed;
    QVector<Mark> m_history;
    double m_lastSounded[forkmesh::sonify::kMotionCount];
    double m_lastLanded[forkmesh::sonify::kMotionCount];
    int m_soundCount[forkmesh::sonify::kMotionCount] = {};
    int m_landCount[forkmesh::sonify::kMotionCount] = {};
    QString m_lastText[forkmesh::sonify::kMotionCount];
    double m_lastTextSeconds[forkmesh::sonify::kMotionCount];
    void noteText(Motion motion, const QString &text, double seconds);
    QVector<LogLine> m_logs;
    QList<Host> m_hosts;
    QList<MirrorSeat> m_mirrors;
    QHash<QString, QString> m_mirrorByAddress;
    Host *mirrorHost(int sessionId);
    void seatMirrors();
    QHash<QString, QStringList> m_addressNetwork;
    QHash<QString, Flow> m_flows; // "session|host"
    QVector<Traffic> m_traffic;
    quint64 m_trafficSerial = 0;
    QHash<QString, QString> m_renamed;
    QVector<Packet> m_packets;
    void settleHosts(double elapsed);
    qint64 m_totalEvents = 0;
    // The desktop's own line, for cues nobody is listening to, and how many
    // marks it has thrown (fanning them round its node).
    forkmesh::sonify::Melody m_desktopMelody;
    int m_desktopMarks = 0;
    QList<Client> m_clients;
    Client *findClient(const QString &id);
    void sortClients();
    QVector<Frame> m_frames;
    QVector<double> m_frameTimes; // the last minute, for the rate
    QVector<double> m_arrivals;   // when clients arrived, over the last minute
    QList<BlockRule> m_blocks;
    // Weighs every site visitor again: its own signals and its crowd's.
    void assess();
    qint64 m_totalFrames = 0;
    RelaySocket m_socket;
};

// "#281 fix the worker - celesta, high register, left - 12 turns, 48k tokens".
// What the window says under the cursor, and the same words the Listen button
// uses for the sound, so the two surfaces never describe an agent differently.
QString describeBody(const Body &body);

} // namespace forkmesh::agentvis
