package sfu

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/mirror-node/rtc/dtls"
	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/rtp"
	"forkmesh.com/mirror-node/rtc/sdp"
	"forkmesh.com/mirror-node/rtc/srtp"
)

const (
	MaxParticipants     = 16
	MaxTracks           = 2
	MaxPacketBytes      = 1500
	keyframeInterval    = 500 * time.Millisecond
	feedbackInterval    = time.Second
	defaultVideoBitrate = 1_500_000
	sendQueue           = 64
	packetBudget        = 2000
	byteBudget          = 2_500_000
)

var (
	ErrRoomFull    = errors.New("room full")
	ErrBadOffer    = errors.New("offer unusable")
	ErrNotJoined   = errors.New("not joined")
	ErrAlreadyLive = errors.New("already negotiated")
)

type Config struct {
	Secret          []byte
	Addresses       []netip.Addr
	AllowOrigin     func(string) bool
	VideoBitrate    uint64
	MaxParticipants int
}

type Server struct {
	cfg         Config
	mux         *ice.Mux
	certificate *dtls.Certificate
	dtlsConfig  *dtls.Config
	candidates  []string
	mu          sync.Mutex
	rooms       map[string]*Room
}

type Room struct {
	server       *Server
	id           string
	mu           sync.Mutex
	participants map[string]*Participant
	order        []string
}

type Track struct {
	owner       *Participant
	kind        string
	ssrc        uint32
	id          string
	payloadType uint8
}

type slot struct {
	mid   string
	kind  string
	track *Track
}

type outgoing struct {
	message map[string]any
}

type Participant struct {
	room        *Room
	id          string
	out         chan outgoing
	ice         *ice.Session
	dtls        *dtls.Server
	inbound     *srtp.Context
	outbound    *srtp.Context
	ready       bool
	sendSSRC    uint32
	fingerprint string
	upstream    []upstreamSection
	tracks      []*Track
	slots       []*slot
	nextMid     int
	negotiated  map[*Track]bool
	offered     []*slot
	version     int
	sessionID   string
	pendingRev  int
	rev         int
	dirty       bool
	closed      bool
	lastKey     map[uint32]time.Time
	budgetStart time.Time
	packets     int
	bytes       int
	lastFeed    time.Time
}

func NewServer(cfg Config, mux *ice.Mux) (*Server, error) {
	if len(cfg.Secret) < 16 {
		return nil, errors.New("calls secret too short")
	}
	if cfg.MaxParticipants <= 0 || cfg.MaxParticipants > MaxParticipants {
		cfg.MaxParticipants = MaxParticipants
	}
	if cfg.VideoBitrate == 0 {
		cfg.VideoBitrate = defaultVideoBitrate
	}
	certificate, err := dtls.GenerateCertificate()
	if err != nil {
		return nil, err
	}
	cookie := make([]byte, 32)
	if _, err := rand.Read(cookie); err != nil {
		return nil, err
	}
	server := &Server{
		cfg:         cfg,
		mux:         mux,
		certificate: certificate,
		candidates:  mux.Candidates(cfg.Addresses),
		rooms:       map[string]*Room{},
	}
	server.dtlsConfig = &dtls.Config{
		Certificate:  certificate,
		SRTPProfiles: []uint16{uint16(srtp.ProfileAEADAES128GCM), uint16(srtp.ProfileAES128CMHMACSHA1_80)},
		CookieSecret: cookie,
	}
	return server, nil
}

func randomID() string {
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	return strconv.FormatUint(binary.BigEndian.Uint64(raw[:])>>1, 10)
}

func randomSSRC() uint32 {
	var raw [4]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	return binary.BigEndian.Uint32(raw[:]) | 1
}

func (s *Server) room(id string) *Room {
	s.mu.Lock()
	defer s.mu.Unlock()
	room := s.rooms[id]
	if room == nil {
		room = &Room{server: s, id: id, participants: map[string]*Participant{}}
		s.rooms[id] = room
	}
	return room
}

func (s *Server) dropRoomIfEmpty(room *Room) {
	s.mu.Lock()
	defer s.mu.Unlock()
	room.mu.Lock()
	empty := len(room.participants) == 0
	room.mu.Unlock()
	if empty && s.rooms[room.id] == room {
		delete(s.rooms, room.id)
	}
}

func (s *Server) Rooms() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.rooms)
}

