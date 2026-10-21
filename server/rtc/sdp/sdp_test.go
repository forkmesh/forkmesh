package sdp

import (
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

const chromeFP = "D4:8E:BD:88:F5:A1:0C:1A:CE:47:66:42:62:1A:A9:4C:B7:8C:FE:D4:69:B5:A4:03:7D:F1:C8:18:51:1B:B9:86"

const firefoxFP = "5E:FD:C8:57:32:74:42:04:D8:10:19:7C:72:A0:1C:7C:ED:BA:71:26:3A:B7:2E:16:60:A1:EF:74:99:CD:89:C3"

var chromeOffer = crlf(`v=0
o=- 4611731400430051336 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1 2
a=extmap-allow-mixed
a=msid-semantic: WMS 5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9
m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:1467250027 1 udp 2122260223 192.168.0.196 46243 typ host generation 0 network-id 1 network-cost 10
a=candidate:842163049 1 udp 1686052607 203.0.113.7 46243 typ srflx raddr 192.168.0.196 rport 46243 generation 0 network-id 1 network-cost 10
a=candidate:2999745851 1 tcp 1518280447 192.168.0.196 9 typ host tcptype active generation 0 network-id 1 network-cost 10
a=ice-ufrag:9uFA
a=ice-pwd:Bh7yCUcN4cWZOfoDHtLJpcmD
a=ice-options:trickle
a=fingerprint:sha-256 ` + chromeFP + `
a=setup:actpass
a=mid:0
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=sendrecv
a=msid:5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9 0b6e4a41-9f39-4c8e-8a5c-61b2f8e1d0a3
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
a=rtpmap:63 red/48000/2
a=fmtp:63 111/111
a=rtpmap:9 G722/8000
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:13 CN/8000
a=rtpmap:110 telephone-event/48000
a=rtpmap:126 telephone-event/8000
a=ssrc:3570614608 cname:4TOk42mSjXCkVIa6
a=ssrc:3570614608 msid:5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9 0b6e4a41-9f39-4c8e-8a5c-61b2f8e1d0a3
m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 127 125
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:9uFA
a=ice-pwd:Bh7yCUcN4cWZOfoDHtLJpcmD
a=ice-options:trickle
a=fingerprint:sha-256 ` + chromeFP + `
a=setup:actpass
a=mid:1
a=extmap:14 urn:ietf:params:rtp-hdrext:toffset
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:13 urn:3gpp:video-orientation
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id
a=sendrecv
a=msid:5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9 7c1d2e3f-4a5b-4c6d-8e7f-9a0b1c2d3e4f
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:96 VP8/90000
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
a=rtpmap:102 H264/90000
a=rtcp-fb:102 goog-remb
a=rtcp-fb:102 transport-cc
a=rtcp-fb:102 ccm fir
a=rtcp-fb:102 nack
a=rtcp-fb:102 nack pli
a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f
a=rtpmap:103 rtx/90000
a=fmtp:103 apt=102
a=rtpmap:127 red/90000
a=rtpmap:125 ulpfec/90000
a=ssrc-group:FID 2231627014 632943048
a=ssrc:2231627014 cname:4TOk42mSjXCkVIa6
a=ssrc:2231627014 msid:5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9 7c1d2e3f-4a5b-4c6d-8e7f-9a0b1c2d3e4f
a=ssrc:632943048 cname:4TOk42mSjXCkVIa6
a=ssrc:632943048 msid:5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9 7c1d2e3f-4a5b-4c6d-8e7f-9a0b1c2d3e4f
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:9uFA
a=ice-pwd:Bh7yCUcN4cWZOfoDHtLJpcmD
a=ice-options:trickle
a=fingerprint:sha-256 ` + chromeFP + `
a=setup:actpass
a=mid:2
a=sctp-port:5000
a=max-message-size:262144
`)

var firefoxOffer = crlf(`v=0
o=mozilla...THIS_IS_SDPARTA-99.0 7193356018006263146 0 IN IP4 0.0.0.0
s=-
t=0 0
a=fingerprint:sha-256 ` + firefoxFP + `
a=group:BUNDLE 0 1
a=ice-options:trickle
a=msid-semantic:WMS *
m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101
c=IN IP4 0.0.0.0
a=candidate:0 1 UDP 2122252543 10.0.0.12 52143 typ host
a=candidate:1 1 TCP 2105524479 10.0.0.12 9 typ host tcptype active
a=sendrecv
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2/recvonly urn:ietf:params:rtp-hdrext:csrc-audio-level
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=fmtp:109 maxplaybackrate=48000;stereo=1;useinbandfec=1
a=fmtp:101 0-15
a=ice-pwd:4e8a3c3d1d2f6b7a9c0e1f2a3b4c5d6e
a=ice-ufrag:a1b2c3d4
a=mid:0
a=msid:{6b2f1e3a-1c4d-4f5e-9a8b-7c6d5e4f3a2b} {1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d}
a=rtcp-mux
a=rtpmap:109 opus/48000/2
a=rtpmap:9 G722/8000/1
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:101 telephone-event/8000
a=setup:actpass
a=ssrc:1828476542 cname:{e7f8a9b0-c1d2-4e3f-8a4b-5c6d7e8f9a0b}
m=video 9 UDP/TLS/RTP/SAVPF 120 124 121 125 126 127 97 98
c=IN IP4 0.0.0.0
a=recvonly
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:5 urn:ietf:params:rtp-hdrext:toffset
a=extmap:6/recvonly http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=fmtp:126 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1
a=fmtp:97 profile-level-id=42e01f;level-asymmetry-allowed=1
a=fmtp:120 max-fs=12288;max-fr=60
a=fmtp:124 apt=120
a=fmtp:121 max-fs=12288;max-fr=60
a=fmtp:125 apt=121
a=fmtp:127 apt=126
a=fmtp:98 apt=97
a=ice-pwd:9f8e7d6c5b4a39281706f5e4d3c2b1a0
a=ice-ufrag:e5f6a7b8
a=mid:1
a=rtcp-fb:120 nack
a=rtcp-fb:120 nack pli
a=rtcp-fb:120 ccm fir
a=rtcp-fb:120 goog-remb
a=rtcp-fb:120 transport-cc
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:120 VP8/90000
a=rtpmap:124 rtx/90000
a=rtpmap:121 VP9/90000
a=rtpmap:125 rtx/90000
a=rtpmap:126 H264/90000
a=rtpmap:127 rtx/90000
a=rtpmap:97 H264/90000
a=rtpmap:98 rtx/90000
a=setup:actpass
a=ssrc:2989112307 cname:{e7f8a9b0-c1d2-4e3f-8a4b-5c6d7e8f9a0b}
a=ssrc:3346118915 cname:{e7f8a9b0-c1d2-4e3f-8a4b-5c6d7e8f9a0b}
a=ssrc-group:FID 2989112307 3346118915
`)

func crlf(s string) string {
	return strings.ReplaceAll(s, "\n", "\r\n")
}

func mustParse(t *testing.T, text string) *Session {
	t.Helper()
	s, err := Parse(text)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	return s
}

func TestChromeOfferSession(t *testing.T) {
	s := mustParse(t, chromeOffer)
	if s.Version != 0 || s.Origin != "- 4611731400430051336 2 IN IP4 127.0.0.1" || s.Name != "-" || s.Timing != "0 0" {
		t.Fatalf("session lines: %+v", s)
	}
	if v, ok := s.Attr("group"); !ok || v != "BUNDLE 0 1 2" {
		t.Fatalf("group = %q %v", v, ok)
	}
	if v, ok := s.Attr("extmap-allow-mixed"); !ok || v != "" {
		t.Fatalf("extmap-allow-mixed = %q %v", v, ok)
	}
	if s.Attributes[1] != (Attribute{Key: "extmap-allow-mixed", Flag: true}) {
		t.Fatalf("flag attribute: %+v", s.Attributes[1])
	}
	if v, _ := s.Attr("msid-semantic"); v != " WMS 5f3a1c2e-7b9d-4e1f-a2c3-d4e5f6a7b8c9" {
		t.Fatalf("msid-semantic = %q", v)
	}
	if _, ok := s.Attr("ice-ufrag"); ok {
		t.Fatal("session ice-ufrag should be absent")
	}
	if len(s.Media) != 3 {
		t.Fatalf("media = %d", len(s.Media))
	}
}

func TestChromeOfferAudio(t *testing.T) {
	s := mustParse(t, chromeOffer)
	a := s.Media[0]
	if a.Kind != "audio" || a.Port != 9 || a.Proto != "UDP/TLS/RTP/SAVPF" {
		t.Fatalf("m-line: %+v", a)
	}
	if !reflect.DeepEqual(a.Formats, []string{"111", "63", "9", "0", "8", "13", "110", "126"}) {
		t.Fatalf("formats %v", a.Formats)
	}
	if a.Connection != "IN IP4 0.0.0.0" || len(a.Bandwidth) != 0 {
		t.Fatalf("c/b: %q %v", a.Connection, a.Bandwidth)
	}
	if a.Mid() != "0" || a.Direction() != "sendrecv" {
		t.Fatalf("mid %q dir %q", a.Mid(), a.Direction())
	}
	if !a.Has("rtcp-mux") || !a.Has("rtcp-rsize") || a.Has("rtcp-mux-only") {
		t.Fatal("rtcp flags")
	}
	if got := a.Attrs("candidate"); len(got) != 3 || !strings.HasPrefix(got[1], "842163049 1 udp 1686052607 203.0.113.7") {
		t.Fatalf("candidates %q", got)
	}
	if got := a.Attrs("extmap"); len(got) != 4 || got[3] != "4 urn:ietf:params:rtp-hdrext:sdes:mid" {
		t.Fatalf("extmaps %q", got)
	}
	want := map[uint8]string{
		111: "opus/48000/2",
		63:  "red/48000/2",
		9:   "G722/8000",
		0:   "PCMU/8000",
		8:   "PCMA/8000",
		13:  "CN/8000",
		110: "telephone-event/48000",
		126: "telephone-event/8000",
	}
	if got := a.RTPMaps(); !reflect.DeepEqual(got, want) {
		t.Fatalf("rtpmaps %v", got)
	}
	if got := a.Fmtps(); !reflect.DeepEqual(got, map[uint8]string{111: "minptime=10;useinbandfec=1", 63: "111/111"}) {
		t.Fatalf("fmtps %v", got)
	}
	if got := a.SSRCs(); !reflect.DeepEqual(got, []uint32{3570614608}) {
		t.Fatalf("ssrcs %v", got)
	}
	if got := a.FIDGroups(); len(got) != 0 {
		t.Fatalf("fid %v", got)
	}
	if v, _ := a.Attr("setup"); v != "actpass" {
		t.Fatalf("setup %q", v)
	}
	ufrag, pwd := ICECredentials(s, a)
	if ufrag != "9uFA" || pwd != "Bh7yCUcN4cWZOfoDHtLJpcmD" {
		t.Fatalf("ice %q %q", ufrag, pwd)
	}
	alg, fp := Fingerprint(s, a)
	if alg != "sha-256" || fp != chromeFP {
		t.Fatalf("fingerprint %q %q", alg, fp)
	}
}

func TestChromeOfferVideo(t *testing.T) {
	s := mustParse(t, chromeOffer)
	v := s.Media[1]
	if v.Kind != "video" || v.Mid() != "1" || v.Direction() != "sendrecv" {
		t.Fatalf("video %+v", v)
	}
	maps := v.RTPMaps()
	if maps[96] != "VP8/90000" || maps[97] != "rtx/90000" || maps[102] != "H264/90000" || maps[103] != "rtx/90000" || len(maps) != 6 {
		t.Fatalf("rtpmaps %v", maps)
	}
	fmtps := v.Fmtps()
	if fmtps[97] != "apt=96" || fmtps[103] != "apt=102" || !strings.Contains(fmtps[102], "profile-level-id=42001f") {
		t.Fatalf("fmtps %v", fmtps)
	}
	if got := v.SSRCs(); !reflect.DeepEqual(got, []uint32{2231627014, 632943048}) {
		t.Fatalf("ssrcs %v", got)
	}
	if got := v.FIDGroups(); !reflect.DeepEqual(got, [][2]uint32{{2231627014, 632943048}}) {
		t.Fatalf("fid %v", got)
	}
	if got := v.Attrs("rtcp-fb"); len(got) != 10 || got[4] != "96 nack pli" {
		t.Fatalf("rtcp-fb %q", got)
	}
	d := s.Media[2]
	if d.Kind != "application" || d.Proto != "UDP/DTLS/SCTP" || !reflect.DeepEqual(d.Formats, []string{"webrtc-datachannel"}) {
		t.Fatalf("data %+v", d)
	}
	if len(d.RTPMaps()) != 0 || len(d.SSRCs()) != 0 {
		t.Fatal("data channel has no rtp")
	}
	if p, _ := d.Attr("sctp-port"); p != "5000" {
		t.Fatalf("sctp-port %q", p)
	}
}

func TestFirefoxOffer(t *testing.T) {
	s := mustParse(t, firefoxOffer)
	if !strings.HasPrefix(s.Origin, "mozilla...THIS_IS_SDPARTA-99.0 ") {
		t.Fatalf("origin %q", s.Origin)
	}
	if v, _ := s.Attr("msid-semantic"); v != "WMS *" {
		t.Fatalf("msid-semantic %q", v)
	}
	if len(s.Media) != 2 {
		t.Fatalf("media %d", len(s.Media))
	}
	a, v := s.Media[0], s.Media[1]
	if a.Direction() != "sendrecv" || v.Direction() != "recvonly" {
		t.Fatalf("directions %q %q", a.Direction(), v.Direction())
	}
	alg, fp := Fingerprint(s, a)
	if alg != "sha-256" || fp != firefoxFP {
		t.Fatalf("session fingerprint fallback %q %q", alg, fp)
	}
	if u, p := ICECredentials(s, a); u != "a1b2c3d4" || p != "4e8a3c3d1d2f6b7a9c0e1f2a3b4c5d6e" {
		t.Fatalf("audio ice %q %q", u, p)
	}
	if u, p := ICECredentials(s, v); u != "e5f6a7b8" || p != "9f8e7d6c5b4a39281706f5e4d3c2b1a0" {
		t.Fatalf("video ice %q %q", u, p)
	}
	if got := a.RTPMaps(); got[109] != "opus/48000/2" || got[101] != "telephone-event/8000" || got[9] != "G722/8000/1" {
		t.Fatalf("audio rtpmaps %v", got)
	}
	if got := a.Fmtps(); got[101] != "0-15" {
		t.Fatalf("audio fmtps %v", got)
	}
	if got := a.SSRCs(); !reflect.DeepEqual(got, []uint32{1828476542}) {
		t.Fatalf("audio ssrcs %v", got)
	}
	if got := v.SSRCs(); !reflect.DeepEqual(got, []uint32{2989112307, 3346118915}) {
		t.Fatalf("video ssrcs %v", got)
	}
	if got := v.FIDGroups(); !reflect.DeepEqual(got, [][2]uint32{{2989112307, 3346118915}}) {
		t.Fatalf("video fid %v", got)
	}
	if got := v.RTPMaps(); len(got) != 8 || got[121] != "VP9/90000" || got[98] != "rtx/90000" {
		t.Fatalf("video rtpmaps %v", got)
	}
	if got := v.Fmtps(); got[124] != "apt=120" || got[98] != "apt=97" {
		t.Fatalf("video fmtps %v", got)
	}
	if got := a.Attrs("extmap"); got[1] != "2/recvonly urn:ietf:params:rtp-hdrext:csrc-audio-level" {
		t.Fatalf("extmap %q", got)
	}
}

func TestRoundTrip(t *testing.T) {
	for name, text := range map[string]string{"chrome": chromeOffer, "firefox": firefoxOffer} {
		s := mustParse(t, text)
		if got := s.String(); got != text {
			t.Errorf("%s: round trip differs\n%s", name, diffLine(text, got))
		}
	}
}

func TestLFInput(t *testing.T) {
	lf := strings.ReplaceAll(chromeOffer, "\r\n", "\n")
	s := mustParse(t, lf)
	if got := s.String(); got != chromeOffer {
		t.Fatalf("LF input not normalised\n%s", diffLine(chromeOffer, got))
	}
	mixed := strings.Replace(firefoxOffer, "\r\n", "\n", 7)
	mixed = strings.TrimSuffix(mixed, "\r\n")
	if got := mustParse(t, mixed).String(); got != firefoxOffer {
		t.Fatalf("mixed input not normalised\n%s", diffLine(firefoxOffer, got))
	}
}

func TestNormalisedOrderAndUnknownLines(t *testing.T) {
	in := "v=0\no=- 1 1 IN IP4 0.0.0.0\na=tool:x\ns=call\ni=about\nc=IN IP4 0.0.0.0\nb=AS:300\nt=0 0\nr=7d 1h 0 25h\nt=1 2\nz=0 -1h\nk=prompt\n" +
		"x=unknown session\n" +
		"m=audio 5004/2 RTP/AVP 0\na=rtpmap:0 PCMU/8000\nb=AS:64\nb=TIAS:64000\nc=IN IP4 192.0.2.1\nc=IN IP4 192.0.2.2\ni=media info\nk=clear:x\ny=unknown media\na=custom:with:colons\na=empty:\na=sendonly\n" +
		"m=video 0 RTP/AVP\n"
	want := "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=call\r\ni=about\r\nc=IN IP4 0.0.0.0\r\nb=AS:300\r\nt=0 0\r\nr=7d 1h 0 25h\r\nt=1 2\r\nz=0 -1h\r\nk=prompt\r\nx=unknown session\r\na=tool:x\r\n" +
		"m=audio 5004/2 RTP/AVP 0\r\ni=media info\r\nc=IN IP4 192.0.2.1\r\nc=IN IP4 192.0.2.2\r\nb=AS:64\r\nb=TIAS:64000\r\nk=clear:x\r\ny=unknown media\r\na=rtpmap:0 PCMU/8000\r\na=custom:with:colons\r\na=empty:\r\na=sendonly\r\n" +
		"m=video 0 RTP/AVP\r\n"
	s := mustParse(t, in)
	if got := s.String(); got != want {
		t.Fatalf("normalised output\n%s", diffLine(want, got))
	}
	m := s.Media[0]
	if m.Port != 5004 || m.Connection != "IN IP4 192.0.2.1" || !reflect.DeepEqual(m.Bandwidth, []string{"AS:64", "TIAS:64000"}) {
		t.Fatalf("media %+v", m)
	}
	if v, _ := m.Attr("custom"); v != "with:colons" {
		t.Fatalf("custom %q", v)
	}
	if v, ok := m.Attr("empty"); !ok || v != "" || m.Attributes[2].Flag {
		t.Fatalf("empty %q %v", v, ok)
	}
	if m.Direction() != "sendonly" || s.Media[1].Direction() != "sendrecv" || len(s.Media[1].Formats) != 0 {
		t.Fatal("directions or formats")
	}
	if got := mustParse(t, want).String(); got != want {
		t.Fatal("second round trip differs")
	}
}

func TestBuiltSession(t *testing.T) {
	s := &Session{
		Origin: "- 1 1 IN IP4 0.0.0.0",
		Name:   "-",
		Timing: "0 0",
		Attributes: []Attribute{
			{Key: "group", Value: "BUNDLE 0"},
			{Key: "ice-lite", Flag: true},
		},
		Media: []*Media{{
			Kind:       "video",
			Port:       9,
			Proto:      "UDP/TLS/RTP/SAVPF",
			Formats:    []string{"96"},
			Connection: "IN IP4 0.0.0.0",
			Attributes: []Attribute{{Key: "mid", Value: "0"}, {Key: "inactive", Flag: true}},
		}, nil},
	}
	want := "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\na=ice-lite\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=inactive\r\n"
	if got := s.String(); got != want {
		t.Fatalf("built\n%s", diffLine(want, got))
	}
	if s.Media[0].Direction() != "inactive" {
		t.Fatal("inactive")
	}
}

func TestCredentialPrecedence(t *testing.T) {
	text := "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=ice-ufrag:sess\r\na=ice-pwd:sesspwd\r\na=fingerprint:sha-256 AA:BB\r\n" +
		"m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=ice-ufrag:med\r\na=fingerprint:sha-1 CC:DD\r\n" +
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=ice-pwd:medpwd\r\n" +
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=fingerprint:broken\r\n"
	s := mustParse(t, text)
	if u, p := ICECredentials(s, s.Media[0]); u != "med" || p != "sesspwd" {
		t.Fatalf("media 0: %q %q", u, p)
	}
	if u, p := ICECredentials(s, s.Media[1]); u != "sess" || p != "medpwd" {
		t.Fatalf("media 1: %q %q", u, p)
	}
	if a, v := Fingerprint(s, s.Media[0]); a != "sha-1" || v != "CC:DD" {
		t.Fatalf("fp 0: %q %q", a, v)
	}
	if a, v := Fingerprint(s, s.Media[1]); a != "sha-256" || v != "AA:BB" {
		t.Fatalf("fp 1: %q %q", a, v)
	}
	if a, v := Fingerprint(s, s.Media[2]); a != "" || v != "" {
		t.Fatalf("fp 2: %q %q", a, v)
	}
	if u, p := ICECredentials(s, nil); u != "sess" || p != "sesspwd" {
		t.Fatalf("nil media: %q %q", u, p)
	}
	if u, p := ICECredentials(nil, s.Media[0]); u != "med" || p != "" {
		t.Fatalf("nil session: %q %q", u, p)
	}
	if a, v := Fingerprint(nil, nil); a != "" || v != "" {
		t.Fatal("nil fingerprint")
	}
}

func TestHelpersSkipBadValues(t *testing.T) {
	m := &Media{Attributes: []Attribute{
		{Key: "ssrc", Value: "notanumber cname:x"},
		{Key: "ssrc", Value: "4294967296 cname:x"},
		{Key: "ssrc", Value: "-1 cname:x"},
		{Key: "ssrc", Value: "7"},
		{Key: "ssrc", Value: "4294967295 cname:x"},
		{Key: "ssrc", Value: "7 msid:a b"},
		{Key: "ssrc", Flag: true},
		{Key: "ssrc-group", Value: "FID 1"},
		{Key: "ssrc-group", Value: "SIM 1 2 3"},
		{Key: "ssrc-group", Value: "FID x 2"},
		{Key: "ssrc-group", Value: "FID 5 6"},
		{Key: "rtpmap", Value: "128 bad/90000"},
		{Key: "rtpmap", Value: "-1 bad/90000"},
		{Key: "rtpmap", Value: "+5 bad/90000"},
		{Key: "rtpmap", Value: "96"},
		{Key: "rtpmap", Value: "96 "},
		{Key: "rtpmap", Value: "x VP8/90000"},
		{Key: "rtpmap", Value: "127 last/90000"},
		{Key: "rtpmap", Value: "0 PCMU/8000"},
		{Key: "rtpmap", Value: "0 dup/8000"},
		{Key: "fmtp", Value: "300 apt=1"},
		{Key: "fmtp", Value: "97 apt=96"},
	}}
	if got := m.SSRCs(); !reflect.DeepEqual(got, []uint32{7, 4294967295}) {
		t.Fatalf("ssrcs %v", got)
	}
	if got := m.FIDGroups(); !reflect.DeepEqual(got, [][2]uint32{{5, 6}}) {
		t.Fatalf("fid %v", got)
	}
	if got := m.RTPMaps(); !reflect.DeepEqual(got, map[uint8]string{127: "last/90000", 0: "PCMU/8000"}) {
		t.Fatalf("rtpmaps %v", got)
	}
	if got := m.Fmtps(); !reflect.DeepEqual(got, map[uint8]string{97: "apt=96"}) {
		t.Fatalf("fmtps %v", got)
	}
	var nilMedia *Media
	if nilMedia.Has("x") || nilMedia.Mid() != "" || nilMedia.Direction() != "sendrecv" || nilMedia.SSRCs() != nil || nilMedia.FIDGroups() != nil || nilMedia.Attrs("x") != nil || len(nilMedia.RTPMaps()) != 0 || len(nilMedia.Fmtps()) != 0 {
		t.Fatal("nil media helpers")
	}
	var nilSession *Session
	if _, ok := nilSession.Attr("x"); ok {
		t.Fatal("nil session attr")
	}
}

const minimal = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"

func TestMalformedRejected(t *testing.T) {
	cases := map[string]string{
		"empty":           "",
		"blank lines":     "\r\n\n\r\n",
		"no version":      "o=- 1 1 IN IP4 0.0.0.0\r\nv=0\r\ns=-\r\nt=0 0\r\n",
		"bad version":     "v=x\r\n" + minimal[5:],
		"negative ver":    "v=-1\r\n" + minimal[5:],
		"huge version":    "v=99999999999\r\n" + minimal[5:],
		"second version":  minimal + "v=0\r\n",
		"no equals":       minimal + "hello\r\n",
		"upper type":      minimal + "A=foo\r\n",
		"leading space":   minimal + " a=foo\r\n",
		"just letter":     minimal + "a\r\n",
		"bom":             "\xef\xbb\xbfv=0\r\n" + minimal[5:],
		"empty attr":      minimal + "a=\r\n",
		"colon attr":      minimal + "a=:value\r\n",
		"bare cr":         minimal + "a=x\ry=z\r\n",
		"nul":             minimal + "a=x\x00\r\n",
		"missing origin":  "v=0\r\ns=-\r\nt=0 0\r\n",
		"missing name":    "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nt=0 0\r\n",
		"missing timing":  "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\n",
		"double origin":   minimal + "o=- 2 2 IN IP4 0.0.0.0\r\n",
		"double name":     minimal + "s=again\r\n",
		"short m":         minimal + "m=audio 9\r\n",
		"empty m":         minimal + "m=\r\n",
		"bad port":        minimal + "m=audio x RTP/AVP 0\r\n",
		"big port":        minimal + "m=audio 65536 RTP/AVP 0\r\n",
		"neg port":        minimal + "m=audio -1 RTP/AVP 0\r\n",
		"bad port count":  minimal + "m=audio 9/x RTP/AVP 0\r\n",
		"zero port count": minimal + "m=audio 9/0 RTP/AVP 0\r\n",
		"empty count":     minimal + "m=audio 9/ RTP/AVP 0\r\n",
		"origin in media": minimal + "m=audio 9 RTP/AVP 0\r\no=- 1 1 IN IP4 0.0.0.0\r\n",
		"timing in media": minimal + "m=audio 9 RTP/AVP 0\r\nt=0 0\r\n",
		"bad attr media":  minimal + "m=audio 9 RTP/AVP 0\r\na=\r\n",
	}
	for name, text := range cases {
		if s, err := Parse(text); err == nil {
			t.Errorf("%s: accepted: %+v", name, s)
		}
	}
}

func TestSizeLimits(t *testing.T) {
	pad := strings.Repeat("a=x\r\n", (maxSize-len(minimal))/5)
	ok := minimal + pad
	ok += strings.Repeat("\n", maxSize-len(ok))
	if len(ok) != maxSize {
		t.Fatalf("setup %d", len(ok))
	}
	if _, err := Parse(ok); err != nil {
		t.Fatalf("exactly 64 KiB rejected: %v", err)
	}
	if _, err := Parse(ok + "\n"); !errors.Is(err, errTooLarge) {
		t.Fatalf("over 64 KiB: %v", err)
	}
	many := minimal + strings.Repeat("m=audio 9 RTP/AVP 0\n", maxMedia)
	s, err := Parse(many)
	if err != nil || len(s.Media) != maxMedia {
		t.Fatalf("512 media: %v", err)
	}
	if _, err := Parse(many + "m=audio 9 RTP/AVP 0\n"); !errors.Is(err, errTooManyMedia) {
		t.Fatalf("513 media: %v", err)
	}
}

func TestMalformedNoPanic(t *testing.T) {
	inputs := []string{
		"v=0", "v=0\n", "v=0\no=\ns=\nt=\n", "v=0\nm=a 1 b\n", minimal + "m=audio 9 RTP/AVP\na=ssrc:\na=ssrc-group:\na=rtpmap:\na=fmtp:\na=fingerprint:\na=ice-ufrag\n",
		minimal + "m=audio 9 RTP/AVP\na=rtpmap: \na=rtpmap:  x\na=fingerprint:sha-256\n",
		"\x00\x01\x02", "=\n", "v=\n", strings.Repeat("v", 70000),
	}
	for _, in := range inputs {
		s, err := Parse(in)
		if err != nil {
			continue
		}
		_ = s.String()
		for _, m := range s.Media {
			m.SSRCs()
			m.FIDGroups()
			m.RTPMaps()
			m.Fmtps()
			m.Direction()
			ICECredentials(s, m)
			Fingerprint(s, m)
		}
	}
}

func FuzzParse(f *testing.F) {
	f.Add(chromeOffer)
	f.Add(firefoxOffer)
	f.Add(minimal)
	f.Add(minimal + "m=audio 9/2 RTP/AVP 0\nc=\nc=IN IP4 1.2.3.4\nb=AS:1\ni=x\nq=y\na=k\na=k:v\n")
	f.Fuzz(func(t *testing.T, text string) {
		s, err := Parse(text)
		if err != nil {
			return
		}
		out := s.String()
		for _, m := range s.Media {
			m.SSRCs()
			m.FIDGroups()
			m.RTPMaps()
			m.Fmtps()
			ICECredentials(s, m)
			Fingerprint(s, m)
		}
		if len(out) > maxSize {
			return
		}
		again, err := Parse(out)
		if err != nil {
			t.Fatalf("reparse of output failed: %v\n%q", err, out)
		}
		if again.String() != out {
			t.Fatalf("output not stable\n%q\n%q", out, again.String())
		}
	})
}

func diffLine(want, got string) string {
	w := strings.Split(want, "\r\n")
	g := strings.Split(got, "\r\n")
	for i := 0; i < len(w) || i < len(g); i++ {
		var a, b string
		if i < len(w) {
			a = w[i]
		}
		if i < len(g) {
			b = g[i]
		}
		if a != b {
			return fmt.Sprintf("line %d:\nwant %q\ngot  %q", i+1, a, b)
		}
	}
	return "lengths differ"
}
