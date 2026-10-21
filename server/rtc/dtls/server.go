package dtls

import (
	"crypto"
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/subtle"
	"crypto/x509"
	"encoding/binary"
	"errors"
	"sync"
	"time"
)

type Config struct {
	Certificate  *Certificate
	SRTPProfiles []uint16
	CookieSecret []byte
	VerifyPeer   func(der []byte) error
}

type serverState int

const (
	stateWaitHello serverState = iota
	stateWaitFlight
	stateDone
	stateFailed
	stateClosed
)

const (
	initialRetransmit = time.Second
	maxRetransmit     = 8 * time.Second
	maxRetransmits    = 4
	minPeerResendGap  = 250 * time.Millisecond
)

var (
	errClosed          = errors.New("dtls: connection closed by peer")
	errTimeout         = errors.New("dtls: handshake timed out")
	errNotDone         = errors.New("dtls: handshake not complete")
	errBadExportLength = errors.New("dtls: invalid keying material length")
	errReservedLabel   = errors.New("dtls: reserved exporter label")
	errSequenceExhaust = errors.New("dtls: record sequence exhausted")
	errInvalidConfig   = errors.New("dtls: invalid config")
)

type pendingMessage struct {
	typ    uint8
	epoch  uint16
	length int
	data   []byte
	have   []uint64
	filled int
}

func newPendingMessage(typ uint8, epoch uint16, length int) *pendingMessage {
	return &pendingMessage{typ: typ, epoch: epoch, length: length, data: make([]byte, length), have: make([]uint64, (length+63)/64)}
}

func (p *pendingMessage) add(offset int, data []byte) {
	for i, c := range data {
		j := offset + i
		bit := uint64(1) << (j % 64)
		if p.have[j/64]&bit == 0 {
			p.have[j/64] |= bit
			p.data[j] = c
			p.filled++
		}
	}
}

func (p *pendingMessage) complete() bool {
	return p.filled == p.length
}

type flightItem struct {
	ccs   bool
	epoch uint16
	typ   uint8
	seq   uint16
	body  []byte
}

type Server struct {
	mu     sync.Mutex
	cfg    *Config
	remote string
	send   func([]byte) error
	mtu    int

	state         serverState
	err           error
	handshakeDone bool

	readWindow [2]replayWindow
	writeSeq   [2]uint64
	writeEpoch uint16
	clientKeys *cipherState
	serverKeys *cipherState
	early      [][]byte
	peerAuthed bool

	hello    *pendingMessage
	helloSeq uint16

	pending        map[uint16]*pendingMessage
	recvSeq        uint16
	sendSeq        uint16
	clientHelloSeq uint16
	expect         uint8

	offeredReneg        bool
	offeredPointFormats bool

	clientRandom []byte
	serverRandom []byte
	profile      uint16
	ecdhKey      *ecdh.PrivateKey
	transcript   []byte
	peerCert     []byte
	peerKey      crypto.PublicKey
	master       []byte

	flight     []flightItem
	timerOn    bool
	deadline   time.Time
	interval   time.Duration
	resends    int
	resendNow  bool
	lastResend time.Time
}

func NewServer(cfg *Config, remote string, send func([]byte) error) *Server {
	s := &Server{cfg: cfg, remote: remote, send: send, mtu: defaultMTU, pending: make(map[uint16]*pendingMessage)}
	if !validConfig(cfg) || send == nil {
		s.state = stateFailed
		s.err = errInvalidConfig
	}
	return s
}

func validConfig(cfg *Config) bool {
	if cfg == nil || cfg.Certificate == nil || cfg.Certificate.Key == nil || len(cfg.Certificate.DER) == 0 {
		return false
	}
	if cfg.Certificate.Key.Curve != elliptic.P256() {
		return false
	}
	return len(cfg.SRTPProfiles) > 0 && len(cfg.CookieSecret) >= minCookieSecret
}

func (s *Server) Done() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.handshakeDone
}

func (s *Server) Failed() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.err
}

func (s *Server) SRTPProfile() uint16 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.handshakeDone {
		return 0
	}
	return s.profile
}

