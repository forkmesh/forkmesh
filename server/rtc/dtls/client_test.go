package dtls

import (
	"crypto"
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/subtle"
	"crypto/x509"
	"encoding/binary"
	"errors"
	"fmt"
)

type clientOptions struct {
	key            crypto.Signer
	certDER        []byte
	noCertificate  bool
	sigAlg         uint16
	noEMS          bool
	noSRTP         bool
	srtpProfiles   []uint16
	mtu            int
	tamperFinished bool
	badSignature   bool
	badCookie      bool
	skipCCS        bool
	plainFinished  bool
}

type testClient struct {
	opts clientOptions

	random   []byte
	cookie   []byte
	writeSeq [2]uint64
	window   [2]replayWindow
	msgSeq   uint16
	recvSeq  uint16
	pending  map[uint16]*pendingMessage
	state    int

	clientHello  []byte
	transcript   []byte
	serverRandom []byte
	serverCert   []byte
	serverPub    *ecdsa.PublicKey
	serverPoint  []byte
	profile      uint16
	emsAck       bool
	certRequest  bool
	master       []byte
	clientKeys   *cipherState
	serverKeys   *cipherState
	final        []flightItem
	hvrs         int
	serverCCS    bool
	done         bool
	alerts       []uint8
	closeNotify  bool
	serverHellos int
}

func newTestClient(opts clientOptions) *testClient {
	if opts.mtu == 0 {
		opts.mtu = 1200
	}
	if opts.srtpProfiles == nil {
		opts.srtpProfiles = []uint16{0x0007, 0x0001}
	}
	c := &testClient{opts: opts, pending: make(map[uint16]*pendingMessage), random: make([]byte, 32)}
	rand.Read(c.random)
	return c
}

func (c *testClient) helloBody() []byte {
	b := appendU16(nil, versionDTLS12)
	b = append(b, c.random...)
	b = append(b, 0)
	cookie := c.cookie
	if c.opts.badCookie && cookie != nil {
		cookie = append([]byte(nil), cookie...)
		cookie[0] ^= 1
	}
	b = appendVec8(b, cookie)
	suites := appendU16(nil, 0xc02f)
	suites = appendU16(suites, suiteECDHEECDSAAES128GCMSHA256)
	suites = appendU16(suites, suiteRenegotiationSCSV)
	b = appendVec16(b, suites)
	b = appendVec8(b, []byte{0})
	var ext []byte
	if !c.opts.noEMS {
		ext = appendU16(ext, extExtendedMasterSec)
		ext = appendU16(ext, 0)
	}
	if !c.opts.noSRTP {
		var list []byte
		for _, p := range c.opts.srtpProfiles {
			list = appendU16(list, p)
		}
		srtp := appendVec16(nil, list)
		srtp = appendVec8(srtp, nil)
		ext = appendU16(ext, extUseSRTP)
		ext = appendVec16(ext, srtp)
	}
	groups := appendVec16(nil, appendU16(appendU16(nil, 29), curveSecp256r1))
	ext = appendU16(ext, extSupportedGroups)
	ext = appendVec16(ext, groups)
	ext = appendU16(ext, extECPointFormats)
	ext = appendVec16(ext, []byte{1, 0})
	algs := appendU16(appendU16(appendU16(nil, sigECDSASecp256r1SHA256), sigRSAPKCS1SHA256), sigECDSASecp384r1SHA384)
	ext = appendU16(ext, extSignatureAlgorithms)
	ext = appendVec16(ext, appendVec16(nil, algs))
	ext = appendU16(ext, 0x0023)
	ext = appendU16(ext, 0)
	return appendVec16(b, ext)
}

func (c *testClient) start() [][]byte {
	body := c.helloBody()
	item := flightItem{typ: hsClientHello, seq: c.msgSeq, body: body}
	c.msgSeq++
	return c.pack([]flightItem{item})
}

