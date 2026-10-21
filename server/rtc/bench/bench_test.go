package bench

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func TestCallBench(t *testing.T) {
	if os.Getenv("FORKMESH_CALL_BENCH") != "1" {
		t.Skip("set FORKMESH_CALL_BENCH=1 to run the call bench")
	}
	chromium := HeadlessChromium()
	if chromium == "" {
		t.Fatal("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	public, err := filepath.Abs(filepath.Join("..", "..", "..", "app", "public"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(public, "chat-group-calls.js")); err != nil {
		t.Fatal("app/public is not beside the server module")
	}
	opts := Options{Chromium: chromium, Public: public, Home: t.TempDir(), Seed: 1}
	if seconds, err := strconv.Atoi(os.Getenv("FORKMESH_CALL_BENCH_SECONDS")); err == nil && seconds > 0 {
		opts.Duration = time.Duration(seconds) * time.Second
	}
	profiles := Select(Profiles(), os.Getenv("FORKMESH_CALL_BENCH_PROFILES"))
	if len(profiles) == 0 {
		t.Fatal("FORKMESH_CALL_BENCH_PROFILES matches no profile")
	}
	out, err := OutputDir()
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("results in %s", out)
	engine, ok := EngineNamed(os.Getenv("FORKMESH_CALL_BENCH_ENGINE"))
	if !ok {
		t.Fatalf("FORKMESH_CALL_BENCH_ENGINE names no engine: %q", os.Getenv("FORKMESH_CALL_BENCH_ENGINE"))
	}
	var results []Result
	for _, profile := range profiles {
		t.Run(profile.Name, func(t *testing.T) {
			result := RunProfile(t, engine, profile, opts)
			results = append(results, result)
			if err := Write(out, engine.Name, opts, results); err != nil {
				t.Fatal(err)
			}
			t.Logf("join %.0f/%.0f ms; down %.1f fps %.0f kbps %.1f freezes %.0f ms p50; up %.1f fps %.0f kbps %.1f freezes %.0f ms p50; avail %.0f kbps",
				result.Join.VideoMs, result.Join.AudioMs,
				result.Down.FPS, result.Down.VideoKbps, result.Down.FreezeCount, result.Down.DelayP50,
				result.Up.FPS, result.Up.VideoKbps, result.Up.FreezeCount, result.Up.DelayP50,
				result.Sender.AvailableKbps)
			for _, problem := range result.Errors {
				t.Error(problem)
			}
		})
	}
	t.Logf("results in %s", out)
}
