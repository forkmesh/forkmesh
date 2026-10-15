package paths

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

func TestMatchesPythonResolver(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	_, file, _, _ := runtime.Caller(0)
	tools := filepath.Join(filepath.Dir(file), "..", "..", "..", "..", "tools")
	if _, err := os.Stat(filepath.Join(tools, "forkmesh_paths.py")); err != nil {
		t.Skip("tools/forkmesh_paths.py not found")
	}
	cases := []struct {
		name    string
		project string
		env     map[string]string
		setup   func(home string)
	}{
		{name: "fresh", project: ""},
		{name: "other project", project: "blt"},
		{name: "explicit home", env: map[string]string{"FORKMESH_HOME": "/srv/fm"}},
		{name: "projects dir", project: "next", env: map[string]string{"FORKMESH_PROJECTS_DIR": "/srv/projects"}},
		{name: "xdg sandbox", env: map[string]string{"XDG_DATA_HOME": "/elsewhere/data"}},
		{name: "legacy state", setup: func(home string) {
			os.MkdirAll(filepath.Join(home, ".forkmesh", "diagnostics"), 0o700)
			os.WriteFile(filepath.Join(home, ".forkmesh", "forkmesh.lock"), nil, 0o600)
		}},
		{name: "spawned data dir", env: map[string]string{"FORKMESH_DATA_DIR": "/custom/data"}},
		{name: "bad slug", project: "Bad Slug!"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			home := t.TempDir()
			if tc.setup != nil {
				tc.setup(home)
			}
			env := map[string]string{"HOME": home}
			for k, v := range tc.env {
				env[k] = v
			}
			script := `import json,sys
sys.path.insert(0, sys.argv[1])
import forkmesh_paths as p
from pathlib import Path
l = p.resolve(sys.argv[2] or None, json.loads(sys.argv[3]), Path(sys.argv[4]))
print(json.dumps({k: str(getattr(l, k)) for k in ["projects_root","home_dir","project","project_dir","data","config","cache","state","logs","ssh"]} | {"reroot_allowed": l.reroot_allowed}))`
			raw, _ := json.Marshal(env)
			out, err := exec.Command(python, "-c", script, tools, tc.project, string(raw), home).Output()
			if err != nil {
				t.Fatalf("python: %v", err)
			}
			var want map[string]any
			if err := json.Unmarshal(out, &want); err != nil {
				t.Fatal(err)
			}
			got := Resolve(tc.project, func(name string) string { return env[name] }, home)
			check := map[string]any{
				"projects_root": got.ProjectsRoot, "home_dir": got.HomeDir, "project": got.Project,
				"project_dir": got.ProjectDir, "data": got.Data, "config": got.Config, "cache": got.Cache,
				"state": got.State, "logs": got.Logs, "ssh": got.SSH, "reroot_allowed": got.RerootAllowed,
			}
			for key, value := range check {
				if want[key] != value {
					t.Errorf("%s: go %v, python %v", key, value, want[key])
				}
			}
		})
	}
}
