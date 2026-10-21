package signal

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

var secret = []byte("calls-secret-for-tests")

func TestTicketRoundTripAndRefusals(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	room := RoomID(secret, "0123456789abcdef0123456789abcdef")
	if len(room) != 64 || room == RoomID([]byte("other"), "0123456789abcdef0123456789abcdef") {
		t.Fatalf("room id %q", room)
	}
	ticket := Ticket{Room: room, Peer: NewPeerID(), Acct: NewPeerID(), Exp: now.Add(TicketLifetime).Unix()}
	token := MintTicket(secret, ticket)
	got, err := VerifyTicket(secret, token, now)
	if err != nil || got != ticket {
		t.Fatalf("verify %v %v", got, err)
	}
	if _, err := VerifyTicket([]byte("wrong"), token, now); err != ErrTicketSignature {
		t.Fatalf("wrong secret %v", err)
	}
	if _, err := VerifyTicket(secret, token, now.Add(2*TicketLifetime)); err != ErrTicketExpired {
		t.Fatalf("expired %v", err)
	}
	far := MintTicket(secret, Ticket{Room: room, Peer: ticket.Peer, Acct: ticket.Acct, Exp: now.Add(time.Hour).Unix()})
	if _, err := VerifyTicket(secret, far, now); err != ErrTicketExpired {
		t.Fatalf("too long lived %v", err)
	}
	payload, signature, _ := strings.Cut(token, ".")
	flipped := []byte(payload)
	flipped[3] ^= 1
	if _, err := VerifyTicket(secret, string(flipped)+"."+signature, now); err == nil {
		t.Fatal("tampered payload accepted")
	}
	bad := MintTicket(secret, Ticket{Room: "../room", Peer: ticket.Peer, Acct: ticket.Acct, Exp: ticket.Exp})
	if _, err := VerifyTicket(secret, MintTicket(secret, Ticket{Room: room, Peer: ticket.Peer, Exp: ticket.Exp}), now); err != ErrTicketMalformed {
		t.Fatalf("ticket without an account %v", err)
	}
	if _, err := VerifyTicket(secret, bad, now); err != ErrTicketMalformed {
		t.Fatalf("bad room %v", err)
	}
	for _, junk := range []string{"", ".", "abc", "a.b.c", strings.Repeat("a", 2000)} {
		if _, err := VerifyTicket(secret, junk, now); err == nil {
			t.Fatalf("junk %q accepted", junk)
		}
	}
}

type testClient struct {
	conn   net.Conn
	reader *bufio.Reader
}

func dial(t *testing.T, server *httptest.Server, origin string, queries ...string) (*testClient, int) {
	query := ""
	if len(queries) > 0 {
		query = queries[0]
	}
	t.Helper()
	conn, err := net.Dial("tcp", strings.TrimPrefix(server.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	var raw [16]byte
	rand.Read(raw[:])
	key := base64.StdEncoding.EncodeToString(raw[:])
	request := "GET /calls/ws" + query + " HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n" +
		"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\nOrigin: " + origin + "\r\n\r\n"
	conn.Write([]byte(request))
	reader := bufio.NewReader(conn)
	response, err := http.ReadResponse(reader, nil)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode == http.StatusSwitchingProtocols {
		digest := sha1.Sum([]byte(key + websocketGUID))
		if response.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(digest[:]) {
			t.Fatal("bad accept")
		}
	}
	return &testClient{conn: conn, reader: reader}, response.StatusCode
}

func (c *testClient) send(op byte, fin bool, payload []byte, masked bool) {
	first := op
	if fin {
		first |= 0x80
	}
	frame := []byte{first}
	maskBit := byte(0)
	if masked {
		maskBit = 0x80
	}
	switch {
	case len(payload) < 126:
		frame = append(frame, maskBit|byte(len(payload)))
	case len(payload) <= 0xFFFF:
		frame = append(frame, maskBit|126, byte(len(payload)>>8), byte(len(payload)))
	default:
		frame = append(frame, maskBit|127)
		frame = binary.BigEndian.AppendUint64(frame, uint64(len(payload)))
	}
	body := append([]byte(nil), payload...)
	if masked {
		mask := []byte{0x12, 0x34, 0x56, 0x78}
		frame = append(frame, mask...)
		for i := range body {
			body[i] ^= mask[i%4]
		}
	}
	c.conn.Write(append(frame, body...))
}

func (c *testClient) receive(t *testing.T) (byte, []byte) {
	t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	var head [2]byte
	if _, err := io.ReadFull(c.reader, head[:]); err != nil {
		t.Fatal(err)
	}
	length := int(head[1] & 0x7F)
	if length == 126 {
		var ext [2]byte
		io.ReadFull(c.reader, ext[:])
		length = int(binary.BigEndian.Uint16(ext[:]))
	} else if length == 127 {
		var ext [8]byte
		io.ReadFull(c.reader, ext[:])
		length = int(binary.BigEndian.Uint64(ext[:]))
	}
	payload := make([]byte, length)
	io.ReadFull(c.reader, payload)
	return head[0] & 0x0F, payload
}

func echoServer(t *testing.T, results chan error) *httptest.Server {
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := Upgrade(w, r, func(origin string) bool { return origin == "https://app.example" })
		if err != nil {
			results <- err
			return
		}
		defer conn.Close()
		if r.URL.Query().Get("joined") != "" {
			conn.SetLimit(MaxMessageBytes)
		}
		for {
			var message map[string]any
			if err := conn.ReadJSON(&message); err != nil {
				results <- err
				return
			}
			message["echo"] = true
			if err := conn.WriteJSON(message); err != nil {
				results <- err
				return
			}
		}
	}))
}

