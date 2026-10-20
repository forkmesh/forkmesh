package com.forkmesh.nativeapp.feature.health

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.health.MotionSample
import com.forkmesh.nativeapp.health.StepsDailyLog
import com.forkmesh.nativeapp.health.StepsText
import com.forkmesh.nativeapp.health.StrideGait
import com.forkmesh.nativeapp.health.StrideMeter
import com.forkmesh.nativeapp.health.WalkDetector
import com.forkmesh.nativeapp.health.WalkEvent
import com.forkmesh.nativeapp.health.WalkMotion
import com.forkmesh.nativeapp.health.WalkMotionSample
import com.forkmesh.nativeapp.health.WalkPace
import com.forkmesh.nativeapp.health.WalkProgress
import com.forkmesh.nativeapp.health.HealthStepDay
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.time.Instant
import kotlin.math.max
import kotlin.math.roundToInt

object WalkActivity {
    var cadence by mutableDoubleStateOf(0.0)
        private set
    var running by mutableStateOf(false)
        private set
    var walking by mutableStateOf(false)
        private set
    var walkSteps by mutableIntStateOf(0)
        private set
    var walkDistanceM by mutableDoubleStateOf(0.0)
        private set
    var walkStartedAt by mutableStateOf<Instant?>(null)
        private set

    val stepping: Boolean get() = cadence > 0
    val moving: Boolean get() = stepping || walking
    val stepsPerMinute: Int get() = (cadence * 60).roundToInt()
    val gaitText: String get() = if (running) "Running" else StepsText.walkingGait(stepsPerMinute)

    private var bridge: StepsSensorBridge? = null
    private var watching = false
    private var pacing = false
    private var sensing = false
    private var pace = WalkPace()
    private val meter = StrideMeter()
    private var detector = WalkDetector()
    private var progress: WalkProgress? = null
    private var lastDaySteps = 0
    private var movingMark: Int? = null
    private var ticker: Job? = null

    fun start() {
        watching = true
        startPace()
    }

    fun stop() {
        stopPace()
        watching = false
        detector = WalkDetector()
        movingMark = null
        endWalk()
    }

    fun appResumed() {
        startPace()
    }

    fun appPaused() {
        stopPace()
    }

    private fun startPace() {
        if (pacing || !watching || !AppLifecycle.foreground.value) return
        val source = StepsSensorBridge()
        if (!source.available) return
        bridge = source
        pace = WalkPace()
        meter.reset()
        pacing = true
        source.startLiveSteps { day -> onDay(day) }
        sensing = source.startMotionSamples { samples -> onMotion(samples) }
        ticker = AppLifecycle.scope.launch {
            while (isActive) {
                delay(1000)
                tick()
            }
        }
    }

    private fun stopPace() {
        if (pacing) {
            pacing = false
            bridge?.stopLiveSteps()
            bridge?.stopMotionSamples()
            ticker?.cancel()
            ticker = null
        }
        bridge = null
        sensing = false
        pace = WalkPace()
        meter.reset()
        detector.forgetPendingStart()
        movingMark = null
        if (cadence != 0.0) cadence = 0.0
        if (running) running = false
    }

    private fun onDay(day: HealthStepDay) {
        if (!pacing) return
        lastDaySteps = day.steps
        pace.record(day.steps, 0.0, Instant.now())
        updateWalk()
        publish()
    }

    private fun onMotion(samples: List<MotionSample>) {
        if (!pacing || !sensing) return
        val before = meter.reading
        var reading = before
        for (sample in samples) reading = meter.add(sample)
        if (reading.cadence != before.cadence || reading.gait != before.gait) publish()
    }

    private fun tick() {
        if (!pacing) return
        val now = Instant.now()
        if (pace.expire(now)) publish()
        apply(detector.tick(now))
    }

    private fun publish() {
        val reading = meter.reading
        val metered = sensing && reading.cadence > 0
        val rate = if (metered) reading.cadence else pace.cadence
        val fast = if (metered) reading.gait == StrideGait.Running else pace.running
        if (cadence != rate) cadence = rate
        if (running != fast) running = fast
        val now = Instant.now()
        val motion = if (rate > 0) (if (fast) WalkMotion.Running else WalkMotion.Walking) else WalkMotion.Still
        apply(detector.observe(WalkMotionSample(motion, true, now), now))
        if (detector.movingSince != null && movingMark == null) {
            movingMark = lastDaySteps
        } else if (detector.movingSince == null && !detector.walking) {
            movingMark = null
        }
    }

    private fun apply(event: WalkEvent?) {
        when (event) {
            is WalkEvent.Started -> {
                progress = WalkProgress(event.at, baseline = movingMark ?: lastDaySteps)
                walking = true
                walkStartedAt = event.at
                updateWalk()
            }
            is WalkEvent.Ended -> endWalk()
            null -> Unit
        }
    }

    private fun endWalk() {
        progress = null
        walking = false
        walkStartedAt = null
        walkSteps = 0
        walkDistanceM = 0.0
    }

    private fun updateWalk() {
        val current = progress ?: return
        val steps = max(0, lastDaySteps - current.baseline)
        current.record(steps, steps * StepsDailyLog.STRIDE_M)
        walkSteps = current.steps
        walkDistanceM = current.distanceM
    }
}
