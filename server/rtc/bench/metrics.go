package bench

import (
	"encoding/json"
	"math"
	"slices"
	"sort"
	"time"

	"forkmesh.com/mirror-node/rtc/netsim"
)

type Entry map[string]any

func (e Entry) num(key string) float64 {
	if value, ok := e[key].(float64); ok {
		return value
	}
	return 0
}

func (e Entry) has(key string) bool {
	_, ok := e[key].(float64)
	return ok
}

func (e Entry) text(key string) string {
	value, _ := e[key].(string)
	return value
}

type Sample struct {
	T         float64 `json:"t"`
	State     string  `json:"state"`
	Inbound   []Entry `json:"inbound"`
	Outbound  []Entry `json:"outbound"`
	Transport Entry   `json:"transport"`
}

type Trace struct {
	First       float64   `json:"first"`
	Last        float64   `json:"last"`
	Frames      int       `json:"frames"`
	GapFreezes  int       `json:"gapFreezes"`
	GapFreezeMs float64   `json:"gapFreezeMs"`
	Stamps      int       `json:"stamps"`
	Unreadable  int       `json:"unreadable"`
	Delays      []float64 `json:"delays"`
}

type Report struct {
	Name       string           `json:"name"`
	UA         string           `json:"ua"`
	Self       string           `json:"self"`
	Selves     []string         `json:"selves"`
	State      string           `json:"state"`
	Error      string           `json:"error"`
	Origin     float64          `json:"origin"`
	FirstVideo float64          `json:"firstVideo"`
	FirstAudio float64          `json:"firstAudio"`
	Timeline   []Sample         `json:"timeline"`
	Final      Sample           `json:"final"`
	Traces     map[string]Trace `json:"traces"`
}

type Join struct {
	VideoMs float64 `json:"videoMs"`
	AudioMs float64 `json:"audioMs"`
}

type Direction struct {
	VideoStreams       int      `json:"videoStreams"`
	AudioStreams       int      `json:"audioStreams"`
	FPS                float64  `json:"fps"`
	FreezeCount        float64  `json:"freezeCount"`
	FreezeSeconds      float64  `json:"freezeSeconds"`
	FreezeSource       string   `json:"freezeSource"`
	GapFreezeCount     float64  `json:"gapFreezeCount"`
	GapFreezeSeconds   float64  `json:"gapFreezeSeconds"`
	Height             float64  `json:"height"`
	FramesDropped      float64  `json:"framesDropped"`
	Nack               float64  `json:"nack"`
	PLI                float64  `json:"pli"`
	FIR                float64  `json:"fir"`
	KeyFrames          float64  `json:"keyFrames"`
	VideoKbps          float64  `json:"videoKbps"`
	VideoLossPct       float64  `json:"videoLossPct"`
	DelayP50           float64  `json:"delayP50Ms"`
	DelayP95           float64  `json:"delayP95Ms"`
	DelaySamples       int      `json:"delaySamples"`
	StampReadPct       float64  `json:"stampReadPct"`
	ConcealedPct       float64  `json:"concealedPct"`
	ConcealmentEvents  float64  `json:"concealmentEvents"`
	AudioJitterBuffer  float64  `json:"audioJitterBufferMs"`
	VideoJitterBuffer  float64  `json:"videoJitterBufferMs"`
	AudioPacketsLost   float64  `json:"audioPacketsLost"`
	AudioLossPct       float64  `json:"audioLossPct"`
	DelayProxyMs       float64  `json:"delayProxyMs"`
	RecoverySeconds    *float64 `json:"recoverySeconds,omitempty"`
	RecoveryKbpsSecond *float64 `json:"recoveryKbpsSeconds,omitempty"`
}

type Sender struct {
	AvailableKbps     float64 `json:"availableOutgoingKbps"`
	VideoKbps         float64 `json:"videoKbps"`
	TargetKbps        float64 `json:"targetKbps"`
	Height            float64 `json:"height"`
	FPS               float64 `json:"fps"`
	QualityLimitation string  `json:"qualityLimitation"`
	RTTMs             float64 `json:"rttMs"`
	Retransmitted     float64 `json:"retransmittedPackets"`
	NackReceived      float64 `json:"nackReceived"`
	PLIReceived       float64 `json:"pliReceived"`
}

