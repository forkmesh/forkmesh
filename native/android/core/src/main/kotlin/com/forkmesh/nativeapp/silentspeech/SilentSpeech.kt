package com.forkmesh.nativeapp.silentspeech

import com.forkmesh.nativeapp.json.Json
import java.util.Locale
import java.util.UUID
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong
import kotlin.math.sin
import kotlin.math.sqrt

data class SilentVector(val x: Double, val y: Double, val z: Double) {
    val length: Double get() = sqrt(x * x + y * y + z * z)

    fun lerp(other: SilentVector, f: Double) = SilentVector(x + (other.x - x) * f, y + (other.y - y) * f, z + (other.z - z) * f)

    companion object {
        val ZERO = SilentVector(0.0, 0.0, 0.0)
    }
}

data class SilentQuaternion(val w: Double, val x: Double, val y: Double, val z: Double) {
    val conjugate: SilentQuaternion get() = SilentQuaternion(w, -x, -y, -z)

    val normalized: SilentQuaternion
        get() {
            val n = sqrt(w * w + x * x + y * y + z * z)
            return if (n > 1e-12) SilentQuaternion(w / n, x / n, y / n, z / n) else IDENTITY
        }

    operator fun times(b: SilentQuaternion) = SilentQuaternion(
        w * b.w - x * b.x - y * b.y - z * b.z,
        w * b.x + x * b.w + y * b.z - z * b.y,
        w * b.y - x * b.z + y * b.w + z * b.x,
        w * b.z + x * b.y - y * b.x + z * b.w,
    )

    val rotationVector: SilentVector
        get() {
            val q = if (w < 0) SilentQuaternion(-w, -x, -y, -z) else this
            val s = sqrt(q.x * q.x + q.y * q.y + q.z * q.z)
            if (s <= 1e-12) return SilentVector(2 * q.x, 2 * q.y, 2 * q.z)
            val angle = 2 * atan2(s, q.w)
            return SilentVector(q.x / s * angle, q.y / s * angle, q.z / s * angle)
        }

    fun nlerp(other: SilentQuaternion, f: Double): SilentQuaternion {
        val dot = w * other.w + x * other.x + y * other.y + z * other.z
        val o = if (dot < 0) SilentQuaternion(-other.w, -other.x, -other.y, -other.z) else other
        return SilentQuaternion(w + (o.w - w) * f, x + (o.x - x) * f, y + (o.y - y) * f, z + (o.z - z) * f).normalized
    }

    companion object {
        val IDENTITY = SilentQuaternion(1.0, 0.0, 0.0, 0.0)

        fun fromRotationVector(v: SilentVector): SilentQuaternion {
            val angle = v.length
            if (angle <= 1e-12) return IDENTITY
            val s = sin(angle / 2) / angle
            return SilentQuaternion(cos(angle / 2), v.x * s, v.y * s, v.z * s)
        }
    }
}

data class SilentSample(
    val t: Double,
    val rotationRate: SilentVector,
    val attitude: SilentQuaternion,
    val acceleration: SilentVector? = null,
) {
    val energy: Double get() = rotationRate.length + (acceleration?.length ?: 0.0)

    fun lerp(other: SilentSample, time: Double): SilentSample {
        val span = other.t - t
        val f = if (span > 0) ((time - t) / span).coerceIn(0.0, 1.0) else 0.0
        val a = acceleration
        val b = other.acceleration
        val accel = if (a != null && b != null) a.lerp(b, f) else a ?: b
        return SilentSample(time, rotationRate.lerp(other.rotationRate, f), attitude.nlerp(other.attitude, f), accel)
    }
}

enum class SilentChannel(val wire: String, val label: String) {
    ACCELERATION("acceleration", "Movement"),
    ROTATION("rotation", "Turning speed"),
    ATTITUDE("attitude", "Head angle");

    companion object {
        val DEFAULTS: Set<SilentChannel> = entries.toSet()

        fun of(wire: String): SilentChannel? = entries.firstOrNull { it.wire == wire }
    }
}

object SilentFeatures {
    const val RATE = 25.0
    const val TRIM_FRACTION = 0.12
    const val MIN_FRAMES = 4

    fun resampled(samples: List<SilentSample>, rate: Double = RATE): List<SilentSample> {
        if (samples.size < 2) return samples
        val first = samples.first()
        val last = samples.last()
        if (last.t <= first.t) return samples
        val step = 1 / rate
        val count = floor((last.t - first.t) / step).toInt() + 1
        val out = ArrayList<SilentSample>(count)
        var index = 0
        for (k in 0 until count) {
            val time = first.t + k * step
            while (index < samples.size - 2 && samples[index + 1].t < time) index++
            out.add(samples[index].lerp(samples[index + 1], time))
        }
        return out
    }

