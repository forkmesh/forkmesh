import Foundation

private let minWindows = 8
private let histLowHz = 50.0
private let histStepsPerSemitone = 4
private let histBins = 160
private let histHalfLife = 3000
private let seedWeight = 60.0
private let pitchWindow = 512
private let pitchEvery = 2
private let peakSpan = 0.5
private let lifterPeriods = 0.55
private let envelopeGlideSeconds = 0.006
private let voicingGlide = 0.25
private let unvoicedWarp = 0.4
private let warpKeepHz = 5000.0
private let warpReleaseHz = 9000.0
private let breathLevel = 2.0
private let unvoicedBreath = 0.3
private let breathLowHz = 800.0
private let breathRiseHz = 3000.0
private let voicedHoldFrames = 10

struct VoicePlan {
    var ratio = 1.0
    var formant = 1.0
    var brightness = 0.0
    var breath = 0.0
    var range = 1.0
    var jitter = 0.0
    var tremor = 0.0
    var mix = 0.0
}

enum VoicePlanner {
    private static func orDefault(_ value: Double, _ fallback: Double) -> Double {
        value.isNaN || value == 0 ? fallback : value
    }

    private static func clamp(_ value: Double, _ limits: ClosedRange<Double>) -> Double {
        voiceClamp(value, limits.lowerBound, limits.upperBound)
    }

    static func tractFromPitch(_ hz: Double) -> Double {
        let f = clamp(orDefault(hz, voiceMaleHz), CallVoiceCatalog.callerHzLimits)
        let t = log(f / voiceMaleHz) / log(voiceFemaleHz / voiceMaleHz)
        return clamp(1 + t * (voiceFemaleTract - 1), CallVoiceCatalog.tractLimits)
    }

    static func plan(_ preset: CallVoice?, callerHz: Double) -> VoicePlan {
        var out = VoicePlan()
        guard let preset, preset.pitchHz > 0 else { return out }
        let caller = clamp(orDefault(callerHz, voiceMaleHz), CallVoiceCatalog.callerHzLimits)
        out.ratio = clamp(preset.pitchHz / caller, CallVoiceCatalog.ratioLimits)
        out.formant = clamp(orDefault(preset.tract, 1) / tractFromPitch(caller), CallVoiceCatalog.formantLimits)
        out.brightness = voiceClamp(orDefault(preset.brightness, 0), -1, 1)
        out.breath = voiceClamp(orDefault(preset.breath, 0), 0, 1)
        out.range = clamp(preset.range.isFinite ? preset.range : 1, CallVoiceCatalog.rangeLimits)
        out.jitter = clamp(orDefault(preset.jitter, 0), CallVoiceCatalog.jitterLimits)
        out.tremor = clamp(orDefault(preset.tremor, 0), CallVoiceCatalog.tremorLimits)
        out.mix = 1
        return out
    }
}