func (s *Server) PeerCertificate() []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.handshakeDone {
		return nil
	}
	return append([]byte(nil), s.peerCert...)
}

func (s *Server) ExportKeyingMaterial(label string, length int) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.handshakeDone || s.master == nil {
		return nil, errNotDone
	}
	switch label {
	case "client finished", "server finished", "master secret", "key expansion", "extended master secret":
		return nil, errReservedLabel
	}
	if length <= 0 || length > 0xffff {
		return nil, errBadExportLength
	}
	seed := append(append([]byte(nil), s.clientRandom...), s.serverRandom...)
	return prf(s.master, label, seed, length), nil
}

func (s *Server) HandleDatagram(b []byte, now time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.state == stateFailed || s.state == stateClosed {
		return s.err
	}
	s.resendNow = false
	if err := s.handleRecords(b, now); err != nil {
		return s.fail(err)
	}
	for len(s.early) > 0 && s.clientKeys != nil && s.state == stateWaitFlight {
		rec := s.early[0]
		s.early = s.early[1:]
		if err := s.handleRecords(rec, now); err != nil {
			return s.fail(err)
		}
	}
	if s.state == stateFailed || s.state == stateClosed {
		return s.err
	}
	if s.resendNow && (s.state == stateWaitFlight || s.state == stateDone) && (s.lastResend.IsZero() || now.Sub(s.lastResend) >= minPeerResendGap || now.Before(s.lastResend)) {
		s.lastResend = now
		if err := s.sendFlight(); err != nil {
			return s.sendFailure(err)
		}
	}
	return nil
}

func (s *Server) Tick(now time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.state == stateFailed || s.state == stateClosed {
		return s.err
	}
	if s.state != stateWaitFlight || !s.timerOn || now.Before(s.deadline) {
		return nil
	}
	if s.resends >= maxRetransmits {
		return s.fail(errTimeout)
	}
	s.resends++
	s.interval *= 2
	if s.interval > maxRetransmit {
		s.interval = maxRetransmit
	}
	s.deadline = now.Add(s.interval)
	if err := s.sendFlight(); err != nil {
		return s.sendFailure(err)
	}
	return nil
}

func (s *Server) sendFailure(err error) error {
	if errors.Is(err, errSequenceExhaust) {
		return s.fail(err)
	}
	return err
}

func (s *Server) fail(err error) error {
	if s.state == stateFailed || s.state == stateClosed {
		return s.err
	}
	var ae *alertError
	if errors.As(err, &ae) {
		s.sendAlert(alertLevelFatal, ae.desc)
	}
	s.state = stateFailed
	s.err = err
	s.release()
	return err
}

func (s *Server) release() {
	s.flight = nil
	s.timerOn = false
	s.pending = nil
	s.hello = nil
	s.early = nil
	s.transcript = nil
	s.ecdhKey = nil
}

func (s *Server) sendAlert(level, desc uint8) {
	epoch := s.writeEpoch
	if s.writeSeq[epoch] > maxSeq {
		return
	}
	seq := s.writeSeq[epoch]
	s.writeSeq[epoch]++
	payload := []byte{level, desc}
	var rec []byte
	if epoch == 1 && s.serverKeys != nil {
		rec = s.serverKeys.seal(nil, contentAlert, epoch, seq, payload)
	} else {
		rec = plainRecord(nil, contentAlert, 0, seq, payload)
	}
	_ = s.send(rec)
}

func (s *Server) handleRecords(b []byte, now time.Time) error {
	for len(b) > 0 {
		rec, rest, ok := nextRecord(b)
		if !ok {
			return nil
		}
		b = rest
		if err := s.handleRecord(rec, now); err != nil {
			return err
		}
		if s.state == stateFailed || s.state == stateClosed {
			return nil
		}
	}
	return nil
}

