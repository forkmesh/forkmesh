package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.OrgApiException
import java.time.Instant
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

private object StretchRead {
    fun double(value: Any?, fallback: Double): Double = (value as? Number)?.toDouble() ?: fallback

    fun int(value: Any?, fallback: Int): Int = (value as? Number)?.toInt() ?: fallback

    fun long(value: Any?, fallback: Long): Long = (value as? Number)?.toLong() ?: fallback

    fun string(value: Any?, fallback: String): String = (value as? String) ?: fallback

    fun bool(value: Any?, fallback: Boolean): Boolean = (value as? Boolean) ?: fallback

    fun doubles(value: Any?, fallback: List<Double>): List<Double> {
        val list = value as? List<*> ?: return fallback
        if (list.any { it !is Number }) return fallback
        return list.map { (it as Number).toDouble() }
    }
}

data class StretchPoint(val x: Double, val y: Double) {
    operator fun plus(other: StretchPoint) = StretchPoint(x + other.x, y + other.y)
    operator fun minus(other: StretchPoint) = StretchPoint(x - other.x, y - other.y)
    operator fun times(factor: Double) = StretchPoint(x * factor, y * factor)
}

data class StretchFigure(
    val width: Double = 200.0,
    val height: Double = 264.0,
    val hip: List<Double> = listOf(100.0, 158.0),
    val hipShift: Double = 16.0,
    val kneeDip: Double = 14.0,
    val torso: Double = 80.0,
    val shoulderHalf: Double = 30.0,
    val shrug: Double = 7.0,
    val neck: Double = 24.0,
    val head: Double = 16.0,
    val upperArm: Double = 36.0,
    val forearm: Double = 32.0,
    val hipHalf: Double = 12.0,
    val feet: List<List<Double>> = listOf(listOf(78.0, 254.0), listOf(122.0, 254.0)),
    val tapLift: Double = 4.0,
    val tapHz: Double = 3.0,
) {
    val hipPoint: StretchPoint get() = StretchPoint(if (hip.size > 1) hip[0] else 100.0, if (hip.size > 1) hip[1] else 158.0)

    fun foot(index: Int): StretchPoint {
        if (index >= feet.size || feet[index].size <= 1) {
            return if (index == 0) StretchPoint(78.0, 254.0) else StretchPoint(122.0, 254.0)
        }
        return StretchPoint(feet[index][0], feet[index][1])
    }

    companion object {
        fun fromJson(json: HealthJson): StretchFigure {
            val d = StretchFigure()
            val rawFeet = json["feet"] as? List<*>
            val feet = if (rawFeet != null && rawFeet.all { row -> row is List<*> && row.all { it is Number } }) {
                rawFeet.map { row -> (row as List<*>).map { (it as Number).toDouble() } }
            } else {
                d.feet
            }
            return StretchFigure(
                width = StretchRead.double(json["width"], d.width),
                height = StretchRead.double(json["height"], d.height),
                hip = StretchRead.doubles(json["hip"], d.hip),
                hipShift = StretchRead.double(json["hipShift"], d.hipShift),
                kneeDip = StretchRead.double(json["kneeDip"], d.kneeDip),
                torso = StretchRead.double(json["torso"], d.torso),
                shoulderHalf = StretchRead.double(json["shoulderHalf"], d.shoulderHalf),
                shrug = StretchRead.double(json["shrug"], d.shrug),
                neck = StretchRead.double(json["neck"], d.neck),
                head = StretchRead.double(json["head"], d.head),
                upperArm = StretchRead.double(json["upperArm"], d.upperArm),
                forearm = StretchRead.double(json["forearm"], d.forearm),
                hipHalf = StretchRead.double(json["hipHalf"], d.hipHalf),
                feet = feet,
                tapLift = StretchRead.double(json["tapLift"], d.tapLift),
                tapHz = StretchRead.double(json["tapHz"], d.tapHz),
            )
        }
    }
}

data class StretchMotion(
    val period: Double,
    val tap: String = "",
    val frames: List<Map<String, Double>>,
) {
    companion object {
        fun fromJson(json: HealthJson): StretchMotion {
            val frames = (json["frames"] as? List<*>)?.mapNotNull { frame ->
                val map = frame as? Map<*, *> ?: return@mapNotNull null
                val out = LinkedHashMap<String, Double>()
                for ((key, value) in map) {
                    if (key is String && value is Number) out[key] = value.toDouble()
                }
                out
            } ?: emptyList()
            return StretchMotion(StretchRead.double(json["period"], 1.0), StretchRead.string(json["tap"], ""), frames)
        }
    }
}

