import Foundation

let voiceTwoPi = 2 * Double.pi
let voiceEps = 1e-9
let voiceLimitCeiling = 0.95
let voiceMaleHz = 115.0
let voiceFemaleHz = 205.0
let voiceFemaleTract = 1.17
let voiceVoicingEnter = 0.55
let voiceVoicingStay = 0.45
let voiceAnchorBias = 0.3
let voiceLocalVoicing = 0.6
let voicePitchLowHz = 50.0
let voicePitchHighHz = 420.0
let voicePulseDepth = 0.7
let voicePulseGain = 1 / sqrt(1 + (voicePulseDepth * voicePulseDepth) / 2)
let voiceTremorHz = 5.5

@inline(__always) func voiceClamp(_ value: Double, _ low: Double, _ high: Double) -> Double {
    value < low ? low : value > high ? high : value
}

@inline(__always) func voiceRound(_ x: Double) -> Double {
    let f = floor(x)
    return x - f >= 0.5 ? f + 1 : f
}

func voiceBuffer(_ count: Int, _ value: Double = 0) -> UnsafeMutablePointer<Double> {
    let pointer = UnsafeMutablePointer<Double>.allocate(capacity: max(1, count))
    pointer.initialize(repeating: value, count: max(1, count))
    return pointer
}

final class VoiceFft {
    let size: Int
    private let cosTable: UnsafeMutablePointer<Double>
    private let sinTable: UnsafeMutablePointer<Double>
    private let reverse: UnsafeMutablePointer<Int>

    init(_ size: Int) {
        self.size = size
        cosTable = voiceBuffer(size / 2)
        sinTable = voiceBuffer(size / 2)
        reverse = UnsafeMutablePointer<Int>.allocate(capacity: size)
        reverse.initialize(repeating: 0, count: size)
        for i in 0..<(size / 2) {
            cosTable[i] = cos((voiceTwoPi * Double(i)) / Double(size))
            sinTable[i] = sin((voiceTwoPi * Double(i)) / Double(size))
        }
        let bits = size.trailingZeroBitCount
        for i in 0..<size {
            var r = 0
            for b in 0..<bits { r |= ((i >> b) & 1) << (bits - 1 - b) }
            reverse[i] = r
        }
    }

    deinit {
        cosTable.deallocate()
        sinTable.deallocate()
        reverse.deallocate()
    }

    func transform(_ re: UnsafeMutablePointer<Double>, _ im: UnsafeMutablePointer<Double>, inverse: Bool) {
        let n = size
        for i in 0..<n {
            let j = reverse[i]
            if j > i {
                var t = re[i]
                re[i] = re[j]
                re[j] = t
                t = im[i]
                im[i] = im[j]
                im[j] = t
            }
        }
        let sign: Double = inverse ? 1 : -1
        var len = 2
        while len <= n {
            let half = len >> 1
            let step = n / len
            var start = 0
            while start < n {
                for k in 0..<half {
                    let wr = cosTable[k * step]
                    let wi = sign * sinTable[k * step]
                    let a = start + k
                    let b = a + half
                    let xr = re[b] * wr - im[b] * wi
                    let xi = re[b] * wi + im[b] * wr
                    re[b] = re[a] - xr
                    im[b] = im[a] - xi
                    re[a] += xr
                    im[a] += xi
                }
                start += len
            }
            len <<= 1
        }
        if inverse {
            let scale = 1 / Double(n)
            for i in 0..<n {
                re[i] *= scale
                im[i] *= scale
            }
        }
    }
}

final class VoiceRealFft {
    let size: Int
    private let half: Int
    private let fft: VoiceFft
    private let zr: UnsafeMutablePointer<Double>
    private let zi: UnsafeMutablePointer<Double>
    private let wr: UnsafeMutablePointer<Double>
    private let wi: UnsafeMutablePointer<Double>

    init(_ size: Int) {
        self.size = size
        half = size / 2
        fft = VoiceFft(size / 2)
        zr = voiceBuffer(size / 2)
        zi = voiceBuffer(size / 2)
        wr = voiceBuffer(size / 2)
        wi = voiceBuffer(size / 2)
        for k in 0..<(size / 2) {
            wr[k] = cos((voiceTwoPi * Double(k)) / Double(size))
            wi[k] = -sin((voiceTwoPi * Double(k)) / Double(size))
        }
    }

