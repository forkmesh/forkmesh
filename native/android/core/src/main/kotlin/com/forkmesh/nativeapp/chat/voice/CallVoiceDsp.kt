package com.forkmesh.nativeapp.chat.voice

internal const val TWO_PI = 2 * Math.PI
internal const val EPS = 1e-9
internal const val LIMIT_CEILING = 0.95
internal const val MALE_HZ = 115.0
internal const val FEMALE_HZ = 205.0
internal const val FEMALE_TRACT = 1.17
internal const val VOICING_ENTER = 0.55
internal const val VOICING_STAY = 0.45
internal const val ANCHOR_BIAS = 0.3
internal const val LOCAL_VOICING = 0.6
internal const val PITCH_LOW_HZ = 50.0
internal const val PITCH_HIGH_HZ = 420.0
internal const val PULSE_DEPTH = 0.7
internal val PULSE_GAIN = 1 / StrictMath.sqrt(1 + (PULSE_DEPTH * PULSE_DEPTH) / 2)
internal const val TREMOR_HZ = 5.5

internal object VoiceMath {
    private val LN2 = StrictMath.log(2.0)

    fun sin(x: Double) = StrictMath.sin(x)
    fun cos(x: Double) = StrictMath.cos(x)
    fun exp(x: Double) = StrictMath.exp(x)
    fun log(x: Double) = StrictMath.log(x)
    fun log2(x: Double) = StrictMath.log(x) / LN2
    fun pow(x: Double, y: Double) = StrictMath.pow(x, y)
    fun sqrt(x: Double) = StrictMath.sqrt(x)
    fun floor(x: Double) = StrictMath.floor(x)
    fun ceil(x: Double) = StrictMath.ceil(x)
    fun abs(x: Double) = Math.abs(x)

    fun round(x: Double): Double {
        val f = StrictMath.floor(x)
        return if (x - f >= 0.5) f + 1 else f
    }

    fun clamp(value: Double, low: Double, high: Double) = if (value < low) low else if (value > high) high else value
}

internal class Fft(val size: Int) {
    private val cos = DoubleArray(size / 2)
    private val sin = DoubleArray(size / 2)
    private val reverse = IntArray(size)

    init {
        for (i in 0 until size / 2) {
            cos[i] = VoiceMath.cos((TWO_PI * i) / size)
            sin[i] = VoiceMath.sin((TWO_PI * i) / size)
        }
        val bits = Integer.numberOfTrailingZeros(size)
        for (i in 0 until size) {
            var r = 0
            for (b in 0 until bits) r = r or (((i shr b) and 1) shl (bits - 1 - b))
            reverse[i] = r
        }
    }

    fun transform(re: DoubleArray, im: DoubleArray, inverse: Boolean) {
        val n = size
        for (i in 0 until n) {
            val j = reverse[i]
            if (j > i) {
                var t = re[i]
                re[i] = re[j]
                re[j] = t
                t = im[i]
                im[i] = im[j]
                im[j] = t
            }
        }
        val sign = if (inverse) 1.0 else -1.0
        var len = 2
        while (len <= n) {
            val half = len shr 1
            val step = n / len
            var start = 0
            while (start < n) {
                for (k in 0 until half) {
                    val wr = cos[k * step]
                    val wi = sign * sin[k * step]
                    val a = start + k
                    val b = a + half
                    val xr = re[b] * wr - im[b] * wi
                    val xi = re[b] * wi + im[b] * wr
                    re[b] = re[a] - xr
                    im[b] = im[a] - xi
                    re[a] += xr
                    im[a] += xi
                }
                start += len
            }
            len = len shl 1
        }
        if (inverse) {
            val scale = 1.0 / n
            for (i in 0 until n) {
                re[i] *= scale
                im[i] *= scale
            }
        }
    }
}

internal class RealFft(val size: Int) {
    private val half = size / 2
    private val fft = Fft(size / 2)
    private val zr = DoubleArray(size / 2)
    private val zi = DoubleArray(size / 2)
    private val wr = DoubleArray(size / 2)
    private val wi = DoubleArray(size / 2)