final class VoiceEngine {
    private let sampleRate: Double
    private let size: Int
    private let hop: Int
    private let half: Int
    private let bins: Int
    private let oversample = 8
    private let fft: VoiceRealFft
    private let tail: Int
    private let window: UnsafeMutablePointer<Double>
    private let synthesis: UnsafeMutablePointer<Double>
    private var windowPower = 0.0
    private let olaScale: Double
    private let inFifo: UnsafeMutablePointer<Double>
    private let outFifo: UnsafeMutablePointer<Double>
    private let accum: UnsafeMutablePointer<Double>
    private var fifoPos: Int
    private let frameBuffer: UnsafeMutablePointer<Double>
    private let re: UnsafeMutablePointer<Double>
    private let im: UnsafeMutablePointer<Double>
    private let cre: UnsafeMutablePointer<Double>
    private let cim: UnsafeMutablePointer<Double>
    private let cepstrum: UnsafeMutablePointer<Double>
    private let lifter: Int
    private let anaMag: UnsafeMutablePointer<Double>
    private let envelope: UnsafeMutablePointer<Double>
    private let logEnvelope: UnsafeMutablePointer<Double>
    private let peakLog: UnsafeMutablePointer<Double>
    private var envelopeReady = false
    private let warpSource: UnsafeMutablePointer<Double>
    private var warpFormant = 0.0
    private var voicing = 0.0
    private let airRe: UnsafeMutablePointer<Double>
    private let airIm: UnsafeMutablePointer<Double>
    private let airBuffer: UnsafeMutablePointer<Double>
    private let airAccum: UnsafeMutablePointer<Double>
    private let airFifo: UnsafeMutablePointer<Double>
    private var airActive = false
    private var airTurn = false
    private let envelopeGlide: Double
    private let gain: UnsafeMutablePointer<Double>
    private let noiseCos: UnsafeMutablePointer<Double>
    private let noiseSin: UnsafeMutablePointer<Double>
    private let tilt: UnsafeMutablePointer<Double>
    private let decimate: Int
    private let pitch: VoicePitchDetector
    private let pitchRing: UnsafeMutablePointer<Double>
    private let pitchScratch: UnsafeMutablePointer<Double>
    private var pitchWrite = 0
    private var decAcc = 0.0
    private var decCount = 0
    private var framesSincePitch = 0
    private var userHz = 150.0
    private let pitchHist: UnsafeMutablePointer<Double>
    private var histWeight = 1.0
    private var histTotal = 0.0
    private let histGrowth: Double
    private(set) var voiced = false
    private(set) var lastPitchHz = 0.0
    private var recentPitch: (Double, Double, Double) = (0, 0, 0)
    private var voicedHold = 0
    private var shiftPitchHz = 0.0
    private let shifter: VoicePitchShifter
    private let limiter: VoicePeakLimiter
    private var preset: CallVoice?
    private(set) var ratio = 1.0
    private(set) var formant = 1.0
    private var brightness = 0.0
    private var tiltBrightness = 0.0
    private var breath = 0.0
    private var range = 1.0
    private var jitter = 0.0
    private var tremor = 0.0
    private(set) var mix = 0.0
    private var mixTarget = 0.0
    private let mixStep: Double
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
    private let noisePower: UnsafeMutablePointer<Double>
    private let trackPower: UnsafeMutablePointer<Double>
    private let minPower: UnsafeMutablePointer<Double>
    private let cleanPower: UnsafeMutablePointer<Double>
    private let minHistory: UnsafeMutablePointer<Double>
    private var seed: UInt32 = 0x2545f491
    private let airWindow: UnsafeMutablePointer<Double>
    private let dry: UnsafeMutablePointer<Double>
    private let dryLength: Int
    private var dryPos = 0
    private let airDelay: UnsafeMutablePointer<Double>
    private let airDelayLength: Int
    private var airPos = 0

