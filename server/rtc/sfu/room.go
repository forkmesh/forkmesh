package sfu

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"math"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
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
	keyframeInterval    = time.Second
	answerTimeout       = 10 * time.Second
	defaultVideoBitrate = 1_500_000
	sendQueue           = 64
	roomQueue           = 4096
	packetBudget        = 600
	byteBudget          = 500_000
	MaxPerAccount       = 4
	MaxServerPeople     = 400
	usedSSRCMemory      = 4096
	roomCeilingTotal    = 3_000_000
	minCeiling          = 200_000
	pauseBelow          = 80_000
	resumeAbove         = 120_000
	pauseAfter          = time.Second
	rembEvery           = time.Second
	rembRaiseGap        = 200 * time.Millisecond
	repairNackBudget    = 50
	repairPacketBudget  = 200
	repairByteCeiling   = 256 << 10
	repairByteFloor     = 4 << 10
	repairShare         = 0.2
	repairSeqsPerNack   = 64
	resendSpacing       = 30 * time.Millisecond
	resendTimes         = 4
	resentSlots         = 256
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
	epoch       time.Time
	mux         *ice.Mux
	certificate *dtls.Certificate
	dtlsConfig  *dtls.Config
	candidates  []string
	mu          sync.Mutex
	rooms       map[string]*Room
	sockets     atomic.Int64
	waiting     atomic.Int64
}

type Room struct {
	server       *Server
	id           string
	mu           sync.Mutex
	participants map[string]*Participant
	order        []string
	used         map[uint32]bool
	usedOrder    []uint32
	queue        chan inbound
	done         chan struct{}
	scratch      []byte
	rtxScratch   []byte
}

type inbound struct {
	participant *Participant
	kind        int
	packet      []byte
}

type Track struct {
	owner          *Participant
	kind           string
	ssrc           uint32
	id             string
	payloadType    uint8
	rtxSSRC        uint32
	rtxPayloadType uint8
	cache          *packetCache
	stats          *receiveStats
}

type slot struct {
	mid   string
	kind  string
	track *Track
	rtx   uint32
}

type resent struct {
	ext   int64
	at    time.Time
	count int
}

type downstream struct {
	rtxSSRC   uint32
	rtxSeq    uint16
	rtx       bool
	start     int64
	timestamp uint32
	resent    *[resentSlots]resent
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
	pendingAt   time.Time
	account     string
	rev         int
	dirty       bool
	closed      bool
	lastKey     map[uint32]time.Time
	budgetStart time.Time
	packets     int
	bytes       int
	write       func([]byte) error
	tccID       uint8
	tccDown     bool
	arrivals    *arrivals
	downlink    *downlink
	down        map[*Track]*downstream
	videoPaused bool
	belowSince  time.Time
	path        netip.AddrPort
	havePath    bool
	lastReport  time.Time
	remb        uint64
	rembAt      time.Time
	repairStart time.Time
	repairNacks int
	repairCount int
	repairBytes int
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
		epoch:       time.Now(),
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

func (s *Server) micros(now time.Time) int64 {
	return now.Sub(s.epoch).Microseconds()
}

func (r *Room) newSSRCLocked() uint32 {
	for {
		ssrc := randomSSRC()
		if !r.used[ssrc] {
			r.used[ssrc] = true
			r.usedOrder = append(r.usedOrder, ssrc)
			return ssrc
		}
	}
}

func randomSSRC() uint32 {
	var raw [4]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	return binary.BigEndian.Uint32(raw[:]) | 1
}

func (s *Server) room(id string) *Room {
	room := s.rooms[id]
	if room == nil {
		room = &Room{
			server: s, id: id, participants: map[string]*Participant{}, used: map[uint32]bool{},
			queue: make(chan inbound, roomQueue), done: make(chan struct{}),
		}
		s.rooms[id] = room
		go room.work()
	}
	return room
}

func (r *Room) work() {
	for {
		select {
		case <-r.done:
			return
		case in := <-r.queue:
			r.packet(in.participant, in.kind, in.packet)
		}
	}
}

func (r *Room) enqueue(p *Participant, kind int, packet []byte) {
	select {
	case r.queue <- inbound{participant: p, kind: kind, packet: packet}:
	default:
	}
}

func (s *Server) people() int {
	total := 0
	for _, room := range s.rooms {
		room.mu.Lock()
		total += len(room.participants)
		room.mu.Unlock()
	}
	return total
}

func (s *Server) join(id, peerID, account string) (*Room, *Participant, []string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.people() >= MaxServerPeople {
		return nil, nil, nil, ErrRoomFull
	}
	room := s.room(id)
	participant, others, err := room.join(peerID, account)
	if err != nil {
		room.mu.Lock()
		empty := len(room.participants) == 0
		room.mu.Unlock()
		if empty && s.rooms[id] == room {
			delete(s.rooms, id)
			close(room.done)
		}
		return nil, nil, nil, err
	}
	return room, participant, others, nil
}

func (s *Server) dropRoomIfEmpty(room *Room) {
	s.mu.Lock()
	defer s.mu.Unlock()
	room.mu.Lock()
	empty := len(room.participants) == 0
	room.mu.Unlock()
	if empty && s.rooms[room.id] == room {
		delete(s.rooms, room.id)
		close(room.done)
	}
}

func (s *Server) Rooms() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.rooms)
}

