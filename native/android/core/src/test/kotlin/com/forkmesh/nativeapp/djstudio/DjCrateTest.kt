package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant

class DjCrateTest {
    private val one = "dQw4w9WgXcQ"
    private val two = "9bZkp7q19f0"
    private val three = "kJQP7kiw5Fk"
    private val four = "JGwWNGJdvx8"

    fun testTagNamesAreTidied() {
        assertEq("deep house", DjCrate.tagName("  #Deep   House "))
        assertEq(listOf("chill", "warm up"), DjCrate.tagNames("Chill, #chill, warm up,,"))
        assertEq(DjCrate.TAG_LENGTH, DjCrate.tagName("a".repeat(50)).length)
        assertEq("", DjCrate.tagName(" # "))
    }

    fun testTaggingKeepsTheSongAndCountsTags() {
        val crate = DjCrate()
        expect(crate.tag(one, "chill, House", "First", "Someone"))
        expect(crate.tag(two, "house"))
        expect(!crate.tag("short", "house"))
        expect(!crate.tag(three, " , "))
        assertEq("First", crate.track(one)?.title)
        assertEq(listOf("chill", "house"), crate.track(one)?.tags)
        assertEq(listOf(DjTagCount("chill", 1), DjTagCount("house", 2)), crate.tagCounts)
        assertEq(listOf(one, two), crate.songs(DjFocus.Tag("house")).map { it.id })
        crate.untag(one, "house")
        assertEq(listOf(two), crate.songs(DjFocus.Tag("house")).map { it.id })
    }

    fun testRenamingAndDeletingTagsFollowTheFocus() {
        val crate = DjCrate()
        crate.tag(one, "house, deep")
        crate.tag(two, "deep")
        crate.focusOn(DjFocus.Tag("deep"))
        expect(crate.renameTag("deep", "House"))
        assertEq(listOf("house"), crate.track(one)?.tags)
        assertEq(listOf("house"), crate.track(two)?.tags)
        assertEq(DjFocus.Tag("house"), crate.focus)
        crate.deleteTag("house")
        assertEq(DjFocus.Everything, crate.focus)
        expect(crate.tagCounts.isEmpty())
        expect(!crate.renameTag("house", "  "))
    }

    fun testPlaylistsKeepOrderNamesAndMembers() {
        val crate = DjCrate()
        assertEq("p1", crate.createPlaylist("  Friday   Night ", "p1"))
        assertEq("Friday Night", crate.playlist("p1")?.name)
        assertEq("p2", crate.createPlaylist("friday night", "p2"))
        assertEq("friday night 2", crate.playlist("p2")?.name)
        crate.renamePlaylist("p2", "Sunday")
        assertEq("Sunday", crate.playlist("p2")?.name)
        assertEq(2, crate.add(listOf(DjTrack.of(one, "A"), DjTrack.of(two), DjTrack.of(one), DjTrack.of("bad")), "p1"))
        assertEq(1, crate.add(listOf(DjTrack.of(three)), "p1"))
        assertEq(listOf(one, two, three), crate.playlist("p1")?.songs)
        crate.moveSongs("p1", setOf(2), 0)
        assertEq(listOf(three, one, two), crate.playlist("p1")?.songs)
        crate.moveSongs("p1", setOf(0), 3)
        assertEq(listOf(one, two, three), crate.playlist("p1")?.songs)
        crate.removeSongs(setOf(1), "p1")
        assertEq(listOf(one, three), crate.songs(DjFocus.Playlist("p1")).map { it.id })
        expect(crate.track(two) != null)
        crate.focusOn(DjFocus.Playlist("p1"))
        crate.removeTrack(one)
        assertEq(listOf(three), crate.playlist("p1")?.songs)
        crate.deletePlaylist("p1")
        assertEq(DjFocus.Everything, crate.focus)
    }

