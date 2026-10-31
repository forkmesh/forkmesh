package fmmserver

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/signal"
)

const fmmCallPage = `<!doctype html><meta charset="utf-8"><body><script type="module">
import { createGroupCalls } from "/public/chat-group-calls.js";
import { signerFromSeed, keyId } from "/public/chat-call-e2ee.js";
const params = new URLSearchParams(location.search);
const name = params.get("name");
const until = Number(params.get("until"));
const post = (path, body) => fetch(path + "?name=" + name, {method: "POST", body: JSON.stringify(body)});
const log = (text) => post("/log", String(text)).catch(() => {});
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
try {
  const seed = crypto.getRandomValues(new Uint8Array(32));
  const signer = await signerFromSeed(seed);
  const kid = await keyId(signer.idk);
  const timeline = [];
  const calls = createGroupCalls({
    sendFrame: async (channelKey, type, extra) => {
      await fetch("/relay", {method: "POST", body: JSON.stringify({type, sender: name, sealKid: kid, sealAcct: "acct-" + name, ...extra})});
      return true;
    },
    fetchTicket: async (channelId, body) => (await fetch("/ticket?name=" + name, {method: "POST", body: JSON.stringify(body || {})})).json(),
    accountName: () => name,
    workerUrl: "/public/chat-group-call-crypto.js",
    callSigner: async () => signer,
    callAccount: () => "acct-" + name,
    callId: () => "call-room-test",
    fmm: {
      workerUrl: "/public/fmm/fmm-worker.js",
      workletUrl: "/public/fmm/fmm-playout-worklet.js",
      faceStreamUrl: "/public/chat-call-facestream.js",
      onEvent: (event) => { if (event.type !== "auth") log(JSON.stringify(event)); },
    },
  });
  let since = 0;
  (async () => {
    for (;;) {
      try {
        const batch = await (await fetch("/relay?since=" + since)).json();
        for (const frame of batch.frames || []) {
          if (frame.sender === name) continue;
          calls.handleFrame(frame, "#team");
        }
        since = batch.next;
      } catch (_) {}
      await sleep(100);
    }
  })();
  const joined = await calls.join("#team", "c".repeat(32), "video");
  if (joined.error) throw new Error(joined.error);
  let last = 0;
  while (Date.now() < until) {
    await sleep(250);
    const active = calls.active;
    if (!active?.fmm) continue;
    if (Date.now() - last >= 500) {
      last = Date.now();
      const sample = await active.fmm.stats();
      if (sample) timeline.push({t: Date.now(), state: active.state, inbound: sample.inbound.map((s) => ({peer: s.peer, kind: s.kind, decoded: s.framesDecoded || 0, played: (s.totalSamplesReceived || 0) - (s.concealedSamples || 0)}))});
    }
    const v = active.verification;
    if (v?.code && active.confirmed !== v.code.digits) {
      active.confirmed = v.code.digits;
      await calls.confirmCode(true);
    }
  }
  const active = calls.active;
  const sample = await active.fmm.stats();
  const verification = active.verification || null;
  const tiles = [...active.tiles.values()].map((tile) => ({peer: tile.peer, name: tile.name, auth: tile.auth, rep: tile.rep, tracks: tile.stream.getVideoTracks().length}));
  await post("/report", {self: active.peer, engine: active.engine, state: active.state, moved: active.moved || 0, tiles, sample, verification, timeline, codec: sample?.codec});
} catch (error) {
  await post("/report", {error: String(error && error.stack || error)});
}
</script></body>`

type fmmInbound struct {
	Peer                 string             `json:"peer"`
	Kind                 string             `json:"kind"`
	FramesDecoded        int                `json:"framesDecoded"`
	KeyFramesDecoded     int                `json:"keyFramesDecoded"`
	FrameHeight          int                `json:"frameHeight"`
	BytesReceived        int                `json:"bytesReceived"`
	TotalSamplesReceived int                `json:"totalSamplesReceived"`
	ConcealedSamples     int                `json:"concealedSamples"`
	SealDrops            int                `json:"sealDrops"`
	FreezeCount          int                `json:"freezeCount"`
	FirstPlayWall        int64              `json:"firstPlayWall"`
	Statuses             []int              `json:"statuses"`
	TargetMs             float64            `json:"targetMs"`
	Buffer               map[string]float64 `json:"buffer"`
	TotalFreezesDuration float64            `json:"totalFreezesDuration"`
}