    fun trimmed(samples: List<SilentSample>, fraction: Double = TRIM_FRACTION, keep: Int = MIN_FRAMES): List<SilentSample> {
        if (samples.size <= keep) return samples
        val peak = samples.maxOf { it.energy }
        if (peak <= 0) return samples
        val limit = peak * fraction
        var start = samples.indexOfFirst { it.energy >= limit }.coerceAtLeast(0)
        var end = samples.indexOfLast { it.energy >= limit }.let { if (it < 0) samples.size - 1 else it }
        while (end - start + 1 < keep) {
            if (start > 0) start--
            if (end - start + 1 < keep && end < samples.size - 1) end++
        }
        return samples.subList(start, end + 1)
    }

    fun dimension(channels: Set<SilentChannel>): Int = channels.size * 3

    fun frames(samples: List<SilentSample>, channels: Set<SilentChannel>): List<DoubleArray> {
        val kept = trimmed(resampled(samples))
        val origin = kept.firstOrNull()?.attitude?.conjugate ?: return emptyList()
        val width = dimension(channels)
        val frames = kept.map { sample ->
            val row = DoubleArray(width)
            var c = 0
            for (channel in SilentChannel.entries) {
                if (channel !in channels) continue
                val v = when (channel) {
                    SilentChannel.ACCELERATION -> sample.acceleration ?: SilentVector.ZERO
                    SilentChannel.ROTATION -> sample.rotationRate
                    SilentChannel.ATTITUDE -> (origin * sample.attitude).rotationVector
                }
                row[c] = v.x
                row[c + 1] = v.y
                row[c + 2] = v.z
                c += 3
            }
            row
        }
        for (c in 0 until width) {
            val mean = frames.sumOf { it[c] } / frames.size
            for (row in frames) row[c] -= mean
        }
        return frames
    }
}

class SilentNormalizer(val scale: DoubleArray) {
    fun apply(frames: List<DoubleArray>): List<DoubleArray> = frames.map { row ->
        if (row.size == scale.size) DoubleArray(row.size) { row[it] * scale[it] } else row
    }

    companion object {
        fun of(sets: List<List<DoubleArray>>): SilentNormalizer {
            val width = sets.firstOrNull { it.isNotEmpty() }?.first()?.size ?: 0
            val sum = DoubleArray(width)
            val squares = DoubleArray(width)
            var n = 0.0
            for (frames in sets) {
                for (row in frames) {
                    if (row.size != width) continue
                    for (c in 0 until width) {
                        sum[c] += row[c]
                        squares[c] += row[c] * row[c]
                    }
                    n += 1
                }
            }
            return SilentNormalizer(DoubleArray(width) { c ->
                if (n <= 1) return@DoubleArray 1.0
                val mean = sum[c] / n
                val sd = sqrt(max(squares[c] / n - mean * mean, 0.0))
                if (sd > 1e-9) 1 / sd else 1.0
            })
        }
    }
}

object SilentDTW {
    const val BAND = 0.25

    fun distance(a: List<DoubleArray>, b: List<DoubleArray>, band: Double = BAND, cutoff: Double = Double.POSITIVE_INFINITY): Double {
        val n = a.size
        val m = b.size
        if (n == 0 || m == 0) return Double.POSITIVE_INFINITY
        val window = max(ceil(band * max(n, m)).toInt(), abs(n - m))
        val norm = (n + m).toDouble()
        var previous = DoubleArray(m + 1) { Double.POSITIVE_INFINITY }
        var current = DoubleArray(m + 1) { Double.POSITIVE_INFINITY }
        previous[0] = 0.0
        for (i in 1..n) {
            current.fill(Double.POSITIVE_INFINITY)
            val lo = max(1, i - window)
            val hi = min(m, i + window)
            var rowBest = Double.POSITIVE_INFINITY
            for (j in lo..hi) {
                val step = min(previous[j], min(current[j - 1], previous[j - 1]))
                current[j] = euclidean(a[i - 1], b[j - 1]) + step
                rowBest = min(rowBest, current[j])
            }
            if (rowBest / norm > cutoff) return Double.POSITIVE_INFINITY
            val swap = previous
            previous = current
            current = swap
        }
        return previous[m] / norm
    }

    fun euclidean(a: DoubleArray, b: DoubleArray): Double {
        var sum = 0.0
        for (k in 0 until min(a.size, b.size)) {
            val d = a[k] - b[k]
            sum += d * d
        }
        return sqrt(sum)
    }
}

data class SilentWord(val word: String, val phrase: String = "") {
    val spoken: String get() = phrase.trim().ifEmpty { word }
}

data class SilentTake(val id: String, val word: String, val samples: List<SilentSample>)

data class SilentMatch(
    val word: String,
    val distance: Double,
    val runnerUp: String?,
    val margin: Double,
    val accepted: Boolean,
) {
    val summary: String
        get() = if (accepted) "$word · ${(margin * 100).roundToInt()}% ahead of ${runnerUp ?: "nothing"}" else "Not sure · closest was $word"

    companion object {
        fun margin(first: Double, second: Double?): Double {
            if (second == null || !second.isFinite() || second <= 0) return 1.0
            return ((second - first) / second).coerceIn(0.0, 1.0)
        }
    }
}

data class SilentRanked(val word: String, val distance: Double, val effective: Double)

