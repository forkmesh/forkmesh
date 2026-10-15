package vault

import (
	"encoding/json"
	"os"
	"strings"
	"sync/atomic"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
)

const (
	VariablesSetting = "actions/variables"
	ActionsNamespace = "actions"
	Marker           = "[stored in the ForkMesh vault]"
)

func EntryName(namespace, name string) string {
	return namespace + "/" + escapeName(name)
}

func escapeName(name string) string {
	const hexDigits = "0123456789ABCDEF"
	var b strings.Builder
	for i := 0; i < len(name); i++ {
		c := name[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' {
			b.WriteByte(c)
			continue
		}
		b.WriteByte('%')
		b.WriteByte(hexDigits[c>>4])
		b.WriteByte(hexDigits[c&15])
	}
	return b.String()
}

func unescapeName(text string) (string, bool) {
	out := make([]byte, 0, len(text))
	for i := 0; i < len(text); i++ {
		if text[i] != '%' {
			out = append(out, text[i])
			continue
		}
		if i+2 >= len(text) || !isHex(text[i+1]) || !isHex(text[i+2]) {
			return "", false
		}
		out = append(out, unhex(text[i+1])<<4|unhex(text[i+2]))
		i += 2
	}
	return string(out), true
}

func isHex(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func unhex(c byte) byte {
	switch {
	case c >= '0' && c <= '9':
		return c - '0'
	case c >= 'a' && c <= 'f':
		return c - 'a' + 10
	}
	return c - 'A' + 10
}

func NameOf(namespace, entry string) (string, bool) {
	prefix := namespace + "/"
	if !strings.HasPrefix(entry, prefix) {
		return "", false
	}
	return unescapeName(entry[len(prefix):])
}

func ValidNamespace(namespace string) bool {
	if namespace == "" || len(namespace) > 32 {
		return false
	}
	for i := 0; i < len(namespace); i++ {
		c := namespace[i]
		if !((c >= 'a' && c <= 'z') || (i > 0 && ((c >= '0' && c <= '9') || c == '-'))) {
			return false
		}
	}
	return true
}

func IsMarker(value string) bool { return value == Marker }

func PlaintextVariables(settings *qsettings.Settings) map[string]string {
	out := map[string]string{}
	if settings == nil {
		return out
	}
	value, ok := settings.Value(VariablesSetting)
	if !ok {
		return out
	}
	raw := value.Bytes
	if value.Kind != "bytes" {
		raw = []byte(value.String())
	}
	var object map[string]any
	if json.Unmarshal(raw, &object) != nil {
		return out
	}
	for key, item := range object {
		text, _ := item.(string)
		out[key] = text
	}
	return out
}

func PendingPlaintext(settings *qsettings.Settings) map[string]string {
	out := map[string]string{}
	for name, value := range PlaintextVariables(settings) {
		if !IsMarker(value) {
			out[name] = value
		}
	}
	return out
}

func settingsTime(path string) int64 {
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.ModTime().UnixMilli()
}

func Variables(core *daemon.Core) map[string]string {
	if core == nil {
		return map[string]string{}
	}
	if module, ok := core.Module("vault").(*Module); ok && module != nil && module.store != nil {
		values, _ := module.ActionVariables()
		return values
	}
	if core.Settings == nil {
		return map[string]string{}
	}
	return PendingPlaintext(core.Settings.Get())
}

func Variable(core *daemon.Core, name string) string {
	return Variables(core)[name]
}

var active atomic.Pointer[Module]

func ActionVariablesOr(settings *qsettings.Settings) map[string]string {
	if module := active.Load(); module != nil && module.store != nil {
		values, _ := module.ActionVariables()
		return values
	}
	return PendingPlaintext(settings)
}
