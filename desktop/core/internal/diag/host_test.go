package diag

import (
	"os"
	"runtime"
	"testing"
)

func TestReadHostLive(t *testing.T) {
	r := readHost(t.TempDir())
	if r.CPUCount <= 0 {
		t.Fatal("cpu count")
	}
	if runtime.GOOS != "linux" {
		return
	}
	if r.MemoryTotal <= 0 || r.MemoryAvailable <= 0 || r.MemoryAvailable > r.MemoryTotal || !r.CPUValid || r.DiskTotal <= 0 {
		t.Fatalf("reading = %+v", r)
	}
	proc := readProcess(os.Getpid())
	if proc.Resident <= 0 || proc.Threads <= 0 || proc.OpenFiles <= 0 {
		t.Fatalf("process = %+v", proc)
	}
	if readProcess(-1).Resident != -1 {
		t.Fatal("bad pid")
	}
	t.Logf("%s", describePressure(r, proc))
}
