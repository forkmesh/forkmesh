package hosts

import (
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

func (j *jobFixture) fleet(t *testing.T, params map[string]any) (map[string]any, error) {
	t.Helper()
	prepared, err := call(t, j.fixture, j.phone, "fleet.prepare", params)
	if err != nil {
		return nil, err
	}
	run := map[string]any{"token": prepared["token"], "confirmText": prepared["typeToConfirm"]}
	for k, v := range params {
		run[k] = v
	}
	return call(t, j.fixture, j.phone, "fleet.run", run)
}

func TestFleetPrepareParity(t *testing.T) {
	j := newJobFixture(t)
	cases := []struct {
		params map[string]any
		code   string
	}{
		{map[string]any{"action": "explode"}, "unsupported_action"},
		{map[string]any{"action": "add", "host": "new", "address": "bad address", "user": "root"}, "bad_address"},
		{map[string]any{"action": "add", "host": "new", "address": "192.0.2.1", "user": "Root"}, "bad_user"},
		{map[string]any{"action": "add", "host": "mirror1", "address": "192.0.2.1", "user": "root"}, "host_exists"},
		{map[string]any{"action": "forget", "host": "ghost"}, "unknown_host"},
		{map[string]any{"action": "deleteNode", "host": "forkmesh"}, "protected_node"},
		{map[string]any{"action": "actions", "host": "mirror1"}, "bad_params"},
		{map[string]any{"action": "deployAll", "mode": "nuke"}, "bad_mode"},
		{map[string]any{"action": "linkCode", "code": "12"}, "bad_code"},
		{map[string]any{"action": "fleetTarget", "enabled": true, "desired": 13}, "bad_params"},
		{map[string]any{"action": "mirrorPush", "repo": "../x"}, "bad_repo"},
	}
	for _, c := range cases {
		if _, err := call(t, j.fixture, j.phone, "fleet.prepare", c.params); rpc.Code(err) != c.code {
			t.Fatalf("%v: %v", c.params, err)
		}
	}
	prepared, err := call(t, j.fixture, j.phone, "fleet.prepare", map[string]any{"action": "uninstall", "host": "MIRROR1"})
	if err != nil || prepared["subject"] != "mirror1" || prepared["typeToConfirm"] != "mirror1" ||
		!strings.HasPrefix(jsonString(prepared["summary"]), "Uninstall ForkMesh from \"mirror1\"") {
		t.Fatalf("prepared %v %v", prepared, err)
	}
	if _, err := call(t, j.fixture, j.phone, "fleet.run", map[string]any{"action": "uninstall", "host": "mirror1", "token": prepared["token"], "confirmText": "nope"}); rpc.Code(err) != "name_mismatch" {
		t.Fatalf("typed: %v", err)
	}
	prepared, _ = call(t, j.fixture, j.phone, "fleet.prepare", map[string]any{"action": "fleetTarget", "enabled": true, "desired": 3})
	if _, err := call(t, j.fixture, j.phone, "fleet.run", map[string]any{"action": "fleetTarget", "enabled": true, "desired": 3, "token": prepared["token"]}); rpc.Code(err) != "hosts_page_unavailable" {
		t.Fatalf("fleetTarget: %v", err)
	}
}

func TestFleetAddForgetAndDeploy(t *testing.T) {
	j := newJobFixture(t)
	started, err := j.fleet(t, map[string]any{"action": "add", "host": "Box2", "address": "192.0.2.44", "user": "deploy"})
	if err != nil || started["message"] != "Saved \"Box2\". The desktop is checking it now." || started["subject"] != "box2|192.0.2.44|deploy" {
		t.Fatalf("add %v %v", started, err)
	}
	waitFor(t, "added", func() bool { return j.hostStatus("Box2") == "added" })
	if _, err := j.fleet(t, map[string]any{"action": "forget", "host": "box2"}); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "forgotten", func() bool { _, i := j.mod.savedHostByName("box2"); return i < 0 })
	j.desktop.env.BuildCommit = strings.Repeat("a", 40)
	j.desktop.env.ManifestDigestEnv = strings.Repeat("b", 64)
	started, err = j.fleet(t, map[string]any{"action": "deployAll", "mode": "install"})
	if err != nil || started["message"] != "Deploying to 2 host(s) in parallel." {
		t.Fatalf("deploy %v %v", started, err)
	}
	waitFor(t, "deploy done", func() bool { return !j.mod.jobRunning("deploy") })
	if _, err := j.fleet(t, map[string]any{"action": "deployAll", "mode": "reinstall"}); err == nil {
		t.Log("reinstall started")
	}
	waitFor(t, "reinstall done", func() bool { return !j.mod.jobRunning("deploy") })
}

func TestPhoneUpdateRunsTheInstaller(t *testing.T) {
	j := newJobFixture(t)
	prepared, err := call(t, j.fixture, j.phone, "hosts.prepare", map[string]any{"action": "update", "host": "mirror1"})
	if err != nil {
		t.Fatal(err)
	}
	started, err := call(t, j.fixture, j.phone, "hosts.run", map[string]any{"action": "update", "host": "mirror1", "token": prepared["token"]})
	if err != nil || started["message"] != "Installing the latest ForkMesh." {
		t.Fatalf("%v %v", started, err)
	}
	event := decode(t, j.phone.Next(t, "hosts.action", 10*time.Second))
	if event["ok"] != true || event["message"] != "ForkMesh was updated and restarted." {
		t.Fatalf("event %v", event)
	}
	prepared, _ = call(t, j.fixture, j.phone, "hosts.prepare", map[string]any{"action": "destroy", "host": "mirror1"})
	if _, err := call(t, j.fixture, j.phone, "hosts.run", map[string]any{"action": "destroy", "host": "mirror1", "token": prepared["token"], "confirmName": "mirror2"}); rpc.Code(err) != "name_mismatch" {
		t.Fatalf("destroy typed: %v", err)
	}
}

func TestFleetLinkAndResume(t *testing.T) {
	j := newJobFixture(t)
	if _, err := j.fleet(t, map[string]any{"action": "linkCode", "code": "123456"}); rpc.Code(err) != "cannot_sign" {
		t.Fatalf("link: %v", err)
	}
	if _, err := j.fleet(t, map[string]any{"action": "resume"}); rpc.Code(err) != "nothing_to_resume" {
		t.Fatalf("resume: %v", err)
	}
}
