package sfu

import (
	"slices"
	"strings"
	"testing"

	"forkmesh.com/mirror-node/rtc/rtp"

	"forkmesh.com/mirror-node/rtc/sdp"
)

const chromeOffer = "v=0\r\n" +
	"o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n" +
	"s=-\r\nt=0 0\r\n" +
	"a=group:BUNDLE 0 1 2\r\n" +
	"a=msid-semantic: WMS stream\r\n" +
	"m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 126\r\n" +
	"c=IN IP4 0.0.0.0\r\n" +
	"a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdefghijklmn\r\n" +
	"a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n" +
	"a=setup:actpass\r\na=mid:0\r\na=sendonly\r\na=msid:stream audio-track\r\na=rtcp-mux\r\n" +
	"a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\na=rtpmap:63 red/48000/2\r\n" +
	"a=rtpmap:9 G722/8000\r\na=rtpmap:126 telephone-event/8000\r\n" +
	"a=ssrc:1111 cname:x\r\n" +
	"m=video 9 UDP/TLS/RTP/SAVPF 96 97 102\r\n" +
	"c=IN IP4 0.0.0.0\r\n" +
	"a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdefghijklmn\r\n" +
	"a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n" +
	"a=setup:actpass\r\na=mid:1\r\na=sendonly\r\na=rtcp-mux\r\n" +
	"a=rtpmap:96 VP8/90000\r\na=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=96\r\na=rtpmap:102 H264/90000\r\n" +
	"a=ssrc-group:FID 2222 3333\r\na=ssrc:2222 cname:x\r\na=ssrc:3333 cname:x\r\n" +
	"m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n" +
	"c=IN IP4 0.0.0.0\r\na=mid:2\r\na=sctp-port:5000\r\n"

func TestTheAnswerMirrorsTheOfferAndReceivesOnly(t *testing.T) {
	offer, err := sdp.Parse(chromeOffer)
	if err != nil {
		t.Fatal(err)
	}
	upstream := readUpstream(offer)
	if len(upstream) != 3 || !upstream[0].accepted || upstream[0].payloadType != 111 ||
		!upstream[1].accepted || upstream[1].payloadType != 96 || upstream[2].accepted {
		t.Fatalf("upstream %+v", upstream)
	}
	d := description{sessionID: "7", version: 1, ufrag: "sfuufrag", pwd: "sfupassword0123456789abc",
		fingerprint: "sha-256 11:22", candidates: []string{"1 1 udp 2130706431 203.0.113.5 3479 typ host"}}
	text := d.render(upstream, nil)
	answer, err := sdp.Parse(text)
	if err != nil {
		t.Fatalf("answer does not parse: %v\n%s", err, text)
	}
	if value, _ := answer.Attr("group"); value != "BUNDLE 0 1" {
		t.Fatalf("bundle %q", value)
	}
	if _, ok := answer.Attr("ice-lite"); !ok {
		t.Fatal("no ice-lite")
	}
	if len(answer.Media) != 3 {
		t.Fatalf("media %d", len(answer.Media))
	}
	for index, media := range answer.Media[:2] {
		if media.Mid() != upstream[index].mid || media.Direction() != "recvonly" {
			t.Fatalf("section %d mid %q direction %q", index, media.Mid(), media.Direction())
		}
		if setup, _ := media.Attr("setup"); setup != "passive" {
			t.Fatalf("setup %q", setup)
		}
		if !media.Has("rtcp-mux") || len(media.Attrs("candidate")) != 1 {
			t.Fatalf("transport lines missing in %d", index)
		}
	}
	if answer.Media[0].Formats[0] != "111" || len(answer.Media[0].Formats) != 1 ||
		answer.Media[1].Formats[0] != "96" || answer.Media[1].Formats[1] != "97" || len(answer.Media[1].Formats) != 2 {
		t.Fatalf("formats %v %v", answer.Media[0].Formats, answer.Media[1].Formats)
	}
	if rtx, ok := answer.Media[1].RTXFor(96); !ok || rtx != 97 {
		t.Fatal("the answer does not accept the sender's rtx")
	}
	feedback := answer.Media[1].Feedback(96)
	if !slices.Contains(feedback, "nack") || !slices.Contains(feedback, "nack pli") || !slices.Contains(feedback, "goog-remb") {
		t.Fatalf("video feedback %q", feedback)
	}
	if slices.Contains(feedback, "transport-cc") || strings.Contains(text, "extmap") {
		t.Fatal("transport-cc accepted although the offer has no extension for it")
	}
	if answer.Media[2].Port != 0 {
		t.Fatal("the data channel was not refused")
	}
	if strings.Contains(text, "H264") || strings.Contains(text, "G722") {
		t.Fatal("answer offers codecs the SFU does not forward")
	}
}

