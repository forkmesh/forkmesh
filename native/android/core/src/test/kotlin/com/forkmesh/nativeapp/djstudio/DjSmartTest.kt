package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DjSmartTest {
    private val one = "dQw4w9WgXcQ"
    private val two = "9bZkp7q19f0"
    private val three = "kJQP7kiw5Fk"
    private val four = "JGwWNGJdvx8"
    private val five = "OPf0YbXqDm0"

    private val songs: List<DjTrack>
        get() = listOf(
            DjTrack.of(one, "Sunrise (Deep House Mix) 1998", "Night Owls - Topic"),
            DjTrack.of(two, "Lights Out - Deep House Remix", "Night Owls"),
            DjTrack.of(three, "Study Beats", "Quiet Room", listOf("lofi")),
            DjTrack.of(four, "Rain Window lofi hip-hop", "Quiet Room"),
            DjTrack.of(five, "Old Song (Official Video) 1994", ""),
        )

    fun testTextIsLowercasedWordsWithAmpersands() {
        assertEq(" lo fi hip hop r&b ", DjSmart.text("Lo-Fi  Hip-Hop / R&B!"))
    }

    fun testArtistDropsTopicAndVevoAndFallsBackToTheTitle() {
        assertEq("Night Owls", DjSmart.artist(DjTrack.of(one, author = "Night Owls - Topic")))
        assertEq("Someone", DjSmart.artist(DjTrack.of(one, author = "SomeoneVEVO")))
        assertEq("Band Name", DjSmart.artist(DjTrack.of(one, "Band Name - The Song")))
        assertEq("", DjSmart.artist(DjTrack.of(one, "No dash here")))
    }

    fun testDecadesComeFromYearsAndShortForms() {
        assertEq(1980, DjSmart.decade(DjTrack.of(one, "Track (1987)")))
        assertEq(1990, DjSmart.decade(DjTrack.of(one, "Best of 90s dance")))
        assertEq(null, DjSmart.decade(DjTrack.of(one, "Track 3000")))
    }

    fun testListsGroupSongsByGenreStyleDecadeArtistAndListening() {
        val library = listOf(
            DjPlayedSong.of(one, "", "", 4, 100.0),
            DjPlayedSong.of(two, "", "", 2, 300.0),
            DjPlayedSong.of(three, "", "", 1, 200.0),
        )
        val byId = DjSmart.lists(songs, library, 400.0).associateBy { it.id }
        assertEq(listOf(one, two), byId["genre:house"]?.songs?.map { it.id })
        assertEq(listOf(three, four), byId["genre:chill"]?.songs?.map { it.id })
        assertEq(listOf(one, five), byId["decade:1990"]?.songs?.map { it.id })
        assertEq("Night Owls", byId["artist:night owls"]?.name)
        assertEq(listOf(one, two), byId["artist:night owls"]?.songs?.map { it.id })
        assertEq(listOf(three, four), byId["artist:quiet room"]?.songs?.map { it.id })
        assertEq(listOf(one, two), byId["listen:most"]?.songs?.map { it.id })
        assertEq(listOf(two, three, one), byId["listen:recent"]?.songs?.map { it.id })
        assertEq(listOf(four, five), byId["listen:new"]?.songs?.map { it.id })
        assertEq(null, byId["genre:hiphop"], "a category with one song is left out")
        assertEq(null, byId["listen:rediscover"])
        val later = DjSmart.lists(songs, library, 400 + DjSmart.REDISCOVER_AFTER + 200).firstOrNull { it.id == "listen:rediscover" }
        assertEq(listOf(one, two, three), later?.songs?.map { it.id })
    }

    fun testSimilarRanksArtistTagsPlaylistsAndGenres() {
        val seed = DjTrack.of(four, "Rain Window lofi hip-hop", "Quiet Room", listOf("lofi"))
        val pool = songs + DjTrack.of("aaaaaaaaaaa", "Unrelated", "Other")
        val found = DjSmart.similar(seed, pool, listOf(DjPlaylist.of("p", "Evening", listOf(four, one))))
        assertEq(listOf(three, one), found.map { it.id })
        assertEq("Same artist", found.first().reasons.first())
        expect("#lofi" in found.first().reasons)
        assertEq(listOf("Evening"), found.last().reasons)
        expect(found.none { it.id == four })
    }

    fun testSearchQueryUsesTheArtistOrTheCleanedTitle() {
        assertEq("Night Owls House", DjSmart.searchQuery(DjTrack.of(one, "Sunrise deep house", "Night Owls - Topic")))
        assertEq("Sunrise", DjSmart.searchQuery(DjTrack.of(one, "Sunrise (Official Video) [HD]")))
    }

    fun testSimilarResultsReadRecommendedVideos() {
        val body = """{"recommendedVideos":[{"videoId":"dQw4w9WgXcQ","title":"One","author":"A","lengthSeconds":212},{"videoId":"dQw4w9WgXcQ","title":"Again"},{"videoId":"bad"}]}"""
        assertEq(listOf(DjSearchResult(one, "One", "A", 212.0)), DjStudio.similarResults(body))
        assertEq("https://example.com/api/v1/videos/$one?fields=recommendedVideos", DjStudio.similarUrl("https://example.com", one))
        assertEq(null, DjStudio.similarUrl("https://example.com", "bad"))
    }

    fun testSmartFocusPlaysItsSongsAndIsDroppedWhenTheListGoes() {
        val crate = DjCrate()
        crate.categorize(songs, emptyList(), 0.0)
        crate.focusOn(DjFocus.Smart("genre:house"))
        assertEq(DjFocus.Smart("genre:house"), crate.focus)
        assertEq("House", crate.label(crate.focus))
        assertEq(listOf(one, two), crate.pool(emptyList()).map { it.id })
        assertEq(DjMove.ADDED, crate.move(one, to = DjFocus.Tag("keep")))
        assertEq(DjFocus.Smart("genre:house"), crate.focus)
        val saved = DjCrate.decode(crate.encoded())
        assertEq(DjFocus.Smart("genre:house"), saved.focus)
        expect(saved.smart.isEmpty())
        crate.categorize(listOf(songs[2], songs[3]), emptyList(), 0.0)
        assertEq(DjFocus.Everything, crate.focus)
    }

    fun testSmartListsExportAsAPlaylist() {
        val crate = DjCrate()
        crate.categorize(songs, emptyList(), 0.0)
        val root = Json.parseObject(crate.exported(DjFocus.Smart("genre:house")))
        val lists = root.maps("playlists")
        assertEq("House", lists.firstOrNull()?.get("name"))
        assertEq(listOf(one, two), lists.firstOrNull()?.get("songs"))
        assertEq("House", crate.fileName(DjFocus.Smart("genre:house")))
    }
}
