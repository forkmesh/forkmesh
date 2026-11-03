package com.forkmesh.nativeapp.silentspeech

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sin

object SilentSynth {
    fun pattern(word: String, p: Double, amp: Double): SilentVector {
        val one = amp * sin(2 * PI * p)
        val two = amp * sin(4 * PI * p)
        return when (word) {
            "yes" -> SilentVector(one, 0.0, 0.0)
            "no" -> SilentVector(0.0, one, 0.0)
            "help" -> SilentVector(two, 0.0, 0.0)
            else -> SilentVector(0.0, 0.0, one)
        }
    }

    fun take(word: String, random: SilentRandom, rate: Double = 25.0, acceleration: Boolean = true, noise: Double = 0.05): List<SilentSample> {
        val duration = 0.8 + 0.3 * random.unit()
        val amp = 1.5 + 0.5 * random.unit()
        val lead = 0.3
        val count = ((2 * lead + duration) * rate).roundToInt()
        var attitude = SilentQuaternion.IDENTITY
        val samples = ArrayList<SilentSample>()
        for (k in 0 until count) {
            val t = k / rate
            val p = (t - lead) / duration
            val base = if (p in 0.0..1.0) pattern(word, p, amp) else SilentVector.ZERO
            val r = SilentVector(base.x + noise * (random.unit() - 0.5), base.y + noise * (random.unit() - 0.5), base.z + noise * (random.unit() - 0.5))
            attitude = (attitude * SilentQuaternion.fromRotationVector(SilentVector(r.x / rate, r.y / rate, r.z / rate))).normalized
            val accel = if (acceleration) SilentVector(0.02 * r.x, 0.02 * r.y, 0.02 * r.z) else null
            samples.add(SilentSample(t, r, attitude, accel))
        }
        return samples
    }

    fun dataset(words: List<String>, takes: Int, seed: Long, acceleration: Boolean = true): SilentDataset {
        val random = SilentRandom(seed)
        var dataset = SilentDataset(words.map { SilentWord(it) })
        for (n in 0 until takes) for (word in words) {
            dataset = dataset.addTake(SilentTake("$word-$n", word, take(word, random, acceleration = acceleration)))
        }
        return dataset
    }
}

class SilentSpeechTest {
    private fun near(expected: Double, actual: Double, accuracy: Double = 1e-9) =
        expect(abs(expected - actual) <= accuracy, "expected $expected, got $actual")

    fun testRandomMatchesSplitMix64() {
        val random = SilentRandom(SilentEvaluator.SEED)
        assertEq("99a4143d34585f45", java.lang.Long.toHexString(random.next()))
        assertEq("fc18d87fcc9ca7a3", java.lang.Long.toHexString(random.next()))
        assertEq("7220ff9660d13a72", java.lang.Long.toHexString(random.next()))
    }

    fun testQuaternionRoundTripsARotationVector() {
        val v = SilentVector(0.3, -0.2, 0.9)
        val back = SilentQuaternion.fromRotationVector(v).rotationVector
        near(v.x, back.x)
        near(v.y, back.y)
        near(v.z, back.z)
        near(0.0, (SilentQuaternion.fromRotationVector(v).conjugate * SilentQuaternion.fromRotationVector(v)).rotationVector.length)
    }

    fun testResamplesToTwentyFiveHertz() {
        val fast = SilentSynth.take("yes", SilentRandom(1), rate = 100.0)
        val resampled = SilentFeatures.resampled(fast)
        assertEq(((fast.last().t - fast.first().t) * 25).toInt() + 1, resampled.size)
        near(0.04, resampled[1].t - resampled[0].t)
    }

    fun testTrimsTheQuietLeadInAndLeadOut() {
        val samples = SilentSynth.take("no", SilentRandom(2), noise = 0.01)
        val trimmed = SilentFeatures.trimmed(samples)
        expect(trimmed.size < samples.size - 10)
        expect(trimmed.first().t > 0.25)
    }

    fun testFramesHaveThreeValuesPerChannelAndAreCentred() {
        val samples = SilentSynth.take("help", SilentRandom(3))
        assertEq(9, SilentFeatures.frames(samples, SilentChannel.DEFAULTS).first().size)
        val frames = SilentFeatures.frames(samples, setOf(SilentChannel.ROTATION))
        assertEq(3, frames.first().size)
        near(0.0, frames.sumOf { it[0] } / frames.size)
    }

