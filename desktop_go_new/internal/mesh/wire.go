package mesh

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"math"
	"net"
	"strconv"
	"strings"
)

const (
	ProtocolVersion      = 1
	DefaultDiscoveryPort = 48732
	Conversation         = "#mesh"
	RoomName             = "forkmesh-lan-mesh-v1"
	MulticastGroupV4     = "239.192.79.42"
	BluetoothServiceUUID = "7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11"
	BeaconTag            = "forkmesh-lan-mesh"
	ControlHelloTag      = "fm-control-hello"

	MaxFrameBytes        = 24 * 1024 * 1024
	maxDisplayNameChars  = 32
	maxVersionChars      = 32
	maxWakeTokenChars    = 200
	maxWakeTokens        = 32
	maxTextChars         = 16000
	maxFileNameChars     = 180
	maxMimeChars         = 100
	maxFileBytes         = 16 * 1024 * 1024
	maxBase64FileChars   = 22 * 1024 * 1024
	maxIDChars           = 160
	historyFileVersion   = 1
	seenCacheLimit       = 4096
	maxLinks             = 64
	maxPendingFrames     = 32
	maxEntriesPerChannel = 200
	maxCharsPerChannel   = 2 * 1024 * 1024
)

type Object = map[string]any

type Beacon struct {
	NodeID      string
	Name        string
	NodeName    string
	OwnerUser   string
	AccountKind string
	Platform    string
	Version     string
	Port        int
	TimestampMs int64
	Reply       bool
	Control     bool
}

func BeaconObject(b Beacon) Object {
	object := Object{"t": BeaconTag, "v": ProtocolVersion, "id": b.NodeID, "port": b.Port, "ts": float64(b.TimestampMs)}
	for key, value := range map[string]string{"name": b.Name, "nodeName": b.NodeName, "ownerUser": b.OwnerUser,
		"accountKind": b.AccountKind, "platform": b.Platform, "version": b.Version} {
		if value != "" {
			object[key] = value
		}
	}
	if b.Reply {
		object["reply"] = true
	}
	if b.Control {
		object["control"] = true
	}
	return object
}

func ParseBeacon(object Object) (Beacon, bool) {
	if str(object["t"]) != BeaconTag || qtInt(object["v"]) != ProtocolVersion {
		return Beacon{}, false
	}
	nodeID := left16(trimmed(str(object["id"])), maxIDChars)
	port := qtInt(object["port"])
	if nodeID == "" || port <= 0 || port > 65535 {
		return Beacon{}, false
	}
	return Beacon{
		NodeID:      nodeID,
		Port:        port,
		Name:        boundedText(object, "name", maxDisplayNameChars),
		NodeName:    boundedText(object, "nodeName", maxDisplayNameChars),
		OwnerUser:   boundedText(object, "ownerUser", maxDisplayNameChars),
		AccountKind: boundedText(object, "accountKind", 16),
		Platform:    boundedText(object, "platform", 16),
		Version:     boundedText(object, "version", maxVersionChars),
		TimestampMs: int64(num(object["ts"])),
		Reply:       boolean(object["reply"]),
		Control:     boolean(object["control"]),
	}, true
}

func IsLocalNetworkAddress(ip net.IP) bool {
	if ip == nil {
		return false
	}
	if v4 := ip.To4(); v4 != nil {
		switch {
		case v4[0] == 127, v4[0] == 10:
			return true
		case v4[0] == 172 && v4[1]&0xf0 == 16:
			return true
		case v4[0] == 192 && v4[1] == 168:
			return true
		case v4[0] == 169 && v4[1] == 254:
			return true
		}
		return false
	}
	if len(ip) != net.IPv6len {
		return false
	}
	if ip.Equal(net.IPv6loopback) {
		return true
	}
	if ip[0]&0xfe == 0xfc {
		return true
	}
	return ip[0] == 0xfe && ip[1]&0xc0 == 0x80
}

func SweepAddresses(ip net.IP, prefixLength int) []net.IP {
	v4 := ip.To4()
	if v4 == nil || prefixLength > 30 || v4[0] == 127 || !IsLocalNetworkAddress(v4) || (v4[0] == 169 && v4[1] == 254) {
		return nil
	}
	bits := prefixLength
	if bits < 24 {
		bits = 24
	}
	hostBits := uint(32 - bits)
	self := binary.BigEndian.Uint32(v4)
	network := self & (0xffffffff << hostBits)
	count := uint32(1)<<hostBits - 1
	hosts := make([]net.IP, 0, count)
	for host := uint32(1); host < count; host++ {
		if network+host == self {
			continue
		}
		out := make(net.IP, 4)
		binary.BigEndian.PutUint32(out, network+host)
		hosts = append(hosts, out)
	}
	return hosts
}

