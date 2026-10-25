#include "DjAutoDj.h"
#include "DjBpm.h"
#include "DjCore.h"
#include "DjJam.h"
#include "DjWave.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace forkmesh::djstudio;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

bool near(double value, double expected, double accuracy) { return std::abs(value - expected) <= accuracy; }

QStringList ids(const QList<DjAutoDjCandidate> &list)
{
    QStringList out;
    for (const DjAutoDjCandidate &candidate : list)
        out.append(candidate.id);
    return out;
}

QJsonObject json(const char *text) { return QJsonDocument::fromJson(QByteArray(text)).object(); }

DjAutoDjCandidate candidate(const QString &id, const QString &title, const QString &author = QString(),
                            double seconds = 0, double views = 0, std::optional<double> likes = std::nullopt,
                            int rank = 0)
{
    DjAutoDjCandidate out;
    out.id = id;
    out.title = title;
    out.author = author;
    out.seconds = seconds;
    out.views = views;
    out.likes = likes;
    out.rank = rank;
    return out;
}

const DjTrack seedTrack{QStringLiteral("dQw4w9WgXcQ"), QStringLiteral("Night Owls - Sunrise (Official Video)"),
                        QStringLiteral("Night Owls"), {}};

void testCandidates()
{
    const QByteArray data = R"({"recommendedVideos": [
      {"videoId": "9bZkp7q19f0", "title": "Lights Out", "author": "Night Owls", "lengthSeconds": 215, "viewCount": 1200000},
      {"videoId": "kJQP7kiw5Fk", "title": "Study Beats", "author": "Quiet Room", "lengthSeconds": 190, "viewCountText": "3.4M views"},
      {"videoId": "9bZkp7q19f0", "title": "Lights Out again"},
      {"type": "playlist", "title": "A playlist"},
      {"title": "No id"}
    ]})";
    const QList<DjAutoDjCandidate> found = autodj::candidates(data);
    check(ids(found) == QStringList({QStringLiteral("9bZkp7q19f0"), QStringLiteral("kJQP7kiw5Fk")}),
          "autodj candidates keep unique videos");
    check(found.size() == 2 && found.at(0).rank == 0 && found.at(1).rank == 1, "autodj candidates rank in order");
    check(found.size() == 2 && found.at(0).views == 1200000, "autodj candidates read viewCount");
    check(found.size() == 2 && near(found.at(1).views, 3400000, 1e-6), "autodj candidates read viewCountText");
    check(found.size() == 2 && found.at(1).seconds == 190, "autodj candidates read length");
    check(ids(autodj::candidates(R"([{"videoId":"JGwWNGJdvx8","title":"Search hit","lengthSeconds":200}])")) ==
              QStringList({QStringLiteral("JGwWNGJdvx8")}),
          "autodj candidates read a search array");
    check(autodj::candidates("nope").isEmpty(), "autodj candidates ignore junk");
}

void testCount()
{
    check(autodj::count(QStringLiteral("1,234,567 views")) == 1234567, "autodj count reads commas");
    check(autodj::count(QStringLiteral("12K views")) == 12000, "autodj count reads K");
    check(autodj::count(QStringLiteral("2.5B")) == 2500000000.0, "autodj count reads B");
    check(autodj::count(QStringLiteral("No views")) == 0, "autodj count reads nothing");
}

void testLikes()
{
    const auto liked = autodj::likes(R"({"likeCount": 52000, "viewCount": 1000000})");
    check(liked && liked->likes == 52000 && liked->views == 1000000, "autodj likes read the video record");
    check(!autodj::likes(R"({"viewCount": 10})"), "autodj likes need a like count");
    check(autodj::likesUrl(QUrl(QStringLiteral("https://inv.example")), QStringLiteral("9bZkp7q19f0")).toString() ==
              QLatin1String("https://inv.example/api/v1/videos/9bZkp7q19f0?fields=likeCount,viewCount"),
          "autodj likes url");
    check(autodj::likesUrl(QUrl(QStringLiteral("https://inv.example")), QStringLiteral("bad")).isEmpty(),
          "autodj likes url needs a video id");
}

