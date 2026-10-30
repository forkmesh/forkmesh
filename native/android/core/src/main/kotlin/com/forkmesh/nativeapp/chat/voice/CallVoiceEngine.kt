package com.forkmesh.nativeapp.chat.voice

private const val MIN_WINDOWS = 8
private const val HIST_LOW_HZ = 50.0
private const val HIST_STEPS_PER_SEMITONE = 4
private const val HIST_BINS = 160
private const val HIST_HALF_LIFE = 3000
private const val SEED_WEIGHT = 60.0
private const val PITCH_WINDOW = 512
private const val PITCH_EVERY = 2
private const val PEAK_SPAN = 0.5
private const val LIFTER_PERIODS = 0.55
private const val ENVELOPE_GLIDE_S = 0.006
private const val VOICING_GLIDE = 0.25
private const val UNVOICED_WARP = 0.4
private const val WARP_KEEP_HZ = 5000.0
private const val WARP_RELEASE_HZ = 9000.0
private const val BREATH_LEVEL = 2.0
private const val UNVOICED_BREATH = 0.3
private const val BREATH_LOW_HZ = 800.0
private const val BREATH_RISE_HZ = 3000.0
private const val VOICED_HOLD = 10

internal class VoicePlan {
    var ratio = 1.0
    var formant = 1.0
    var brightness = 0.0
    var breath = 0.0
    var range = 1.0
    var jitter = 0.0
    var tremor = 0.0
    var mix = 0.0
}

internal object VoicePlanner {
    fun tractFromPitch(hz: Double): Double {
        val limits = CallVoiceCatalog.callerHzLimits
        val f = VoiceMath.clamp(if (hz.isNaN() || hz == 0.0) MALE_HZ else hz, limits.first, limits.second)
        val t = VoiceMath.log(f / MALE_HZ) / VoiceMath.log(FEMALE_HZ / MALE_HZ)
        return VoiceMath.clamp(1 + t * (FEMALE_TRACT - 1), CallVoiceCatalog.tractLimits.first, CallVoiceCatalog.tractLimits.second)
    }

    fun planInto(out: VoicePlan, preset: CallVoice?, callerHz: Double): VoicePlan {
        if (preset == null || !(preset.pitchHz > 0)) {
            out.ratio = 1.0
            out.formant = 1.0
            out.brightness = 0.0
            out.breath = 0.0
            out.range = 1.0
            out.jitter = 0.0
            out.tremor = 0.0
            out.mix = 0.0
            return out
        }
        val c = CallVoiceCatalog
        val caller = VoiceMath.clamp(if (callerHz.isNaN() || callerHz == 0.0) MALE_HZ else callerHz, c.callerHzLimits.first, c.callerHzLimits.second)
        out.ratio = VoiceMath.clamp(preset.pitchHz / caller, c.ratioLimits.first, c.ratioLimits.second)
        val tract = if (preset.tract.isNaN() || preset.tract == 0.0) 1.0 else preset.tract
        out.formant = VoiceMath.clamp(tract / tractFromPitch(caller), c.formantLimits.first, c.formantLimits.second)
        out.brightness = VoiceMath.clamp(zeroIfNaN(preset.brightness), -1.0, 1.0)
        out.breath = VoiceMath.clamp(zeroIfNaN(preset.breath), 0.0, 1.0)
        out.range = VoiceMath.clamp(if (preset.range.isFinite()) preset.range else 1.0, c.rangeLimits.first, c.rangeLimits.second)
        out.jitter = VoiceMath.clamp(zeroIfNaN(preset.jitter), c.jitterLimits.first, c.jitterLimits.second)
        out.tremor = VoiceMath.clamp(zeroIfNaN(preset.tremor), c.tremorLimits.first, c.tremorLimits.second)
        out.mix = 1.0
        return out
    }

    private fun zeroIfNaN(value: Double) = if (value.isNaN()) 0.0 else value
}