func FrameBytes(payload []byte) []byte {
	out := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(out, uint32(len(payload)))
	copy(out[4:], payload)
	return out
}

func TakeFrame(buffer *[]byte) ([]byte, int) {
	if len(*buffer) < 4 {
		return nil, 0
	}
	length := binary.BigEndian.Uint32(*buffer)
	if length == 0 || int64(length) > MaxFrameBytes {
		return nil, -1
	}
	if len(*buffer) < int(length)+4 {
		return nil, 0
	}
	payload := append([]byte(nil), (*buffer)[4:4+int(length)]...)
	rest := (*buffer)[4+int(length):]
	if len(rest) == 0 {
		*buffer = nil
	} else {
		*buffer = append([]byte(nil), rest...)
	}
	return payload, 1
}

func WakeTokenExpiry(token string) int64 {
	if token == "" || len16(token) > maxWakeTokenChars {
		return 0
	}
	parts := strings.Split(token, ".")
	if len(parts) != 4 || parts[0] != "mw1" {
		return 0
	}
	expires, err := strconv.ParseInt(strings.TrimSpace(parts[2]), 10, 64)
	if err != nil || expires <= 0 {
		return 0
	}
	return expires
}

func IsControlHello(payload []byte) bool {
	if !bytes.Contains(payload, []byte(`"fm-control-hello"`)) {
		return false
	}
	object, ok := parseObject(payload)
	return ok && str(object["t"]) == ControlHelloTag
}

func marshal(value any) []byte {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return nil
	}
	return bytes.TrimRight(buffer.Bytes(), "\n")
}

func parseObject(raw []byte) (Object, bool) {
	var object Object
	decoder := json.NewDecoder(bytes.NewReader(raw))
	if err := decoder.Decode(&object); err != nil || object == nil {
		return nil, false
	}
	if decoder.More() {
		return nil, false
	}
	return object, true
}

func str(value any) string {
	text, _ := value.(string)
	return text
}

func boolean(value any) bool {
	flag, _ := value.(bool)
	return flag
}

func num(value any) float64 {
	switch v := value.(type) {
	case float64:
		return v
	case int:
		return float64(v)
	case int64:
		return float64(v)
	case json.Number:
		f, _ := v.Float64()
		return f
	}
	return 0
}

func qtInt(value any) int {
	switch value.(type) {
	case float64, int, int64, json.Number:
	default:
		return 0
	}
	f := num(value)
	if math.IsNaN(f) || math.IsInf(f, 0) || f != math.Trunc(f) || f > math.MaxInt32 || f < math.MinInt32 {
		return 0
	}
	return int(f)
}

func trimmed(text string) string {
	return strings.TrimSpace(text)
}

func len16(text string) int {
	count := 0
	for _, r := range text {
		if r >= 0x10000 {
			count += 2
		} else {
			count++
		}
	}
	return count
}

func left16(text string, limit int) string {
	if limit <= 0 {
		return ""
	}
	count := 0
	for index, r := range text {
		width := 1
		if r >= 0x10000 {
			width = 2
		}
		if count+width > limit {
			return text[:index]
		}
		count += width
	}
	return text
}

func boundedText(object Object, key string, limit int) string {
	return left16(str(object[key]), limit)
}

func SafeFileName(name string) string {
	cleaned := strings.ReplaceAll(name, "\\", "/")
	if slash := strings.LastIndexByte(cleaned, '/'); slash >= 0 {
		cleaned = cleaned[slash+1:]
	}
	cleaned = strings.ReplaceAll(trimmed(cleaned), "\x00", "")
	if cleaned == "" {
		cleaned = "file"
	}
	return left16(cleaned, maxFileNameChars)
}

func qtBase64(text string) []byte {
	filtered := make([]byte, 0, len(text))
	for i := 0; i < len(text); i++ {
		c := text[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' {
			filtered = append(filtered, c)
		}
	}
	if len(filtered)%4 == 1 {
		filtered = filtered[:len(filtered)-1]
	}
	out, err := base64.RawStdEncoding.DecodeString(string(filtered))
	if err != nil {
		return nil
	}
	return out
}

func boundedBase64(object Object, key string) []byte {
	encoded := str(object[key])
	if encoded == "" || len16(encoded) > maxBase64FileChars {
		return nil
	}
	decoded := qtBase64(encoded)
	if len(decoded) > maxFileBytes {
		return nil
	}
	return decoded
}

func stdBase64(raw []byte) string { return base64.StdEncoding.EncodeToString(raw) }