func (r *Room) join(peerID string) (*Participant, []string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, taken := r.participants[peerID]; taken {
		return nil, nil, ErrRoomFull
	}
	if len(r.participants) >= r.server.cfg.MaxParticipants {
		return nil, nil, ErrRoomFull
	}
	others := append([]string(nil), r.order...)
	participant := &Participant{
		room:       r,
		id:         peerID,
		out:        make(chan outgoing, sendQueue),
		sendSSRC:   randomSSRC(),
		negotiated: map[*Track]bool{},
		sessionID:  randomID(),
		lastKey:    map[uint32]time.Time{},
	}
	r.participants[peerID] = participant
	r.order = append(r.order, peerID)
	for _, other := range r.participants {
		if other != participant {
			other.send(map[string]any{"type": "peer-joined", "peer": peerID})
		}
	}
	return participant, others, nil
}

func (p *Participant) send(message map[string]any) {
	if p.closed {
		return
	}
	select {
	case p.out <- outgoing{message: message}:
	default:
		p.closeLocked()
	}
}

func (p *Participant) description() description {
	p.version++
	return description{
		sessionID:   p.sessionID,
		version:     p.version,
		ufrag:       p.ice.LocalUfrag,
		pwd:         p.ice.LocalPwd,
		fingerprint: p.room.server.certificate.Fingerprint,
		candidates:  p.room.server.candidates,
		cname:       p.id,
	}
}

