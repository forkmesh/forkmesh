package sdp

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

const (
	maxSize  = 64 * 1024
	maxMedia = 512
)

var (
	errTooLarge     = errors.New("sdp: description larger than 64 KiB")
	errTooManyMedia = errors.New("sdp: more than 512 media sections")
	errEmpty        = errors.New("sdp: empty description")
	errNoVersion    = errors.New("sdp: description does not start with v=")
	errMissing      = errors.New("sdp: missing o=, s= or t= line")
)

type Session struct {
	Version    int
	Origin     string
	Name       string
	Timing     string
	Attributes []Attribute
	Media      []*Media

	beforeTiming []string
	afterTiming  []string
}

type Media struct {
	Kind       string
	Port       int
	Proto      string
	Formats    []string
	Connection string
	Bandwidth  []string
	Attributes []Attribute

	portCount        string
	info             []string
	extraConnections []string
	extra            []string
}

type Attribute struct {
	Key   string
	Value string
	Flag  bool
}

func Parse(text string) (*Session, error) {
	if len(text) > maxSize {
		return nil, errTooLarge
	}
	s := &Session{}
	var media *Media
	seenVersion, seenOrigin, seenName, seenTiming := false, false, false, false
	lineNo := 0
	for len(text) > 0 {
		var line string
		if i := strings.IndexByte(text, '\n'); i >= 0 {
			line, text = text[:i], text[i+1:]
		} else {
			line, text = text, ""
		}
		lineNo++
		line = strings.TrimSuffix(line, "\r")
		if line == "" {
			continue
		}
		if len(line) < 2 || line[1] != '=' || line[0] < 'a' || line[0] > 'z' {
			return nil, fmt.Errorf("sdp: line %d: not of the form x=value", lineNo)
		}
		if strings.ContainsAny(line, "\r\x00") {
			return nil, fmt.Errorf("sdp: line %d: control character in line", lineNo)
		}
		typ, value := line[0], line[2:]
		if !seenVersion {
			if typ != 'v' {
				return nil, errNoVersion
			}
			v, ok := parseDigits(value, 1<<30)
			if !ok {
				return nil, fmt.Errorf("sdp: line %d: bad version %q", lineNo, value)
			}
			s.Version = v
			seenVersion = true
			continue
		}
		if typ == 'v' {
			return nil, fmt.Errorf("sdp: line %d: second v= line", lineNo)
		}
		if typ == 'm' {
			if len(s.Media) >= maxMedia {
				return nil, errTooManyMedia
			}
			m, err := parseMediaLine(value)
			if err != nil {
				return nil, fmt.Errorf("sdp: line %d: %w", lineNo, err)
			}
			media = m
			s.Media = append(s.Media, m)
			continue
		}
		if typ == 'a' {
			a, err := parseAttribute(value)
			if err != nil {
				return nil, fmt.Errorf("sdp: line %d: %w", lineNo, err)
			}
			if media != nil {
				media.Attributes = append(media.Attributes, a)
			} else {
				s.Attributes = append(s.Attributes, a)
			}
			continue
		}
		if media != nil {
			switch typ {
			case 'o', 's', 't':
				return nil, fmt.Errorf("sdp: line %d: %c= inside a media section", lineNo, typ)
			case 'i':
				media.info = append(media.info, line)
			case 'c':
				if media.Connection == "" && value != "" && len(media.extraConnections) == 0 {
					media.Connection = value
				} else {
					media.extraConnections = append(media.extraConnections, line)
				}
			case 'b':
				media.Bandwidth = append(media.Bandwidth, value)
			default:
				media.extra = append(media.extra, line)
			}
			continue
		}
		switch typ {
		case 'o':
			if seenOrigin {
				return nil, fmt.Errorf("sdp: line %d: second o= line", lineNo)
			}
			s.Origin, seenOrigin = value, true
		case 's':
			if seenName {
				return nil, fmt.Errorf("sdp: line %d: second s= line", lineNo)
			}
			s.Name, seenName = value, true
		case 't':
			if seenTiming {
				s.afterTiming = append(s.afterTiming, line)
			} else {
				s.Timing, seenTiming = value, true
			}
		case 'i', 'u', 'e', 'p', 'c', 'b':
			s.beforeTiming = append(s.beforeTiming, line)
		default:
			s.afterTiming = append(s.afterTiming, line)
		}
	}
	if !seenVersion {
		return nil, errEmpty
	}
	if !seenOrigin || !seenName || !seenTiming {
		return nil, errMissing
	}
	return s, nil
}