func (s *Server) handleRecord(rec record, now time.Time) error {
	if len(rec.payload) > maxCiphertext || rec.seq > maxSeq {
		return nil
	}
	var pt []byte
	switch rec.epoch {
	case 0:
		if rec.version != versionDTLS10 && rec.version != versionDTLS12 {
			return nil
		}
		if len(rec.payload) > maxPlaintext {
			return nil
		}
		if s.state != stateWaitHello {
			if !s.readWindow[0].fresh(rec.seq) {
				return nil
			}
			s.readWindow[0].mark(rec.seq)
		}
		pt = rec.payload
	case 1:
		if rec.version != versionDTLS12 {
			return nil
		}
		if s.clientKeys == nil {
			if s.state == stateWaitFlight && len(s.early) < maxEarlyRecords && len(rec.payload) <= maxEarlyRecordLen {
				raw := plainRecordRaw(rec)
				s.early = append(s.early, raw)
			}
			return nil
		}
		if !s.readWindow[1].fresh(rec.seq) {
			return nil
		}
		var ok bool
		pt, ok = s.clientKeys.open(rec)
		if !ok || len(pt) > maxPlaintext {
			return nil
		}
		s.readWindow[1].mark(rec.seq)
		s.peerAuthed = true
	default:
		return nil
	}
	if s.state == stateWaitHello {
		if rec.typ == contentHandshake && rec.epoch == 0 {
			s.handleHelloRecord(rec.seq, pt, now)
		}
		return nil
	}
	if s.state == stateDone && rec.epoch == 0 {
		if rec.typ == contentHandshake {
			s.noteRetransmission(pt)
		}
		return nil
	}
	switch rec.typ {
	case contentHandshake:
		return s.handleHandshakeRecord(rec.epoch, pt)
	case contentChangeCipherSpec:
		if rec.epoch != 0 {
			return fatal(alertUnexpectedMessage, "change_cipher_spec in epoch %d", rec.epoch)
		}
		if len(pt) != 1 || pt[0] != 1 {
			return fatal(alertDecodeError, "malformed change_cipher_spec")
		}
		return nil
	case contentAlert:
		return s.handleAlert(rec.epoch, pt)
	case contentApplicationData:
		return fatal(alertUnexpectedMessage, "application data is refused")
	default:
		return fatal(alertUnexpectedMessage, "unexpected record type %d", rec.typ)
	}
}

func plainRecordRaw(rec record) []byte {
	b := make([]byte, 0, recordHeaderLen+len(rec.payload))
	b = append(b, rec.typ)
	b = appendU16(b, rec.version)
	b = appendU16(b, rec.epoch)
	b = appendU48(b, rec.seq)
	b = appendU16(b, uint16(len(rec.payload)))
	return append(b, rec.payload...)
}

func (s *Server) handleAlert(epoch uint16, pt []byte) error {
	if epoch == 0 && s.peerAuthed {
		return nil
	}
	if len(pt) != 2 {
		return fatal(alertDecodeError, "malformed alert")
	}
	if pt[1] == alertCloseNotify {
		s.sendAlert(alertLevelWarning, alertCloseNotify)
		s.state = stateClosed
		s.err = errClosed
		s.release()
		return nil
	}
	if pt[0] == alertLevelFatal {
		s.state = stateFailed
		s.err = &peerAlertError{desc: pt[1]}
		s.release()
		return nil
	}
	return nil
}

func (s *Server) handleHelloRecord(recSeq uint64, pt []byte, now time.Time) {
	frags, ok := parseFragments(pt)
	if !ok {
		return
	}
	for _, f := range frags {
		if s.state != stateWaitHello {
			return
		}
		if f.typ != hsClientHello || f.length > maxMessageLen {
			continue
		}
		if s.hello == nil || s.helloSeq != f.seq || s.hello.length != f.length {
			s.hello = newPendingMessage(f.typ, 0, f.length)
			s.helloSeq = f.seq
		}
		s.hello.add(f.offset, f.data)
		if !s.hello.complete() {
			continue
		}
		body := s.hello.data
		s.hello = nil
		if err := s.handleClientHello(f.seq, recSeq, body, now); err != nil {
			s.fail(err)
			return
		}
	}
}