type Phase struct {
	From          float64 `json:"from"`
	To            float64 `json:"to"`
	DownFPS       float64 `json:"downFps"`
	DownKbps      float64 `json:"downKbps"`
	UpFPS         float64 `json:"upFps"`
	UpKbps        float64 `json:"upKbps"`
	AvailableKbps float64 `json:"availableOutgoingKbps"`
	SentKbps      float64 `json:"sentKbps"`
}

type Result struct {
	Engine    string                     `json:"engine"`
	Profile   Profile                    `json:"profile"`
	Started   time.Time                  `json:"started"`
	Seconds   float64                    `json:"seconds"`
	Impaired  string                     `json:"impaired"`
	Peers     []string                   `json:"peers"`
	Join      Join                       `json:"join"`
	Down      Direction                  `json:"down"`
	Up        Direction                  `json:"up"`
	Control   Direction                  `json:"control"`
	Sender    Sender                     `json:"sender"`
	Phases    []Phase                    `json:"phases,omitempty"`
	CPU       map[string]float64         `json:"cpuPercent"`
	ServerCPU float64                    `json:"serverCpuPercent"`
	Relay     []netsim.ClientStats       `json:"relay"`
	Keys      map[string][]string        `json:"keys"`
	Errors    []string                   `json:"errors,omitempty"`
	Raw       map[string]json.RawMessage `json:"-"`
	Logs      []string                   `json:"-"`
}

const gapFreezeMs = 500

type point struct {
	t     float64
	entry Entry
}

type stream struct {
	receiver string
	sender   string
	kind     string
	final    Entry
	finalT   float64
	series   []point
	trace    *Trace
}

func identities(reports map[string]*Report) map[string]string {
	names := map[string]string{}
	for name, report := range reports {
		if report == nil {
			continue
		}
		for _, id := range append([]string{report.Self}, report.Selves...) {
			if id != "" {
				names[id] = name
			}
		}
	}
	return names
}

func collect(reports map[string]*Report) []stream {
	names := identities(reports)
	var out []stream
	for receiver, report := range reports {
		if report == nil || report.Error != "" {
			continue
		}
		for _, entry := range report.Final.Inbound {
			s := stream{
				receiver: receiver, sender: names[entry.text("peer")], kind: entry.text("kind"),
				final: entry, finalT: report.Final.T,
			}
			ssrc := entry.num("ssrc")
			for _, sample := range report.Timeline {
				for _, older := range sample.Inbound {
					if older.num("ssrc") == ssrc && older.text("kind") == s.kind {
						s.series = append(s.series, point{t: sample.T, entry: older})
					}
				}
			}
			s.series = append(s.series, point{t: report.Final.T, entry: entry})
			if trace, ok := report.Traces[entry.text("peer")]; ok && s.kind == "video" {
				s.trace = &trace
			}
			out = append(out, s)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].receiver != out[j].receiver {
			return out[i].receiver < out[j].receiver
		}
		return out[i].sender < out[j].sender
	})
	return out
}

func mean(values []float64) float64 {
	if len(values) == 0 {
		return 0
	}
	sum := 0.0
	for _, value := range values {
		sum += value
	}
	return sum / float64(len(values))
}

func percentile(values []float64, p float64) float64 {
	if len(values) == 0 {
		return 0
	}
	sorted := slices.Clone(values)
	slices.Sort(sorted)
	index := p * float64(len(sorted)-1)
	low := int(math.Floor(index))
	high := int(math.Ceil(index))
	return sorted[low] + (sorted[high]-sorted[low])*(index-float64(low))
}

func firstWith(series []point, key string) float64 {
	for _, p := range series {
		if p.entry.num(key) > 0 {
			return p.t
		}
	}
	return 0
}

