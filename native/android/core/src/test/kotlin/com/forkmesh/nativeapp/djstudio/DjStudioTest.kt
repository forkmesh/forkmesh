package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

internal fun near(expected: Double, actual: Double, accuracy: Double, message: String = "") {
    expect(abs(expected - actual) <= accuracy, "${if (message.isEmpty()) "" else "$message: "}expected <$expected> ± $accuracy but was <$actual>")
}

class DjStudioTest {
    fun testVideoIdAcceptsCommonYouTubeForms() {
        val id = "dQw4w9WgXcQ"
        val forms = listOf(
            id, "<$id>", "https://www.youtube.com/watch?v=$id&t=42s", "youtube.com/watch?feature=share&v=$id",
            "https://youtu.be/$id?si=abc", "youtu.be/$id", "https://m.youtube.com/shorts/$id",
            "https://music.youtube.com/watch?v=$id&list=RD", "https://www.youtube-nocookie.com/embed/$id?start=3",
            "https://www.youtube.com/live/$id", "https://www.youtube.com/attribution_link?u=/watch%3Fv%3D$id%26feature%3Dshare",
            "https://www.youtube.com/watch#?v=$id", "Listen: https://youtu.be/$id now",
        )
        for (form in forms) assertEq(id, DjStudio.videoId(form), form)
        for (bad in listOf("", "hello", "https://example.com/watch?v=$id", "https://youtu.be/short", "dQw4w9WgXc!")) {
            assertEq("", DjStudio.videoId(bad), bad)
        }
    }

    fun testSharedVideoIdsTakeOnlyYouTubeShares() {
        val id = "dQw4w9WgXcQ"
        val other = "9bZkp7q19f0"
        assertEq(listOf(id), DjStudio.sharedVideoIds(listOf("https://youtu.be/$id?si=abc"), "Never Gonna Give You Up"))
        assertEq(listOf(id, other), DjStudio.sharedVideoIds(listOf("https://www.youtube.com/watch?v=$id", "https://youtu.be/$other", "https://youtu.be/$id"), ""))
        assertEq(listOf(id), DjStudio.sharedVideoIds(emptyList(), " https://youtu.be/$id\n"))
        assertEq(emptyList<String>(), DjStudio.sharedVideoIds(listOf("https://youtu.be/$id", "https://example.com/page"), ""))
        assertEq(emptyList<String>(), DjStudio.sharedVideoIds(emptyList(), "Summarize https://youtu.be/$id"))
        assertEq(emptyList<String>(), DjStudio.sharedVideoIds(emptyList(), id))
        assertEq(emptyList<String>(), DjStudio.sharedVideoIds(emptyList(), ""))
    }

    fun testEqualPowerCrossfader() {
        val left = DjStudio.gains(-1.0)
        near(1.0, left.first, 1e-9)
        assertEq(0.0, left.second)
        val right = DjStudio.gains(1.0)
        assertEq(0.0, right.first)
        near(1.0, right.second, 1e-9)
        val centre = DjStudio.gains(0.0)
        near(1.0, centre.first * centre.first + centre.second * centre.second, 1e-9)
        near(1.0, DjStudio.gain(DjDeckSide.B, 5.0), 1e-9)
    }

    fun testClockAndRates() {
        assertEq("00:48", DjStudio.clock(48.9))
        assertEq("1:02:05", DjStudio.clock(3725.0))
        assertEq("00:00", DjStudio.clock(Double.NaN))
        assertEq(1 + DjStudio.PITCH_RANGE, DjStudio.clampRate(3.0))
        near(1.05, DjStudio.syncedRate(120.0, 126.0, 1.0), 1e-9)
        near(1.02, DjStudio.syncedRate(0.0, 126.0, 1.02), 1e-9)
        near(DjStudio.SECONDS_PER_TURN, DjStudio.scratchSeconds(2 * Math.PI), 1e-9)
    }

    fun testScratchSpeedFollowsTheHand() {
        val forward = DjStudio.scratchSpeed(0.0, 0.1, 0.1)
        near(0.6, forward, 1e-9)
        near(0.84, DjStudio.scratchSpeed(forward, 0.1, 0.1), 1e-9)
        near(4.8, DjStudio.scratchSpeed(0.0, 5.0, 0.001), 1e-9)
        assertEq(1.0, DjStudio.scratchSpeed(1.0, 0.1, 0.0))
        assertEq(1.0, DjStudio.scratchLevel(-5.0))
        assertEq(0.5, DjStudio.scratchLevel(5.0))
        assertEq(0.0, DjStudio.scratchLevel(0.0))
        assertEq(0.0, DjStudio.scratchLevel(Double.NaN))
    }