func (s *Server) handleClientHello(msgSeq uint16, recSeq uint64, body []byte, now time.Time) error {
	ch, ok := parseClientHello(body)
	if !ok {
		return nil
	}
	if !cookieValid(s.cfg, s.remote, ch) {
		s.sendHelloVerify(ch, msgSeq, recSeq)
		return nil
	}
	if s.writeSeq[0] < recSeq {
		s.writeSeq[0] = recSeq
	}
	s.readWindow[0].mark(recSeq)
	s.state = stateWaitFlight
	s.clientHelloSeq = msgSeq
	s.recvSeq = msgSeq + 1
	s.sendSeq = msgSeq
	s.expect = hsCertificate
	if err := s.negotiate(ch); err != nil {
		return err
	}
	s.transcript = append(s.transcript, marshalHandshake(hsClientHello, msgSeq, body)...)
	if err := s.buildServerFlight(); err != nil {
		return err
	}
	s.timerOn = true
	s.interval = initialRetransmit
	s.deadline = now.Add(s.interval)
	s.resends = 0
	if err := s.sendFlight(); err != nil {
		if errors.Is(err, errSequenceExhaust) {
			return err
		}
	}
	return nil
}

func (s *Server) sendHelloVerify(ch *clientHello, msgSeq uint16, recSeq uint64) {
	if len(s.cfg.CookieSecret) < minCookieSecret {
		return
	}
	cookie := computeCookie(s.cfg.CookieSecret, s.remote, ch)
	body := appendU16(nil, versionDTLS10)
	body = appendVec8(body, cookie)
	rec := plainRecord(nil, contentHandshake, 0, recSeq, marshalHandshake(hsHelloVerifyRequest, msgSeq, body))
	_ = s.send(rec)
}

func (s *Server) negotiate(ch *clientHello) error {
	if ch.version > versionDTLS12 {
		return fatal(alertProtocolVersion, "client does not offer DTLS 1.2")
	}
	if !ch.hasSuite(suiteECDHEECDSAAES128GCMSHA256) {
		return fatal(alertHandshakeFailure, "client does not offer TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256")
	}
	nullCompression := false
	for _, c := range ch.compression {
		if c == 0 {
			nullCompression = true
		}
	}
	if !nullCompression {
		return fatal(alertIllegalParameter, "client does not offer null compression")
	}
	ems, ok := ch.extension(extExtendedMasterSec)
	if !ok {
		return fatal(alertHandshakeFailure, "client does not offer extended_master_secret")
	}
	if len(ems) != 0 {
		return fatal(alertDecodeError, "malformed extended_master_secret")
	}
	if groups, ok := ch.extension(extSupportedGroups); ok {
		r := newReader(groups)
		list := r.vec16()
		if !r.empty() || len(list)%2 != 0 {
			return fatal(alertDecodeError, "malformed supported_groups")
		}
		found := false
		for i := 0; i+1 < len(list); i += 2 {
			if binary.BigEndian.Uint16(list[i:]) == curveSecp256r1 {
				found = true
			}
		}
		if !found {
			return fatal(alertHandshakeFailure, "client does not offer secp256r1")
		}
	}
	if formats, ok := ch.extension(extECPointFormats); ok {
		r := newReader(formats)
		list := r.vec8()
		if !r.empty() || len(list) == 0 {
			return fatal(alertDecodeError, "malformed ec_point_formats")
		}
		found := false
		for _, f := range list {
			if f == 0 {
				found = true
			}
		}
		if !found {
			return fatal(alertIllegalParameter, "client does not offer uncompressed points")
		}
	}
	algs, ok := ch.extension(extSignatureAlgorithms)
	if !ok {
		return fatal(alertHandshakeFailure, "client does not offer signature_algorithms")
	}
	r := newReader(algs)
	list := r.vec16()
	if !r.empty() || len(list)%2 != 0 {
		return fatal(alertDecodeError, "malformed signature_algorithms")
	}
	found := false
	for i := 0; i+1 < len(list); i += 2 {
		if binary.BigEndian.Uint16(list[i:]) == sigECDSASecp256r1SHA256 {
			found = true
		}
	}
	if !found {
		return fatal(alertHandshakeFailure, "client does not offer ecdsa_secp256r1_sha256")
	}
	if reneg, ok := ch.extension(extRenegotiationInfo); ok {
		if len(reneg) != 1 || reneg[0] != 0 {
			return fatal(alertHandshakeFailure, "non-empty renegotiation_info")
		}
	}
	srtp, ok := ch.extension(extUseSRTP)
	if !ok {
		return fatal(alertHandshakeFailure, "client does not offer use_srtp")
	}
	r = newReader(srtp)
	profiles := r.vec16()
	r.vec8()
	if !r.empty() || len(profiles) < 2 || len(profiles)%2 != 0 {
		return fatal(alertDecodeError, "malformed use_srtp")
	}
	s.profile = 0
	for _, want := range s.cfg.SRTPProfiles {
		for i := 0; i+1 < len(profiles); i += 2 {
			if binary.BigEndian.Uint16(profiles[i:]) == want {
				s.profile = want
				break
			}
		}
		if s.profile != 0 {
			break
		}
	}
	if s.profile == 0 {
		return fatal(alertHandshakeFailure, "no common SRTP protection profile")
	}
	_, s.offeredPointFormats = ch.extension(extECPointFormats)
	_, reneg := ch.extension(extRenegotiationInfo)
	s.offeredReneg = reneg || ch.hasSuite(suiteRenegotiationSCSV)
	s.clientRandom = append([]byte(nil), ch.random...)
	return nil
}