func rate(series []point, key string, from, to float64) (float64, bool) {
	var a, b *point
	for i := range series {
		p := &series[i]
		if p.t < from || p.t > to {
			continue
		}
		if a == nil {
			a = p
		}
		b = p
	}
	if a == nil || b == nil || b.t-a.t < 500 {
		return 0, false
	}
	return (b.entry.num(key) - a.entry.num(key)) / ((b.t - a.t) / 1000), true
}

func direction(streams []stream, windowStart, recoverFrom, baseFrom, baseTo float64) Direction {
	var d Direction
	var fps, freezes, freezeSeconds, gapFreezes, gapSeconds, heights, dropped, nack, pli, fir, keys, kbps, videoLoss, videoBuffer []float64
	var concealed, events, audioBuffer, audioLost, audioLoss []float64
	var delays []float64
	var recovery, recoveryKbps []float64
	stamps, frames := 0, 0
	allFrozen := true
	for _, s := range streams {
		final := s.final
		if s.kind == "video" {
			d.VideoStreams++
			first := firstWith(s.series, "framesDecoded")
			if s.trace != nil && s.trace.First > 0 {
				first = s.trace.First
			}
			if first > 0 && s.finalT > first {
				fps = append(fps, final.num("framesDecoded")/((s.finalT-first)/1000))
			} else {
				fps = append(fps, 0)
			}
			if received := firstWith(s.series, "bytesReceived"); received > 0 && s.finalT > received {
				kbps = append(kbps, final.num("bytesReceived")*8/1000/((s.finalT-received)/1000))
			}
			if final.has("freezeCount") {
				freezes = append(freezes, final.num("freezeCount"))
				freezeSeconds = append(freezeSeconds, final.num("totalFreezesDuration"))
			} else {
				allFrozen = false
			}
			switch {
			case s.trace != nil && s.trace.Last > 0:
				count, stalled := float64(s.trace.GapFreezes), s.trace.GapFreezeMs
				if tail := s.finalT - s.trace.Last; tail > gapFreezeMs {
					count++
					stalled += tail
				}
				gapFreezes = append(gapFreezes, count)
				gapSeconds = append(gapSeconds, stalled/1000)
			case windowStart > 0 && s.finalT > windowStart:
				gapFreezes = append(gapFreezes, 1)
				gapSeconds = append(gapSeconds, (s.finalT-windowStart)/1000)
			}
			if s.trace != nil {
				delays = append(delays, s.trace.Delays...)
				stamps += s.trace.Stamps
				frames += s.trace.Stamps + s.trace.Unreadable
			}
			var h []float64
			for _, p := range s.series {
				if height := p.entry.num("frameHeight"); height > 0 {
					h = append(h, height)
				}
			}
			heights = append(heights, percentile(h, 0.5))
			dropped = append(dropped, final.num("framesDropped"))
			nack = append(nack, final.num("nackCount"))
			pli = append(pli, final.num("pliCount"))
			fir = append(fir, final.num("firCount"))
			keys = append(keys, final.num("keyFramesDecoded"))
			if lost, total := final.num("packetsLost"), final.num("packetsLost")+final.num("packetsReceived"); lost >= 0 && total > 0 {
				videoLoss = append(videoLoss, 100*lost/total)
			}
			if count := final.num("jitterBufferEmittedCount"); count > 0 {
				videoBuffer = append(videoBuffer, 1000*final.num("jitterBufferDelay")/count)
			}
			if recoverFrom > 0 {
				recovery = append(recovery, recoveryTime(s.series, "framesDecoded", recoverFrom, baseFrom, baseTo))
				recoveryKbps = append(recoveryKbps, recoveryTime(s.series, "bytesReceived", recoverFrom, baseFrom, baseTo))
			}
		}
		if s.kind == "audio" {
			d.AudioStreams++
			if total := final.num("totalSamplesReceived"); total > 0 {
				concealed = append(concealed, 100*final.num("concealedSamples")/total)
			}
			events = append(events, final.num("concealmentEvents"))
			if count := final.num("jitterBufferEmittedCount"); count > 0 {
				audioBuffer = append(audioBuffer, 1000*final.num("jitterBufferDelay")/count)
			}
			audioLost = append(audioLost, final.num("packetsLost"))
			if total := final.num("packetsLost") + final.num("packetsReceived"); total > 0 {
				audioLoss = append(audioLoss, 100*final.num("packetsLost")/total)
			}
		}
	}
	d.FPS = mean(fps)
	if allFrozen && len(freezes) > 0 {
		d.FreezeSource = "getStats"
		d.FreezeCount = mean(freezes)
		d.FreezeSeconds = mean(freezeSeconds)
	} else {
		d.FreezeSource = "frame gaps"
		d.FreezeCount = mean(gapFreezes)
		d.FreezeSeconds = mean(gapSeconds)
	}
	d.GapFreezeCount = mean(gapFreezes)
	d.GapFreezeSeconds = mean(gapSeconds)
	d.Height = mean(heights)
	d.FramesDropped = mean(dropped)
	d.Nack = mean(nack)
	d.PLI = mean(pli)
	d.FIR = mean(fir)
	d.KeyFrames = mean(keys)
	d.VideoKbps = mean(kbps)
	d.VideoLossPct = mean(videoLoss)
	d.VideoJitterBuffer = mean(videoBuffer)
	d.DelayP50 = percentile(delays, 0.5)
	d.DelayP95 = percentile(delays, 0.95)
	d.DelaySamples = len(delays)
	if frames > 0 {
		d.StampReadPct = 100 * float64(stamps) / float64(frames)
	}
	d.ConcealedPct = mean(concealed)
	d.ConcealmentEvents = mean(events)
	d.AudioJitterBuffer = mean(audioBuffer)
	d.AudioPacketsLost = mean(audioLost)
	d.AudioLossPct = mean(audioLoss)
	d.RecoverySeconds = worstRecovery(recovery)
	d.RecoveryKbpsSecond = worstRecovery(recoveryKbps)
	return d
}