    fun testDTWIsZeroForItselfSymmetricAndForgivesTempo() {
        val random = SilentRandom(4)
        val rotation = setOf(SilentChannel.ROTATION)
        val a = SilentFeatures.frames(SilentSynth.take("yes", random), rotation)
        val slow = SilentFeatures.frames(SilentSynth.take("yes", random), rotation)
        val other = SilentFeatures.frames(SilentSynth.take("no", random), rotation)
        near(0.0, SilentDTW.distance(a, a), 1e-12)
        near(SilentDTW.distance(a, other), SilentDTW.distance(other, a), 1e-12)
        expect(SilentDTW.distance(a, slow) < SilentDTW.distance(a, other))
        assertEq(Double.POSITIVE_INFINITY, SilentDTW.distance(a, other, cutoff = 1e-6))
        assertEq(Double.POSITIVE_INFINITY, SilentDTW.distance(emptyList(), a))
    }

    fun testModelRecognisesEachWordAndRejectsFarOnes() {
        val words = listOf("yes", "no", "help", "stop")
        val model = SilentModel(SilentSynth.dataset(words, 3, 10).takes, SilentChannel.DEFAULTS)
        val random = SilentRandom(99)
        for (word in words) {
            val match = model.match(SilentSynth.take(word, random), null, 0.0)
            assertEq(word, match?.word)
            assertEq(true, match?.accepted)
        }
        val strict = model.match(SilentSynth.take("yes", random), 1e-6, 0.0)
        assertEq(false, strict?.accepted)
        expect(strict?.summary?.startsWith("Not sure") == true)
    }

    fun testWorksWithoutAccelerationLikeAndroidHeadTrackers() {
        val model = SilentModel(SilentSynth.dataset(listOf("yes", "no"), 3, 11, acceleration = false).takes, SilentChannel.DEFAULTS)
        assertEq("no", model.match(SilentSynth.take("no", SilentRandom(12), acceleration = false), null, 0.0)?.word)
    }

    fun testEvaluationFindsSeparableWordsSignificant() {
        val dataset = SilentSynth.dataset(listOf("yes", "no", "help", "stop"), 4, 42)
        val result = SilentEvaluator.evaluate(dataset, SilentChannel.DEFAULTS, permutations = 200)!!
        assertEq(16, result.evaluated)
        assertEq(16, result.correct)
        near(1.0 / 201.0, result.pValue, 1e-12)
        expect(result.significant)
        assertEq(listOf(4, 4, 4, 4), result.confusion.map { it.sum() })
        expect(result.suggestedMaxDistance != null)
        expect(result.nullMean < 0.5)
        assertEq("Right 16 of 16 (100%), chance is about 25%", result.headline)
        expect(result.verdict.startsWith("p = 0.005"), result.verdict)
    }

    fun testEvaluationOfIdenticalMotionIsNotSignificant() {
        val random = SilentRandom(7)
        var dataset = SilentDataset(listOf(SilentWord("a"), SilentWord("b"), SilentWord("c")))
        for (n in 0 until 12) {
            dataset = dataset.addTake(SilentTake("$n", listOf("a", "b", "c")[n % 3], SilentSynth.take("yes", random, noise = 0.4)))
        }
        val result = SilentEvaluator.evaluate(dataset, SilentChannel.DEFAULTS, permutations = 200)!!
        expect(!result.significant)
        expect(result.verdict.contains("not yet better than chance"))
    }

    fun testEvaluationNeedsTwoWordsWithTwoTakes() {
        var dataset = SilentSynth.dataset(listOf("yes", "no"), 1, 5)
        expect(SilentEvaluator.requirement(dataset) != null)
        expect(SilentEvaluator.evaluate(dataset, SilentChannel.DEFAULTS) == null)
        val random = SilentRandom(6)
        dataset = dataset.addTake(SilentTake("y", "yes", SilentSynth.take("yes", random)))
        dataset = dataset.addTake(SilentTake("n", "no", SilentSynth.take("no", random)))
        assertEq(null, SilentEvaluator.requirement(dataset))
    }

    fun testLeaveOneOutUsesTheMatrixAndSkipsSingleTakeWords() {
        val d = arrayOf(doubleArrayOf(0.0, 1.0, 5.0, 5.0), doubleArrayOf(1.0, 0.0, 5.0, 5.0), doubleArrayOf(5.0, 5.0, 0.0, 2.0), doubleArrayOf(5.0, 5.0, 2.0, 0.0))
        val results = SilentEvaluator.leaveOneOut(d, intArrayOf(0, 0, 1, 1), 2, 1)
        assertEq(listOf(0, 0, 1, 1), results.map { it.guess })
        assertEq(listOf(1.0, 1.0, 2.0, 2.0), results.map { it.distance })
        assertEq(3, SilentEvaluator.leaveOneOut(d, intArrayOf(0, 1, 1, 1), 2, 1).size)
    }

