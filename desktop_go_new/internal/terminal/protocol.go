package terminal

import (
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"math"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxCoreTerminals      = 64
	replayBytes           = 128 * 1024
	replayLineSearchBytes = 8 * 1024
	maxInputBytes         = 16 * 1024
	maxEventBytes         = 16 * 1024
	maxPendingBytes       = 256 * 1024
	idChars               = 32
	readChunkBytes        = 6144
	relayedInterval       = 80 * time.Millisecond
	directInterval        = 15 * time.Millisecond
	coreOutputEvent       = "core.terminal.output"
	coreExitEvent         = "core.terminal.exit"
	errUnknownTerminal    = "unknown_terminal"
	errBadInput           = "bad_input"
	errBadRequest         = "bad_request"
	errTerminalLimit      = "terminal_limit"
	errTerminalFailed     = "terminal_failed"
	errCliMissing         = "cli_missing"
	errForbidden          = "forbidden"
	errUnsupported        = "unsupported"
	maxInputBase64Chars   = (maxInputBytes/3 + 1) * 4
	stopTermAfter         = 50 * time.Millisecond
	stopKillAfter         = 150 * time.Millisecond
	stopGiveUpAfter       = 200 * time.Millisecond
)

func followInterval(transport rpc.Transport) time.Duration {
	if transport == rpc.Relay {
		return relayedInterval
	}
	return directInterval
}

func cliProgram(provider string) string {
	switch provider {
	case "claude-code":
		return "claude"
	case "codex":
		return "codex"
	}
	return ""
}

func cliLabel(provider string) string {
	switch provider {
	case "claude-code":
		return "Claude Code"
	case "codex":
		return "Codex"
	}
	return ""
}

func validTerminalID(id string) bool {
	if len(id) != idChars {
		return false
	}
	for i := 0; i < len(id); i++ {
		c := id[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

func newTerminalID() string {
	buf := make([]byte, idChars/2)
	_, _ = rand.Read(buf)
	return hex.EncodeToString(buf)
}

func bound(low, value, high int) int {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}

func clampCols(cols int) int { return bound(20, cols, 500) }

func clampRows(rows int) int { return bound(5, rows, 200) }

func jsonInt(value any, fallback int) int {
	f, ok := value.(float64)
	if !ok || f != math.Trunc(f) || f < math.MinInt32 || f > math.MaxInt32 {
		return fallback
	}
	return int(f)
}

func jsonString(value any) string {
	s, _ := value.(string)
	return s
}

func appendReplay(replay, chunk []byte, limit int) []byte {
	replay = append(replay, chunk...)
	if len(replay) <= limit {
		return replay
	}
	cut := len(replay) - limit
	if line := bytes.IndexByte(replay[cut:], '\n'); line >= 0 && line < replayLineSearchBytes {
		cut += line + 1
	} else {
		for cut < len(replay) && replay[cut]&0xC0 == 0x80 {
			cut++
		}
	}
	kept := copy(replay, replay[cut:])
	return replay[:kept]
}

func qtBase64Decode(input []byte) ([]byte, bool) {
	out := make([]byte, 0, len(input)*3/4+3)
	var buf uint32
	nbits := 0
	size := len(input)
	for i := 0; i < size; i++ {
		ch := input[i]
		d := -1
		switch {
		case ch >= 'A' && ch <= 'Z':
			d = int(ch - 'A')
		case ch >= 'a' && ch <= 'z':
			d = int(ch-'a') + 26
		case ch >= '0' && ch <= '9':
			d = int(ch-'0') + 52
		case ch == '+':
			d = 62
		case ch == '/':
			d = 63
		case ch == '=':
			if size%4 != 0 {
				return nil, false
			}
			if i == size-1 {
				break
			}
			if i == size-2 {
				i++
				if input[i] == '=' {
					break
				}
			}
			return nil, false
		default:
			return nil, false
		}
		if d != -1 {
			buf = buf<<6 | uint32(d)
			nbits += 6
			if nbits >= 8 {
				nbits -= 8
				out = append(out, byte(buf>>uint(nbits)))
				buf &= (1 << uint(nbits)) - 1
			}
		}
	}
	return out, true
}

func latin1(text string) []byte {
	out := make([]byte, 0, len(text))
	for _, r := range text {
		if r > 0xFF {
			out = append(out, '?')
		} else {
			out = append(out, byte(r))
		}
	}
	return out
}

func decodeInput(data any) ([]byte, error) {
	text, ok := data.(string)
	if !ok {
		return nil, rpc.Fail(errBadInput)
	}
	raw := latin1(text)
	if len(raw) == 0 || len(raw) > maxInputBase64Chars {
		return nil, rpc.Fail(errBadInput)
	}
	decoded, ok := qtBase64Decode(raw)
	if !ok || len(decoded) == 0 || len(decoded) > maxInputBytes {
		return nil, rpc.Fail(errBadInput)
	}
	return decoded, nil
}

type outbox struct {
	pending []byte
	reset   bool
	sentAt  time.Time
}

func (b *outbox) queue(chunk []byte, maxPending int) {
	if b.reset {
		return
	}
	if len(b.pending)+len(chunk) > maxPending {
		b.pending = nil
		b.reset = true
		return
	}
	b.pending = append(b.pending, chunk...)
}

func (b *outbox) hasOutput() bool {
	return b.reset || len(b.pending) > 0
}

func (b *outbox) take(terminal string, replay []byte, maxBytes int) map[string]any {
	var data []byte
	reset := b.reset
	if reset {
		data = replay
		b.pending = nil
		b.reset = false
	} else {
		size := min(len(b.pending), maxBytes)
		for size < len(b.pending) && size > 0 && b.pending[size]&0xC0 == 0x80 {
			size--
		}
		if size == 0 {
			size = min(len(b.pending), maxBytes)
		}
		data = b.pending[:size]
		rest := len(b.pending) - size
		if rest == 0 {
			b.pending = nil
		} else {
			b.pending = append([]byte(nil), b.pending[size:]...)
		}
	}
	return map[string]any{
		"terminal": terminal,
		"data":     base64.StdEncoding.EncodeToString(data),
		"reset":    reset,
	}
}

type meta struct {
	id        string
	provider  string
	cli       string
	owner     string
	name      string
	cwd       string
	device    string
	mode      string
	account   string
	startedMs int64
	session   int
}

func terminalJSON(m meta, running bool, cols, rows int) map[string]any {
	out := map[string]any{
		"terminal":        m.id,
		"provider":        m.provider,
		"label":           cliLabel(m.provider),
		"cli":             m.cli,
		"repositoryOwner": m.owner,
		"repositoryName":  m.name,
		"cwd":             m.cwd,
		"startedAtMs":     m.startedMs,
		"device":          m.device,
		"running":         running,
		"cols":            cols,
		"rows":            rows,
	}
	if m.session > 0 {
		out["session"] = m.session
	}
	if m.mode != "" {
		out["mode"] = m.mode
	}
	if m.account != "" {
		out["account"] = m.account
	}
	return out
}
