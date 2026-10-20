package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class DjJamTest {
    fun testJamParsesMembersQueueAndDecks() {
        val reply = mapOf(
            "ok" to true,
            "jam" to mapOf(
                "id" to "j1", "title" to "Friday", "owner" to "alice", "me" to "bob", "myStatus" to "joined", "now" to 10_000L,
                "members" to listOf(mapOf("name" to "alice", "status" to "owner"), mapOf("name" to "bob", "status" to "joined"), mapOf("name" to "cara", "status" to "invited"), mapOf("status" to "x")),
                "queue" to listOf(
                    mapOf("id" to "q1", "videoId" to "dQw4w9WgXcQ", "title" to "Song", "author" to "Band", "addedBy" to "bob"),
                    mapOf("id" to "q2", "videoId" to "bad"),
                ),
                "decks" to mapOf(
                    "a" to mapOf("videoId" to "9bZkp7q19f0", "title" to "Other", "playing" to true, "position" to 30L, "rate" to 1.1, "at" to 8_000L, "by" to "alice"),
                    "b" to mapOf("videoId" to "nope", "rate" to 9L),
                ),
            ),
        )
        val jam = DjJam.parse(reply)!!
        assertEq("j1", jam.id)
        expect(!jam.isOwner)
        expect(jam.joined)
        assertEq(listOf("alice", "bob", "cara"), jam.members.map { it.name })
        assertEq(listOf("alice", "bob"), jam.listeners.map { it.name })
        assertEq(listOf(DjQueueItem.of("q1", "dQw4w9WgXcQ", "Song", "Band", "bob")), jam.queue)
        assertEq("9bZkp7q19f0", jam[DjDeckSide.A].videoId)
        near(32.2, jam[DjDeckSide.A].expectedPosition(jam.now), 1e-9)
        assertEq("", jam[DjDeckSide.B].videoId)
        assertEq(1 + DjStudio.PITCH_RANGE, jam[DjDeckSide.B].rate)
        assertEq(0.0, jam[DjDeckSide.B].expectedPosition(99_000))
        assertEq(null, DjJam.parse(mapOf("ok" to true)))
    }

    fun testJamListAndSeekThreshold() {
        val list = DjJamSummary.list(mapOf("jams" to listOf(mapOf("id" to "j1", "title" to "T", "owner" to "alice", "myStatus" to "invited", "members" to 3L), mapOf("title" to "no id"))))
        assertEq(listOf(DjJamSummary("j1", "T", "alice", "invited", 3)), list)
        expect(list[0].invited)
        expect(!DjJam.needsSeek(10.0, 11.0, true))
        expect(DjJam.needsSeek(10.0, 12.0, true))
        expect(DjJam.needsSeek(10.0, 10.8, false))
        expect(!DjJam.needsSeek(Double.NaN, 10.0, false))
        assertEq(listOf("bob"), DjJam.userNames(mapOf("users" to listOf(mapOf("name" to "Bob "), mapOf("name" to "bob"), mapOf("name" to "alice"), mapOf("x" to 1L))), setOf("alice")))
    }

    fun testDeckBodyRoundsPosition() {
        val body = DjJamDeck("dQw4w9WgXcQ", "Song", playing = true, position = 12.34567, rate = 1.02).body
        assertEq(12.346, body["position"])
        assertEq(true, body["playing"])
        assertEq("dQw4w9WgXcQ", body["videoId"])
    }

    fun testQueueItemsDecodeSafely() {
        val items = listOf(mapOf("id" to "a", "videoId" to "dQw4w9WgXcQ", "title" to "Song"), mapOf("videoId" to "bad")).mapNotNull(DjQueueItem::parse)
        assertEq(listOf("dQw4w9WgXcQ"), items.map { it.videoId })
        expect(DjQueueItem.parse(mapOf("videoId" to "dQw4w9WgXcQ"))!!.id.isNotEmpty())
    }

    fun testThirtyDistinctReactionsCoverEveryStyle() {
        assertEq(30, DjReaction.ALL.size)
        assertEq(30, DjReaction.ALL.map { it.id }.toSet().size)
        assertEq(30, DjReaction.ALL.map { it.emoji }.toSet().size)
        assertEq(DjReactionStyle.entries.toSet(), DjReaction.ALL.map { it.style }.toSet())
        for (reaction in DjReaction.ALL) {
            expect(Regex("^[a-z0-9-]{1,24}$").matches(reaction.id), reaction.id)
            expect(reaction.seconds > 1)
        }
        assertEq("🔥", DjReaction.find("fire")?.emoji)
        assertEq(null, DjReaction.find("nope"))
    }

    fun testJamParsesPresenceReactionsAndStatus() {
        val reply = mapOf(
            "jam" to mapOf(
                "id" to "j1", "me" to "bob", "myStatus" to "joined", "now" to 50_000L,
                "members" to listOf(
                    mapOf("name" to "alice", "status" to "owner"), mapOf("name" to "bob", "status" to "joined"), mapOf("name" to "cara", "status" to "joined"),
                    mapOf("name" to "dan", "status" to "joined"), mapOf("name" to "eve", "status" to "invited"),
                ),
                "presence" to listOf(
                    mapOf("name" to "alice", "state" to "studio", "playing" to true, "energy" to 0.8, "motion" to listOf(listOf(0.5, -0.5), listOf(9L, 1L), listOf("x", 1L), listOf(1L)), "ago" to 400L),
                    mapOf("name" to "bob", "state" to "listening", "playing" to true, "ago" to 1_000L),
                    mapOf("name" to "cara", "state" to "studio", "ago" to 60_000L),
                    mapOf("state" to "studio"),
                ),
                "reactions" to listOf(mapOf("id" to "r1", "name" to "alice", "kind" to "hearts", "ago" to 300L), mapOf("id" to "r2", "kind" to "made-up"), mapOf("kind" to "fire")),
            ),
        )
        val jam = DjJam.parse(reply)!!
        assertEq(listOf("alice", "bob", "cara"), jam.presence.map { it.name })
        val alice = jam.presenceOf("alice")!!
        expect(alice.dancing)
        assertEq(listOf(DjMotionSample(0.5, -0.5), DjMotionSample(2.0, 1.0)), alice.motion)
        assertEq(listOf(DjJamReaction("r1", "alice", "hearts", 300)), jam.reactions)
        assertEq(listOf(DjPersonStatus.STUDIO, DjPersonStatus.LISTENING, DjPersonStatus.AWAY, DjPersonStatus.AWAY, DjPersonStatus.INVITED), jam.members.map(jam::status))
        expect(DjPersonStatus.STUDIO.live)
        expect(!DjPersonStatus.AWAY.live)
        val later = jam.copy(now = 99, presence = emptyList(), reactions = emptyList())
        assertEq(jam.shared, later.shared)
        expect(later.copy(queue = listOf(DjQueueItem.of("q", "dQw4w9WgXcQ", "", ""))).shared != jam.shared)
    }

    fun testMotionIsDownsampledMeasuredAndReplayed() {
        val still = List(40) { DjMotionSample.STILL }
        assertEq(0.0, DjMotion.energy(still))
        assertEq(0.0, DjMotion.energy(emptyList()))
        val shaking = List(40) { DjMotionSample(if (it % 2 == 0) 0.9 else -0.9, 0.2) }
        expect(DjMotion.energy(shaking) > DjMotion.DANCING)
        val shared = DjMotion.downsample(shaking)
        assertEq(DjMotion.SHARED, shared.size)
        expect(shared.all { abs(it.x) == 0.9 })
        assertEq(listOf(DjMotionSample(2.0, -2.0)), DjMotion.downsample(listOf(DjMotionSample(5.0, -5.0))))
        val path = listOf(DjMotionSample(0.0, 0.0), DjMotionSample(1.0, -1.0))
        near(0.5, DjMotion.replay(path, 0.5).x, 1e-9)
        near(-0.5, DjMotion.replay(path, 1.5).y, 1e-9)
        assertEq(DjMotionSample.STILL, DjMotion.replay(emptyList(), 0.3))
        val body = DjJamPresence.body(DjPresenceState.STUDIO, true, shaking)
        assertEq("studio", body["state"])
        assertEq(DjMotion.SHARED, (body["motion"] as List<*>).size)
        expect((body["energy"] as Double) > 0)
    }

    fun testAutomixStartsPreloadsAndMixesThroughTheQueue() {
        val a = DjDeckSide.A
        val b = DjDeckSide.B
        val empty = DjAutomixDeck(false)
        val cued = DjAutomixDeck(true, duration = 200.0)
        assertEq(DjAutomixStep.Wait, DjAutomix.step(empty, empty, a, false))
        assertEq(DjAutomixStep.Preload(a), DjAutomix.step(empty, empty, a, true))
        assertEq(DjAutomixStep.Start(b), DjAutomix.step(empty, cued, a, false))
        val early = DjAutomixDeck(true, true, 20.0, 200.0)
        assertEq(DjAutomixStep.Preload(b), DjAutomix.step(early, empty, a, true))
        assertEq(DjAutomixStep.Wait, DjAutomix.step(early, empty, a, false))
        assertEq(DjAutomixStep.Wait, DjAutomix.step(early, cued, a, true))
        val ending = DjAutomixDeck(true, true, 194.0, 200.0)
        assertEq(DjAutomixStep.Mix(a, b, 6.0), DjAutomix.step(ending, cued, a, false))
        val fast = DjAutomixDeck(true, true, 180.0, 200.0, 2.5)
        assertEq(DjAutomixStep.Mix(a, b, 8.0), DjAutomix.step(fast, cued, a, false))
        val spent = DjAutomixDeck(true, position = 199.0, duration = 200.0)
        expect(!spent.fresh)
        assertEq(DjAutomixStep.Preload(a), DjAutomix.step(spent, early, b, true))
        assertEq(DjAutomixStep.Wait, DjAutomix.step(spent, empty, a, false))
        assertEq(DjAutomixStep.Wait, DjAutomix.step(early, ending, b, true))
    }

    fun testAutomixReplaysPlayedSongsOnceTheQueueRunsOut() {
        val library = listOf(
            DjPlayedSong.of("aaaaaaaaaaa", "Newest", "", 3, 500.0),
            DjPlayedSong.of("bbbbbbbbbbb", "Newer", "", 1, 400.0),
            DjPlayedSong.of("ccccccccccc", "Older", "", 2, 200.0),
            DjPlayedSong.of("ddddddddddd", "Oldest", "", 1, 100.0),
        )
        val picked = mutableSetOf<String>()
        for (seed in 0L until 64L) picked.add(DjAutomix.next(library, setOf("aaaaaaaaaaa"), setOf("aaaaaaaaaaa"), seed)?.id ?: "")
        assertEq(setOf("ccccccccccc", "ddddddddddd"), picked)
        assertEq(DjAutomix.next(library, emptySet(), emptySet(), 1), DjAutomix.next(library, emptySet(), emptySet(), 1))
        val one = listOf(library[0])
        assertEq(null, DjAutomix.next(one, setOf("aaaaaaaaaaa"), setOf("aaaaaaaaaaa"), 3))
        assertEq("aaaaaaaaaaa", DjAutomix.next(one, setOf("aaaaaaaaaaa"), emptySet(), 3)?.id)
        assertEq(null, DjAutomix.next(emptyList(), emptySet(), emptySet(), 3))
        val spent = DjAutomixDeck(true, position = 199.0, duration = 200.0)
        assertEq(DjAutomixStep.Preload(DjDeckSide.B), DjAutomix.step(spent, spent, DjDeckSide.B, true))
        val early = DjAutomixDeck(true, true, 20.0, 200.0)
        assertEq(DjAutomixStep.Preload(DjDeckSide.B), DjAutomix.step(early, spent, DjDeckSide.A, true))
        assertEq(DjAutomixStep.Preload(DjDeckSide.B), DjAutomix.step(early, DjAutomixDeck(false), DjDeckSide.A, true))
        val ending = DjAutomixDeck(true, true, 191.0, 200.0)
        val cued = DjAutomixDeck(true, duration = 180.0)
        assertEq(DjAutomixStep.Mix(DjDeckSide.A, DjDeckSide.B, 8.0), DjAutomix.step(ending, cued, DjDeckSide.A, true))
    }
}