    fun testOnlyADeliberateDragScratches() {
        expect(!DjStudio.isScratch(3.0, 4.0))
        expect(!DjStudio.isScratch(8.0, -8.0))
        expect(DjStudio.isScratch(0.0, DjStudio.SCRATCH_THRESHOLD))
        expect(DjStudio.isScratch(-12.0, 9.0))
        expect(!DjStudio.isScratch(Double.NaN, 40.0))
    }

    fun testSampleSectionRunsFromStartToStop() {
        assertEq(DjSection(DjDeckSide.A, 10.0, 4.5), DjStudio.sampleSection(DjDeckSide.A, 10.0, 14.5))
        assertEq(DjSection(DjDeckSide.B, 10.0, 1.0), DjStudio.sampleSection(DjDeckSide.B, 10.0, 10.6))
        assertEq(30.0, DjStudio.sampleSection(DjDeckSide.B, 0.0, 90.0)?.duration)
        assertEq(null, DjStudio.sampleSection(DjDeckSide.A, 10.0, 10.1))
        assertEq(null, DjStudio.sampleSection(DjDeckSide.A, 10.0, 4.0))
        assertEq(null, DjStudio.sampleSection(DjDeckSide.A, Double.NaN, 4.0))
        assertEq(DjDeckSide.B, DjLoadTarget.of(DjDeckSide.B).side)
        assertEq(null, DjLoadTarget.QUEUE.side)
    }

    fun testTrackedPositionSmoothsSmallDriftAndSnapsLargeJumps() {
        near(10.035, DjStudio.trackedPosition(10.0, 10.1, true), 1e-9)
        assertEq(12.0, DjStudio.trackedPosition(10.0, 12.0, true))
        assertEq(10.1, DjStudio.trackedPosition(10.0, 10.1, false))
        assertEq(10.0, DjStudio.trackedPosition(10.0, Double.NaN, true))
    }

    fun testAmplitudeFollowsThePlayheadSmoothly() {
        val bars = listOf(DjWaveBar(0.2, 0.1), DjWaveBar(1.0, 0.5), DjWaveBar(0.4, 0.9))
        assertEq(bars[0], DjStudio.amplitude(bars, 50.0, 300.0))
        assertEq(bars[1], DjStudio.amplitude(bars, 150.0, 300.0))
        near(0.6, DjStudio.amplitude(bars, 100.0, 300.0).level, 1e-9)
        near(0.4, DjStudio.amplitude(bars, 300.0, 300.0).level, 1e-9)
        assertEq(0.0, DjStudio.amplitude(bars, 301.0, 300.0).level)
        assertEq(0.0, DjStudio.amplitude(emptyList(), 5.0, 10.0).level)
        assertEq(0.0, DjStudio.amplitude(bars, Double.NaN, 10.0).level)
    }

    fun testSpinRampsUpAndDownWithoutJumps() {
        val spin = DjSpin()
        assertEq(10.0, spin.step(10.0, 1.0, now = 0.0))
        spin.hold(10.0, 0.0)
        var now = 0.0
        var shown = 10.0
        var target = 10.0
        for (frame in 1..30) {
            now += 1.0 / 60
            if (frame > 6) target += 1.0 / 60
            val next = spin.step(target, if (frame > 6) 1.0 else DjSpin.WAITING, now = now)
            expect(next >= shown)
            expect(next - shown < 0.03)
            shown = next
        }
        near(target, shown, 0.03)
        repeat(60) {
            now += 1.0 / 60
            val next = spin.step(target, 0.0, now = now)
            expect(abs(next - shown) < 0.03)
            shown = next
        }
        near(target, shown, 0.01)
        spin.hold(target, now)
        assertEq(target, spin.step(target, 1.0, now = now + 5))
        expect(spin.step(target, 1.0, now = now + 5.016) > target)
        assertEq(42.0, spin.step(42.0, 1.0, now = now + 5.03))
        assertEq(42.0, spin.step(42.0, 1.0, now = now + 5.03))
        assertEq(42.0, spin.step(Double.NaN, 1.0, now = now + 5.05))
    }