object SilentScores {
    fun perWord(distances: List<Pair<String, Double>>, neighbors: Int): List<Pair<String, Double>> {
        val byWord = LinkedHashMap<String, MutableList<Double>>()
        for ((word, distance) in distances) if (distance.isFinite()) byWord.getOrPut(word) { mutableListOf() }.add(distance)
        val k = max(1, neighbors)
        return byWord.entries.mapIndexed { index, (word, values) ->
            val best = values.sorted().take(k)
            Triple(word, best.sum() / best.size, index)
        }.sortedWith(compareBy({ it.second }, { it.third })).map { it.first to it.second }
    }

    fun ranked(scores: List<Pair<String, Double>>, language: SilentLanguage?, previous: String?): List<SilentRanked> {
        if (language == null || scores.size < 2) return scores.map { SilentRanked(it.first, it.second, it.second) }
        val best = scores[0].second
        val middle = scores[scores.size / 2].second
        val tau = maxOf((middle - best) / 2, best * 0.05, 1e-6)
        val costs = scores.map { language.cost(it.first, previous, scores.size) }
        val lowest = costs.min()
        return scores.indices.map { i -> SilentRanked(scores[i].first, scores[i].second, scores[i].second + SilentLanguage.WEIGHT * tau * (costs[i] - lowest)) }
            .withIndex().sortedWith(compareBy({ it.value.effective }, { it.index })).map { it.value }
    }
}

class SilentLanguage(sentences: List<List<String>>) {
    val unigrams = HashMap<String, Int>()
    val bigrams = HashMap<String, HashMap<String, Int>>()
    var total = 0
        private set

    init {
        for (sentence in sentences) {
            var previous = ""
            for (word in sentence) {
                unigrams[word] = (unigrams[word] ?: 0) + 1
                val context = bigrams.getOrPut(previous) { HashMap() }
                context[word] = (context[word] ?: 0) + 1
                total++
                previous = word
            }
        }
    }

    fun probability(word: String, previous: String?, vocabulary: Int): Double {
        val size = maxOf(vocabulary, unigrams.size, 1).toDouble()
        val unigram = ((unigrams[word] ?: 0) + 1) / (total + size)
        val context = bigrams[previous ?: ""] ?: emptyMap<String, Int>()
        val seen = context.values.sum().toDouble()
        return ((context[word] ?: 0) + SMOOTHING * unigram) / (seen + SMOOTHING)
    }

    fun cost(word: String, previous: String?, vocabulary: Int): Double = -ln(probability(word, previous, vocabulary))

    companion object {
        const val WEIGHT = 0.5
        const val SMOOTHING = 2.0
    }
}

class SilentModel(takes: List<SilentTake>, val channels: Set<SilentChannel>, val neighbors: Int = 1, val language: SilentLanguage? = null) {
    val normalizer: SilentNormalizer
    val templates: List<Pair<String, List<DoubleArray>>>

    init {
        val raw = takes.map { it.word to SilentFeatures.frames(it.samples, channels) }.filter { it.second.isNotEmpty() }
        normalizer = SilentNormalizer.of(raw.map { it.second })
        templates = raw.map { it.first to normalizer.apply(it.second) }
    }

    val words: Set<String> get() = templates.map { it.first }.toSet()

    fun match(samples: List<SilentSample>, maxDistance: Double?, minMargin: Double, previous: String? = null): SilentMatch? {
        val frames = normalizer.apply(SilentFeatures.frames(samples, channels))
        if (frames.isEmpty() || templates.isEmpty()) return null
        val ranked = SilentScores.ranked(SilentScores.perWord(templates.map { it.first to SilentDTW.distance(frames, it.second) }, neighbors), language, previous)
        val best = ranked.firstOrNull() ?: return null
        val second = ranked.getOrNull(1)
        val margin = SilentMatch.margin(best.effective, second?.effective)
        val accepted = (maxDistance == null || best.distance <= maxDistance) && (second == null || margin >= minMargin)
        return SilentMatch(best.word, best.distance, second?.word, margin, accepted)
    }
}

class SilentRandom(private var state: Long) {
    fun next(): Long {
        state += -0x61c8864680b583ebL
        var z = state
        z = (z xor (z ushr 30)) * -0x40a7b892e31b1a47L
        z = (z xor (z ushr 27)) * -0x6b2fb644ecceee15L
        return z xor (z ushr 31)
    }

    fun below(n: Int): Int = java.lang.Long.remainderUnsigned(next(), n.toLong()).toInt()

    fun unit(): Double = (next() ushr 11).toDouble() / 9_007_199_254_740_992.0

    fun shuffle(values: IntArray) {
        for (i in values.size - 1 downTo 1) {
            val j = below(i + 1)
            val t = values[i]
            values[i] = values[j]
            values[j] = t
        }
    }
}

