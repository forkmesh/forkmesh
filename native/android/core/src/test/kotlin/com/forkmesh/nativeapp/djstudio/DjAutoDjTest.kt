package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class DjAutoDjTest {
    private val seed = DjTrack("dQw4w9WgXcQ", "Night Owls - Sunrise (Official Video)", "Night Owls")

    private fun song(title: String, author: String) = DjAutoDjCandidate("9bZkp7q19f0", title, author, 200.0)

    fun testCandidatesReadRecommendationsWithViewsAndSkipNonVideos() {
        val text = """
            {"recommendedVideos": [
              {"videoId": "9bZkp7q19f0", "title": "Lights Out", "author": "Night Owls", "lengthSeconds": 215, "viewCount": 1200000},
              {"videoId": "kJQP7kiw5Fk", "title": "Study Beats", "author": "Quiet Room", "lengthSeconds": 190, "viewCountText": "3.4M views"},
              {"videoId": "9bZkp7q19f0", "title": "Lights Out again"},
              {"type": "playlist", "title": "A playlist"},
              {"title": "No id"}
            ]}
        """.trimIndent()
        val found = DjAutoDj.candidates(text)
        assertEq(listOf("9bZkp7q19f0", "kJQP7kiw5Fk"), found.map { it.id })
        assertEq(listOf(0, 1), found.map { it.rank })
        assertEq(1_200_000.0, found[0].views)
        assertEq(3_400_000.0, found[1].views)
        assertEq(190.0, found[1].seconds)
        assertEq(listOf("JGwWNGJdvx8"), DjAutoDj.candidates("""[{"videoId":"JGwWNGJdvx8","title":"Search hit","lengthSeconds":200}]""").map { it.id })
        assertEq(emptyList<DjAutoDjCandidate>(), DjAutoDj.candidates("nope"))
    }

    fun testCountReadsYouTubeViewText() {
        assertEq(1_234_567.0, DjAutoDj.count("1,234,567 views"))
        assertEq(12_000.0, DjAutoDj.count("12K views"))
        assertEq(2_500_000_000.0, DjAutoDj.count("2.5B"))
        assertEq(0.0, DjAutoDj.count("No views"))
    }

    fun testLikesComeFromTheVideoRecord() {
        assertEq(DjLikes(52000.0, 1_000_000.0), DjAutoDj.likes("""{"likeCount": 52000, "viewCount": 1000000}"""))
        assertEq(null, DjAutoDj.likes("""{"viewCount": 10}"""))
        assertEq("https://inv.example/api/v1/videos/9bZkp7q19f0?fields=likeCount,viewCount", DjAutoDj.likesUrl("https://inv.example", "9bZkp7q19f0"))
        assertEq(null, DjAutoDj.likesUrl("https://inv.example", "bad"))
    }

    fun testSongKeyIgnoresArtistBracketsAndVideoWords() {
        assertEq("sunrise", DjAutoDj.songKey("Night Owls - Sunrise (Official Video)"))
        assertEq("sunrise", DjAutoDj.songKey("Sunrise [HD] Lyrics"))
        assertEq("sunrise", DjAutoDj.songKey("Sunrise (Remix)"))
        assertEq("lights out", DjAutoDj.songKey("Lights Out"))
        assertEq("lights out", DjAutoDj.songKey("Official Video - Lights Out"))
    }

    fun testEligibleLeavesOutTheSeedRepeatsOtherUploadsAndOddLengths() {
        val found = listOf(
            DjAutoDjCandidate(seed.id, "Sunrise", seconds = 200.0, rank = 0),
            DjAutoDjCandidate("9bZkp7q19f0", "Sunrise (Lyrics)", "Fan", 200.0, rank = 1),
            DjAutoDjCandidate("kJQP7kiw5Fk", "Lights Out", seconds = 200.0, rank = 2),
            DjAutoDjCandidate("JGwWNGJdvx8", "Night Owls 3 hour mix", seconds = 3 * 3600.0, rank = 3),
            DjAutoDjCandidate("OPf0YbXqDm0", "Short clip", seconds = 20.0, rank = 4),
            DjAutoDjCandidate("YQHsXMglC9A", "Live now", seconds = 0.0, rank = 5),
            DjAutoDjCandidate("fJ9rUzIMcZQ", "Heard Before", seconds = 200.0, rank = 6),
            DjAutoDjCandidate("hTWKbfoikeg", "On a deck", seconds = 200.0, rank = 7),
            DjAutoDjCandidate("RgKAFK5djSk", "Lights Out (Official Audio)", seconds = 200.0, rank = 8),
            DjAutoDjCandidate("CevxZvSJLk8", "Fresh Song", seconds = 240.0, rank = 9),
        )
        val kept = DjAutoDj.eligible(found, seed, setOf("hTWKbfoikeg"), listOf("Someone - Heard Before"))
        assertEq(listOf("RgKAFK5djSk", "CevxZvSJLk8"), kept.map { it.id })
    }

    fun testOnlySongsQualifyNotTalentShowsCoversOrLiveTakes() {
        expect(!DjAutoDj.isSong(song("Golden Buzzer: Teen Sings Hallelujah | America's Got Talent 2024", "America's Got Talent")))
        expect(!DjAutoDj.isSong(song("Nervous Singer STUNS The Judges | Auditions | BGT 2023", "Britain's Got Talent")))
        expect(!DjAutoDj.isSong(song("Hallelujah", "The X Factor UK")))
        expect(!DjAutoDj.isSong(song("Vocal Coach Reacts to Queen", "Music Room")))
        expect(!DjAutoDj.isSong(song("Bohemian Rhapsody (Live Aid 1985)", "Queen")))
        expect(!DjAutoDj.isSong(song("Queen - Bohemian Rhapsody - Live", "Fan")))
        expect(!DjAutoDj.isSong(song("Hallelujah | Acoustic Cover by Sam", "Sam")))
        expect(!DjAutoDj.isSong(song("Hallelujah piano cover", "Keys")))
        expect(!DjAutoDj.isSong(song("Sunrise (Sped Up)", "Edits")))
        expect(!DjAutoDj.isSong(song("Night Owls - Sunrise (Karaoke Version)", "Sing King")))
        expect(DjAutoDj.isSong(song("Oasis - Live Forever (Remastered)", "Oasis")))
        expect(DjAutoDj.isSong(song("Queen - Bohemian Rhapsody (Official Video Remastered)", "Queen Official")))
        expect(DjAutoDj.isSong(song("Hallelujah", "Jeff Buckley - Topic")))
        expect(DjAutoDj.isSong(song("Billy Idol - White Wedding", "Billy Idol")))
    }

    fun testOriginalUploadsAreTheArtistsOwn() {
        expect(DjAutoDj.isOriginal(song("Hallelujah", "Jeff Buckley - Topic")))
        expect(DjAutoDj.isOriginal(song("Lights Out", "NightOwlsVEVO")))
        expect(DjAutoDj.isOriginal(song("Night Owls - Lights Out (Official Audio)", "Records Ltd")))
        expect(DjAutoDj.isOriginal(song("Oasis - Live Forever (Remastered)", "Big Brother")))
        expect(DjAutoDj.isOriginal(song("Night Owls – Lights Out", "Night Owls")))
        expect(!DjAutoDj.isOriginal(song("Night Owls - Lights Out", "Lyrics Hub")))
        expect(!DjAutoDj.isOriginal(song("Lights Out", "")))
    }

    fun testEligibleLeavesOutNonSongsAndPutsOriginalsFirst() {
        val found = listOf(
            DjAutoDjCandidate("9bZkp7q19f0", "Teen Sings Sunrise | AGT 2024", "America's Got Talent", 300.0, rank = 0),
            DjAutoDjCandidate("kJQP7kiw5Fk", "Lights Out", "Lyrics Hub", 200.0, rank = 1),
            DjAutoDjCandidate("JGwWNGJdvx8", "Moonset (Live)", "Night Owls", 200.0, rank = 2),
            DjAutoDjCandidate("CevxZvSJLk8", "Fresh Song", "Night Owls - Topic", 240.0, rank = 3),
        )
        assertEq(listOf("CevxZvSJLk8", "kJQP7kiw5Fk"), DjAutoDj.eligible(found, seed, emptySet(), emptyList()).map { it.id })
        val liked = listOf(
            DjAutoDjCandidate("kJQP7kiw5Fk", "Lights Out", "Lyrics Hub", 200.0, 9_000_000.0, 400_000.0, 0),
            DjAutoDjCandidate("CevxZvSJLk8", "Fresh Song", "Night Owls - Topic", 240.0, 3000.0, 20.0, 1),
        )
        assertEq(listOf("CevxZvSJLk8", "kJQP7kiw5Fk"), DjAutoDj.ranked(liked).map { it.id })
    }

    fun testRankingPrefersLikedSongsAmongRelatedOnes() {
        val found = listOf(
            DjAutoDjCandidate("9bZkp7q19f0", "Most related, few likes", seconds = 200.0, views = 20000.0, likes = 150.0, rank = 0),
            DjAutoDjCandidate("kJQP7kiw5Fk", "Loved", seconds = 200.0, views = 5_000_000.0, likes = 240_000.0, rank = 1),
            DjAutoDjCandidate("JGwWNGJdvx8", "Popular, unchecked", seconds = 200.0, views = 3_000_000.0, rank = 2),
        )
        assertEq(listOf("kJQP7kiw5Fk", "JGwWNGJdvx8", "9bZkp7q19f0"), DjAutoDj.ranked(found).map { it.id })
        val even = listOf(
            DjAutoDjCandidate("9bZkp7q19f0", "First", seconds = 200.0, views = 1000.0, likes = 50.0, rank = 0),
            DjAutoDjCandidate("kJQP7kiw5Fk", "Second", seconds = 200.0, views = 1000.0, likes = 50.0, rank = 1),
        )
        assertEq(listOf("9bZkp7q19f0", "kJQP7kiw5Fk"), DjAutoDj.ranked(even).map { it.id })
    }

    fun testNextSkipsLoadedFailedAndPlayedSongs() {
        val ranked = listOf("9bZkp7q19f0", "kJQP7kiw5Fk", "JGwWNGJdvx8", "OPf0YbXqDm0").map { DjAutoDjCandidate(it, it, seconds = 200.0) }
        assertEq("OPf0YbXqDm0", DjAutoDj.next(ranked, setOf("9bZkp7q19f0"), setOf("kJQP7kiw5Fk"), listOf("JGwWNGJdvx8"))?.id)
        assertEq(null, DjAutoDj.next(ranked, emptySet(), emptySet(), ranked.map { it.id }))
    }

    fun testSavedStateKeepsAutoDjOnlyWithAutomix() {
        expect(DjSaved.decode(DjSaved(automix = true, autoDj = true).encoded()).autoDj)
        expect(!DjSaved.decode("""{"automix":false,"autoDj":true}""").autoDj)
        expect(!DjSaved.decode("""{"automix":true}""").autoDj)
    }

    fun testRememberKeepsTheSessionNewestFirstWithoutRepeats() {
        assertEq(listOf("b", "a", "c"), DjAutoDj.remember("b", listOf("a", "b", "c")))
        assertEq(DjAutoDj.SESSION_LIMIT, DjAutoDj.remember("x", (0 until DjAutoDj.SESSION_LIMIT).map { it.toString() }).size)
    }

    fun testRepeatSkipsTheSilenceAtTheFrontAndBackOfASong() {
        val quiet = DjWaveBar(0.04, 0.0)
        val loud = DjWaveBar(0.8, 0.0)
        val bars = List(30) { quiet } + List(1900) { loud } + List(70) { quiet }
        val span = DjRepeat.span(bars, 100.0)!!
        expect(abs(span.start - 1.5) < 0.001)
        expect(abs(span.end - 96.5) < 0.001)
        assertEq(DjRepeatSpan(0.0, 99.75), DjRepeat.span(List(2000) { quiet }, 100.0))
        assertEq(DjRepeatSpan(0.0, 99.75), DjRepeat.span(emptyList(), 100.0))
        assertEq(DjRepeatSpan(0.0, 99.75), DjRepeat.span(List(1000) { quiet } + listOf(loud) + List(999) { quiet }, 100.0))
        assertEq(DjRepeatSpan(0.0, 99.75), DjRepeat.span(List(2000) { loud }, 100.0))
        assertEq(null, DjRepeat.span(bars, 0.0))
        assertEq(null, DjRepeat.span(bars, Double.NaN))
    }

    fun testRepeatWrapsWhenTheSoundEndsAndAimsJustBeforeIt() {
        val span = DjRepeatSpan(1.5, 96.5)
        assertEq(DjRepeatStep.Wait, DjRepeat.step(50.0, span, 1.0))
        assertEq(DjRepeatStep.Wrap, DjRepeat.step(96.5, span, 1.0))
        assertEq(DjRepeatStep.Wrap, DjRepeat.step(98.0, span, 1.0))
        val after = DjRepeat.step(96.2, span, 1.5) as DjRepeatStep.After
        expect(abs(after.seconds - 0.2) < 0.0001)
        assertEq(DjRepeatStep.Wait, DjRepeat.step(96.2, span, 0.4))
        assertEq(DjRepeatStep.Wait, DjRepeat.step(98.0, span, 0.0))
    }
}
