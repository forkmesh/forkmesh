package bench

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

type Results struct {
	Engine   string    `json:"engine"`
	Written  time.Time `json:"written"`
	Chromium string    `json:"chromium"`
	CPUs     int       `json:"cpus"`
	Seconds  float64   `json:"seconds"`
	Peers    []string  `json:"peers"`
	Results  []Result  `json:"results"`
}

func writeJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0o644)
}

func Write(dir, engine string, opts Options, results []Result) error {
	opts = opts.withDefaults()
	summary := Results{
		Engine: engine, Written: time.Now(), Chromium: opts.Chromium, CPUs: runtime.NumCPU(),
		Seconds: opts.Duration.Seconds(), Peers: opts.Peers, Results: results,
	}
	if err := writeJSON(filepath.Join(dir, "results.json"), summary); err != nil {
		return err
	}
	raw := filepath.Join(dir, "raw")
	if err := os.MkdirAll(raw, 0o755); err != nil {
		return err
	}
	for _, result := range results {
		dump := map[string]any{"reports": result.Raw, "logs": result.Logs, "relay": result.Relay}
		if err := writeJSON(filepath.Join(raw, result.Profile.Name+".json"), dump); err != nil {
			return err
		}
	}
	return nil
}