data class SilentEvaluation(
    val words: List<String>,
    val evaluated: Int,
    val correct: Int,
    val nullMean: Double,
    val pValue: Double,
    val permutations: Int,
    val confusion: List<List<Int>>,
    val suggestedMaxDistance: Double?,
) {
    val accuracy: Double get() = if (evaluated > 0) correct.toDouble() / evaluated else 0.0
    val chance: Double get() = if (words.isEmpty()) 0.0 else 1.0 / words.size
    val significant: Boolean get() = pValue < 0.05

    val headline: String get() = "Right $correct of $evaluated (${percent(accuracy)}), chance is about ${percent(max(chance, nullMean))}"

    val verdict: String
        get() {
            val p = if (pValue < 0.001) "p < 0.001" else String.format(Locale.US, "p = %.3f", pValue)
            return if (significant) "$p: your head motion tells these words apart better than chance."
            else "$p: not yet better than chance. Record more takes, or mouth the words more clearly."
        }

    fun mixups(limit: Int = 3): List<Triple<String, String, Int>> {
        val out = ArrayList<Triple<String, String, Int>>()
        for (row in words.indices) for (column in words.indices) {
            if (row != column && confusion[row][column] > 0) out.add(Triple(words[row], words[column], confusion[row][column]))
        }
        return out.withIndex().sortedWith(compareBy({ -it.value.third }, { it.index })).map { it.value }.take(limit)
    }

    private fun percent(value: Double) = "${(value * 100).roundToInt()}%"
}

data class SilentResult(val truth: Int, val guess: Int, val distance: Double)

object SilentEvaluator {
    const val PERMUTATIONS = 500
    const val SEED = 0x5EED5EEDL
    const val MINIMUM_TAKES = 2
    const val MANY_TAKES = 10
    const val MANY_NEIGHBORS = 3
    const val MAX_PER_WORD = 12
    const val MAX_TAKES = 600
    const val MINIMUM_PERMUTATIONS = 100
    const val BUDGET = 40_000_000

    fun rounds(requested: Int, takes: Int): Int = min(requested, max(MINIMUM_PERMUTATIONS, BUDGET / max(1, takes * takes)))

    fun sample(takes: List<SilentTake>, perWord: Int = MAX_PER_WORD, total: Int = MAX_TAKES): List<SilentTake> {
        val seen = HashMap<String, Int>()
        val kept = ArrayList<Int>()
        for (i in takes.indices.reversed()) {
            if (kept.size >= total) break
            val n = seen[takes[i].word] ?: 0
            if (n >= perWord) continue
            seen[takes[i].word] = n + 1
            kept.add(i)
        }
        return kept.reversed().map { takes[it] }
    }

    fun neighbors(dataset: SilentDataset): Int {
        val counts = dataset.words.map { dataset.count(it.word) }.filter { it > 0 }
        if (counts.size < 2) return 1
        return if (counts.min() >= MANY_TAKES) MANY_NEIGHBORS else 1
    }

    fun requirement(dataset: SilentDataset): String? {
        val ready = dataset.words.count { dataset.count(it.word) >= MINIMUM_TAKES }
        if (ready >= 2) return null
        return "Record at least $MINIMUM_TAKES takes of at least 2 words to check them. Five or more takes each gives a fairer score."
    }

    fun matrix(frames: List<List<DoubleArray>>): Array<DoubleArray> {
        val n = frames.size
        val d = Array(n) { DoubleArray(n) }
        for (i in 0 until n) for (j in i + 1 until n) {
            val value = SilentDTW.distance(frames[i], frames[j])
            d[i][j] = value
            d[j][i] = value
        }
        return d
    }

    fun leaveOneOut(d: Array<DoubleArray>, labels: IntArray, wordCount: Int, neighbors: Int): List<SilentResult> {
        val counts = IntArray(wordCount)
        for (label in labels) counts[label]++
        val k = max(1, neighbors)
        val out = ArrayList<SilentResult>()
        val near = Array(wordCount) { DoubleArray(k) }
        val sizes = IntArray(wordCount)
        for (i in labels.indices) {
            if (counts[labels[i]] < MINIMUM_TAKES) continue
            sizes.fill(0)
            val row = d[i]
            for (j in labels.indices) {
                if (j == i) continue
                val w = labels[j]
                sizes[w] = keep(near[w], sizes[w], row[j], k)
            }
            var guess = -1
            var score = Double.POSITIVE_INFINITY
            for (w in 0 until wordCount) {
                val size = sizes[w]
                if (size == 0) continue
                var value = near[w][0]
                if (k > 1) {
                    var sum = 0.0
                    for (n in 0 until size) sum += near[w][n]
                    value = sum / size
                }
                if (value < score) {
                    score = value
                    guess = w
                }
            }
            if (guess >= 0) out.add(SilentResult(labels[i], guess, score))
        }
        return out
    }

    fun keep(best: DoubleArray, size: Int, value: Double, k: Int): Int {
        var i: Int
        val next: Int
        if (size < k) {
            best[size] = value
            i = size
            next = size + 1
        } else if (value < best[k - 1]) {
            best[k - 1] = value
            i = k - 1
            next = size
        } else {
            return size
        }
        while (i > 0 && best[i - 1] > best[i]) {
            val t = best[i - 1]
            best[i - 1] = best[i]
            best[i] = t
            i--
        }
        return next
    }

