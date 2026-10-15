package netlog

import (
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	RetentionDays    = 30
	RetentionMs      = int64(RetentionDays) * 24 * 60 * 60 * 1000
	BucketMs         = int64(15 * 60 * 1000)
	RebuildScanBytes = int64(64 * 1024 * 1024)
	segmentPrefix    = "netlog-"
	segmentSuffix    = ".txt"
	storedFormat     = "2006-01-02 15:04:05"
	nameFormat       = "20060102150405"
)

type Digest map[int64]map[string]int

func (d Digest) add(bucket int64, badge string, count int) {
	row := d[bucket]
	if row == nil {
		row = map[string]int{}
		d[bucket] = row
	}
	row[badge] += count
}

func LivePath(dataDir string) string {
	return filepath.Join(dataDir, "network_log.txt")
}

func ArchiveDir(livePath string) string {
	return filepath.Join(filepath.Dir(livePath), "network_log.d")
}

func DigestPath(livePath string) string {
	return filepath.Join(ArchiveDir(livePath), "digest.tsv")
}

func ArchiveSegments(livePath string) []string {
	entries, err := os.ReadDir(ArchiveDir(livePath))
	if err != nil {
		return nil
	}
	var names []string
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") || !strings.HasPrefix(name, segmentPrefix) || !strings.HasSuffix(name, segmentSuffix) {
			continue
		}
		info, err := os.Stat(filepath.Join(ArchiveDir(livePath), name))
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		names = append(names, name)
	}
	sort.Strings(names)
	out := make([]string, len(names))
	for i, name := range names {
		out[i] = filepath.Join(ArchiveDir(livePath), name)
	}
	return out
}

func stampToMs(stamp string) int64 {
	if len(stamp) != len(nameFormat) {
		return -1
	}
	parsed, err := time.ParseInLocation(nameFormat, stamp, time.Local)
	if err != nil {
		return -1
	}
	return parsed.UnixMilli()
}

func SegmentSpan(path string) (int64, int64, bool) {
	name := filepath.Base(path)
	if !strings.HasPrefix(name, segmentPrefix) || !strings.HasSuffix(name, segmentSuffix) {
		return 0, 0, false
	}
	body := name[len(segmentPrefix) : len(name)-len(segmentSuffix)]
	parts := strings.Split(body, "-")
	if len(parts) != 2 && len(parts) != 3 {
		return 0, 0, false
	}
	first, last := stampToMs(parts[0]), stampToMs(parts[1])
	if first < 0 || last < 0 {
		return 0, 0, false
	}
	return first, last, true
}

func StoredLineTimestampMs(line string) int64 {
	if len(line) < 19 {
		return -1
	}
	head := line[:19]
	for i := 0; i < len(head); i++ {
		if head[i] >= 0x80 {
			return -1
		}
	}
	parsed, err := time.ParseInLocation(storedFormat, head, time.Local)
	if err != nil {
		return -1
	}
	return parsed.UnixMilli()
}

func BucketOf(ms int64) int64 {
	return ms - ms%BucketMs
}

func textModeLine(line string) string {
	if runtime.GOOS == "windows" {
		return strings.TrimSuffix(line, "\r")
	}
	return line
}

func LoadDigest(livePath string) (Digest, bool) {
	data, err := os.ReadFile(DigestPath(livePath))
	if err != nil {
		return Digest{}, false
	}
	return ParseDigest(string(data)), true
}

func parseLongLong(s string) (int64, bool) {
	s = qtTrim(s)
	if strings.HasPrefix(s, "+") {
		s = s[1:]
		if strings.HasPrefix(s, "-") || strings.HasPrefix(s, "+") {
			return 0, false
		}
	}
	if s == "" {
		return 0, false
	}
	value, err := strconv.ParseInt(s, 10, 64)
	return value, err == nil
}

func ParseDigest(text string) Digest {
	digest := Digest{}
	for _, row := range strings.Split(text, "\n") {
		row = textModeLine(row)
		if row == "" {
			continue
		}
		fields := strings.Split(row, "\t")
		if len(fields) != 3 {
			continue
		}
		start, startOK := parseLongLong(fields[0])
		count, countOK := qtToInt(fields[2])
		if !startOK || !countOK || count <= 0 {
			continue
		}
		digest.add(start, fields[1], count)
	}
	return digest
}