    fun testWaveformKeepsMovingBetweenRecordStepsTimedForTheNextFrame() {
        val spin = DjSpin()
        var now = 0.0
        spin.step(0.0, 1.0, 1.0, now)
        repeat(120) {
            now += 1.0 / 60
            spin.step(now, 1.0, 1.0, now)
        }
        val turned = spin.turn
        val ahead = now + 1.0 / 15
        val record = spin.step(ahead, 1.0, 1.0, ahead)
        var shown = spin.step(now, 1.0, 1.0, now)
        near(now, shown, 0.01)
        for (frame in 1..4) {
            val at = now + frame / 60.0
            val next = spin.step(at, 1.0, 1.0, at)
            near(1.0 / 60, next - shown, 0.002)
            expect(next <= record + 0.0001)
            shown = next
        }
        near(1.0 / 15, spin.turn - turned, 0.001)
        val later = ahead + 1.0 / 15
        near(1.0 / 15, spin.step(later, 1.0, 1.0, later) - record, 0.002)
        near(2.0 / 15, spin.turn - turned, 0.001)
    }

    fun testRecordTurnsAtOnceWhileTheMusicStarts() {
        val spin = DjSpin()
        spin.hold(10.0, 0.0)
        var now = 0.0
        repeat(60) {
            now += 1.0 / 60
            near(10.0, spin.step(10.0, DjSpin.WAITING, 1.0, now), 0.1)
        }
        near(1.0, spin.motor, 0.01)
        expect(spin.turn > 0.8)
        val turned = spin.turn
        spin.step(10.016, 1.0, 1.0, now + 0.4)
        near(0.4, spin.turn - turned, 0.01)
        spin.step(12.0, 1.0, 1.0, now + 5)
        near(0.4, spin.turn - turned, 0.01)
        spin.step(12.016, 1.0, 1.0, now + 5.016)
        near(0.416, spin.turn - turned, 0.01)
        spin.step(200.0, 1.0, 1.0, now + 5.03)
        near(0.416, spin.turn - turned, 0.01)
        spin.hold(200.1, now + 5.05)
        near(0.516, spin.turn - turned, 0.01)
        assertEq(0.0, spin.motor)
        spin.hold(5.0, now + 5.07)
        near(0.516, spin.turn - turned, 0.01)
    }

    fun testWaveBarsFollowTheSongLength() {
        assertEq(DjWaveAccumulator.BAR_COUNT, DjWaveAccumulator.bars(30.0))
        assertEq(4800, DjWaveAccumulator.bars(240.0))
        assertEq(DjWaveAccumulator.MOST_BARS, DjWaveAccumulator.bars(7200.0))
        assertEq(DjWaveAccumulator.BAR_COUNT, DjWaveAccumulator.bars(Double.NaN))
    }

    fun testWaveRangesSplitTheStream() {
        assertEq(5_000_001L, DjWaveRanges.declaredLength("https://rr1.googlevideo.com/videoplayback?itag=18&clen=5000001&dur=212.4"))
        assertEq(null, DjWaveRanges.declaredLength("https://example.com/a.mp4"))
        assertEq(5_000_001L, DjWaveRanges.total("bytes 0-2097151/5000001"))
        assertEq(null, DjWaveRanges.total("bytes 0-10/*"))
        assertEq(null, DjWaveRanges.total(null))
        val spans = DjWaveRanges.spans(5_000_001, 2_097_152)
        assertEq(listOf(2_097_152L..4_194_303L, 4_194_304L..5_000_000L), spans)
        assertEq(5_000_001L - 2_097_152L, spans.sumOf { it.last - it.first + 1 })
        expect(DjWaveRanges.spans(10, 10).isEmpty())
        assertEq("bytes=0-2097151", DjWaveRanges.header(0L..2_097_151L))
    }

    fun testTapTempo() {
        val tempo = DjTapTempo()
        assertEq(null, tempo.tap(10.0))
        assertEq(null, tempo.tap(10.5))
        assertEq(120.0, tempo.tap(11.0))
        assertEq(120.0, tempo.tap(11.5))
        assertEq(null, tempo.tap(20.0))
        assertEq(listOf(20.0), tempo.taps)
    }