func TestAnOfferToAReceiverDescribesEachForwardedTrack(t *testing.T) {
	offer, _ := sdp.Parse(chromeOffer)
	upstream := readUpstream(offer)
	owner := &Participant{id: "0123456789abcdef0123456789abcdef"}
	slots := []*slot{
		{mid: "s0", kind: "audio", track: &Track{owner: owner, kind: "audio", ssrc: 5555, id: "audio-5555"}},
		{mid: "s1", kind: "video", track: &Track{owner: owner, kind: "video", ssrc: 6666, id: "video-6666"}, rtx: 7777},
		{mid: "s2", kind: "video"},
	}
	d := description{sessionID: "7", version: 3, ufrag: "u", pwd: "p", fingerprint: "sha-256 11:22", tccID: 3}
	session, err := sdp.Parse(d.render(upstream, slots))
	if err != nil {
		t.Fatal(err)
	}
	if value, _ := session.Attr("group"); value != "BUNDLE 0 1 s0 s1 s2" {
		t.Fatalf("bundle %q", value)
	}
	audio, video, empty := session.Media[3], session.Media[4], session.Media[5]
	if audio.Direction() != "sendonly" || audio.Formats[0] != "111" || audio.SSRCs()[0] != 5555 {
		t.Fatalf("audio slot %+v", audio)
	}
	if msid, _ := video.Attr("msid"); msid != owner.id+" video-6666" || video.Formats[0] != "96" {
		t.Fatalf("video msid %q", msid)
	}
	if empty.Direction() != "inactive" || len(empty.SSRCs()) != 0 {
		t.Fatal("an empty slot must be inactive")
	}
	if groups := video.FIDGroups(); len(groups) != 1 || groups[0] != [2]uint32{6666, 7777} {
		t.Fatalf("video fid %v", groups)
	}
	if rtx, ok := video.RTXFor(96); !ok || rtx != 97 || len(video.Formats) != 2 {
		t.Fatalf("video rtx %v %v", rtx, video.Formats)
	}
	for _, media := range []*sdp.Media{audio, video} {
		if media.Extensions()[rtp.TransportCCURI] != 3 {
			t.Fatalf("%s slot lacks transport-cc", media.Kind)
		}
	}
	feedback := video.Feedback(96)
	if !slices.Contains(feedback, "nack") || !slices.Contains(feedback, "transport-cc") || slices.Contains(feedback, "goog-remb") {
		t.Fatalf("slot feedback %q", feedback)
	}
	if !slices.Contains(audio.Feedback(111), "transport-cc") {
		t.Fatal("audio slot lacks transport-cc feedback")
	}
}

func TestTheAnswerAcceptsTransportWideCC(t *testing.T) {
	offerText := strings.Replace(chromeOffer, "a=rtpmap:96 VP8/90000\r\n",
		"a=extmap:5 "+rtp.TransportCCURI+"\r\na=rtpmap:96 VP8/90000\r\na=rtcp-fb:96 transport-cc\r\n", 1)
	offerText = strings.Replace(offerText, "a=rtpmap:111 opus/48000/2\r\n",
		"a=extmap:5 "+rtp.TransportCCURI+"\r\na=rtpmap:111 opus/48000/2\r\n", 1)
	offer, err := sdp.Parse(offerText)
	if err != nil {
		t.Fatal(err)
	}
	upstream := readUpstream(offer)
	if upstream[0].tccID != 5 || upstream[1].tccID != 5 {
		t.Fatalf("tcc ids %d %d", upstream[0].tccID, upstream[1].tccID)
	}
	d := description{sessionID: "7", version: 1, ufrag: "u", pwd: "p", fingerprint: "sha-256 11:22", tccID: 5}
	answer, err := sdp.Parse(d.render(upstream, nil))
	if err != nil {
		t.Fatal(err)
	}
	for _, media := range answer.Media[:2] {
		if media.Extensions()[rtp.TransportCCURI] != 5 || len(media.Extensions()) != 1 {
			t.Fatalf("%s extensions %v", media.Kind, media.Extensions())
		}
	}
	if !slices.Contains(answer.Media[1].Feedback(96), "transport-cc") || !slices.Contains(answer.Media[0].Feedback(111), "transport-cc") {
		t.Fatal("transport-cc feedback missing")
	}
}

func TestRTXIsRefusedWhenItsPayloadTypeCollides(t *testing.T) {
	cases := []struct{ vp8, rtx string }{{"96", "111"}, {"120", "97"}, {"120", "96"}}
	for _, c := range cases {
		text := strings.NewReplacer("a=rtpmap:96 VP8/90000", "a=rtpmap:"+c.vp8+" VP8/90000", "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102",
			"m=video 9 UDP/TLS/RTP/SAVPF "+c.vp8+" "+c.rtx+" 102", "a=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=96",
			"a=rtpmap:"+c.rtx+" rtx/90000\r\na=fmtp:"+c.rtx+" apt="+c.vp8).Replace(chromeOffer)
		offer, err := sdp.Parse(text)
		if err != nil {
			t.Fatal(err)
		}
		if section := readUpstream(offer)[1]; !section.accepted || section.rtx {
			t.Fatalf("vp8 %s rtx %s: %+v", c.vp8, c.rtx, section)
		}
	}
}
