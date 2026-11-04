package sfu

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/signal"
)

const meetingDriverPage = `<!doctype html><meta charset="utf-8"><body style="margin:0"><script type="module">
const params = new URLSearchParams(location.search);
const name = params.get("name");
const other = params.get("other");
const post = (path, body) => fetch(path + "?name=" + encodeURIComponent(name), {method: "POST", body: typeof body === "string" ? body : JSON.stringify(body)});
const log = (text) => post("/log", String(text));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const errors = [];
const frame = document.createElement("iframe");
frame.allow = "camera; microphone; autoplay";
frame.style.cssText = "width:1280px;height:800px;border:0";
frame.src = "/call?" + params.get("room") + "#" + params.get("key");
document.body.append(frame);
await new Promise((resolve) => frame.addEventListener("load", resolve, {once: true}));
const win = frame.contentWindow;
const doc = frame.contentDocument;
win.addEventListener("error", (event) => errors.push("error: " + (event.message || event.filename || event.target?.src || "")), true);
win.addEventListener("unhandledrejection", (event) => errors.push("rejection: " + String(event.reason?.stack || event.reason)));
const consoleError = win.console.error.bind(win.console);
win.console.error = (...args) => { errors.push("console: " + args.map(String).join(" ")); consoleError(...args); };
const q = (key) => doc.querySelector('[data-mr="' + key + '"]');
const text = (key) => q(key)?.textContent || "";
const snapshot = () => ({
  phase: doc.body.dataset.phase || "",
  status: text("status"),
  joinError: q("join-error")?.hidden ? "" : text("join-error"),
  message: q("message")?.hidden ? "" : text("message-title") + ": " + text("message-text"),
  lock: text("lock-label"),
  tiles: q("grid")?.children.length || 0,
  captions: [...(q("grid")?.querySelectorAll("figcaption") || [])].map((node) => node.textContent),
  effects: text("effects-note"),
  faces: q("face") ? !q("face").disabled : false,
  errors,
});
const until = async (test, ms, what) => {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (test()) return;
    await sleep(100);
  }
  throw new Error("timed out waiting for " + what);
};
const remoteTile = () => [...(q("grid")?.children || [])].find((node) => !node.classList.contains("is-self")) || null;
try {
  await until(() => doc.body.dataset.phase === "lobby", 15000, "the lobby");
  const input = q("name");
  input.value = name;
  input.dispatchEvent(new win.Event("input", {bubbles: true}));
  await until(() => !q("join").disabled, 25000, "the join button");
  await log("lobby ready, faces " + snapshot().faces + ", note: " + text("effects-note"));
  q("join").click();
  await until(() => doc.body.dataset.phase === "call", 15000, "the call");
  await log("in the call");
  await until(() => {
    const tile = remoteTile();
    const video = tile?.querySelector("video");
    return q("grid").children.length === 2 && video && video.videoWidth > 0
      && tile.querySelector("figcaption").textContent === other
      && text("lock-label") === "End-to-end encrypted";
  }, 30000, "the other person's decoded video");
  const tile = remoteTile();
  const video = tile.querySelector("video");
  const startTime = video.currentTime;
  const startFrames = video.getVideoPlaybackQuality().totalVideoFrames;
  const tracks = video.srcObject.getAudioTracks();
  let peak = 0;
  if (tracks.length) {
    const context = new win.AudioContext();
    await context.resume().catch(() => {});
    const source = context.createMediaStreamSource(new win.MediaStream(tracks));
    const analyser = context.createAnalyser();
    analyser.fftSize = 2048;
    source.connect(analyser);
    const samples = new Float32Array(analyser.fftSize);
    const end = Date.now() + 3000;
    while (Date.now() < end) {
      analyser.getFloatTimeDomainData(samples);
      for (const value of samples) peak = Math.max(peak, Math.abs(value));
      await sleep(20);
    }
    context.close();
  } else {
    await sleep(3000);
  }
  await post("/report", {
    ok: true,
    width: video.videoWidth,
    height: video.videoHeight,
    advanced: video.currentTime - startTime,
    frames: video.getVideoPlaybackQuality().totalVideoFrames - startFrames,
    caption: tile.querySelector("figcaption").textContent,
    audioTracks: tracks.length,
    audioLive: tracks.length > 0 && tracks.every((track) => track.readyState === "live"),
    audioPeak: peak,
    code: text("code"),
    snapshot: snapshot(),
  });
} catch (error) {
  await post("/report", {ok: false, error: String(error?.stack || error), snapshot: snapshot()});
}
</script></body>`