func (c *testClient) pack(items []flightItem) [][]byte {
	var out [][]byte
	var cur []byte
	flush := func() {
		if len(cur) > 0 {
			out = append(out, cur)
			cur = nil
		}
	}
	addRecord := func(typ uint8, epoch uint16, payload []byte) {
		size := recordHeaderLen + len(payload)
		if epoch == 1 {
			size += gcmOverhead
		}
		if len(cur)+size > c.opts.mtu {
			flush()
		}
		seq := c.writeSeq[epoch]
		c.writeSeq[epoch]++
		if epoch == 1 {
			cur = c.clientKeys.seal(cur, typ, epoch, seq, payload)
		} else {
			cur = plainRecord(cur, typ, epoch, seq, payload)
		}
	}
	for _, it := range items {
		if it.ccs {
			addRecord(contentChangeCipherSpec, 0, []byte{1})
			continue
		}
		overhead := recordHeaderLen + handshakeHeaderLen
		if it.epoch == 1 {
			overhead += gcmOverhead
		}
		off := 0
		for {
			room := c.opts.mtu - len(cur) - overhead
			if room < len(it.body)-off && room < 32 && len(cur) > 0 {
				flush()
				continue
			}
			n := len(it.body) - off
			if n > room {
				n = room
			}
			frag := appendHandshakeHeader(nil, it.typ, len(it.body), it.seq, off, n)
			frag = append(frag, it.body[off:off+n]...)
			addRecord(contentHandshake, it.epoch, frag)
			off += n
			if off >= len(it.body) {
				break
			}
		}
	}
	flush()
	return out
}

func (c *testClient) handle(d []byte) ([][]byte, error) {
	var out [][]byte
	resend := false
	for len(d) > 0 {
		rec, rest, ok := nextRecord(d)
		if !ok {
			return out, errors.New("client: bad record")
		}
		d = rest
		pt := rec.payload
		if rec.epoch == 1 {
			if c.serverKeys == nil {
				continue
			}
			if !c.window[1].fresh(rec.seq) {
				continue
			}
			var ok bool
			pt, ok = c.serverKeys.open(rec)
			if !ok {
				return out, errors.New("client: record decrypt failed")
			}
			c.window[1].mark(rec.seq)
		} else if rec.epoch != 0 {
			return out, errors.New("client: bad epoch")
		}
		switch rec.typ {
		case contentAlert:
			if len(pt) != 2 {
				return out, errors.New("client: bad alert")
			}
			c.alerts = append(c.alerts, pt[1])
			if pt[1] == alertCloseNotify {
				c.closeNotify = true
			}
		case contentChangeCipherSpec:
			c.serverCCS = true
		case contentHandshake:
			frags, ok := parseFragments(pt)
			if !ok {
				return out, errors.New("client: bad handshake record")
			}
			for _, f := range frags {
				if f.seq < c.recvSeq {
					if c.state == 2 {
						resend = true
					}
					continue
				}
				p := c.pending[f.seq]
				if p == nil {
					p = newPendingMessage(f.typ, rec.epoch, f.length)
					c.pending[f.seq] = p
				}
				p.add(f.offset, f.data)
			}
			for {
				p := c.pending[c.recvSeq]
				if p == nil || !p.complete() {
					break
				}
				delete(c.pending, c.recvSeq)
				seq := c.recvSeq
				c.recvSeq++
				more, err := c.process(p, seq)
				if err != nil {
					return out, err
				}
				out = append(out, more...)
			}
		default:
			return out, fmt.Errorf("client: unexpected record type %d", rec.typ)
		}
	}
	if resend && len(out) == 0 {
		out = append(out, c.pack(c.final)...)
	}
	return out, nil
}

