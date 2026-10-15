package diag

import (
	"bytes"
	"math"
	"strconv"
	"strings"
)

const vitalsSeconds = 30

type sample struct {
	Memory float64 `json:"memory"`
	CPU    float64 `json:"cpu"`
	Disk   float64 `json:"disk"`
	Swap   float64 `json:"swap"`
}

type history struct {
	Memory []float64 `json:"memory"`
	CPU    []float64 `json:"cpu"`
	Disk   []float64 `json:"disk"`
	Swap   []float64 `json:"swap"`
}

func historyOf(samples []sample) history {
	out := history{Memory: []float64{}, CPU: []float64{}, Disk: []float64{}, Swap: []float64{}}
	for _, s := range samples {
		out.Memory = append(out.Memory, s.Memory)
		out.CPU = append(out.CPU, s.CPU)
		out.Disk = append(out.Disk, s.Disk)
		out.Swap = append(out.Swap, s.Swap)
	}
	return out
}

func appendVitals(samples []sample, s sample) []sample {
	samples = append(samples, s)
	if len(samples) > vitalsSeconds {
		samples = append([]sample(nil), samples[len(samples)-vitalsSeconds:]...)
	}
	return samples
}

func usedPercent(total, free int64) float64 {
	if total <= 0 || free < 0 || free > total {
		return -1
	}
	return 100.0 * float64(total-free) / float64(total)
}

type pressure struct {
	Valid        bool
	MemorySome   float64
	MemoryFull   float64
	MemoryFull60 float64
	IOSome       float64
	IOFull       float64
	CPUSome      float64
	Load1        float64
}

type reading struct {
	MemoryTotal     int64
	MemoryAvailable int64
	SwapTotal       int64
	SwapFree        int64
	DiskTotal       int64
	DiskFree        int64
	CPUBusy         float64
	CPUTotal        float64
	CPUValid        bool
	CPUCount        int
	Pressure        pressure
}

type processStats struct {
	Resident      int64
	Swapped       int64
	OpenFiles     int
	OpenFileLimit int
	Threads       int
}

func unknownProcess() processStats {
	return processStats{Resident: -1, Swapped: -1, OpenFiles: -1, OpenFileLimit: -1, Threads: -1}
}

type cpuMeter struct {
	lastBusy    float64
	lastTotal   float64
	lastPercent float64
	primed      bool
}

func (c *cpuMeter) update(r reading) float64 {
	if !r.CPUValid || r.CPUTotal <= 0 {
		return -1
	}
	busy := r.CPUBusy
	if !c.primed {
		c.primed = true
		c.lastBusy = busy
		c.lastTotal = r.CPUTotal
		c.lastPercent = 0
		return 0
	}
	dTotal := r.CPUTotal - c.lastTotal
	dBusy := busy - c.lastBusy
	c.lastBusy = busy
	c.lastTotal = r.CPUTotal
	if dTotal > 0 {
		pct := dBusy / dTotal * 100.0
		if pct < 0 {
			pct = 0
		}
		if pct > 100 {
			pct = 100
		}
		c.lastPercent = pct
	}
	return c.lastPercent
}

func sampleOf(r reading, cpu float64) sample {
	return sample{
		Memory: usedPercent(r.MemoryTotal, r.MemoryAvailable),
		CPU:    cpu,
		Disk:   usedPercent(r.DiskTotal, r.DiskFree),
		Swap:   usedPercent(r.SwapTotal, r.SwapFree),
	}
}

func fields(line []byte) [][]byte {
	return bytes.Fields(line)
}

func parseCPULine(stat []byte) (busy, total float64, ok bool) {
	first := stat
	if i := bytes.IndexByte(stat, '\n'); i >= 0 {
		first = stat[:i]
	}
	if !bytes.HasPrefix(first, []byte("cpu ")) {
		return 0, 0, false
	}
	parts := fields(first)
	idle := 0.0
	for i := 1; i < len(parts); i++ {
		v, err := strconv.ParseInt(string(parts[i]), 10, 64)
		if err != nil {
			continue
		}
		total += float64(v)
		if i == 4 || i == 5 {
			idle += float64(v)
		}
	}
	if total <= 0 {
		return 0, 0, false
	}
	return total - idle, total, true
}