const noBaseline = -2

func worstRecovery(values []float64) *float64 {
	var worst *float64
	for _, value := range values {
		if value == noBaseline {
			continue
		}
		if worst == nil || value < 0 || (*worst >= 0 && value > *worst) {
			v := value
			worst = &v
		}
	}
	return worst
}

func recoveryTime(series []point, key string, from, baseFrom, baseTo float64) float64 {
	baseline, ok := rate(series, key, baseFrom, baseTo)
	if !ok || baseline <= 0 {
		return noBaseline
	}
	for i := 1; i < len(series); i++ {
		a, b := series[i-1], series[i]
		if a.t < from || b.t-a.t < 200 {
			continue
		}
		if (b.entry.num(key)-a.entry.num(key))/((b.t-a.t)/1000) >= 0.8*baseline {
			return (b.t - from) / 1000
		}
	}
	return -1
}

func senderStats(report *Report) Sender {
	var s Sender
	if report == nil {
		return s
	}
	var available, target, rtt, heights []float64
	reasons := map[string]int{}
	var video []point
	for _, sample := range append(slices.Clone(report.Timeline), report.Final) {
		if value := sample.Transport.num("availableOutgoingBitrate"); value > 0 {
			available = append(available, value/1000)
		}
		if value := sample.Transport.num("currentRoundTripTime"); value > 0 {
			rtt = append(rtt, value*1000)
		}
		for _, entry := range sample.Outbound {
			if entry.text("kind") != "video" {
				continue
			}
			video = append(video, point{t: sample.T, entry: entry})
			if value := entry.num("targetBitrate"); value > 0 {
				target = append(target, value/1000)
			}
			if value := entry.num("frameHeight"); value > 0 {
				heights = append(heights, value)
			}
			if reason := entry.text("qualityLimitationReason"); reason != "" {
				reasons[reason]++
			}
		}
	}
	s.AvailableKbps = mean(available)
	s.TargetKbps = mean(target)
	s.RTTMs = mean(rtt)
	s.Height = percentile(heights, 0.5)
	if len(video) > 1 {
		if value, ok := rate(video, "bytesSent", video[0].t, video[len(video)-1].t); ok {
			s.VideoKbps = value * 8 / 1000
		}
		if value, ok := rate(video, "framesSent", video[0].t, video[len(video)-1].t); ok {
			s.FPS = value
		}
		last := video[len(video)-1].entry
		s.Retransmitted = last.num("retransmittedPacketsSent")
		s.NackReceived = last.num("nackCount")
		s.PLIReceived = last.num("pliCount")
	}
	best := 0
	for reason, count := range reasons {
		if count > best || (count == best && reason < s.QualityLimitation) {
			s.QualityLimitation, best = reason, count
		}
	}
	return s
}