    init {
        for (k in 0 until size / 2) {
            wr[k] = VoiceMath.cos((TWO_PI * k) / size)
            wi[k] = -VoiceMath.sin((TWO_PI * k) / size)
        }
    }

    fun forward(x: DoubleArray, re: DoubleArray, im: DoubleArray) {
        val h = half
        for (i in 0 until h) {
            zr[i] = x[2 * i]
            zi[i] = x[2 * i + 1]
        }
        fft.transform(zr, zi, false)
        re[0] = zr[0] + zi[0]
        im[0] = 0.0
        re[h] = zr[0] - zi[0]
        im[h] = 0.0
        for (k in 1 until h) {
            val ar = zr[k]
            val ai = zi[k]
            val br = zr[h - k]
            val bi = -zi[h - k]
            val er = 0.5 * (ar + br)
            val ei = 0.5 * (ai + bi)
            val or = 0.5 * (ai - bi)
            val oi = -0.5 * (ar - br)
            val wr = wr[k]
            val wi = wi[k]
            re[k] = er + wr * or - wi * oi
            im[k] = ei + wr * oi + wi * or
        }
    }

    fun inverse(re: DoubleArray, im: DoubleArray, x: DoubleArray) {
        val h = half
        for (k in 0 until h) {
            val ar = re[k]
            val ai = im[k]
            val br = re[h - k]
            val bi = -im[h - k]
            val er = 0.5 * (ar + br)
            val ei = 0.5 * (ai + bi)
            val dr = 0.5 * (ar - br)
            val di = 0.5 * (ai - bi)
            val wr = wr[k]
            val wi = -wi[k]
            val or = dr * wr - di * wi
            val oi = dr * wi + di * wr
            zr[k] = er - oi
            zi[k] = ei + or
        }
        fft.transform(zr, zi, true)
        for (i in 0 until h) {
            x[2 * i] = zr[i]
            x[2 * i + 1] = zi[i]
        }
    }
}

internal class PitchDetector(private val sampleRate: Double, private val size: Int) {
    private val fft = RealFft(size * 2)
    private val buffer = DoubleArray(size * 2)
    private val re = DoubleArray(size + 1)
    private val im = DoubleArray(size + 1)
    private val minLag = maxOf(2, VoiceMath.floor(sampleRate / PITCH_HIGH_HZ).toInt())
    private val maxLag = minOf(size - 2, VoiceMath.ceil(sampleRate / PITCH_LOW_HZ).toInt())
    private val nsdf = DoubleArray(maxLag + 2)
    private val peaks = IntArray(64)

    fun detect(samples: DoubleArray, previousHz: Double): Double {
        val n = size
        var mean = 0.0
        for (i in 0 until n) mean += samples[i]
        mean /= n
        var energy = 0.0
        for (i in 0 until n) {
            val v = samples[i] - mean
            buffer[i] = v
            energy += v * v
        }
        if (VoiceMath.sqrt(energy / n) < 0.004) return 0.0
        buffer.fill(0.0, n, buffer.size)
        fft.forward(buffer, re, im)
        for (k in 0..n) {
            re[k] = re[k] * re[k] + im[k] * im[k]
            im[k] = 0.0
        }
        fft.inverse(re, im, buffer)
        var m = 2 * buffer[0]
        for (tau in 0..maxLag + 1) {
            if (tau > 0) {
                val head = samples[tau - 1] - mean
                val tail = samples[n - tau] - mean
                m -= head * head + tail * tail
            }
            nsdf[tau] = if (m > EPS) (2 * buffer[tau]) / m else 0.0
        }
        var count = 0
        var tau = 1
        while (tau < maxLag && nsdf[tau] > 0) tau += 1
        while (tau < maxLag && count < 64) {
            while (tau < maxLag && nsdf[tau] <= 0) tau += 1
            var best = tau
            while (tau < maxLag && nsdf[tau] > 0) {
                if (nsdf[tau] > nsdf[best]) best = tau
                tau += 1
            }
            if (best >= minLag && best < maxLag) peaks[count++] = best
        }
        if (count == 0) return 0.0
        var highest = 0.0
        for (i in 0 until count) highest = maxOf(highest, nsdf[peaks[i]])
        val tracking = previousHz > 0
        if (highest < (if (tracking) VOICING_STAY else VOICING_ENTER)) return 0.0
        var lag = peaks[0]
        for (i in 0 until count) {
            if (nsdf[peaks[i]] >= 0.9 * highest) {
                lag = peaks[i]
                break
            }
        }
        if (tracking) {
            val expected = sampleRate / previousHz
            if (VoiceMath.abs(VoiceMath.log2(lag / expected)) > 0.4) {
                var near = -1
                for (i in 0 until count) {
                    if (VoiceMath.abs(VoiceMath.log2(peaks[i] / expected)) < 0.2 && (near < 0 || nsdf[peaks[i]] > nsdf[near])) near = peaks[i]
                }
                if (near > 0 && nsdf[near] >= 0.8 * highest) lag = near
            }
        }
        val a = nsdf[lag - 1]
        val b = nsdf[lag]
        val c = nsdf[lag + 1]
        val denominator = a - 2 * b + c
        val shift = if (VoiceMath.abs(denominator) > EPS) VoiceMath.clamp((0.5 * (a - c)) / denominator, -1.0, 1.0) else 0.0
        return sampleRate / (lag + shift)
    }
}

