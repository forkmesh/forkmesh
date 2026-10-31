package sfu

import (
	"errors"
	"net/netip"
	"testing"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/signal"
)

func TestAWebRTCRoomRefusesTheOtherEngineUntilItEmpties(t *testing.T) {
	mux, err := ice.Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer mux.Close()
	engines := signal.NewRoomEngines(MaxServerPeople)
	server, err := NewServer(Config{
		Secret: []byte("0123456789abcdef0123456789abcdef"), Addresses: []netip.Addr{netip.MustParseAddr("127.0.0.1")},
		Engines: engines,
	}, mux)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := engines.Join("held", signal.EngineFMM); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := server.join("held", "p1", "a1"); !errors.Is(err, ErrRoomEngine) {
		t.Fatalf("WebRTC joined an FM Media room: %v", err)
	}
	if server.Rooms() != 0 {
		t.Fatal("a refused join left a room")
	}
	room, participant, _, err := server.join("open", "p1", "a1")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := engines.Join("open", signal.EngineFMM); !errors.Is(err, signal.ErrRoomEngine) {
		t.Fatalf("FM Media joined a WebRTC room: %v", err)
	}
	if _, _, _, err := server.join("open", "p1", "a1"); err == nil {
		t.Fatal("duplicate peer admitted")
	}
	if engines.People() != 2 {
		t.Fatalf("a refused duplicate kept its claim: %d people", engines.People())
	}
	room.leave(participant)
	room.leave(participant)
	if engines.Engine("open") != "" || engines.People() != 1 {
		t.Fatalf("left room still claimed: %q, %d people", engines.Engine("open"), engines.People())
	}
}