func (c *testClient) process(p *pendingMessage, seq uint16) ([][]byte, error) {
	full := marshalHandshake(p.typ, seq, p.data)
	switch {
	case p.typ == hsHelloVerifyRequest && c.state == 0:
		r := newReader(p.data)
		r.u16()
		c.cookie = append([]byte(nil), r.vec8()...)
		if !r.empty() {
			return nil, errors.New("client: bad hello verify request")
		}
		c.hvrs++
		c.state = 1
		body := c.helloBody()
		c.clientHello = marshalHandshake(hsClientHello, c.msgSeq, body)
		item := flightItem{typ: hsClientHello, seq: c.msgSeq, body: body}
		c.msgSeq++
		return c.pack([]flightItem{item}), nil
	case p.typ == hsHelloVerifyRequest && c.state == 1 && c.opts.badCookie:
		c.hvrs++
		return nil, nil
	case p.typ == hsServerHello && c.state == 1:
		c.serverHellos++
		c.transcript = append(append([]byte(nil), c.clientHello...), full...)
		return nil, c.parseServerHello(p.data)
	case p.typ == hsCertificate && c.state == 1:
		c.transcript = append(c.transcript, full...)
		r := newReader(p.data)
		lr := newReader(r.vec24())
		c.serverCert = append([]byte(nil), lr.vec24()...)
		if !r.empty() || !lr.ok {
			return nil, errors.New("client: bad certificate")
		}
		cert, err := x509.ParseCertificate(c.serverCert)
		if err != nil {
			return nil, err
		}
		pub, ok := cert.PublicKey.(*ecdsa.PublicKey)
		if !ok {
			return nil, errors.New("client: server key not ecdsa")
		}
		c.serverPub = pub
		return nil, nil
	case p.typ == hsServerKeyExchange && c.state == 1:
		c.transcript = append(c.transcript, full...)
		r := newReader(p.data)
		if r.u8() != 3 || r.u16() != curveSecp256r1 {
			return nil, errors.New("client: bad curve")
		}
		c.serverPoint = append([]byte(nil), r.vec8()...)
		params := p.data[:len(p.data)-len(r.b)]
		alg := r.u16()
		sig := r.vec16()
		if !r.empty() || alg != sigECDSASecp256r1SHA256 {
			return nil, errors.New("client: bad server key exchange")
		}
		h := sha256.New()
		h.Write(c.random)
		h.Write(c.serverRandom)
		h.Write(params)
		if !ecdsa.VerifyASN1(c.serverPub, h.Sum(nil), sig) {
			return nil, errors.New("client: server key exchange signature invalid")
		}
		return nil, nil
	case p.typ == hsCertificateRequest && c.state == 1:
		c.transcript = append(c.transcript, full...)
		c.certRequest = true
		return nil, nil
	case p.typ == hsServerHelloDone && c.state == 1:
		c.transcript = append(c.transcript, full...)
		if !c.certRequest {
			return nil, errors.New("client: no certificate request")
		}
		if err := c.buildFinal(); err != nil {
			return nil, err
		}
		c.state = 2
		return c.pack(c.final), nil
	case p.typ == hsFinished && c.state == 2:
		if p.epoch != 1 {
			return nil, errors.New("client: plaintext server finished")
		}
		h := sha256.Sum256(c.transcript)
		want := prf(c.master, "server finished", h[:], 12)
		if subtle.ConstantTimeCompare(want, p.data) != 1 {
			return nil, errors.New("client: server finished mismatch")
		}
		c.state = 3
		c.done = true
		return nil, nil
	}
	return nil, fmt.Errorf("client: unexpected message %d in state %d", p.typ, c.state)
}