    deinit {
        zr.deallocate()
        zi.deallocate()
        wr.deallocate()
        wi.deallocate()
    }

    func forward(_ x: UnsafeMutablePointer<Double>, _ re: UnsafeMutablePointer<Double>, _ im: UnsafeMutablePointer<Double>) {
        let h = half
        for i in 0..<h {
            zr[i] = x[2 * i]
            zi[i] = x[2 * i + 1]
        }
        fft.transform(zr, zi, inverse: false)
        re[0] = zr[0] + zi[0]
        im[0] = 0
        re[h] = zr[0] - zi[0]
        im[h] = 0
        for k in 1..<h {
            let ar = zr[k]
            let ai = zi[k]
            let br = zr[h - k]
            let bi = -zi[h - k]
            let er = 0.5 * (ar + br)
            let ei = 0.5 * (ai + bi)
            let or = 0.5 * (ai - bi)
            let oi = -0.5 * (ar - br)
            let cr = wr[k]
            let ci = wi[k]
            re[k] = er + cr * or - ci * oi
            im[k] = ei + cr * oi + ci * or
        }
    }

    func inverse(_ re: UnsafeMutablePointer<Double>, _ im: UnsafeMutablePointer<Double>, _ x: UnsafeMutablePointer<Double>) {
        let h = half
        for k in 0..<h {
            let ar = re[k]
            let ai = im[k]
            let br = re[h - k]
            let bi = -im[h - k]
            let er = 0.5 * (ar + br)
            let ei = 0.5 * (ai + bi)
            let dr = 0.5 * (ar - br)
            let di = 0.5 * (ai - bi)
            let cr = wr[k]
            let ci = -wi[k]
            let or = dr * cr - di * ci
            let oi = dr * ci + di * cr
            zr[k] = er - oi
            zi[k] = ei + or
        }
        fft.transform(zr, zi, inverse: true)
        for i in 0..<h {
            x[2 * i] = zr[i]
            x[2 * i + 1] = zi[i]
        }
    }
}

final class VoicePitchDetector {
    private let sampleRate: Double
    private let size: Int
    private let fft: VoiceRealFft
    private let buffer: UnsafeMutablePointer<Double>
    private let re: UnsafeMutablePointer<Double>
    private let im: UnsafeMutablePointer<Double>
    private let minLag: Int
    private let maxLag: Int
    private let nsdf: UnsafeMutablePointer<Double>
    private let peaks: UnsafeMutablePointer<Int>

    init(sampleRate: Double, size: Int) {
        self.sampleRate = sampleRate
        self.size = size
        fft = VoiceRealFft(size * 2)
        buffer = voiceBuffer(size * 2)
        re = voiceBuffer(size + 1)
        im = voiceBuffer(size + 1)
        minLag = max(2, Int(floor(sampleRate / voicePitchHighHz)))
        maxLag = min(size - 2, Int(ceil(sampleRate / voicePitchLowHz)))
        nsdf = voiceBuffer(maxLag + 2)
        peaks = UnsafeMutablePointer<Int>.allocate(capacity: 64)
        peaks.initialize(repeating: 0, count: 64)
    }

    deinit {
        buffer.deallocate()
        re.deallocate()
        im.deallocate()
        nsdf.deallocate()
        peaks.deallocate()
    }

