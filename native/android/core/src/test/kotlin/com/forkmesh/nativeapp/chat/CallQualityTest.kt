package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CallQualityTest {
    private fun event(text: String): CallQualityEvent = CallQuality.parse(Json.parseObject(text)) ?: error("not a quality event")

    private val good = """{"event":"quality","live":true,"grade":"good","bars":4,"rttMs":48,"lossPct":0,"jitterMs":3,
        "sendKbps":900,"recvKbps":1800,"inWidth":1280,"inHeight":720,"inFps":30,"outWidth":640,"outHeight":360,
        "outFps":24,"limit":"bandwidth","mic":[0.5,0.25],"peer":[0.1,null]}"""

    fun testALiveEventReadsEveryField() {
        val parsed = event(good)
        val sample = parsed.sample
        expect(sample.live)
        assertEq(CallQualityGrade.GOOD, sample.grade)
        assertEq(4, sample.bars)
        assertEq(48.0, sample.rttMs)
        assertEq(1280, sample.inWidth)
        assertEq("bandwidth", sample.limit)
        assertEq(listOf(CallLevel(0.5, 0.1), CallLevel(0.25, null)), parsed.levels)
    }

    fun testOtherEventsAreIgnored() {
        assertEq(null, CallQuality.parse(Json.parseObject("""{"event":"route","route":"direct"}""")))
    }

    fun testThePreviewCarriesOnlyMicLevels() {
        val parsed = event("""{"event":"quality","live":false,"grade":"unknown","bars":0,"mic":[0.4,2,-1,"x"],"peer":[]}""")
        expect(!parsed.sample.live)
        assertEq(listOf(0.4, 1.0, 0.0, 0.0), parsed.levels.map { it.mic })
        val history = CallQualityHistory().add(parsed)
        assertEq("Microphone", CallQuality.heading(history))
        assertEq("Call quality: Microphone level", CallQuality.accessibility(history))
        assertEq("You", CallQuality.voiceText(history))
        assertEq(0, history.samples.size)
        assertEq("", CallQuality.rttText(history.latest))
    }

    fun testBarsAndGradesFollowTheWebThresholds() {
        assertEq(0, CallQuality.bars(null, null, 5.0))
        assertEq(4, CallQuality.bars(48.0, 0.0, 3.0))
        assertEq(3, CallQuality.bars(150.0, 0.0, 3.0))
        assertEq(2, CallQuality.bars(80.0, 3.0, 3.0))
        assertEq(1, CallQuality.bars(600.0, 0.0, 0.0))
        assertEq(1, CallQuality.bars(20.0, 12.0, 150.0))
        assertEq(CallQualityGrade.UNKNOWN, CallQualityGrade.ofBars(0))
        assertEq(CallQualityGrade.POOR, CallQualityGrade.ofBars(1))
        assertEq(CallQualityGrade.FAIR, CallQualityGrade.ofBars(2))
        assertEq(CallQualityGrade.FAIR, CallQualityGrade.ofBars(3))
        assertEq(CallQualityGrade.GOOD, CallQualityGrade.ofBars(4))
        val computed = event("""{"event":"quality","live":true,"rttMs":320,"lossPct":0.5,"jitterMs":10,"mic":[],"peer":[]}""")
        assertEq(2, computed.sample.bars)
        assertEq(CallQualityGrade.FAIR, computed.sample.grade)
    }

    fun testTextMatchesTheWebWidget() {
        val history = CallQualityHistory().add(event(good))
        val latest = history.latest
        assertEq("48 ms", CallQuality.rttText(latest))
        assertEq("48 ms · 0.0% loss · 3 ms jitter", CallQuality.describeNetwork(latest))
        assertEq("Call quality: Good connection · 48 ms · 0.0% loss · 3 ms jitter", CallQuality.accessibility(history))
        assertEq("Good connection", CallQuality.heading(history))
        assertEq("1280×720 · 30 fps · 1.8 Mb/s", CallQuality.videoInText(latest))
        assertEq("640×360 · 24 fps · 900 kb/s · limited by bandwidth", CallQuality.videoOutText(latest))
        assertEq("You ▲ · Them ▼", CallQuality.voiceText(history))
        assertEq("12% loss", CallQuality.describeNetwork(CallQualitySample(live = true, rttMs = 10.0, lossPct = 12.4)).substringAfter(" · "))
        assertEq("Measuring…", CallQuality.describeNetwork(CallQualitySample(live = true)))
        assertEq("No video", CallQuality.videoInText(CallQualitySample(live = true)))
        assertEq("Camera off", CallQuality.videoOutText(CallQualitySample(live = true)))
        assertEq("limited by CPU", CallQuality.describeVideo(CallQualitySample(limit = "cpu"), incoming = false))
        assertEq("limited", CallQuality.describeVideo(CallQualitySample(limit = "weird"), incoming = false))
    }

    fun testLossTicksTurnAmberThenRed() {
        assertEq(CallLossMark.NONE, CallQuality.lossMark(null))
        assertEq(CallLossMark.NONE, CallQuality.lossMark(0.9))
        assertEq(CallLossMark.FAIR, CallQuality.lossMark(1.0))
        assertEq(CallLossMark.FAIR, CallQuality.lossMark(7.9))
        assertEq(CallLossMark.POOR, CallQuality.lossMark(8.0))
    }

    fun testHistoryKeepsFiveSecondsOfLevelsAndAMinuteOfStats() {
        var history = CallQualityHistory()
        val levels = (1..10).joinToString(",") { "0.1" }
        repeat(70) { history = history.add(event("""{"event":"quality","live":true,"rttMs":$it,"mic":[$levels],"peer":[]}""")) }
        assertEq(CallQuality.LEVEL_SAMPLES, history.levels.size)
        assertEq(CallQuality.STATS_SAMPLES, history.samples.size)
        assertEq(69.0, history.samples.last().rttMs)
        assertEq(10.0, history.samples.first().rttMs)
    }
}
