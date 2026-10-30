package webtransport

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/json"
	"io"
	"math/big"
	mrand "math/rand"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/h3"
	"forkmesh.com/mirror-node/rtc/quic"
)

const browserPage = `<!doctype html><meta charset="utf-8"><body><script>
const params = new URLSearchParams(location.search);
const log = (text) => fetch("/log", {method: "POST", body: String(text)});
const report = (body) => fetch("/report", {method: "POST", body: JSON.stringify(body)});
const withTimeout = (promise, ms) => Promise.race([promise, new Promise((_, reject) => setTimeout(() => reject(new Error("timeout")), ms))]);
(async () => {
  const result = {datagrams: 0, stream: "", uni: "", serverStream: "", error: ""};
  try {
    const hash = Uint8Array.from(atob(params.get("hash")), (c) => c.charCodeAt(0));
    const wt = new WebTransport(params.get("url"), {serverCertificateHashes: [{algorithm: "sha-256", value: hash}]});
    await withTimeout(wt.ready, 10000);
    log("ready");
    const dw = wt.datagrams.writable.getWriter();
    const dr = wt.datagrams.readable.getReader();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    const got = new Set();
    const reading = (async () => {
      while (got.size < 10) {
        const {value, done} = await dr.read();
        if (done) break;
        got.add(decoder.decode(value));
      }
    })();
    for (let round = 0; round < 20 && got.size < 10; round++) {
      for (let i = 0; i < 10; i++) {
        if (!got.has("echo:dgram-" + i)) await dw.write(encoder.encode("dgram-" + i));
      }
      await new Promise((r) => setTimeout(r, 100));
    }
    result.datagrams = got.size;
    log("datagrams " + got.size);
    const stream = await withTimeout(wt.createBidirectionalStream(), 5000);
    const sw = stream.writable.getWriter();
    const big = "x".repeat(200000);
    await sw.write(encoder.encode("hello stream|" + big));
    await sw.close();
    let text = "";
    const sr = stream.readable.getReader();
    for (;;) {
      const {value, done} = await withTimeout(sr.read(), 10000);
      if (done) break;
      text += decoder.decode(value, {stream: true});
    }
    result.stream = text.length > 64 ? text.slice(0, 18) + ":" + text.length : text;
    log("stream echoed " + text.length);
    const uni = await wt.createUnidirectionalStream();
    const uw = uni.getWriter();
    await uw.write(encoder.encode("uni hello"));
    await uw.close();
    const incoming = wt.incomingUnidirectionalStreams.getReader();
    const {value: inStream} = await withTimeout(incoming.read(), 5000);
    const ir = inStream.getReader();
    let utext = "";
    for (;;) {
      const {value, done} = await withTimeout(ir.read(), 5000);
      if (done) break;
      utext += decoder.decode(value, {stream: true});
    }
    result.uni = utext;
    const incomingBidi = wt.incomingBidirectionalStreams.getReader();
    const {value: sb} = await withTimeout(incomingBidi.read(), 5000);
    const sbr = sb.readable.getReader();
    let stext = "";
    for (;;) {
      const {value, done} = await withTimeout(sbr.read(), 5000);
      if (done) break;
      stext += decoder.decode(value, {stream: true});
    }
    const sbw = sb.writable.getWriter();
    await sbw.write(encoder.encode("browser reply"));
    await sbw.close();
    result.serverStream = stext;
    wt.close({closeCode: 7, reason: "done"});
    await withTimeout(wt.closed, 5000).catch((e) => log("closed: " + e));
  } catch (error) {
    result.error = String(error && error.stack || error);
  }
  await report(result);
})();
</script></body>`

type browserResult struct {
	Datagrams    int    `json:"datagrams"`
	Stream       string `json:"stream"`
	Uni          string `json:"uni"`
	ServerStream string `json:"serverStream"`
	Error        string `json:"error"`
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

func shortLivedCert(t testing.TB) (tls.Certificate, []byte) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(now.UnixNano()),
		Subject:      pkix.Name{CommonName: "forkmesh webtransport test"},
		NotBefore:    now.Add(-time.Hour),
		NotAfter:     now.Add(10 * 24 * time.Hour),
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(der)
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, sum[:]
}

func echoHandler(replies chan<- string) func(*Session) {
	return func(sess *Session) {
		ctx := sess.Context()
		go func() {
			for {
				d, err := sess.ReceiveDatagram(ctx)
				if err != nil {
					return
				}
				sess.SendDatagram(append([]byte("echo:"), d...))
			}
		}()
		go func() {
			for {
				st, err := sess.AcceptUniStream(ctx)
				if err != nil {
					return
				}
				go func() {
					data, _ := io.ReadAll(st)
					out, err := sess.OpenUniStream(ctx)
					if err != nil {
						return
					}
					out.Write(append([]byte("server got: "), data...))
					out.Close()
					sb, err := sess.OpenStream(ctx)
					if err != nil {
						return
					}
					sb.Write([]byte("server bidi"))
					sb.Close()
					reply, _ := io.ReadAll(sb)
					if replies != nil {
						select {
						case replies <- string(reply):
						default:
						}
					}
				}()
			}
		}()
		for {
			st, err := sess.AcceptStream(ctx)
			if err != nil {
				return
			}
			go func() {
				io.Copy(st, st)
				st.Close()
			}()
		}
	}
}

