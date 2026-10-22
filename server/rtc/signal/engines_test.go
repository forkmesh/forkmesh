package signal

import (
	"errors"
	"testing"
	"time"
)

func TestFirstEngineHoldsTheRoomUntilItEmpties(t *testing.T) {
	r := NewRoomEngines(3)
	if _, err := r.Join("a", EngineFMM); err != nil {
		t.Fatal(err)
	}
	if holder, err := r.Join("a", EngineWebRTC); !errors.Is(err, ErrRoomEngine) || holder != EngineFMM {
		t.Fatalf("mixed join: %q %v", holder, err)
	}
	if _, err := r.Join("a", EngineFMM); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Join("b", EngineWebRTC); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Join("c", EngineWebRTC); !errors.Is(err, ErrServerFull) {
		t.Fatalf("server cap across engines: %v", err)
	}
	r.Leave("a", EngineWebRTC)
	if r.People() != 3 {
		t.Fatalf("a leave for the wrong engine counted: %d", r.People())
	}
	r.Leave("a", EngineFMM)
	r.Leave("a", EngineFMM)
	if r.Engine("a") != "" || r.People() != 1 {
		t.Fatalf("room kept: %q %d", r.Engine("a"), r.People())
	}
	if _, err := r.Join("a", EngineWebRTC); err != nil {
		t.Fatalf("emptied room refused the other engine: %v", err)
	}
	var none *RoomEngines
	if _, err := none.Join("x", EngineFMM); err != nil || none.People() != 0 || none.Engine("x") != "" {
		t.Fatal("nil registry is not a no-op")
	}
	none.Leave("x", EngineFMM)
}

func TestFMMTicketsKeepTheWebRTCFormatAndAddTheirFields(t *testing.T) {
	secret := []byte("browser-test-secret-0123456789abcdef")
	base := Ticket{Room: RoomID(secret, "0123456789abcdef0123456789abcdef"), Peer: "abababababababababababababababab",
		Acct: "db440f5017a75b3c737907e23c3d78f1", Exp: 1_800_000_060}
	webrtc := MintTicket(secret, base)
	const wantWebRTC = "eyJyb29tIjoiMjA5MDU0YzFjNjhkMzI1YzE0OWQ2NjdhZGMwZGY1ZjYwYjBmMGIzNTE0YzY1MWVhOGJiODhkNzE1NjQwYjk1ZSIsInBlZXIiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYiIsImFjY3QiOiJkYjQ0MGY1MDE3YTc1YjNjNzM3OTA3ZTIzYzNkNzhmMSIsImV4cCI6MTgwMDAwMDA2MH0._SwoxYSH23v-fsxGRbL66itpXI8mLo_MLfma9EB3U9Y"
	if webrtc != wantWebRTC {
		t.Fatalf("WebRTC ticket changed: %s", webrtc)
	}
	standby := base
	standby.Engine, standby.Standby = EngineFMM, true
	minted := MintTicket(secret, standby)
	const wantStandby = "eyJyb29tIjoiMjA5MDU0YzFjNjhkMzI1YzE0OWQ2NjdhZGMwZGY1ZjYwYjBmMGIzNTE0YzY1MWVhOGJiODhkNzE1NjQwYjk1ZSIsInBlZXIiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYiIsImFjY3QiOiJkYjQ0MGY1MDE3YTc1YjNjNzM3OTA3ZTIzYzNkNzhmMSIsImV4cCI6MTgwMDAwMDA2MCwiZW5naW5lIjoiZm1tIiwic3RhbmRieSI6dHJ1ZX0.PEyg510KisogiG5OcGiNFb8xW1ghm-tRorgTogp6n6g"
	if minted != wantStandby {
		t.Fatalf("FM Media standby ticket: %s", minted)
	}
	got, err := VerifyTicket(secret, minted, time.Unix(1_800_000_000, 0))
	if err != nil || got != standby || TicketEngine(got) != EngineFMM {
		t.Fatalf("%+v %v", got, err)
	}
	plain, _ := VerifyTicket(secret, webrtc, time.Unix(1_800_000_000, 0))
	if TicketEngine(plain) != EngineWebRTC || plain.Standby {
		t.Fatalf("%+v", plain)
	}
}