    fun testMovingASongLeavesTheFocusedList() {
        val crate = DjCrate()
        crate.createPlaylist("Main", "main")
        crate.createPlaylist("Later", "later")
        crate.add(listOf(DjTrack.of(one, "One"), DjTrack.of(two)), "main")
        crate.focusOn(DjFocus.Playlist("main"))
        assertEq(DjMove.MOVED, crate.move(one, to = DjFocus.Playlist("later")))
        assertEq(listOf(two), crate.playlist("main")?.songs)
        assertEq(listOf(one), crate.playlist("later")?.songs)
        assertEq(DjMove.ADDED, crate.move(three, "Three", to = DjFocus.Tag("chill")))
        assertEq(listOf("chill"), crate.track(three)?.tags)
        assertEq(DjMove.ADDED, crate.move(two, to = DjFocus.Playlist("main")))
        assertEq(listOf(two), crate.playlist("main")?.songs)
        assertEq(DjMove.REMOVED, crate.move(two, to = DjFocus.Everything))
        assertEq(emptyList<String>(), crate.playlist("main")?.songs)
        assertEq(DjFocus.Playlist("main"), crate.focus)

        crate.tag(four, "loud")
        crate.tag(one, "loud")
        crate.focusOn(DjFocus.Tag("loud"))
        assertEq(DjMove.MOVED, crate.move(four, to = DjFocus.Tag("chill")))
        assertEq(listOf("chill"), crate.track(four)?.tags)
        assertEq(DjFocus.Tag("loud"), crate.focus)
        assertEq(DjMove.MOVED, crate.move(one, to = DjFocus.Playlist("main")))
        assertEq(DjFocus.Everything, crate.focus)
        assertEq(DjMove.FULL, crate.move("short", to = DjFocus.Tag("x")))
        assertEq(DjMove.FULL, crate.move(one, to = DjFocus.Playlist("gone")))
    }

    fun testFocusOnAPlaylistPlaysItInOrderAndWraps() {
        val crate = DjCrate()
        crate.createPlaylist("Set", "p")
        crate.add(listOf(DjTrack.of(one), DjTrack.of(two), DjTrack.of(three)), "p")
        crate.focusOn(DjFocus.Playlist("p"))
        crate.shuffle = false
        val library = listOf(DjPlayedSong.of(four, "Elsewhere", ""))
        assertEq(one, crate.next(library, emptySet(), emptySet(), seed = 1)?.id)
        assertEq(two, crate.next(library, setOf(one), setOf(one), seed = 1)?.id)
        assertEq(three, crate.next(library, setOf(one, two), setOf(two), seed = 1)?.id)
        assertEq(one, crate.next(library, setOf(two, three), setOf(three), seed = 1)?.id)
        assertEq(three, crate.next(library, emptySet(), emptySet(), setOf(two), 1)?.id)
    }

    fun testShuffledFocusStaysInsideTheTag() {
        val crate = DjCrate()
        crate.tag(one, "chill")
        crate.tag(two, "chill")
        crate.tag(three, "loud")
        crate.focusOn(DjFocus.Tag("chill"))
        val library = listOf(DjPlayedSong.of(four, "", "", 9, 1.0), DjPlayedSong.of(one, "Played", "", 3, 50.0))
        for (seed in 0L until 40L) {
            val pick = crate.copy().next(library, emptySet(), emptySet(), seed = seed)
            expect(pick?.id in listOf(one, two))
        }
        assertEq(one, crate.copy().next(library, setOf(two), setOf(two), seed = 3)?.id)
        assertEq(3, crate.pool(library).firstOrNull { it.id == one }?.plays)
        assertEq("Played", crate.pool(library).firstOrNull { it.id == one }?.title)
        crate.focusOn(DjFocus.Tag("nobody"))
        assertEq(DjFocus.Everything, crate.focus)
    }

    fun testEverythingAddsKeptSongsToThePlayedOnes() {
        val crate = DjCrate()
        crate.tag(two, "new")
        val library = listOf(DjPlayedSong.of(one, "", ""))
        assertEq(listOf(one, two), crate.pool(library).map { it.id })
        assertEq(listOf(two), crate.pool(library, setOf(one)).map { it.id })
        expect(DjCrate().pool(emptyList()).isEmpty())
        assertEq(null, DjCrate().next(emptyList(), emptySet(), emptySet(), seed = 0))
    }

