package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.PingCelebration
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class GenieGravityTest {
    private val arena = GenieArena.of(400.0, 800.0, ceiling = 60.0, floor = 720.0, side = 92.0)
    private val down = GenieVector(0.0, 1.0)

    fun testArenaStandsOnTheBarAndUnderTheTop() {
        val stand = 92 * GenieGravity.STAND
        assertEq(GenieBox(stand, 60 + stand, 400 - stand, 720 - stand), arena.box)
        assertEq(GenieEdge.Bottom, arena.nearest(GeniePoint(200.0, 670.0)))
        assertEq(GeniePoint(arena.box.minX, 300.0), arena.on(GenieEdge.Left, GeniePoint(10.0, 300.0)))
    }

    fun testADroppedSpiderFallsBouncesAndLandsOnTheFloor() {
        val track = GenieGravity.fly(GeniePoint(200.0, 200.0), GenieVector.ZERO, down, arena, 0.0, 0.0)
        assertEq(GenieEdge.Bottom, track.surface)
        expect(abs(track.final!!.y - arena.box.maxY) < 0.01)
        expect(track.airborne(0.1))
        expect(!track.airborne(track.end + 1))
        assertEq(0.0, track.turn)
    }

    fun testTiltPullsItSidewaysOntoTheLowWall() {
        val tilted = GenieVector(0.9, 0.3)
        assertEq(GenieEdge.Right, GenieGravity.floor(tilted))
        val track = GenieGravity.fly(GeniePoint(200.0, 400.0), GenieVector.ZERO, tilted, arena, 0.0, 0.0)
        assertEq(GenieEdge.Right, track.surface)
        expect(abs(GenieGravity.upright(GenieVector(1.0, 0.0)) + 90) < 0.001)
        expect(abs(GenieGravity.upright(down)) < 0.001)
    }

    fun testAFlickSidewaysBouncesOffAWallButAGentleOneClings() {
        val gentle = GenieGravity.fly(GeniePoint(200.0, 400.0), GenieVector(900.0, 0.0), down, arena, 0.0, 0.0)
        val hard = GenieGravity.fly(GeniePoint(200.0, 400.0), GenieVector(4000.0, 0.0), down, arena, 0.0, 0.0)
        assertEq(GenieEdge.Right, gentle.surface)
        expect(hard.surface != GenieEdge.Right)
    }

    fun testAFastFlickIsCappedAtTheTopSpeed() {
        val (_, velocity) = GenieGravity.flightStep(GeniePoint.ZERO, GenieVector(99999.0, 0.0), down)
        expect(velocity.length <= GenieGravity.MAX_SPEED + 0.001)
    }

    fun testClimbingIsSlowerThanComingDown() {
        val up = GenieGravity.pace(GenieVector(0.0, -1.0), GenieEdge.Left, down)
        val downhill = GenieGravity.pace(GenieVector(0.0, 1.0), GenieEdge.Left, down)
        assertEq(0.55, up)
        assertEq(1.2, downhill)
        val track = GenieGravity.crawl(
            arena.on(GenieEdge.Bottom, GeniePoint(60.0, 0.0)), GenieEdge.Bottom, GeniePoint(60.0, 0.0), GenieEdge.Top, arena, down,
            GenieCrawl.SPEED, 0.0, 0.0,
        )
        assertEq(GenieEdge.Top, track.surface)
        assertEq(GenieEdge.Left, track.moves[1].surface)
        val walking = track.gait(track.moves[0].start + track.moves[0].duration / 2)
        expect(walking.walking > 0.9)
    }

    fun testFromTheCeilingItLowersOnSilkAndClimbsBack() {
        val top = arena.on(GenieEdge.Top, GeniePoint(200.0, 0.0))
        val track = GenieGravity.silk(top, GenieEdge.Top, arena, down, 0.5, 1.0, false, GenieCrawl.SPEED, 0.0, 180.0)!!
        assertEq(GenieEdge.Top, track.surface)
        assertEq(top, track.final)
        expect(track.silk(0.5) == top)
        assertEq(null, GenieGravity.silk(arena.on(GenieEdge.Bottom, top), GenieEdge.Bottom, arena, down, 0.5, 1.0, false, 80.0, 0.0, 0.0))
    }

    fun testWanderStaysOnSurfaces() {
        var seed = 0
        val draws = listOf(0.1, 0.3, 0.7, 0.2, 0.9, 0.4, 0.6, 0.8)
        repeat(40) {
            val (point, edge) = GenieGravity.wander(GeniePoint(200.0, arena.box.maxY), GenieEdge.Bottom, arena, down) { draws[seed++ % draws.size] }
            expect(arena.touches(point, edge))
        }
    }

    fun testBadgePingsGiveTheGenieALine() {
        fun ping(meta: Map<String, Any?>) = ForkNotification("b", "badge", "You earned a badge", meta = meta)
        assertEq("You earned Stargazer!", PingCelebration.badgeLine(ping(mapOf("celebrate" to "badge", "badgeName" to "Stargazer", "tier" to "default"))))
        assertEq("You reached Silver Fan Club!", PingCelebration.badgeLine(ping(mapOf("celebrate" to "badge", "badgeName" to "Fan Club", "tier" to "silver"))))
        assertEq("You earned a new badge!", PingCelebration.badgeLine(ping(mapOf("celebrate" to "badge"))))
        assertEq(null, PingCelebration.badgeLine(ping(mapOf("celebrate" to "crm-email-tracked", "badgeName" to "X"))))
        assertEq(null, PingCelebration.badgeLine(ping(emptyMap())))
    }

    fun testWordsStopListeningAndSayBack() {
        expect(GenieWords.stopsListening("Genie, stop listening please"))
        expect(GenieWords.stopsListening("go to sleep"))
        expect(!GenieWords.stopsListening("stop listening to the radio"))
        assertEq("hello world", GenieWords.sayRequest("Genie, say hello world"))
        assertEq("I am a spider", GenieWords.sayRequest("repeat after me: I am a spider"))
        assertEq(null, GenieWords.sayRequest("say that again"))
        assertEq(null, GenieWords.sayRequest("what do you say"))
    }

    fun testChatKeepsFourExchangesForTenMinutes() {
        var now = 0.0
        val chat = GenieChat { now }
        val writer = GenieOutreachWriter("Model", 5.0) { messages -> "heard ${messages.size}" }
        assertEq(GenieChat.NO_MODEL, chat.answer("what's up", null))
        assertEq("hi there", chat.answer("Genie say hi there", writer))
        repeat(6) { chat.answer("question $it", writer) }
        assertEq(4, chat.history.size)
        assertEq("heard 10", chat.answer("one more", writer))
        now += 601
        assertEq("heard 2", chat.answer("after a break", writer))
    }

    fun testHintsPutNewsBeforeTips() {
        val lines = GenieHints.lines(GenieHintContext(unread = 2, running = 1))
        assertEq("You have 2 unread pings in your inbox.", lines[0])
        assertEq("One agent is working for you right now.", lines[1])
        expect(lines.contains("Turn on Suggestion mode in the You menu for people to reach out to."))
        assertEq(GenieHints.general.last(), lines.last())
    }
}