internal class VoiceEngine(private val sampleRate: Double, userPitchHz: Double = 0.0) {
    private val size = if (sampleRate > 32000) 1024 else 512
    private val hop = size / 8
    private val half = size / 2
    private val oversample = 8
    private val fft = RealFft(size)
    private val tail = size / 2
    private val window = DoubleArray(size)
    private val synthesis = DoubleArray(size)
    private var windowPower = 0.0
    private val olaScale = (size / 8).toDouble() / (size / 4).toDouble()
    private val inFifo = DoubleArray(size)
    private val outFifo = DoubleArray(size)
    private val accum = DoubleArray(size * 2)
    private var fifoPos = size - size / 8
    private val frameBuffer = DoubleArray(size)
    private val re = DoubleArray(size / 2 + 1)
    private val im = DoubleArray(size / 2 + 1)
    private val cre = DoubleArray(size / 2 + 1)
    private val cim = DoubleArray(size / 2 + 1)
    private val cepstrum = DoubleArray(size)
    private val lifter = maxOf(12, VoiceMath.round(sampleRate / 650).toInt())
    private val bins = size / 2 + 1
    private val anaMag = DoubleArray(bins)
    private val envelope = DoubleArray(bins)
    private val logEnvelope = DoubleArray(bins)
    private val peakLog = DoubleArray(bins)
    private var envelopeReady = false
    private val warpSource = DoubleArray(bins)
    private var warpFormant = 0.0
    private var voicing = 0.0
    private val airRe = DoubleArray(bins)
    private val airIm = DoubleArray(bins)
    private val airBuffer = DoubleArray(size)
    private val airAccum = DoubleArray(size * 2)
    private val airFifo = DoubleArray(size)
    private var airActive = false
    private var airTurn = false
    private val envelopeGlide = 1 - VoiceMath.exp(-hop / (ENVELOPE_GLIDE_S * sampleRate))
    private val gain = DoubleArray(bins) { 1.0 }
    private val noiseCos = DoubleArray(1024) { VoiceMath.cos((TWO_PI * it) / 1024) }
    private val noiseSin = DoubleArray(1024) { VoiceMath.sin((TWO_PI * it) / 1024) }
    private val tilt = DoubleArray(bins) { 1.0 }
    private val decimate = maxOf(1, VoiceMath.round(sampleRate / 12000).toInt())
    private val pitch = PitchDetector(sampleRate / decimate, PITCH_WINDOW)
    private val pitchRing = DoubleArray(PITCH_WINDOW)
    private val pitchScratch = DoubleArray(PITCH_WINDOW)
    private var pitchWrite = 0
    private var decAcc = 0.0
    private var decCount = 0
    private var framesSincePitch = 0
    private var userHz = 150.0
    private val pitchHist = DoubleArray(HIST_BINS)
    private var histWeight = 1.0
    private var histTotal = 0.0
    private val histGrowth = VoiceMath.pow(2.0, PITCH_EVERY.toDouble() / (4 * HIST_HALF_LIFE))
    var voiced = false
        private set
    var lastPitchHz = 0.0
        private set
    private val recentPitch = DoubleArray(3)
    private var voicedHold = 0
    private var shiftPitchHz = 0.0
    private val shifter = PitchShifter(sampleRate)
    private val limiter = PeakLimiter(sampleRate)
    private var preset: CallVoice? = null
    private val plan = VoicePlan()
    var ratio = 1.0
        private set
    var formant = 1.0
        private set
    private var brightness = 0.0
    private var tiltBrightness = 0.0
    private var breath = 0.0
    private var range = 1.0
    private var jitter = 0.0
    private var tremor = 0.0
    var mix = 0.0
        private set
    private var mixTarget = 0.0
    private val mixStep = 1 / (0.03 * sampleRate)
    private var agc = 1.0
    private var inPower = 0.0
    private var outPower = 0.0
    private var speechEnergy = 0.0
    private var noiseEnergy = 0.0
    private var denoiseAmount = 0.0
    private var hpB0 = 1.0
    private var hpB1 = 0.0
    private var hpB2 = 0.0
    private var hpA1 = 0.0
    private var hpA2 = 0.0
    private var hpX1 = 0.0
    private var hpX2 = 0.0
    private var hpY1 = 0.0
    private var hpY2 = 0.0
    private var noiseReady = false
    private var denoiseFrames = 0
    private var minFrames = 0
    private var minSlot = 0
    private val noisePower = DoubleArray(bins)
    private val trackPower = DoubleArray(bins)
    private val minPower = DoubleArray(bins)
    private val cleanPower = DoubleArray(bins)
    private val minHistory = DoubleArray(MIN_WINDOWS * bins)
    private var seed = 0x2545f491
    private val airWindow = DoubleArray(tail)
    private val dry: DoubleArray
    private var dryPos = 0
    private val airDelay = DoubleArray(shifter.latency)
    private var airPos = 0