void testSongKey()
{
    check(autodj::songKey(QStringLiteral("Night Owls - Sunrise (Official Video)")) == QLatin1String("sunrise"),
          "songKey drops artist and brackets");
    check(autodj::songKey(QStringLiteral("Sunrise [HD] Lyrics")) == QLatin1String("sunrise"), "songKey drops video words");
    check(autodj::songKey(QStringLiteral("Sunrise (Remix)")) == QLatin1String("sunrise"), "songKey drops remix bracket");
    check(autodj::songKey(QStringLiteral("Lights Out")) == QLatin1String("lights out"), "songKey keeps plain titles");
    check(autodj::songKey(QStringLiteral("Official Video - Lights Out")) == QLatin1String("lights out"),
          "songKey uses the tail after a dash");
}

void testEligible()
{
    const QList<DjAutoDjCandidate> found{
        candidate(seedTrack.id, QStringLiteral("Sunrise"), QString(), 200, 0, std::nullopt, 0),
        candidate(QStringLiteral("9bZkp7q19f0"), QStringLiteral("Sunrise (Lyrics)"), QStringLiteral("Fan"), 200, 0, std::nullopt, 1),
        candidate(QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("Lights Out"), QString(), 200, 0, std::nullopt, 2),
        candidate(QStringLiteral("JGwWNGJdvx8"), QStringLiteral("Night Owls 3 hour mix"), QString(), 3 * 3600, 0, std::nullopt, 3),
        candidate(QStringLiteral("OPf0YbXqDm0"), QStringLiteral("Short clip"), QString(), 20, 0, std::nullopt, 4),
        candidate(QStringLiteral("YQHsXMglC9A"), QStringLiteral("Live now"), QString(), 0, 0, std::nullopt, 5),
        candidate(QStringLiteral("fJ9rUzIMcZQ"), QStringLiteral("Heard Before"), QString(), 200, 0, std::nullopt, 6),
        candidate(QStringLiteral("hTWKbfoikeg"), QStringLiteral("On a deck"), QString(), 200, 0, std::nullopt, 7),
        candidate(QStringLiteral("RgKAFK5djSk"), QStringLiteral("Lights Out (Official Audio)"), QString(), 200, 0, std::nullopt, 8),
        candidate(QStringLiteral("CevxZvSJLk8"), QStringLiteral("Fresh Song"), QString(), 240, 0, std::nullopt, 9),
    };
    const auto kept = autodj::eligible(found, seedTrack, {QStringLiteral("hTWKbfoikeg")},
                                       {QStringLiteral("Someone - Heard Before")});
    check(ids(kept) == QStringList({QStringLiteral("RgKAFK5djSk"), QStringLiteral("CevxZvSJLk8")}),
          "eligible leaves out the seed, repeats, other uploads and odd lengths");
}

void testIsSong()
{
    const auto song = [](const char *title, const char *author) {
        return candidate(QStringLiteral("9bZkp7q19f0"), QString::fromUtf8(title), QString::fromUtf8(author), 200);
    };
    check(!autodj::isSong(song("Golden Buzzer: Teen Sings Hallelujah | America's Got Talent 2024", "America's Got Talent")), "isSong rejects AGT");
    check(!autodj::isSong(song("Nervous Singer STUNS The Judges | Auditions | BGT 2023", "Britain's Got Talent")), "isSong rejects BGT");
    check(!autodj::isSong(song("Hallelujah", "The X Factor UK")), "isSong rejects X Factor");
    check(!autodj::isSong(song("Vocal Coach Reacts to Queen", "Music Room")), "isSong rejects reactions");
    check(!autodj::isSong(song("Bohemian Rhapsody (Live Aid 1985)", "Queen")), "isSong rejects live brackets");
    check(!autodj::isSong(song("Queen - Bohemian Rhapsody - Live", "Fan")), "isSong rejects live dash parts");
    check(!autodj::isSong(song("Hallelujah | Acoustic Cover by Sam", "Sam")), "isSong rejects covers");
    check(!autodj::isSong(song("Hallelujah piano cover", "Keys")), "isSong rejects piano covers");
    check(!autodj::isSong(song("Sunrise (Sped Up)", "Edits")), "isSong rejects sped up");
    check(!autodj::isSong(song("Night Owls - Sunrise (Karaoke Version)", "Sing King")), "isSong rejects karaoke");
    check(autodj::isSong(song("Oasis - Live Forever (Remastered)", "Oasis")), "isSong keeps Live Forever");
    check(autodj::isSong(song("Queen - Bohemian Rhapsody (Official Video Remastered)", "Queen Official")), "isSong keeps official remasters");
    check(autodj::isSong(song("Hallelujah", "Jeff Buckley - Topic")), "isSong keeps topic uploads");
    check(autodj::isSong(song("Billy Idol - White Wedding", "Billy Idol")), "isSong keeps Billy Idol");
}