    init(sampleRate: Double, userPitchHz: Double = 0) {
        let size = sampleRate > 32000 ? 1024 : 512
        let hop = size / 8
        let bins = size / 2 + 1
        let tail = size / 2
        let shifter = VoicePitchShifter(sampleRate: sampleRate)
        let decimate = max(1, Int(voiceRound(sampleRate / 12000)))
        self.sampleRate = sampleRate
        self.size = size
        self.hop = hop
        half = size / 2
        self.bins = bins
        fft = VoiceRealFft(size)
        self.tail = tail
        window = voiceBuffer(size)
        synthesis = voiceBuffer(size)
        olaScale = Double(size / 8) / Double(size / 4)
        inFifo = voiceBuffer(size)
        outFifo = voiceBuffer(size)
        accum = voiceBuffer(size * 2)
        fifoPos = size - size / 8
        frameBuffer = voiceBuffer(size)
        re = voiceBuffer(bins)
        im = voiceBuffer(bins)
        cre = voiceBuffer(bins)
        cim = voiceBuffer(bins)
        cepstrum = voiceBuffer(size)
        lifter = max(12, Int(voiceRound(sampleRate / 650)))
        anaMag = voiceBuffer(bins)
        envelope = voiceBuffer(bins)
        logEnvelope = voiceBuffer(bins)
        peakLog = voiceBuffer(bins)
        warpSource = voiceBuffer(bins)
        airRe = voiceBuffer(bins)
        airIm = voiceBuffer(bins)
        airBuffer = voiceBuffer(size)
        airAccum = voiceBuffer(size * 2)
        airFifo = voiceBuffer(size)
        envelopeGlide = 1 - exp(-Double(hop) / (envelopeGlideSeconds * sampleRate))
        gain = voiceBuffer(bins, 1)
        noiseCos = voiceBuffer(1024)
        noiseSin = voiceBuffer(1024)
        tilt = voiceBuffer(bins, 1)
        self.decimate = decimate
        pitch = VoicePitchDetector(sampleRate: sampleRate / Double(decimate), size: pitchWindow)
        pitchRing = voiceBuffer(pitchWindow)
        pitchScratch = voiceBuffer(pitchWindow)
        pitchHist = voiceBuffer(histBins)
        histGrowth = pow(2, Double(pitchEvery) / Double(4 * histHalfLife))
        self.shifter = shifter
        limiter = VoicePeakLimiter(sampleRate: sampleRate)
        mixStep = 1 / (0.03 * sampleRate)
        noisePower = voiceBuffer(bins)
        trackPower = voiceBuffer(bins)
        minPower = voiceBuffer(bins)
        cleanPower = voiceBuffer(bins)
        minHistory = voiceBuffer(minWindows * bins)
        airWindow = voiceBuffer(tail)
        dryLength = tail + shifter.latency
        dry = voiceBuffer(tail + shifter.latency)
        airDelayLength = shifter.latency
        airDelay = voiceBuffer(shifter.latency)
        for i in 0..<1024 {
            noiseCos[i] = cos((voiceTwoPi * Double(i)) / 1024)
            noiseSin[i] = sin((voiceTwoPi * Double(i)) / 1024)
        }
        setUserPitch(userPitchHz)
        let rise = size - tail / 2
        let fall = size - tail
        for i in 0..<size {
            let hann = i < rise
                ? 0.5 - 0.5 * cos((Double.pi * Double(i)) / Double(rise))
                : 0.5 - 0.5 * cos((voiceTwoPi * Double(i - fall)) / Double(tail))
            window[i] = sqrt(hann)
            windowPower += hann / Double(size)
            synthesis[i] = i < fall ? 0 : (0.5 - 0.5 * cos((voiceTwoPi * Double(i - fall)) / Double(tail))) / window[i]
        }
        for i in 0..<tail {
            airWindow[i] = sin((Double.pi * (Double(i) + 0.5)) / Double(tail)) / sqrt(windowPower)
        }
        setNoiseReduction(0.6)
        setVoice(nil)
    }

    deinit {
        for pointer in [window, synthesis, inFifo, outFifo, accum, frameBuffer, re, im, cre, cim, cepstrum, anaMag,
                        envelope, logEnvelope, peakLog, warpSource, airRe, airIm, airBuffer, airAccum, airFifo, gain,
                        noiseCos, noiseSin, tilt, pitchRing, pitchScratch, pitchHist, noisePower, trackPower, minPower,
                        cleanPower, minHistory, airWindow, dry, airDelay] {
            pointer.deallocate()
        }
    }

    var latencySamples: Int { dryLength + limiter.latency }

    func setVoice(_ preset: CallVoice?) {
        if let preset, preset.pitchHz > 0 {
            self.preset = preset
        } else {
            self.preset = nil
        }
        mixTarget = self.preset == nil ? 0 : 1
    }

    func setUserPitch(_ hz: Double) {
        guard hz > 50 && hz < 500 else { return }
        pitchHist.update(repeating: 0, count: histBins)
        histWeight = 1
        histTotal = 0
        addPitchReading(hz, weight: seedWeight)
    }

    private func addPitchReading(_ hz: Double, weight: Double) {
        let position = Double(12 * histStepsPerSemitone) * log2(hz / histLowHz)
        let bin = Int(voiceClamp(floor(position), 0, Double(histBins - 1)))
        let amount = weight * histWeight
        pitchHist[bin] += amount
        histTotal += amount
        histWeight *= histGrowth
        if histWeight > 1e6 {
            let scale = 1 / histWeight
            for i in 0..<histBins { pitchHist[i] *= scale }
            histTotal *= scale
            histWeight = 1
        }
        let middle = histTotal / 2
        var sum = 0.0
        for i in 0..<histBins {
            let next = sum + pitchHist[i]
            if next >= middle {
                let fraction = pitchHist[i] > 0 ? (middle - sum) / pitchHist[i] : 0.5
                userHz = histLowHz * pow(2, (Double(i) + fraction) / Double(12 * histStepsPerSemitone))
                return
            }
            sum = next
        }
    }

    var userPitchHz: Double { userHz }