    fun evaluate(
        dataset: SilentDataset,
        channels: Set<SilentChannel>,
        neighbors: Int = 1,
        permutations: Int = PERMUTATIONS,
        seed: Long = SEED,
    ): SilentEvaluation? {
        if (requirement(dataset) != null) return null
        val sampled = sample(dataset.takes)
        val counts = sampled.groupingBy { it.word }.eachCount()
        val words = dataset.words.map { it.word }.filter { (counts[it] ?: 0) >= MINIMUM_TAKES }
        if (words.size < 2) return null
        val chosen = words.toSet()
        val model = SilentModel(sampled.filter { it.word in chosen }, channels, neighbors)
        if (model.templates.isEmpty()) return null
        val index = words.withIndex().associate { it.value to it.index }
        val labels = IntArray(model.templates.size) { index[model.templates[it].first] ?: 0 }
        val d = matrix(model.templates.map { it.second })
        val results = leaveOneOut(d, labels, words.size, neighbors)
        if (results.isEmpty()) return null
        val correct = results.count { it.truth == it.guess }
        val confusion = List(words.size) { IntArray(words.size) }
        for (r in results) confusion[r.truth][r.guess]++
        val rounds = rounds(permutations, labels.size)
        val random = SilentRandom(seed)
        val shuffled = labels.copyOf()
        var atLeast = 0
        var nullTotal = 0.0
        repeat(rounds) {
            random.shuffle(shuffled)
            val null0 = leaveOneOut(d, shuffled, words.size, neighbors)
            val hits = null0.count { it.truth == it.guess }
            if (hits >= correct) atLeast++
            nullTotal += if (null0.isEmpty()) 0.0 else hits.toDouble() / null0.size
        }
        val good = results.filter { it.truth == it.guess }.map { it.distance }.sorted()
        val suggested = if (good.size >= 3) good[max(0, ceil(0.9 * good.size).toInt() - 1)] * 1.25 else null
        return SilentEvaluation(
            words, results.size, correct,
            if (rounds > 0) nullTotal / rounds else 0.0,
            (1 + atLeast).toDouble() / (1 + rounds), rounds,
            confusion.map { it.toList() }, suggested,
        )
    }
}

data class SilentNoiseFloor(val floor: Double, val spread: Double) {
    val on: Double get() = floor + max(4 * spread, MINIMUM_RISE)
    val off: Double get() = floor + max(2 * spread, MINIMUM_RISE / 2)

    companion object {
        const val MINIMUM_RISE = 0.05

        fun calibrate(energies: List<Double>): SilentNoiseFloor? {
            if (energies.size < 10) return null
            val middle = median(energies)
            return SilentNoiseFloor(middle, 1.4826 * median(energies.map { abs(it - middle) }))
        }

        private fun median(values: List<Double>): Double {
            val sorted = values.sorted()
            val m = sorted.size / 2
            return if (sorted.size % 2 == 1) sorted[m] else (sorted[m - 1] + sorted[m]) / 2
        }
    }
}

class SilentSegmenter(
    val noise: SilentNoiseFloor,
    val preRoll: Double = 0.2,
    val hangover: Double = 0.35,
    val minimum: Double = 0.3,
    val maximum: Double = 3.0,
) {
    var active: Boolean = false
        private set
    private val history = ArrayList<SilentSample>()
    private var segment = ArrayList<SilentSample>()
    private var rising = 0
    private var onset = 0.0
    private var quietSince: Double? = null

    fun reset() {
        active = false
        history.clear()
        segment = ArrayList()
        rising = 0
        quietSince = null
    }

    fun push(sample: SilentSample): List<SilentSample>? {
        val energy = sample.energy
        if (!active) {
            history.add(sample)
            history.removeAll { it.t < sample.t - preRoll }
            rising = if (energy > noise.on) rising + 1 else 0
            if (rising == 1) onset = sample.t
            if (rising >= 2) {
                active = true
                segment = ArrayList(history)
                history.clear()
                quietSince = null
            }
            return null
        }
        segment.add(sample)
        if (energy < noise.off) {
            if (quietSince == null) quietSince = sample.t
        } else {
            quietSince = null
        }
        val start = segment.firstOrNull()?.t ?: sample.t
        val ended = quietSince?.let { sample.t - it >= hangover } ?: false
        if (!ended && sample.t - start < maximum) return null
        val cut = quietSince ?: sample.t
        val finished = segment.filter { it.t <= cut + 0.08 }
        active = false
        segment = ArrayList()
        rising = 0
        quietSince = null
        if (finished.isEmpty() || cut - onset < minimum) return null
        return finished
    }
}

