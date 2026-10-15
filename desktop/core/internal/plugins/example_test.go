package plugins

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func copyTree(t *testing.T, from, to string) {
	err := filepath.Walk(from, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		relative, _ := filepath.Rel(from, path)
		target := filepath.Join(to, relative)
		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, raw, 0o644)
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestRepoPulseExampleEndToEnd(t *testing.T) {
	if _, err := resolveInterpreter("python3"); err != nil {
		t.Skip("python3 is not installed")
	}
	f := newFixture(t)
	dir := filepath.Join(f.packages, "repo-pulse")
	copyTree(t, filepath.Join("..", "..", "examples", "service-plugins", "repo-pulse"), dir)
	if _, err := Sign(dir, f.signer); err != nil {
		t.Fatal(err)
	}
	repo := t.TempDir()
	if out, err := exec.Command("git", "init", "-q", "-b", "main", repo).CombinedOutput(); err != nil {
		t.Skipf("git init: %v %s", err, out)
	}
	if err := os.WriteFile(filepath.Join(repo, ".git", "refs", "heads", "main"), []byte("1111111111111111111111111111111111111111\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	f.host.Rescan()
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "repo-pulse"}); err != nil {
		t.Fatal(err)
	}
	f.waitState("repo-pulse", StateRunning)
	got, err := f.call("plugin.repo-pulse.watch", map[string]any{"path": repo})
	if err != nil {
		t.Fatal(err)
	}
	var card struct {
		Title string `json:"title"`
		Rows  []struct {
			Value string `json:"value"`
		} `json:"rows"`
	}
	_ = json.Unmarshal(got, &card)
	if card.Title != "Repository pulse" || len(card.Rows) != 1 || card.Rows[0].Value != "main @ 111111111111" {
		t.Fatalf("card %s", got)
	}
	if err := os.WriteFile(filepath.Join(repo, ".git", "refs", "heads", "main"), []byte("2222222222222222222222222222222222222222\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := f.call("plugin.repo-pulse.poll", nil); err != nil {
		t.Fatal(err)
	}
	data := f.ui.Next(t, "plugin.repo-pulse.changed", 5*time.Second)
	if !strings.Contains(string(data), `"commit":"222222222222"`) {
		t.Fatalf("changed %s", data)
	}
	if _, err := f.call("plugin.repo-pulse.watch", map[string]any{"path": "relative"}); err == nil || !strings.Contains(err.Error(), "bad_params") {
		t.Fatalf("relative path: %v", err)
	}
	if _, err := os.Stat(filepath.Join(f.host.options.DataDir, "repo-pulse", "watched.json")); err != nil {
		t.Fatalf("state not kept in the plug-in data folder: %v", err)
	}
}
