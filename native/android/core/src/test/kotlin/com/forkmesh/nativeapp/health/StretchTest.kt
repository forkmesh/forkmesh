package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset
import kotlin.math.abs

class StretchTest {
    private class FakeCall : HealthCall {
        val replies = HashMap<String, HealthJson>()
        val calls = ArrayList<Triple<String, String, HealthJson?>>()

        override fun call(method: String, suffix: String, body: HealthJson?): HealthJson {
            calls += Triple(method, suffix, body)
            return replies["$method $suffix"] ?: replies[method] ?: mapOf("ok" to true)
        }

        fun bodies(method: String, suffix: String): List<HealthJson> = calls.filter { it.first == method && it.second == suffix }.mapNotNull { it.third }
    }

    private class FakeSpeaker : StretchSpeaker {
        val said = ArrayList<Pair<String, Boolean>>()
        var paused = 0
        var resumed = 0
        var stopped = 0
        override fun say(text: String, interrupt: Boolean) { said += text to interrupt }
        override fun pause() { paused += 1 }
        override fun resume() { resumed += 1 }
        override fun stop() { stopped += 1 }
    }

    private val figure = StretchFigure()

    private fun near(a: Double, b: Double, accuracy: Double = 1e-6, message: String = "") {
        expect(abs(a - b) <= accuracy, "$message expected $b but was $a")
    }

    private fun close(a: StretchPoint, b: StretchPoint, name: String = "") {
        near(a.x, b.x, message = "$name x")
        near(a.y, b.y, message = "$name y")
    }

    private val breathe = StretchMotion(
        8.0,
        frames = listOf(
            mapOf("t" to 0.0, "lArm" to 10.0, "rArm" to 10.0, "headNod" to 0.0),
            mapOf("t" to 0.5, "lArm" to 168.0, "rArm" to 168.0, "headNod" to -0.3),
            mapOf("t" to 1.0, "lArm" to 10.0, "rArm" to 10.0, "headNod" to 0.0),
        ),
    )

    private fun routine(id: String = "desk", workout: String = "flexibility") = StretchRoutine(
        id = id,
        title = if (id == "lymph") "Lymph tapping" else "Desk stretch",
        workout = workout,
        steps = listOf(
            StretchStep("a", "Hip circles", 20, "Hands on hips.", "Other way.", "breathe"),
            StretchStep("b", "Twist", 16, "Rotate.", "", "breathe"),
            StretchStep("c", "Shake out", 10, "Shake.", "Ignored.", "breathe"),
        ),
    )

    private fun play(steps: Double, state: StretchRun, dt: Double = 0.5): List<StretchCue> {
        val cues = ArrayList<StretchCue>()
        var done = 0.0
        while (done < steps - 1e-9) {
            cues += state.advance(dt, Instant.ofEpochSecond(1000 + done.toLong()))
            done += dt
        }
        return cues
    }

    fun testInterpolationEasesBetweenFrames() {
        near(StretchMath.pose(breathe, 0.0).lArm, 10.0, 1e-9)
        near(StretchMath.pose(breathe, 2.0).lArm, 89.0, 1e-9)
        near(StretchMath.pose(breathe, 4.0).lArm, 168.0, 1e-9)
        near(StretchMath.pose(breathe, 1.0).lArm, 10 + 158 * 0.15625, 1e-9)
        near(StretchMath.pose(breathe, 10.0).lArm, 89.0, 1e-9)
        near(StretchMath.pose(breathe, 6.0).headNod, -0.15, 1e-9)
        assertEq(0.0, StretchMath.pose(breathe, 3.0).twist)
        near(StretchMath.smooth(0.25), 0.15625, 1e-12)
    }