    fun testSearchParsing() {
        val instances = """[["a",{"api":false,"uri":"https://a.example"}],["b",{"api":true,"uri":"http://b.example"}],["c",{"api":true,"uri":"https://c.example/"}]]"""
        assertEq(listOf("https://c.example"), DjStudio.searchInstances(instances))
        assertEq(emptyList<String>(), DjStudio.searchInstances("{}"))
        assertEq("https://c.example/api/v1/search?q=deep%20house%20%26%20mix&type=video", DjStudio.searchUrl("https://c.example", "deep house & mix"))
        val results = DjStudio.searchResults("""[{"videoId":"dQw4w9WgXcQ","title":"Song","author":"Band","lengthSeconds":213},{"videoId":"dQw4w9WgXcQ"},{"videoId":"bad"},{"url":"/watch?v=9bZkp7q19f0","title":""}]""")
        assertEq(listOf(DjSearchResult("dQw4w9WgXcQ", "Song", "Band", 213.0), DjSearchResult("9bZkp7q19f0", "Untitled video", "")), results)
    }

    fun testSavedStateIsSanitised() {
        val text = """{"a":{"videoId":"dQw4w9WgXcQ","cue":-4,"level":7,"rate":9,"bpm":"x","loop":true},"b":{"videoId":"nope"},"crossfader":3,"samplerLevel":-1,"sections":[{"deck":"b","start":12,"duration":90},null,null,null,{"deck":"a","start":1,"duration":2}]}"""
        val saved = DjSaved.decode(text)
        assertEq("dQw4w9WgXcQ", saved.a.videoId)
        assertEq(0.0, saved.a.cue)
        assertEq(1.0, saved.a.level)
        assertEq(1 + DjStudio.PITCH_RANGE, saved.a.rate)
        assertEq(0.0, saved.a.bpm)
        expect(saved.a.loop)
        assertEq("", saved.b.videoId)
        assertEq(1.0, saved.crossfader)
        assertEq(0.0, saved.samplerLevel)
        assertEq(DjStudio.SECTION_COUNT, saved.sections.size)
        assertEq(DjSection(DjDeckSide.B, 12.0, 30.0), saved.sections[0])
        assertEq(saved, DjSaved.decode(saved.encoded()))
        assertEq(DjSaved(), DjSaved.decode("nonsense"))
    }

    fun testSavedPlaybackResumesWhereItWas() {
        val old = DjSaved.decode("""{"a":{"videoId":"dQw4w9WgXcQ","cue":42}}""")
        assertEq(42.0, old.a.position)
        expect(!old.playing)
        expect(!old.automix)
        val saved = DjSaved.decode("""{"a":{"videoId":"dQw4w9WgXcQ","cue":5,"position":-3,"playing":true},"b":{"position":80,"playing":true},"automix":true}""")
        assertEq(0.0, saved.a.position)
        expect(saved.a.playing)
        assertEq(80.0, saved.b.position)
        expect(!saved.b.playing)
        expect(saved.playing)
        expect(saved.automix)
        val playing = DjSaved(b = DjSavedDeck(videoId = "9bZkp7q19f0", position = 61.5, playing = true))
        assertEq(playing, DjSaved.decode(playing.encoded()))
    }

    fun testPlaysAreCountedPerSongAndKept() {
        var library = DjStudio.played(emptyList(), "dQw4w9WgXcQ", "Song", "Band", 100.0)
        library = DjStudio.played(library, "9bZkp7q19f0", "Other", "", 200.0)
        library = DjStudio.played(library, "dQw4w9WgXcQ", "", "", 300.0)
        assertEq(listOf(DjPlayedSong("dQw4w9WgXcQ", "Song", "Band", 2, 300.0), DjPlayedSong("9bZkp7q19f0", "Other", "", 1, 200.0)), library)
        assertEq(library, DjStudio.played(library, "bad", "x", "", 400.0))
        assertEq(listOf(2), DjStudio.played(library, "9bZkp7q19f0", "", "", 400.0, 1).map { it.plays })
        assertEq("Named", DjStudio.retitled(library, "9bZkp7q19f0", "Named", "Artist")[1].title)
        assertEq(listOf("dQw4w9WgXcQ"), DjStudio.mostPlayed(library).map { it.id })
        assertEq("", DjStudio.playsLabel(0))
        assertEq("1 play", DjStudio.playsLabel(1))
        assertEq("12 plays", DjStudio.playsLabel(12))
        val saved = DjSaved(library = library)
        assertEq(saved, DjSaved.decode(saved.encoded()))
        val old = DjSaved.decode("""{"history":[{"id":"dQw4w9WgXcQ","title":"Song"},{"id":"9bZkp7q19f0"}]}""")
        assertEq(listOf("dQw4w9WgXcQ", "9bZkp7q19f0"), old.library.map { it.id })
        assertEq(listOf(1, 1), old.library.map { it.plays })
        val stored = DjSaved.decode("""{"history":[{"id":"dQw4w9WgXcQ"}],"library":[{"id":"9bZkp7q19f0","plays":7,"last":5},{"id":"nope"},{"id":"9bZkp7q19f0","plays":1}]}""")
        assertEq(listOf(DjPlayedSong("9bZkp7q19f0", "", "", 7, 5.0)), stored.library)
        assertEq(emptyList<DjPlayedSong>(), DjSaved.decode("""{"history":[{"id":"dQw4w9WgXcQ"}],"library":[]}""").library)
    }