internal class PitchShifter(sampleRate: Double) {
    private val mask = 8191
    private val maxHalf = VoiceMath.floor(sampleRate * 0.0107).toInt()
    private val unvoiced = maxOf(16, VoiceMath.floor(sampleRate * 0.005).toInt())
    private val lookahead = maxHalf + maxHalf / 4
    val latency = lookahead + maxHalf + 1
    private val delay = latency
    private val input = DoubleArray(mask + 1)
    private val output = DoubleArray(mask + 1)
    private val pulse = DoubleArray(mask + 1) { 1.0 }
    var pulseOut = 1.0
        private set
    private val smooth = DoubleArray(mask + 1)
    private val lpCoef = 1 - VoiceMath.exp((-TWO_PI * 900) / sampleRate)
    private var lp1 = 0.0
    private var lp2 = 0.0
    private val markCount = 32
    private val markAt = DoubleArray(markCount)
    private val markPeriod = DoubleArray(markCount)
    private val markHalf = IntArray(markCount)
    private val markVoiced = BooleanArray(markCount)
    private var markWrite = 0
    private var written = 0
    private var lastAt = 0.0
    private var lastVoiced = false
    private var next = 0.0
    private var shifting = 0.0
    private var range = 1.0
    private var centrePeriod = sampleRate / MALE_HZ
    private var jitter = 0.0
    private var tremor = 0.0
    private val tremorStep = (TWO_PI * TREMOR_HZ) / sampleRate
    private var seed = 0x6d2b79f5

    fun shape(range: Double, centrePeriod: Double, jitter: Double, tremor: Double) {
        this.range = range
        this.centrePeriod = centrePeriod
        this.jitter = jitter
        this.tremor = tremor
    }

    private fun random(): Double {
        var x = seed
        x = x xor (x shl 13)
        x = x xor (x ushr 17)
        x = x xor (x shl 5)
        seed = x
        return (x.toLong() and 0xffffffffL).toDouble() / 4294967296.0 - 0.5
    }

    private fun similarity(a: Int, b: Int, reach: Int, stride: Int = 2): Double {
        var dot = 0.0
        var ea = 0.0
        var eb = 0.0
        var j = -reach
        while (j <= reach) {
            val x = input[(a + j) and mask]
            val y = input[(b + j) and mask]
            dot += x * y
            ea += x * x
            eb += y * y
            j += stride
        }
        return dot / VoiceMath.sqrt(ea * eb + 1e-18)
    }