    fun testGeometryAtRest() {
        val body = StretchMath.body(figure, StretchPoseValues())
        close(body.hip, StretchPoint(100.0, 158.0), "hip")
        close(body.chest, StretchPoint(100.0, 78.0), "chest")
        close(body.leftShoulder, StretchPoint(70.0, 78.0), "leftShoulder")
        close(body.rightShoulder, StretchPoint(130.0, 78.0), "rightShoulder")
        close(body.head, StretchPoint(100.0, 54.0), "head")
        close(body.eyes[0], StretchPoint(95.0, 52.0), "eye0")
        close(body.eyes[1], StretchPoint(105.0, 52.0), "eye1")
        close(body.leftElbow, StretchPoint(70.0, 114.0), "leftElbow")
        close(body.leftHand, StretchPoint(70.0, 146.0), "leftHand")
        close(body.rightHand, StretchPoint(130.0, 146.0), "rightHand")
        close(body.leftHip, StretchPoint(88.0, 158.0), "leftHip")
        close(body.leftKnee, StretchPoint(83.0, 206.0), "leftKnee")
        close(body.rightKnee, StretchPoint(117.0, 206.0), "rightKnee")
        close(body.leftFoot, StretchPoint(78.0, 254.0), "leftFoot")
        assertEq(16.0, body.headRadius)
        assertEq(listOf(14.0, 10.0, 9.0, 9.0, 8.0, 8.0, 10.0, 10.0, 10.0, 10.0), body.segments.map { it.width })
    }

    fun testGeometryFollowsTheReadmeArithmetic() {
        val pose = StretchPoseValues(
            mapOf(
                "hipX" to 0.5, "knees" to 0.5, "lean" to 90.0, "twist" to 0.5, "fold" to 0.5, "shrug" to 1.0, "headTilt" to -90.0,
                "headTurn" to 1.0, "headNod" to 1.0, "lArm" to 90.0, "lElbow" to 90.0, "rArm" to 180.0, "rElbow" to 0.0,
            ),
        )
        val body = StretchMath.body(figure, pose, 1.0 / 12, "left")
        close(body.hip, StretchPoint(108.0, 165.0), "hip")
        close(body.chest, StretchPoint(168.0, 165.0), "chest")
        close(body.leftShoulder, StretchPoint(168.0, 134.0), "leftShoulder")
        close(body.rightShoulder, StretchPoint(168.0, 182.0), "rightShoulder")
        close(body.head, StretchPoint(169.5, 147.0), "head")
        close(body.eyes[0], StretchPoint(169.5, 148.0), "eye0")
        close(body.eyes[1], StretchPoint(179.5, 148.0), "eye1")
        close(body.leftElbow, StretchPoint(168.0, 98.0), "leftElbow")
        close(body.leftHand, StretchPoint(200.0, 94.0), "leftHand")
        close(body.rightElbow, StretchPoint(204.0, 182.0), "rightElbow")
        close(body.rightHand, StretchPoint(236.0, 182.0), "rightHand")
        close(body.leftKnee, StretchPoint(82.0, 209.5), "leftKnee")
        close(body.rightKnee, StretchPoint(126.0, 209.5), "rightKnee")
    }

    fun testLibraryDecodesFromTheApi() {
        val payload: HealthJson = mapOf(
            "version" to 1L,
            "figure" to mapOf("width" to 200L, "height" to 264L, "hip" to listOf(100L, 158L), "torso" to 80L, "feet" to listOf(listOf(78L, 254L), listOf(122L, 254L))),
            "poseFields" to listOf("lArm", "rArm"),
            "motions" to mapOf("breathe" to mapOf("period" to 8L, "tap" to "", "frames" to listOf(mapOf("t" to 0L, "lArm" to 10L), mapOf("t" to 1L, "lArm" to 10L)))),
            "routines" to listOf(
                mapOf(
                    "id" to "lymph", "title" to "Lymph tapping", "summary" to "s", "workout" to "mindAndBody", "seconds" to 30L,
                    "steps" to listOf(mapOf("id" to "x", "title" to "Belly breaths", "seconds" to 30L, "say" to "Breathe.", "half" to "", "motion" to "breathe")),
                ),
            ),
        )
        val library = StretchLibrary.fromJson(payload)
        assertEq(StretchWorkoutKind.MindAndBody, library.routines.first().workoutKind)
        assertEq("Belly breaths", library.routines.first().steps.first().title)
        assertEq(8.0, library.motions["breathe"]?.period)
        assertEq(30.0, library.figure.shoulderHalf)
        assertEq(listOf("lArm", "rArm"), library.poseFields)
    }

