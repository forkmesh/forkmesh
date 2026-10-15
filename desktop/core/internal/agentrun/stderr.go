package agentrun

import (
	"strings"
	"unicode/utf8"
)

const (
	stderrReplayBytes = 256 << 10
	stderrKeepBytes   = 1 << 20
)

type subscribeOptions struct {
	after       int64
	stderr      bool
	stderrAfter int64
}

func completeUTF8(buf []byte) int {
	n := len(buf)
	for back := 1; back <= utf8.UTFMax && back <= n; back++ {
		c := buf[n-back]
		if c&0xC0 == 0x80 {
			continue
		}
		if c < 0x80 || utf8.FullRune(buf[n-back:]) {
			return n
		}
		return n - back
	}
	return n
}

func stderrText(raw []byte, credentials []string) string {
	return RedactText(strings.ToValidUTF8(string(raw), "�"), credentials)
}

func (r *run) Stderr(data []byte, offset int64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if offset > r.errNext {
		r.errCarry = nil
		r.errSent = offset
	} else if offset < r.errNext {
		skip := r.errNext - offset
		if skip >= int64(len(data)) {
			return
		}
		data = data[skip:]
		offset = r.errNext
	}
	r.errNext = offset + int64(len(data))
	buffer := append(r.errCarry, data...)
	cut := completeUTF8(buffer)
	if len(buffer)-cut > utf8.UTFMax {
		cut = len(buffer)
	}
	r.errCarry = append([]byte(nil), buffer[cut:]...)
	if cut == 0 {
		return
	}
	r.publishStderrLocked(buffer[:cut], r.errNext-int64(len(r.errCarry)))
}

func (r *run) flushStderrLocked() {
	if len(r.errCarry) == 0 {
		return
	}
	carry := r.errCarry
	r.errCarry = nil
	r.publishStderrLocked(carry, r.errNext)
}

func (r *run) publishStderrLocked(raw []byte, end int64) {
	r.errSent = end
	payload := map[string]any{"key": r.key, "stderr": stderrText(raw, r.redactionValues()), "eo": end}
	for id, sub := range r.subscribers {
		if !sub.stderr {
			continue
		}
		if !sub.peer.Send("core.agentrun.event", payload) {
			delete(r.subscribers, id)
			close(sub.stop)
		}
	}
	if end-r.errAcked >= 2*stderrKeepBytes && r.m.procs != nil {
		target := end - stderrKeepBytes
		r.errAcked = target
		procs, key := r.m.procs, r.key
		go procs.AckStderr(key, target)
	}
}

func (r *run) replayStderrLocked(after int64) (map[string]any, bool) {
	from := max(after, r.errSent-stderrReplayBytes, 0)
	if r.m.procs == nil || from >= r.errSent {
		return nil, false
	}
	raw, start := r.m.procs.ReadStderr(r.key, from, r.errSent)
	for len(raw) > 0 && raw[0]&0xC0 == 0x80 {
		raw = raw[1:]
		start++
	}
	if len(raw) == 0 {
		return nil, false
	}
	return map[string]any{"key": r.key, "stderr": stderrText(raw, r.redactionValues()), "eo": start + int64(len(raw)),
		"replay": true}, true
}
