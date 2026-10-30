package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant

class WalkDetectorTest {
    private val t0 = Instant.ofEpochSecond(1_789_561_860)

    private fun at(offset: Double): Instant = t0.plusMillis((offset * 1000).toLong())

    private fun sample(motion: WalkMotion, offset: Double, confident: Boolean = true) =
        WalkMotionSample(motion, confident, at(offset))

    fun testStartsAfterSustainedWalking() {
        val detector = WalkDetector()
        assertEq(null, detector.observe(sample(WalkMotion.Walking, 0.0), at(0.0)))
        assertEq(at(WalkDetector.START_AFTER), detector.nextCheck)
        assertEq(null, detector.tick(at(WalkDetector.START_AFTER - 1)))
        assertEq(WalkEvent.Started(t0), detector.tick(at(WalkDetector.START_AFTER)))
        expect(detector.walking, "walking")
        assertEq(null, detector.nextCheck)
    }

    fun testIgnoresLowConfidenceAndUnknown() {
        val detector = WalkDetector()
        assertEq(null, detector.observe(sample(WalkMotion.Walking, 0.0, confident = false), at(0.0)))
        assertEq(null, detector.observe(sample(WalkMotion.Unknown, 5.0), at(5.0)))
        assertEq(null, detector.tick(at(600.0)))
        expect(!detector.walking, "not walking")
        assertEq(null, detector.nextCheck)
    }