    fun testCueSchedule() {
        val state = StretchRun(routine(), Instant.ofEpochSecond(1000))
        assertEq(listOf("Hip circles. Hands on hips."), state.begin().map { it.text })
        expect(play(9.5, state).isEmpty(), "no early cue")
        assertEq(listOf("Other way."), play(0.5, state).map { it.text })
        expect(play(6.5, state).isEmpty(), "quiet")
        assertEq(listOf("Next: Twist"), play(0.5, state).map { it.text })
        assertEq(listOf("Twist. Rotate."), play(3.0, state).map { it.text })
        val twist = play(16.0, state)
        assertEq(listOf(StretchCueKind.Next, StretchCueKind.Start), twist.map { it.kind })
        assertEq(listOf("Next: Shake out", "Shake out. Shake."), twist.map { it.text })
        assertEq(listOf("Well done. That is your Desk stretch finished."), play(10.0, state).map { it.text })
        expect(state.finished, "finished")
        expect(state.completed, "completed")
        assertEq(3, state.stepsDone)
        near(state.activeSeconds, 46.0, 1e-9)
        expect(state.shouldLog, "should log")
    }

    fun testPauseBackAndSkip() {
        val state = StretchRun(routine(), Instant.ofEpochSecond(1000))
        state.begin()
        play(5.0, state)
        state.pause(Instant.ofEpochSecond(1005))
        expect(state.advance(30.0, Instant.ofEpochSecond(1035)).isEmpty(), "paused")
        near(state.activeSeconds, 5.0, 1e-9)
        state.resume(Instant.ofEpochSecond(1035))
        assertEq(listOf(StretchPause(Instant.ofEpochSecond(1005), Instant.ofEpochSecond(1035))), state.pauses)
        assertEq(listOf("Twist. Rotate."), state.skip(Instant.ofEpochSecond(1035)).map { it.text })
        assertEq(1, state.index)
        assertEq(listOf("Hip circles. Hands on hips."), state.back().map { it.text })
        play(4.0, state)
        assertEq(listOf("Hip circles. Hands on hips."), state.back().map { it.text })
        assertEq(0, state.index)
        assertEq(0.0, state.stepElapsed)
    }

    fun testLoggingRules() {
        val short = StretchRun(routine(), Instant.ofEpochSecond(1000))
        short.begin()
        play(29.5, short)
        short.stop(Instant.ofEpochSecond(1030))
        expect(short.finished, "finished")
        expect(!short.completed, "not completed")
        expect(!short.shouldLog, "too short")

        val enough = StretchRun(routine(), Instant.ofEpochSecond(1000))
        enough.begin()
        play(30.0, enough)
        enough.stop(Instant.ofEpochSecond(1040))
        expect(enough.shouldLog, "should log")
        val body = enough.logBody("2026-10-08", true)
        assertEq(30L, body["seconds"])
        assertEq(1L, body["steps"])
        assertEq(3L, body["stepsTotal"])
        assertEq(false, body["completed"])
        assertEq("android", body["source"])
        assertEq(true, body["health"])
        assertEq(1_000_000L, body["at"])
        assertEq(Instant.ofEpochSecond(1040), enough.workout.end)
    }

    private fun overview(installed: Boolean = true): HealthJson {
        val lymph = mapOf(
            "id" to "lymph", "title" to "Lymph tapping", "summary" to "", "workout" to "mindAndBody",
            "steps" to listOf(mapOf("id" to "x", "title" to "Belly breaths", "seconds" to 40L, "say" to "Breathe.", "half" to "Slowly.", "motion" to "breathe")),
        )
        return mapOf(
            "ok" to true, "installed" to installed, "day" to "2026-10-08",
            "profile" to mapOf("voice" to true, "routine" to "lymph", "tzOffsetMinutes" to 0L),
            "library" to mapOf("version" to 1L, "motions" to emptyMap<String, Any?>(), "routines" to listOf(lymph)),
            "today" to mapOf("day" to "2026-10-08", "sessions" to 1L, "seconds" to 300L),
            "summary" to mapOf("streak" to 3L, "bestStreak" to 5L, "sessions" to 9L, "weekSeconds" to 1200L, "weekSessions" to 4L),
            "days" to emptyList<Any?>(),
            "recent" to listOf(
                mapOf(
                    "id" to "s1", "day" to "2026-10-08", "loggedAt" to 0L, "routine" to "lymph", "seconds" to 270L, "steps" to 11L,
                    "stepsTotal" to 11L, "completed" to true, "at" to 0L, "source" to "web", "health" to false,
                ),
            ),
        )
    }

