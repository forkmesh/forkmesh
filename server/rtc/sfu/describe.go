package sfu

import (
	"strconv"
	"strings"

	"forkmesh.com/mirror-node/rtc/rtp"
	"forkmesh.com/mirror-node/rtc/sdp"
)

const (
	audioPayloadType    = 111
	videoPayloadType    = 96
	videoRTXPayloadType = 97
)

type upstreamSection struct {
	mid            string
	kind           string
	proto          string
	formats        []string
	accepted       bool
	payloadType    uint8
	direction      string
	rtx            bool
	rtxPayloadType uint8
	tccID          uint8
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
				if id := media.Extensions()[rtp.TransportCCURI]; id > 0 && id <= 255 {
					section.tccID = uint8(id)
				}
				if media.Kind == "video" {
					if rtx, ok := media.RTXFor(pt); ok && rtx != audioPayloadType && rtx != videoPayloadType &&
						(rtx == videoRTXPayloadType) == (pt == videoPayloadType) {
						section.rtx, section.rtxPayloadType = true, rtx
					}
				}
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
	tccID       uint8
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

func codecLines(lines []string, kind string, pt uint8, rtx uint8, tccID uint8, upstream bool) []string {
	p := strconv.Itoa(int(pt))
	if tccID != 0 {
		lines = append(lines, "a=extmap:"+strconv.Itoa(int(tccID))+" "+rtp.TransportCCURI)
	}
	if kind == "audio" {
		lines = append(lines,
			"a=rtpmap:"+p+" opus/48000/2",
			"a=fmtp:"+p+" minptime=10;useinbandfec=1",
		)
		if tccID != 0 {
			lines = append(lines, "a=rtcp-fb:"+p+" transport-cc")
		}
		return lines
	}
	lines = append(lines, "a=rtpmap:"+p+" VP8/90000")
	if upstream {
		lines = append(lines, "a=rtcp-fb:"+p+" goog-remb")
	}
	if tccID != 0 {
		lines = append(lines, "a=rtcp-fb:"+p+" transport-cc")
	}
	lines = append(lines,
		"a=rtcp-fb:"+p+" ccm fir",
		"a=rtcp-fb:"+p+" nack",
		"a=rtcp-fb:"+p+" nack pli",
	)
	if rtx != 0 {
		r := strconv.Itoa(int(rtx))
		lines = append(lines, "a=rtpmap:"+r+" rtx/90000", "a=fmtp:"+r+" apt="+p)
	}
	return lines
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
	formats := strconv.Itoa(int(section.payloadType))
	var rtx uint8
	if section.rtx {
		rtx = section.rtxPayloadType
		formats += " " + strconv.Itoa(int(rtx))
	}
	lines = append(lines, "m="+section.kind+" 9 UDP/TLS/RTP/SAVPF "+formats)
	lines = d.transport(lines)
	lines = append(lines, "a=mid:"+section.mid, "a="+section.direction, "a=rtcp-mux", "a=rtcp-rsize")
	lines = codecLines(lines, section.kind, section.payloadType, rtx, section.tccID, true)
	return d.candidateLines(lines)
}

func (d description) slotLines(lines []string, slot *slot) []string {
	pt := uint8(audioPayloadType)
	var rtx uint8
	formats := strconv.Itoa(audioPayloadType)
	if slot.kind == "video" {
		pt, rtx = videoPayloadType, videoRTXPayloadType
		formats = strconv.Itoa(videoPayloadType) + " " + strconv.Itoa(videoRTXPayloadType)
	}
	lines = append(lines, "m="+slot.kind+" 9 UDP/TLS/RTP/SAVPF "+formats)
	lines = d.transport(lines)
	direction := "inactive"
	if slot.track != nil {
		direction = "sendonly"
	}
	lines = append(lines, "a=mid:"+slot.mid, "a="+direction, "a=rtcp-mux", "a=rtcp-rsize")
	lines = codecLines(lines, slot.kind, pt, rtx, d.tccID, false)
	if slot.track != nil {
		stream := slot.track.owner.id
		ssrc := strconv.FormatUint(uint64(slot.track.ssrc), 10)
		lines = append(lines, "a=msid:"+stream+" "+slot.track.id)
		if slot.rtx != 0 {
			lines = append(lines, "a=ssrc-group:FID "+ssrc+" "+strconv.FormatUint(uint64(slot.rtx), 10))
		}
		lines = append(lines,
			"a=ssrc:"+ssrc+" cname:"+stream,
			"a=ssrc:"+ssrc+" msid:"+stream+" "+slot.track.id,
		)
		if slot.rtx != 0 {
			rtxSSRC := strconv.FormatUint(uint64(slot.rtx), 10)
			lines = append(lines,
				"a=ssrc:"+rtxSSRC+" cname:"+stream,
				"a=ssrc:"+rtxSSRC+" msid:"+stream+" "+slot.track.id,
			)
		}
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
