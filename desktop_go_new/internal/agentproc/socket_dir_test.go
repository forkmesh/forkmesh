//go:build !windows

package agentproc

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestLongSocketPathRefusesASharedFallbackDirectory(t *testing.T) {
	base := t.TempDir()
	t.Setenv("TMPDIR", base)
	planted := filepath.Join(base, "fmap-"+strconv.Itoa(os.Getuid()))
	if err := os.Mkdir(planted, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(planted, 0o777); err != nil {
		t.Fatal(err)
	}
	long := filepath.Join(base, strings.Repeat("d", 120))
	socket := (&Module{}).socketPath(long)
	dir := filepath.Dir(socket)
	if dir == planted {
		t.Fatalf("the socket went into a world-writable directory: %s", socket)
	}
	if !privateDir(dir) || !strings.HasPrefix(dir, base+string(os.PathSeparator)) {
		t.Fatalf("the fallback directory is not private: %s", dir)
	}

	if err := os.Chmod(planted, 0o700); err != nil {
		t.Fatal(err)
	}
	if dir := filepath.Dir((&Module{}).socketPath(long)); dir != planted {
		t.Fatalf("a private fallback directory is reused: %s", dir)
	}
}