    func setNoiseReduction(_ amount: Double) {
        let wanted = amount.isFinite ? voiceClamp(amount, 0, 1) : 0
        if wanted > 0 && denoiseAmount <= 0 {
            noiseReady = false
            denoiseFrames = 0
            minFrames = 0
        }
        denoiseAmount = wanted
        let w0 = (voiceTwoPi * 70) / sampleRate
        let alpha = sin(w0) / 2.0.squareRoot()
        let a0 = 1 + alpha
        hpB0 = (1 + cos(w0)) / 2 / a0
        hpB1 = -(1 + cos(w0)) / a0
        hpB2 = hpB0
        hpA1 = (-2 * cos(w0)) / a0
        hpA2 = (1 - alpha) / a0
    }

    private func random() -> Double {
        var x = seed
        x ^= x << 13
        x ^= x >> 17
        x ^= x << 5
        seed = x
        return Double(x) / 4294967296
    }

    private func updateTilt() {
        let tiltDb = brightness * 6
        let binHz = sampleRate / Double(size)
        for k in 0...half {
            let hz = max(Double(k) * binHz, 60)
            tilt[k] = pow(10, (tiltDb * log2(hz / 1000)) / 20)
        }
        tiltBrightness = brightness
    }

    private func trackPitch(_ sample: Double) {
        decAcc += sample
        decCount += 1
        if decCount < decimate { return }
        pitchRing[pitchWrite] = decAcc / Double(decCount)
        pitchWrite = (pitchWrite + 1) % pitchWindow
        decAcc = 0
        decCount = 0
    }

    private func analysePitch() {
        let n = pitchWindow
        let write = pitchWrite
        for i in 0..<n {
            let j = write + i
            pitchScratch[i] = pitchRing[j < n ? j : j - n]
        }
        let hz = pitch.detect(pitchScratch, previousHz: voicedHold > 0 ? shiftPitchHz : 0)
        voiced = hz >= voicePitchLowHz && hz <= voicePitchHighHz
        if !voiced {
            if voicedHold > 0 {
                voicedHold -= 1
                if voicedHold == 0 { shiftPitchHz = 0 }
            }
            return
        }
        if voicedHold == 0 { recentPitch = (hz, hz, hz) }
        recentPitch = (recentPitch.1, recentPitch.2, hz)
        let (a, b, c) = recentPitch
        shiftPitchHz = max(min(a, b), min(max(a, b), c))
        voicedHold = voicedHoldFrames
        lastPitchHz = hz
        addPitchReading(shiftPitchHz, weight: Double(pitchEvery) / 4)
    }

    private func steer() {
        let plan = VoicePlanner.plan(preset, callerHz: userHz)
        ratio += 0.25 * (plan.ratio - ratio)
        formant += 0.15 * (plan.formant - formant)
        brightness += 0.15 * (plan.brightness - brightness)
        breath += 0.15 * (plan.breath - breath)
        range += 0.15 * (plan.range - range)
        jitter += 0.15 * (plan.jitter - jitter)
        tremor += 0.15 * (plan.tremor - tremor)
        if abs(range - 1) < 1e-4 { range = 1 }
        if jitter < 1e-5 { jitter = 0 }
        if tremor < 1e-5 { tremor = 0 }
        shifter.shape(range: range, centrePeriod: sampleRate / userHz, jitter: jitter, tremor: tremor)
        if abs(brightness - tiltBrightness) > 2e-3 { updateTilt() }
    }

