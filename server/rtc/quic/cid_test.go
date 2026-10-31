package quic

import (
	"context"
	"errors"
	"net"
	"testing"
	"time"
)

func TestPeerConnectionIDRetirement(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	c := p.cliConn
	waitFor(t, "server connection ids", func() bool {
		c.mu.Lock()
		defer c.mu.Unlock()
		return len(c.peerCIDs) >= 2
	})
	c.mu.Lock()
	defer c.mu.Unlock()
	maxSeq := uint64(0)
	for _, pc := range c.peerCIDs {
		if pc.seq > maxSeq {
			maxSeq = pc.seq
		}
	}
	f := &frame{typ: frameNewConnectionID, seq: maxSeq + 1, retirePrior: maxSeq + 1, data: []byte{9, 9, 9, 9, 9, 9, 9, 9}}
	f.resetToken[0] = 7
	if err := c.onNewConnectionID(f); err != nil {
		t.Fatal(err)
	}
	if c.dcidSeq != maxSeq+1 || string(c.dcid) != string(f.data) {
		t.Fatalf("active connection id seq %d after retire_prior_to", c.dcidSeq)
	}
	if len(c.retireQueue) < 2 {
		t.Fatalf("retire queue %v", c.retireQueue)
	}
	again := &frame{typ: frameNewConnectionID, seq: maxSeq + 1, data: []byte{1, 1, 1, 1, 1, 1, 1, 1}}
	if err := c.onNewConnectionID(again); err == nil || err.Code != ProtocolViolation {
		t.Fatalf("reused sequence with a new id: %v", err)
	}
	for i := uint64(0); i < 10; i++ {
		g := &frame{typ: frameNewConnectionID, seq: maxSeq + 2 + i, data: []byte{byte(i), 2, 3, 4, 5, 6, 7, 8}}
		if err := c.onNewConnectionID(g); err != nil {
			if err.Code != ConnectionIDLimitError {
				t.Fatalf("unexpected error %v", err)
			}
			return
		}
	}
	t.Fatal("connection id limit not enforced")
}

func TestRetireConnectionIDChecks(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	c := p.srvConn
	waitFor(t, "issued ids", func() bool {
		c.mu.Lock()
		defer c.mu.Unlock()
		return len(c.localCIDs) >= 2
	})
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.onRetireConnectionID(c.nextLocalSeq+5, nil); err == nil {
		t.Fatal("retiring an unissued id accepted")
	}
	inUse := c.localCIDs[0]
	if err := c.onRetireConnectionID(inUse.seq, inUse.cid); err == nil {
		t.Fatal("retiring the id carrying the frame accepted")
	}
	victim := c.localCIDs[1]
	before := c.activeLocalCIDs()
	if err := c.onRetireConnectionID(victim.seq, inUse.cid); err != nil {
		t.Fatal(err)
	}
	if c.localCIDFor(victim.cid) != nil {
		t.Fatal("retired id still active")
	}
	if c.activeLocalCIDs() != before {
		t.Fatalf("replacement not issued: %d active, was %d", c.activeLocalCIDs(), before)
	}
	c.ep.mu.Lock()
	_, routed := c.ep.conns[string(victim.cid)]
	c.ep.mu.Unlock()
	if routed {
		t.Fatal("retired id still routed")
	}
}

func TestClientRejectsVersionNegotiationWithoutV1(t *testing.T) {
	fake := mustUDP(t)
	defer fake.Close()
	go func() {
		buf := make([]byte, 2048)
		n, from, err := fake.ReadFromUDPAddrPort(buf)
		if err != nil {
			return
		}
		inv, ok := parseInvariant(buf[:n], localCIDLen)
		if !ok {
			return
		}
		fake.WriteToUDPAddrPort(appendVersionNegotiation(nil, inv.scid, inv.dcid, []uint32{0xff00001d}), from)
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := Dial(ctx, "udp4", fake.LocalAddr().(*net.UDPAddr).AddrPort().String(), testClientConfig(t))
	if !errors.Is(err, ErrNoCompatibleVersion) {
		t.Fatalf("dial: %v", err)
	}
}

func TestDatagramQueueCap(t *testing.T) {
	cconf := testClientConfig(t)
	cconf.MaxPendingDatagrams = 4
	p := newPair(t, testServerConfig(t), cconf, nil)
	c := p.cliConn
	c.mu.Lock()
	c.cc.cwnd = 0
	c.mu.Unlock()
	var err error
	for i := 0; i < 10 && err == nil; i++ {
		err = c.SendDatagram([]byte("x"))
	}
	if err != ErrDatagramQueueFull {
		t.Fatalf("queue cap: %v", err)
	}
}