func (s *Server) buildServerFlight() error {
	s.serverRandom = make([]byte, 32)
	if _, err := rand.Read(s.serverRandom); err != nil {
		return fatal(alertInternalError, "random: %v", err)
	}
	key, err := ecdh.P256().GenerateKey(rand.Reader)
	if err != nil {
		return fatal(alertInternalError, "ecdh: %v", err)
	}
	s.ecdhKey = key

	var exts []byte
	exts = appendU16(exts, extExtendedMasterSec)
	exts = appendU16(exts, 0)
	srtp := appendU16(nil, 2)
	srtp = appendU16(srtp, s.profile)
	srtp = append(srtp, 0)
	exts = appendU16(exts, extUseSRTP)
	exts = appendVec16(exts, srtp)
	if s.offeredReneg {
		exts = appendU16(exts, extRenegotiationInfo)
		exts = appendVec16(exts, []byte{0})
	}
	if s.offeredPointFormats {
		exts = appendU16(exts, extECPointFormats)
		exts = appendVec16(exts, []byte{1, 0})
	}
	sh := appendU16(nil, versionDTLS12)
	sh = append(sh, s.serverRandom...)
	sh = append(sh, 0)
	sh = appendU16(sh, suiteECDHEECDSAAES128GCMSHA256)
	sh = append(sh, 0)
	sh = appendVec16(sh, exts)

	certs := appendVec24(nil, s.cfg.Certificate.DER)
	certMsg := appendVec24(nil, certs)

	params := []byte{3}
	params = appendU16(params, curveSecp256r1)
	params = appendVec8(params, key.PublicKey().Bytes())
	h := sha256.New()
	h.Write(s.clientRandom)
	h.Write(s.serverRandom)
	h.Write(params)
	sig, err := ecdsa.SignASN1(rand.Reader, s.cfg.Certificate.Key, h.Sum(nil))
	if err != nil {
		return fatal(alertInternalError, "sign: %v", err)
	}
	ske := append([]byte(nil), params...)
	ske = appendU16(ske, sigECDSASecp256r1SHA256)
	ske = appendVec16(ske, sig)

	cr := appendVec8(nil, []byte{certTypeECDSASign, certTypeRSASign})
	algs := appendU16(nil, sigECDSASecp256r1SHA256)
	algs = appendU16(algs, sigRSAPKCS1SHA256)
	algs = appendU16(algs, sigECDSASecp384r1SHA384)
	cr = appendVec16(cr, algs)
	cr = appendU16(cr, 0)

	s.flight = nil
	for _, m := range []struct {
		typ  uint8
		body []byte
	}{
		{hsServerHello, sh},
		{hsCertificate, certMsg},
		{hsServerKeyExchange, ske},
		{hsCertificateRequest, cr},
		{hsServerHelloDone, nil},
	} {
		s.queueHandshake(0, m.typ, m.body)
	}
	return nil
}