func meminfoField(text []byte, key string) (int64, bool) {
	for _, line := range bytes.Split(text, []byte{'\n'}) {
		if !bytes.HasPrefix(line, []byte(key+":")) {
			continue
		}
		parts := fields(line)
		if len(parts) < 2 {
			continue
		}
		kib, err := strconv.ParseUint(string(parts[1]), 10, 63)
		if err != nil {
			continue
		}
		return int64(kib) * 1024, true
	}
	return 0, false
}

func kilobyteField(text []byte, key string) int64 {
	for _, line := range bytes.Split(text, []byte{'\n'}) {
		if !bytes.HasPrefix(line, []byte(key+":")) {
			continue
		}
		parts := strings.Split(strings.TrimSpace(string(line[len(key)+1:])), " ")
		kb, err := strconv.ParseInt(parts[0], 10, 64)
		if err != nil {
			return -1
		}
		return kb * 1024
	}
	return -1
}

func avgField(line []byte, key string) (float64, bool) {
	at := bytes.Index(line, []byte(key+"="))
	if at < 0 {
		return 0, false
	}
	begin := at + len(key) + 1
	end := begin
	for end < len(line) && line[end] != ' ' {
		end++
	}
	value, err := strconv.ParseFloat(string(line[begin:end]), 64)
	if err != nil {
		return 0, false
	}
	return value, true
}

func parsePressureStall(text []byte, some, full, full60 *float64) bool {
	sawSome := false
	for _, raw := range bytes.Split(text, []byte{'\n'}) {
		line := bytes.TrimSpace(raw)
		if bytes.HasPrefix(line, []byte("some ")) {
			if value, ok := avgField(line, "avg10"); ok {
				if some != nil {
					*some = value
				}
				sawSome = true
			}
		} else if bytes.HasPrefix(line, []byte("full ")) {
			if value, ok := avgField(line, "avg10"); ok && full != nil {
				*full = value
			}
			if value, ok := avgField(line, "avg60"); ok && full60 != nil {
				*full60 = value
			}
		}
	}
	return sawSome
}

func parseLoadAverage(text []byte) (float64, bool) {
	parts := strings.Split(strings.TrimSpace(string(text)), " ")
	value, err := strconv.ParseFloat(parts[0], 64)
	if err != nil {
		return 0, false
	}
	return value, true
}

func parseOpenFileLimit(limits []byte) int {
	for _, line := range bytes.Split(limits, []byte{'\n'}) {
		if !bytes.HasPrefix(line, []byte("Max open files")) {
			continue
		}
		parts := fields(line[len("Max open files"):])
		if len(parts) == 0 {
			return -1
		}
		if string(parts[0]) == "unlimited" {
			return 0
		}
		n, err := strconv.ParseInt(string(parts[0]), 10, 64)
		if err != nil {
			return -1
		}
		if n > 1<<31-1 {
			n = 1<<31 - 1
		}
		return int(n)
	}
	return -1
}

func healthLevel(r reading, proc processStats) string {
	p := r.Pressure
	memoryStarved := p.Valid && p.MemoryFull >= 10.0
	ioStarved := p.Valid && p.IOFull >= 30.0
	availableShare := 1.0
	if r.MemoryTotal > 0 && r.MemoryAvailable >= 0 {
		availableShare = float64(r.MemoryAvailable) / float64(r.MemoryTotal)
	}
	diskShare := 1.0
	if r.DiskTotal > 0 && r.DiskFree >= 0 {
		diskShare = float64(r.DiskFree) / float64(r.DiskTotal)
	}
	if memoryStarved || ioStarved || availableShare < 0.05 || diskShare < 0.03 {
		return "strained"
	}
	loaded := r.CPUCount > 0 && p.Load1 > float64(r.CPUCount)*1.5
	if loaded || availableShare < 0.15 || diskShare < 0.10 || (p.Valid && p.MemorySome >= 10.0) ||
		(proc.OpenFileLimit > 0 && float64(proc.OpenFiles) > float64(proc.OpenFileLimit)*0.8) {
		return "busy"
	}
	return "ok"
}