    fun testSavedCrateRoundTripsAndDropsBadEntries() {
        val crate = DjCrate()
        crate.tag(one, "chill", "One")
        crate.createPlaylist("Mine", "p")
        crate.add(listOf(DjTrack.of(one), DjTrack.of(two)), "p")
        crate.focusOn(DjFocus.Playlist("p"))
        crate.shuffle = false
        assertEq(crate, DjCrate.decode(crate.encoded()))
        val raw = """{"tracks":[{"id":"$one","tags":["A"]},{"id":"bad"},{"id":"$one"}],"playlists":[{"id":"p","name":"x","songs":["$one","$three"]}],"focus":"playlist:gone","shuffle":false}"""
        val decoded = DjCrate.decode(raw)
        assertEq(listOf(one), decoded.tracks.map { it.id })
        assertEq(listOf("a"), decoded.tracks.firstOrNull()?.tags)
        assertEq(listOf(one), decoded.playlist("p")?.songs)
        assertEq(DjFocus.Everything, decoded.focus)
        expect(!decoded.shuffle)
        assertEq(DjCrate(), DjCrate.decode("nope"))
        assertEq(DjFocus.Tag("deep house"), DjFocus.of(DjFocus.Tag("deep house").key))
        assertEq(DjFocus.Everything, DjFocus.of("tag: "))
    }

    fun testExportImportsBackIntoAnotherCrate() {
        val crate = DjCrate()
        crate.tag(one, "chill", "One", "Artist")
        crate.createPlaylist("Warm Up", "p")
        crate.add(listOf(DjTrack.of(two, "Two"), DjTrack.of(one)), "p")
        val text = crate.exported(DjFocus.Everything, Instant.EPOCH)
        val root = Json.parseObject(text)
        assertEq(DjCrate.FORMAT, root["format"])
        assertEq("1970-01-01T00:00:00Z", root["exported"])
        val parsed = DjCrate.parse(text.toByteArray())!!
        assertEq(listOf(one, two), parsed.tracks.map { it.id })
        assertEq(listOf(DjImportedList.of("Warm Up", listOf(two, one))), parsed.playlists)

        val other = DjCrate()
        other.tag(two, "loud")
        other.createPlaylist("warm up", "mine")
        other.add(listOf(DjTrack.of(three)), "mine")
        val report = other.merge(parsed, "Imported")
        assertEq(DjMergeReport(1, 1), report)
        assertEq("Imported 1 new song into 1 playlist.", report.summary)
        assertEq(1, other.playlists.size)
        assertEq(listOf(three, two, one), other.playlist("mine")?.songs)
        assertEq("Two", other.track(two)?.title)
        assertEq(listOf("loud"), other.track(two)?.tags)
        assertEq(listOf("chill"), other.track(one)?.tags)
    }

    fun testTagExportCarriesTagsWithoutMakingAPlaylist() {
        val crate = DjCrate()
        crate.tag(one, "chill")
        crate.tag(two, "loud")
        val parsed = DjCrate.parse(crate.exported(DjFocus.Tag("chill")).toByteArray())!!
        assertEq(listOf(one), parsed.tracks.map { it.id })
        val other = DjCrate()
        assertEq(DjMergeReport(1, 0), other.merge(parsed, "Imported"))
        expect(other.playlists.isEmpty())
        assertEq(listOf(one), other.songs(DjFocus.Tag("chill")).map { it.id })
    }