func (s *Server) queueHandshake(epoch uint16, typ uint8, body []byte) {
	seq := s.sendSeq
	s.sendSeq++
	s.flight = append(s.flight, flightItem{epoch: epoch, typ: typ, seq: seq, body: body})
	s.transcript = append(s.transcript, marshalHandshake(typ, seq, body)...)
}

func (s *Server) noteRetransmission(pt []byte) {
	frags, ok := parseFragments(pt)
	if !ok {
		return
	}
	for _, f := range frags {
		if f.seq < s.recvSeq && s.isPreviousFlight(f.seq) {
			s.resendNow = true
		}
	}
}

func (s *Server) handleHandshakeRecord(epoch uint16, pt []byte) error {
	frags, ok := parseFragments(pt)
	if !ok {
		return fatal(alertDecodeError, "malformed handshake record")
	}
	for _, f := range frags {
		if err := s.handleFragment(epoch, f); err != nil {
			return err
		}
	}
	return s.drain()
}

func (s *Server) handleFragment(epoch uint16, f fragment) error {
	if f.length > maxMessageLen {
		return fatal(alertHandshakeFailure, "handshake message too large")
	}
	if f.seq < s.recvSeq {
		if s.isPreviousFlight(f.seq) {
			s.resendNow = true
		}
		return nil
	}
	if s.state == stateDone {
		return fatal(alertUnexpectedMessage, "renegotiation is refused")
	}
	if int(f.seq) >= int(s.recvSeq)+maxPendingMessages {
		return nil
	}
	p := s.pending[f.seq]
	if p == nil {
		p = newPendingMessage(f.typ, epoch, f.length)
		s.pending[f.seq] = p
	} else if p.typ != f.typ || p.length != f.length || p.epoch != epoch {
		return fatal(alertIllegalParameter, "inconsistent handshake fragments")
	}
	p.add(f.offset, f.data)
	return nil
}

func (s *Server) isPreviousFlight(seq uint16) bool {
	switch s.state {
	case stateWaitFlight:
		return seq == s.clientHelloSeq
	case stateDone:
		return seq > s.clientHelloSeq
	}
	return false
}

func (s *Server) drain() error {
	for s.state == stateWaitFlight {
		p := s.pending[s.recvSeq]
		if p == nil || !p.complete() {
			return nil
		}
		delete(s.pending, s.recvSeq)
		seq := s.recvSeq
		s.recvSeq++
		if err := s.processMessage(p, seq); err != nil {
			return err
		}
	}
	return nil
}

func (s *Server) processMessage(p *pendingMessage, seq uint16) error {
	if p.typ != s.expect {
		return fatal(alertUnexpectedMessage, "unexpected handshake message %d", p.typ)
	}
	if p.typ == hsFinished {
		if p.epoch != 1 {
			return fatal(alertUnexpectedMessage, "unencrypted finished")
		}
	} else if p.epoch != 0 {
		return fatal(alertUnexpectedMessage, "unexpected encrypted handshake message")
	}
	full := marshalHandshake(p.typ, seq, p.data)
	switch p.typ {
	case hsCertificate:
		if err := s.processCertificate(p.data); err != nil {
			return err
		}
		s.transcript = append(s.transcript, full...)
		s.expect = hsClientKeyExchange
	case hsClientKeyExchange:
		s.transcript = append(s.transcript, full...)
		if err := s.processKeyExchange(p.data); err != nil {
			return err
		}
		s.expect = hsCertificateVerify
	case hsCertificateVerify:
		if err := s.processCertificateVerify(p.data); err != nil {
			return err
		}
		s.transcript = append(s.transcript, full...)
		s.expect = hsFinished
	case hsFinished:
		return s.processFinished(p.data, full)
	}
	return nil
}

