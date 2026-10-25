#include "DjCore.h"
#include "DjCrate.h"
#include "DjSmart.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>

#include <cstdio>

using namespace forkmesh::djstudio;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++failures;
    }
}

const QString one = QStringLiteral("dQw4w9WgXcQ");
const QString two = QStringLiteral("9bZkp7q19f0");
const QString three = QStringLiteral("kJQP7kiw5Fk");
const QString four = QStringLiteral("JGwWNGJdvx8");
const QString five = QStringLiteral("OPf0YbXqDm0");

QStringList ids(const QList<DjTrack> &tracks)
{
    QStringList out;
    for (const DjTrack &track : tracks)
        out.append(track.id);
    return out;
}

QStringList ids(const QList<DjPlayedSong> &songs)
{
    QStringList out;
    for (const DjPlayedSong &song : songs)
        out.append(song.id);
    return out;
}

QStringList ids(const QList<DjSimilarSong> &songs)
{
    QStringList out;
    for (const DjSimilarSong &song : songs)
        out.append(song.id());
    return out;
}

QStringList titles(const QList<DjTrack> &tracks)
{
    QStringList out;
    for (const DjTrack &track : tracks)
        out.append(track.title);
    return out;
}

QSet<QString> setOf(const QStringList &values) { return QSet<QString>(values.begin(), values.end()); }

QString joined(const QStringList &values) { return values.join(QLatin1Char(',')); }

QStringList playlistSongs(const DjCrate &crate, const QString &id)
{
    const auto list = crate.playlist(id);
    return list ? list->songs : QStringList{QStringLiteral("<missing>")};
}

QStringList trackTags(const DjCrate &crate, const QString &id)
{
    const auto track = crate.track(id);
    return track ? track->tags : QStringList{QStringLiteral("<missing>")};
}

void testTagNamesAreTidied()
{
    check(crate::tagName(QStringLiteral("  #Deep   House ")) == QStringLiteral("deep house"), "tag name is tidied");
    check(crate::tagNames(QStringLiteral("Chill, #chill, warm up,,")) ==
              QStringList{QStringLiteral("chill"), QStringLiteral("warm up")},
          "tag names split, deduplicated and tidied");
    check(crate::tagName(QString(50, QLatin1Char('a'))).size() == crate::tagLength, "tag name is cut to the length");
    check(crate::tagName(QStringLiteral(" # ")).isEmpty(), "a tag of only separators is empty");
}

void testTaggingKeepsTheSongAndCountsTags()
{
    DjCrate crate;
    check(crate.tag(one, QStringLiteral("chill, House"), QStringLiteral("First"), QStringLiteral("Someone")),
          "tagging a new song keeps it");
    check(crate.tag(two, QStringLiteral("house")), "tagging a second song");
    check(!crate.tag(QStringLiteral("short"), QStringLiteral("house")), "a bad id is not tagged");
    check(!crate.tag(three, QStringLiteral(" , ")), "empty tags are refused");
    check(crate.track(one) && crate.track(one)->title == QStringLiteral("First"), "tagging saves the title");
    check(trackTags(crate, one) == QStringList{QStringLiteral("chill"), QStringLiteral("house")}, "tags are lowercased");
    check(crate.tagCounts() == QList<DjTagCount>{{QStringLiteral("chill"), 1}, {QStringLiteral("house"), 2}},
          "tag counts are sorted by name");
    check(ids(crate.songs(DjFocus::tag(QStringLiteral("house")))) == QStringList{one, two}, "a tag lists its songs");
    crate.untag(one, QStringLiteral("house"));
    check(ids(crate.songs(DjFocus::tag(QStringLiteral("house")))) == QStringList{two}, "untagging removes the song");
}

void testRenamingAndDeletingTagsFollowTheFocus()
{
    DjCrate crate;
    crate.tag(one, QStringLiteral("house, deep"));
    crate.tag(two, QStringLiteral("deep"));
    crate.setFocus(DjFocus::tag(QStringLiteral("deep")));
    check(crate.renameTag(QStringLiteral("deep"), QStringLiteral("House")), "renaming a tag");
    check(trackTags(crate, one) == QStringList{QStringLiteral("house")}, "renaming onto an existing tag merges");
    check(trackTags(crate, two) == QStringList{QStringLiteral("house")}, "renaming changes every song");
    check(crate.focus == DjFocus::tag(QStringLiteral("house")), "the focus follows the renamed tag");
    crate.deleteTag(QStringLiteral("house"));
    check(crate.focus == DjFocus::everything(), "deleting the focused tag falls back to everything");
    check(crate.tagCounts().isEmpty(), "no tags are left");
    check(!crate.renameTag(QStringLiteral("house"), QStringLiteral("  ")), "renaming to nothing is refused");
}

