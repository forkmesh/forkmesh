package diag

import (
	"math"
	"testing"
)

const meminfoFixture = `MemTotal:       16303428 kB
MemFree:         1022380 kB
MemAvailable:    8151714 kB
Buffers:          123456 kB
SwapTotal:       2097148 kB
SwapFree:        1048574 kB
`

func TestParsers(t *testing.T) {
	if v, ok := meminfoField([]byte(meminfoFixture), "MemAvailable"); !ok || v != 8151714*1024 {
		t.Fatalf("MemAvailable = %d", v)
	}
	if _, ok := meminfoField([]byte(meminfoFixture), "Nope"); ok {
		t.Fatal("missing key must not parse")
	}
	if kilobyteField([]byte("Name:\tx\nVmRSS:\t  2048 kB\nVmSwap:\t0 kB\n"), "VmRSS") != 2048*1024 {
		t.Fatal("VmRSS")
	}
	if kilobyteField([]byte("Name:\tx\n"), "VmSwap") != -1 {
		t.Fatal("absent VmSwap must be -1")
	}
	busy, total, ok := parseCPULine([]byte("cpu  100 20 30 800 50 0 0 0 0 0\ncpu0 1 2 3 4\n"))
	if !ok || total != 1000 || busy != 150 {
		t.Fatalf("cpu busy %v total %v", busy, total)
	}
	if _, _, ok := parseCPULine([]byte("intr 1 2 3\n")); ok {
		t.Fatal("no cpu line")
	}
	var some, full, full60 float64
	psi := []byte("some avg10=12.50 avg60=3.00 avg300=1.00 total=1\nfull avg10=4.25 avg60=2.75 avg300=0.10 total=2\n")
	if !parsePressureStall(psi, &some, &full, &full60) || some != 12.5 || full != 4.25 || full60 != 2.75 {
		t.Fatalf("psi %v %v %v", some, full, full60)
	}
	if parsePressureStall([]byte(""), &some, nil, nil) {
		t.Fatal("empty psi is invalid")
	}
	if load, ok := parseLoadAverage([]byte("1.25 0.80 0.50 2/345 6789\n")); !ok || load != 1.25 {
		t.Fatalf("load %v", load)
	}
	limits := []byte("Limit                     Soft Limit           Hard Limit           Units\nMax open files            1024                 524288               files\n")
	if parseOpenFileLimit(limits) != 1024 {
		t.Fatal("open file limit")
	}
	if parseOpenFileLimit([]byte("Max open files            unlimited            unlimited            files\n")) != 0 {
		t.Fatal("unlimited reads as 0")
	}
}

func TestCPUMeter(t *testing.T) {
	var meter cpuMeter
	if got := meter.update(reading{}); got != -1 {
		t.Fatalf("unreadable cpu = %v", got)
	}
	if got := meter.update(reading{CPUValid: true, CPUBusy: 100, CPUTotal: 1000}); got != 0 {
		t.Fatalf("baseline = %v", got)
	}
	if got := meter.update(reading{CPUValid: true, CPUBusy: 150, CPUTotal: 1200}); got != 25 {
		t.Fatalf("delta = %v", got)
	}
	if got := meter.update(reading{CPUValid: true, CPUBusy: 150, CPUTotal: 1200}); got != 25 {
		t.Fatalf("no progress keeps the last value, got %v", got)
	}
	if got := meter.update(reading{CPUValid: true, CPUBusy: 500, CPUTotal: 1300}); got != 100 {
		t.Fatalf("clamped = %v", got)
	}
}

func TestVitalsSamples(t *testing.T) {
	if usedPercent(0, 0) != -1 || usedPercent(100, -1) != -1 || usedPercent(100, 200) != -1 || usedPercent(200, 50) != 75 {
		t.Fatal("usedPercent")
	}
	s := sampleOf(reading{MemoryTotal: 100, MemoryAvailable: 40, DiskTotal: 10, DiskFree: 1}, 12)
	if s.Memory != 60 || s.Disk != 90 || s.Swap != -1 || s.CPU != 12 {
		t.Fatalf("sample = %+v", s)
	}
	var samples []sample
	for i := 0; i < 45; i++ {
		samples = appendVitals(samples, sample{CPU: float64(i)})
	}
	h := historyOf(samples)
	if len(h.CPU) != vitalsSeconds || h.CPU[0] != 15 || h.CPU[29] != 44 {
		t.Fatalf("history = %v", h.CPU)
	}
	empty := historyOf(nil)
	if empty.Memory == nil || len(empty.Swap) != 0 {
		t.Fatal("empty history must encode as arrays")
	}
}