    func detect(_ samples: UnsafeMutablePointer<Double>, previousHz: Double) -> Double {
        let n = size
        var mean = 0.0
        for i in 0..<n { mean += samples[i] }
        mean /= Double(n)
        var energy = 0.0
        for i in 0..<n {
            let v = samples[i] - mean
            buffer[i] = v
            energy += v * v
        }
        if sqrt(energy / Double(n)) < 0.004 { return 0 }
        (buffer + n).update(repeating: 0, count: n)
        fft.forward(buffer, re, im)
        for k in 0...n {
            re[k] = re[k] * re[k] + im[k] * im[k]
            im[k] = 0
        }
        fft.inverse(re, im, buffer)
        var m = 2 * buffer[0]
        for tau in 0...(maxLag + 1) {
            if tau > 0 {
                let head = samples[tau - 1] - mean
                let tail = samples[n - tau] - mean
                m -= head * head + tail * tail
            }
            nsdf[tau] = m > voiceEps ? (2 * buffer[tau]) / m : 0
        }
        var count = 0
        var tau = 1
        while tau < maxLag && nsdf[tau] > 0 { tau += 1 }
        while tau < maxLag && count < 64 {
            while tau < maxLag && nsdf[tau] <= 0 { tau += 1 }
            var best = tau
            while tau < maxLag && nsdf[tau] > 0 {
                if nsdf[tau] > nsdf[best] { best = tau }
                tau += 1
            }
            if best >= minLag && best < maxLag {
                peaks[count] = best
                count += 1
            }
        }
        if count == 0 { return 0 }
        var highest = 0.0
        for i in 0..<count { highest = max(highest, nsdf[peaks[i]]) }
        let tracking = previousHz > 0
        if highest < (tracking ? voiceVoicingStay : voiceVoicingEnter) { return 0 }
        var lag = peaks[0]
        for i in 0..<count where nsdf[peaks[i]] >= 0.9 * highest {
            lag = peaks[i]
            break
        }
        if tracking {
            let expected = sampleRate / previousHz
            if abs(log2(Double(lag) / expected)) > 0.4 {
                var near = -1
                for i in 0..<count {
                    if abs(log2(Double(peaks[i]) / expected)) < 0.2 && (near < 0 || nsdf[peaks[i]] > nsdf[near]) { near = peaks[i] }
                }
                if near > 0 && nsdf[near] >= 0.8 * highest { lag = near }
            }
        }
        let a = nsdf[lag - 1]
        let b = nsdf[lag]
        let c = nsdf[lag + 1]
        let denominator = a - 2 * b + c
        let shift = abs(denominator) > voiceEps ? voiceClamp((0.5 * (a - c)) / denominator, -1, 1) : 0
        return sampleRate / (Double(lag) + shift)
    }
}

final class VoicePitchShifter {
    private let mask = 8191
    private let maxHalf: Int
    private let unvoiced: Int
    private let lookahead: Int
    let latency: Int
    private let input: UnsafeMutablePointer<Double>
    private let output: UnsafeMutablePointer<Double>
    private let pulse: UnsafeMutablePointer<Double>
    private(set) var pulseOut = 1.0
    private let smooth: UnsafeMutablePointer<Double>
    private let lpCoef: Double
    private var lp1 = 0.0
    private var lp2 = 0.0
    private let markCount = 32
    private let markAt: UnsafeMutablePointer<Double>
    private let markPeriod: UnsafeMutablePointer<Double>
    private let markHalf: UnsafeMutablePointer<Int>
    private let markVoiced: UnsafeMutablePointer<Bool>
    private var markWrite = 0
    private var written = 0
    private var lastAt = 0.0
    private var lastVoiced = false
    private var next = 0.0
    private var shifting = 0.0
    private var range = 1.0
    private var centrePeriod: Double
    private var jitter = 0.0
    private var tremor = 0.0
    private let tremorStep: Double
    private var seed: UInt32 = 0x6d2b79f5

    init(sampleRate: Double) {
        maxHalf = Int(floor(sampleRate * 0.0107))
        unvoiced = max(16, Int(floor(sampleRate * 0.005)))
        lookahead = maxHalf + maxHalf / 4
        latency = lookahead + maxHalf + 1
        input = voiceBuffer(mask + 1)
        output = voiceBuffer(mask + 1)
        pulse = voiceBuffer(mask + 1, 1)
        smooth = voiceBuffer(mask + 1)
        lpCoef = 1 - exp((-voiceTwoPi * 900) / sampleRate)
        markAt = voiceBuffer(markCount)
        markPeriod = voiceBuffer(markCount)
        markHalf = UnsafeMutablePointer<Int>.allocate(capacity: markCount)
        markHalf.initialize(repeating: 0, count: markCount)
        markVoiced = UnsafeMutablePointer<Bool>.allocate(capacity: markCount)
        markVoiced.initialize(repeating: false, count: markCount)
        centrePeriod = sampleRate / voiceMaleHz
        tremorStep = (voiceTwoPi * voiceTremorHz) / sampleRate
    }

    deinit {
        input.deallocate()
        output.deallocate()
        pulse.deallocate()
        smooth.deallocate()
        markAt.deallocate()
        markPeriod.deallocate()
        markHalf.deallocate()
        markVoiced.deallocate()
    }

    func shape(range: Double, centrePeriod: Double, jitter: Double, tremor: Double) {
        self.range = range
        self.centrePeriod = centrePeriod
        self.jitter = jitter
        self.tremor = tremor
    }