void testIsOriginal()
{
    const auto song = [](const char *title, const char *author) {
        return candidate(QStringLiteral("9bZkp7q19f0"), QString::fromUtf8(title), QString::fromUtf8(author), 200);
    };
    check(autodj::isOriginal(song("Hallelujah", "Jeff Buckley - Topic")), "isOriginal topic channel");
    check(autodj::isOriginal(song("Lights Out", "NightOwlsVEVO")), "isOriginal vevo");
    check(autodj::isOriginal(song("Night Owls - Lights Out (Official Audio)", "Records Ltd")), "isOriginal official");
    check(autodj::isOriginal(song("Oasis - Live Forever (Remastered)", "Big Brother")), "isOriginal remastered");
    check(autodj::isOriginal(song("Night Owls – Lights Out", "Night Owls")), "isOriginal artist matches");
    check(!autodj::isOriginal(song("Night Owls - Lights Out", "Lyrics Hub")), "isOriginal rejects lyric channels");
    check(!autodj::isOriginal(song("Lights Out", "")), "isOriginal rejects unknown uploads");
}

void testEligibleOriginalsFirst()
{
    const QList<DjAutoDjCandidate> found{
        candidate(QStringLiteral("9bZkp7q19f0"), QStringLiteral("Teen Sings Sunrise | AGT 2024"), QStringLiteral("America's Got Talent"), 300, 0, std::nullopt, 0),
        candidate(QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("Lights Out"), QStringLiteral("Lyrics Hub"), 200, 0, std::nullopt, 1),
        candidate(QStringLiteral("JGwWNGJdvx8"), QStringLiteral("Moonset (Live)"), QStringLiteral("Night Owls"), 200, 0, std::nullopt, 2),
        candidate(QStringLiteral("CevxZvSJLk8"), QStringLiteral("Fresh Song"), QStringLiteral("Night Owls - Topic"), 240, 0, std::nullopt, 3),
    };
    check(ids(autodj::eligible(found, seedTrack, {}, {})) ==
              QStringList({QStringLiteral("CevxZvSJLk8"), QStringLiteral("kJQP7kiw5Fk")}),
          "eligible leaves out non-songs and puts originals first");
    const QList<DjAutoDjCandidate> liked{
        candidate(QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("Lights Out"), QStringLiteral("Lyrics Hub"), 200, 9000000, 400000, 0),
        candidate(QStringLiteral("CevxZvSJLk8"), QStringLiteral("Fresh Song"), QStringLiteral("Night Owls - Topic"), 240, 3000, 20, 1),
    };
    check(ids(autodj::ranked(liked)) == QStringList({QStringLiteral("CevxZvSJLk8"), QStringLiteral("kJQP7kiw5Fk")}),
          "ranked puts originals before liked covers");
}

void testRanking()
{
    const QList<DjAutoDjCandidate> found{
        candidate(QStringLiteral("9bZkp7q19f0"), QStringLiteral("Most related, few likes"), QString(), 200, 20000, 150, 0),
        candidate(QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("Loved"), QString(), 200, 5000000, 240000, 1),
        candidate(QStringLiteral("JGwWNGJdvx8"), QStringLiteral("Popular, unchecked"), QString(), 200, 3000000, std::nullopt, 2),
    };
    check(ids(autodj::ranked(found)) ==
              QStringList({QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("JGwWNGJdvx8"), QStringLiteral("9bZkp7q19f0")}),
          "ranked prefers liked songs among related ones");
    const QList<DjAutoDjCandidate> even{
        candidate(QStringLiteral("9bZkp7q19f0"), QStringLiteral("First"), QString(), 200, 1000, 50, 0),
        candidate(QStringLiteral("kJQP7kiw5Fk"), QStringLiteral("Second"), QString(), 200, 1000, 50, 1),
    };
    check(ids(autodj::ranked(even)) == QStringList({QStringLiteral("9bZkp7q19f0"), QStringLiteral("kJQP7kiw5Fk")}),
          "ranked keeps relation order on ties");
}