    fun testLinksAndM3uBecomeAPlaylist() {
        val crate = DjCrate()
        crate.createPlaylist("Set", "p")
        crate.add(listOf(DjTrack.of(one, "One\nline"), DjTrack.of(two)), "p")
        val m3u = crate.m3u(DjFocus.Playlist("p"))
        assertEq("#EXTM3U\n#EXTINF:-1,One line\nhttps://www.youtube.com/watch?v=$one\n#EXTINF:-1,YouTube video\nhttps://www.youtube.com/watch?v=$two\n", m3u)
        val parsed = DjCrate.parse(m3u.toByteArray())!!
        assertEq(listOf("One line", "YouTube video"), parsed.tracks.map { it.title })

        val text = "My set\nhttps://youtu.be/$three?si=x\n\nwatch this https://www.youtube.com/watch?v=$four&t=3\nhttps://youtu.be/$three\nhttps://example.com\n"
        val links = DjCrate.parse(text.toByteArray())!!
        assertEq(listOf(three, four), links.tracks.map { it.id })
        val other = DjCrate()
        assertEq(DjMergeReport(2, 1), other.merge(links, "Pasted"))
        assertEq("Pasted", other.playlists.firstOrNull()?.name)
        assertEq(listOf(three, four), other.playlists.firstOrNull()?.songs)
        assertEq(null, DjCrate.parse("nothing here".toByteArray()))
        assertEq(null, DjCrate.parse(ByteArray(0)))
    }

    fun testOtherJsonShapesImport() {
        val array = """["https://youtu.be/$one", {"videoId":"$two","title":"Two","artist":"Band","tags":"a, b"}, {"url":"nope"}]"""
        val parsed = DjCrate.parse(array.toByteArray())!!
        assertEq(listOf(one, two), parsed.tracks.map { it.id })
        assertEq("Band", parsed.tracks.last().author)
        assertEq(listOf("a", "b"), parsed.tracks.last().tags)
        expect(parsed.playlists.isEmpty())
        val named = """{"name":"Road Trip","videos":[{"link":"https://www.youtube.com/watch?v=$three"}]}"""
        assertEq(listOf(DjImportedList.of("Road Trip", listOf(three))), DjCrate.parse(named.toByteArray())?.playlists)
    }

    fun testFileNamesAreSafe() {
        val crate = DjCrate()
        crate.createPlaylist("Late/Night: Set", "p")
        assertEq("Late-Night- Set", crate.fileName(DjFocus.Playlist("p")))
        assertEq("Tag deep house", crate.fileName(DjFocus.Tag("deep house")))
        assertEq("DJ Playlists", crate.fileName(DjFocus.Everything))
    }

    fun testDontShuffleSongsAreLeftOutOfShuffledPicksButPlayInOrder() {
        val crate = DjCrate()
        crate.createPlaylist("Set", "p")
        crate.add(listOf(DjTrack.of(one), DjTrack.of(two), DjTrack.of(three)), "p")
        expect(crate.setShuffles(two, false))
        expect(crate.setShuffles(four, false, "Saved"))
        assertEq("Saved", crate.track(four)?.title)
        expect(!crate.shuffles(two))
        assertEq(setOf(two, four), crate.unshuffled)
        val library = listOf(DjPlayedSong.of(one, "", ""), DjPlayedSong.of(two, "", ""))
        assertEq(setOf(one, three), crate.pool(library).map { it.id }.toSet())
        for (seed in 0L until 20L) expect(crate.next(library, emptySet(), emptySet(), seed = seed)?.id != two)
        crate.focusOn(DjFocus.Playlist("p"))
        crate.shuffle = false
        assertEq(listOf(one, two, three), crate.pool(emptyList()).map { it.id })
        assertEq(two, crate.next(emptyList(), setOf(one), emptySet(), seed = 0)?.id)
        assertEq(setOf(two, four), DjCrate.decode(crate.encoded()).unshuffled)
        val exported = crate.exported(DjFocus.Everything)
        assertEq(2, exported.split("noShuffle").size - 1)
        val other = DjCrate()
        other.merge(DjCrate.parse(exported.toByteArray())!!, "In")
        assertEq(setOf(two, four), other.unshuffled)
        expect(crate.setShuffles(two, true))
        expect(crate.shuffles(two))
        crate.removeTrack(four)
        expect(crate.unshuffled.isEmpty())
    }
}