    func process(_ samples: UnsafeMutableBufferPointer<Float>) {
        let n = size
        let filtering = denoiseAmount > 0
        for i in 0..<samples.count {
            let raw = Double(samples[i])
            let sample = raw.isFinite ? raw : 0
            var changed = sample
            if filtering {
                changed = hpB0 * sample + hpB1 * hpX1 + hpB2 * hpX2 - hpA1 * hpY1 - hpA2 * hpY2
                hpX2 = hpX1
                hpX1 = sample
                hpY2 = hpY1
                hpY1 = changed
            }
            trackPitch(changed)
            inFifo[fifoPos] = changed
            let delayed = dry[dryPos]
            dry[dryPos] = sample
            dryPos = dryPos + 1 == dryLength ? 0 : dryPos + 1
            if mix != mixTarget {
                mix = mixTarget > mix ? min(mixTarget, mix + mixStep) : max(mixTarget, mix - mixStep)
            }
            let filtered = agc * outFifo[fifoPos - (n - hop)]
            let period = shiftPitchHz > 0 ? sampleRate / shiftPitchHz : 0
            let air = airDelay[airPos]
            airDelay[airPos] = agc * airFifo[fifoPos - (n - hop)]
            airPos = airPos + 1 == airDelayLength ? 0 : airPos + 1
            let shifted = shifter.process(filtered, period: period, ratio: ratio)
            let wet = shifted + air * shifter.pulseOut
            let mixed = mix * wet + (1 - mix) * delayed
            samples[i] = Float(limiter.process(mixed))
            fifoPos += 1
            if fifoPos >= n {
                fifoPos = n - hop
                frame()
                for k in 0..<hop { outFifo[k] = accum[k] }
                for k in 0..<n { accum[k] = accum[k + hop] }
                for k in n..<(n + hop) { accum[k] = 0 }
                for k in 0..<hop { airFifo[k] = airAccum[k] }
                for k in 0..<n { airAccum[k] = airAccum[k + hop] }
                for k in n..<(n + hop) { airAccum[k] = 0 }
                for k in 0..<(n - hop) { inFifo[k] = inFifo[k + hop] }
                levelMatch()
            }
        }
    }

    private func levelMatch() {
        if inPower < 1e-6 || outPower < 1e-9 { return }
        let wanted = voiceClamp(sqrt(inPower / outPower), 0.5, 4)
        agc += 0.05 * (wanted - agc)
    }