type fmmTimelinePoint struct {
	T       int64  `json:"t"`
	State   string `json:"state"`
	Inbound []struct {
		Peer    string `json:"peer"`
		Kind    string `json:"kind"`
		Decoded int    `json:"decoded"`
		Played  int    `json:"played"`
	} `json:"inbound"`
}

type fmmReport struct {
	Error  string `json:"error"`
	Self   string `json:"self"`
	Engine string `json:"engine"`
	State  string `json:"state"`
	Moved  int    `json:"moved"`
	Codec  string `json:"codec"`
	Tiles  []struct {
		Peer   string `json:"peer"`
		Name   string `json:"name"`
		Auth   string `json:"auth"`
		Rep    string `json:"rep"`
		Tracks int    `json:"tracks"`
	} `json:"tiles"`
	Sample struct {
		Inbound  []fmmInbound           `json:"inbound"`
		Counters map[string]any         `json:"counters"`
		Engine   map[string]any         `json:"engine"`
		Moved    bool                   `json:"moved"`
		Standby  bool                   `json:"standby"`
		Outbound []map[string]any       `json:"outbound"`
		Extra    map[string]interface{} `json:"-"`
	} `json:"sample"`
	Verification *struct {
		Code *struct {
			EmojiText string `json:"emojiText"`
			Digits    string `json:"digits"`
		} `json:"code"`
		Verified bool     `json:"verified"`
		Waiting  []string `json:"waiting"`
	} `json:"verification"`
	Timeline []fmmTimelinePoint `json:"timeline"`
}

type fmmBrowserRun struct {
	t       *testing.T
	mu      sync.Mutex
	reports map[string]fmmReport
	relay   []json.RawMessage
	ticket  func(name string) map[string]any
	procs   []*exec.Cmd
	web     *httptest.Server
	public  string
}

func appPublic(t *testing.T) string {
	public, err := filepath.Abs(filepath.Join("..", "..", "..", "app", "public"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(public, "fmm", "fmm-worker.js")); err != nil {
		t.Skip("app/public/fmm is not beside the server module")
	}
	return public
}

func newFMMBrowserRun(t *testing.T, ticket func(name string) map[string]any) *fmmBrowserRun {
	r := &fmmBrowserRun{t: t, reports: map[string]fmmReport{}, ticket: ticket, public: appPublic(t)}
	routes := http.NewServeMux()
	routes.Handle("/public/", http.StripPrefix("/public/", http.FileServer(http.Dir(r.public))))
	routes.HandleFunc("/page", func(w http.ResponseWriter, req *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, fmmCallPage)
	})
	routes.HandleFunc("/ticket", func(w http.ResponseWriter, req *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(r.ticket(req.URL.Query().Get("name")))
	})
	routes.HandleFunc("/relay", func(w http.ResponseWriter, req *http.Request) {
		r.mu.Lock()
		defer r.mu.Unlock()
		if req.Method == http.MethodPost {
			body, _ := io.ReadAll(io.LimitReader(req.Body, 256<<10))
			r.relay = append(r.relay, json.RawMessage(body))
			return
		}
		since, _ := strconv.Atoi(req.URL.Query().Get("since"))
		if since < 0 || since > len(r.relay) {
			since = len(r.relay)
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"frames": append([]json.RawMessage{}, r.relay[since:]...), "next": len(r.relay)})
	})
	routes.HandleFunc("/log", func(w http.ResponseWriter, req *http.Request) {
		body, _ := io.ReadAll(io.LimitReader(req.Body, 16<<10))
		t.Logf("%s: %s", req.URL.Query().Get("name"), body)
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, req *http.Request) {
		body, _ := io.ReadAll(io.LimitReader(req.Body, 8<<20))
		var rep fmmReport
		if err := json.Unmarshal(body, &rep); err != nil {
			rep.Error = "unreadable report: " + err.Error()
		}
		r.mu.Lock()
		r.reports[req.URL.Query().Get("name")] = rep
		r.mu.Unlock()
	})
	r.web = httptest.NewServer(routes)
	t.Cleanup(r.web.Close)
	t.Cleanup(func() {
		for _, p := range r.procs {
			syscall.Kill(-p.Process.Pid, syscall.SIGKILL)
			p.Wait()
		}
	})
	return r
}