    private func random() -> Double {
        var x = seed
        x ^= x << 13
        x ^= x >> 17
        x ^= x << 5
        seed = x
        return Double(x) / 4294967296 - 0.5
    }

    private func similarity(_ a: Int, _ b: Int, _ reach: Int, _ stride: Int = 2) -> Double {
        var dot = 0.0
        var ea = 0.0
        var eb = 0.0
        var j = -reach
        while j <= reach {
            let x = input[(a + j) & mask]
            let y = input[(b + j) & mask]
            dot += x * y
            ea += x * x
            eb += y * y
            j += stride
        }
        return dot / sqrt(ea * eb + 1e-18)
    }

    private func addMarks(_ period: Double) {
        let voiced = period >= 16
        let step = voiced ? Int(floor(period + 0.5)) : unvoiced
        let half = voiced ? min(step, maxHalf) : unvoiced
        let reach = max(8, half)
        while true {
            let onset = voiced && !lastVoiced
            let search = voiced ? (onset ? step / 2 : step / 8) : 0
            let lastAt = self.lastAt
            let anchor = Int(voiceRound(lastAt))
            let guess = anchor + step
            if guess + search + half + 2 >= written { return }
            var bestIndex = guess
            var best = Double(guess)
            var likeness = 1.0
            if voiced {
                var peak = 1e-12
                for c in (guess - search)...(guess + search) {
                    let v = abs(smooth[c & mask])
                    if v > peak {
                        peak = v
                        if onset { bestIndex = c }
                    }
                }
                if onset {
                    likeness = -1
                    let wobble = max(1, step / 8)
                    for lag in (step - wobble)...(step + wobble) {
                        likeness = max(likeness, similarity(bestIndex, bestIndex - lag, reach))
                    }
                    best = Double(bestIndex)
                } else {
                    var bestScore = -Double.infinity
                    for c in (guess - search)...(guess + search) {
                        let like = similarity(anchor, c, reach)
                        let score = like + (voiceAnchorBias * abs(smooth[c & mask])) / peak
                        if score > bestScore {
                            bestScore = score
                            bestIndex = c
                            likeness = like
                        }
                    }
                    let a = similarity(anchor, bestIndex - 1, reach, 1)
                    let b = similarity(anchor, bestIndex, reach, 1)
                    let c = similarity(anchor, bestIndex + 1, reach, 1)
                    let denominator = a - 2 * b + c
                    let shift = denominator < -voiceEps ? voiceClamp((0.5 * (a - c)) / denominator, -0.5, 0.5) : 0
                    best = Double(bestIndex) + (shift + (lastAt - Double(anchor)))
                }
            }
            let periodic = voiced && likeness >= voiceLocalVoicing
            if voiced && !periodic { best = Double(anchor + unvoiced) }
            let slot = markWrite
            markAt[slot] = best
            markPeriod[slot] = periodic ? (onset ? Double(step) : best - lastAt) : Double(unvoiced)
            markHalf[slot] = periodic ? half : unvoiced
            markVoiced[slot] = periodic
            markWrite = (slot + 1) % markCount
            self.lastAt = best
            lastVoiced = periodic
        }
    }

    private func placeGrains(_ ratio: Double) {
        while next + Double(lookahead) < Double(written) {
            let s = next
            var mark = -1
            var nearest = 0.0
            for i in 0..<markCount {
                if markPeriod[i] <= 0 { continue }
                let distance = abs(markAt[i] - s)
                if mark < 0 || distance < nearest {
                    mark = i
                    nearest = distance
                }
            }
            if mark < 0 {
                next += Double(unvoiced)
                continue
            }
            let voiced = markVoiced[mark]
            let period = markPeriod[mark]
            var hop = period
            if voiced {
                var r = ratio
                if range != 1 { r *= pow(period / centrePeriod, 1 - range) }
                if tremor > 0 { r *= 1 + tremor * sin(tremorStep * s) }
                hop = period / voiceClamp(r, 0.45 * 0.9, 2.6 * 1.1)
                if jitter > 0 { hop *= 1 + 2 * jitter * random() }
                hop = max(8, hop)
            }
            let centre = markAt[mark]
            var half = markHalf[mark]
            if voiced && hop > Double(2 * half) {
                let room = Int(floor(min(Double(maxHalf), min(Double(written - 3) - centre, s - Double(written - latency)))))
                half = max(half, min(Int(ceil(hop / 2)), room))
            }
            let gain = voiced ? sqrt(hop / Double(half)) : 1
            let offset = centre - s
            let wholeValue = floor(offset)
            let whole = Int(wholeValue)
            let f = offset - wholeValue
            let f2 = f * f
            let f3 = f2 * f
            let c0 = gain * (-0.5 * f3 + f2 - 0.5 * f)
            let c1 = gain * (1.5 * f3 - 2.5 * f2 + 1)
            let c2 = gain * (-1.5 * f3 + 2 * f2 + 0.5 * f)
            let c3 = gain * (0.5 * f3 - 0.5 * f2)
            let start = Int(floor(s - Double(half))) + 1
            let end = Int(ceil(s + Double(half)))
            let w = Double.pi / Double(half)
            let turnCos = cos(w)
            let turnSin = sin(w)
            var wc = cos(w * (Double(start) - s))
            var ws = sin(w * (Double(start) - s))
            var n = start
            while n < end {
                let j = n + whole
                let value = c0 * input[(j - 1) & mask] + c1 * input[j & mask] + c2 * input[(j + 1) & mask] + c3 * input[(j + 2) & mask]
                output[n & mask] += (0.5 + 0.5 * wc) * value
                let nc = wc * turnCos - ws * turnSin
                ws = ws * turnCos + wc * turnSin
                wc = nc
                n += 1
            }
            if voiced { markPulse(s, hop) }
            next += hop
        }
    }