    private fun addMarks(period: Double) {
        val voiced = period >= 16
        val step = if (voiced) VoiceMath.floor(period + 0.5).toInt() else unvoiced
        val half = if (voiced) minOf(step, maxHalf) else unvoiced
        val reach = maxOf(8, half)
        while (true) {
            val onset = voiced && !lastVoiced
            val search = if (voiced) (if (onset) step / 2 else step / 8) else 0
            val lastAt = this.lastAt
            val anchor = VoiceMath.round(lastAt).toInt()
            val guess = anchor + step
            if (guess + search + half + 2 >= written) return
            var bestIndex = guess
            var best = guess.toDouble()
            var likeness = 1.0
            if (voiced) {
                var peak = 1e-12
                for (c in guess - search..guess + search) {
                    val v = VoiceMath.abs(smooth[c and mask])
                    if (v > peak) {
                        peak = v
                        if (onset) bestIndex = c
                    }
                }
                if (onset) {
                    likeness = -1.0
                    val wobble = maxOf(1, step / 8)
                    for (lag in step - wobble..step + wobble) {
                        likeness = maxOf(likeness, similarity(bestIndex, bestIndex - lag, reach))
                    }
                    best = bestIndex.toDouble()
                } else {
                    var bestScore = Double.NEGATIVE_INFINITY
                    for (c in guess - search..guess + search) {
                        val like = similarity(anchor, c, reach)
                        val score = like + (ANCHOR_BIAS * VoiceMath.abs(smooth[c and mask])) / peak
                        if (score > bestScore) {
                            bestScore = score
                            bestIndex = c
                            likeness = like
                        }
                    }
                    val a = similarity(anchor, bestIndex - 1, reach, 1)
                    val b = similarity(anchor, bestIndex, reach, 1)
                    val c = similarity(anchor, bestIndex + 1, reach, 1)
                    val denominator = a - 2 * b + c
                    val shift = if (denominator < -EPS) VoiceMath.clamp((0.5 * (a - c)) / denominator, -0.5, 0.5) else 0.0
                    best = bestIndex + (shift + (lastAt - anchor))
                }
            }
            val periodic = voiced && likeness >= LOCAL_VOICING
            if (voiced && !periodic) best = (anchor + unvoiced).toDouble()
            val slot = markWrite
            markAt[slot] = best
            markPeriod[slot] = if (periodic) (if (onset) step.toDouble() else best - lastAt) else unvoiced.toDouble()
            markHalf[slot] = if (periodic) half else unvoiced
            markVoiced[slot] = periodic
            markWrite = (slot + 1) % markCount
            this.lastAt = best
            lastVoiced = periodic
        }
    }

    private fun placeGrains(ratio: Double) {
        while (next + lookahead < written) {
            val s = next
            var mark = -1
            var nearest = 0.0
            for (i in 0 until markCount) {
                if (markPeriod[i] <= 0) continue
                val distance = VoiceMath.abs(markAt[i] - s)
                if (mark < 0 || distance < nearest) {
                    mark = i
                    nearest = distance
                }
            }
            if (mark < 0) {
                next += unvoiced
                continue
            }
            val voiced = markVoiced[mark]
            val period = markPeriod[mark]
            var hop = period
            if (voiced) {
                var r = ratio
                if (range != 1.0) r *= VoiceMath.pow(period / centrePeriod, 1 - range)
                if (tremor > 0) r *= 1 + tremor * VoiceMath.sin(tremorStep * s)
                hop = period / VoiceMath.clamp(r, 0.45 * 0.9, 2.6 * 1.1)
                if (jitter > 0) hop *= 1 + 2 * jitter * random()
                hop = maxOf(8.0, hop)
            }
            val centre = markAt[mark]
            var half = markHalf[mark]
            if (voiced && hop > 2 * half) {
                val room = VoiceMath.floor(minOf(maxHalf.toDouble(), minOf((written - 3) - centre, s - (written - delay)))).toInt()
                half = maxOf(half, minOf(VoiceMath.ceil(hop / 2).toInt(), room))
            }
            val gain = if (voiced) VoiceMath.sqrt(hop / half) else 1.0
            val offset = centre - s
            val wholeD = VoiceMath.floor(offset)
            val whole = wholeD.toInt()
            val f = offset - wholeD
            val f2 = f * f
            val f3 = f2 * f
            val c0 = gain * (-0.5 * f3 + f2 - 0.5 * f)
            val c1 = gain * (1.5 * f3 - 2.5 * f2 + 1)
            val c2 = gain * (-1.5 * f3 + 2 * f2 + 0.5 * f)
            val c3 = gain * (0.5 * f3 - 0.5 * f2)
            val start = VoiceMath.floor(s - half).toInt() + 1
            val end = VoiceMath.ceil(s + half).toInt()
            val w = Math.PI / half
            val turnCos = VoiceMath.cos(w)
            val turnSin = VoiceMath.sin(w)
            var wc = VoiceMath.cos(w * (start - s))
            var ws = VoiceMath.sin(w * (start - s))
            for (n in start until end) {
                val j = n + whole
                val value = c0 * input[(j - 1) and mask] + c1 * input[j and mask] + c2 * input[(j + 1) and mask] + c3 * input[(j + 2) and mask]
                output[n and mask] += (0.5 + 0.5 * wc) * value
                val nc = wc * turnCos - ws * turnSin
                ws = ws * turnCos + wc * turnSin
                wc = nc
            }
            if (voiced) markPulse(s, hop)
            next += hop
        }
    }