func (s *Server) processCertificate(body []byte) error {
	r := newReader(body)
	list := r.vec24()
	if !r.empty() {
		return fatal(alertDecodeError, "malformed certificate")
	}
	if len(list) == 0 {
		return fatal(alertHandshakeFailure, "client sent no certificate")
	}
	lr := newReader(list)
	leaf := lr.vec24()
	for lr.ok && len(lr.b) > 0 {
		if len(lr.vec24()) == 0 {
			lr.ok = false
		}
	}
	if !lr.ok {
		return fatal(alertDecodeError, "malformed certificate list")
	}
	if len(leaf) == 0 {
		return fatal(alertHandshakeFailure, "client sent no certificate")
	}
	cert, err := x509.ParseCertificate(leaf)
	if err != nil {
		return fatal(alertBadCertificate, "client certificate: %v", err)
	}
	switch k := cert.PublicKey.(type) {
	case *ecdsa.PublicKey:
		if k.Curve != elliptic.P256() && k.Curve != elliptic.P384() {
			return fatal(alertUnsupportedCertificate, "unsupported client certificate curve")
		}
	case *rsa.PublicKey:
		if k.N.BitLen() < 2048 || k.N.BitLen() > 8192 {
			return fatal(alertUnsupportedCertificate, "unsupported client RSA key size")
		}
	default:
		return fatal(alertUnsupportedCertificate, "unsupported client certificate key")
	}
	if s.cfg.VerifyPeer == nil {
		return fatal(alertBadCertificate, "no peer verifier configured")
	}
	der := append([]byte(nil), leaf...)
	if err := s.cfg.VerifyPeer(append([]byte(nil), der...)); err != nil {
		return fatal(alertBadCertificate, "peer certificate rejected: %v", err)
	}
	s.peerCert = der
	s.peerKey = cert.PublicKey
	return nil
}

func (s *Server) processKeyExchange(body []byte) error {
	r := newReader(body)
	point := r.vec8()
	if !r.empty() {
		return fatal(alertDecodeError, "malformed client key exchange")
	}
	pub, err := ecdh.P256().NewPublicKey(point)
	if err != nil {
		return fatal(alertIllegalParameter, "invalid client key share")
	}
	shared, err := s.ecdhKey.ECDH(pub)
	if err != nil {
		return fatal(alertIllegalParameter, "invalid client key share")
	}
	s.ecdhKey = nil
	sessionHash := sha256.Sum256(s.transcript)
	s.master = prf(shared, "extended master secret", sessionHash[:], 48)
	clear(shared)
	seed := append(append([]byte(nil), s.serverRandom...), s.clientRandom...)
	kb := prf(s.master, "key expansion", seed, 40)
	defer clear(kb)
	client, err := newCipherState(kb[0:16], kb[32:36])
	if err != nil {
		return fatal(alertInternalError, "cipher: %v", err)
	}
	server, err := newCipherState(kb[16:32], kb[36:40])
	if err != nil {
		return fatal(alertInternalError, "cipher: %v", err)
	}
	s.clientKeys = client
	s.serverKeys = server
	return nil
}

func (s *Server) processCertificateVerify(body []byte) error {
	r := newReader(body)
	alg := r.u16()
	sig := r.vec16()
	if !r.empty() {
		return fatal(alertDecodeError, "malformed certificate verify")
	}
	var ok bool
	switch alg {
	case sigECDSASecp256r1SHA256, sigECDSASecp384r1SHA384:
		k, isECDSA := s.peerKey.(*ecdsa.PublicKey)
		if !isECDSA {
			return fatal(alertIllegalParameter, "signature algorithm does not match certificate")
		}
		var digest []byte
		if alg == sigECDSASecp256r1SHA256 {
			d := sha256.Sum256(s.transcript)
			digest = d[:]
		} else {
			d := sha512.Sum384(s.transcript)
			digest = d[:]
		}
		ok = ecdsa.VerifyASN1(k, digest, sig)
	case sigRSAPKCS1SHA256:
		k, isRSA := s.peerKey.(*rsa.PublicKey)
		if !isRSA {
			return fatal(alertIllegalParameter, "signature algorithm does not match certificate")
		}
		d := sha256.Sum256(s.transcript)
		ok = rsa.VerifyPKCS1v15(k, crypto.SHA256, d[:], sig) == nil
	default:
		return fatal(alertIllegalParameter, "unsupported signature algorithm %#04x", alg)
	}
	if !ok {
		return fatal(alertDecryptError, "certificate verify signature invalid")
	}
	return nil
}