func (r *Room) join(peerID, account string) (*Participant, []string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, taken := r.participants[peerID]; taken {
		return nil, nil, ErrRoomFull
	}
	if len(r.participants) >= r.server.cfg.MaxParticipants {
		return nil, nil, ErrRoomFull
	}
	same := 0
	for _, other := range r.participants {
		if other.account == account {
			same++
		}
	}
	if same >= MaxPerAccount {
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
		account:    account,
		arrivals:   &arrivals{},
		downlink:   newDownlink(),
		down:       map[*Track]*downstream{},
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
		tccID:       p.tccID,
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
	for _, section := range upstream {
		if section.accepted && section.tccID != 0 {
			p.tccID = section.tccID
			break
		}
	}
	for i := range upstream {
		if upstream[i].tccID != p.tccID {
			upstream[i].tccID = 0
		}
	}
	p.upstream = upstream
	kinds := map[string]bool{}
	offered := map[uint32]bool{}
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
			if offered[ssrc] || r.used[ssrc] || ssrc == 0 {
				p.tracks = nil
				return ErrBadOffer
			}
			offered[ssrc] = true
		}
		for _, ssrc := range media.SSRCs() {
			if rtx[ssrc] {
				continue
			}
			if len(p.tracks) >= MaxTracks {
				break
			}
			kinds[section.kind] = true
			track := &Track{
				owner:       p,
				kind:        section.kind,
				ssrc:        ssrc,
				id:          section.kind + "-" + strconv.FormatUint(uint64(ssrc), 10),
				payloadType: section.payloadType,
				stats:       newReceiveStats(section.kind),
			}
			if section.kind == "video" {
				track.cache = &packetCache{}
				for _, group := range media.FIDGroups() {
					if group[0] == ssrc && section.rtx {
						track.rtxSSRC, track.rtxPayloadType = group[1], section.rtxPayloadType
					}
				}
			}
			p.tracks = append(p.tracks, track)
			break
		}
	}
	for ssrc := range offered {
		r.used[ssrc] = true
		r.usedOrder = append(r.usedOrder, ssrc)
	}
	for len(r.usedOrder) > usedSSRCMemory {
		delete(r.used, r.usedOrder[0])
		r.usedOrder = r.usedOrder[1:]
	}
	p.ice = r.server.mux.NewSession(ufrag, pwd, func(kind int, packet []byte) { r.enqueue(p, kind, packet) })
	config := *r.server.dtlsConfig
	expected := p.fingerprint
	config.VerifyPeer = func(der []byte) error {
		if dtls.FingerprintOf(der) != expected {
			return errors.New("peer certificate does not match its fingerprint")
		}
		return nil
	}
	p.write = p.ice.Send
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
	rtx := map[string]bool{}
	tcc := map[string]bool{}
	for _, media := range session.Media {
		if media.Port != 0 && media.Direction() != "inactive" {
			accepted[media.Mid()] = true
			if pt, ok := media.RTXFor(videoPayloadType); ok && pt == videoRTXPayloadType {
				rtx[media.Mid()] = true
			}
			if p.tccID != 0 && media.Extensions()[rtp.TransportCCURI] == int(p.tccID) {
				tcc[media.Mid()] = true
			}
		}
	}
	before := p.negotiated
	p.negotiated = map[*Track]bool{}
	var fresh []*Track
	anyTCC := false
	for _, slot := range p.offered {
		if slot.track != nil && accepted[slot.mid] {
			p.negotiated[slot.track] = true
			if !before[slot.track] {
				fresh = append(fresh, slot.track)
			}
			if d := p.down[slot.track]; d != nil {
				d.rtx = rtx[slot.mid]
			}
			anyTCC = anyTCC || tcc[slot.mid]
		}
	}
	if anyTCC && !p.tccDown {
		p.downlink.reset(r.server.micros(time.Now()))
	}
	p.tccDown = anyTCC
	p.pendingRev = 0
	for _, track := range fresh {
		if track.kind == "video" && p.ready {
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
		if other == nil || other == p || other.closed {
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
			delete(p.down, slot.track)
			slot.track = nil
			slot.rtx = 0
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
		d := p.down[track]
		if d == nil {
			d = &downstream{rtxSSRC: r.newSSRCLocked()}
			p.down[track] = d
		}
		target.rtx = 0
		if track.kind == "video" {
			target.rtx = d.rtxSSRC
		}
	}
	p.offered = make([]*slot, len(p.slots))
	for i, current := range p.slots {
		copied := *current
		p.offered[i] = &copied
	}
	p.rev++
	p.pendingRev = p.rev
	p.pendingAt = time.Now()
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
			r.rtpLocked(p, packet, now)
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
	for track, ok := range p.negotiated {
		if ok && track.kind == "video" {
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

func (r *Room) rtpLocked(p *Participant, packet []byte, now time.Time) {
	plain, err := p.inbound.DecryptRTP(nil, packet)
	if err != nil {
		return
	}
	header, err := rtp.ParseHeader(plain)
	if err != nil {
		return
	}
	if p.tccID != 0 {
		if seq, ok := rtp.TransportSequence(header, p.tccID); ok {
			p.arrivals.record(seq, r.server.micros(now))
		}
	}
	var track *Track
	recovered := false
	for _, owned := range p.tracks {
		if owned.ssrc == header.SSRC {
			track = owned
		} else if owned.rtxSSRC != 0 && owned.rtxSSRC == header.SSRC {
			track, recovered = owned, true
		}
	}
	if track == nil {
		return
	}
	if recovered {
		if header.PayloadType != track.rtxPayloadType {
			return
		}
		osn, payload, err := rtp.ParseRTX(rtp.Payload(plain, header))
		if err != nil {
			return
		}
		rebuilt := rtp.Packet{
			Marker: header.Marker, PayloadType: track.payloadType, SequenceNumber: osn,
			Timestamp: header.Timestamp, SSRC: track.ssrc, CSRC: header.CSRC, Payload: payload,
		}
		plain = rebuilt.Marshal(nil)
		if header, err = rtp.ParseHeader(plain); err != nil {
			return
		}
	} else if header.PayloadType != track.payloadType {
		return
	}
	stats := track.stats
	ext := stats.ext(header.SequenceNumber)
	if stats.started && ext <= stats.highest-cacheSlots {
		return
	}
	if track.cache != nil && track.cache.has(ext) {
		return
	}
	if stats.arrived(ext, header.Timestamp, now, r.server.epoch, recovered) {
		r.requestKeyframeLocked(track, now)
	}
	pt := uint8(audioPayloadType)
	if track.kind == "video" {
		pt = videoPayloadType
	}
	rtp.SetPayloadType(plain, pt)
	if track.cache != nil {
		track.cache.put(ext, now, plain)
	}
	out := rtp.Packet{
		Marker: header.Marker, PayloadType: pt, SequenceNumber: header.SequenceNumber,
		Timestamp: header.Timestamp, SSRC: track.ssrc, CSRC: header.CSRC,
		Payload: rtp.Payload(plain, header), Padding: header.PaddingLength,
	}
	for _, receiver := range r.participants {
		if receiver == p || !receiver.ready || !receiver.negotiated[track] {
			continue
		}
		d := receiver.down[track]
		if d == nil || (track.kind == "video" && receiver.videoPaused) {
			continue
		}
		if d.start == 0 {
			d.start = ext
		}
		d.timestamp = header.Timestamp
		r.sendRTPLocked(receiver, &out, 0, now)
	}
	if track.kind == "video" {
		r.sendNacksLocked(track, now)
	}
}

func (r *Room) sendRTPLocked(receiver *Participant, packet *rtp.Packet, probe int32, now time.Time) (int, bool) {
	packet.TransportID = 0
	var seq int64
	if receiver.tccDown {
		packet.TransportID = receiver.tccID
		packet.TransportSeq, seq = receiver.downlink.stamp()
	}
	r.scratch = packet.Marshal(r.scratch)
	sealed, err := receiver.outbound.EncryptRTP(nil, r.scratch)
	if err != nil {
		return 0, false
	}
	_ = receiver.write(sealed)
	if seq != 0 {
		receiver.downlink.sent(seq, r.server.micros(now), len(sealed), probe)
	}
	return len(sealed), true
}

func (r *Room) sendNacksLocked(track *Track, now time.Time) {
	if seqs := track.stats.nacks(now); len(seqs) > 0 {
		r.sendRTCPLocked(track.owner, rtp.NewNACK(track.owner.sendSSRC, track.ssrc, seqs))
	}
}

func (p *Participant) repairBudget(now time.Time) int {
	if now.Sub(p.repairStart) > time.Second {
		p.repairStart, p.repairNacks, p.repairCount, p.repairBytes = now, 0, 0, 0
	}
	if !p.downlink.known() {
		return repairByteCeiling
	}
	return max(repairByteFloor, min(repairByteCeiling, int(p.downlink.estimate*repairShare/8)))
}

func (r *Room) nackLocked(p *Participant, part []byte, now time.Time) {
	media, lost, err := rtp.ParseNACK(part)
	if err != nil {
		return
	}
	track := r.trackBySSRC(media)
	if track == nil || track.owner == p || track.cache == nil || !p.negotiated[track] || p.videoPaused {
		return
	}
	d := p.down[track]
	if d == nil || !d.rtx {
		return
	}
	byteBudget := p.repairBudget(now)
	p.repairNacks++
	if p.repairNacks > repairNackBudget {
		return
	}
	if len(lost) > repairSeqsPerNack {
		lost = lost[:repairSeqsPerNack]
	}
	for _, seq := range lost {
		if p.repairCount >= repairPacketBudget || p.repairBytes >= byteBudget {
			return
		}
		ext, plain := track.cache.get(seq, now)
		if plain == nil || d.start == 0 || ext < d.start {
			continue
		}
		if d.resent == nil {
			d.resent = new([resentSlots]resent)
		}
		mark := &d.resent[ext&(resentSlots-1)]
		if mark.ext == ext {
			if mark.count >= resendTimes || now.Sub(mark.at) < resendSpacing {
				continue
			}
		} else {
			*mark = resent{ext: ext}
		}
		header, err := rtp.ParseHeader(plain)
		if err != nil {
			continue
		}
		r.rtxScratch = rtp.RTXPayload(r.rtxScratch, header.SequenceNumber, rtp.Payload(plain, header))
		packet := rtp.Packet{
			Marker: header.Marker, PayloadType: videoRTXPayloadType, SequenceNumber: d.rtxSeq,
			Timestamp: header.Timestamp, SSRC: d.rtxSSRC, CSRC: header.CSRC, Payload: r.rtxScratch,
		}
		d.rtxSeq++
		size, ok := r.sendRTPLocked(p, &packet, 0, now)
		if !ok {
			continue
		}
		mark.count++
		mark.at = now
		p.repairCount++
		p.repairBytes += size
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
			switch rtp.Count(part) {
			case rtp.FmtNACK:
				r.nackLocked(p, part, now)
			case rtp.FmtTCC:
				if !p.tccDown {
					continue
				}
				if feedback, err := rtp.ParseTransportFeedback(part); err == nil {
					p.downlink.feedback(feedback, r.server.micros(now))
				}
			}
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
			if ntp, ok := rtp.SenderReportNTP(part); ok {
				track.stats.srNTP, track.stats.srAt = ntp, now
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
	_ = p.write(sealed)
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
		if p.pendingRev != 0 && now.Sub(p.pendingAt) > answerTimeout {
			expired = append(expired, p)
			continue
		}
		if p.ice != nil {
			p.ice.Refresh(now)
		}
		if p.dtls != nil && !p.ready {
			if err := p.dtls.Tick(now); err != nil || p.dtls.Failed() != nil {
				expired = append(expired, p)
				continue
			}
		}
	}
	ceiling := r.ceilingLocked()
	for _, p := range r.participants {
		if !p.closed && p.ready {
			r.feedbackLocked(p, now)
			r.adaptReceiverLocked(p, now, ceiling)
		}
	}
	for _, p := range r.participants {
		if !p.closed && p.ready {
			r.capSenderLocked(p, now, ceiling)
		}
	}
	return expired
}

func (r *Room) ceilingLocked() float64 {
	n := len(r.participants)
	ceiling := float64(r.server.cfg.VideoBitrate)
	if n > 1 {
		ceiling = math.Min(ceiling, math.Max(minCeiling, roomCeilingTotal/float64(n-1)))
	}
	return ceiling
}

func (p *Participant) senders() int {
	owners := map[*Participant]bool{}
	for track, ok := range p.negotiated {
		if ok && !track.owner.closed {
			owners[track.owner] = true
		}
	}
	return len(owners)
}

func (r *Room) feedbackLocked(p *Participant, now time.Time) {
	if p.ice != nil {
		if addr, ok := p.ice.Selected(); ok {
			if p.havePath && addr != p.path {
				p.downlink.reset(r.server.micros(now))
			}
			p.path, p.havePath = addr, true
		}
	}
	if p.tccID != 0 && len(p.tracks) > 0 {
		for _, packet := range p.arrivals.feedback(p.sendSSRC, p.tracks[0].ssrc) {
			r.sendRTCPLocked(p, packet)
		}
	}
	for _, track := range p.tracks {
		if track.kind == "video" {
			r.sendNacksLocked(track, now)
		}
	}
	if now.Sub(p.lastReport) >= reportInterval {
		p.lastReport = now
		var blocks []rtp.ReportBlock
		for _, track := range p.tracks {
			if block, ok := track.stats.report(track.ssrc, now); ok {
				blocks = append(blocks, block)
			}
		}
		if len(blocks) > 0 {
			r.sendRTCPLocked(p, rtp.NewReceiverReport(p.sendSSRC, blocks...))
		}
	}
}

func (r *Room) adaptReceiverLocked(p *Participant, now time.Time, ceiling float64) {
	senders := p.senders()
	if senders == 0 {
		return
	}
	limit := ceiling * float64(senders)
	link := p.downlink
	if link.known() && link.estimate >= 1.2*limit {
		link.estimate = 0
	}
	share := math.Inf(1)
	if link.known() {
		share = link.estimate / float64(senders)
	}
	switch {
	case !p.videoPaused && share < pauseBelow:
		if p.belowSince.IsZero() {
			p.belowSince = now
		} else if now.Sub(p.belowSince) >= pauseAfter {
			p.videoPaused = true
		}
	case !p.videoPaused:
		p.belowSince = time.Time{}
	case share >= resumeAbove:
		p.videoPaused = false
		p.belowSince = time.Time{}
		for track, ok := range p.negotiated {
			if ok && track.kind == "video" {
				if d := p.down[track]; d != nil {
					d.start = 0
				}
				r.requestKeyframeLocked(track, now)
			}
		}
	}
	if !p.tccDown {
		return
	}
	_, packets, ok := link.wantProbe(r.server.micros(now), limit)
	if !ok {
		return
	}
	var carrier *downstream
	for track, negotiated := range p.negotiated {
		if d := p.down[track]; negotiated && d != nil && d.rtx && track.kind == "video" {
			carrier = d
			break
		}
	}
	if carrier == nil {
		link.probe.done = true
		return
	}
	for i := 0; i < packets; i++ {
		packet := rtp.Packet{
			PayloadType: videoRTXPayloadType, SequenceNumber: carrier.rtxSeq,
			Timestamp: carrier.timestamp, SSRC: carrier.rtxSSRC, Padding: probePadding,
		}
		carrier.rtxSeq++
		r.sendRTPLocked(p, &packet, link.probe.id, now)
	}
}

func (r *Room) capSenderLocked(p *Participant, now time.Time, ceiling float64) {
	var ssrcs []uint32
	for _, track := range p.tracks {
		if track.kind == "video" {
			ssrcs = append(ssrcs, track.ssrc)
		}
	}
	if len(ssrcs) == 0 {
		return
	}
	limit := ceiling
	for _, receiver := range r.participants {
		if receiver == p || receiver.closed || !receiver.ready || receiver.videoPaused || !receiver.downlink.known() {
			continue
		}
		receives := false
		for _, track := range p.tracks {
			receives = receives || receiver.negotiated[track]
		}
		if !receives {
			continue
		}
		share := receiver.downlink.estimate / float64(max(1, receiver.senders()))
		limit = math.Min(limit, math.Max(share, pauseBelow))
	}
	value := uint64(limit)
	since := now.Sub(p.rembAt)
	if p.rembAt.IsZero() || since >= rembEvery || value*100 < p.remb*95 || (value*100 > p.remb*105 && since >= rembRaiseGap) {
		p.remb, p.rembAt = value, now
		r.sendRTCPLocked(p, rtp.NewREMB(p.sendSSRC, value, ssrcs))
	}
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

func (s *Server) Close() {
	s.mu.Lock()
	rooms := make([]*Room, 0, len(s.rooms))
	for _, room := range s.rooms {
		rooms = append(rooms, room)
	}
	s.mu.Unlock()
	for _, room := range rooms {
		room.mu.Lock()
		participants := make([]*Participant, 0, len(room.participants))
		for _, participant := range room.participants {
			participants = append(participants, participant)
		}
		room.mu.Unlock()
		for _, participant := range participants {
			room.leave(participant)
		}
	}
}

func (s *Server) Fingerprint() string {
	return s.certificate.Fingerprint
}