func (c *testClient) parseServerHello(body []byte) error {
	r := newReader(body)
	if r.u16() != versionDTLS12 {
		return errors.New("client: bad server version")
	}
	c.serverRandom = append([]byte(nil), r.take(32)...)
	r.vec8()
	if r.u16() != suiteECDHEECDSAAES128GCMSHA256 || r.u8() != 0 {
		return errors.New("client: bad suite")
	}
	er := newReader(r.vec16())
	if !r.empty() {
		return errors.New("client: bad server hello")
	}
	for er.ok && len(er.b) > 0 {
		typ := er.u16()
		data := er.vec16()
		switch typ {
		case extExtendedMasterSec:
			c.emsAck = len(data) == 0
		case extUseSRTP:
			sr := newReader(data)
			list := sr.vec16()
			mki := sr.vec8()
			if !sr.empty() || len(list) != 2 || len(mki) != 0 {
				return errors.New("client: bad use_srtp")
			}
			c.profile = binary.BigEndian.Uint16(list)
		case extRenegotiationInfo:
			if len(data) != 1 || data[0] != 0 {
				return errors.New("client: bad renegotiation_info")
			}
		case extECPointFormats:
		default:
			return fmt.Errorf("client: unexpected extension %#x", typ)
		}
	}
	if !er.ok {
		return errors.New("client: bad extensions")
	}
	if !c.emsAck {
		return errors.New("client: server did not ack extended_master_secret")
	}
	return nil
}

func (c *testClient) buildFinal() error {
	var items []flightItem
	add := func(epoch uint16, typ uint8, body []byte) {
		items = append(items, flightItem{epoch: epoch, typ: typ, seq: c.msgSeq, body: body})
		c.transcript = append(c.transcript, marshalHandshake(typ, c.msgSeq, body)...)
		c.msgSeq++
	}
	var certs []byte
	if !c.opts.noCertificate {
		certs = appendVec24(nil, c.opts.certDER)
	}
	add(0, hsCertificate, appendVec24(nil, certs))
	key, err := ecdh.P256().GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	pub, err := ecdh.P256().NewPublicKey(c.serverPoint)
	if err != nil {
		return err
	}
	shared, err := key.ECDH(pub)
	if err != nil {
		return err
	}
	add(0, hsClientKeyExchange, appendVec8(nil, key.PublicKey().Bytes()))
	sessionHash := sha256.Sum256(c.transcript)
	c.master = prf(shared, "extended master secret", sessionHash[:], 48)
	kb := prf(c.master, "key expansion", append(append([]byte(nil), c.serverRandom...), c.random...), 40)
	c.clientKeys, _ = newCipherState(kb[0:16], kb[32:36])
	c.serverKeys, _ = newCipherState(kb[16:32], kb[36:40])
	if !c.opts.noCertificate {
		alg := c.opts.sigAlg
		var digest []byte
		var opts crypto.SignerOpts
		switch alg {
		case sigECDSASecp384r1SHA384:
			d := sha512.Sum384(c.transcript)
			digest, opts = d[:], crypto.SHA384
		default:
			d := sha256.Sum256(c.transcript)
			digest, opts = d[:], crypto.SHA256
		}
		sig, err := c.opts.key.Sign(rand.Reader, digest, opts)
		if err != nil {
			return err
		}
		if c.opts.badSignature {
			digest[0] ^= 1
			sig, _ = c.opts.key.Sign(rand.Reader, digest, opts)
		}
		body := appendU16(nil, alg)
		body = appendVec16(body, sig)
		add(0, hsCertificateVerify, body)
	}
	if !c.opts.skipCCS {
		items = append(items, flightItem{ccs: true})
	}
	h := sha256.Sum256(c.transcript)
	verify := prf(c.master, "client finished", h[:], 12)
	if c.opts.tamperFinished {
		verify[3] ^= 0x40
	}
	epoch := uint16(1)
	if c.opts.plainFinished {
		epoch = 0
	}
	add(epoch, hsFinished, verify)
	c.final = items
	return nil
}

func (c *testClient) encrypted(typ uint8, payload []byte) []byte {
	seq := c.writeSeq[1]
	c.writeSeq[1]++
	return c.clientKeys.seal(nil, typ, 1, seq, payload)
}

func (c *testClient) export(label string, n int) []byte {
	return prf(c.master, label, append(append([]byte(nil), c.random...), c.serverRandom...), n)
}