func TestHealthLevel(t *testing.T) {
	healthy := reading{MemoryTotal: 100, MemoryAvailable: 50, DiskTotal: 100, DiskFree: 50, CPUCount: 4, Pressure: pressure{Load1: 1}}
	none := unknownProcess()
	if healthLevel(healthy, none) != "ok" {
		t.Fatal("ok")
	}
	busy := healthy
	busy.Pressure.Load1 = 7
	if healthLevel(busy, none) != "busy" {
		t.Fatal("load busy")
	}
	if healthLevel(healthy, processStats{OpenFiles: 900, OpenFileLimit: 1000}) != "busy" {
		t.Fatal("fd busy")
	}
	strained := healthy
	strained.Pressure = pressure{Valid: true, MemoryFull: 12, Load1: 1}
	if healthLevel(strained, none) != "strained" {
		t.Fatal("psi strained")
	}
	low := healthy
	low.DiskFree = 2
	if healthLevel(low, none) != "strained" {
		t.Fatal("disk strained")
	}
}

func TestDescribePressure(t *testing.T) {
	r := reading{CPUCount: 8, MemoryTotal: 16 << 30, MemoryAvailable: 4 << 30, SwapTotal: 2 << 30, SwapFree: 1 << 30,
		Pressure: pressure{Valid: true, MemorySome: 1.25, MemoryFull: 0, IOFull: 3.06, Load1: 2.345}}
	want := "memory PSI some 1.3% / full 0.0% (avg10), io full 3.1%, load 2.3 on 8 cpus, 4.00 GB of 16.00 GB available, swap 1.00 GB used of 2.00 GB"
	if got := describePressure(r, unknownProcess()); got != want {
		t.Fatalf("got  %q\nwant %q", got, want)
	}
	proc := processStats{Resident: 300 << 20, Swapped: 0}
	if got := describePressure(reading{Pressure: pressure{Load1: -1}}, proc); got != "this process 300.0 MB resident, 0 B swapped" {
		t.Fatalf("got %q", got)
	}
	if formatBytes(1500) != "1 KB" || formatBytes(0) != "0 B" || formatBytes(2560) != "3 KB" {
		t.Fatal("formatBytes")
	}
	for value, want := range map[float64]string{0.25: "0.3", 9.95: "9.9", 9.96: "10.0", 99.95: "100.0", -1.25: "-1.3", 0: "0.0"} {
		if got := fixed(value, 1); got != want {
			t.Errorf("fixed(%v) = %q, want %q", value, got, want)
		}
	}
	summary := systemSummary(r, 33, unknownProcess())
	if summary["level"] != "ok" || summary["cpuPercent"] != 33.0 || summary["load1"] != 2.345 {
		t.Fatalf("summary = %v", summary)
	}
	if math.IsNaN(summary["cpuPercent"].(float64)) {
		t.Fatal("nan")
	}
}

func TestBuildMacStatus(t *testing.T) {
	off := buildMacStatus(false, nil, 5000)
	if off["state"] != "offline" || off["configured"] != true || off["checkedAtMs"] != int64(5000) {
		t.Fatalf("offline = %v", off)
	}
	if idle := buildMacStatus(true, []byte(""), 1); idle["state"] != "idle" {
		t.Fatalf("idle = %v", idle)
	}
	if stale := buildMacStatus(true, []byte("14400\nmacos-desktop\n"), 1); stale["state"] != "idle" {
		t.Fatalf("stale = %v", stale)
	}
	busy := buildMacStatus(true, []byte(" 90 \nmacos-desktop #12; rm -rf $HOME\n"), 1_000_000)
	if busy["state"] != "building" || busy["job"] != "macos-desktop 12 rm -rf HOME" || busy["sinceMs"] != int64(910_000) {
		t.Fatalf("busy = %v", busy)
	}
}
