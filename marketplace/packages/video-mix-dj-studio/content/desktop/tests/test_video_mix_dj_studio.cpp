#include "DjModel.h"
#include "DjPlayer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QWidget>

#include <cmath>
#include <cstdio>

int runCrateTests();
int runLogicTests();

using namespace forkmesh::djstudio;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    ++failures;
    std::printf("FAIL: %s\n", message);
}

void wait(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void tick(DjPlayer &player, int deck, double time, double duration, int state)
{
    player.onEvent(QStringLiteral("tick"), QJsonObject{{QStringLiteral("type"), QStringLiteral("tick")},
                                                       {QStringLiteral("d"), QJsonArray{QJsonArray{deck, time, duration, state, 0.5}}}});
}

const QString songA = QStringLiteral("dQw4w9WgXcQ");
const QString songB = QStringLiteral("9bZkp7q19f0");
const QString songC = QStringLiteral("kJQP7kiw5Fk");

DjModel::Host host(const QString &dir)
{
    DjModel::Host out;
    out.dataDir = dir;
    return out;
}

void testLoadPlayAndTrack(const QString &dir)
{
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    model.load(0, songA, QStringLiteral("Never Gonna"), QStringLiteral("Rick"));
    check(model.deck(0).loaded() && model.deck(0).title == QLatin1String("Never Gonna"), "load puts the song on the deck");
    model.start(0);
    check(model.deck(0).playing && model.deck(0).keepsPlaying, "START plays the deck and remembers the intent");
    check(model.history().size() == 1 && model.history().first().id == songA, "a started song goes to Recently Played");
    check(model.plays(songA) == 1, "a started song counts one play");
    model.pauseDeck(0);
    model.start(0);
    check(model.plays(songA) == 1, "pause and start again do not count a second play");
    wait(650);
    tick(player, 0, 10, 200, 1);
    check(model.deck(0).advancing && std::abs(model.deck(0).time - 10) < 0.01, "a playing tick moves the deck to the reported time");
    check(model.deck(0).duration == 200, "the tick reports the song length");
    const double before = model.deck(0).position(DjModel::now());
    wait(120);
    check(model.deck(0).position(DjModel::now()) > before + 0.08, "the position runs on between ticks");
    model.hotCue(0, 2);
    check(model.deck(0).hotcues[2] > 9.9, "pressing an empty hot cue sets it at the playhead");
    model.seek(0, 50);
    model.hotCue(0, 2);
    check(std::abs(model.deck(0).time - model.deck(0).hotcues[2]) < 0.01, "pressing a set hot cue jumps back to it");
    model.deleteHotCue(0, 2);
    check(model.deck(0).hotcues[2] < 0, "a hot cue can be cleared");
    model.deck(0).bpm = 120;
    model.setLoopSize(0, 5);
    model.toggleLoop(0);
    check(model.deck(0).loop && std::abs(model.deck(0).loopLength - 2.0) < 1e-9, "a 4-beat loop at 120 BPM lasts two seconds");
    model.toggleLoop(0);
    check(!model.deck(0).loop, "LOOP toggles the loop off");
    tick(player, 0, 30, 200, 0);
    check(model.deck(0).playing, "an end report long before the end is not believed");
    model.stopAll();
    check(!model.deck(0).playing && !model.deck(0).keepsPlaying, "Stop the Music pauses every deck");
}

void testCrossfaderCurves(const QString &dir)
{
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    model.setCrossfader(-1);
    check(std::abs(model.crossfadeGain(0) - 1) < 1e-9 && model.crossfadeGain(1) < 1e-9, "hard left is only deck 1");
    model.setCrossfader(0);
    check(std::abs(model.crossfadeGain(0) - std::sqrt(0.5)) < 1e-6, "the smooth curve is equal power in the middle");
    model.setCrossfaderCurve(1);
    check(std::abs(model.crossfadeGain(0) - 1) < 1e-9 && std::abs(model.crossfadeGain(1) - 1) < 1e-9, "the full curve keeps both decks loud in the middle");
    model.setCrossfaderCurve(2);
    model.setCrossfader(0.95);
    check(model.crossfadeGain(0) < 0.5 && std::abs(model.crossfadeGain(1) - 1) < 1e-9, "the cut curve closes a deck near the end");
    model.setAssign(0, 2);
    check(std::abs(model.crossfadeGain(0) - 1) < 1e-9, "a deck set to through ignores the crossfader");
    model.setLevel(1, 0.5);
    model.setMaster(1);
    check(std::abs(model.audibleGain(1) - 0.5) < 1e-9, "the deck level multiplies with the crossfader");
    check(model.audibleGain(2) == 0, "decks 3 and 4 are silent in two-deck mode");
    model.setFourDecks(true);
    check(model.deckCount() == 4, "four-deck mode shows four decks");
    model.setFourDecks(false);
    model.setAssign(0, 0);
    model.setCrossfaderCurve(0);
    model.setCrossfader(0);
}

void testWaveformFromPcm(const QString &dir)
{
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    model.load(1, songB);
    QByteArray pcm;
    for (int i = 0; i < DjModel::pcmRate * 2; ++i) {
        const qint16 sample = qint16(std::lround(std::sin(2 * M_PI * 440 * i / DjModel::pcmRate) * 12000));
        pcm.append(char(sample & 0xff));
        pcm.append(char((sample >> 8) & 0xff));
    }
    player.onEvent(QStringLiteral("pcm"), QJsonObject{{QStringLiteral("k"), 1},
                                                      {QStringLiteral("id"), songB},
                                                      {QStringLiteral("t"), 1.0},
                                                      {QStringLiteral("d"), QString::fromLatin1(pcm.toBase64())}});
    const DjDeck &deck = model.deck(1);
    int heard = 0;
    for (char on : deck.heard)
        heard += on ? 1 : 0;
    check(heard >= 38 && heard <= 41, "two seconds of audio fill forty waveform bars");
    check(deck.heard.size() > 20 && !deck.heard.at(5) && deck.heard.at(30), "bars are placed at the audio's position in the song");
    check(deck.loudest > 0.2, "the bars measure the loudness");
    const DjWaveBar &bar = deck.bars.at(30);
    check(bar.mid > bar.high && bar.level > 0, "a 440 Hz tone reads as mids");
    player.onEvent(QStringLiteral("pcm"), QJsonObject{{QStringLiteral("k"), 1},
                                                      {QStringLiteral("id"), songA},
                                                      {QStringLiteral("t"), 20.0},
                                                      {QStringLiteral("d"), QString::fromLatin1(pcm.toBase64())}});
    check(model.deck(1).heard.size() < 400, "audio for another song is ignored");
    model.eject(1);
}

void testSaveAndRestore(const QString &dir)
{
    {
        QWidget parent;
        DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
        DjModel model(host(dir), &player);
        model.load(0, songC, QStringLiteral("Despacito"), QStringLiteral("Luis Fonsi"));
        model.deck(0).bpm = 89;
        model.hotCue(0, 0);
        model.setCrossfader(0.5);
        model.setEq(0, 2, -0.5);
        model.toggleKeylock(0);
        model.setFourDecks(true);
        model.save(true);
    }
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    check(std::abs(model.crossfader() - 0.5) < 1e-9, "the crossfader is restored");
    check(model.deck(0).videoId == songC && model.deck(0).title == QLatin1String("Despacito"), "the deck's song is restored");
    check(std::abs(model.deck(0).bpm - 89) < 1e-9, "the BPM is restored");
    check(model.deck(0).hotcues[0] >= 0, "hot cues are restored");
    check(std::abs(model.deck(0).eq[2] + 0.5) < 1e-9 && model.deck(0).keylock, "EQ and key lock are restored");
    check(model.fourDecks(), "four-deck mode is restored");
    check(!model.deck(0).playing, "a restored deck waits to be started");
    model.resetAll();
    check(!model.deck(0).loaded() && model.crossfader() == 0, "Reset All empties the decks and centres the crossfader");
    check(!model.history().isEmpty() || model.library().isEmpty(), "Reset All keeps Recently Played");
}

void testQueueAndAutomix(const QString &dir)
{
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    model.resetAll();
    model.clearQueue();
    model.enqueue(songA, QStringLiteral("First"), QString());
    check(model.deck(0).videoId == songA && model.deck(0).playing, "queueing with nothing playing starts the song at once");
    check(model.shownQueue().isEmpty(), "that song does not wait in the queue");
    model.enqueue(songB, QStringLiteral("Second"), QString());
    check(model.shownQueue().size() == 1, "queueing while a deck plays adds to the queue");
    wait(650);
    tick(player, 0, 195, 200, 1);
    model.setAutomix(true);
    wait(1200);
    check(model.deck(1).videoId == songB, "AUTOMIX loads the queued song onto the idle deck");
    check(model.shownQueue().isEmpty(), "loading from the queue takes the song off it");
    wait(700);
    check(model.automixing() && model.deck(1).playing, "AUTOMIX starts the other deck when the song nears its end");
    model.setAutomix(false);
    check(!model.automixing(), "turning AUTOMIX off stops the mix");
}

void testBrakeStopsTheDeck(const QString &dir)
{
    QWidget parent;
    DjPlayer player(QUrl(QStringLiteral("https://forkmesh.com/")), &parent);
    DjModel model(host(dir), &player);
    model.load(0, songA);
    model.start(0);
    model.brakeDeck(0);
    check(model.deck(0).spinning, "BRAKE starts the slow-down");
    player.onEvent(QStringLiteral("spun"), QJsonObject{{QStringLiteral("k"), 0}, {QStringLiteral("t"), 42.0}});
    check(!model.deck(0).playing && !model.deck(0).keepsPlaying && !model.deck(0).spinning, "the deck stops when the brake ends");
    check(std::abs(model.deck(0).time - 42) < 1e-9, "the deck rests where the record stopped");
    model.sample(0);
    check(model.deck(0).sample.kind == DjSampleStepKind::Idle, "a deck with no length cannot be sampled");
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "a temporary data folder");
    testLoadPlayAndTrack(dir.filePath(QStringLiteral("a")));
    testCrossfaderCurves(dir.filePath(QStringLiteral("b")));
    testWaveformFromPcm(dir.filePath(QStringLiteral("c")));
    testSaveAndRestore(dir.filePath(QStringLiteral("d")));
    testQueueAndAutomix(dir.filePath(QStringLiteral("e")));
    testBrakeStopsTheDeck(dir.filePath(QStringLiteral("f")));
    const int crate = runCrateTests();
    const int logic = runLogicTests();
    if (failures > 0 || crate || logic) {
        std::printf("%d model check(s) failed\n", failures);
        return 1;
    }
    std::printf("All Video Mix DJ Studio checks passed\n");
    return 0;
}