object SilentText {
    val SENTENCES: List<String> = listOf(
        "Can you help me please",
        "I would like some water",
        "Yes that sounds good to me",
        "No thank you I am fine",
        "Please stop for a moment",
        "Where are we going today",
        "What time is it now",
        "I need to go home soon",
        "Call me when you are ready",
        "Let us meet again tomorrow",
        "The weather is nice outside",
        "I am feeling tired tonight",
        "Turn the music down a little",
        "Open the door for me",
        "How was your day at work",
        "We should eat something first",
        "Send a message to my friend",
        "I do not know the answer",
        "Bring me a cup of coffee",
        "Wait here I will be back",
        "That is a really good idea",
        "Could you speak more slowly",
        "Read the next page out loud",
        "My phone battery is almost empty",
        "Remind me to buy some bread",
        "Good morning how did you sleep",
        "I love you very much",
        "Is there anything else you need",
        "Show me the way to the station",
        "Thank you for your help",
    )

    fun words(text: String): List<String> {
        val cleaned = text.lowercase(Locale.ROOT).map { c ->
            when {
                c == '\u2019' -> '\''
                c.isLetterOrDigit() || c == '\'' -> c
                else -> ' '
            }
        }.joinToString("")
        return cleaned.split(' ').map { it.trim('\'') }.filter { it.isNotEmpty() }
    }
}

data class SilentReading(
    val sentences: List<List<String>>,
    val duration: Double = DURATION,
    val index: Int = 0,
    val heard: List<List<SilentSample>> = emptyList(),
    val started: Double? = null,
    val elapsed: Double = 0.0,
    val sentencesRead: Int = 0,
    val collected: Int = 0,
) {
    val sentence: List<String> get() = if (sentences.isEmpty()) emptyList() else sentences[index % sentences.size]
    val position: Int get() = heard.size
    val timeUp: Boolean get() = elapsed >= duration
    val done: Boolean get() = timeUp && heard.isEmpty()

    val clock: String get() = "${minutes(elapsed)} of ${minutes(duration)}"

    val summary: String
        get() {
            val takes = if (collected == 1) "1 take" else "$collected takes"
            val read = if (sentencesRead == 1) "1 sentence" else "$sentencesRead sentences"
            return "Saved $takes from $read."
        }

    fun tick(t: Double): SilentReading {
        val from = started ?: t
        return copy(started = from, elapsed = max(0.0, t - from))
    }

    fun hear(segment: List<SilentSample>): Pair<SilentReading, List<Pair<String, List<SilentSample>>>> {
        if (segment.isEmpty() || sentence.isEmpty()) return this to emptyList()
        val next = heard + listOf(segment)
        if (next.size < sentence.size) return copy(heard = next) to emptyList()
        val labelled = sentence.zip(next)
        return copy(heard = emptyList(), index = index + 1, sentencesRead = sentencesRead + 1, collected = collected + labelled.size) to labelled
    }

    fun again(): SilentReading = copy(heard = emptyList())

    fun skip(): SilentReading = copy(heard = emptyList(), index = index + 1)

    fun forget(takes: Int): SilentReading =
        if (takes <= 0 || sentencesRead <= 0) this else copy(sentencesRead = sentencesRead - 1, collected = max(0, collected - takes))

    companion object {
        const val DURATION = 300.0
        const val SCRIPT_LENGTH = 100
        const val SESSION_SENTENCES = 8
        const val CHUNK = 5

        fun start(sentences: List<List<String>>, seed: Long, duration: Double = DURATION) = SilentReading(script(sentences, seed), duration)

        fun pool(dataset: SilentDataset): List<List<String>> {
            val out = ArrayList<List<String>>()
            for (text in dataset.sentences + SilentText.SENTENCES) {
                val words = SilentText.words(text)
                if (words.isNotEmpty() && words !in out) out.add(words)
            }
            val covered = out.flatten().toSet()
            val orphans = dataset.words.map { it.word }.filter { it !in covered }
            orphans.chunked(CHUNK).forEach { out.add(it) }
            return out
        }

        fun plan(dataset: SilentDataset, size: Int = SESSION_SENTENCES): List<List<String>> {
            val counts = dataset.counts()
            return pool(dataset).withIndex()
                .map { (i, sentence) -> Triple(sentence, sentence.sumOf { min(counts[it] ?: 0, 10) }.toDouble() / sentence.size, i) }
                .sortedWith(compareBy({ it.second }, { it.third }))
                .take(max(1, size))
                .map { it.first }
        }

        fun script(sentences: List<List<String>>, seed: Long, count: Int = SCRIPT_LENGTH): List<List<String>> {
            val usable = sentences.filter { it.isNotEmpty() }
            if (usable.isEmpty()) return emptyList()
            val random = SilentRandom(seed)
            val order = ArrayList<Int>()
            while (order.size < count) {
                val pass = IntArray(usable.size) { it }
                random.shuffle(pass)
                if (order.isNotEmpty() && pass.size > 1 && pass[0] == order.last()) {
                    pass[0] = pass[1]
                    pass[1] = order.last()
                }
                pass.forEach { order.add(it) }
            }
            return order.take(count).map { usable[it] }
        }

        private fun minutes(seconds: Double): String {
            val whole = max(0, floor(seconds).toInt())
            return String.format(Locale.US, "%d:%02d", whole / 60, whole % 60)
        }
    }
}

