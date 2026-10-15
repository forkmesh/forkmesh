package qvariant

import (
	"strings"
	"testing"
)

const realUsageCounts = `@Variant(\0\0\0\b\0\0\0\x3\0\0\0l\0s\0h\0o\0r\0t\0\x63\0u\0t\0:\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0/\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0:\0\x64\0\x65\0p\0l\0o\0y\0-\0n\0\x61\0t\0i\0v\0\x65\0-\0i\0o\0s\0-\0\x61\0\x64\0-\0h\0o\0\x63\0.\0s\0h\0\0\0\x2\0\0\0\x14\0\0\0h\0s\0h\0o\0r\0t\0\x63\0u\0t\0:\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0/\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0:\0l\0\x61\0u\0n\0\x63\0h\0-\0\x66\0l\0u\0t\0t\0\x65\0r\0-\0\x61\0n\0\x64\0r\0o\0i\0\x64\0.\0s\0h\0\0\0\x2\0\0\0\x5\0\0\0\x66\0s\0h\0o\0r\0t\0\x63\0u\0t\0:\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0/\0\x66\0o\0r\0k\0m\0\x65\0s\0h\0:\0l\0\x61\0u\0n\0\x63\0h\0-\0n\0\x61\0t\0i\0v\0\x65\0-\0\x61\0n\0\x64\0r\0o\0i\0\x64\0.\0s\0h\0\0\0\x2\0\0\0\x1)`

func TestDecodesARealQSettingsValue(t *testing.T) {
	value, err := FromSettingsRaw(realUsageCounts)
	if err != nil {
		t.Fatal(err)
	}
	counts := value.(map[string]any)
	if len(counts) != 3 || Int64(counts["shortcut:forkmesh/forkmesh:deploy-native-ios-ad-hoc.sh"]) != 20 ||
		Int64(counts["shortcut:forkmesh/forkmesh:launch-native-android.sh"]) != 1 {
		t.Fatalf("decoded %v", counts)
	}
}

const quotedStats = `"@Variant(\0\0\0\b\0\0\0\x3\0\0\0 \0\x61\0p\0p\0:\0\x66\0i\0x\0-\0t\0\x65\0s\0t\0s\0.\0m\0\x64\0\0\0\b\0\0\0\b\0\0\0\x10\0\x66\0i\0n\0i\0s\0h\0\x65\0\x64\0\0\0\x2\0\0\0\x3\0\0\0\f\0l\0\x61\0s\0t\0M\0s\0\0\0\x4\xff\xff\xff\xff\xff\xff\xff\xff\0\0\0\f\0l\0\x61\0s\0t\0O\0k\0\0\0\x1\x1\0\0\0\x14\0l\0\x61\0s\0t\0O\0u\0t\0p\0u\0t\0\0\0\n\0\0\0L\0\x64\0o\0n\0\x65\0,\0 \0o\0k\0;\0 \0\x63\0\x61\0\x66\0\xe9\0 \0=\0 \0\x31\0\x30\0\x30\0%\0 \0\x1\0 \0\"\0q\0u\0o\0t\0\x65\0\x64\0\"\0 \0\\\0 \0\x65\0n\0\x64\0\0\0\x16\0l\0\x61\0s\0t\0R\0u\0n\0\x41\0t\0M\0s\0\0\0\x4\0\0\x1\xa0\xfe\xdf\xd9t\0\0\0\x12\0s\0u\0\x63\0\x63\0\x65\0\x65\0\x64\0\x65\0\x64\0\0\0\x2\0\0\0\x2\0\0\0\n\0t\0i\0m\0\x65\0\x64\0\0\0\x2\0\0\0\x3\0\0\0\xe\0t\0o\0t\0\x61\0l\0M\0s\0\0\0\x4\0\0\0\0\0\xd7\xd7\xdc\0\0\0\b\0l\0i\0s\0t\0\0\0\v\0\0\0\x2\0\0\0\x2\0\x61\0\0\0\x6\0\xfc\0n\0\xef\0\0\0\x4\0p\0i\0\0\0\x6@\n\0\0\0\0\0\0)"`

func TestDecodesEveryTypeTheDesktopStores(t *testing.T) {
	raw := quotedStats
	if !strings.HasPrefix(raw, "\"@Variant(") {
		t.Fatalf("expected the quoted form for a value with commas: %.40s", raw)
	}
	value, err := FromSettingsRaw(raw)
	if err != nil {
		t.Fatal(err)
	}
	got := value.(map[string]any)
	row := got["app:fix-tests.md"].(map[string]any)
	if Int64(row["lastRunAtMs"]) != 1790982478196 || Int64(row["finished"]) != 3 || Int64(row["lastMs"]) != -1 ||
		!Bool(row["lastOk"]) || String(row["lastOutput"]) != "done, ok; café = 100% \x01 \"quoted\" \\ end" {
		t.Fatalf("row %v", row)
	}
	if list := got["list"].([]string); len(list) != 2 || list[1] != "ünï" {
		t.Fatalf("list %v", got["list"])
	}
	if got["pi"].(float64) != 3.25 {
		t.Fatalf("pi %v", got["pi"])
	}
}

func TestRejectsGarbage(t *testing.T) {
	for _, raw := range []string{"", "plain", "@Variant(\\0\\0)", "@Variant(\\0\\0\\0\\b\\0\\0\\0\\x5)", "@ByteArray(x)"} {
		if _, err := FromSettingsRaw(raw); err == nil {
			t.Errorf("%q decoded", raw)
		}
	}
	if Int64("12") != 12 || Int64("x") != 0 || Bool("false") || !Bool("true") {
		t.Fatal("conversions")
	}
}