    private func denoise() {
        if !noiseReady {
            denoiseFrames += 1
            if denoiseFrames < oversample { return }
        }
        let framesPerSecond = sampleRate / Double(hop)
        let track = 1 - exp(-1 / (0.02 * framesPerSecond))
        let windowFrames = max(1, Int(floor(0.2 * framesPerSecond)))
        let bias = 4.0
        let floorGain = pow(10, (-18 * denoiseAmount) / 20)
        var newWindow = false
        if noiseReady {
            minFrames += 1
            newWindow = minFrames >= windowFrames
        }
        let finished = minSlot
        if newWindow {
            minFrames = 0
            minSlot = (minSlot + 1) % minWindows
        }
        let history = minHistory
        var speech = 0.0
        var noise = 0.0
        for k in 0...half {
            let power = anaMag[k] * anaMag[k]
            if !noiseReady {
                trackPower[k] = power
                for w in 0..<minWindows { history[w * bins + k] = power }
            } else {
                trackPower[k] += track * (power - trackPower[k])
            }
            let tracked = trackPower[k]
            if newWindow || !noiseReady {
                if noiseReady { history[finished * bins + k] = minPower[k] }
                minPower[k] = tracked
            } else if tracked < minPower[k] {
                minPower[k] = tracked
            }
            var minimum = minPower[k]
            for w in 0..<minWindows {
                let value = history[w * bins + k]
                if value < minimum { minimum = value }
            }
            let estimate = bias * minimum
            noisePower[k] = estimate
            let expected = estimate + 1e-12
            let posterior = power / expected
            let prior = max((0.96 * cleanPower[k]) / expected + 0.04 * max(posterior - 1, 0), 1e-3)
            let g = max(prior / (1 + prior), floorGain)
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

    private func frame() {
        let n = size
        let buffer = frameBuffer
        framesSincePitch += 1
        if framesSincePitch >= pitchEvery {
            framesSincePitch = 0
            analysePitch()
        }
        steer()
        for i in 0..<n {
            let v = inFifo[i] * window[i]
            let j = i + half
            buffer[j < n ? j : j - n] = v
        }
        fft.forward(buffer, re, im)
        for k in 0...half { anaMag[k] = sqrt(re[k] * re[k] + im[k] * im[k]) }
        var speech = true
        if denoiseAmount > 0 {
            denoise()
            speech = speechEnergy > 3 * noiseEnergy
        } else {
            gain.update(repeating: 1, count: bins)
        }
        var spectrumIn = 0.0
        for k in 0...half { spectrumIn += anaMag[k] * anaMag[k] }
        updateEnvelope()
        voicing += voicingGlide * ((voiced ? 1 : 0) - voicing)
        let depth = unvoicedWarp + (1 - unvoicedWarp) * voicing
        let formantNow = 1 + (formant - 1) * depth
        if abs(formantNow - warpFormant) > 1e-4 { updateWarp(formantNow) }
        var spectrumOut = 0.0
        for k in 0...half {
            let at = warpSource[k]
            let low = Int(floor(at))
            let env = low >= half ? envelope[half] : envelope[low] + (envelope[low + 1] - envelope[low]) * (at - Double(low))
            let factor = (gain[k] * env * tilt[k]) / (envelope[k] + voiceEps)
            let outRe = re[k] * factor
            let outIm = im[k] * factor
            re[k] = outRe
            im[k] = outIm
            spectrumOut += outRe * outRe + outIm * outIm
        }
        im[0] = 0
        im[half] = 0
        if speech {
            let scale = 2 / (Double(n * n) * windowPower)
            let smoothing = 1 - exp(-Double(hop) / (0.3 * sampleRate))
            inPower += smoothing * (spectrumIn * scale - inPower)
            outPower += smoothing * (spectrumOut * scale - outPower)
        }
        fft.inverse(re, im, buffer)
        let scale = olaScale
        let fall = n - tail
        for i in fall..<n {
            let j = i + half
            accum[i - fall] += buffer[j < n ? j : j - n] * synthesis[i] * scale
        }
        let breathNow = breath * (unvoicedBreath + (1 - unvoicedBreath) * voicing)
        airTurn.toggle()
        if airTurn && (breathNow > 1e-3 || airActive) { breathe(breathNow) }
    }

    private func updateEnvelope() {
        let n = size
        let peaks = peakLog
        for k in 0...half {
            peaks[k] = log(anaMag[k] + voiceEps)
            cim[k] = 0
        }
        let localHz = shiftPitchHz > 0 ? shiftPitchHz : userHz
        let reach = max(1, Int(voiceRound((peakSpan * localHz * Double(n)) / sampleRate)))
        for k in 0...half {
            var top = peaks[k]
            let from = k > reach ? k - reach : 0
            let to = k + reach < half ? k + reach : half
            for j in from...to where peaks[j] > top { top = peaks[j] }
            cre[k] = top
        }
        fft.inverse(cre, cim, cepstrum)
        let lifterNow = Int(voiceRound(voiceClamp((lifterPeriods * sampleRate) / userHz, Double(lifter), Double(n) / 4)))
        if n - lifterNow + 1 > lifterNow {
            (cepstrum + lifterNow).update(repeating: 0, count: n - lifterNow + 1 - lifterNow)
        }
        fft.forward(cepstrum, cre, cim)
        let settle = envelopeReady ? envelopeGlide : 1
        for k in 0...half {
            logEnvelope[k] += settle * (cre[k] - logEnvelope[k])
            envelope[k] = exp(logEnvelope[k])
        }
        envelopeReady = true
    }

    private func updateWarp(_ formant: Double) {
        let binHz = sampleRate / Double(size)
        let top = Double(half - 1) * binHz
        let keep = min(warpKeepHz, 0.5 * top)
        let release = min(warpReleaseHz, top)
        let from = keep / formant
        let slope = (release - from) / (release - keep)
        for k in 0...half {
            let hz = Double(k) * binHz
            let at = hz <= keep ? hz / formant : hz < release ? from + (hz - keep) * slope : hz
            warpSource[k] = at / binHz
        }
        warpFormant = formant
    }

    private func breathe(_ breath: Double) {
        let n = size
        let binHz = sampleRate / Double(n)
        let level = breath * breathLevel
        for k in 0...half {
            let at = warpSource[k]
            let low = Int(floor(at))
            let env = low >= half ? envelope[half] : envelope[low] + (envelope[low + 1] - envelope[low]) * (at - Double(low))
            let amount = level * env * tilt[k] * voiceClamp((Double(k) * binHz - breathLowHz) / breathRiseHz, 0, 1)
            let slot = Int(random() * 1024)
            airRe[k] = amount * noiseCos[slot]
            airIm[k] = amount * noiseSin[slot]
        }
        airIm[0] = 0
        airIm[half] = 0
        fft.inverse(airRe, airIm, airBuffer)
        for i in 0..<tail { airAccum[i] += airBuffer[i] * airWindow[i] }
        airActive = breath > 1e-3
    }
}
