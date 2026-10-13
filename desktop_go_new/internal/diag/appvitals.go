package diag

import (
	"path/filepath"
	"time"
)

const maxDetailDisks = 8

type appMeter struct {
	lastSecs float64
	lastAt   time.Time
	primed   bool
	percent  float64
}

func (a *appMeter) update(secs float64, ok bool, now time.Time) float64 {
	if !ok {
		return -1
	}
	if !a.primed {
		a.primed = true
		a.lastSecs = secs
		a.lastAt = now
		a.percent = -1
		return -1
	}
	elapsed := now.Sub(a.lastAt).Seconds()
	if elapsed <= 0 {
		return a.percent
	}
	pct := (secs - a.lastSecs) / elapsed * 100.0
	if pct < 0 {
		pct = 0
	}
	a.lastSecs = secs
	a.lastAt = now
	a.percent = pct
	return pct
}

type watchOptions struct {
	detail bool
	pid    int
	disks  []string
}

func detailDisks(raw any) []string {
	items, _ := raw.([]any)
	var out []string
	seen := map[string]bool{}
	for _, item := range items {
		path, _ := item.(string)
		if path == "" || !filepath.IsAbs(path) || seen[path] || len(out) >= maxDetailDisks {
			continue
		}
		seen[path] = true
		out = append(out, path)
	}
	return out
}

func (m *Module) appCPU(pid int, now time.Time) float64 {
	readCPU := m.readCPU
	if readCPU == nil {
		readCPU = processCPUSeconds
	}
	m.appMu.Lock()
	defer m.appMu.Unlock()
	secs, ok := readCPU(pid)
	if m.appMeters == nil {
		m.appMeters = map[int]*appMeter{}
	}
	meter := m.appMeters[pid]
	if meter == nil {
		meter = &appMeter{}
		m.appMeters[pid] = meter
	}
	return meter.update(secs, ok, now)
}

func (m *Module) detailSample(r reading, s sample, opts watchOptions, now time.Time) map[string]any {
	disk := m.readDisk
	if disk == nil {
		disk = diskSpace
	}
	disks := make([]any, 0, len(opts.disks))
	for _, path := range opts.disks {
		total, free := disk(path)
		disks = append(disks, map[string]any{"path": path, "total": total, "free": free})
	}
	app := map[string]any{"pid": opts.pid, "cpuPercent": -1.0, "resident": int64(-1), "swapped": int64(-1),
		"threads": -1, "openFiles": -1, "openFileLimit": -1}
	if opts.pid > 0 {
		proc := m.readProc(opts.pid)
		app["cpuPercent"] = m.appCPU(opts.pid, now)
		app["resident"] = proc.Resident
		app["swapped"] = proc.Swapped
		app["threads"] = proc.Threads
		app["openFiles"] = proc.OpenFiles
		app["openFileLimit"] = proc.OpenFileLimit
	}
	return map[string]any{
		"memory":          s.Memory,
		"cpu":             s.CPU,
		"disk":            s.Disk,
		"swap":            s.Swap,
		"memoryTotal":     r.MemoryTotal,
		"memoryAvailable": r.MemoryAvailable,
		"swapTotal":       r.SwapTotal,
		"swapFree":        r.SwapFree,
		"diskTotal":       r.DiskTotal,
		"diskFree":        r.DiskFree,
		"cpuCount":        r.CPUCount,
		"load1":           r.Pressure.Load1,
		"disks":           disks,
		"app":             app,
		"sampledAtMs":     now.UnixMilli(),
	}
}