func (r *Room) offer(p *Participant, text string) error {
	session, err := sdp.Parse(text)
	if err != nil {
		return ErrBadOffer
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if p.closed {
		return ErrNotJoined
	}
	if p.ice != nil {
		return ErrAlreadyLive
	}
	upstream := readUpstream(session)
	accepted := 0
	var ufrag, pwd, algorithm, value string
	for index, media := range session.Media {
		if !upstream[index].accepted {
			continue
		}
		accepted++
		u, w := sdp.ICECredentials(session, media)
		a, v := sdp.Fingerprint(session, media)
		if ufrag == "" {
			ufrag, pwd, algorithm, value = u, w, a, v
		}
	}
	if accepted == 0 || ufrag == "" || pwd == "" || !strings.EqualFold(algorithm, "sha-256") || value == "" {
		return ErrBadOffer
	}
	p.fingerprint = "sha-256 " + strings.ToUpper(strings.TrimSpace(value))
	p.upstream = upstream
	kinds := map[string]bool{}
	for index, media := range session.Media {
		section := upstream[index]
		if !section.accepted || section.direction != "recvonly" || kinds[section.kind] {
			continue
		}
		rtx := map[uint32]bool{}
		for _, group := range media.FIDGroups() {
			rtx[group[1]] = true
		}
		for _, ssrc := range media.SSRCs() {
			if rtx[ssrc] {
				continue
			}
			if len(p.tracks) >= MaxTracks {
				break
			}
			kinds[section.kind] = true
			p.tracks = append(p.tracks, &Track{
				owner:       p,
				kind:        section.kind,
				ssrc:        ssrc,
				id:          section.kind + "-" + strconv.FormatUint(uint64(ssrc), 10),
				payloadType: section.payloadType,
			})
			break
		}
	}
	for _, track := range p.tracks {
		for _, other := range r.participants {
			for _, owned := range other.tracks {
				if other != p && owned.ssrc == track.ssrc {
					p.tracks = nil
					return ErrBadOffer
				}
			}
		}
	}
	p.ice = r.server.mux.NewSession(ufrag, func(kind int, packet []byte) { r.packet(p, kind, packet) })
	config := *r.server.dtlsConfig
	expected := p.fingerprint
	config.VerifyPeer = func(der []byte) error {
		if dtls.FingerprintOf(der) != expected {
			return errors.New("peer certificate does not match its fingerprint")
		}
		return nil
	}
	p.dtls = dtls.NewServer(&config, p.id, p.ice.Send)
	p.send(map[string]any{"type": "answer", "sdp": p.description().render(p.upstream, nil)})
	for _, other := range r.participants {
		if other != p {
			r.renegotiateLocked(other)
		}
	}
	r.renegotiateLocked(p)
	return nil
}

func (r *Room) answered(p *Participant, rev int, text string) error {
	session, err := sdp.Parse(text)
	if err != nil {
		return ErrBadOffer
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if p.closed || p.pendingRev == 0 || rev != p.pendingRev {
		return nil
	}
	accepted := map[string]bool{}
	for _, media := range session.Media {
		if media.Port != 0 && media.Direction() != "inactive" {
			accepted[media.Mid()] = true
		}
	}
	p.negotiated = map[*Track]bool{}
	var fresh []*Track
	for _, slot := range p.offered {
		if slot.track != nil && accepted[slot.mid] {
			p.negotiated[slot.track] = true
			fresh = append(fresh, slot.track)
		}
	}
	p.pendingRev = 0
	for _, track := range fresh {
		if track.kind == "video" {
			r.requestKeyframeLocked(track, time.Now())
		}
	}
	if p.dirty {
		r.renegotiateLocked(p)
	}
	return nil
}

func (r *Room) renegotiateLocked(p *Participant) {
	if p.closed || p.ice == nil {
		return
	}
	if p.pendingRev != 0 {
		p.dirty = true
		return
	}
	p.dirty = false
	wanted := []*Track{}
	for _, id := range r.order {
		other := r.participants[id]
		if other == nil || other == p {
			continue
		}
		wanted = append(wanted, other.tracks...)
	}
	if len(wanted) == 0 && len(p.slots) == 0 {
		return
	}
	present := map[*Track]bool{}
	for _, slot := range p.slots {
		if slot.track != nil && slot.track.owner.closed {
			slot.track = nil
		}
		if slot.track != nil {
			present[slot.track] = true
		}
	}
	for _, track := range wanted {
		if present[track] {
			continue
		}
		var target *slot
		for _, candidate := range p.slots {
			if candidate.track == nil && candidate.kind == track.kind {
				target = candidate
				break
			}
		}
		if target == nil {
			target = &slot{mid: "s" + strconv.Itoa(p.nextMid), kind: track.kind}
			p.nextMid++
			p.slots = append(p.slots, target)
		}
		target.track = track
	}
	p.offered = make([]*slot, len(p.slots))
	for i, current := range p.slots {
		copied := *current
		p.offered[i] = &copied
	}
	p.rev++
	p.pendingRev = p.rev
	p.send(map[string]any{
		"type": "offer",
		"rev":  p.rev,
		"sdp":  p.description().render(p.upstream, p.offered),
	})
}

func (r *Room) leave(p *Participant) {
	r.mu.Lock()
	if p.closed && r.participants[p.id] != p {
		r.mu.Unlock()
		return
	}
	p.closeLocked()
	delete(r.participants, p.id)
	for i, id := range r.order {
		if id == p.id {
			r.order = append(r.order[:i], r.order[i+1:]...)
			break
		}
	}
	for _, other := range r.participants {
		other.send(map[string]any{"type": "peer-left", "peer": p.id})
		r.renegotiateLocked(other)
	}
	r.mu.Unlock()
	r.server.dropRoomIfEmpty(r)
}

func (p *Participant) closeLocked() {
	if p.closed {
		return
	}
	p.closed = true
	if p.ice != nil {
		p.ice.Close()
	}
	close(p.out)
}

func (r *Room) packet(p *Participant, kind int, packet []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if p.closed || len(packet) > MaxPacketBytes {
		return
	}
	now := time.Now()
	if now.Sub(p.budgetStart) > time.Second {
		p.budgetStart, p.packets, p.bytes = now, 0, 0
	}
	p.packets++
	p.bytes += len(packet)
	if p.packets > packetBudget || p.bytes > byteBudget {
		return
	}
	switch kind {
	case ice.KindDTLS:
		r.dtlsLocked(p, packet, now)
	case ice.KindRTP:
		if !p.ready {
			return
		}
		if rtp.IsRTCP(packet) {
			r.rtcpLocked(p, packet, now)
		} else {
			r.rtpLocked(p, packet)
		}
	}
}

func (r *Room) dtlsLocked(p *Participant, packet []byte, now time.Time) {
	if err := p.dtls.HandleDatagram(packet, now); err != nil || p.dtls.Failed() != nil {
		return
	}
	if p.ready || !p.dtls.Done() {
		return
	}
	profile := srtp.Profile(p.dtls.SRTPProfile())
	material, err := p.dtls.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", srtp.KeyingMaterialLength(profile))
	if err != nil {
		return
	}
	clientKey, clientSalt, serverKey, serverSalt, err := srtp.SplitKeyingMaterial(profile, material)
	if err != nil {
		return
	}
	inbound, err := srtp.NewContext(profile, clientKey, clientSalt)
	if err != nil {
		return
	}
	outbound, err := srtp.NewContext(profile, serverKey, serverSalt)
	if err != nil {
		return
	}
	p.inbound, p.outbound, p.ready = inbound, outbound, true
	for _, track := range p.tracks {
		if track.kind == "video" {
			r.requestKeyframeLocked(track, now)
		}
	}
}

func (r *Room) trackBySSRC(ssrc uint32) *Track {
	for _, participant := range r.participants {
		for _, track := range participant.tracks {
			if track.ssrc == ssrc {
				return track
			}
		}
	}
	return nil
}

func (r *Room) rtpLocked(p *Participant, packet []byte) {
	plain, err := p.inbound.DecryptRTP(nil, packet)
	if err != nil {
		return
	}
	header, err := rtp.ParseHeader(plain)
	if err != nil {
		return
	}
	var track *Track
	for _, owned := range p.tracks {
		if owned.ssrc == header.SSRC {
			track = owned
		}
	}
	if track == nil || header.PayloadType != track.payloadType {
		return
	}
	if track.kind == "video" {
		rtp.SetPayloadType(plain, videoPayloadType)
	} else {
		rtp.SetPayloadType(plain, audioPayloadType)
	}
	for _, receiver := range r.participants {
		if receiver == p || !receiver.ready || !receiver.negotiated[track] {
			continue
		}
		sealed, err := receiver.outbound.EncryptRTP(nil, plain)
		if err != nil {
			continue
		}
		_ = receiver.ice.Send(sealed)
	}
}

func (r *Room) rtcpLocked(p *Participant, packet []byte, now time.Time) {
	plain, err := p.inbound.DecryptRTCP(nil, packet)
	if err != nil {
		return
	}
	parts, err := rtp.SplitCompound(plain)
	if err != nil {
		return
	}
	for _, part := range parts {
		switch rtp.PacketType(part) {
		case rtp.TypePSFB:
			format := rtp.Count(part)
			if format != rtp.FmtPLI && format != rtp.FmtFIR {
				continue
			}
			track := r.trackBySSRC(rtp.MediaSSRC(part))
			if track != nil && track.owner != p && track.kind == "video" && p.negotiated[track] {
				r.requestKeyframeLocked(track, now)
			}
		case rtp.TypeRTPFB:
			if rtp.Count(part) != rtp.FmtNACK {
				continue
			}
			media, lost, err := rtp.ParseNACK(part)
			if err != nil || len(lost) == 0 {
				continue
			}
			track := r.trackBySSRC(media)
			if track == nil || track.owner == p || !p.negotiated[track] || !track.owner.ready {
				continue
			}
			r.sendRTCPLocked(track.owner, rtp.NewNACK(track.owner.sendSSRC, media, lost))
		case rtp.TypeSR:
			if len(part) < 28 || rtp.SenderSSRC(part) == 0 {
				continue
			}
			var track *Track
			for _, owned := range p.tracks {
				if owned.ssrc == rtp.SenderSSRC(part) {
					track = owned
				}
			}
			if track == nil {
				continue
			}
			report := append([]byte(nil), part[:28]...)
			report[0] = 0x80
			binary.BigEndian.PutUint16(report[2:4], 6)
			for _, receiver := range r.participants {
				if receiver != p && receiver.ready && receiver.negotiated[track] {
					r.sendRTCPLocked(receiver, report)
				}
			}
		}
	}
}

func (r *Room) requestKeyframeLocked(track *Track, now time.Time) {
	owner := track.owner
	if !owner.ready || now.Sub(owner.lastKey[track.ssrc]) < keyframeInterval {
		return
	}
	owner.lastKey[track.ssrc] = now
	r.sendRTCPLocked(owner, rtp.NewPLI(owner.sendSSRC, track.ssrc))
}

func (r *Room) sendRTCPLocked(p *Participant, packet []byte) {
	if !p.ready || p.closed {
		return
	}
	sealed, err := p.outbound.EncryptRTCP(nil, packet)
	if err != nil {
		return
	}
	_ = p.ice.Send(sealed)
}

func (r *Room) tick(now time.Time) []*Participant {
	r.mu.Lock()
	defer r.mu.Unlock()
	var expired []*Participant
	for _, p := range r.participants {
		if p.closed {
			continue
		}
		if p.ice != nil && p.ice.Expired(now) {
			expired = append(expired, p)
			continue
		}
		if p.dtls != nil && !p.ready {
			if err := p.dtls.Tick(now); err != nil || p.dtls.Failed() != nil {
				expired = append(expired, p)
				continue
			}
		}
		if p.ready && now.Sub(p.lastFeed) >= feedbackInterval {
			p.lastFeed = now
			report := rtp.NewReceiverReport(p.sendSSRC)
			for _, track := range p.tracks {
				if track.kind == "video" {
					report = append(report, rtp.NewREMB(p.sendSSRC, r.server.cfg.VideoBitrate, []uint32{track.ssrc})...)
				}
			}
			r.sendRTCPLocked(p, report)
		}
	}
	return expired
}

func (s *Server) Tick(now time.Time) {
	s.mu.Lock()
	rooms := make([]*Room, 0, len(s.rooms))
	for _, room := range s.rooms {
		rooms = append(rooms, room)
	}
	s.mu.Unlock()
	for _, room := range rooms {
		for _, participant := range room.tick(now) {
			room.leave(participant)
		}
	}
}

func (s *Server) Fingerprint() string {
	return s.certificate.Fingerprint
}