    init {
        setUserPitch(userPitchHz)
        val rise = size - tail / 2
        val fall = size - tail
        for (i in 0 until size) {
            val hann = if (i < rise) {
                0.5 - 0.5 * VoiceMath.cos((Math.PI * i) / rise)
            } else {
                0.5 - 0.5 * VoiceMath.cos((TWO_PI * (i - fall)) / tail)
            }
            window[i] = VoiceMath.sqrt(hann)
            windowPower += hann / size
            synthesis[i] = if (i < fall) 0.0 else (0.5 - 0.5 * VoiceMath.cos((TWO_PI * (i - fall)) / tail)) / window[i]
        }
        for (i in 0 until tail) {
            airWindow[i] = VoiceMath.sin((Math.PI * (i + 0.5)) / tail) / VoiceMath.sqrt(windowPower)
        }
        dry = DoubleArray(tail + shifter.latency)
        setNoiseReduction(0.6)
        setVoice(null)
    }

    val latencySamples: Int get() = dry.size + limiter.latency

    fun setVoice(preset: CallVoice?) {
        this.preset = if (preset != null && preset.pitchHz > 0) preset else null
        mixTarget = if (this.preset != null) 1.0 else 0.0
    }

    fun setUserPitch(hz: Double) {
        if (!(hz > 50 && hz < 500)) return
        pitchHist.fill(0.0)
        histWeight = 1.0
        histTotal = 0.0
        addPitchReading(hz, SEED_WEIGHT)
    }

    private fun addPitchReading(hz: Double, weight: Double) {
        val position = (12 * HIST_STEPS_PER_SEMITONE) * VoiceMath.log2(hz / HIST_LOW_HZ)
        val bin = VoiceMath.clamp(VoiceMath.floor(position), 0.0, (HIST_BINS - 1).toDouble()).toInt()
        val amount = weight * histWeight
        pitchHist[bin] += amount
        histTotal += amount
        histWeight *= histGrowth
        if (histWeight > 1e6) {
            val scale = 1 / histWeight
            for (i in 0 until HIST_BINS) pitchHist[i] *= scale
            histTotal *= scale
            histWeight = 1.0
        }
        val half = histTotal / 2
        var sum = 0.0
        for (i in 0 until HIST_BINS) {
            val next = sum + pitchHist[i]
            if (next >= half) {
                val fraction = if (pitchHist[i] > 0) (half - sum) / pitchHist[i] else 0.5
                userHz = HIST_LOW_HZ * VoiceMath.pow(2.0, (i + fraction) / (12 * HIST_STEPS_PER_SEMITONE))
                return
            }
            sum = next
        }
    }

    fun userPitchHz(): Double = userHz

    fun setNoiseReduction(amount: Double) {
        val wanted = if (amount.isFinite()) VoiceMath.clamp(amount, 0.0, 1.0) else 0.0
        if (wanted > 0 && denoiseAmount <= 0) {
            noiseReady = false
            denoiseFrames = 0
            minFrames = 0
        }
        denoiseAmount = wanted
        val w0 = (TWO_PI * 70) / sampleRate
        val alpha = VoiceMath.sin(w0) / VoiceMath.sqrt(2.0)
        val a0 = 1 + alpha
        hpB0 = (1 + VoiceMath.cos(w0)) / 2 / a0
        hpB1 = -(1 + VoiceMath.cos(w0)) / a0
        hpB2 = hpB0
        hpA1 = (-2 * VoiceMath.cos(w0)) / a0
        hpA2 = (1 - alpha) / a0
    }

    private fun random(): Double {
        var x = seed
        x = x xor (x shl 13)
        x = x xor (x ushr 17)
        x = x xor (x shl 5)
        seed = x
        return (x.toLong() and 0xffffffffL).toDouble() / 4294967296.0
    }

