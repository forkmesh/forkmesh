package sfu

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/signal"
)

const browserPage = `<!doctype html><meta charset="utf-8"><body><script>
const params = new URLSearchParams(location.search);
const name = params.get("name");
const report = (body) => fetch("/report?name=" + name, {method: "POST", body: JSON.stringify(body)});
const log = (text) => fetch("/log?name=" + name, {method: "POST", body: String(text)});
(async () => {
  try {
    const ticket = await (await fetch("/ticket")).text();
    const media = await navigator.mediaDevices.getUserMedia({audio: true, video: {width: 320, height: 240}});
    const pc = new RTCPeerConnection({bundlePolicy: "max-bundle", rtcpMuxPolicy: "require"});
    pc.addTransceiver(media.getAudioTracks()[0], {direction: "sendonly"});
    pc.addTransceiver(media.getVideoTracks()[0], {direction: "sendonly"});
    const remote = new Map();
    pc.ontrack = (event) => {
      const stream = event.streams[0];
      if (!stream) return;
      remote.set(stream.id, (remote.get(stream.id) || 0) + 1);
      const video = document.createElement("video");
      video.muted = true; video.autoplay = true; video.srcObject = stream;
      document.body.appendChild(video);
    };
    pc.onconnectionstatechange = () => log("connection " + pc.connectionState);
    const ws = new WebSocket("ws://" + location.host + "/calls/ws");
    let self = "";
    let peerCount = 0;
    ws.onmessage = async (event) => {
      const message = JSON.parse(event.data);
      if (message.type === "joined") {
        self = message.peer;
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        ws.send(JSON.stringify({type: "offer", sdp: pc.localDescription.sdp}));
      } else if (message.type === "answer") {
        await pc.setRemoteDescription({type: "answer", sdp: message.sdp});
      } else if (message.type === "offer") {
        await pc.setRemoteDescription({type: "offer", sdp: message.sdp});
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        ws.send(JSON.stringify({type: "answer", sdp: pc.localDescription.sdp, rev: message.rev}));
      } else if (message.type === "error") {
        log("error " + message.error);
      }
    };
    ws.onopen = () => ws.send(JSON.stringify({type: "join", ticket}));
    const deadline = Date.now() + Number(params.get("ms"));
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 500));
    }
    const stats = await pc.getStats();
    const inbound = [];
    let sentAudio = 0, sentVideo = 0;
    stats.forEach((entry) => {
      if (entry.type === "inbound-rtp") {
        inbound.push({kind: entry.kind, ssrc: entry.ssrc, bytes: entry.bytesReceived || 0,
          frames: entry.framesDecoded || 0, packets: entry.packetsReceived || 0});
      }
      if (entry.type === "outbound-rtp") {
        if (entry.kind === "audio") sentAudio += entry.bytesSent || 0;
        if (entry.kind === "video") sentVideo += entry.bytesSent || 0;
      }
    });
    await report({self, inbound, sentAudio, sentVideo, streams: [...remote.keys()],
      state: pc.connectionState});
  } catch (error) {
    await log("failed " + (error && error.stack || error));
    await report({error: String(error)});
  }
})();
</script></body>`

type browserReport struct {
	Self    string   `json:"self"`
	Error   string   `json:"error"`
	State   string   `json:"state"`
	Streams []string `json:"streams"`
	Inbound []struct {
		Kind    string `json:"kind"`
		SSRC    uint32 `json:"ssrc"`
		Bytes   int    `json:"bytes"`
		Frames  int    `json:"frames"`
		Packets int    `json:"packets"`
	} `json:"inbound"`
	SentAudio int `json:"sentAudio"`
	SentVideo int `json:"sentVideo"`
}

func headlessChromium() string {
	if path := os.Getenv("FORKMESH_TEST_CHROMIUM"); path != "" {
		return path
	}
	home, _ := os.UserHomeDir()
	matches, _ := filepath.Glob(filepath.Join(home, ".cache", "ms-playwright", "chromium_headless_shell-*", "chrome-headless-shell-linux64", "chrome-headless-shell"))
	sort.Strings(matches)
	if len(matches) == 0 {
		return ""
	}
	return matches[len(matches)-1]
}

func TestThreeBrowsersShareAudioAndVideoThroughTheSFU(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
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
	room := signal.RoomID(secret, "0123456789abcdef0123456789abcdef")
	var mu sync.Mutex
	reports := map[string]browserReport{}
	routes := http.NewServeMux()
	routes.Handle("/calls/", server.Handler())
	routes.HandleFunc("/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, browserPage)
	})
	routes.HandleFunc("/ticket", func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, signal.MintTicket(secret, signal.Ticket{
			Room: room, Peer: signal.NewPeerID(), Acct: signal.NewPeerID(), Exp: time.Now().Add(signal.TicketLifetime).Unix(),
		}))
	})
	routes.HandleFunc("/log", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		t.Logf("%s: %s", r.URL.Query().Get("name"), body)
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, r *http.Request) {
		var report browserReport
		json.NewDecoder(r.Body).Decode(&report)
		mu.Lock()
		reports[r.URL.Query().Get("name")] = report
		mu.Unlock()
	})
	web := httptest.NewServer(routes)
	defer web.Close()
	names := []string{"ana", "ben", "cai"}
	var processes []*exec.Cmd
	for index, name := range names {
		profile := t.TempDir()
		stay := 14000 - index*1500
		command := exec.CommandContext(ctx, chromium,
			"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
			"--user-data-dir="+profile,
			"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
			"--allow-loopback-in-peer-connection",
			"--disable-features=WebRtcHideLocalIpsWithMdns",
			"--autoplay-policy=no-user-gesture-required",
			web.URL+"/page?name="+name+"&ms="+strconv.Itoa(stay))
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
	deadline := time.Now().Add(40 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		done := len(reports) == len(names)
		mu.Unlock()
		if done {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(reports) != len(names) {
		t.Fatalf("only %d of %d browsers reported: %+v", len(reports), len(names), reports)
	}
	for _, name := range names {
		report := reports[name]
		if report.Error != "" {
			t.Fatalf("%s failed: %s", name, report.Error)
		}
		if report.SentAudio == 0 || report.SentVideo == 0 {
			t.Errorf("%s sent nothing: %+v", name, report)
		}
		audio, video := 0, 0
		for _, stream := range report.Inbound {
			if stream.Kind == "audio" && stream.Bytes > 1000 {
				audio++
			}
			if stream.Kind == "video" && stream.Frames > 10 {
				video++
			}
		}
		if audio != len(names)-1 || video != len(names)-1 || len(report.Streams) != len(names)-1 {
			t.Errorf("%s heard %d audio and saw %d video streams from %d peers: %+v", name, audio, video, len(report.Streams), report)
		}
		t.Logf("%s: state %s, inbound %+v", name, report.State, report.Inbound)
	}
}