func browserEnvFor(home string) []string {
	var env []string
	for _, entry := range os.Environ() {
		key, _, _ := strings.Cut(entry, "=")
		switch key {
		case "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "FORKMESH_HOME", "FORKMESH_PROJECTS_DIR":
			continue
		}
		env = append(env, entry)
	}
	return append(env, "HOME="+home, "XDG_DATA_HOME="+home, "XDG_CONFIG_HOME="+home, "XDG_CACHE_HOME="+home)
}

func (r *fmmBrowserRun) launch(chromium, name string, until time.Time) {
	home := r.t.TempDir()
	cmd := exec.Command(chromium,
		"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
		"--user-data-dir="+filepath.Join(home, "profile"),
		"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
		"--autoplay-policy=no-user-gesture-required",
		"--disable-background-timer-throttling",
		"--disable-renderer-backgrounding",
		r.web.URL+"/page?name="+name+"&until="+strconv.FormatInt(until.UnixMilli(), 10))
	cmd.Env = browserEnvFor(home)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		r.t.Fatal(err)
	}
	r.procs = append(r.procs, cmd)
}

func (r *fmmBrowserRun) wait(names []string, deadline time.Time) map[string]fmmReport {
	for time.Now().Before(deadline) {
		r.mu.Lock()
		done := len(r.reports) == len(names)
		r.mu.Unlock()
		if done {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	out := map[string]fmmReport{}
	for k, v := range r.reports {
		out[k] = v
	}
	return out
}

func fmmBlock(c *core, room, peer, acct string, standby *core) map[string]any {
	port := c.ep.LocalAddr().(*net.UDPAddr).Port
	block := map[string]any{
		"engine": "fmm",
		"url":    "https://127.0.0.1:" + strconv.Itoa(port) + Path,
		"addr":   "127.0.0.1:" + strconv.Itoa(port),
		"alpn":   "fmm/1",
		"hashes": c.certs.Hashes(),
		"ticket": ticketFor(room, peer, acct, false),
	}
	if standby != nil {
		sport := standby.ep.LocalAddr().(*net.UDPAddr).Port
		block["standby"] = map[string]any{
			"url":    "https://127.0.0.1:" + strconv.Itoa(sport) + Path,
			"addr":   "127.0.0.1:" + strconv.Itoa(sport),
			"alpn":   "fmm/1",
			"hashes": standby.certs.Hashes(),
			"ticket": ticketFor(room, peer, acct, true),
		}
	}
	return block
}

func checkFMMCall(t *testing.T, names []string, reports map[string]fmmReport, minVideo, minAudio int) {
	t.Helper()
	if len(reports) != len(names) {
		t.Fatalf("only %d of %d browsers reported", len(reports), len(names))
	}
	codes := map[string]bool{}
	for _, name := range names {
		rep := reports[name]
		if rep.Error != "" {
			t.Fatalf("%s failed: %s", name, rep.Error)
		}
		if rep.Engine != "fmm" {
			t.Fatalf("%s is on engine %q", name, rep.Engine)
		}
		videos, audios := 0, 0
		for _, in := range rep.Sample.Inbound {
			t.Logf("%s <- %s %s: decoded %d (keys %d, height %d, %d bytes), audio %d/%d concealed (statuses %v, target %.0f ms, buffer %v), seal drops %d, freezes %d (%.1f s)",
				name, in.Peer[:6], in.Kind, in.FramesDecoded, in.KeyFramesDecoded, in.FrameHeight, in.BytesReceived,
				in.ConcealedSamples/960, in.TotalSamplesReceived/960, in.Statuses, in.TargetMs, in.Buffer, in.SealDrops, in.FreezeCount, in.TotalFreezesDuration)
			if in.Kind == "video" && in.FramesDecoded >= minVideo {
				videos++
			}
			if in.Kind == "audio" && (in.TotalSamplesReceived-in.ConcealedSamples)/960 >= minAudio {
				audios++
			}
		}
		for _, tile := range rep.Tiles {
			t.Logf("%s tile %s (%s): auth %s, rep %s, tracks %d", name, tile.Name, tile.Peer[:6], tile.Auth, tile.Rep, tile.Tracks)
			if tile.Auth != "verified" {
				t.Errorf("%s sees %s's media as %q, want verified", name, tile.Name, tile.Auth)
			}
		}
		if rep.Verification == nil || rep.Verification.Code == nil {
			t.Errorf("%s has no verification code: %+v", name, rep.Verification)
		} else {
			codes[rep.Verification.Code.EmojiText+" "+rep.Verification.Code.Digits] = true
			t.Logf("%s code %s %s verified=%v", name, rep.Verification.Code.EmojiText, rep.Verification.Code.Digits, rep.Verification.Verified)
		}
		t.Logf("%s: codec %s, counters %v", name, rep.Codec, rep.Sample.Counters)
		if videos != len(names)-1 || audios != len(names)-1 {
			t.Errorf("%s decoded video from %d and audio from %d of %d others", name, videos, audios, len(names)-1)
		}
	}
	if len(codes) != 1 {
		t.Errorf("the browsers show %d different verification codes: %v", len(codes), codes)
	}
}

func TestThreeBrowsersHoldAnFMMediaCall(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	c := startCore(t, "n1", 1)
	room := testRoom(11)
	peers := map[string]string{}
	var mu sync.Mutex
	run := newFMMBrowserRun(t, func(name string) map[string]any {
		mu.Lock()
		defer mu.Unlock()
		peer := signal.NewPeerID()
		peers[name] = peer
		acct := strings.Repeat(string(name[0]), 32)
		return map[string]any{"ticket": "webrtc-unused", "url": "ws://127.0.0.1:1/calls/ws", "room": room, "peer": peer, "fmm": fmmBlock(c, room, peer, acct, nil)}
	})
	names := []string{"ana", "ben", "cai"}
	until := time.Now().Add(20 * time.Second)
	for _, name := range names {
		run.launch(chromium, name, until)
		time.Sleep(1200 * time.Millisecond)
	}
	reports := run.wait(names, until.Add(30*time.Second))
	checkFMMCall(t, names, reports, 100, 150)
}

func TestBrowserFMMediaCallMovesToTheStandbyCore(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	room := testRoom(12)
	ranked := rendezvous(room, []*core{startCore(t, "n1", 1), startCore(t, "n2", 2)})
	primary, standby := ranked[0], ranked[1]
	run := newFMMBrowserRun(t, func(name string) map[string]any {
		peer := signal.NewPeerID()
		acct := strings.Repeat(string(name[0]), 32)
		return map[string]any{"ticket": "webrtc-unused", "url": "ws://127.0.0.1:1/calls/ws", "room": room, "peer": peer, "fmm": fmmBlock(primary, room, peer, acct, standby)}
	})
	names := []string{"ana", "ben", "cai"}
	until := time.Now().Add(24 * time.Second)
	for _, name := range names {
		run.launch(chromium, name, until)
		time.Sleep(1200 * time.Millisecond)
	}
	for i := 0; i < 8; i++ {
		time.Sleep(time.Second)
		t.Logf("t+%ds: primary rooms %d, standby parked %d", i+1, primary.server.Rooms(), standby.server.Parked())
	}
	if primary.server.Rooms() != 1 || standby.server.Parked() != len(names) {
		t.Fatalf("before the death: primary rooms %d, standby parked %d", primary.server.Rooms(), standby.server.Parked())
	}
	killed := time.Now()
	primary.stop()
	t.Logf("primary %s stopped at %s", primary.name, killed.Format("15:04:05.000"))
	reports := run.wait(names, until.Add(30*time.Second))
	if standby.server.Rooms() != 1 {
		t.Fatalf("the call did not move to the standby core: %d rooms", standby.server.Rooms())
	}
	for _, name := range names {
		rep := reports[name]
		if rep.Moved < 1 {
			t.Errorf("%s did not move (state %s)", name, rep.State)
		}
		dead, _ := rep.Sample.Counters["deadAtWall"].(float64)
		var back []string
		for _, in := range rep.Sample.Inbound {
			if in.Kind == "audio" && in.FirstPlayWall > 0 {
				back = append(back, strconv.FormatInt(in.FirstPlayWall-killed.UnixMilli(), 10)+" ms")
			}
		}
		t.Logf("%s: moved %d, judged dead after %d ms (%v), audio back after %v", name, rep.Moved, int64(dead)-killed.UnixMilli(), rep.Sample.Counters["deadReason"], back)
	}
	checkFMMCall(t, names, reports, 20, 50)
}