    fun testShortWalkNeverStarts() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(0.0))
        assertEq(null, detector.observe(sample(WalkMotion.Still, 10.0), at(10.0)))
        assertEq(null, detector.tick(at(600.0)))
        expect(!detector.walking, "not walking")
    }

    fun testRunningCountsAsAWalk() {
        val detector = WalkDetector()
        assertEq(WalkEvent.Started(t0), detector.observe(sample(WalkMotion.Running, 0.0), at(60.0)))
    }

    fun testCatchesUpOnAWalkThatBeganWhileSuspended() {
        val detector = WalkDetector()
        assertEq(WalkEvent.Started(at(-1200.0)), detector.observe(sample(WalkMotion.Walking, -1200.0), at(0.0)))
    }

    fun testForgetPendingStartKeepsAWalkInProgress() {
        val pending = WalkDetector()
        pending.observe(sample(WalkMotion.Walking, 0.0), at(10.0))
        pending.forgetPendingStart()
        assertEq(null, pending.nextCheck)
        assertEq(null, pending.tick(at(3600.0)))
        expect(!pending.walking, "not walking")

        val walking = WalkDetector()
        walking.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        walking.observe(sample(WalkMotion.Still, 300.0), at(300.0))
        walking.forgetPendingStart()
        expect(walking.walking, "walking")
        assertEq(WalkEvent.Ended(at(300.0)), walking.tick(at(300.0 + WalkDetector.END_AFTER)))
    }

    fun testBriefStopKeepsTheWalk() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        assertEq(null, detector.observe(sample(WalkMotion.Still, 300.0), at(300.0)))
        assertEq(at(300.0 + WalkDetector.END_AFTER), detector.nextCheck)
        assertEq(null, detector.observe(sample(WalkMotion.Walking, 360.0), at(360.0)))
        assertEq(null, detector.nextCheck)
        assertEq(null, detector.tick(at(1000.0)))
        expect(detector.walking, "walking")
        assertEq(t0, detector.walkStartedAt)
    }

    fun testEndsAfterStayingStill() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        assertEq(null, detector.observe(sample(WalkMotion.Still, 600.0), at(600.0)))
        assertEq(null, detector.tick(at(600.0 + WalkDetector.END_AFTER - 1)))
        assertEq(WalkEvent.Ended(at(600.0)), detector.tick(at(600.0 + WalkDetector.END_AFTER)))
        assertEq(WalkDetector(), detector)
    }

    fun testStaleStillSampleEndsAtOnceButNotBeforeTheWalk() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        assertEq(WalkEvent.Ended(t0), detector.observe(sample(WalkMotion.Still, -3600.0), at(3600.0)))
    }

    fun testReplayEndsAtTheFirstLongStop() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        val history = listOf(
            sample(WalkMotion.Walking, 900.0), sample(WalkMotion.Still, 600.0),
            sample(WalkMotion.Walking, 660.0), sample(WalkMotion.Still, 1200.0),
        )
        assertEq(WalkEvent.Ended(at(1200.0)), detector.replay(history, at(1500.0)))
        assertEq(WalkDetector(), detector)
    }

    fun testReplayEndsAStopFollowedByAnotherWalk() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        val history = listOf(sample(WalkMotion.Still, 600.0), sample(WalkMotion.Walking, 600.0 + WalkDetector.END_AFTER + 60))
        assertEq(WalkEvent.Ended(at(600.0)), detector.replay(history, at(3600.0)))
    }

    fun testReplayKeepsAnOngoingWalk() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        assertEq(null, detector.replay(listOf(sample(WalkMotion.Still, 600.0), sample(WalkMotion.Walking, 650.0)), at(3600.0)))
        expect(detector.walking, "walking")
        assertEq(null, detector.nextCheck)
    }

    fun testProgressKeepsTheHighestReading() {
        val progress = WalkProgress(t0, baseline = 4200)
        progress.record(120, 90.0)
        progress.record(100, 95.0)
        assertEq(120, progress.steps)
        assertEq(95.0, progress.distanceM)
        assertEq(4320, progress.todaySteps)
    }

    fun testPaceFollowsStepDeltasAndExpires() {
        val pace = WalkPace()
        pace.record(100, 0.0, t0)
        assertEq(0.0, pace.cadence)
        pace.record(110, 0.0, at(5.0))
        assertEq(2.0, pace.cadence)
        assertEq(120, pace.stepsPerMinute)
        expect(!pace.expire(at(10.0)), "not stale yet")
        expect(pace.expire(at(5.0 + WalkPace.STALE_AFTER)), "stale")
        assertEq(0.0, pace.cadence)
        pace.record(200, 5.0, at(30.0))
        assertEq(WalkPace.MAX_CADENCE, pace.cadence)
        expect(pace.running, "fast cadence runs")
    }

    fun testSustainedPaceStartsAWalk() {
        val detector = WalkDetector()
        assertEq(null, detector.paced(true, at(0.0)))
        assertEq(null, detector.paced(true, at(10.0)))
        assertEq(at(WalkDetector.START_AFTER), detector.nextCheck)
        assertEq(WalkEvent.Started(t0), detector.paced(true, at(WalkDetector.START_AFTER)))
        expect(detector.walking, "walking")
    }

    fun testPaceStopCancelsAPendingStart() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(0.0))
        assertEq(null, detector.paced(false, at(15.0)))
        assertEq(null, detector.nextCheck)
        assertEq(null, detector.tick(at(600.0)))
        expect(!detector.walking, "not walking")
    }

    fun testPaceLeavesAWalkInProgressAlone() {
        val detector = WalkDetector()
        detector.observe(sample(WalkMotion.Walking, 0.0), at(60.0))
        detector.observe(sample(WalkMotion.Still, 300.0), at(300.0))
        assertEq(null, detector.paced(true, at(310.0)))
        assertEq(null, detector.paced(false, at(320.0)))
        assertEq(at(300 + WalkDetector.END_AFTER), detector.nextCheck)
        assertEq(t0, detector.walkStartedAt)
    }

    fun testBeginStartsEarlyOnceMovingLongEnough() {
        val detector = WalkDetector()
        assertEq(null, detector.begin(at(0.0), WalkDetector.LEAVE_START_AFTER))
        detector.paced(true, at(0.0))
        assertEq(null, detector.begin(at(WalkDetector.LEAVE_START_AFTER - 1), WalkDetector.LEAVE_START_AFTER))
        assertEq(WalkEvent.Started(t0), detector.begin(at(WalkDetector.LEAVE_START_AFTER), WalkDetector.LEAVE_START_AFTER))
        expect(detector.walking, "walking")
        assertEq(null, detector.begin(at(600.0), WalkDetector.LEAVE_START_AFTER))
    }
}