void testPlaylistsKeepOrderNamesAndMembers()
{
    DjCrate crate;
    check(crate.createPlaylist(QStringLiteral("  Friday   Night "), QStringLiteral("p1")) == QStringLiteral("p1"),
          "create returns the id");
    check(crate.playlist(QStringLiteral("p1"))->name == QStringLiteral("Friday Night"), "playlist name is tidied");
    check(crate.createPlaylist(QStringLiteral("friday night"), QStringLiteral("p2")) == QStringLiteral("p2"),
          "second playlist is created");
    check(crate.playlist(QStringLiteral("p2"))->name == QStringLiteral("friday night 2"),
          "a taken name gets a number");
    crate.renamePlaylist(QStringLiteral("p2"), QStringLiteral("Sunday"));
    check(crate.playlist(QStringLiteral("p2"))->name == QStringLiteral("Sunday"), "renaming a playlist");
    check(crate.add({crate::makeTrack(one, QStringLiteral("A")), crate::makeTrack(two), crate::makeTrack(one),
                     crate::makeTrack(QStringLiteral("bad"))},
                    QStringLiteral("p1")) == 2,
          "adding skips duplicates and bad ids");
    check(crate.add({crate::makeTrack(three)}, QStringLiteral("p1")) == 1, "adding one more");
    check(playlistSongs(crate, QStringLiteral("p1")) == QStringList{one, two, three}, "playlist keeps order");
    crate.moveSongs(QStringLiteral("p1"), {2}, 0);
    check(playlistSongs(crate, QStringLiteral("p1")) == QStringList{three, one, two}, "moving a song up");
    crate.moveSongs(QStringLiteral("p1"), {0}, 3);
    check(playlistSongs(crate, QStringLiteral("p1")) == QStringList{one, two, three}, "moving a song to the end");
    crate.removeSongs({1}, QStringLiteral("p1"));
    check(ids(crate.songs(DjFocus::playlist(QStringLiteral("p1")))) == QStringList{one, three},
          "removing a song from a playlist");
    check(crate.track(two).has_value(), "the removed song stays in the library");
    crate.setFocus(DjFocus::playlist(QStringLiteral("p1")));
    crate.removeTrack(one);
    check(playlistSongs(crate, QStringLiteral("p1")) == QStringList{three}, "removing a track leaves every playlist");
    crate.deletePlaylist(QStringLiteral("p1"));
    check(crate.focus == DjFocus::everything(), "deleting the focused playlist falls back to everything");
}

void testMovingASongLeavesTheFocusedList()
{
    DjCrate crate;
    crate.createPlaylist(QStringLiteral("Main"), QStringLiteral("main"));
    crate.createPlaylist(QStringLiteral("Later"), QStringLiteral("later"));
    crate.add({crate::makeTrack(one, QStringLiteral("One")), crate::makeTrack(two)}, QStringLiteral("main"));
    crate.setFocus(DjFocus::playlist(QStringLiteral("main")));
    check(crate.move(one, DjFocus::playlist(QStringLiteral("later"))) == DjMove::Moved, "move between playlists");
    check(playlistSongs(crate, QStringLiteral("main")) == QStringList{two}, "the song left the focused playlist");
    check(playlistSongs(crate, QStringLiteral("later")) == QStringList{one}, "the song joined the target");
    check(crate.move(three, DjFocus::tag(QStringLiteral("chill")), QStringLiteral("Three")) == DjMove::Added,
          "a song outside the focus is only added");
    check(trackTags(crate, three) == QStringList{QStringLiteral("chill")}, "the tag was added");
    check(crate.move(two, DjFocus::playlist(QStringLiteral("main"))) == DjMove::Added,
          "moving onto the focused list only adds");
    check(playlistSongs(crate, QStringLiteral("main")) == QStringList{two}, "the focused list is unchanged");
    check(crate.move(two, DjFocus::everything()) == DjMove::Removed, "moving to everything removes");
    check(playlistSongs(crate, QStringLiteral("main")).isEmpty(), "the focused playlist is empty");
    check(crate.focus == DjFocus::playlist(QStringLiteral("main")), "an empty playlist keeps the focus");

    crate.tag(four, QStringLiteral("loud"));
    crate.tag(one, QStringLiteral("loud"));
    crate.setFocus(DjFocus::tag(QStringLiteral("loud")));
    check(crate.move(four, DjFocus::tag(QStringLiteral("chill"))) == DjMove::Moved, "move between tags");
    check(trackTags(crate, four) == QStringList{QStringLiteral("chill")}, "the old tag was taken off");
    check(crate.focus == DjFocus::tag(QStringLiteral("loud")), "the tag still has a song");
    check(crate.move(one, DjFocus::playlist(QStringLiteral("main"))) == DjMove::Moved, "move from a tag to a playlist");
    check(crate.focus == DjFocus::everything(), "the emptied tag drops the focus");
    check(crate.move(QStringLiteral("short"), DjFocus::tag(QStringLiteral("x"))) == DjMove::Full,
          "a bad id cannot be moved");
    check(crate.move(one, DjFocus::playlist(QStringLiteral("gone"))) == DjMove::Full,
          "a missing playlist reports full");
}

