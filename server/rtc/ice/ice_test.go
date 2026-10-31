package ice

import (
	"crypto/rand"
	"net"
	"net/netip"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/stun"
)

type received struct {
	mu      sync.Mutex
	packets [][]byte
	kinds   []int
}

func (r *received) handle(kind int, packet []byte) {
	r.mu.Lock()
	r.kinds = append(r.kinds, kind)
	r.packets = append(r.packets, packet)
	r.mu.Unlock()
}

func (r *received) count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.packets)
}

func startMux(t *testing.T) *Mux {
	t.Helper()
	mux, err := Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go mux.Serve()
	t.Cleanup(func() { mux.Close() })
	return mux
}

func client(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func check(username, password string, nominate bool) ([]byte, [12]byte) {
	message := &stun.Message{Type: stun.BindingRequest}
	rand.Read(message.TransactionID[:])
	message.Add(stun.AttrUsername, []byte(username))
	message.Add(stun.AttrPriority, []byte{0x6e, 0x00, 0x1e, 0xff})
	message.Add(stun.AttrICEControlling, make([]byte, 8))
	if nominate {
		message.Add(stun.AttrUseCandidate, nil)
	}
	return message.Encode([]byte(password), true), message.TransactionID
}

const peerPwd = "peer-password-0123456789"

func target(mux *Mux) *net.UDPAddr {
	return &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: mux.Port()}
}

func read(t *testing.T, conn *net.UDPConn) ([]byte, *stun.Message) {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	buffer := make([]byte, 1500)
	n, _, err := conn.ReadFromUDP(buffer)
	if err != nil {
		return nil, nil
	}
	message, err := stun.Parse(buffer[:n])
	if err != nil {
		t.Fatalf("bad stun: %v", err)
	}
	return buffer[:n], message
}

func exchange(t *testing.T, conn *net.UDPConn, mux *Mux, packet []byte) *stun.Message {
	t.Helper()
	conn.WriteToUDP(packet, target(mux))
	_, message := read(t, conn)
	return message
}

func answerCheck(t *testing.T, conn *net.UDPConn, mux *Mux, session *Session) bool {
	t.Helper()
	raw, message := read(t, conn)
	if message == nil || message.Type != stun.BindingRequest {
		return false
	}
	username, _ := message.Get(stun.AttrUsername)
	if string(username) != "peer:"+session.LocalUfrag || !stun.CheckIntegrity(raw, []byte(peerPwd)) {
		t.Fatalf("verification check %q not signed with the peer's password", username)
	}
	response := &stun.Message{Type: stun.BindingSuccess, TransactionID: message.TransactionID}
	local := conn.LocalAddr().(*net.UDPAddr).AddrPort()
	response.AddXORMappedAddress(local)
	conn.WriteToUDP(response.Encode([]byte(peerPwd), true), target(mux))
	return true
}

func connect(t *testing.T, conn *net.UDPConn, mux *Mux, session *Session, nominate bool) {
	t.Helper()
	request, tid := check(session.LocalUfrag+":peer", session.LocalPwd, nominate)
	response := exchange(t, conn, mux, request)
	if response == nil || response.Type != stun.BindingSuccess || response.TransactionID != tid {
		t.Fatalf("response %+v", response)
	}
	if !answerCheck(t, conn, mux, session) {
		t.Fatal("the mux sent no verification check")
	}
}

func waitFor(condition func() bool) bool {
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if condition() {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return condition()
}

func selectedPort(session *Session) int {
	selected, ok := session.Selected()
	if !ok {
		return 0
	}
	return int(selected.Port())
}

func TestAPathIsSelectedOnlyAfterThePeerAnswersOurCheck(t *testing.T) {
	mux := startMux(t)
	got := &received{}
	session := mux.NewSession("peer", peerPwd, got.handle)
	if len(session.LocalUfrag) != 8 || len(session.LocalPwd) != 24 {
		t.Fatalf("credentials %q %q", session.LocalUfrag, session.LocalPwd)
	}
	conn := client(t)
	request, tid := check(session.LocalUfrag+":peer", session.LocalPwd, false)
	response := exchange(t, conn, mux, request)
	if response == nil || response.Type != stun.BindingSuccess || response.TransactionID != tid {
		t.Fatalf("response %+v", response)
	}
	value, _ := response.Get(stun.AttrXORMappedAddress)
	mapped, err := stun.DecodeXORAddress(value, tid)
	local := conn.LocalAddr().(*net.UDPAddr).AddrPort()
	if err != nil || mapped != netip.AddrPortFrom(local.Addr().Unmap(), local.Port()) {
		t.Fatalf("mapped %v %v", mapped, err)
	}
	if _, ok := session.Selected(); ok {
		t.Fatal("a check alone selected the path")
	}
	if !answerCheck(t, conn, mux, session) {
		t.Fatal("no verification check")
	}
	if !waitFor(func() bool { return selectedPort(session) == int(mapped.Port()) }) {
		t.Fatal("an answered check did not select the path")
	}
	conn.WriteToUDP([]byte{22, 254, 253, 0}, target(mux))
	conn.WriteToUDP([]byte{0x80, 111, 0, 1}, target(mux))
	conn.WriteToUDP([]byte{70, 1, 2}, target(mux))
	if !waitFor(func() bool { return got.count() == 2 }) {
		t.Fatalf("delivered %d", got.count())
	}
	if got.kinds[0] != KindDTLS || got.kinds[1] != KindRTP {
		t.Fatalf("kinds %v", got.kinds)
	}
	if err := session.Send([]byte{0x80, 1}); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, 100)
	conn.SetReadDeadline(time.Now().Add(time.Second))
	if n, _, err := conn.ReadFromUDP(buffer); err != nil || n != 2 {
		t.Fatalf("send %d %v", n, err)
	}
}

