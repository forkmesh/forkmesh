package fec

import "math"

var (
	audioKs = []int{1, 2, 3}
	videoKs = []int{2, 3, 4, 6, 8, 12, 16, 24, 32, 48}
)

const (
	audioOff       = 0.01
	videoOff       = 0.005
	audioTarget    = 0.005
	videoTarget    = 0.005
	audioOverhead  = 2.0
	videoOverhead  = 0.5
	burstThreshold = 1.5

	importantTarget   = 0.001
	importantOverhead = 1.0
	otherTarget       = 0.01
	otherOverhead     = 0.5
)

func Residual(k, m int, p, burst float64) float64 {
	n := k + m
	b := max(burst, 1)
	pe := p / b
	if pe <= 0 {
		return 0
	}
	if pe >= 1 {
		return 1
	}
	q := 1 - pe
	term := math.Pow(q, float64(n))
	sum := 0.0
	for j := 1; j <= n; j++ {
		term *= float64(n-j+1) / float64(j) * pe / q
		if float64(j)*b > float64(m) {
			sum += term * min(1, float64(j)*b/float64(n))
		}
	}
	return sum
}

func Choose(audio bool, loss, burst float64, maxK int) Params {
	if audio {
		if loss < audioOff {
			return Params{}
		}
		return choose(audioKs, loss, burst, maxK, audioTarget, audioOverhead)
	}
	if loss < videoOff {
		return Params{}
	}
	return choose(videoKs, loss, burst, maxK, videoTarget, videoOverhead)
}

func ChooseVideo(loss, burst float64, maxKImportant, maxKOther int) (important, other Params) {
	if loss < videoOff {
		return Params{}, Params{}
	}
	return choose(videoKs, loss, burst, maxKImportant, importantTarget, importantOverhead),
		choose(videoKs, loss, burst, maxKOther, otherTarget, otherOverhead)
}

func choose(ks []int, loss, burst float64, maxK int, target, capRatio float64) Params {
	p := min(loss, 0.5)
	maxK = max(maxK, ks[0])
	if burst < burstThreshold {
		burst = 1
	}
	best, bestRes, bestRatio := Params{}, math.Inf(1), math.Inf(1)
	fallback, fallbackRes := Params{}, math.Inf(1)
	for _, k := range ks {
		if k > maxK {
			break
		}
		limit := min(16, int(math.Floor(capRatio*float64(k)+1e-9)))
		for m := 1; m <= limit; m++ {
			res := Residual(k, m, p, burst)
			ratio := float64(m) / float64(k)
			if res <= target && (ratio < bestRatio || (ratio == bestRatio && res < bestRes)) {
				best, bestRes, bestRatio = Params{K: k, M: m}, res, ratio
			}
			if res < fallbackRes {
				fallback, fallbackRes = Params{K: k, M: m}, res
			}
		}
	}
	if best.On() {
		return best
	}
	return fallback
}