void testFocusOnAPlaylistPlaysItInOrderAndWraps()
{
    DjCrate crate;
    crate.createPlaylist(QStringLiteral("Set"), QStringLiteral("p"));
    crate.add({crate::makeTrack(one), crate::makeTrack(two), crate::makeTrack(three)}, QStringLiteral("p"));
    crate.setFocus(DjFocus::playlist(QStringLiteral("p")));
    crate.shuffle = false;
    DjPlayedSong elsewhere;
    elsewhere.id = four;
    elsewhere.title = QStringLiteral("Elsewhere");
    const QList<DjPlayedSong> library{elsewhere};
    const auto pick = [&](const QSet<QString> &loaded, const QSet<QString> &playing, const QSet<QString> &skip = {}) {
        const auto song = crate.next(library, loaded, playing, 1, skip);
        return song ? song->id : QString();
    };
    check(pick({}, {}) == one, "in order starts at the first song");
    check(pick({one}, {one}) == two, "in order moves to the second");
    check(pick({one, two}, {two}) == three, "in order moves to the third");
    check(pick({two, three}, {three}) == one, "in order wraps around");
    check(pick({}, {}, {two}) == three, "skipped songs are passed over");
}

void testShuffledFocusStaysInsideTheTag()
{
    DjCrate crate;
    crate.tag(one, QStringLiteral("chill"));
    crate.tag(two, QStringLiteral("chill"));
    crate.tag(three, QStringLiteral("loud"));
    crate.setFocus(DjFocus::tag(QStringLiteral("chill")));
    DjPlayedSong heard;
    heard.id = four;
    heard.plays = 9;
    heard.last = 1;
    DjPlayedSong played;
    played.id = one;
    played.title = QStringLiteral("Played");
    played.plays = 3;
    played.last = 50;
    const QList<DjPlayedSong> library{heard, played};
    bool inside = true;
    for (quint64 seed = 0; seed < 40; ++seed) {
        DjCrate copy = crate;
        const auto song = copy.next(library, {}, {}, seed);
        if (!song || (song->id != one && song->id != two))
            inside = false;
    }
    check(inside, "shuffled picks stay inside the tag");
    DjCrate copy = crate;
    const auto song = copy.next(library, {two}, {two}, 3);
    check(song && song->id == one, "a loaded song is not picked again");
    const QList<DjPlayedSong> pool = crate.pool(library);
    const auto found = std::find_if(pool.begin(), pool.end(), [](const DjPlayedSong &s) { return s.id == one; });
    check(found != pool.end() && found->plays == 3, "the pool keeps the play count");
    check(found != pool.end() && found->title == QStringLiteral("Played"), "the pool keeps the played title");
    crate.setFocus(DjFocus::tag(QStringLiteral("nobody")));
    check(crate.focus == DjFocus::everything(), "an unknown tag falls back to everything");
}

void testEverythingAddsKeptSongsToThePlayedOnes()
{
    DjCrate crate;
    crate.tag(two, QStringLiteral("new"));
    DjPlayedSong song;
    song.id = one;
    const QList<DjPlayedSong> library{song};
    check(ids(crate.pool(library)) == QStringList{one, two}, "everything is played plus kept songs");
    check(ids(crate.pool(library, {one})) == QStringList{two}, "skip removes from the pool");
    check(DjCrate().pool({}).isEmpty(), "an empty crate has an empty pool");
    DjCrate empty;
    check(!empty.next({}, {}, {}, 0).has_value(), "nothing to pick from");
}