    fun testSpinCatchesUpAfterAStalledFrame() {
        val spin = DjSpin()
        spin.hold(10.0, 0.0)
        near(10.0, spin.step(10.0, 1.0, now = 0.016), 0.01)
        assertEq(10.4, spin.step(10.4, 1.0, now = 0.4))
        expect(spin.step(10.416, 1.0, now = 0.416) > 10.4)
    }

    fun testHistoryKeepsLatestFirstWithoutDuplicates() {
        var history = DjStudio.remember(emptyList(), "dQw4w9WgXcQ", "Song", "Band")
        history = DjStudio.remember(history, "9bZkp7q19f0", "Other", "")
        history = DjStudio.remember(history, "dQw4w9WgXcQ", "", "")
        assertEq(listOf(DjHistoryItem("dQw4w9WgXcQ", "Song", "Band"), DjHistoryItem("9bZkp7q19f0", "Other", "")), history)
        assertEq(history, DjStudio.remember(history, "bad", "x", ""))
        assertEq(listOf(DjHistoryItem("9bZkp7q19f0", "New", "")), DjStudio.remember(history, "9bZkp7q19f0", "New", "", 1))
        val saved = DjSaved.decode("""{"history":[{"id":"dQw4w9WgXcQ","title":"Song"},{"id":"nope"},{"id":"dQw4w9WgXcQ","title":"Again"},{"title":"x"}]}""")
        assertEq(listOf(DjHistoryItem("dQw4w9WgXcQ", "Song", "")), saved.history)
        val queued = DjSaved.decode("""{"queue":[{"id":"q1","videoId":"dQw4w9WgXcQ"},{"id":"q1","videoId":"9bZkp7q19f0"},{"id":"q2","videoId":"x"}]}""")
        assertEq(listOf("dQw4w9WgXcQ"), queued.queue.map { it.videoId })
        assertEq(queued, DjSaved.decode(queued.encoded()))
        assertEq(saved, DjSaved.decode(saved.encoded()))
    }

    fun testDrumPadsAreAudibleAndBounded() {
        for (pad in DjDrumPad.entries) {
            val samples = DjDrumSynth.samples(pad, 22_050.0)
            expect(samples.isNotEmpty(), pad.key)
            expect(samples.all { it.isFinite() && abs(it) <= 1 }, pad.key)
            expect((samples.maxOfOrNull { abs(it) } ?: 0f) > 0.05f, pad.key)
            expect(abs(samples.last()) < 0.01f, pad.key)
        }
        expect(DjDrumSynth.samples(DjDrumPad.SNARE, 8000.0).contentEquals(DjDrumSynth.samples(DjDrumPad.SNARE, 8000.0)))
    }

    fun testYouTubePageResultsAreRead() {
        val data = """{"contents":{"list":[{"itemSectionRenderer":{"contents":[{"videoRenderer":{"videoId":"dQw4w9WgXcQ","title":{"runs":[{"text":"Never "},{"text":"Gonna"}]},"ownerText":{"runs":[{"text":"Rick"}]},"lengthText":{"simpleText":"3:33"}}},{"adSlotRenderer":{}},{"videoRenderer":{"videoId":"dQw4w9WgXcQ"}},{"videoRenderer":{"videoId":"9bZkp7q19f0","title":{"simpleText":"Gangnam } Style"},"lengthText":{"simpleText":"1:04:13"}}}]}}]}}"""
        val html = "<html><script>var ytInitialData = $data;</script></html>"
        assertEq(
            listOf(DjSearchResult("dQw4w9WgXcQ", "Never Gonna", "Rick", 213.0), DjSearchResult("9bZkp7q19f0", "Gangnam } Style", "", 3853.0)),
            DjStudio.pageResults(html),
        )
        assertEq(emptyList<DjSearchResult>(), DjStudio.pageResults("<html></html>"))
        assertEq("https://www.youtube.com/results?search_query=deep%20house&hl=en", DjStudio.pageSearchUrl("deep house"))
    }
}