data class StretchStep(
    val id: String,
    val title: String,
    val seconds: Int,
    val say: String = "",
    val half: String = "",
    val motion: String = "",
) {
    companion object {
        fun fromJson(json: HealthJson): StretchStep = StretchStep(
            id = StretchRead.string(json["id"], ""),
            title = StretchRead.string(json["title"], ""),
            seconds = max(1, StretchRead.int(json["seconds"], 1)),
            say = StretchRead.string(json["say"], ""),
            half = StretchRead.string(json["half"], ""),
            motion = StretchRead.string(json["motion"], ""),
        )
    }
}

enum class StretchWorkoutKind(val raw: String) {
    Flexibility("flexibility"),
    MindAndBody("mindAndBody"),
}

data class StretchRoutine(
    val id: String,
    val title: String,
    val summary: String = "",
    val workout: String = "flexibility",
    val steps: List<StretchStep>,
    val seconds: Int = steps.sumOf { it.seconds },
) {
    val workoutKind: StretchWorkoutKind get() = StretchWorkoutKind.entries.firstOrNull { it.raw == workout } ?: StretchWorkoutKind.Flexibility
    val durationLabel: String get() = StretchText.duration(seconds)

    companion object {
        fun fromJson(json: HealthJson): StretchRoutine {
            val steps = HealthJsonRead.objects(json["steps"]).map { StretchStep.fromJson(it) }
            val total = StretchRead.int(json["seconds"], 0)
            return StretchRoutine(
                id = StretchRead.string(json["id"], ""),
                title = StretchRead.string(json["title"], ""),
                summary = StretchRead.string(json["summary"], ""),
                workout = StretchRead.string(json["workout"], "flexibility"),
                steps = steps,
                seconds = if (total > 0) total else steps.sumOf { it.seconds },
            )
        }
    }
}

object StretchPose {
    val FIELDS = listOf(
        "hipX", "knees", "lean", "twist", "fold", "shrug", "headTilt", "headTurn", "headNod",
        "lArm", "lElbow", "rArm", "rElbow",
    )
}

data class StretchPoseValues(val values: Map<String, Double> = emptyMap()) {
    operator fun get(field: String): Double = values[field] ?: 0.0
    val hipX: Double get() = this["hipX"]
    val knees: Double get() = this["knees"]
    val lean: Double get() = this["lean"]
    val twist: Double get() = this["twist"]
    val fold: Double get() = this["fold"]
    val shrug: Double get() = this["shrug"]
    val headTilt: Double get() = this["headTilt"]
    val headTurn: Double get() = this["headTurn"]
    val headNod: Double get() = this["headNod"]
    val lArm: Double get() = this["lArm"]
    val lElbow: Double get() = this["lElbow"]
    val rArm: Double get() = this["rArm"]
    val rElbow: Double get() = this["rElbow"]
}

data class StretchLibrary(
    val version: Int = 1,
    val figure: StretchFigure = StretchFigure(),
    val poseFields: List<String> = StretchPose.FIELDS,
    val motions: Map<String, StretchMotion> = emptyMap(),
    val routines: List<StretchRoutine> = emptyList(),
) {
    fun routine(id: String): StretchRoutine? = routines.firstOrNull { it.id == id }

    companion object {
        fun fromJson(json: HealthJson): StretchLibrary {
            val fields = (json["poseFields"] as? List<*>)?.takeIf { list -> list.all { it is String } }?.map { it as String } ?: emptyList()
            val motions = LinkedHashMap<String, StretchMotion>()
            for ((key, value) in HealthJsonRead.obj(json["motions"])) {
                val map = value as? Map<*, *> ?: continue
                @Suppress("UNCHECKED_CAST")
                motions[key] = StretchMotion.fromJson(map as HealthJson)
            }
            return StretchLibrary(
                version = StretchRead.int(json["version"], 1),
                figure = StretchFigure.fromJson(HealthJsonRead.obj(json["figure"])),
                poseFields = fields.ifEmpty { StretchPose.FIELDS },
                motions = motions,
                routines = HealthJsonRead.objects(json["routines"]).map { StretchRoutine.fromJson(it) },
            )
        }
    }
}

data class StretchProfile(
    val voice: Boolean = true,
    val routine: String = "desk",
    val tzOffsetMinutes: Int = 0,
) {
    companion object {
        fun fromJson(json: HealthJson): StretchProfile = StretchProfile(
            voice = StretchRead.bool(json["voice"], true),
            routine = StretchRead.string(json["routine"], "desk"),
            tzOffsetMinutes = StretchRead.int(json["tzOffsetMinutes"], 0),
        )
    }
}