void testSavedCrateRoundTripsAndDropsBadEntries()
{
    DjCrate crate;
    crate.tag(one, QStringLiteral("chill"), QStringLiteral("One"));
    crate.createPlaylist(QStringLiteral("Mine"), QStringLiteral("p"));
    crate.add({crate::makeTrack(one), crate::makeTrack(two)}, QStringLiteral("p"));
    crate.setFocus(DjFocus::playlist(QStringLiteral("p")));
    crate.shuffle = false;
    check(DjCrate::decode(crate.encoded()) == crate, "the crate round trips");
    check(DjCrate::fromJson(crate.toJson()) == crate, "the crate round trips through json objects");
    const QString raw = QStringLiteral("{\"tracks\":[{\"id\":\"%1\",\"tags\":[\"A\"]},{\"id\":\"bad\"},{\"id\":\"%1\"}],"
                                       "\"playlists\":[{\"id\":\"p\",\"name\":\"x\",\"songs\":[\"%1\",\"%2\"]}],"
                                       "\"focus\":\"playlist:gone\",\"shuffle\":false}")
                            .arg(one, three);
    const DjCrate decoded = DjCrate::decode(raw.toUtf8());
    check(ids(decoded.tracks) == QStringList{one}, "bad and duplicate tracks are dropped");
    check(!decoded.tracks.isEmpty() && decoded.tracks.first().tags == QStringList{QStringLiteral("a")},
          "decoded tags are tidied");
    check(playlistSongs(decoded, QStringLiteral("p")) == QStringList{one}, "playlist songs must be saved tracks");
    check(decoded.focus == DjFocus::everything(), "an unknown focus falls back");
    check(!decoded.shuffle, "shuffle is read");
    check(DjCrate::decode(QByteArray("nope")) == DjCrate(), "garbage decodes to an empty crate");
    check(DjFocus::fromKey(DjFocus::tag(QStringLiteral("deep house")).key()) == DjFocus::tag(QStringLiteral("deep house")),
          "a tag focus key round trips");
    check(DjFocus::fromKey(QStringLiteral("tag: ")) == DjFocus::everything(), "an empty tag key is everything");
}

void testExportImportsBackIntoAnotherCrate()
{
    DjCrate crate;
    crate.tag(one, QStringLiteral("chill"), QStringLiteral("One"), QStringLiteral("Artist"));
    crate.createPlaylist(QStringLiteral("Warm Up"), QStringLiteral("p"));
    crate.add({crate::makeTrack(two, QStringLiteral("Two")), crate::makeTrack(one)}, QStringLiteral("p"));
    const QByteArray data =
        crate.exported(DjFocus::everything(), QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC));
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    check(object.value(QStringLiteral("format")).toString() == QLatin1String(crate::format), "export has the format");
    check(object.value(QStringLiteral("version")).toInt() == 1, "export has version 1");
    check(object.value(QStringLiteral("exported")).toString() == QStringLiteral("1970-01-01T00:00:00Z"),
          "export has the date", object.value(QStringLiteral("exported")).toString());
    const auto parsed = DjCrate::parse(data);
    check(parsed.has_value(), "the export parses");
    if (!parsed)
        return;
    check(ids(parsed->tracks) == QStringList{one, two}, "parsed tracks");
    check(parsed->playlists == QList<DjImportedList>{{QStringLiteral("Warm Up"), {two, one}}}, "parsed playlists");

    DjCrate other;
    other.tag(two, QStringLiteral("loud"));
    other.createPlaylist(QStringLiteral("warm up"), QStringLiteral("mine"));
    other.add({crate::makeTrack(three)}, QStringLiteral("mine"));
    const DjMergeReport report = other.merge(*parsed, QStringLiteral("Imported"));
    check(report == DjMergeReport{1, 1, false}, "merge report counts");
    check(report.summary() == QStringLiteral("Imported 1 new song into 1 playlist."), "merge summary",
          report.summary());
    check(other.playlists.size() == 1, "a playlist with the same name is reused");
    check(playlistSongs(other, QStringLiteral("mine")) == QStringList{three, two, one}, "songs are appended");
    check(other.track(two)->title == QStringLiteral("Two"), "an empty title is filled in");
    check(trackTags(other, two) == QStringList{QStringLiteral("loud")}, "existing tags are kept");
    check(trackTags(other, one) == QStringList{QStringLiteral("chill")}, "imported tags arrive");
}