type meetingReport struct {
	OK          bool    `json:"ok"`
	Error       string  `json:"error"`
	Width       int     `json:"width"`
	Height      int     `json:"height"`
	Advanced    float64 `json:"advanced"`
	Frames      int     `json:"frames"`
	Caption     string  `json:"caption"`
	AudioTracks int     `json:"audioTracks"`
	AudioLive   bool    `json:"audioLive"`
	AudioPeak   float64 `json:"audioPeak"`
	Code        string  `json:"code"`
	Snapshot    struct {
		Phase     string   `json:"phase"`
		Status    string   `json:"status"`
		JoinError string   `json:"joinError"`
		Message   string   `json:"message"`
		Lock      string   `json:"lock"`
		Tiles     int      `json:"tiles"`
		Captions  []string `json:"captions"`
		Effects   string   `json:"effects"`
		Faces     bool     `json:"faces"`
		Errors    []string `json:"errors"`
	} `json:"snapshot"`
}

type meetingBox struct {
	Seq int    `json:"seq"`
	Box string `json:"box"`
}

func randomHex(t *testing.T, size int) string {
	raw := make([]byte, size)
	if _, err := rand.Read(raw); err != nil {
		t.Fatal(err)
	}
	return hex.EncodeToString(raw)
}

func TestTwoBrowsersHoldAnEncryptedMeetingRoom(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	public, err := filepath.Abs(filepath.Join("..", "..", "..", "app", "public"))
	if err != nil {
		t.Fatal(err)
	}
	callPage, err := os.ReadFile(filepath.Join(public, "call.html"))
	if err != nil {
		t.Skip("app/public is not beside the server module")
	}
	callPage = bytes.ReplaceAll(callPage, []byte("${version}"), []byte("dev"))
	secret := []byte("browser-test-secret-0123456789abcdef")
	mux, err := ice.Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer mux.Close()
	go mux.Serve()
	server, err := NewServer(Config{
		Secret:      secret,
		Addresses:   []netip.Addr{netip.MustParseAddr("127.0.0.1")},
		AllowOrigin: func(string) bool { return true },
	}, mux)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Run(ctx)
	meeting := randomHex(t, 16)
	key := randomHex(t, 32)
	sfuRoom := signal.RoomID(secret, meeting)
	var mu sync.Mutex
	reports := map[string]meetingReport{}
	var boxes []meetingBox
	tickets := 0
	apiPrefix := "/api/booking/-/call/" + meeting
	reply := func(w http.ResponseWriter, status int, body any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(status)
		json.NewEncoder(w).Encode(body)
	}
	routes := http.NewServeMux()
	routes.Handle("/calls/", server.Handler())
	routes.Handle("/", http.FileServer(http.Dir(public)))
	routes.HandleFunc("/call", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(callPage)
	})
	routes.HandleFunc("/drive", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		io.WriteString(w, meetingDriverPage)
	})
	routes.HandleFunc("/api/booking/-/call/", func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path
		if !strings.HasPrefix(path, apiPrefix) {
			reply(w, http.StatusNotFound, map[string]any{"error": "room_not_found"})
			return
		}
		now := time.Now().UnixMilli()
		switch action := strings.TrimPrefix(path, apiPrefix); {
		case action == "" && r.Method == http.MethodGet:
			start := now - 5*60*1000
			reply(w, http.StatusOK, map[string]any{"ok": true, "room": map[string]any{
				"state": "open", "title": "Meeting", "host": "alice", "accent": "#2563eb", "minutes": 30,
				"startAt": start, "endAt": start + 30*60*1000, "opensAt": start - 30*60*1000,
				"closesAt": start + 150*60*1000, "timezone": "UTC", "now": now,
			}})
		case action == "/ticket" && r.Method == http.MethodPost:
			peer := signal.NewPeerID()
			mu.Lock()
			tickets++
			mu.Unlock()
			reply(w, http.StatusOK, map[string]any{
				"ok": true,
				"ticket": signal.MintTicket(secret, signal.Ticket{
					Room: sfuRoom, Peer: peer, Acct: signal.NewPeerID(), Exp: time.Now().Add(signal.TicketLifetime).Unix(),
				}),
				"room": sfuRoom,
				"peer": peer,
				"url":  "ws://" + r.Host + "/calls/ws",
			})
		case action == "/signal" && r.Method == http.MethodPost:
			var body struct {
				Box string `json:"box"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Box == "" {
				reply(w, http.StatusBadRequest, map[string]any{"error": "invalid_signal"})
				return
			}
			mu.Lock()
			boxes = append(boxes, meetingBox{Seq: len(boxes) + 1, Box: body.Box})
			mu.Unlock()
			reply(w, http.StatusAccepted, map[string]any{"ok": true})
		case action == "/signal" && r.Method == http.MethodGet:
			after, _ := strconv.Atoi(r.URL.Query().Get("after"))
			mu.Lock()
			frames := []meetingBox{}
			for _, box := range boxes {
				if box.Seq > after {
					frames = append(frames, box)
				}
			}
			latest := len(boxes)
			mu.Unlock()
			reply(w, http.StatusOK, map[string]any{"ok": true, "frames": frames, "seq": latest})
		default:
			reply(w, http.StatusMethodNotAllowed, map[string]any{"error": "method_not_allowed"})
		}
	})
	routes.HandleFunc("/log", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		t.Logf("%s: %s", r.URL.Query().Get("name"), body)
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		var report meetingReport
		if err := json.Unmarshal(body, &report); err != nil {
			report.Error = "unreadable report: " + string(body)
		}
		mu.Lock()
		reports[r.URL.Query().Get("name")] = report
		mu.Unlock()
	})
	web := httptest.NewServer(routes)
	defer web.Close()
	people := [][2]string{{"Ana", "Ben"}, {"Ben", "Ana"}}
	started := time.Now()
	var processes []*exec.Cmd
	for _, pair := range people {
		query := url.Values{"name": {pair[0]}, "other": {pair[1]}, "room": {meeting}, "key": {key}}
		command := exec.CommandContext(ctx, chromium,
			"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
			"--user-data-dir="+t.TempDir(),
			"--window-size=1280,800",
			"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
			"--allow-loopback-in-peer-connection",
			"--disable-features=WebRtcHideLocalIpsWithMdns",
			"--autoplay-policy=no-user-gesture-required",
			web.URL+"/drive?"+query.Encode())
		command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
		if err := command.Start(); err != nil {
			t.Fatal(err)
		}
		processes = append(processes, command)
		time.Sleep(1500 * time.Millisecond)
	}
	defer func() {
		for _, process := range processes {
			syscall.Kill(-process.Process.Pid, syscall.SIGKILL)
			process.Wait()
		}
	}()
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		done := len(reports) == len(people)
		mu.Unlock()
		if done {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	t.Logf("both browsers reported %.1fs after the first started; %d tickets, %d sealed boxes relayed", time.Since(started).Seconds(), tickets, len(boxes))
	if len(reports) != len(people) {
		t.Fatalf("only %d of %d browsers reported: %+v", len(reports), len(people), reports)
	}
	for _, box := range boxes {
		raw, err := base64.RawURLEncoding.DecodeString(box.Box)
		if err != nil {
			t.Errorf("box %d is not base64url: %v", box.Seq, err)
			continue
		}
		for _, clear := range []string{`"n":"`, "group-call", `"peer"`, `"key"`} {
			if bytes.Contains(raw, []byte(clear)) {
				t.Errorf("box %d shows %q to the relay", box.Seq, clear)
			}
		}
	}
	codes := map[string]bool{}
	for _, pair := range people {
		name, other := pair[0], pair[1]
		report := reports[name]
		t.Logf("%s: %+v", name, report)
		if !report.OK {
			t.Errorf("%s did not see %s: %s", name, other, report.Error)
			continue
		}
		codes[report.Code] = true
		if report.Width == 0 || report.Frames < 10 || report.Advanced <= 0 {
			t.Errorf("%s did not decode %s's video: %dx%d, %d frames, %.2fs", name, other, report.Width, report.Height, report.Frames, report.Advanced)
		}
		if report.Caption != other || report.Snapshot.Tiles != 2 || report.Snapshot.Lock != "End-to-end encrypted" {
			t.Errorf("%s's call view is wrong: caption %q, %d tiles, lock %q", name, report.Caption, report.Snapshot.Tiles, report.Snapshot.Lock)
		}
		if report.AudioTracks != 1 || !report.AudioLive || report.AudioPeak < 0.01 {
			t.Errorf("%s did not hear %s: %d tracks, live %v, peak %.4f", name, other, report.AudioTracks, report.AudioLive, report.AudioPeak)
		}
		if len(report.Snapshot.Errors) > 0 {
			t.Logf("%s page errors: %v", name, report.Snapshot.Errors)
		}
	}
	if len(codes) != 1 {
		t.Errorf("the two people see different meeting codes: %v", codes)
	}
}