data class StretchDayTotal(val day: String = "", val sessions: Int = 0, val seconds: Int = 0) {
    companion object {
        fun fromJson(json: HealthJson): StretchDayTotal = StretchDayTotal(
            StretchRead.string(json["day"], ""), StretchRead.int(json["sessions"], 0), StretchRead.int(json["seconds"], 0),
        )
    }
}

data class StretchSummary(
    val streak: Int = 0,
    val bestStreak: Int = 0,
    val sessions: Int = 0,
    val weekSeconds: Int = 0,
    val weekSessions: Int = 0,
) {
    companion object {
        fun fromJson(json: HealthJson): StretchSummary = StretchSummary(
            streak = StretchRead.int(json["streak"], 0),
            bestStreak = StretchRead.int(json["bestStreak"], 0),
            sessions = StretchRead.int(json["sessions"], 0),
            weekSeconds = StretchRead.int(json["weekSeconds"], 0),
            weekSessions = StretchRead.int(json["weekSessions"], 0),
        )
    }
}

data class StretchSessionRecord(
    val id: String = "",
    val day: String = "",
    val loggedAt: Long = 0,
    val routine: String = "desk",
    val seconds: Int = 0,
    val steps: Int = 0,
    val stepsTotal: Int = 0,
    val completed: Boolean = false,
    val at: Long = 0,
    val source: String = "web",
    val health: Boolean = false,
) {
    companion object {
        fun fromJson(json: HealthJson): StretchSessionRecord = StretchSessionRecord(
            id = StretchRead.string(json["id"], ""),
            day = StretchRead.string(json["day"], ""),
            loggedAt = StretchRead.long(json["loggedAt"], 0),
            routine = StretchRead.string(json["routine"], "desk"),
            seconds = StretchRead.int(json["seconds"], 0),
            steps = StretchRead.int(json["steps"], 0),
            stepsTotal = StretchRead.int(json["stepsTotal"], 0),
            completed = StretchRead.bool(json["completed"], false),
            at = StretchRead.long(json["at"], 0),
            source = StretchRead.string(json["source"], "web"),
            health = StretchRead.bool(json["health"], false),
        )
    }
}

data class StretchOverview(
    val installed: Boolean = false,
    val day: String = "",
    val profile: StretchProfile = StretchProfile(),
    val library: StretchLibrary = StretchLibrary(),
    val today: StretchDayTotal = StretchDayTotal(),
    val summary: StretchSummary = StretchSummary(),
    val days: List<StretchDayTotal> = emptyList(),
    val recent: List<StretchSessionRecord> = emptyList(),
) {
    companion object {
        fun fromJson(json: HealthJson): StretchOverview = StretchOverview(
            installed = StretchRead.bool(json["installed"], false),
            day = StretchRead.string(json["day"], ""),
            profile = StretchProfile.fromJson(HealthJsonRead.obj(json["profile"])),
            library = StretchLibrary.fromJson(HealthJsonRead.obj(json["library"])),
            today = StretchDayTotal.fromJson(HealthJsonRead.obj(json["today"])),
            summary = StretchSummary.fromJson(HealthJsonRead.obj(json["summary"])),
            days = HealthJsonRead.objects(json["days"]).map { StretchDayTotal.fromJson(it) },
            recent = HealthJsonRead.objects(json["recent"]).map { StretchSessionRecord.fromJson(it) },
        )
    }
}

data class StretchSegment(val from: StretchPoint, val to: StretchPoint, val width: Double)

data class StretchBody(
    val hip: StretchPoint,
    val chest: StretchPoint,
    val leftShoulder: StretchPoint,
    val rightShoulder: StretchPoint,
    val leftElbow: StretchPoint,
    val rightElbow: StretchPoint,
    val leftHand: StretchPoint,
    val rightHand: StretchPoint,
    val leftHip: StretchPoint,
    val rightHip: StretchPoint,
    val leftKnee: StretchPoint,
    val rightKnee: StretchPoint,
    val leftFoot: StretchPoint,
    val rightFoot: StretchPoint,
    val head: StretchPoint,
    val headRadius: Double,
    val eyes: List<StretchPoint>,
    val eyeRadius: Double,
) {
    val segments: List<StretchSegment>
        get() = listOf(
            StretchSegment(hip, chest, 14.0),
            StretchSegment(leftShoulder, rightShoulder, 10.0),
            StretchSegment(leftShoulder, leftElbow, 9.0),
            StretchSegment(rightShoulder, rightElbow, 9.0),
            StretchSegment(leftElbow, leftHand, 8.0),
            StretchSegment(rightElbow, rightHand, 8.0),
            StretchSegment(leftHip, leftKnee, 10.0),
            StretchSegment(leftKnee, leftFoot, 10.0),
            StretchSegment(rightHip, rightKnee, 10.0),
            StretchSegment(rightKnee, rightFoot, 10.0),
        )
}