func TestWebSocketEchoesJSONAndAnswersPings(t *testing.T) {
	results := make(chan error, 4)
	server := echoServer(t, results)
	defer server.Close()
	client, status := dial(t, server, "https://app.example", "?joined=1")
	if status != http.StatusSwitchingProtocols {
		t.Fatalf("status %d", status)
	}
	client.send(opPing, true, []byte("hi"), true)
	if op, payload := client.receive(t); op != opPong || string(payload) != "hi" {
		t.Fatalf("pong %d %q", op, payload)
	}
	client.send(opText, false, []byte(`{"type":`), true)
	client.send(opPing, true, nil, true)
	client.receive(t)
	client.send(opContinuation, true, []byte(`"join"}`), true)
	op, payload := client.receive(t)
	var reply map[string]any
	json.Unmarshal(payload, &reply)
	if op != opText || reply["type"] != "join" || reply["echo"] != true {
		t.Fatalf("echo %d %s", op, payload)
	}
	big := `{"type":"offer","sdp":"` + strings.Repeat("x", 60000) + `"}`
	client.send(opText, true, []byte(big), true)
	if op, payload := client.receive(t); op != opText || len(payload) < 60000 {
		t.Fatalf("big echo %d %d", op, len(payload))
	}
	client.send(opClose, true, nil, true)
	if err := <-results; err != ErrClosed {
		t.Fatalf("close %v", err)
	}
}

func TestWebSocketRefusesBadOriginUnmaskedAndOversize(t *testing.T) {
	results := make(chan error, 4)
	server := echoServer(t, results)
	defer server.Close()
	if _, status := dial(t, server, "https://evil.example"); status != http.StatusForbidden {
		t.Fatalf("origin status %d", status)
	}
	if err := <-results; err != ErrOrigin {
		t.Fatalf("origin %v", err)
	}
	client, _ := dial(t, server, "https://app.example")
	client.send(opText, true, []byte(`{}`), false)
	if err := <-results; err != ErrProtocol {
		t.Fatalf("unmasked %v", err)
	}
	client, _ = dial(t, server, "https://app.example")
	client.send(opText, true, make([]byte, JoinMessageBytes+1), true)
	if err := <-results; err != ErrMessageTooBig {
		t.Fatalf("oversize before join %v", err)
	}
	client, _ = dial(t, server, "https://app.example", "?joined=1")
	client.send(opText, true, make([]byte, MaxMessageBytes+1), true)
	if err := <-results; err != ErrMessageTooBig {
		t.Fatalf("oversize %v", err)
	}
	client, _ = dial(t, server, "https://app.example")
	client.send(opBinary, true, []byte{1, 2}, true)
	if err := <-results; err != ErrProtocol {
		t.Fatalf("binary %v", err)
	}
}

func TestTheAppServersTicketIsAcceptedAsMinted(t *testing.T) {
	appSecret := []byte("browser-test-secret-0123456789abcdef")
	minted := "eyJyb29tIjoiMjA5MDU0YzFjNjhkMzI1YzE0OWQ2NjdhZGMwZGY1ZjYwYjBmMGIzNTE0YzY1MWVhOGJiODhkNzE1NjQwYjk1ZSIsInBlZXIiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYiIsImFjY3QiOiJkYjQ0MGY1MDE3YTc1YjNjNzM3OTA3ZTIzYzNkNzhmMSIsImV4cCI6MTgwMDAwMDA2MH0._SwoxYSH23v-fsxGRbL66itpXI8mLo_MLfma9EB3U9Y"
	room := RoomID(appSecret, "0123456789abcdef0123456789abcdef")
	if room != "209054c1c68d325c149d667adc0df5f60b0f0b3514c651ea8bb88d715640b95e" {
		t.Fatalf("room %s", room)
	}
	ticket, err := VerifyTicket(appSecret, minted, time.Unix(1_800_000_000, 0))
	if err != nil || ticket.Room != room || ticket.Peer != strings.Repeat("ab", 16) ||
		ticket.Acct != "db440f5017a75b3c737907e23c3d78f1" || ticket.Exp != 1_800_000_060 {
		t.Fatalf("ticket %+v %v", ticket, err)
	}
	if MintTicket(appSecret, ticket) != minted {
		t.Fatal("Go and the app server mint different tickets")
	}
}