void testTagExportCarriesTagsWithoutMakingAPlaylist()
{
    DjCrate crate;
    crate.tag(one, QStringLiteral("chill"));
    crate.tag(two, QStringLiteral("loud"));
    const auto parsed = DjCrate::parse(crate.exported(DjFocus::tag(QStringLiteral("chill"))));
    check(parsed.has_value() && ids(parsed->tracks) == QStringList{one}, "a tag export holds its songs");
    if (!parsed)
        return;
    DjCrate other;
    check(other.merge(*parsed, QStringLiteral("Imported")) == DjMergeReport{1, 0, false},
          "tagged songs merge without a playlist");
    check(other.playlists.isEmpty(), "no playlist was made");
    check(ids(other.songs(DjFocus::tag(QStringLiteral("chill")))) == QStringList{one}, "the tag came along");
}

void testLinksAndM3uBecomeAPlaylist()
{
    DjCrate crate;
    crate.createPlaylist(QStringLiteral("Set"), QStringLiteral("p"));
    crate.add({crate::makeTrack(one, QStringLiteral("One\nline")), crate::makeTrack(two)}, QStringLiteral("p"));
    const QByteArray m3u = crate.m3u(DjFocus::playlist(QStringLiteral("p")));
    const QString expected = QStringLiteral("#EXTM3U\n#EXTINF:-1,One line\nhttps://www.youtube.com/watch?v=%1\n"
                                            "#EXTINF:-1,YouTube video\nhttps://www.youtube.com/watch?v=%2\n")
                                 .arg(one, two);
    check(m3u == expected.toUtf8(), "m3u export", QString::fromUtf8(m3u));
    const auto parsed = DjCrate::parse(m3u);
    check(parsed && titles(parsed->tracks) == QStringList{QStringLiteral("One line"), QStringLiteral("YouTube video")},
          "m3u titles are read back");

    const QString text = QStringLiteral("My set\nhttps://youtu.be/%1?si=x\n\nwatch this "
                                        "https://www.youtube.com/watch?v=%2&t=3\nhttps://youtu.be/%1\n"
                                        "https://example.com\n")
                             .arg(three, four);
    const auto links = DjCrate::parse(text.toUtf8());
    check(links && ids(links->tracks) == QStringList{three, four}, "links in text are read once each");
    if (links) {
        DjCrate other;
        check(other.merge(*links, QStringLiteral("Pasted")) == DjMergeReport{2, 1, false},
              "plain links become a playlist");
        check(!other.playlists.isEmpty() && other.playlists.first().name == QStringLiteral("Pasted"),
              "the playlist is named after the fallback");
        check(!other.playlists.isEmpty() && other.playlists.first().songs == QStringList{three, four},
              "the playlist holds the links");
    }
    check(!DjCrate::parse(QByteArray("nothing here")).has_value(), "text without links is refused");
    check(!DjCrate::parse(QByteArray()).has_value(), "empty data is refused");
}

void testOtherJsonShapesImport()
{
    const QString array = QStringLiteral("[\"https://youtu.be/%1\", {\"videoId\":\"%2\",\"title\":\"Two\","
                                         "\"artist\":\"Band\",\"tags\":\"a, b\"}, {\"url\":\"nope\"}]")
                              .arg(one, two);
    const auto parsed = DjCrate::parse(array.toUtf8());
    check(parsed && ids(parsed->tracks) == QStringList{one, two}, "a json list of links and objects");
    if (parsed) {
        check(parsed->tracks.last().author == QStringLiteral("Band"), "artist is read as the author");
        check(parsed->tracks.last().tags == QStringList{QStringLiteral("a"), QStringLiteral("b")},
              "a tag string is split");
        check(parsed->playlists.isEmpty(), "a bare list has no playlists");
    }
    const QString named =
        QStringLiteral("{\"name\":\"Road Trip\",\"videos\":[{\"link\":\"https://www.youtube.com/watch?v=%1\"}]}")
            .arg(three);
    const auto road = DjCrate::parse(named.toUtf8());
    check(road && road->playlists == QList<DjImportedList>{{QStringLiteral("Road Trip"), {three}}},
          "a named object becomes a playlist");
}

void testFileNamesAreSafe()
{
    DjCrate crate;
    crate.createPlaylist(QStringLiteral("Late/Night: Set"), QStringLiteral("p"));
    check(crate.fileName(DjFocus::playlist(QStringLiteral("p"))) == QStringLiteral("Late-Night- Set"),
          "unsafe characters become dashes");
    check(crate.fileName(DjFocus::tag(QStringLiteral("deep house"))) == QStringLiteral("Tag deep house"),
          "tag file name");
    check(crate.fileName(DjFocus::everything()) == QStringLiteral("DJ Playlists"), "everything file name");
}