func phaseRate(streams []stream, key string, from, to float64, scale float64) float64 {
	var values []float64
	for _, s := range streams {
		if s.kind != "video" {
			continue
		}
		if value, ok := rate(s.series, key, from, to); ok {
			values = append(values, value*scale)
		}
	}
	return mean(values)
}

func (r *Result) compute(reports map[string]*Report, marks []time.Duration) {
	all := collect(reports)
	var down, up, control []stream
	for _, s := range all {
		switch {
		case s.receiver == r.Impaired:
			down = append(down, s)
		case s.sender == r.Impaired:
			up = append(up, s)
		case s.sender != "":
			control = append(control, s)
		}
	}
	start := float64(r.Started.UnixMilli())
	end := start + r.Seconds*1000
	recoverFrom, baseFrom, baseTo := 0.0, 0.0, 0.0
	if len(marks) > 0 {
		recoverFrom = start + float64(marks[len(marks)-1].Milliseconds())
		baseFrom = start + 3000
		baseTo = start + float64(marks[0].Milliseconds())
	}
	r.Down = direction(down, start, recoverFrom, baseFrom, baseTo)
	r.Up = direction(up, start, recoverFrom, baseFrom, baseTo)
	r.Control = direction(control, 0, 0, 0, 0)
	impaired := reports[r.Impaired]
	r.Sender = senderStats(impaired)
	if impaired != nil {
		r.Join = Join{VideoMs: impaired.FirstVideo, AudioMs: impaired.FirstAudio}
	}
	proxy := r.Sender.RTTMs / 2
	r.Down.DelayProxyMs = proxy + r.Down.VideoJitterBuffer
	r.Up.DelayProxyMs = proxy + r.Up.VideoJitterBuffer
	if len(marks) == 0 {
		return
	}
	bounds := []float64{start}
	for _, mark := range marks {
		bounds = append(bounds, start+float64(mark.Milliseconds()))
	}
	bounds = append(bounds, end)
	var senderSeries []point
	var transport []point
	if impaired != nil {
		for _, sample := range append(slices.Clone(impaired.Timeline), impaired.Final) {
			transport = append(transport, point{t: sample.T, entry: sample.Transport})
			for _, entry := range sample.Outbound {
				if entry.text("kind") == "video" {
					senderSeries = append(senderSeries, point{t: sample.T, entry: entry})
				}
			}
		}
	}
	for i := 0; i+1 < len(bounds); i++ {
		from, to := bounds[i], bounds[i+1]
		phase := Phase{
			From:     (from - start) / 1000,
			To:       (to - start) / 1000,
			DownFPS:  phaseRate(down, "framesDecoded", from, to, 1),
			DownKbps: phaseRate(down, "bytesReceived", from, to, 8.0/1000),
			UpFPS:    phaseRate(up, "framesDecoded", from, to, 1),
			UpKbps:   phaseRate(up, "bytesReceived", from, to, 8.0/1000),
		}
		if value, ok := rate(senderSeries, "bytesSent", from, to); ok {
			phase.SentKbps = value * 8 / 1000
		}
		var available []float64
		for _, p := range transport {
			if p.t >= from && p.t <= to && p.entry.num("availableOutgoingBitrate") > 0 {
				available = append(available, p.entry.num("availableOutgoingBitrate")/1000)
			}
		}
		phase.AvailableKbps = mean(available)
		r.Phases = append(r.Phases, phase)
	}
}
