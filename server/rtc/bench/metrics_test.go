package bench

import (
	"math"
	"testing"
	"time"
)

func syntheticReport(name, self, from string, start float64, fps func(second int) float64) *Report {
	report := &Report{Name: name, Self: self, FirstVideo: 400, FirstAudio: 300, Traces: map[string]Trace{}}
	frames, bytes, audio := 0.0, 0.0, 0.0
	for second := 0; second <= 30; second++ {
		if second > 0 {
			frames += fps(second)
			bytes += fps(second) * 5000
			audio += 48000
		}
		sample := Sample{
			T: start + float64(second)*1000,
			Inbound: []Entry{
				{"peer": from, "kind": "video", "ssrc": 7.0, "framesDecoded": frames, "bytesReceived": bytes, "frameHeight": 360.0,
					"packetsReceived": frames * 4, "packetsLost": frames / 4, "freezeCount": 2.0, "totalFreezesDuration": 1.5,
					"jitterBufferDelay": frames * 0.05, "jitterBufferEmittedCount": frames},
				{"peer": from, "kind": "audio", "ssrc": 8.0, "totalSamplesReceived": audio, "concealedSamples": audio / 10,
					"concealmentEvents": 3.0, "jitterBufferDelay": audio * 0.04, "jitterBufferEmittedCount": audio, "packetsReceived": 50.0 * float64(second)},
			},
			Outbound: []Entry{{"kind": "video", "bytesSent": float64(second) * 125000, "framesSent": float64(second) * 24,
				"frameHeight": 360.0, "qualityLimitationReason": "bandwidth", "targetBitrate": 1_000_000.0}},
			Transport: Entry{"availableOutgoingBitrate": 1_000_000.0, "currentRoundTripTime": 0.04},
		}
		report.Timeline = append(report.Timeline, sample)
	}
	report.Final = report.Timeline[len(report.Timeline)-1]
	report.Traces[from] = Trace{First: start, Frames: int(frames), Delays: []float64{40, 50, 60, 70, 200}, Stamps: 5, Unreadable: 5}
	return report
}

func TestMetricsSplitTheImpairedClientsTwoDirections(t *testing.T) {
	start := time.UnixMilli(1_800_000_000_000)
	origin := float64(start.UnixMilli())
	drop := func(second int) float64 {
		if second > 10 && second <= 22 {
			return 6
		}
		return 24
	}
	steady := func(int) float64 { return 24 }
	reports := map[string]*Report{
		"ana": syntheticReport("ana", "a1", "b1", origin, drop),
		"ben": syntheticReport("ben", "b1", "a1", origin, steady),
	}
	result := Result{Impaired: "ana", Started: start, Seconds: 30}
	result.compute(reports, []time.Duration{10 * time.Second, 20 * time.Second})
	if result.Down.VideoStreams != 1 || result.Up.VideoStreams != 1 || result.Down.AudioStreams != 1 {
		t.Fatalf("streams down %+v up %+v", result.Down, result.Up)
	}
	if math.Abs(result.Up.FPS-24) > 0.01 || result.Down.FPS >= 20 {
		t.Errorf("fps down %.2f up %.2f", result.Down.FPS, result.Up.FPS)
	}
	if result.Down.FreezeSource != "getStats" || result.Down.FreezeCount != 2 || result.Down.FreezeSeconds != 1.5 {
		t.Errorf("freezes %+v", result.Down)
	}
	if result.Down.DelayP50 != 60 || result.Down.StampReadPct != 50 || math.Abs(result.Down.VideoLossPct-100.0/17) > 0.01 {
		t.Errorf("delay %.0f read %.0f%% loss %.1f%%", result.Down.DelayP50, result.Down.StampReadPct, result.Down.VideoLossPct)
	}
	if math.Abs(result.Down.ConcealedPct-10) > 0.01 || math.Abs(result.Down.AudioJitterBuffer-40) > 0.01 || math.Abs(result.Down.VideoJitterBuffer-50) > 0.01 {
		t.Errorf("audio %+v", result.Down)
	}
	if result.Join.VideoMs != 400 || result.Join.AudioMs != 300 {
		t.Errorf("join %+v", result.Join)
	}
	if result.Sender.QualityLimitation != "bandwidth" || math.Abs(result.Sender.VideoKbps-1000) > 1 || result.Sender.RTTMs != 40 {
		t.Errorf("sender %+v", result.Sender)
	}
	if math.Abs(result.Down.DelayProxyMs-70) > 0.01 {
		t.Errorf("delay proxy %.1f", result.Down.DelayProxyMs)
	}
	if len(result.Phases) != 3 {
		t.Fatalf("phases %+v", result.Phases)
	}
	if math.Abs(result.Phases[0].DownFPS-24) > 0.01 || math.Abs(result.Phases[1].DownFPS-6) > 0.01 || math.Abs(result.Phases[1].UpFPS-24) > 0.01 {
		t.Errorf("phases %+v", result.Phases)
	}
	if result.Down.RecoverySeconds == nil || *result.Down.RecoverySeconds != 3 || result.Up.RecoverySeconds == nil || *result.Up.RecoverySeconds != 1 {
		t.Errorf("recovery down %v s up %v s", result.Down.RecoverySeconds, result.Up.RecoverySeconds)
	}
	if result.Control.RecoverySeconds != nil {
		t.Errorf("no change, no recovery figure")
	}
	never := func(second int) float64 {
		if second > 10 {
			return 2
		}
		return 24
	}
	reports["ana"] = syntheticReport("ana", "a1", "b1", origin, never)
	stuck := Result{Impaired: "ana", Started: start, Seconds: 30}
	stuck.compute(reports, []time.Duration{10 * time.Second, 20 * time.Second})
	if stuck.Down.RecoverySeconds == nil || *stuck.Down.RecoverySeconds != -1 {
		t.Errorf("a stream that never recovers reports %v s", stuck.Down.RecoverySeconds)
	}
	dead := func(int) float64 { return 0 }
	reports["ana"] = syntheticReport("ana", "a1", "b1", origin, dead)
	blind := Result{Impaired: "ana", Started: start, Seconds: 30}
	blind.compute(reports, []time.Duration{10 * time.Second, 20 * time.Second})
	if blind.Down.RecoverySeconds != nil {
		t.Errorf("a stream with no rate before the change has no recovery figure, got %v", *blind.Down.RecoverySeconds)
	}
}

func TestProfilesHaveUniqueNamesAndSelect(t *testing.T) {
	seen := map[string]bool{}
	for _, profile := range Profiles() {
		if seen[profile.Name] {
			t.Errorf("duplicate profile %s", profile.Name)
		}
		seen[profile.Name] = true
	}
	for _, name := range []string{"clean", "loss-5", "loss-10", "loss-20", "burst", "jitter-100", "cap-300k", "cap-1m", "drop-recover", "path-change"} {
		if !seen[name] {
			t.Errorf("missing profile %s", name)
		}
	}
	picked := Select(Profiles(), "clean, path-change")
	if len(picked) != 2 || picked[1].Name != "path-change" || len(picked[1].Marks()) != 1 {
		t.Errorf("picked %+v", picked)
	}
	if marks := Select(Profiles(), "drop-recover")[0].Marks(); len(marks) != 2 || marks[0] != 10*time.Second || marks[1] != 20*time.Second {
		t.Errorf("marks %v", marks)
	}
}