    private func markPulse(_ s: Double, _ hop: Double) {
        let from = Int(ceil(s - 0.75 * hop))
        let to = Int(ceil(s + 0.25 * hop))
        let step = voiceTwoPi / hop
        var n = from
        while n < to {
            pulse[n & mask] = voicePulseGain * (1 + voicePulseDepth * cos(step * (Double(n) - s + 0.25 * hop)))
            n += 1
        }
    }

    func process(_ sample: Double, period: Double, ratio: Double) -> Double {
        input[written & mask] = sample
        lp1 += lpCoef * (sample - lp1)
        lp2 += lpCoef * (lp1 - lp2)
        smooth[written & mask] = lp2
        written += 1
        addMarks(period)
        placeGrains(ratio)
        let t = written - 1 - latency
        if t < 0 { return 0 }
        let slot = t & mask
        let shifted = output[slot]
        output[slot] = 0
        pulseOut = pulse[slot]
        pulse[slot] = 1
        let wanted: Double = abs(ratio - 1) > 2e-3 || abs(range - 1) > 2e-3 || jitter > 0 || tremor > 0 ? 1 : 0
        let step = 1 / Double(2 * unvoiced)
        shifting = wanted > shifting ? min(wanted, shifting + step) : max(wanted, shifting - step)
        return shifting * shifted + (1 - shifting) * input[slot]
    }
}

final class VoicePeakLimiter {
    private let length: Int
    private let required: UnsafeMutablePointer<Double>
    private let held: UnsafeMutablePointer<Double>
    private let delayLine: UnsafeMutablePointer<Double>
    private var pos = 0
    private var heldSum: Double
    private let release: Double
    private var smoothed = 1.0
    private var lowAge: Int
    let latency: Int

    init(sampleRate: Double) {
        length = max(8, Int(voiceRound(sampleRate * 0.0013)))
        required = voiceBuffer(length, 1)
        held = voiceBuffer(length, 1)
        delayLine = voiceBuffer(length)
        heldSum = Double(length)
        release = 1 / (0.1 * sampleRate)
        lowAge = length
        latency = length - 1
    }

    deinit {
        required.deallocate()
        held.deallocate()
        delayLine.deallocate()
    }

    func process(_ sample: Double) -> Double {
        let n = length
        let pos = self.pos
        let magnitude = abs(sample)
        var minimum = 1.0
        if magnitude > voiceLimitCeiling {
            required[pos] = voiceLimitCeiling / magnitude
            lowAge = 0
        } else {
            required[pos] = 1
            if lowAge < n { lowAge += 1 }
        }
        if lowAge < n {
            for i in 0..<n where required[i] < minimum { minimum = required[i] }
        }
        let released = smoothed + release * (1 - smoothed)
        smoothed = minimum < released ? minimum : released
        heldSum += smoothed - held[pos]
        held[pos] = smoothed
        let next = pos + 1 == n ? 0 : pos + 1
        let delayed = delayLine[next]
        delayLine[pos] = sample
        self.pos = next
        return delayed * min(1, heldSum / Double(n))
    }
}
