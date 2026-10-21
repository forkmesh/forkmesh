package dtls

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
)

const minCookieSecret = 16

type extension struct {
	typ  uint16
	data []byte
}

type clientHello struct {
	version     uint16
	random      []byte
	sessionID   []byte
	cookie      []byte
	suites      []byte
	compression []byte
	extensions  []extension
}

func parseClientHello(body []byte) (*clientHello, bool) {
	r := newReader(body)
	ch := &clientHello{version: r.u16(), random: r.take(32)}
	ch.sessionID = r.vec8()
	ch.cookie = r.vec8()
	ch.suites = r.vec16()
	ch.compression = r.vec8()
	if !r.ok || len(ch.sessionID) > 32 || len(ch.suites) < 2 || len(ch.suites)%2 != 0 || len(ch.compression) < 1 {
		return nil, false
	}
	if r.empty() {
		return ch, true
	}
	er := newReader(r.vec16())
	if !r.empty() {
		return nil, false
	}
	seen := make(map[uint16]bool)
	for er.ok && len(er.b) > 0 {
		e := extension{typ: er.u16()}
		e.data = er.vec16()
		if !er.ok || seen[e.typ] {
			return nil, false
		}
		seen[e.typ] = true
		ch.extensions = append(ch.extensions, e)
	}
	if !er.ok {
		return nil, false
	}
	return ch, true
}

func (ch *clientHello) extension(typ uint16) ([]byte, bool) {
	for _, e := range ch.extensions {
		if e.typ == typ {
			return e.data, true
		}
	}
	return nil, false
}

func (ch *clientHello) hasSuite(suite uint16) bool {
	for i := 0; i+1 < len(ch.suites); i += 2 {
		if binary.BigEndian.Uint16(ch.suites[i:]) == suite {
			return true
		}
	}
	return false
}

func computeCookie(secret []byte, remote string, ch *clientHello) []byte {
	mac := hmac.New(sha256.New, secret)
	var hdr [4]byte
	binary.BigEndian.PutUint32(hdr[:], uint32(len(remote)))
	mac.Write(hdr[:])
	mac.Write([]byte(remote))
	mac.Write(ch.random)
	binary.BigEndian.PutUint16(hdr[:2], ch.version)
	mac.Write(hdr[:2])
	for _, field := range [][]byte{ch.sessionID, ch.suites, ch.compression} {
		binary.BigEndian.PutUint32(hdr[:], uint32(len(field)))
		mac.Write(hdr[:])
		mac.Write(field)
	}
	return mac.Sum(nil)
}

func cookieValid(cfg *Config, remote string, ch *clientHello) bool {
	if cfg == nil || len(cfg.CookieSecret) < minCookieSecret || len(ch.cookie) != sha256.Size {
		return false
	}
	return hmac.Equal(ch.cookie, computeCookie(cfg.CookieSecret, remote, ch))
}

func VerifyCookie(cfg *Config, remote string, hello []byte) bool {
	if len(hello) == 0 {
		return false
	}
	msg := hello
	if hello[0] == contentHandshake {
		rec, _, ok := nextRecord(hello)
		if !ok || rec.epoch != 0 {
			return false
		}
		msg = rec.payload
	}
	frags, ok := parseFragments(msg)
	if !ok {
		return false
	}
	f := frags[0]
	if f.typ != hsClientHello || f.offset != 0 || len(f.data) != f.length {
		return false
	}
	ch, ok := parseClientHello(f.data)
	if !ok {
		return false
	}
	return cookieValid(cfg, remote, ch)
}
