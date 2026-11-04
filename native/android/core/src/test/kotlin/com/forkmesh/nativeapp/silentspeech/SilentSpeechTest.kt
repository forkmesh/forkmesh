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

    fun testTextSplitsSentencesIntoWords() {
        assertEq(listOf("could", "you", "please", "help", "me", "it's", "late"), SilentText.words("  Could you, PLEASE — help me? It’s late. "))
        assertEq(listOf("quoted", "42"), SilentText.words("'quoted' 42"))
        assertEq(emptyList<String>(), SilentText.words("..."))
        val vocabulary = SilentText.SENTENCES.flatMap(SilentText::words).toSet()
        expect(vocabulary.size > 100, "over a hundred words")
        expect(vocabulary.containsAll(listOf("yes", "no", "help", "stop", "water")), "starter words are in the sentences")
    }

    fun testReadingScriptCyclesSentencesWithoutRepeatsAndMatchesTheIPhone() {
        val sentences = (0 until 4).map { listOf("s$it", "w") }
        val script = SilentReading.script(sentences, 7, count = 20)
        assertEq(20, script.size)
        assertEq(setOf(5), sentences.map { s -> script.count { it == s } }.toSet())
        expect(script.zipWithNext().none { it.first == it.second }, "no sentence twice in a row")
        assertEq(listOf("s1", "s2", "s0", "s3", "s0"), script.take(5).map { it[0] })
        assertEq(emptyList<List<String>>(), SilentReading.script(emptyList(), 1))
    }

    fun testReadingPlanPrefersYourSentencesAndWordsWithFewTakes() {
        var dataset = SilentDataset.STARTER.addSentence("Pass the salt, please")!!
        assertEq(null, dataset.addSentence("pass the SALT please"))
        dataset = dataset.addEntry("zebra")!!.first
        val pool = SilentReading.pool(dataset)
        assertEq(listOf("pass", "the", "salt", "please"), pool.first())
        assertEq(listOf("zebra"), pool.last())
        assertEq(SilentText.SENTENCES.size + 2, pool.size)
        val plan = SilentReading.plan(dataset, 3)
        assertEq(3, plan.size)
        assertEq(listOf("pass", "the", "salt", "please"), plan[0])
        val random = SilentRandom(1)
        for (n in 0 until 10) for (word in listOf("pass", "the", "salt", "please")) {
            dataset = dataset.learn(SilentTake("$word$n", word, SilentSynth.take("yes", random)))
        }
        expect(listOf("pass", "the", "salt", "please") !in SilentReading.plan(dataset, 3), "trained sentence moves back")
        val reading = SilentReading.start(plan, 5)
        assertEq(SilentReading.SCRIPT_LENGTH, reading.sentences.size)
        expect(reading.sentences.all { it in plan }, "script uses the plan")
    }

    fun testDatasetLearnsNewWordsFromSentencesAndEntries() {
        val random = SilentRandom(2)
        var dataset = SilentDataset(emptyList()).learn(SilentTake("a", "coffee", SilentSynth.take("yes", random)))
        assertEq(listOf("coffee"), dataset.words.map { it.word })
        assertEq(mapOf("coffee" to 1), dataset.counts())
        val (withPhrase, word) = dataset.addEntry("thirsty: Could I have some water?")!!
        assertEq("thirsty", word)
        dataset = withPhrase
        assertEq("Could I have some water?", dataset.phrase("thirsty"))
        val (withSentence, first) = dataset.addEntry("See you later")!!
        assertEq("see", first)
        dataset = withSentence
        assertEq(listOf("See you later"), dataset.sentences)
        assertEq(null, dataset.addEntry("coffee"))
        val back = SilentDataset.decode(dataset.encoded())!!
        assertEq(listOf("See you later"), back.sentences)
        assertEq(listOf("coffee", "thirsty", "see", "you", "later"), back.words.map { it.word })
        assertEq(emptyList<String>(), dataset.removeSentence("See you later").sentences)
    }

    fun testLanguageModelPrefersWordsThatFollow() {
        val language = SilentLanguage(listOf(listOf("i", "would", "like", "water"), listOf("i", "would", "like", "coffee"), listOf("i", "need", "help")))
        expect(language.probability("would", "i", 10) > language.probability("help", "i", 10), "would follows i")
        expect(language.probability("i", null, 10) > language.probability("water", null, 10), "i starts sentences")
        expect(language.probability("unknown", "i", 10) > 0, "unseen words keep a chance")
        val close = listOf("help" to 1.0, "would" to 1.02, "water" to 2.0, "coffee" to 2.2)
        assertEq("would", SilentScores.ranked(close, language, "i").first().word)
        assertEq("help", SilentScores.ranked(close, null, "i").first().word)
        val far = listOf("help" to 0.5, "would" to 2.0, "water" to 2.1, "coffee" to 2.2)
        assertEq("help", SilentScores.ranked(far, language, "i").first().word)
    }

    fun testEvaluationSamplesTheNewestTakesOfEachWord() {
        val dataset = SilentSynth.dataset(listOf("yes", "no"), 5, 3)
        assertEq(listOf("no-2", "yes-3", "no-3", "yes-4", "no-4"), SilentEvaluator.sample(dataset.takes, 3, 5).map { it.id })
        assertEq(500, SilentEvaluator.rounds(500, 40))
        assertEq(111, SilentEvaluator.rounds(500, 600))
        assertEq(100, SilentEvaluator.rounds(500, 2000))
        assertEq(50, SilentEvaluator.rounds(50, 2000))
    }

    fun testEvaluationListsTheWorstMixups() {
        val result = SilentEvaluation(listOf("a", "b", "c"), 9, 4, 0.3, 0.2, 10, listOf(listOf(1, 2, 0), listOf(3, 1, 0), listOf(0, 1, 2)), null)
        val mixups = result.mixups(2)
        assertEq(listOf("b", "a"), mixups.map { it.first })
        assertEq(listOf("a", "b"), mixups.map { it.second })
        assertEq(listOf(3, 2), mixups.map { it.third })
    }

    fun testScopeKeepsAWindowAndShowsDegrees() {
        var scope = SilentScope()
        var attitude = SilentQuaternion.IDENTITY
        for (k in 0 until 250) {
            val spin = SilentVector(1.0, 0.0, if (k < 25) 0.5 else 0.0)
            attitude = (attitude * SilentQuaternion.fromRotationVector(SilentVector(0.0, 0.0, spin.z / 25))).normalized
            scope = scope.push(SilentSample(k / 25.0, spin, attitude, SilentVector(0.0, 0.0, 0.1)))
        }
        expect(scope.samples.last().t - scope.samples.first().t <= SilentScope.WINDOW, "six seconds kept")
        val last = scope.frames.last()
        near(57.29578, last.rotation.x, 1e-4)
        near(0.48 * 180 / PI, last.angle.z, 1e-6)
        near(0.1, last.acceleration!!.z)
        scope = scope.recentre()
        near(0.0, scope.frames.last().angle.z)
        scope = scope.mark(scope.samples.takeLast(10), "yes")
        assertEq(listOf("yes"), scope.marks.map { it.label })
        for (k in 250 until 500) scope = scope.push(SilentSample(k / 25.0, SilentVector.ZERO, attitude))
        expect(scope.marks.isEmpty(), "old marks drop out")
    }

    fun testSpectrumFindsTheNodFrequency() {
        val values = (0 until 100).map { SilentVector(0.0, sin(2 * PI * 3.125 * it / 25), 0.0) }
        val spectrum = SilentScope.spectrum(values)
        assertEq(33, spectrum.power.size)
        near(25.0 / 64, spectrum.binHz)
        near(3.125, spectrum.peakHz!!)
        near(3.875669, spectrum.power[8], 1e-5)
        assertEq(null, SilentScope.spectrum(values.take(10)).peakHz)
        assertEq(null, SilentScope.spectrum(List(64) { SilentVector(1.0, 1.0, 1.0) }).peakHz)
    }

    fun testReadingLabelsEachHeardWordInOrder() {
        var reading = SilentReading.start(listOf(listOf("yes", "no", "help", "stop")), 3, duration = 10.0)
        val first = reading.sentence
        val random = SilentRandom(4)
        var labelled = emptyList<Pair<String, List<SilentSample>>>()
        for (word in first) {
            expect(labelled.isEmpty(), "labelled only at the end of the sentence")
            val (next, out) = reading.hear(SilentSynth.take(word, random))
            reading = next
            labelled = out
        }
        assertEq(first, labelled.map { it.first })
        assertEq(1, reading.sentencesRead)
        assertEq(4, reading.collected)
        assertEq(1, reading.index)
        assertEq(0, reading.position)
        assertEq("Saved 4 takes from 1 sentence.", reading.summary)
        reading = reading.hear(SilentSynth.take("yes", random)).first
        assertEq(1, reading.position)
        reading = reading.again()
        assertEq(0, reading.position)
        assertEq(1, reading.index)
        reading = reading.skip()
        assertEq(2, reading.index)
        reading = reading.forget(4)
        assertEq(0, reading.collected)
        assertEq(0, reading.sentencesRead)
    }

    fun testReadingRunsForItsDuration() {
        var reading = SilentReading.start(listOf(listOf("yes", "no")), 1, duration = 300.0)
        reading = reading.tick(1000.0).tick(1072.6)
        assertEq("1:12 of 5:00", reading.clock)
        expect(!reading.timeUp, "not yet")
        reading = reading.tick(1300.0)
        expect(reading.done, "done after five minutes")
        reading = reading.hear(listOf(SilentSample(0.0, SilentVector.ZERO, SilentQuaternion.IDENTITY))).first
        expect(!reading.done, "finishes the sentence first")
    }

    fun testManyTakesMatchOnThreeNeighbours() {
        assertEq(1, SilentEvaluator.neighbors(SilentSynth.dataset(listOf("yes", "no"), 9, 1)))
        var dataset = SilentSynth.dataset(listOf("yes", "no"), 10, 1)
        assertEq(3, SilentEvaluator.neighbors(dataset))
        dataset = dataset.addWord("help")!!
        assertEq(3, SilentEvaluator.neighbors(dataset))
        dataset = dataset.removeTakes(setOf("yes-0", "no-1"))
        assertEq(9, dataset.count("yes"))
        assertEq(1, SilentEvaluator.neighbors(dataset))
    }

    fun testLeaveOneOutOnThreeNeighboursMatchesSortingEveryDistance() {
        val random = SilentRandom(21)
        val n = 30
        val d = Array(n) { DoubleArray(n) }
        for (i in 0 until n) for (j in i + 1 until n) {
            d[i][j] = Math.round(random.unit() * 8) / 4.0
            d[j][i] = d[i][j]
        }
        val labels = IntArray(n) { it % 3 }
        val results = SilentEvaluator.leaveOneOut(d, labels, 3, 3)
        results.forEachIndexed { i, r ->
            val scores = (0 until 3).map { w -> labels.indices.filter { it != i && labels[it] == w }.map { d[i][it] }.sorted().take(3).average() }
            assertEq(scores.min(), r.distance)
            assertEq(scores.indexOf(scores.min()), r.guess)
        }
        assertEq(n, results.size)
    }

    fun testTranscriptUndoAndClear() {
        val transcript = SilentTranscript().append("help").append("water")
        assertEq("help water", transcript.text)
        assertEq("help", transcript.undo().text)
        assertEq("", transcript.clear().text)
    }
}