func TestChromiumWebTransportEcho(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	t.Run("direct", func(t *testing.T) { runChromiumEcho(t, chromium, false, 0) })
	t.Run("retry", func(t *testing.T) { runChromiumEcho(t, chromium, true, 0) })
	t.Run("lossy", func(t *testing.T) { runChromiumEcho(t, chromium, false, 0.05) })
}

type dropConn struct {
	net.PacketConn
	mu   sync.Mutex
	rng  *mrand.Rand
	rate float64
	drop int
}

func (d *dropConn) lose() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.rng.Float64() < d.rate {
		d.drop++
		return true
	}
	return false
}

func (d *dropConn) WriteTo(b []byte, addr net.Addr) (int, error) {
	if d.lose() {
		return len(b), nil
	}
	return d.PacketConn.WriteTo(b, addr)
}

func (d *dropConn) ReadFrom(b []byte) (int, net.Addr, error) {
	for {
		n, addr, err := d.PacketConn.ReadFrom(b)
		if err != nil || !d.lose() {
			return n, addr, err
		}
	}
}

func runChromiumEcho(t *testing.T, chromium string, retry bool, loss float64) {
	cert, hash := shortLivedCert(t)
	qconf := ServerQUICConfig(&quic.Config{
		TLSConfig:                &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"h3"}},
		MaxIdleTimeout:           30 * time.Second,
		RequireAddressValidation: retry,
	})
	udp, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	var pc net.PacketConn = udp
	var dropper *dropConn
	if loss > 0 {
		dropper = &dropConn{PacketConn: udp, rng: mrand.New(mrand.NewSource(11)), rate: loss}
		pc = dropper
	}
	ep, err := quic.NewEndpoint(pc, qconf, true)
	if err != nil {
		t.Fatal(err)
	}
	defer ep.Close()
	replies := make(chan string, 1)
	var mu sync.Mutex
	var origins []string
	var closes []error
	var sawRetry bool
	server := &Server{
		Check: func(r *h3.Request) int {
			mu.Lock()
			origins = append(origins, r.Get("origin"))
			mu.Unlock()
			sawRetry = r.Conn.QUIC().Retried()
			peer, _ := r.Conn.PeerSettings(context.Background())
			state := r.Conn.QUIC().ConnectionState()
			t.Logf("chromium settings %v, headers %v, tls suite %s", peer, r.Header, tls.CipherSuiteName(state.CipherSuite))
			if r.Path != "/echo" {
				return 404
			}
			return 200
		},
		Handler: func(sess *Session) {
			go func() {
				<-sess.Done()
				mu.Lock()
				closes = append(closes, sess.Err())
				mu.Unlock()
			}()
			echoHandler(replies)(sess)
		},
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Serve(ctx, ep)
	port := ep.LocalAddr().(*net.UDPAddr).Port
	results := make(chan browserResult, 1)
	routes := http.NewServeMux()
	routes.HandleFunc("/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, browserPage)
	})
	routes.HandleFunc("/log", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		t.Logf("browser: %s", body)
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, r *http.Request) {
		var res browserResult
		json.NewDecoder(r.Body).Decode(&res)
		select {
		case results <- res:
		default:
		}
	})
	web := httptest.NewServer(routes)
	defer web.Close()
	target := "https://127.0.0.1:" + strconv.Itoa(port) + "/echo"
	page := web.URL + "/page?hash=" + url.QueryEscape(base64.StdEncoding.EncodeToString(hash)) + "&url=" + url.QueryEscape(target)
	profile := t.TempDir()
	cmd := exec.CommandContext(ctx, chromium,
		"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
		"--user-data-dir="+profile,
		page)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		cmd.Wait()
	}()
	var res browserResult
	select {
	case res = <-results:
	case <-time.After(60 * time.Second):
		t.Fatal("browser did not report")
	}
	t.Logf("browser result: %+v", res)
	if res.Error != "" {
		t.Fatalf("browser error: %s", res.Error)
	}
	if res.Datagrams < 10 {
		t.Errorf("only %d of 10 datagrams echoed", res.Datagrams)
	}
	if res.Stream != "hello stream|xxxxx:200013" {
		t.Errorf("stream echo %q", res.Stream)
	}
	if res.Uni != "server got: uni hello" {
		t.Errorf("uni stream %q", res.Uni)
	}
	if res.ServerStream != "server bidi" {
		t.Errorf("server-opened stream %q", res.ServerStream)
	}
	select {
	case r := <-replies:
		if r != "browser reply" {
			t.Errorf("browser reply on the server stream %q", r)
		}
	case <-time.After(5 * time.Second):
		t.Error("no reply on the server-opened stream")
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		n := len(closes)
		mu.Unlock()
		if n > 0 {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	t.Logf("origins %v, session ends %v", origins, closes)
	if len(origins) == 0 || origins[0] != web.URL {
		t.Errorf("origin %v, want %s", origins, web.URL)
	}
	if len(closes) == 0 {
		t.Errorf("session close not seen")
	} else if se, ok := closes[0].(*SessionError); !ok || se.Code != 7 || se.Message != "done" {
		t.Errorf("session close %v", closes[0])
	}
	if retry != sawRetry {
		t.Errorf("retry used %v, want %v", sawRetry, retry)
	}
	if dropper != nil {
		dropper.mu.Lock()
		t.Logf("dropped %d packets", dropper.drop)
		dropper.mu.Unlock()
	}
}