func (s *Server) processFinished(body, full []byte) error {
	if len(body) != 12 {
		return fatal(alertDecodeError, "malformed finished")
	}
	h := sha256.Sum256(s.transcript)
	want := prf(s.master, "client finished", h[:], 12)
	if subtle.ConstantTimeCompare(want, body) != 1 {
		return fatal(alertDecryptError, "client finished verify_data mismatch")
	}
	s.transcript = append(s.transcript, full...)
	h = sha256.Sum256(s.transcript)
	verify := prf(s.master, "server finished", h[:], 12)
	s.flight = []flightItem{{ccs: true}}
	s.queueHandshake(1, hsFinished, verify)
	s.writeEpoch = 1
	s.state = stateDone
	s.handshakeDone = true
	s.timerOn = false
	s.transcript = nil
	s.pending = make(map[uint16]*pendingMessage)
	s.early = nil
	if err := s.sendFlight(); err != nil && errors.Is(err, errSequenceExhaust) {
		return err
	}
	return nil
}

type packer struct {
	mtu    int
	dgrams [][]byte
	cur    []byte
}

func (p *packer) flush() {
	if len(p.cur) > 0 {
		p.dgrams = append(p.dgrams, p.cur)
		p.cur = nil
	}
}

func (s *Server) nextWriteSeq(epoch uint16) (uint64, error) {
	seq := s.writeSeq[epoch]
	if seq > maxSeq {
		return 0, errSequenceExhaust
	}
	s.writeSeq[epoch]++
	return seq, nil
}

func (s *Server) sealRecord(dst []byte, typ uint8, epoch uint16, payload []byte) ([]byte, error) {
	seq, err := s.nextWriteSeq(epoch)
	if err != nil {
		return nil, err
	}
	if epoch == 1 {
		return s.serverKeys.seal(dst, typ, epoch, seq, payload), nil
	}
	return plainRecord(dst, typ, epoch, seq, payload), nil
}

func (s *Server) packRecord(p *packer, typ uint8, epoch uint16, payload []byte) error {
	size := recordHeaderLen + len(payload)
	if epoch == 1 {
		size += gcmOverhead
	}
	if len(p.cur)+size > p.mtu {
		p.flush()
	}
	rec, err := s.sealRecord(p.cur, typ, epoch, payload)
	if err != nil {
		return err
	}
	p.cur = rec
	return nil
}

func (s *Server) sendFlight() error {
	mtu := s.mtu
	if mtu < minMTU {
		mtu = minMTU
	}
	p := &packer{mtu: mtu}
	for _, it := range s.flight {
		if it.ccs {
			if err := s.packRecord(p, contentChangeCipherSpec, 0, []byte{1}); err != nil {
				return err
			}
			continue
		}
		overhead := recordHeaderLen + handshakeHeaderLen
		if it.epoch == 1 {
			overhead += gcmOverhead
		}
		offset := 0
		for {
			room := mtu - len(p.cur) - overhead
			remaining := len(it.body) - offset
			if room < remaining && room < minFragment && len(p.cur) > 0 {
				p.flush()
				continue
			}
			n := remaining
			if n > room {
				n = room
			}
			frag := make([]byte, 0, handshakeHeaderLen+n)
			frag = appendHandshakeHeader(frag, it.typ, len(it.body), it.seq, offset, n)
			frag = append(frag, it.body[offset:offset+n]...)
			if err := s.packRecord(p, contentHandshake, it.epoch, frag); err != nil {
				return err
			}
			offset += n
			if offset >= len(it.body) {
				break
			}
		}
	}
	p.flush()
	var first error
	for _, d := range p.dgrams {
		if err := s.send(d); err != nil && first == nil {
			first = err
		}
	}
	return first
}