func parseDigits(v string, max int) (int, bool) {
	if v == "" || len(v) > 10 {
		return 0, false
	}
	n := 0
	for i := 0; i < len(v); i++ {
		c := v[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
	}
	if n > max {
		return 0, false
	}
	return n, true
}

func parseMediaLine(value string) (*Media, error) {
	fields := strings.Fields(value)
	if len(fields) < 3 {
		return nil, errors.New("m= line needs media, port and proto")
	}
	m := &Media{Kind: fields[0], Proto: fields[2]}
	port := fields[1]
	if i := strings.IndexByte(port, '/'); i >= 0 {
		count, ok := parseDigits(port[i+1:], 65535)
		if !ok || count == 0 {
			return nil, fmt.Errorf("bad port count %q", port)
		}
		m.portCount = port[i+1:]
		port = port[:i]
	}
	p, ok := parseDigits(port, 65535)
	if !ok {
		return nil, fmt.Errorf("bad port %q", fields[1])
	}
	m.Port = p
	if len(fields) > 3 {
		m.Formats = append([]string(nil), fields[3:]...)
	}
	return m, nil
}

func parseAttribute(value string) (Attribute, error) {
	if i := strings.IndexByte(value, ':'); i >= 0 {
		if i == 0 {
			return Attribute{}, errors.New("attribute without a name")
		}
		return Attribute{Key: value[:i], Value: value[i+1:]}, nil
	}
	if value == "" {
		return Attribute{}, errors.New("attribute without a name")
	}
	return Attribute{Key: value, Flag: true}, nil
}

func (s *Session) String() string {
	var b strings.Builder
	line := func(typ byte, v string) {
		b.WriteByte(typ)
		b.WriteByte('=')
		b.WriteString(v)
		b.WriteString("\r\n")
	}
	raw := func(lines []string) {
		for _, l := range lines {
			b.WriteString(l)
			b.WriteString("\r\n")
		}
	}
	line('v', strconv.Itoa(s.Version))
	line('o', s.Origin)
	line('s', s.Name)
	raw(s.beforeTiming)
	line('t', s.Timing)
	raw(s.afterTiming)
	writeAttributes(&b, s.Attributes)
	for _, m := range s.Media {
		if m == nil {
			continue
		}
		b.WriteString("m=")
		b.WriteString(m.Kind)
		b.WriteByte(' ')
		b.WriteString(strconv.Itoa(m.Port))
		if m.portCount != "" {
			b.WriteByte('/')
			b.WriteString(m.portCount)
		}
		b.WriteByte(' ')
		b.WriteString(m.Proto)
		for _, f := range m.Formats {
			b.WriteByte(' ')
			b.WriteString(f)
		}
		b.WriteString("\r\n")
		raw(m.info)
		if m.Connection != "" {
			line('c', m.Connection)
		}
		raw(m.extraConnections)
		for _, bw := range m.Bandwidth {
			line('b', bw)
		}
		raw(m.extra)
		writeAttributes(&b, m.Attributes)
	}
	return b.String()
}

func writeAttributes(b *strings.Builder, attrs []Attribute) {
	for _, a := range attrs {
		b.WriteString("a=")
		b.WriteString(a.Key)
		if !a.Flag {
			b.WriteByte(':')
			b.WriteString(a.Value)
		}
		b.WriteString("\r\n")
	}
}

func lookup(attrs []Attribute, key string) (string, bool) {
	for _, a := range attrs {
		if a.Key == key {
			return a.Value, true
		}
	}
	return "", false
}

func (s *Session) Attr(key string) (string, bool) {
	if s == nil {
		return "", false
	}
	return lookup(s.Attributes, key)
}

func (m *Media) Attr(key string) (string, bool) {
	if m == nil {
		return "", false
	}
	return lookup(m.Attributes, key)
}

func (m *Media) Attrs(key string) []string {
	if m == nil {
		return nil
	}
	var out []string
	for _, a := range m.Attributes {
		if a.Key == key {
			out = append(out, a.Value)
		}
	}
	return out
}

func (m *Media) Has(key string) bool {
	_, ok := m.Attr(key)
	return ok
}

func (m *Media) Mid() string {
	v, _ := m.Attr("mid")
	return v
}

func (m *Media) Direction() string {
	if m == nil {
		return "sendrecv"
	}
	for _, a := range m.Attributes {
		switch a.Key {
		case "sendrecv", "sendonly", "recvonly", "inactive":
			return a.Key
		}
	}
	return "sendrecv"
}

func parseSSRC(v string) (uint32, bool) {
	n, err := strconv.ParseUint(v, 10, 32)
	if err != nil {
		return 0, false
	}
	return uint32(n), true
}

func (m *Media) SSRCs() []uint32 {
	if m == nil {
		return nil
	}
	var out []uint32
	seen := map[uint32]bool{}
	for _, v := range m.Attrs("ssrc") {
		id := v
		if i := strings.IndexByte(v, ' '); i >= 0 {
			id = v[:i]
		}
		n, ok := parseSSRC(id)
		if !ok || seen[n] {
			continue
		}
		seen[n] = true
		out = append(out, n)
	}
	return out
}

func (m *Media) FIDGroups() [][2]uint32 {
	if m == nil {
		return nil
	}
	var out [][2]uint32
	for _, v := range m.Attrs("ssrc-group") {
		f := strings.Fields(v)
		if len(f) < 3 || f[0] != "FID" {
			continue
		}
		a, ok1 := parseSSRC(f[1])
		b, ok2 := parseSSRC(f[2])
		if !ok1 || !ok2 {
			continue
		}
		out = append(out, [2]uint32{a, b})
	}
	return out
}

func payloadMap(m *Media, key string) map[uint8]string {
	out := map[uint8]string{}
	if m == nil {
		return out
	}
	for _, v := range m.Attrs(key) {
		i := strings.IndexByte(v, ' ')
		if i < 0 {
			continue
		}
		pt, ok := parseDigits(v[:i], 127)
		if !ok {
			continue
		}
		rest := strings.TrimSpace(v[i+1:])
		if rest == "" {
			continue
		}
		if _, dup := out[uint8(pt)]; dup {
			continue
		}
		out[uint8(pt)] = rest
	}
	return out
}

func (m *Media) RTPMaps() map[uint8]string {
	return payloadMap(m, "rtpmap")
}

func (m *Media) Fmtps() map[uint8]string {
	return payloadMap(m, "fmtp")
}

func pick(s *Session, m *Media, key string) (string, bool) {
	if v, ok := m.Attr(key); ok {
		return v, true
	}
	return s.Attr(key)
}

func ICECredentials(s *Session, m *Media) (ufrag, pwd string) {
	ufrag, _ = pick(s, m, "ice-ufrag")
	pwd, _ = pick(s, m, "ice-pwd")
	return strings.TrimSpace(ufrag), strings.TrimSpace(pwd)
}

func Fingerprint(s *Session, m *Media) (algorithm, value string) {
	v, ok := pick(s, m, "fingerprint")
	if !ok {
		return "", ""
	}
	f := strings.Fields(v)
	if len(f) != 2 {
		return "", ""
	}
	return f[0], f[1]
}

func (m *Media) Extensions() map[string]int {
	out := map[string]int{}
	for _, v := range m.Attrs("extmap") {
		f := strings.Fields(v)
		if len(f) < 2 {
			continue
		}
		id := f[0]
		if i := strings.IndexByte(id, '/'); i >= 0 {
			id = id[:i]
		}
		n, ok := parseDigits(id, 255)
		if !ok || n == 0 {
			continue
		}
		if _, dup := out[f[1]]; dup {
			continue
		}
		out[f[1]] = n
	}
	return out
}

func (m *Media) RTXFor(primary uint8) (uint8, bool) {
	maps := m.RTPMaps()
	fmtps := m.Fmtps()
	best := -1
	for pt, codec := range maps {
		if !strings.HasPrefix(strings.ToLower(codec), "rtx/") {
			continue
		}
		for _, param := range strings.Split(fmtps[pt], ";") {
			key, value, ok := strings.Cut(strings.TrimSpace(param), "=")
			if !ok || key != "apt" {
				continue
			}
			apt, ok := parseDigits(strings.TrimSpace(value), 127)
			if ok && uint8(apt) == primary && (best < 0 || int(pt) < best) {
				best = int(pt)
			}
		}
	}
	if best < 0 {
		return 0, false
	}
	return uint8(best), true
}

func (m *Media) Feedback(pt uint8) []string {
	want := strconv.Itoa(int(pt))
	var out []string
	for _, v := range m.Attrs("rtcp-fb") {
		i := strings.IndexByte(v, ' ')
		if i < 0 {
			continue
		}
		if v[:i] == want || v[:i] == "*" {
			out = append(out, strings.TrimSpace(v[i+1:]))
		}
	}
	return out
}