    private fun updateTilt() {
        val tiltDb = brightness * 6
        val binHz = sampleRate / size
        for (k in 0..half) {
            val hz = maxOf(k * binHz, 60.0)
            tilt[k] = VoiceMath.pow(10.0, (tiltDb * VoiceMath.log2(hz / 1000)) / 20)
        }
        tiltBrightness = brightness
    }

    private fun trackPitch(sample: Double) {
        decAcc += sample
        decCount += 1
        if (decCount < decimate) return
        pitchRing[pitchWrite] = decAcc / decCount
        pitchWrite = (pitchWrite + 1) % pitchRing.size
        decAcc = 0.0
        decCount = 0
    }

    private fun analysePitch() {
        val n = pitchRing.size
        val write = pitchWrite
        for (i in 0 until n) {
            val j = write + i
            pitchScratch[i] = pitchRing[if (j < n) j else j - n]
        }
        val hz = pitch.detect(pitchScratch, if (voicedHold > 0) shiftPitchHz else 0.0)
        voiced = hz >= PITCH_LOW_HZ && hz <= PITCH_HIGH_HZ
        if (!voiced) {
            if (voicedHold > 0) {
                voicedHold -= 1
                if (voicedHold == 0) shiftPitchHz = 0.0
            }
            return
        }
        val recent = recentPitch
        if (voicedHold == 0) {
            recent[0] = hz
            recent[1] = hz
            recent[2] = hz
        }
        recent[0] = recent[1]
        recent[1] = recent[2]
        recent[2] = hz
        val a = recent[0]
        val b = recent[1]
        val c = recent[2]
        shiftPitchHz = maxOf(minOf(a, b), minOf(maxOf(a, b), c))
        voicedHold = VOICED_HOLD
        lastPitchHz = hz
        addPitchReading(shiftPitchHz, PITCH_EVERY / 4.0)
    }

    private fun steer() {
        val plan = VoicePlanner.planInto(plan, preset, userPitchHz())
        ratio += 0.25 * (plan.ratio - ratio)
        formant += 0.15 * (plan.formant - formant)
        brightness += 0.15 * (plan.brightness - brightness)
        breath += 0.15 * (plan.breath - breath)
        range += 0.15 * (plan.range - range)
        jitter += 0.15 * (plan.jitter - jitter)
        tremor += 0.15 * (plan.tremor - tremor)
        if (VoiceMath.abs(range - 1) < 1e-4) range = 1.0
        if (jitter < 1e-5) jitter = 0.0
        if (tremor < 1e-5) tremor = 0.0
        shifter.shape(range, sampleRate / userPitchHz(), jitter, tremor)
        if (VoiceMath.abs(brightness - tiltBrightness) > 2e-3) updateTilt()
    }

    fun process(samples: FloatArray, offset: Int, count: Int) {
        val n = size
        val latency = dry.size
        val filtering = denoiseAmount > 0
        for (i in offset until offset + count) {
            val raw = samples[i].toDouble()
            val sample = if (raw.isFinite()) raw else 0.0
            var changed = sample
            if (filtering) {
                changed = hpB0 * sample + hpB1 * hpX1 + hpB2 * hpX2 - hpA1 * hpY1 - hpA2 * hpY2
                hpX2 = hpX1
                hpX1 = sample
                hpY2 = hpY1
                hpY1 = changed
            }
            trackPitch(changed)
            inFifo[fifoPos] = changed
            val delayed = dry[dryPos]
            dry[dryPos] = sample
            dryPos = if (dryPos + 1 == latency) 0 else dryPos + 1
            if (mix != mixTarget) {
                mix = if (mixTarget > mix) minOf(mixTarget, mix + mixStep) else maxOf(mixTarget, mix - mixStep)
            }
            val filtered = agc * outFifo[fifoPos - (n - hop)]
            val period = if (shiftPitchHz > 0) sampleRate / shiftPitchHz else 0.0
            val air = airDelay[airPos]
            airDelay[airPos] = agc * airFifo[fifoPos - (n - hop)]
            airPos = if (airPos + 1 == airDelay.size) 0 else airPos + 1
            val shifted = shifter.process(filtered, period, ratio)
            val wet = shifted + air * shifter.pulseOut
            val mixed = mix * wet + (1 - mix) * delayed
            samples[i] = limiter.process(mixed).toFloat()
            fifoPos += 1
            if (fifoPos >= n) {
                fifoPos = n - hop
                frame()
                System.arraycopy(accum, 0, outFifo, 0, hop)
                System.arraycopy(accum, hop, accum, 0, n)
                accum.fill(0.0, n, n + hop)
                System.arraycopy(airAccum, 0, airFifo, 0, hop)
                System.arraycopy(airAccum, hop, airAccum, 0, n)
                airAccum.fill(0.0, n, n + hop)
                System.arraycopy(inFifo, hop, inFifo, 0, n - hop)
                levelMatch()
            }
        }
    }

