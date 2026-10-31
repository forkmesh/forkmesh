package signal

import (
	"errors"
	"sync"
)

const (
	EngineWebRTC = "webrtc"
	EngineFMM    = "fmm"
)

var (
	ErrRoomEngine = errors.New("room runs on another engine")
	ErrServerFull = errors.New("call server full")
)

type RoomEngines struct {
	MaxPeople int

	mu     sync.Mutex
	rooms  map[string]*roomClaim
	people int
}

type roomClaim struct {
	engine string
	people int
}

func NewRoomEngines(maxPeople int) *RoomEngines {
	return &RoomEngines{MaxPeople: maxPeople, rooms: map[string]*roomClaim{}}
}

func TicketEngine(t Ticket) string {
	if t.Engine == "" {
		return EngineWebRTC
	}
	return t.Engine
}

func (r *RoomEngines) Join(room, engine string) (string, error) {
	if r == nil {
		return engine, nil
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.rooms == nil {
		r.rooms = map[string]*roomClaim{}
	}
	claim := r.rooms[room]
	if claim != nil && claim.engine != engine {
		return claim.engine, ErrRoomEngine
	}
	if r.MaxPeople > 0 && r.people >= r.MaxPeople {
		return engine, ErrServerFull
	}
	if claim == nil {
		claim = &roomClaim{engine: engine}
		r.rooms[room] = claim
	}
	claim.people++
	r.people++
	return engine, nil
}

func (r *RoomEngines) Leave(room, engine string) {
	if r == nil {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	claim := r.rooms[room]
	if claim == nil || claim.engine != engine {
		return
	}
	claim.people--
	r.people--
	if claim.people <= 0 {
		delete(r.rooms, room)
	}
}

func (r *RoomEngines) Engine(room string) string {
	if r == nil {
		return ""
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if claim := r.rooms[room]; claim != nil {
		return claim.engine
	}
	return ""
}

func (r *RoomEngines) People() int {
	if r == nil {
		return 0
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.people
}