    private fun markPulse(s: Double, hop: Double) {
        val from = VoiceMath.ceil(s - 0.75 * hop).toInt()
        val to = VoiceMath.ceil(s + 0.25 * hop).toInt()
        val step = TWO_PI / hop
        for (n in from until to) pulse[n and mask] = PULSE_GAIN * (1 + PULSE_DEPTH * VoiceMath.cos(step * (n - s + 0.25 * hop)))
    }

    fun process(sample: Double, period: Double, ratio: Double): Double {
        input[written and mask] = sample
        lp1 += lpCoef * (sample - lp1)
        lp2 += lpCoef * (lp1 - lp2)
        smooth[written and mask] = lp2
        written += 1
        addMarks(period)
        placeGrains(ratio)
        val t = written - 1 - delay
        if (t < 0) return 0.0
        val slot = t and mask
        val shifted = output[slot]
        output[slot] = 0.0
        pulseOut = pulse[slot]
        pulse[slot] = 1.0
        val wanted = if (VoiceMath.abs(ratio - 1) > 2e-3 || VoiceMath.abs(range - 1) > 2e-3 || jitter > 0 || tremor > 0) 1.0 else 0.0
        val step = 1.0 / (2 * unvoiced)
        shifting = if (wanted > shifting) minOf(wanted, shifting + step) else maxOf(wanted, shifting - step)
        return shifting * shifted + (1 - shifting) * input[slot]
    }
}

internal class PeakLimiter(sampleRate: Double) {
    private val length = maxOf(8, VoiceMath.round(sampleRate * 0.0013).toInt())
    private val required = DoubleArray(length) { 1.0 }
    private val held = DoubleArray(length) { 1.0 }
    private val delayLine = DoubleArray(length)
    private var pos = 0
    private var heldSum = length.toDouble()
    private val release = 1 / (0.1 * sampleRate)
    private var smoothed = 1.0
    private var lowAge = length
    val latency = length - 1

    fun process(sample: Double): Double {
        val n = length
        val pos = this.pos
        val magnitude = VoiceMath.abs(sample)
        var minimum = 1.0
        if (magnitude > LIMIT_CEILING) {
            required[pos] = LIMIT_CEILING / magnitude
            lowAge = 0
        } else {
            required[pos] = 1.0
            if (lowAge < n) lowAge += 1
        }
        if (lowAge < n) {
            for (i in 0 until n) if (required[i] < minimum) minimum = required[i]
        }
        val released = smoothed + release * (1 - smoothed)
        smoothed = if (minimum < released) minimum else released
        heldSum += smoothed - held[pos]
        held[pos] = smoothed
        val next = if (pos + 1 == n) 0 else pos + 1
        val delayed = delayLine[next]
        delayLine[pos] = sample
        this.pos = next
        return delayed * minOf(1.0, heldSum / n)
    }
}