data class SilentScopeFrame(val t: Double, val rotation: SilentVector, val angle: SilentVector, val acceleration: SilentVector?, val energy: Double)

data class SilentMark(val start: Double, val end: Double, val label: String)

data class SilentSpectrum(val power: List<Double>, val binHz: Double, val peakHz: Double?) {
    companion object {
        val EMPTY = SilentSpectrum(emptyList(), SilentFeatures.RATE / SilentScope.SPECTRUM_SIZE, null)
    }
}

data class SilentScope(
    val samples: List<SilentSample> = emptyList(),
    val reference: SilentQuaternion? = null,
    val marks: List<SilentMark> = emptyList(),
) {
    val latest: Double? get() = samples.lastOrNull()?.t

    fun push(sample: SilentSample): SilentScope {
        val start = sample.t - WINDOW
        val first = samples.indexOfFirst { it.t >= start }
        val kept = if (first > 0) samples.subList(first, samples.size) else if (first < 0) emptyList() else samples
        return SilentScope(kept + sample, reference ?: sample.attitude, marks.filter { it.end >= start })
    }

    fun recentre(): SilentScope = copy(reference = samples.lastOrNull()?.attitude ?: reference)

    fun mark(segment: List<SilentSample>, label: String): SilentScope {
        if (segment.isEmpty()) return this
        return copy(marks = marks + SilentMark(segment.first().t, segment.last().t, label))
    }

    val frames: List<SilentScopeFrame>
        get() {
            val origin = (reference ?: samples.firstOrNull()?.attitude ?: SilentQuaternion.IDENTITY).conjugate
            return SilentFeatures.resampled(samples).map { s ->
                val r = s.rotationRate
                val a = (origin * s.attitude).rotationVector
                SilentScopeFrame(s.t, SilentVector(r.x * DEGREES, r.y * DEGREES, r.z * DEGREES), SilentVector(a.x * DEGREES, a.y * DEGREES, a.z * DEGREES), s.acceleration, s.energy)
            }
        }

    val spectrum: SilentSpectrum get() = spectrum(frames.map { it.rotation })

    companion object {
        const val WINDOW = 6.0
        const val SPECTRUM_SIZE = 64
        const val MINIMUM_SPECTRUM = 16
        const val LOWEST_PEAK_HZ = 0.5
        const val DEGREES = 180 / PI

        fun spectrum(values: List<SilentVector>, size: Int = SPECTRUM_SIZE, rate: Double = SilentFeatures.RATE): SilentSpectrum {
            val binHz = rate / size
            val tail = values.takeLast(size)
            if (tail.size < MINIMUM_SPECTRUM) return SilentSpectrum(emptyList(), binHz, null)
            val n = tail.size
            val pad = size - n
            val power = DoubleArray(size / 2 + 1)
            for (axis in 0 until 3) {
                val raw = tail.map { if (axis == 0) it.x else if (axis == 1) it.y else it.z }
                val mean = raw.sum() / n
                val signal = DoubleArray(size)
                for (i in 0 until n) {
                    val hann = if (n > 1) 0.5 - 0.5 * cos(2 * PI * i / (n - 1)) else 1.0
                    signal[pad + i] = (raw[i] - mean) * hann
                }
                for (k in 0..size / 2) {
                    var re = 0.0
                    var im = 0.0
                    for (i in 0 until size) {
                        val phase = 2 * PI * (k * i).toDouble() / size
                        re += signal[i] * cos(phase)
                        im -= signal[i] * sin(phase)
                    }
                    power[k] += (re * re + im * im) / size
                }
            }
            var peak: Int? = null
            for (k in power.indices) {
                if (k * binHz < LOWEST_PEAK_HZ || power[k] <= 1e-6) continue
                if (peak == null || power[k] > power[peak]) peak = k
            }
            return SilentSpectrum(power.toList(), binHz, peak?.let { it * binHz })
        }
    }
}

data class SilentDataset(val words: List<SilentWord>, val takes: List<SilentTake> = emptyList(), val sentences: List<String> = emptyList()) {
    fun count(word: String): Int = takes.count { it.word == word }

    fun counts(): Map<String, Int> = takes.groupingBy { it.word }.eachCount()

    val language: SilentLanguage get() = SilentLanguage((sentences + SilentText.SENTENCES).map(SilentText::words))

    fun addSentence(text: String): SilentDataset? {
        val words = SilentText.words(text)
        if (words.size < 2 || sentences.any { SilentText.words(it) == words }) return null
        return words.fold(copy(sentences = sentences + text.trim())) { d, word -> d.addWord(word) ?: d }
    }

    fun addEntry(text: String): Pair<SilentDataset, String>? {
        val colon = text.indexOf(':')
        if (colon >= 0) {
            val word = clean(text.substring(0, colon))
            return addWord(word, text.substring(colon + 1))?.let { it to word }
        }
        val words = SilentText.words(text)
        if (words.size >= 2) return addSentence(text)?.let { it to words.first() }
        val word = words.firstOrNull() ?: return null
        return addWord(word)?.let { it to word }
    }