object StretchMath {
    fun smooth(u: Double): Double = u * u * (3 - 2 * u)

    fun rotate(degrees: Double, x: Double, y: Double): StretchPoint {
        val radians = degrees * Math.PI / 180
        return StretchPoint(x * cos(radians) - y * sin(radians), x * sin(radians) + y * cos(radians))
    }

    fun phase(seconds: Double, period: Double): Double {
        if (period <= 0) return 0.0
        var value = (seconds % period) / period
        if (value < 0) value += 1
        return value
    }

    fun pose(motion: StretchMotion, seconds: Double, fields: List<String> = StretchPose.FIELDS): StretchPoseValues {
        val frames = motion.frames
        val first = frames.firstOrNull() ?: return StretchPoseValues()
        if (frames.size <= 1 || motion.period <= 0) return StretchPoseValues(first)
        val position = phase(seconds, motion.period)
        var a = frames[frames.size - 2]
        var b = frames[frames.size - 1]
        for (index in 0 until frames.size - 1) {
            val low = frames[index]["t"] ?: 0.0
            val high = frames[index + 1]["t"] ?: 1.0
            if (low <= position && position <= high) {
                a = frames[index]
                b = frames[index + 1]
                break
            }
        }
        val start = a["t"] ?: 0.0
        val span = (b["t"] ?: 1.0) - start
        val u = smooth(if (span > 0) min(1.0, max(0.0, (position - start) / span)) else 0.0)
        val values = LinkedHashMap<String, Double>()
        for (field in fields) {
            val from = a[field] ?: 0.0
            values[field] = from + ((b[field] ?: 0.0) - from) * u
        }
        return StretchPoseValues(values)
    }

    fun body(figure: StretchFigure, pose: StretchPoseValues, seconds: Double = 0.0, tap: String = ""): StretchBody {
        val base = figure.hipPoint
        val hip = StretchPoint(base.x + pose.hipX * figure.hipShift, base.y + pose.knees * figure.kneeDip)
        val length = figure.torso * (1 - 0.5 * pose.fold)
        val chest = hip + rotate(pose.lean, 0.0, -1.0) * length
        val halfWidth = figure.shoulderHalf * (1 - 0.4 * abs(pose.twist))
        val side = rotate(pose.lean, 1.0, 0.0)
        val lift = StretchPoint(0.0, -pose.shrug * figure.shrug)
        val leftShoulder = chest - side * halfWidth + lift
        val rightShoulder = chest + side * halfWidth + lift
        val headAngle = pose.lean + pose.headTilt
        val head = chest + rotate(headAngle, 0.0, -1.0) * figure.neck + StretchPoint(pose.twist * 3, pose.headNod * 6)
        val eyes = listOf(-1.0, 1.0).map { sign ->
            head + rotate(headAngle, sign * 5 + pose.headTurn * 5, -2 + pose.headNod * 3)
        }
        val tapOffset = figure.tapLift * abs(sin(2 * Math.PI * figure.tapHz * seconds))
        fun arm(shoulder: StretchPoint, sign: Double, angle: Double, elbow: Double, tapping: Boolean): Pair<StretchPoint, StretchPoint> {
            val a = angle * Math.PI / 180
            val e = (angle + elbow) * Math.PI / 180
            val joint = shoulder + rotate(pose.lean, sign * sin(a), cos(a)) * figure.upperArm
            var hand = joint + rotate(pose.lean, sign * sin(e), cos(e)) * figure.forearm
            if (tapping) hand = StretchPoint(hand.x, hand.y - tapOffset)
            return joint to hand
        }
        val left = arm(leftShoulder, -1.0, pose.lArm, pose.lElbow, tap == "left" || tap == "both")
        val right = arm(rightShoulder, 1.0, pose.rArm, pose.rElbow, tap == "right" || tap == "both")
        val leftHip = hip - StretchPoint(figure.hipHalf, 0.0)
        val rightHip = hip + StretchPoint(figure.hipHalf, 0.0)
        val leftFoot = figure.foot(0)
        val rightFoot = figure.foot(1)
        val leftKnee = StretchPoint((leftHip.x + leftFoot.x) / 2 - pose.knees * 10, (leftHip.y + leftFoot.y) / 2)
        val rightKnee = StretchPoint((rightHip.x + rightFoot.x) / 2 + pose.knees * 10, (rightHip.y + rightFoot.y) / 2)
        return StretchBody(
            hip, chest, leftShoulder, rightShoulder, left.first, right.first, left.second, right.second,
            leftHip, rightHip, leftKnee, rightKnee, leftFoot, rightFoot, head, figure.head, eyes, 2.0,
        )
    }