    private fun levelMatch() {
        if (inPower < 1e-6 || outPower < 1e-9) return
        val wanted = VoiceMath.clamp(VoiceMath.sqrt(inPower / outPower), 0.5, 4.0)
        agc += 0.05 * (wanted - agc)
    }

    private fun denoise() {
        if (!noiseReady) {
            denoiseFrames += 1
            if (denoiseFrames < oversample) return
        }
        val framesPerSecond = sampleRate / hop
        val track = 1 - VoiceMath.exp(-1 / (0.02 * framesPerSecond))
        val windowFrames = maxOf(1, VoiceMath.floor(0.2 * framesPerSecond).toInt())
        val bias = 4.0
        val floorGain = VoiceMath.pow(10.0, (-18 * denoiseAmount) / 20)
        var newWindow = false
        if (noiseReady) {
            minFrames += 1
            newWindow = minFrames >= windowFrames
        }
        val finished = minSlot
        if (newWindow) {
            minFrames = 0
            minSlot = (minSlot + 1) % MIN_WINDOWS
        }
        val history = minHistory
        var speech = 0.0
        var noise = 0.0
        for (k in 0..half) {
            val power = anaMag[k] * anaMag[k]
            if (!noiseReady) {
                trackPower[k] = power
                for (w in 0 until MIN_WINDOWS) history[w * bins + k] = power
            } else {
                trackPower[k] += track * (power - trackPower[k])
            }
            val tracked = trackPower[k]
            if (newWindow || !noiseReady) {
                if (noiseReady) history[finished * bins + k] = minPower[k]
                minPower[k] = tracked
            } else if (tracked < minPower[k]) {
                minPower[k] = tracked
            }
            var minimum = minPower[k]
            for (w in 0 until MIN_WINDOWS) {
                val value = history[w * bins + k]
                if (value < minimum) minimum = value
            }
            val estimate = bias * minimum
            noisePower[k] = estimate
            val expected = estimate + 1e-12
            val posterior = power / expected
            val prior = maxOf((0.96 * cleanPower[k]) / expected + 0.04 * maxOf(posterior - 1, 0.0), 1e-3)
            val g = maxOf(prior / (1 + prior), floorGain)
            cleanPower[k] = g * g * power
            anaMag[k] *= g
            gain[k] = g
            speech += power
            noise += estimate
        }
        noiseReady = true
        speechEnergy = speech
        noiseEnergy = noise
    }