    fun removeSentence(text: String): SilentDataset = copy(sentences = sentences.filter { it != text })

    fun learn(take: SilentTake): SilentDataset = (addWord(take.word) ?: this).addTake(take)

    fun addWord(word: String, phrase: String = ""): SilentDataset? {
        val name = clean(word)
        if (name.isEmpty() || words.any { it.word == name }) return null
        return copy(words = words + SilentWord(name, phrase.trim()))
    }

    fun withPhrase(word: String, phrase: String): SilentDataset =
        copy(words = words.map { if (it.word == word) it.copy(phrase = phrase.trim()) else it })

    fun removeWord(word: String): SilentDataset = copy(words = words.filter { it.word != word }, takes = takes.filter { it.word != word })

    fun addTake(take: SilentTake): SilentDataset =
        if (words.none { it.word == take.word } || take.samples.size < 2) this else copy(takes = takes + take)

    fun removeLastTake(word: String): SilentDataset {
        val i = takes.indexOfLast { it.word == word }
        return if (i < 0) this else copy(takes = takes.filterIndexed { index, _ -> index != i })
    }

    fun removeTakes(ids: Set<String>): SilentDataset = copy(takes = takes.filter { it.id !in ids })

    fun phrase(word: String): String = words.firstOrNull { it.word == word }?.spoken ?: word

    fun json(): Map<String, Any?> = linkedMapOf(
        "sentences" to sentences,
        "takes" to takes.map { take -> linkedMapOf("id" to take.id, "samples" to take.samples.map(::encode), "word" to take.word) },
        "version" to VERSION.toLong(),
        "words" to words.map { linkedMapOf("phrase" to it.phrase, "word" to it.word) },
    )

    fun encoded(): String = Json.stringify(json())

    companion object {
        const val VERSION = 1

        val STARTER = SilentDataset(listOf("yes", "no", "help", "stop", "water").map { SilentWord(it) })

        fun clean(word: String): String = word.trim().lowercase(Locale.ROOT)

        fun decode(text: String): SilentDataset? {
            val root = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return null
            val version = (root["version"] as? Number)?.toInt() ?: 0
            if (version > VERSION) return null
            val rawWords = root["words"] as? List<*> ?: return null
            var dataset = SilentDataset(emptyList(), sentences = (root["sentences"] as? List<*>).orEmpty().filterIsInstance<String>())
            for (raw in rawWords) {
                val map = raw as? Map<*, *> ?: continue
                val word = map["word"] as? String ?: continue
                dataset = dataset.addWord(word, map["phrase"] as? String ?: "") ?: dataset
            }
            for (raw in root["takes"] as? List<*> ?: emptyList<Any?>()) {
                val map = raw as? Map<*, *> ?: continue
                val word = map["word"] as? String ?: continue
                val rows = map["samples"] as? List<*> ?: continue
                val samples = rows.mapNotNull { decodeSample(it as? List<*>) }
                dataset = dataset.addTake(SilentTake(map["id"] as? String ?: UUID.randomUUID().toString(), word, samples))
            }
            return dataset
        }

        private fun round(value: Double): Double = (value * 100_000).roundToLong() / 100_000.0

        fun encode(s: SilentSample): List<Double> {
            val row = mutableListOf(s.t, s.rotationRate.x, s.rotationRate.y, s.rotationRate.z, s.attitude.w, s.attitude.x, s.attitude.y, s.attitude.z)
            s.acceleration?.let { row.addAll(listOf(it.x, it.y, it.z)) }
            return row.map(::round)
        }

        fun decodeSample(raw: List<*>?): SilentSample? {
            if (raw == null) return null
            val v = raw.mapNotNull { (it as? Number)?.toDouble() }
            if (v.size != raw.size || (v.size != 8 && v.size != 11)) return null
            val accel = if (v.size == 11) SilentVector(v[8], v[9], v[10]) else null
            return SilentSample(v[0], SilentVector(v[1], v[2], v[3]), SilentQuaternion(v[4], v[5], v[6], v[7]).normalized, accel)
        }
    }
}

data class SilentTranscript(val words: List<String> = emptyList()) {
    val text: String get() = words.joinToString(" ")

    fun append(word: String) = SilentTranscript(words + word)

    fun undo() = SilentTranscript(words.dropLast(1))

    fun clear() = SilentTranscript()
}

object SilentSpeechPrefs {
    const val FILE = "silent_speech"
    const val CHANNELS = "silentSpeech.channels"
    const val MIN_MARGIN = "silentSpeech.minMargin"
    const val MAX_DISTANCE = "silentSpeech.maxDistance"
    const val SPEAK_ALOUD = "silentSpeech.speakAloud"
    const val AUTO_DETECT = "silentSpeech.autoDetect"
    const val NEIGHBORS = "silentSpeech.neighbors"
    const val DEFAULT_MIN_MARGIN = 0.08
}