    fun body(library: StretchLibrary, step: StretchStep?, seconds: Double): StretchBody {
        val motion = step?.let { library.motions[it.motion] }
        if (motion == null) return body(library.figure, StretchPoseValues(mapOf("lArm" to 8.0, "rArm" to 8.0)))
        val current = pose(motion, seconds, library.poseFields)
        return body(library.figure, current, seconds, motion.tap)
    }
}

enum class StretchCueKind { Start, Half, Next, Finish }

data class StretchCue(val kind: StretchCueKind, val step: Int, val text: String) {
    val interrupts: Boolean get() = kind == StretchCueKind.Start || kind == StretchCueKind.Finish
}

data class StretchPause(val start: Instant, val end: Instant)

data class StretchWorkout(
    val kind: StretchWorkoutKind,
    val routine: String,
    val start: Instant,
    val end: Instant,
    val pauses: List<StretchPause>,
    val seconds: Int,
)

class StretchRun(val routine: StretchRoutine, val startedAt: Instant) {
    companion object {
        const val HALF_MINIMUM = 20
        const val NEXT_MINIMUM = 15
        const val NEXT_LEAD = 3.0
        const val LOG_MINIMUM = 30.0
        const val BACK_RESTART = 3.0

        fun startText(step: StretchStep): String = if (step.say.isEmpty()) "${step.title}." else "${step.title}. ${step.say}"

        fun nextText(step: StretchStep): String = "Next: ${step.title}"

        fun finishText(routine: StretchRoutine): String = "Well done. That is your ${routine.title} finished."
    }

    var index = 0
        private set
    var stepElapsed = 0.0
        private set
    var activeSeconds = 0.0
        private set
    var paused = false
        private set
    private var pausedAt: Instant? = null
    private val pauseList = ArrayList<StretchPause>()
    var finished = false
        private set
    var completed = false
        private set
    var endedAt: Instant? = null
        private set
    private var halfSpoken = false
    private var nextSpoken = false

    val pauses: List<StretchPause> get() = pauseList.toList()
    val steps: List<StretchStep> get() = routine.steps
    val step: StretchStep? get() = steps.getOrNull(index)
    val nextStep: StretchStep? get() = steps.getOrNull(index + 1)
    val stepsDone: Int get() = if (completed) steps.size else min(index, steps.size)
    val stepRemaining: Int get() = max(0, ceil((step?.seconds ?: 0) - stepElapsed).toInt())

    val elapsedTotal: Double get() = steps.take(index).sumOf { it.seconds }.toDouble() + stepElapsed

    val progress: Double
        get() {
            if (routine.seconds <= 0) return 0.0
            return if (completed) 1.0 else min(1.0, elapsedTotal / routine.seconds)
        }

    val shouldLog: Boolean get() = finished && (completed || activeSeconds >= LOG_MINIMUM)
    val loggedSeconds: Int get() = min(14400, max(1, activeSeconds.roundToInt()))

    fun begin(): List<StretchCue> {
        val current = step ?: return finish(startedAt, true)
        return listOf(StretchCue(StretchCueKind.Start, index, startText(current)))
    }

    fun advance(seconds: Double, now: Instant): List<StretchCue> {
        if (finished || paused || seconds <= 0) return emptyList()
        val cues = ArrayList<StretchCue>()
        var left = seconds
        while (left > 0 && !finished) {
            val current = step ?: break
            val length = current.seconds.toDouble()
            val used = min(left, max(0.0, length - stepElapsed))
            val before = stepElapsed
            stepElapsed += used
            activeSeconds += used
            left -= used
            cues += crossings(current, before, stepElapsed)
            if (stepElapsed >= length) cues += moveOn(now)
        }
        return cues
    }

    private fun crossings(current: StretchStep, before: Double, after: Double): List<StretchCue> {
        val cues = ArrayList<StretchCue>()
        val length = current.seconds.toDouble()
        if (!halfSpoken && current.seconds >= HALF_MINIMUM && current.half.isNotEmpty() && before < length / 2 && after >= length / 2) {
            halfSpoken = true
            cues += StretchCue(StretchCueKind.Half, index, current.half)
        }
        val next = nextStep
        if (!nextSpoken && current.seconds >= NEXT_MINIMUM && next != null && before < length - NEXT_LEAD && after >= length - NEXT_LEAD) {
            nextSpoken = true
            cues += StretchCue(StretchCueKind.Next, index, nextText(next))
        }
        return cues
    }