func TestBadChecksAreIgnoredAndOnlyTheSelectedAddressIsHeard(t *testing.T) {
	mux := startMux(t)
	got := &received{}
	session := mux.NewSession("peer", peerPwd, got.handle)
	conn := client(t)
	wrongPassword, _ := check(session.LocalUfrag+":peer", "not-the-password-at-all!", true)
	wrongPeer, _ := check(session.LocalUfrag+":intruder", session.LocalPwd, true)
	unknown, _ := check("nobody00:peer", session.LocalPwd, true)
	for _, packet := range [][]byte{wrongPassword, wrongPeer, unknown, {0, 1, 0, 0}} {
		if response := exchange(t, conn, mux, packet); response != nil {
			t.Fatalf("answered a bad check: %+v", response)
		}
	}
	if _, ok := session.Selected(); ok {
		t.Fatal("a bad check selected a path")
	}
	if err := session.Send([]byte{1}); err != ErrNoPath {
		t.Fatalf("send without path %v", err)
	}
	connect(t, conn, mux, session, false)
	home := int(conn.LocalAddr().(*net.UDPAddr).Port)
	if !waitFor(func() bool { return selectedPort(session) == home }) {
		t.Fatal("no path")
	}
	stranger := client(t)
	stranger.WriteToUDP([]byte{0x80, 111, 0, 1}, target(mux))
	time.Sleep(50 * time.Millisecond)
	if got.count() != 0 {
		t.Fatal("a stranger's packet was delivered")
	}
	moved := client(t)
	connect(t, moved, mux, session, true)
	away := int(moved.LocalAddr().(*net.UDPAddr).Port)
	if !waitFor(func() bool { return selectedPort(session) == away }) {
		t.Fatal("an answered USE-CANDIDATE did not move the path")
	}
	conn.WriteToUDP([]byte{0x80, 111, 0, 1}, target(mux))
	time.Sleep(50 * time.Millisecond)
	if got.count() != 0 {
		t.Fatal("the old path is still heard")
	}
}

func TestAReplayedCheckFromAnotherAddressCannotStealThePath(t *testing.T) {
	mux := startMux(t)
	session := mux.NewSession("peer", peerPwd, nil)
	conn := client(t)
	connect(t, conn, mux, session, false)
	home := int(conn.LocalAddr().(*net.UDPAddr).Port)
	if !waitFor(func() bool { return selectedPort(session) == home }) {
		t.Fatal("no path")
	}
	captured, _ := check(session.LocalUfrag+":peer", session.LocalPwd, true)
	attacker := client(t)
	if response := exchange(t, attacker, mux, captured); response == nil {
		t.Fatal("a valid check went unanswered")
	}
	_, verify := read(t, attacker)
	if verify == nil || verify.Type != stun.BindingRequest {
		t.Fatal("no verification check to the new address")
	}
	forged := &stun.Message{Type: stun.BindingSuccess, TransactionID: verify.TransactionID}
	forged.AddXORMappedAddress(attacker.LocalAddr().(*net.UDPAddr).AddrPort())
	attacker.WriteToUDP(forged.Encode([]byte("guessed-password-wrong!!"), true), target(mux))
	time.Sleep(50 * time.Millisecond)
	if selectedPort(session) != home {
		t.Fatal("a forged answer moved the path")
	}
}

func TestConsentIsRefreshedByAnsweredProbesAndExpiresWithout(t *testing.T) {
	mux := startMux(t)
	var mu sync.Mutex
	now := time.Unix(1000, 0)
	clock := func() time.Time {
		mu.Lock()
		defer mu.Unlock()
		return now
	}
	advance := func(by time.Duration) time.Time {
		mu.Lock()
		defer mu.Unlock()
		now = now.Add(by)
		return now
	}
	mux.now = clock
	session := mux.NewSession("peer", peerPwd, nil)
	conn := client(t)
	connect(t, conn, mux, session, false)
	if !waitFor(func() bool { return selectedPort(session) != 0 }) {
		t.Fatal("no path")
	}
	for i := 0; i < 8; i++ {
		at := advance(ConsentInterval)
		session.Refresh(at)
		if !answerCheck(t, conn, mux, session) {
			t.Fatalf("probe %d not sent", i)
		}
		time.Sleep(20 * time.Millisecond)
		if session.Expired(at) {
			t.Fatalf("expired after answered probe %d", i)
		}
	}
	at := clock()
	if !session.Expired(at.Add(ConsentTimeout + time.Second)) {
		t.Fatal("never expired without answers")
	}
}

func TestClosedSessionsStopAnswering(t *testing.T) {
	mux := startMux(t)
	session := mux.NewSession("", "", nil)
	conn := client(t)
	request, _ := check(session.LocalUfrag+":anyone", session.LocalPwd, false)
	if exchange(t, conn, mux, request) == nil {
		t.Fatal("no remote ufrag yet should accept any peer")
	}
	session.Close()
	if exchange(t, conn, mux, request) != nil {
		t.Fatal("closed session answered")
	}
	if !session.Expired(time.Now()) {
		t.Fatal("closed session not expired")
	}
}

func TestCandidatesNameThePortAndEveryAddress(t *testing.T) {
	mux := startMux(t)
	lines := mux.Candidates([]netip.Addr{netip.MustParseAddr("203.0.113.5"), netip.MustParseAddr("2001:db8::5")})
	if len(lines) != 2 || !strings.HasPrefix(lines[0], "1 1 udp 2130706431 203.0.113.5 ") ||
		!strings.Contains(lines[1], " 2001:db8::5 ") || !strings.HasSuffix(lines[1], " typ host") {
		t.Fatalf("candidates %v", lines)
	}
}