func formatBytes(n int64) string {
	if n <= 0 {
		return "0 B"
	}
	kib := float64(n) / 1024.0
	if kib < 1024.0 {
		return fixed(kib, 0) + " KB"
	}
	mib := kib / 1024.0
	if mib < 1024.0 {
		return fixed(mib, 1) + " MB"
	}
	return fixed(mib/1024.0, 2) + " GB"
}

func percentText(v float64) string {
	return fixed(v, 1) + "%"
}

func describePressure(r reading, proc processStats) string {
	p := r.Pressure
	var parts []string
	if p.Valid {
		parts = append(parts, "memory PSI some "+percentText(p.MemorySome)+" / full "+percentText(p.MemoryFull)+
			" (avg10), io full "+percentText(p.IOFull))
	}
	if p.Load1 >= 0 {
		load := "load " + fixed(p.Load1, 1)
		if r.CPUCount > 0 {
			load += " on " + strconv.Itoa(r.CPUCount) + " cpus"
		}
		parts = append(parts, load)
	}
	if r.MemoryAvailable >= 0 && r.MemoryTotal > 0 {
		parts = append(parts, formatBytes(r.MemoryAvailable)+" of "+formatBytes(r.MemoryTotal)+" available")
	}
	if r.SwapTotal > 0 && r.SwapFree >= 0 {
		parts = append(parts, "swap "+formatBytes(r.SwapTotal-r.SwapFree)+" used of "+formatBytes(r.SwapTotal))
	}
	line := strings.Join(parts, ", ")
	if proc.Resident >= 0 {
		self := "this process " + formatBytes(proc.Resident) + " resident"
		if proc.Swapped >= 0 {
			self += ", " + formatBytes(proc.Swapped) + " swapped"
		}
		if line != "" {
			line += "; "
		}
		line += self
	}
	return line
}

func systemSummary(r reading, cpu float64, proc processStats) map[string]any {
	p := r.Pressure
	return map[string]any{
		"level":      healthLevel(r, proc),
		"cpuPercent": cpu,
		"cpuCount":   r.CPUCount,
		"load1":      p.Load1,
		"memory": map[string]any{
			"total":       r.MemoryTotal,
			"available":   r.MemoryAvailable,
			"swapTotal":   r.SwapTotal,
			"swapFree":    r.SwapFree,
			"appResident": proc.Resident,
		},
		"pressure": map[string]any{
			"valid":      p.Valid,
			"memorySome": p.MemorySome,
			"memoryFull": p.MemoryFull,
			"ioFull":     p.IOFull,
			"cpuSome":    p.CPUSome,
		},
		"disk": map[string]any{"total": r.DiskTotal, "free": r.DiskFree},
		"process": map[string]any{
			"openFiles":     proc.OpenFiles,
			"openFileLimit": proc.OpenFileLimit,
			"threads":       proc.Threads,
		},
		"pressureLine": describePressure(r, proc),
	}
}

func fixed(v float64, prec int) string {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return strconv.FormatFloat(v, 'f', prec, 64)
	}
	negative := v < 0
	digits := strconv.FormatFloat(math.Abs(v), 'f', prec+25, 64)
	point := strings.IndexByte(digits, '.')
	whole := []byte(digits[:point] + digits[point+1:point+1+prec])
	if digits[point+1+prec] >= '5' {
		i := len(whole) - 1
		for ; i >= 0; i-- {
			if whole[i] == '9' {
				whole[i] = '0'
				continue
			}
			whole[i]++
			break
		}
		if i < 0 {
			whole = append([]byte{'1'}, whole...)
		}
	}
	intLen := len(whole) - prec
	out := string(whole[:intLen])
	if prec > 0 {
		out += "." + string(whole[intLen:])
	}
	if negative && strings.Trim(out, "0.") != "" {
		out = "-" + out
	}
	return out
}