void testNextAndRemember()
{
    QList<DjAutoDjCandidate> ranked;
    QStringList all;
    for (const char *id : {"9bZkp7q19f0", "kJQP7kiw5Fk", "JGwWNGJdvx8", "OPf0YbXqDm0"}) {
        ranked.append(candidate(QString::fromLatin1(id), QString::fromLatin1(id), QString(), 200));
        all.append(QString::fromLatin1(id));
    }
    const auto next = autodj::next(ranked, {QStringLiteral("9bZkp7q19f0")}, {QStringLiteral("kJQP7kiw5Fk")},
                                   {QStringLiteral("JGwWNGJdvx8")});
    check(next && next->id == QLatin1String("OPf0YbXqDm0"), "next skips loaded, failed and played songs");
    check(!autodj::next(ranked, {}, {}, all), "next gives nothing when everything played");
    check(autodj::remember(QStringLiteral("b"), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) ==
              QStringList({QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c")}),
          "remember keeps newest first without repeats");
    QStringList many;
    for (int i = 0; i < autodj::sessionLimit; ++i)
        many.append(QString::number(i));
    check(autodj::remember(QStringLiteral("x"), many).size() == autodj::sessionLimit, "remember caps the session");
    check(ranked.first().result() == DjSearchResult{QStringLiteral("9bZkp7q19f0"), QStringLiteral("9bZkp7q19f0"), QString(), 200},
          "candidate converts to a search result");
}

quint64 lcg(quint64 &state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

qint16 pcm(float value)
{
    if (!std::isfinite(value))
        return 0;
    return qint16(std::round(std::min(1.0f, std::max(-1.0f, value)) * 32767.0f));
}

QVector<qint16> groove(double rate, double bpm, double seconds = 40, bool hats = true, bool snare = true, quint64 seed = 7)
{
    quint64 random = seed;
    const auto noise = [&]() {
        return double(qint64(lcg(random))) / double(std::numeric_limits<qint64>::max());
    };
    const int count = int(seconds * rate);
    QVector<double> signal(count, 0);
    const double beat = 60 / bpm;
    int index = 0;
    double time = 0.31;
    while (time < seconds) {
        const int start = int(time * rate);
        for (int offset = 0; offset < int(0.12 * rate); ++offset) {
            if (start + offset >= count)
                continue;
            const double t = double(offset) / rate;
            signal[start + offset] += 0.8 * std::sin(2 * M_PI * (55 + 90 * std::exp(-t * 30)) * t) * std::exp(-t * 22);
        }
        if (snare && index % 2 == 1) {
            for (int offset = 0; offset < int(0.09 * rate); ++offset) {
                if (start + offset >= count)
                    continue;
                signal[start + offset] += 0.35 * noise() * std::exp(-double(offset) / rate * 35);
            }
        }
        if (hats) {
            const int hat = int((time + beat / 2) * rate);
            for (int offset = 0; offset < int(0.03 * rate); ++offset) {
                if (hat + offset >= count)
                    continue;
                signal[hat + offset] += 0.2 * noise() * std::exp(-double(offset) / rate * 120);
            }
        }
        index += 1;
        time += beat;
    }
    QVector<qint16> out(count);
    for (int sample = 0; sample < count; ++sample)
        out[sample] = pcm(float(signal.at(sample) + 0.03 * noise()));
    return out;
}

QVector<qint16> clicks(double rate, double bpm, double seconds)
{
    const int count = int(seconds * rate);
    QVector<double> signal(count, 0);
    for (double time = 0.2; time < seconds; time += 60 / bpm) {
        const int start = int(time * rate);
        for (int offset = 0; offset < int(0.02 * rate) && start + offset < count; ++offset) {
            const double t = double(offset) / rate;
            signal[start + offset] += 0.9 * std::sin(2 * M_PI * 1000 * t) * std::exp(-t * 250);
        }
    }
    QVector<qint16> out(count);
    for (int sample = 0; sample < count; ++sample)
        out[sample] = pcm(float(signal.at(sample)));
    return out;
}

bool tempoNear(const std::optional<double> &found, double bpm) { return found && near(*found, bpm, 0.3); }

void testBpm()
{
    const double rate = 22050;
    check(tempoNear(bpm::detect(groove(rate, 128), rate), 128), "bpm finds house tempo");
    check(tempoNear(bpm::detect(groove(rate, 90, 40, true, true, 11), rate), 90), "bpm finds hip hop tempo without doubling");
    check(tempoNear(bpm::detect(groove(rate, 123.4, 40, false, true, 3), rate), 123.4), "bpm finds fractional tempo");
    check(tempoNear(bpm::detect(groove(rate, 140, 40, false, false, 5), rate), 140), "bpm finds tempo of plain kicks");
    check(!bpm::detect(QVector<qint16>(int(rate * 30), 0), rate), "bpm finds nothing in silence");
    quint64 random = 1;
    QVector<qint16> hiss(int(rate * 30));
    for (qint16 &sample : hiss)
        sample = qint16(qint64(lcg(random)) >> 52);
    check(!bpm::detect(hiss, rate), "bpm finds nothing in hiss");
    check(!bpm::detect({1, 2, 3}, rate), "bpm finds nothing in a few samples");
    check(!bpm::detect({}, 0), "bpm finds nothing without a rate");
    for (int n = 0; 72.0 + n * 13.3 < 180; ++n) {
        const double tempo = 72.0 + n * 13.3;
        for (const bool full : {true, false}) {
            const auto found = bpm::detect(groove(rate, tempo, 30, full, full, quint64(tempo * 10)), rate);
            const QByteArray message = QStringLiteral("bpm finds %1 at 22050 Hz (hats and snare %2, got %3)")
                                           .arg(tempo).arg(full).arg(found ? *found : -1).toUtf8();
            check(tempoNear(found, tempo), message.constData());
        }
    }
}

void testBpmAtDesktopRate()
{
    const double rate = 11025;
    check(bpm::hopFor(22050) == bpm::hop, "bpm hop stays 256 at 22050 Hz");
    check(bpm::hopFor(11025) == 128, "bpm hop halves at 11025 Hz");
    check(tempoNear(bpm::detect(clicks(rate, 120, 30), rate), 120), "bpm finds a 120 click track at 11025 Hz");
    check(tempoNear(bpm::detect(clicks(rate, 97.5, 30), rate), 97.5), "bpm finds a 97.5 click track at 11025 Hz");
    check(tempoNear(bpm::detect(clicks(rate, 174, 30), rate), 174), "bpm finds a 174 click track at 11025 Hz");
    check(tempoNear(bpm::detect(groove(rate, 128), rate), 128), "bpm finds house tempo at 11025 Hz");
    check(tempoNear(bpm::detect(groove(rate, 90, 40, true, true, 11), rate), 90), "bpm finds hip hop tempo at 11025 Hz");
    check(!bpm::detect(QVector<qint16>(int(rate * 30), 0), rate), "bpm finds nothing in silence at 11025 Hz");
    for (int n = 0; 72.0 + n * 13.3 < 180; ++n) {
        const double tempo = 72.0 + n * 13.3;
        for (const bool full : {true, false}) {
            const auto found = bpm::detect(groove(rate, tempo, 30, full, full, quint64(tempo * 10)), rate);
            const QByteArray message = QStringLiteral("bpm finds %1 at 11025 Hz (hats and snare %2, got %3)")
                                           .arg(tempo).arg(full).arg(found ? *found : -1).toUtf8();
            check(tempoNear(found, tempo), message.constData());
        }
    }
}

void testWave()
{
    check(DjWaveAccumulator::bars(30) == DjWaveAccumulator::barCount, "wave bars floor at bar count");
    check(DjWaveAccumulator::bars(240) == 4800, "wave bars follow song length");
    check(DjWaveAccumulator::bars(7200) == DjWaveAccumulator::mostBars, "wave bars cap at most bars");
    check(DjWaveAccumulator::bars(std::nan("")) == DjWaveAccumulator::barCount, "wave bars default for nan");

    const double rate = 22050;
    QVector<float> quiet(22050);
    QVector<float> bass(22050);
    QVector<float> treble(22050);
    for (int index = 0; index < 22050; ++index) {
        const double time = double(index) / rate;
        quiet[index] = float(0.05 * std::sin(2 * M_PI * 60 * time));
        bass[index] = float(0.8 * std::sin(2 * M_PI * 60 * time));
        treble[index] = float(0.8 * std::sin(2 * M_PI * 7000 * time));
    }
    DjWaveAccumulator accumulator(rate, 66150, 3);
    accumulator.add(quiet);
    accumulator.add(bass);
    accumulator.add(treble);
    const QVector<DjWaveBar> bars = accumulator.finish(3);
    check(bars.size() == 3, "wave makes three bars");
    if (bars.size() == 3) {
        const auto hue = [](const DjWaveBar &bar) { return bar.mid * 0.33 + bar.high * 0.6; };
        check(bars.at(0).level < 0.2, "wave quiet bar is low");
        check(bars.at(1).level > 0.8, "wave loud bar is high");
        check(hue(bars.at(1)) < 0.15, "wave bass bar is dark");
        check(hue(bars.at(2)) > 0.45, "wave treble bar is bright");
        check(bars.at(1).low > 0.8, "wave bass bar is mostly low band");
        check(bars.at(2).high > 0.5 && bars.at(2).high > bars.at(2).low, "wave treble bar is mostly high band");
        bool sums = true;
        for (const DjWaveBar &bar : bars)
            sums = sums && near(bar.low + bar.mid + bar.high, 1, 1e-9);
        check(sums, "wave bands sum to one");
        const QByteArray encoded = waveCache::encode(bars);
        check(encoded.size() == 12, "wave cache stores four bytes a bar");
        const QVector<DjWaveBar> decoded = waveCache::decode(encoded);
        check(decoded.size() == 3, "wave cache round trips the count");
        check(decoded.size() == 3 && near(decoded.at(1).level, bars.at(1).level, 0.01), "wave cache keeps the level");
        check(decoded.size() == 3 && near(decoded.at(2).high, bars.at(2).high, 0.01) &&
                  near(decoded.at(1).low, bars.at(1).low, 0.01) && near(decoded.at(2).mid, bars.at(2).mid, 0.01),
              "wave cache keeps the bands");
    }
    DjWaveAccumulator silent(rate, 100);
    silent.add(QVector<float>(100, 0));
    check(silent.finish().isEmpty(), "wave of silence is empty");
    check(waveCache::decode(QByteArray(1, char(1))).isEmpty(), "wave cache rejects a short blob");
    check(waveCache::decode(QByteArray(6, char(1))).isEmpty(), "wave cache rejects a ragged blob");
    const QVector<DjWaveBar> floor = waveCache::decode(QByteArray(4, char(0)));
    check(floor.size() == 1 && floor.first().level == 0.04, "wave cache floors the level");
}

void testJam()
{
    const QJsonObject object = json(R"({
      "ok": true,
      "jam": {
        "id": "j1", "title": "Friday", "owner": "alice", "me": "bob", "myStatus": "joined", "now": 10000,
        "members": [{"name": "alice", "status": "owner"}, {"name": "bob", "status": "joined"}, {"name": "cara", "status": "invited"}, {"status": "x"}],
        "queue": [
          {"id": "q1", "videoId": "dQw4w9WgXcQ", "title": "Song", "author": "Band", "addedBy": "bob", "addedAt": 1792000000000},
          {"id": "q2", "videoId": "bad"},
          {"videoId": "dQw4w9WgXcQ"}
        ],
        "decks": {
          "a": {"videoId": "9bZkp7q19f0", "title": "Other", "playing": true, "position": 30, "rate": 1.1, "at": 8000, "by": "alice"},
          "b": {"videoId": "nope", "rate": 9}
        }
      }
    })");
    const auto jam = DjJam::parse(object);
    check(jam.has_value(), "jam parses");
    if (!jam)
        return;
    check(jam->id == QLatin1String("j1"), "jam reads id");
    check(!jam->isOwner(), "jam knows I am not the owner");
    check(jam->joined(), "jam knows I joined");
    QStringList members;
    for (const DjJamMember &member : jam->members)
        members.append(member.name);
    check(members == QStringList({QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("cara")}), "jam reads named members");
    QStringList listeners;
    for (const DjJamMember &member : jam->listeners())
        listeners.append(member.name);
    check(listeners == QStringList({QStringLiteral("alice"), QStringLiteral("bob")}), "jam listeners are joined members");
    const DjQueueItem item{QStringLiteral("q1"), QStringLiteral("dQw4w9WgXcQ"), QStringLiteral("Song"),
                           QStringLiteral("Band"), QStringLiteral("bob"), 1792000000000};
    check(jam->queue == QList<DjQueueItem>{item}, "jam reads valid queue items with ids");
    check(jam->deck(DjSide::A).videoId == QLatin1String("9bZkp7q19f0"), "jam reads deck a");
    check(near(jam->deck(DjSide::A).expectedPosition(jam->now), 32.2, 1e-9), "jam deck predicts its position");
    check(jam->deck(DjSide::B).videoId.isEmpty(), "jam drops a bad deck video");
    check(jam->deck(DjSide::B).rate == 1 + studio::pitchRange, "jam clamps deck rate");
    check(jam->deck(DjSide::B).expectedPosition(99000) == 0, "jam paused deck stays put");
    check(!DjJam::parse(json(R"({"ok": true})")), "jam needs a jam object");
}

void testJamListAndSeek()
{
    const auto list = DjJamSummary::list(json(R"({"jams": [{"id": "j1", "title": "T", "owner": "alice", "myStatus": "invited", "members": 3}, {"title": "no id"}]})"));
    check(list == QList<DjJamSummary>{DjJamSummary{QStringLiteral("j1"), QStringLiteral("T"), QStringLiteral("alice"),
                                                   QStringLiteral("invited"), 3}},
          "jam list reads summaries");
    check(!list.isEmpty() && list.first().invited(), "jam list knows invitations");
    check(!DjJam::needsSeek(10, 11, true), "jam no seek within 1.5 s while playing");
    check(DjJam::needsSeek(10, 12, true), "jam seeks beyond 1.5 s while playing");
    check(DjJam::needsSeek(10, 10.8, false), "jam seeks beyond 0.5 s while paused");
    check(!DjJam::needsSeek(std::nan(""), 10, false), "jam never seeks on nan");
    check(DjJam::userNames(json(R"({"users": [{"name": "Bob "}, {"name": "bob"}, {"name": "alice"}, {"x": 1}]})"),
                           {QStringLiteral("alice")}) == QStringList({QStringLiteral("bob")}),
          "jam user names are unique lowercase");
}

void testDeckBody()
{
    DjJamDeck deck;
    deck.videoId = QStringLiteral("dQw4w9WgXcQ");
    deck.title = QStringLiteral("Song");
    deck.playing = true;
    deck.position = 12.34567;
    deck.rate = 1.02;
    const QJsonObject body = deck.body();
    check(body.value(QStringLiteral("position")).toDouble() == 12.346, "deck body rounds position");
    check(body.value(QStringLiteral("playing")).toBool(), "deck body sends playing");
    check(body.value(QStringLiteral("videoId")).toString() == QLatin1String("dQw4w9WgXcQ"), "deck body sends video id");
    check(body.value(QStringLiteral("rate")).toDouble() == 1.02, "deck body sends rate");
}

void testQueueItems()
{
    const QJsonArray rows = QJsonDocument::fromJson(R"([{"id":"a","videoId":"dQw4w9WgXcQ","title":"Song"},{"videoId":"bad"}])").array();
    QList<DjQueueItem> items;
    for (const QJsonValue &row : rows) {
        if (const auto item = DjQueueItem::fromJson(row.toObject()))
            items.append(*item);
    }
    check(items.size() == 1 && items.first().videoId == QLatin1String("dQw4w9WgXcQ"), "queue items decode safely");
    check(items.size() == 1 && items.first().addedAt == 0, "queue items default addedAt");
    const DjQueueItem saved{QStringLiteral("b"), QStringLiteral("dQw4w9WgXcQ"), QStringLiteral("Song"),
                            QStringLiteral("Band"), QStringLiteral("bob"), 42};
    check(DjQueueItem::fromJson(saved.toJson()) == saved, "queue items round trip");
}

void testReactions()
{
    QSet<QString> reactionIds;
    QSet<QString> emojis;
    QSet<int> styles;
    bool shaped = true;
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9-]{1,24}$"));
    for (const DjReaction &reaction : DjReaction::all()) {
        reactionIds.insert(reaction.id);
        emojis.insert(reaction.emoji);
        styles.insert(int(reaction.style));
        shaped = shaped && pattern.match(reaction.id).hasMatch() && reaction.seconds() > 1;
    }
    check(DjReaction::all().size() == 30, "thirty reactions");
    check(reactionIds.size() == 30 && emojis.size() == 30, "reactions are distinct");
    check(styles.size() == 10, "reactions cover every style");
    check(shaped, "reaction ids are short and last over a second");
    const DjReaction *fire = DjReaction::find(QStringLiteral("fire"));
    check(fire && fire->emoji == QStringLiteral("🔥"), "reaction find fire");
    check(!DjReaction::find(QStringLiteral("nope")), "reaction find unknown");
}

void testPresence()
{
    const QJsonObject object = json(R"({
      "jam": {
        "id": "j1", "me": "bob", "myStatus": "joined", "now": 50000,
        "members": [{"name": "alice", "status": "owner"}, {"name": "bob", "status": "joined"}, {"name": "cara", "status": "joined"},
                    {"name": "dan", "status": "joined"}, {"name": "eve", "status": "invited"}],
        "presence": [
          {"name": "alice", "state": "studio", "playing": true, "energy": 0.8, "motion": [[0.5, -0.5], [9, 1], ["x", 1], [1]], "ago": 400},
          {"name": "bob", "state": "listening", "playing": true, "ago": 1000},
          {"name": "cara", "state": "studio", "ago": 60000},
          {"state": "studio"}
        ],
        "reactions": [{"id": "r1", "name": "alice", "kind": "hearts", "ago": 300}, {"id": "r2", "kind": "made-up"}, {"kind": "fire"}]
      }
    })");
    const auto jam = DjJam::parse(object);
    check(jam.has_value(), "presence jam parses");
    if (!jam)
        return;
    QStringList names;
    for (const DjJamPresence &seen : jam->presence)
        names.append(seen.name);
    check(names == QStringList({QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("cara")}), "presence keeps named rows");
    const auto alice = jam->presenceOf(QStringLiteral("alice"));
    check(alice && alice->dancing(), "presence alice is dancing");
    check(alice && alice->motion == QList<DjMotionSample>{{0.5, -0.5}, {2, 1}}, "presence motion is parsed and clamped");
    check(jam->reactions == QList<DjJamReaction>{DjJamReaction{QStringLiteral("r1"), QStringLiteral("alice"), QStringLiteral("hearts"), 300}},
          "reactions keep known kinds with ids");
    QList<DjPersonStatus> statuses;
    for (const DjJamMember &member : jam->members)
        statuses.append(jam->status(member));
    check(statuses == QList<DjPersonStatus>{DjPersonStatus::Studio, DjPersonStatus::Listening, DjPersonStatus::Away,
                                            DjPersonStatus::Away, DjPersonStatus::Invited},
          "presence gives each member a status");
    check(isLive(DjPersonStatus::Studio), "studio is live");
    check(!isLive(DjPersonStatus::Away), "away is not live");
    check(statusLabel(DjPersonStatus::Idle) == QLatin1String("Not listening") &&
              statusLabel(DjPersonStatus::Listening) == QLatin1String("Listening in the background") &&
              statusLabel(DjPersonStatus::Studio) == QLatin1String("In the studio") &&
              statusLabel(DjPersonStatus::Invited) == QLatin1String("Invited") &&
              statusLabel(DjPersonStatus::Away) == QLatin1String("Away"),
          "status labels");
    DjJam later = *jam;
    later.now = 99;
    later.presence.clear();
    later.reactions.clear();
    check(later.shared() == jam->shared(), "shared ignores clock, presence and reactions");
    later.queue = {DjQueueItem{QStringLiteral("q"), QStringLiteral("dQw4w9WgXcQ"), QString(), QString(), QString(), 0}};
    check(!(later.shared() == jam->shared()), "shared notices queue changes");
    const QJsonObject body = DjJamPresence::body(DjPresenceState::Studio, true);
    check(body.value(QStringLiteral("state")).toString() == QLatin1String("studio"), "presence body sends state");
    check(body.value(QStringLiteral("playing")).toBool(), "presence body sends playing");
    check(body.value(QStringLiteral("energy")).toDouble(-1) == 0, "presence body sends no energy");
    check(body.value(QStringLiteral("motion")).isArray() && body.value(QStringLiteral("motion")).toArray().isEmpty(),
          "presence body sends no motion");
    check(DjJamPresence::body(DjPresenceState::Listening, false).value(QStringLiteral("state")).toString() ==
              QLatin1String("listening"),
          "presence body sends listening");
}

}

int runLogicTests()
{
    testCandidates();
    testCount();
    testLikes();
    testSongKey();
    testEligible();
    testIsSong();
    testIsOriginal();
    testEligibleOriginalsFirst();
    testRanking();
    testNextAndRemember();
    testBpm();
    testBpmAtDesktopRate();
    testWave();
    testJam();
    testJamListAndSeek();
    testDeckBody();
    testQueueItems();
    testReactions();
    testPresence();

    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All DJ logic checks passed\n");
    return 0;
}
