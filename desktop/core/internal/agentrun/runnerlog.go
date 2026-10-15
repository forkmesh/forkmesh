package agentrun

import (
	"strings"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/agentproc"
)

const RuntimeLog = "log"

func runSecrets(meta map[string]any) []string {
	raw, _ := meta[agentproc.SecretsKey].([]any)
	out := []string{}
	for _, value := range raw {
		if text, ok := value.(string); ok && strings.TrimSpace(text) != "" {
			out = append(out, text, strings.TrimSpace(text))
		}
	}
	return out
}

func (r *run) redactionValues() []string {
	values := r.m.credentialValues()
	if len(r.secrets) == 0 {
		return values
	}
	return append(append([]string{}, r.secrets...), values...)
}

func redactRunnerText(text string, values []string) string {
	for _, secret := range values {
		if secret != "" && strings.Contains(text, secret) {
			text = strings.ReplaceAll(text, secret, "***")
		}
	}
	return RedactText(text, values)
}

func (r *run) logLocked(data []byte, offset int64) {
	buffer := append(r.logCarry, data...)
	cut := completeUTF8(buffer)
	if len(buffer)-cut > utf8.UTFMax {
		cut = len(buffer)
	}
	r.logCarry = append([]byte(nil), buffer[cut:]...)
	if cut == 0 {
		return
	}
	r.publishLogLocked(buffer[:cut], offset+int64(len(data))-int64(len(r.logCarry)))
}

func (r *run) flushLogLocked() {
	if len(r.logCarry) == 0 {
		return
	}
	carry := r.logCarry
	r.logCarry = nil
	r.publishLogLocked(carry, r.next)
}

func (r *run) publishLogLocked(raw []byte, end int64) {
	text := redactRunnerText(strings.ToValidUTF8(string(raw), "�"), r.redactionValues())
	if writer := r.m.writer(); writer != nil && r.dir != "" {
		writer.AppendLog(r.dir, r.sid, text)
	}
	r.delivered = max(r.delivered, end)
	r.sendLocked(map[string]any{"key": r.key, "text": text, "so": end})
}
