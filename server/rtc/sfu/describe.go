package sfu

import (
	"strconv"
	"strings"

	"forkmesh.com/mirror-node/rtc/sdp"
)

const (
	audioPayloadType = 111
	videoPayloadType = 96
)

type upstreamSection struct {
	mid         string
	kind        string
	proto       string
	formats     []string
	accepted    bool
	payloadType uint8
	direction   string
}

func codecFor(kind string) string {
	if kind == "audio" {
		return "opus/48000/2"
	}
	return "vp8/90000"
}

func findPayloadType(media *sdp.Media) (uint8, bool) {
	want := codecFor(media.Kind)
	best := -1
	for pt, codec := range media.RTPMaps() {
		if strings.ToLower(codec) == want && (best < 0 || int(pt) < best) {
			best = int(pt)
		}
	}
	if best < 0 {
		return 0, false
	}
	return uint8(best), true
}

func readUpstream(session *sdp.Session) []upstreamSection {
	sections := make([]upstreamSection, 0, len(session.Media))
	for _, media := range session.Media {
		section := upstreamSection{
			mid:     media.Mid(),
			kind:    media.Kind,
			proto:   media.Proto,
			formats: media.Formats,
		}
		if media.Port != 0 && (media.Kind == "audio" || media.Kind == "video") && section.mid != "" {
			if pt, ok := findPayloadType(media); ok {
				section.accepted = true
				section.payloadType = pt
				switch media.Direction() {
				case "sendrecv", "sendonly":
					section.direction = "recvonly"
				default:
					section.direction = "inactive"
				}
			}
		}
		sections = append(sections, section)
	}
	return sections
}

type description struct {
	sessionID   string
	version     int
	ufrag       string
	pwd         string
	fingerprint string
	candidates  []string
	cname       string
}

func (d description) header(mids []string) []string {
	return []string{
		"v=0",
		"o=- " + d.sessionID + " " + strconv.Itoa(d.version) + " IN IP4 0.0.0.0",
		"s=-",
		"t=0 0",
		"a=ice-lite",
		"a=group:BUNDLE " + strings.Join(mids, " "),
		"a=msid-semantic: WMS *",
	}
}

func (d description) transport(lines []string) []string {
	lines = append(lines,
		"c=IN IP4 0.0.0.0",
		"a=rtcp:9 IN IP4 0.0.0.0",
		"a=ice-ufrag:"+d.ufrag,
		"a=ice-pwd:"+d.pwd,
		"a=fingerprint:"+d.fingerprint,
		"a=setup:passive",
	)
	return lines
}

func codecLines(lines []string, kind string, pt uint8) []string {
	p := strconv.Itoa(int(pt))
	if kind == "audio" {
		return append(lines,
			"a=rtpmap:"+p+" opus/48000/2",
			"a=fmtp:"+p+" minptime=10;useinbandfec=1",
		)
	}
	return append(lines,
		"a=rtpmap:"+p+" VP8/90000",
		"a=rtcp-fb:"+p+" goog-remb",
		"a=rtcp-fb:"+p+" ccm fir",
		"a=rtcp-fb:"+p+" nack pli",
	)
}

func (d description) candidateLines(lines []string) []string {
	for _, candidate := range d.candidates {
		lines = append(lines, "a=candidate:"+candidate)
	}
	return append(lines, "a=end-of-candidates")
}

func (d description) upstreamLines(lines []string, section upstreamSection) []string {
	if !section.accepted {
		lines = append(lines, "m="+section.kind+" 0 "+section.proto+" "+strings.Join(section.formats, " "))
		lines = append(lines, "c=IN IP4 0.0.0.0")
		if section.mid != "" {
			lines = append(lines, "a=mid:"+section.mid)
		}
		return append(lines, "a=inactive")
	}
	lines = append(lines, "m="+section.kind+" 9 UDP/TLS/RTP/SAVPF "+strconv.Itoa(int(section.payloadType)))
	lines = d.transport(lines)
	lines = append(lines, "a=mid:"+section.mid, "a="+section.direction, "a=rtcp-mux", "a=rtcp-rsize")
	lines = codecLines(lines, section.kind, section.payloadType)
	return d.candidateLines(lines)
}

func (d description) slotLines(lines []string, slot *slot) []string {
	pt := uint8(audioPayloadType)
	if slot.kind == "video" {
		pt = videoPayloadType
	}
	lines = append(lines, "m="+slot.kind+" 9 UDP/TLS/RTP/SAVPF "+strconv.Itoa(int(pt)))
	lines = d.transport(lines)
	direction := "inactive"
	if slot.track != nil {
		direction = "sendonly"
	}
	lines = append(lines, "a=mid:"+slot.mid, "a="+direction, "a=rtcp-mux", "a=rtcp-rsize")
	lines = codecLines(lines, slot.kind, pt)
	if slot.track != nil {
		stream := slot.track.owner.id
		ssrc := strconv.FormatUint(uint64(slot.track.ssrc), 10)
		lines = append(lines,
			"a=msid:"+stream+" "+slot.track.id,
			"a=ssrc:"+ssrc+" cname:"+stream,
			"a=ssrc:"+ssrc+" msid:"+stream+" "+slot.track.id,
		)
	}
	return d.candidateLines(lines)
}

func (d description) render(upstream []upstreamSection, slots []*slot) string {
	mids := []string{}
	for _, section := range upstream {
		if section.accepted {
			mids = append(mids, section.mid)
		}
	}
	for _, slot := range slots {
		mids = append(mids, slot.mid)
	}
	lines := d.header(mids)
	for _, section := range upstream {
		lines = d.upstreamLines(lines, section)
	}
	for _, slot := range slots {
		lines = d.slotLines(lines, slot)
	}
	return strings.Join(lines, "\r\n") + "\r\n"
}