    private val start = Instant.ofEpochSecond(1_791_460_800)

    fun testSessionLogsToTheApi() {
        val call = FakeCall()
        call.replies["GET ?day=2026-10-08"] = overview()
        call.replies["POST /sessions"] = mapOf("ok" to true, "session" to mapOf("id" to "s2", "routine" to "lymph", "seconds" to 40L, "health" to false))
        var clock = start
        val model = StretchModel(call, null, ZoneOffset.UTC) { clock }
        val speaker = FakeSpeaker()
        model.speaker = speaker
        model.load()
        assertEq("lymph", model.selectedRoutine)
        assertEq("1 session · 5 min", model.stats.first().value)
        assertEq("4 min 30 s · completed · web", model.recent.first().detail)
        model.start()
        assertEq("Belly breaths. Breathe.", speaker.said.first().first)
        for (i in 0 until 40) {
            clock = clock.plusSeconds(1)
            model.tick(1.0)
        }
        assertEq(listOf("Belly breaths. Breathe.", "Slowly.", "Well done. That is your Lymph tapping finished."), speaker.said.map { it.first })
        val body = call.bodies("POST", "/sessions").first()
        assertEq(false, body["health"])
        assertEq(true, body["completed"])
        assertEq("lymph", body["routine"])
        assertEq("2026-10-08", body["day"])
        assertEq("s2", model.logged?.id)
        assertEq("Logged on your account.", model.logNote)
    }

    fun testShortStopIsNotLoggedAndPausedSpeechWaits() {
        val call = FakeCall()
        call.replies["GET ?day=2026-10-08"] = overview()
        var clock = start
        val model = StretchModel(call, null, ZoneOffset.UTC) { clock }
        val speaker = FakeSpeaker()
        model.speaker = speaker
        model.load()
        model.start()
        model.pause()
        assertEq(1, speaker.paused)
        model.tick(1.0)
        assertEq(0.0, model.run?.activeSeconds)
        model.back()
        model.resume()
        assertEq("Belly breaths. Breathe.", speaker.said.last().first)
        assertEq(true, speaker.said.last().second)
        clock = clock.plusSeconds(10)
        model.tick(10.0)
        model.stop()
        expect(call.bodies("POST", "/sessions").isEmpty(), "short session is not posted")
        expect(model.logNote.isNotEmpty(), "explains why")
    }

    fun testVoiceToggleSavesTheProfile() {
        val call = FakeCall()
        call.replies["GET ?day=2026-10-08"] = overview()
        val model = StretchModel(call, null, ZoneOffset.UTC) { start }
        val speaker = FakeSpeaker()
        model.speaker = speaker
        model.load()
        model.setVoice(false)
        assertEq(false, call.bodies("PUT", "/profile").first()["voice"])
        assertEq(1, speaker.stopped)
        model.start()
        expect(speaker.said.isEmpty(), "silent")
    }

    fun testNotInstalledShowsInstall() {
        val call = FakeCall()
        call.replies["GET ?day=2026-10-08"] = overview(installed = false)
        val model = StretchModel(call, null, ZoneOffset.UTC) { start }
        model.load()
        expect(model.showsInstall, "install panel")
        assertEq(1, model.routines.size)
        model.install()
        assertEq(true, call.bodies("POST", "/install").first()["voice"])
    }

    fun testText() {
        assertEq("5 min 30 s", StretchText.duration(330))
        assertEq("4 min", StretchText.duration(240))
        assertEq("45 s", StretchText.duration(45))
        assertEq("1:15", StretchText.countdown(75))
    }
}