    private fun moveOn(now: Instant): List<StretchCue> {
        if (index + 1 >= steps.size) {
            index = steps.size
            return finish(now, true)
        }
        return go(index + 1)
    }

    private fun go(target: Int): List<StretchCue> {
        index = max(0, min(steps.size - 1, target))
        stepElapsed = 0.0
        halfSpoken = false
        nextSpoken = false
        val current = step ?: return emptyList()
        return listOf(StretchCue(StretchCueKind.Start, index, startText(current)))
    }

    private fun finish(at: Instant, completed: Boolean): List<StretchCue> {
        closePause(at)
        finished = true
        this.completed = completed
        endedAt = at
        stepElapsed = 0.0
        return if (completed) listOf(StretchCue(StretchCueKind.Finish, steps.size, finishText(routine))) else emptyList()
    }

    private fun closePause(at: Instant) {
        val start = pausedAt
        if (start != null) pauseList.add(StretchPause(start, if (at.isBefore(start)) start else at))
        pausedAt = null
        paused = false
    }

    fun pause(at: Instant) {
        if (finished || paused) return
        paused = true
        pausedAt = at
    }

    fun resume(at: Instant) {
        if (finished || !paused) return
        closePause(at)
    }

    fun skip(now: Instant): List<StretchCue> {
        if (finished) return emptyList()
        return moveOn(now)
    }

    fun back(): List<StretchCue> {
        if (finished || steps.isEmpty()) return emptyList()
        if (stepElapsed >= BACK_RESTART || index == 0) return go(index)
        return go(index - 1)
    }

    fun stop(at: Instant) {
        if (finished) return
        finish(at, false)
    }

    fun logBody(day: String, health: Boolean, source: String = "android"): HealthJson = linkedMapOf(
        "routine" to routine.id,
        "seconds" to loggedSeconds.toLong(),
        "steps" to stepsDone.toLong(),
        "stepsTotal" to steps.size.toLong(),
        "completed" to completed,
        "at" to startedAt.toEpochMilli(),
        "day" to day,
        "source" to source,
        "health" to health,
    )

    val workout: StretchWorkout
        get() {
            val end = endedAt ?: startedAt
            return StretchWorkout(routine.workoutKind, routine.id, startedAt, if (end.isBefore(startedAt)) startedAt else end, pauses, loggedSeconds)
        }
}

interface StretchSpeaker {
    fun say(text: String, interrupt: Boolean)
    fun pause()
    fun resume()
    fun stop()
}

data class StretchStat(val label: String, val value: String)

data class StretchRecentRow(
    val id: String,
    val title: String,
    val detail: String,
    val whenText: String,
    val completed: Boolean,
    val health: Boolean,
)