    fun testCrossPlatformParity() {
        val dataset = SilentSynth.dataset(listOf("yes", "no", "help"), 3, 2026, acceleration = false)
        val channels = setOf(SilentChannel.ROTATION, SilentChannel.ATTITUDE)
        val result = SilentEvaluator.evaluate(dataset, channels, permutations = 100)!!
        assertEq(9, result.evaluated)
        assertEq(9, result.correct)
        near(1.0 / 101.0, result.pValue, 1e-12)
        val frames = SilentFeatures.frames(dataset.takes[0].samples, channels)
        assertEq(26, frames.size)
        near(0.866728, SilentDTW.distance(frames, SilentFeatures.frames(dataset.takes[1].samples, channels)), 1e-5)
    }

    fun testSegmenterFindsAMouthedWordWithPreRoll() {
        val random = SilentRandom(8)
        val noise = SilentNoiseFloor.calibrate(List(50) { 0.02 + 0.01 * random.unit() })!!
        expect(noise.on > noise.off)
        val segmenter = SilentSegmenter(noise)
        val found = ArrayList<List<SilentSample>>()
        val word = SilentSynth.take("yes", random, noise = 0.01)
        for (sample in word) segmenter.push(sample)?.let { found.add(it) }
        val tail = word.last().t
        for (k in 1..20) segmenter.push(SilentSample(tail + k / 25.0, SilentVector.ZERO, SilentQuaternion.IDENTITY))?.let { found.add(it) }
        assertEq(1, found.size)
        val segment = found.first()
        expect(segment.first().t < 0.3)
        expect(segment.last().t - segment.first().t > 0.6)
        expect(!segmenter.active)
    }

    fun testSegmenterIgnoresAShortBump() {
        val segmenter = SilentSegmenter(SilentNoiseFloor(0.02, 0.005))
        var found = 0
        for (k in 0 until 40) {
            val bump = if (k in 10..12) 1.0 else 0.0
            if (segmenter.push(SilentSample(k / 25.0, SilentVector(bump, 0.0, 0.0), SilentQuaternion.IDENTITY)) != null) found++
        }
        assertEq(0, found)
    }

    fun testDatasetCleansWordsAndRoundTripsJSON() {
        var dataset = SilentDataset.STARTER
        assertEq(null, dataset.addWord(" YES "))
        dataset = dataset.addWord(" Thirsty ", "Could I have some water, please? ")!!
        assertEq("Could I have some water, please?", dataset.phrase("thirsty"))
        assertEq("yes", dataset.phrase("yes"))
        val random = SilentRandom(9)
        dataset = dataset.addTake(SilentTake("t1", "thirsty", SilentSynth.take("yes", random)))
        dataset = dataset.addTake(SilentTake("t2", "yes", SilentSynth.take("yes", random, acceleration = false)))
        dataset = dataset.addTake(SilentTake("t3", "unknown", SilentSynth.take("yes", random)))
        assertEq(2, dataset.takes.size)
        val back = SilentDataset.decode(dataset.encoded())!!
        assertEq(dataset.words, back.words)
        assertEq(listOf("t1", "t2"), back.takes.map { it.id })
        expect(back.takes[0].samples[0].acceleration != null)
        assertEq(null, back.takes[1].samples[0].acceleration)
        assertEq(dataset.takes[0].samples.size, back.takes[0].samples.size)
        near(dataset.takes[0].samples[5].rotationRate.x, back.takes[0].samples[5].rotationRate.x, 1e-5)
        dataset = dataset.removeWord("thirsty")
        assertEq(listOf("t2"), dataset.takes.map { it.id })
        assertEq(0, dataset.removeLastTake("yes").count("yes"))
        assertEq(null, SilentDataset.decode("{\"version\":99,\"words\":[]}"))
    }

    fun testReadsADatasetWrittenByTheIPhoneApp() {
        val text = """{"takes":[{"id":"a","samples":[[0,0.1,0,0,1,0,0,0,0.01,0,0],[0.04,0.2,0,0,1,0,0,0,0.02,0,0]],"word":"yes"}],"version":1,"words":[{"phrase":"","word":"yes"}]}"""
        val dataset = SilentDataset.decode(text)!!
        assertEq(1, dataset.count("yes"))
        near(0.2, dataset.takes[0].samples[1].rotationRate.x)
        near(0.02, dataset.takes[0].samples[1].acceleration!!.x)
    }

    fun testTranscriptUndoAndClear() {
        val transcript = SilentTranscript().append("help").append("water")
        assertEq("help water", transcript.text)
        assertEq("help", transcript.undo().text)
        assertEq("", transcript.clear().text)
    }
}