    private fun frame() {
        val n = size
        val buffer = frameBuffer
        framesSincePitch += 1
        if (framesSincePitch >= PITCH_EVERY) {
            framesSincePitch = 0
            analysePitch()
        }
        steer()
        for (i in 0 until n) {
            val v = inFifo[i] * window[i]
            val j = i + half
            buffer[if (j < n) j else j - n] = v
        }
        fft.forward(buffer, re, im)
        for (k in 0..half) anaMag[k] = VoiceMath.sqrt(re[k] * re[k] + im[k] * im[k])
        var speech = true
        if (denoiseAmount > 0) {
            denoise()
            speech = speechEnergy > 3 * noiseEnergy
        } else {
            gain.fill(1.0)
        }
        var spectrumIn = 0.0
        for (k in 0..half) spectrumIn += anaMag[k] * anaMag[k]
        updateEnvelope()
        voicing += VOICING_GLIDE * ((if (voiced) 1.0 else 0.0) - voicing)
        val depth = UNVOICED_WARP + (1 - UNVOICED_WARP) * voicing
        val formantNow = 1 + (formant - 1) * depth
        if (VoiceMath.abs(formantNow - warpFormant) > 1e-4) updateWarp(formantNow)
        var spectrumOut = 0.0
        for (k in 0..half) {
            val at = warpSource[k]
            val low = VoiceMath.floor(at).toInt()
            val env = if (low >= half) envelope[half] else envelope[low] + (envelope[low + 1] - envelope[low]) * (at - low)
            val factor = (gain[k] * env * tilt[k]) / (envelope[k] + EPS)
            val outRe = re[k] * factor
            val outIm = im[k] * factor
            re[k] = outRe
            im[k] = outIm
            spectrumOut += outRe * outRe + outIm * outIm
        }
        im[0] = 0.0
        im[half] = 0.0
        if (speech) {
            val scale = 2 / ((n * n).toDouble() * windowPower)
            val smoothing = 1 - VoiceMath.exp(-hop / (0.3 * sampleRate))
            inPower += smoothing * (spectrumIn * scale - inPower)
            outPower += smoothing * (spectrumOut * scale - outPower)
        }
        fft.inverse(re, im, buffer)
        val scale = olaScale
        val fall = n - tail
        for (i in fall until n) {
            val j = i + half
            accum[i - fall] += buffer[if (j < n) j else j - n] * synthesis[i] * scale
        }
        val breathNow = breath * (UNVOICED_BREATH + (1 - UNVOICED_BREATH) * voicing)
        airTurn = !airTurn
        if (airTurn && (breathNow > 1e-3 || airActive)) breathe(breathNow)
    }

    private fun updateEnvelope() {
        val n = size
        val peaks = peakLog
        for (k in 0..half) {
            peaks[k] = VoiceMath.log(anaMag[k] + EPS)
            cim[k] = 0.0
        }
        val localHz = if (shiftPitchHz > 0) shiftPitchHz else userHz
        val reach = maxOf(1, VoiceMath.round((PEAK_SPAN * localHz * n) / sampleRate).toInt())
        for (k in 0..half) {
            var top = peaks[k]
            val from = if (k > reach) k - reach else 0
            val to = if (k + reach < half) k + reach else half
            for (j in from..to) if (peaks[j] > top) top = peaks[j]
            cre[k] = top
        }
        fft.inverse(cre, cim, cepstrum)
        val lifterNow = VoiceMath.round(VoiceMath.clamp((LIFTER_PERIODS * sampleRate) / userHz, lifter.toDouble(), n / 4.0)).toInt()
        cepstrum.fill(0.0, lifterNow, n - lifterNow + 1)
        fft.forward(cepstrum, cre, cim)
        val settle = if (envelopeReady) envelopeGlide else 1.0
        for (k in 0..half) {
            logEnvelope[k] += settle * (cre[k] - logEnvelope[k])
            envelope[k] = VoiceMath.exp(logEnvelope[k])
        }
        envelopeReady = true
    }

    private fun updateWarp(formant: Double) {
        val binHz = sampleRate / size
        val top = (half - 1) * binHz
        val keep = minOf(WARP_KEEP_HZ, 0.5 * top)
        val release = minOf(WARP_RELEASE_HZ, top)
        val from = keep / formant
        val slope = (release - from) / (release - keep)
        for (k in 0..half) {
            val hz = k * binHz
            val at = if (hz <= keep) hz / formant else if (hz < release) from + (hz - keep) * slope else hz
            warpSource[k] = at / binHz
        }
        warpFormant = formant
    }

    private fun breathe(breath: Double) {
        val n = size
        val binHz = sampleRate / n
        val level = breath * BREATH_LEVEL
        for (k in 0..half) {
            val at = warpSource[k]
            val low = VoiceMath.floor(at).toInt()
            val env = if (low >= half) envelope[half] else envelope[low] + (envelope[low + 1] - envelope[low]) * (at - low)
            val amount = level * env * tilt[k] * VoiceMath.clamp((k * binHz - BREATH_LOW_HZ) / BREATH_RISE_HZ, 0.0, 1.0)
            val slot = (random() * 1024).toInt()
            airRe[k] = amount * noiseCos[slot]
            airIm[k] = amount * noiseSin[slot]
        }
        airIm[0] = 0.0
        airIm[half] = 0.0
        fft.inverse(airRe, airIm, airBuffer)
        for (i in 0 until tail) airAccum[i] += airBuffer[i] * airWindow[i]
        airActive = breath > 1e-3
    }
}