void testDontShuffleSongsAreLeftOutOfShuffledPicksButPlayInOrder()
{
    DjCrate crate;
    crate.createPlaylist(QStringLiteral("Set"), QStringLiteral("p"));
    crate.add({crate::makeTrack(one), crate::makeTrack(two), crate::makeTrack(three)}, QStringLiteral("p"));
    check(crate.setShuffles(two, false), "a song can be left out of shuffle");
    check(crate.setShuffles(four, false, QStringLiteral("Saved")), "leaving out a new song saves it");
    check(crate.track(four) && crate.track(four)->title == QStringLiteral("Saved"), "the new song has its title");
    check(!crate.shuffles(two), "shuffles reports the flag");
    check(crate.unshuffled() == QSet<QString>{two, four}, "unshuffled lists both");
    DjPlayedSong first;
    first.id = one;
    DjPlayedSong second;
    second.id = two;
    const QList<DjPlayedSong> library{first, second};
    check(setOf(ids(crate.pool(library))) == QSet<QString>{one, three}, "the shuffled pool leaves them out");
    bool never = true;
    for (quint64 seed = 0; seed < 20; ++seed) {
        const auto song = crate.next(library, {}, {}, seed);
        if (song && song->id == two)
            never = false;
    }
    check(never, "a don't-shuffle song is never picked at random");
    crate.setFocus(DjFocus::playlist(QStringLiteral("p")));
    crate.shuffle = false;
    check(ids(crate.pool({})) == QStringList{one, two, three}, "in order keeps every song");
    const auto ordered = crate.next({}, {one}, {}, 0);
    check(ordered && ordered->id == two, "in order plays the don't-shuffle song");
    const DjCrate saved = DjCrate::decode(crate.encoded());
    check(saved.unshuffled() == QSet<QString>{two, four}, "the flag is saved");
    const QByteArray exported = crate.exported(DjFocus::everything());
    check(exported.count("noShuffle") == 2, "the flag is exported only when set");
    DjCrate other;
    const auto parsed = DjCrate::parse(crate.exported(DjFocus::everything()));
    check(parsed.has_value(), "the export with flags parses");
    if (parsed)
        other.merge(*parsed, QStringLiteral("In"));
    check(other.unshuffled() == QSet<QString>{two, four}, "the flag is imported");
    check(crate.setShuffles(two, true), "shuffle can be turned back on");
    check(crate.shuffles(two), "the song shuffles again");
    crate.removeTrack(four);
    check(crate.unshuffled().isEmpty(), "removing the track drops its flag");
}

QList<DjTrack> smartSongs()
{
    return {
        crate::makeTrack(one, QStringLiteral("Sunrise (Deep House Mix) 1998"), QStringLiteral("Night Owls - Topic")),
        crate::makeTrack(two, QStringLiteral("Lights Out - Deep House Remix"), QStringLiteral("Night Owls")),
        crate::makeTrack(three, QStringLiteral("Study Beats"), QStringLiteral("Quiet Room"), {QStringLiteral("lofi")}),
        crate::makeTrack(four, QStringLiteral("Rain Window lofi hip-hop"), QStringLiteral("Quiet Room")),
        crate::makeTrack(five, QStringLiteral("Old Song (Official Video) 1994"), QString()),
    };
}

DjPlayedSong playedSong(const QString &id, int plays, double last)
{
    DjPlayedSong song;
    song.id = id;
    song.plays = plays;
    song.last = last;
    return song;
}

void testTextIsLowercasedWordsWithAmpersands()
{
    check(smart::text(QStringLiteral("Lo-Fi  Hip-Hop / R&B!")) == QStringLiteral(" lo fi hip hop r&b "),
          "smart text keeps words and ampersands");
}

void testArtistDropsTopicAndVevoAndFallsBackToTheTitle()
{
    check(smart::artist(crate::makeTrack(one, QString(), QStringLiteral("Night Owls - Topic"))) ==
              QStringLiteral("Night Owls"),
          "artist drops - Topic");
    check(smart::artist(crate::makeTrack(one, QString(), QStringLiteral("SomeoneVEVO"))) == QStringLiteral("Someone"),
          "artist drops VEVO");
    check(smart::artist(crate::makeTrack(one, QStringLiteral("Band Name - The Song"))) == QStringLiteral("Band Name"),
          "artist falls back to the title");
    check(smart::artist(crate::makeTrack(one, QStringLiteral("No dash here"))).isEmpty(), "no artist without a dash");
}