class StretchModel(
    private val call: HealthCall,
    private val prefs: PrefStore? = null,
    val zone: java.time.ZoneId = java.time.ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) {
    companion object {
        const val TITLE = "Stretch Break"
        const val INSTALL_TITLE = "Take a stretch break"
        const val INSTALL_MESSAGE =
            "Stretch Break talks you through a short stretch for your back and posture after a long day at the computer, and a gentle lymph tapping routine, while an example person does each move with you. Sessions are private to your ForkMesh account and show here, on the web, and on your desktop."
        const val INSTALL_BUTTON = "Install Stretch Break"
        const val UNAVAILABLE_TITLE = "Stretch Break is unavailable"
        const val DISCLAIMER = "General wellness guidance, not medical advice. Move gently and stop if anything hurts."
    }

    @Volatile var speaker: StretchSpeaker? = null
    @Volatile var lastTick: Instant? = null
        private set
    private var pendingCue: StretchCue? = null

    var overview: StretchOverview? = null
        private set
    var library: StretchLibrary? = null
        private set
    var installed = false
        private set
    var loading = true
        private set
    var error = ""
        private set
    var busy = false
        private set
    var voice = true
        private set
    @Volatile var run: StretchRun? = null
        private set
    var logged: StretchSessionRecord? = null
        private set
    var logNote = ""
        private set
    var selectedRoutine = "desk"
    var notice: HealthNotice? = null

    val today: String get() = HealthDates.isoDay(now(), zone)

    fun load() {
        loading = true
        error = ""
        try {
            fetch()
            loading = false
        } catch (failure: OrgApiException) {
            loading = false
            if (failure.code == "plugin_not_installed") {
                installed = false
                overview = null
            } else {
                error = message(failure)
            }
        } catch (failure: Exception) {
            loading = false
            error = message(failure)
        }
    }

    private fun fetch() {
        val first = overview == null
        val payload = call.call("GET", "?day=$today", null)
        val decoded = StretchOverview.fromJson(payload)
        installed = payload["installed"] == true
        val raw = payload["library"] as? Map<*, *>
        if (raw != null) {
            @Suppress("UNCHECKED_CAST")
            library = StretchLibrary.fromJson(raw as HealthJson)
        }
        overview = if (installed) decoded else null
        if (installed) {
            voice = decoded.profile.voice
            if (first || routine(selectedRoutine) == null) selectedRoutine = decoded.profile.routine
        }
    }

    fun message(failure: Throwable): String {
        val api = failure as? OrgApiException ?: return "Stretch Break is unavailable right now."
        return when (api.code) {
            "invalid_routine" -> "Stretch Break did not recognise that routine."
            "invalid_seconds" -> "That session was too short to log."
            "day_full" -> "Today already holds as many sessions as it can."
            "session_not_found" -> "That session was already removed."
            else -> api.message
        }
    }

    private fun show(text: String, isError: Boolean = false) {
        notice = HealthNotice(text, isError)
    }

    val showsSpinner: Boolean get() = loading && overview == null && error.isEmpty()
    val showsError: Boolean get() = error.isNotEmpty() && overview == null
    val showsInstall: Boolean get() = !loading && error.isEmpty() && !installed
    val inSession: Boolean get() = run != null

    val routines: List<StretchRoutine> get() = library?.routines ?: emptyList()
    fun routine(id: String): StretchRoutine? = library?.routine(id)
    val chosenRoutine: StretchRoutine? get() = routine(selectedRoutine) ?: routines.firstOrNull()

    fun install() {
        busy = true
        try {
            call.call(
                "POST", "/install",
                linkedMapOf(
                    "voice" to true, "routine" to selectedRoutine,
                    "tzOffsetMinutes" to HealthClock.tzOffsetMinutes(zone, now()).toLong(),
                ),
            )
            show("Stretch Break installed.")
            load()
        } catch (failure: Exception) {
            show(message(failure), true)
        } finally {
            busy = false
        }
    }

    fun uninstall(): Boolean {
        busy = true
        return try {
            call.call("POST", "/uninstall", emptyMap())
            prefs?.putBool(HealthPrefs.STRETCH, false)
            show("Stretch Break uninstalled from your account.")
            load()
            true
        } catch (failure: Exception) {
            show(message(failure), true)
            false
        } finally {
            busy = false
        }
    }

    fun setVoice(on: Boolean) {
        voice = on
        if (!on) speaker?.stop()
        try {
            call.call("PUT", "/profile", linkedMapOf("voice" to on))
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun start() {
        val routine = chosenRoutine
        if (routine == null || routine.steps.isEmpty()) {
            show("That routine has no steps yet.", true)
            return
        }
        logged = null
        logNote = ""
        pendingCue = null
        val fresh = StretchRun(routine, now())
        val cues = fresh.begin()
        run = fresh
        lastTick = now()
        speak(cues)
        if (overview?.profile?.routine != routine.id) {
            try {
                call.call("PUT", "/profile", linkedMapOf("routine" to routine.id))
            } catch (_: Exception) {
            }
        }
    }

    fun tick() {
        val current = now()
        val last = lastTick ?: current
        val dt = min(1.0, max(0.0, (current.toEpochMilli() - last.toEpochMilli()) / 1000.0))
        tick(dt)
    }

    fun tick(seconds: Double) {
        lastTick = now()
        val current = run ?: return
        if (current.finished || current.paused) return
        val cues = current.advance(seconds, now())
        speak(cues)
        if (current.finished) log()
    }

    fun figureSeconds(at: Instant): Double {
        val current = run ?: return 0.0
        if (current.finished) return 0.0
        val last = lastTick
        if (current.paused || last == null) return current.stepElapsed
        val extra = min(1.0, max(0.0, (at.toEpochMilli() - last.toEpochMilli()) / 1000.0))
        return min((current.step?.seconds ?: 0).toDouble(), current.stepElapsed + extra)
    }

    fun pause() {
        val current = run ?: return
        if (current.finished || current.paused) return
        current.pause(now())
        speaker?.pause()
    }

    fun resume() {
        val current = run ?: return
        if (!current.paused) return
        current.resume(now())
        lastTick = now()
        if (!voice) return
        val cue = pendingCue
        if (cue != null) {
            pendingCue = null
            speaker?.say(cue.text, true)
        } else {
            speaker?.resume()
        }
    }

    fun togglePause() {
        if (run?.paused == true) resume() else pause()
    }

    fun skip() {
        val current = run ?: return
        if (current.finished) return
        val cues = current.skip(now())
        lastTick = now()
        speak(cues)
        if (current.finished) log()
    }

    fun back() {
        val current = run ?: return
        if (current.finished) return
        val cues = current.back()
        lastTick = now()
        speak(cues)
    }

    fun stop() {
        val current = run
        if (current == null || current.finished) {
            close()
            return
        }
        current.stop(now())
        speaker?.stop()
        log()
    }

    fun close() {
        if (run?.finished == false) speaker?.stop()
        run = null
        lastTick = null
        pendingCue = null
    }

    private fun speak(cues: List<StretchCue>) {
        val voiceOut = speaker
        if (!voice || voiceOut == null) return
        if (run?.paused == true) {
            val cue = cues.lastOrNull { it.interrupts }
            if (cue != null) {
                pendingCue = cue
                voiceOut.stop()
            }
            return
        }
        pendingCue = null
        for (cue in cues) voiceOut.say(cue.text, cue.interrupts)
    }

    fun log() {
        val current = run ?: return
        if (!current.finished) return
        if (!current.shouldLog) {
            logNote = "Not logged: a session counts once 30 seconds are done."
            return
        }
        busy = true
        try {
            val reply = call.call("POST", "/sessions", current.logBody(today, false))
            val raw = reply["session"] as? Map<*, *>
            if (raw != null) {
                @Suppress("UNCHECKED_CAST")
                logged = StretchSessionRecord.fromJson(raw as HealthJson)
            }
            logNote = "Logged on your account."
            try {
                fetch()
            } catch (_: Exception) {
            }
        } catch (failure: Exception) {
            logNote = "Not logged."
            show(message(failure), true)
        } finally {
            busy = false
        }
    }

    fun deleteSession(id: String) {
        if (id.isEmpty()) return
        try {
            call.call("DELETE", "/sessions/$id", null)
            show("Session removed.")
            try {
                fetch()
            } catch (_: Exception) {
            }
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    val profile: StretchProfile get() = overview?.profile ?: StretchProfile()
    val summary: StretchSummary get() = overview?.summary ?: StretchSummary()
    val todayTotal: StretchDayTotal get() = overview?.today ?: StretchDayTotal()
    val days: List<StretchDayTotal> get() = overview?.days ?: emptyList()

    val stats: List<StretchStat>
        get() = listOf(
            StretchStat("Today", StretchText.sessions(todayTotal.sessions, todayTotal.seconds)),
            StretchStat("Streak", StretchText.days(summary.streak)),
            StretchStat("Last 7 days", StretchText.sessions(summary.weekSessions, summary.weekSeconds)),
            StretchStat("Best streak", StretchText.days(summary.bestStreak)),
            StretchStat("All sessions", summary.sessions.toString()),
        )

    val recent: List<StretchRecentRow>
        get() = (overview?.recent ?: emptyList()).map { session ->
            val title = routine(session.routine)?.title ?: session.routine
            val parts = arrayListOf(StretchText.duration(session.seconds))
            parts.add(if (session.completed) "completed" else "stopped early")
            parts.add(StretchText.source(session.source))
            if (session.health) parts.add("Apple Health")
            var whenText = HealthClock.dayLabel(session.day)
            val at = if (session.at > 0) session.at else session.loggedAt
            if (at > 0) whenText += " " + HealthClock.time(Instant.ofEpochMilli(at), zone)
            StretchRecentRow(session.id, title, parts.joinToString(" · "), whenText, session.completed, session.health)
        }
}

object StretchText {
    fun duration(seconds: Int): String {
        val total = max(0, seconds)
        val minutes = total / 60
        val rest = total % 60
        if (minutes == 0) return "$rest s"
        return if (rest == 0) "$minutes min" else "$minutes min $rest s"
    }

    fun countdown(seconds: Int): String {
        val total = max(0, seconds)
        return "${total / 60}:${(total % 60).toString().padStart(2, '0')}"
    }

    fun days(count: Int): String = if (count == 1) "1 day" else "$count days"

    fun sessions(count: Int, seconds: Int): String {
        val label = if (count == 1) "1 session" else "$count sessions"
        return if (count == 0) label else "$label · ${duration(seconds)}"
    }

    fun source(value: String): String = when (value) {
        "ios" -> "iPhone"
        "android" -> "Android"
        "desktop" -> "desktop"
        else -> "web"
    }
}
