package fmmserver

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/signal"
)

type rawClient struct {
	conn *quic.Conn
	st   *quic.Stream
}

func dialRaw(t *testing.T, c *core, direct bool) *rawClient {
	t.Helper()
	tlsConf, err := fmmcall.PinnedTLS("core", c.certs.Hashes(), nil)
	if err != nil {
		t.Fatal(err)
	}
	addr := c.relay.Addr().String()
	if direct {
		addr = c.ep.LocalAddr().String()
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, err := quic.Dial(ctx, "udp4", addr, &quic.Config{TLSConfig: tlsConf, MaxDatagramFrameSize: 1500})
	if err != nil {
		t.Fatal(err)
	}
	st, err := conn.OpenStreamSync(ctx)
	if err != nil {
		t.Fatal(err)
	}
	r := &rawClient{conn: conn, st: st}
	t.Cleanup(func() { conn.CloseWithError(0, "") })
	return r
}

func (r *rawClient) send(t *testing.T, m wire.Message) {
	t.Helper()
	b, err := wire.Append(nil, m)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := r.st.Write(b); err != nil {
		t.Fatal(err)
	}
}

func (r *rawClient) next(t *testing.T, wait time.Duration) wire.Message {
	t.Helper()
	_ = r.st.SetReadDeadline(time.Now().Add(wait))
	m, err := readMessage(r.st)
	if err != nil {
		return nil
	}
	return m
}

func wantError(t *testing.T, m wire.Message, code int) {
	t.Helper()
	e, ok := m.(wire.Error)
	if !ok || int(e.Code) != code {
		t.Fatalf("got %#v, want error %d", m, code)
	}
}

func TestJoinAdmitsOnlyFMMTicketsAndEnforcesCaps(t *testing.T) {
	c := startCore(t, "n1", 1)
	room := testRoom(0)
	acct := "0123456789abcdef0123456789abcdef"
	peer := func(i int) string { return fmt.Sprintf("%032x", i+1) }

	ok := dialRaw(t, c, true)
	ok.send(t, wire.Join{Ticket: ticketFor(room, peer(0), acct, false)})
	if j, good := ok.next(t, 3*time.Second).(wire.Joined); !good || j.Peer != peer(0) {
		t.Fatalf("joined %#v", j)
	}

	webrtc := dialRaw(t, c, true)
	webrtc.send(t, wire.Join{Ticket: signal.MintTicket(testSecret, signal.Ticket{
		Room: room, Peer: peer(1), Acct: acct, Exp: time.Now().Add(time.Minute).Unix()})})
	wantError(t, webrtc.next(t, 3*time.Second), 401)

	forged := dialRaw(t, c, true)
	forged.send(t, wire.Join{Ticket: signal.MintTicket([]byte("another-secret-0123456789abcdef-xx"), signal.Ticket{
		Room: room, Peer: peer(1), Acct: acct, Exp: time.Now().Add(time.Minute).Unix(), Engine: signal.EngineFMM})})
	wantError(t, forged.next(t, 3*time.Second), 401)

	first := dialRaw(t, c, true)
	first.send(t, wire.Publish{Stream: 1, Kind: wire.TrackAudio, Layers: 1})
	wantError(t, first.next(t, 3*time.Second), 401)

	same := dialRaw(t, c, true)
	same.send(t, wire.Join{Ticket: ticketFor(room, peer(0), acct, false)})
	wantError(t, same.next(t, 3*time.Second), 401)

	for i := 1; i < MaxPerAccount; i++ {
		r := dialRaw(t, c, true)
		r.send(t, wire.Join{Ticket: ticketFor(room, peer(i), acct, false)})
		if _, good := r.next(t, 3*time.Second).(wire.Joined); !good {
			t.Fatalf("participant %d of one account refused", i)
		}
	}
	over := dialRaw(t, c, true)
	over.send(t, wire.Join{Ticket: ticketFor(room, peer(7), acct, false)})
	wantError(t, over.next(t, 3*time.Second), 503)

	if got := c.server.cfg.Engines.Engine(room); got != signal.EngineFMM {
		t.Fatalf("room engine %q", got)
	}
	other := testRoom(1)
	if _, err := c.server.cfg.Engines.Join(other, signal.EngineWebRTC); err != nil {
		t.Fatal(err)
	}
	mixed := dialRaw(t, c, true)
	mixed.send(t, wire.Join{Ticket: ticketFor(other, peer(8), acct, false)})
	m := mixed.next(t, 3*time.Second)
	wantError(t, m, ErrCodeEngine)
	if m.(wire.Error).Text != "engine webrtc" {
		t.Fatalf("engine error text %q", m.(wire.Error).Text)
	}
	if _, err := c.server.cfg.Engines.Join(room, signal.EngineWebRTC); !errors.Is(err, signal.ErrRoomEngine) {
		t.Fatalf("a WebRTC join entered an FM Media room: %v", err)
	}
	if c.server.Rooms() != 1 {
		t.Fatalf("%d rooms", c.server.Rooms())
	}
}

func TestRoomEmptiesAndReleasesItsEngine(t *testing.T) {
	c := startCore(t, "n1", 1)
	room := testRoom(2)
	r := dialRaw(t, c, true)
	r.send(t, wire.Join{Ticket: ticketFor(room, "11111111111111111111111111111111", "22222222222222222222222222222222", false)})
	if _, ok := r.next(t, 3*time.Second).(wire.Joined); !ok {
		t.Fatal("not joined")
	}
	r.send(t, wire.Leave{Reason: "bye"})
	deadline := time.Now().Add(3 * time.Second)
	for c.server.Rooms() != 0 || c.server.cfg.Engines.Engine(room) != "" {
		if time.Now().After(deadline) {
			t.Fatalf("room kept: %d rooms, engine %q", c.server.Rooms(), c.server.cfg.Engines.Engine(room))
		}
		time.Sleep(10 * time.Millisecond)
	}
	if _, err := c.server.cfg.Engines.Join(room, signal.EngineWebRTC); err != nil {
		t.Fatalf("emptied room still claimed: %v", err)
	}
}

func TestStandbyParksUntilItsTicketComesAgain(t *testing.T) {
	c := startCore(t, "n1", 1)
	room := testRoom(3)
	ticket := ticketFor(room, "33333333333333333333333333333333", "44444444444444444444444444444444", true)
	r := dialRaw(t, c, true)
	r.send(t, wire.Join{Ticket: ticket})
	if m := r.next(t, 300*time.Millisecond); m != nil {
		t.Fatalf("parked standby got %#v", m)
	}
	if c.server.Parked() != 1 || c.server.Rooms() != 0 {
		t.Fatalf("parked %d rooms %d", c.server.Parked(), c.server.Rooms())
	}
	r.send(t, wire.Join{Ticket: ticket})
	if j, ok := r.next(t, 3*time.Second).(wire.Joined); !ok || j.Peer != "33333333333333333333333333333333" {
		t.Fatalf("activation %#v", j)
	}
	if c.server.Parked() != 0 || c.server.Rooms() != 1 {
		t.Fatalf("parked %d rooms %d", c.server.Parked(), c.server.Rooms())
	}
	wrong := dialRaw(t, c, true)
	wrong.send(t, wire.Join{Ticket: ticket})
	time.Sleep(50 * time.Millisecond)
	wrong.send(t, wire.Join{Ticket: ticketFor(room, "55555555555555555555555555555555", "44444444444444444444444444444444", false)})
	wantError(t, wrong.next(t, 3*time.Second), 401)
}

func TestPinnedTLSRefusesOtherCertificates(t *testing.T) {
	c := startCore(t, "n1", 1)
	other, err := NewCertStore("", nil)
	if err != nil {
		t.Fatal(err)
	}
	tlsConf, err := fmmcall.PinnedTLS("core", other.Hashes(), nil)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if _, err := quic.Dial(ctx, "udp4", c.ep.LocalAddr().String(), &quic.Config{TLSConfig: tlsConf, MaxDatagramFrameSize: 1500}); err == nil {
		t.Fatal("dial with foreign pins succeeded")
	}
	next, _ := fmmcall.PinnedTLS("core", []string{c.certs.Next().HashBase64()}, nil)
	if _, err := quic.Dial(ctx, "udp4", c.ep.LocalAddr().String(), &quic.Config{TLSConfig: next, MaxDatagramFrameSize: 1500}); err == nil {
		t.Fatal("the not-yet-presented certificate was served")
	}
}