void testDecadesComeFromYearsAndShortForms()
{
    check(smart::decade(crate::makeTrack(one, QStringLiteral("Track (1987)"))) == 1980, "decade from a year");
    check(smart::decade(crate::makeTrack(one, QStringLiteral("Best of 90s dance"))) == 1990, "decade from 90s");
    check(!smart::decade(crate::makeTrack(one, QStringLiteral("Track 3000"))).has_value(), "3000 is no decade");
}

void testListsGroupSongsByGenreStyleDecadeArtistAndListening()
{
    const QList<DjPlayedSong> library{playedSong(one, 4, 100), playedSong(two, 2, 300), playedSong(three, 1, 200)};
    const QList<DjSmartList> lists = smart::lists(smartSongs(), library, 400);
    QHash<QString, DjSmartList> byId;
    for (const DjSmartList &list : lists)
        byId.insert(list.id, list);
    const auto songsOf = [&](const char *id) {
        const QString key = QString::fromLatin1(id);
        return byId.contains(key) ? ids(byId.value(key).songs) : QStringList{QStringLiteral("<missing>")};
    };
    check(songsOf("genre:house") == QStringList{one, two}, "house genre", joined(songsOf("genre:house")));
    check(songsOf("genre:chill") == QStringList{three, four}, "chill genre", joined(songsOf("genre:chill")));
    check(songsOf("decade:1990") == QStringList{one, five}, "1990s decade", joined(songsOf("decade:1990")));
    check(byId.value(QStringLiteral("artist:night owls")).name == QStringLiteral("Night Owls"), "artist spelling");
    check(songsOf("artist:night owls") == QStringList{one, two}, "night owls songs");
    check(songsOf("artist:quiet room") == QStringList{three, four}, "quiet room songs");
    check(songsOf("listen:most") == QStringList{one, two}, "most played", joined(songsOf("listen:most")));
    check(songsOf("listen:recent") == QStringList{two, three, one}, "recently played",
          joined(songsOf("listen:recent")));
    check(songsOf("listen:new") == QStringList{four, five}, "never played", joined(songsOf("listen:new")));
    check(!byId.contains(QStringLiteral("genre:hiphop")), "a category with one song is left out");
    check(!byId.contains(QStringLiteral("listen:rediscover")), "nothing to rediscover yet");
    QStringList rediscover;
    for (const DjSmartList &list : smart::lists(smartSongs(), library, 400 + smart::rediscoverAfter + 200)) {
        if (list.id == QStringLiteral("listen:rediscover"))
            rediscover = ids(list.songs);
    }
    check(rediscover == QStringList{one, two, three}, "rediscover after thirty days", joined(rediscover));
}

void testSimilarRanksArtistTagsPlaylistsAndGenres()
{
    const DjTrack seed = crate::makeTrack(four, QStringLiteral("Rain Window lofi hip-hop"), QStringLiteral("Quiet Room"),
                                          {QStringLiteral("lofi")});
    const QList<DjTrack> pool = smartSongs() + QList<DjTrack>{crate::makeTrack(
                                                   QStringLiteral("aaaaaaaaaaa"), QStringLiteral("Unrelated"),
                                                   QStringLiteral("Other"))};
    const QList<DjSimilarSong> found =
        smart::similar(seed, pool, {crate::makePlaylist(QStringLiteral("p"), QStringLiteral("Evening"), {four, one})});
    check(ids(found) == QStringList{three, one}, "similar ranking", joined(ids(found)));
    check(!found.isEmpty() && found.first().reasons.first() == QStringLiteral("Same artist"), "same artist first");
    check(!found.isEmpty() && found.first().reasons.contains(QStringLiteral("#lofi")), "shared tag is a reason");
    check(!found.isEmpty() && found.last().reasons == QStringList{QStringLiteral("Evening")}, "shared playlist reason");
    check(!ids(found).contains(four), "the seed is not suggested");
}

void testSearchQueryUsesTheArtistOrTheCleanedTitle()
{
    check(smart::searchQuery(crate::makeTrack(one, QStringLiteral("Sunrise deep house"),
                                              QStringLiteral("Night Owls - Topic"))) == QStringLiteral("Night Owls House"),
          "search query uses the artist and genre");
    check(smart::searchQuery(crate::makeTrack(one, QStringLiteral("Sunrise (Official Video) [HD]"))) ==
              QStringLiteral("Sunrise"),
          "search query strips brackets");
}

