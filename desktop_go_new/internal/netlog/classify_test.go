package netlog

import "testing"

func TestBadgeMatchesDesktopClassifier(t *testing.T) {
	stamp := "2026-10-03 12:00:00  "
	cases := []struct {
		line  string
		badge string
	}{
		{stamp + "Session started  [desktop/src/MainWindow.cpp:12]", "SESSION"},
		{stamp + "Push failed: denied  [a.cpp:3|INFO]", "INFO"},
		{stamp + "Everything fine  [a.cpp:3|ERROR]", "ERROR"},
		{stamp + "Mirror: managed-host ref fan-out for forkmesh/forkmesh finished (1 updated, 0 failed).", "MIRROR"},
		{stamp + "Mirror: fan-out finished (10 failed).", "ERROR"},
		{stamp + "Mirror: no errors this round", "MIRROR"},
		{stamp + "Mirror: no errorsx", "ERROR"},
		{stamp + `net GET 200 [body: {"ok":true,"error":"x"}] https://app.forkmesh.com/api/x ` + "· relay sync", "SYNC"},
		{stamp + "net GET 503 https://app.forkmesh.com/api/x · relay sync", "ERROR"},
		{stamp + "net GET err https://app.forkmesh.com/api/x · relay sync", "ERROR"},
		{stamp + "net GET expected https://x/api/errors · request", "NETWORK"},
		{stamp + "net GET 200 https://x/api/y · something new", "NETWORK"},
		{stamp + "net POST 200 https://x/api/desktop-errors · security report", "CRYPTO"},
		{stamp + "UI stalled for 3000 ms", "STALL"},
		{stamp + "Background sync not backgrounded (held the GUI thread)", "BGBLOCK"},
		{stamp + "Background sync backgrounded", "BGTASK"},
		{stamp + "Node n1 core forkmesh-app err: boom", "ERROR"},
		{stamp + "Node n1 core forkmesh-app info: cron tick", "CRON"},
		{stamp + "Node n1 core forkmesh-app err: boom (logged 2026-10-01 10:00:00, replayed)", "APP"},
		{stamp + "Node r2 runner caddy warn: slow upstream", "WEB"},
		{stamp + "Node n3 core rqlited info: leader", "DB"},
		{stamp + "Node n3 core fmnext-ids info: ok", "IDS"},
		{stamp + "Node n3 core sshd info: ok", "SYSTEM"},
		{stamp + "autosync done", "INFO"},
		{stamp + "Pull #12 merged", "PULL"},
		{stamp + "Could not reach the relay", "ERROR"},
		{stamp + "Git: merged agent/x into main in its linked worktree", "MERGE"},
		{stamp + "Push to jett/forkmesh on main → issue #477: priority 18", "GIT"},
		{stamp + "Cloud hit: /api/x failed", "CLOUD"},
		{stamp + "Cloud D1 SELECT * FROM error_log", "DB"},
		{stamp + "Opened ~/projects/forkmesh/src", "INFO"},
		{stamp + "Forked into alice/demo", "FORKED"},
		{stamp + "Agents: started a run", "PROMPT"},
		{stamp + "Issues synced", "ISSUE"},
		{"random line without a stamp", "INFO"},
		{stamp + "Unable to open  [x.cpp:9]", "ERROR"},
		{stamp + "Waiting on the network", "NETWORK"},
		{stamp + "Firewall opened port 7000 for the node", "NETWORK"},
	}
	for _, c := range cases {
		if got := Badge(c.line); got != c.badge {
			t.Errorf("Badge(%q) = %q, want %q", c.line, got, c.badge)
		}
	}
}

func TestSplitLogSource(t *testing.T) {
	cases := []struct {
		in, body string
		severity int
	}{
		{"hello  [a/b.cpp:12]", "hello", severityAutomatic},
		{"hello [a/b.cpp:12|INFO]", "hello", severityInformational},
		{"hello [a/b.cpp:12|ERROR]", "hello", severityError},
		{"hello[a/b.cpp:12]", "hello[a/b.cpp:12]", severityAutomatic},
		{"hello [a/b.cpp:]", "hello [a/b.cpp:]", severityAutomatic},
		{"hello [a b.cpp:1]", "hello [a b.cpp:1]", severityAutomatic},
		{"[a.cpp:1]", "[a.cpp:1]", severityAutomatic},
	}
	for _, c := range cases {
		body, severity := splitLogSource(c.in)
		if body != c.body || severity != c.severity {
			t.Errorf("splitLogSource(%q) = %q,%d want %q,%d", c.in, body, severity, c.body, c.severity)
		}
	}
}

func TestSeverityForBadge(t *testing.T) {
	for badge, want := range map[string]string{
		"ERROR": "error", "crash": "error", " FATAL ": "error",
		"STALL": "warning", "BGBLOCK": "warning", "warn": "warning", "WARNING": "warning",
		"INFO": "info", "CLOUD": "info", "BGTASK": "info", "": "info",
	} {
		if got := SeverityForBadge(badge); got != want {
			t.Errorf("SeverityForBadge(%q) = %q, want %q", badge, got, want)
		}
	}
}