void testSimilarResultsReadRecommendedVideos()
{
    const QByteArray body = "{\"recommendedVideos\":[{\"videoId\":\"dQw4w9WgXcQ\",\"title\":\"One\",\"author\":\"A\","
                            "\"lengthSeconds\":212},{\"videoId\":\"dQw4w9WgXcQ\",\"title\":\"Again\"},"
                            "{\"videoId\":\"bad\"}]}";
    DjSearchResult expected;
    expected.id = one;
    expected.title = QStringLiteral("One");
    expected.author = QStringLiteral("A");
    expected.seconds = 212;
    check(studio::similarResults(body) == QList<DjSearchResult>{expected}, "similar results are read once each");
    check(studio::similarUrl(QUrl(QStringLiteral("https://example.com")), one).toString() ==
              QStringLiteral("https://example.com/api/v1/videos/%1?fields=recommendedVideos").arg(one),
          "similar url");
    check(studio::similarUrl(QUrl(QStringLiteral("https://example.com")), QStringLiteral("bad")).isEmpty(),
          "no similar url for a bad id");
}

void testSmartFocusPlaysItsSongsAndIsDroppedWhenTheListGoes()
{
    DjCrate crate;
    const QList<DjTrack> songs = smartSongs();
    crate.categorize(songs, {}, 0);
    crate.setFocus(DjFocus::smart(QStringLiteral("genre:house")));
    check(crate.focus == DjFocus::smart(QStringLiteral("genre:house")), "a smart list can be focused");
    check(crate.label(crate.focus) == QStringLiteral("House"), "smart label");
    check(ids(crate.pool({})) == QStringList{one, two}, "smart pool");
    check(crate.move(one, DjFocus::tag(QStringLiteral("keep"))) == DjMove::Added, "filing from a smart list adds");
    check(crate.focus == DjFocus::smart(QStringLiteral("genre:house")), "the smart focus stays");
    const DjCrate saved = DjCrate::decode(crate.encoded());
    check(saved.focus == DjFocus::smart(QStringLiteral("genre:house")), "the smart focus is saved");
    check(saved.smart.isEmpty(), "smart lists are not saved");
    crate.categorize({songs.at(2), songs.at(3)}, {}, 0);
    check(crate.focus == DjFocus::everything(), "the focus drops when the smart list goes");
}

void testSmartListsExportAsAPlaylist()
{
    DjCrate crate;
    crate.categorize(smartSongs(), {}, 0);
    const QByteArray data = crate.exported(DjFocus::smart(QStringLiteral("genre:house")));
    const QJsonArray lists = QJsonDocument::fromJson(data).object().value(QStringLiteral("playlists")).toArray();
    const QJsonObject first = lists.isEmpty() ? QJsonObject() : lists.first().toObject();
    check(first.value(QStringLiteral("name")).toString() == QStringLiteral("House"), "smart export name");
    QStringList songs;
    for (const QJsonValue &value : first.value(QStringLiteral("songs")).toArray())
        songs.append(value.toString());
    check(songs == QStringList{one, two}, "smart export songs");
    check(crate.fileName(DjFocus::smart(QStringLiteral("genre:house"))) == QStringLiteral("House"), "smart file name");
}

}

int main()
{
    testTagNamesAreTidied();
    testTaggingKeepsTheSongAndCountsTags();
    testRenamingAndDeletingTagsFollowTheFocus();
    testPlaylistsKeepOrderNamesAndMembers();
    testMovingASongLeavesTheFocusedList();
    testFocusOnAPlaylistPlaysItInOrderAndWraps();
    testShuffledFocusStaysInsideTheTag();
    testEverythingAddsKeptSongsToThePlayedOnes();
    testSavedCrateRoundTripsAndDropsBadEntries();
    testExportImportsBackIntoAnotherCrate();
    testTagExportCarriesTagsWithoutMakingAPlaylist();
    testLinksAndM3uBecomeAPlaylist();
    testOtherJsonShapesImport();
    testFileNamesAreSafe();
    testDontShuffleSongsAreLeftOutOfShuffledPicksButPlayInOrder();
    testTextIsLowercasedWordsWithAmpersands();
    testArtistDropsTopicAndVevoAndFallsBackToTheTitle();
    testDecadesComeFromYearsAndShortForms();
    testListsGroupSongsByGenreStyleDecadeArtistAndListening();
    testSimilarRanksArtistTagsPlaylistsAndGenres();
    testSearchQueryUsesTheArtistOrTheCleanedTitle();
    testSimilarResultsReadRecommendedVideos();
    testSmartFocusPlaysItsSongsAndIsDroppedWhenTheListGoes();
    testSmartListsExportAsAPlaylist();

    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All DJ crate checks passed\n");
    return 0;
}
