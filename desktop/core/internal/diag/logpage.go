package diag

import (
	"bytes"
	"io"
	"os"
	"strings"
)

const (
	maxLogLines    = 400
	maxLogScanSize = 512 * 1024
)

type logLine struct {
	Offset   int64  `json:"offset"`
	T        int64  `json:"t"`
	Severity string `json:"severity"`
	Text     string `json:"text"`
}

type logPageResult struct {
	Lines   []logLine `json:"lines"`
	Start   int64     `json:"start"`
	End     int64     `json:"end"`
	More    bool      `json:"more"`
	Scanned int       `json:"scanned"`
}

func clamp64(value, low, high int64) int64 {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}

func logPage(path string, before int64, maxLines int, query, severity string) logPageResult {
	page := logPageResult{Lines: []logLine{}}
	file, err := os.Open(path)
	if err != nil {
		return page
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return page
	}
	size := info.Size()
	end := size
	if before >= 0 {
		end = clamp64(before, 0, size)
	}
	start := end - maxLogScanSize
	if start < 0 {
		start = 0
	}
	wanted := maxLines
	if wanted < 1 {
		wanted = 1
	}
	if wanted > maxLogLines {
		wanted = maxLogLines
	}
	data := make([]byte, end-start)
	n, err := file.ReadAt(data, start)
	if err != nil && err != io.EOF {
		return page
	}
	data = data[:n]
	firstLineOffset := start
	if start > 0 {
		newline := bytes.IndexByte(data, '\n')
		if newline < 0 {
			page.Start = end
			page.End = end
			page.More = true
			return page
		}
		data = data[newline+1:]
		firstLineOffset = start + int64(newline) + 1
	}
	needle := left(strings.TrimSpace(query), 200)
	type kept struct {
		offset int64
		text   string
	}
	var lines []kept
	offset := firstLineOffset
	for _, raw := range bytes.Split(data, []byte{'\n'}) {
		lineOffset := offset
		offset += int64(len(raw)) + 1
		text := strings.TrimSuffix(fromUTF8(raw), "\r")
		if strings.TrimSpace(text) == "" {
			continue
		}
		page.Scanned++
		if needle != "" && !containsFold(text, needle) {
			continue
		}
		if severity != "" && severity != "all" {
			level := severityOf(text)
			if severity == "warning" {
				if level == "info" {
					continue
				}
			} else if level != severity {
				continue
			}
		}
		lines = append(lines, kept{lineOffset, text})
	}
	from := len(lines) - wanted
	if from < 0 {
		from = 0
	}
	for _, line := range lines[from:] {
		text := redactSecrets(left(line.text, 2000))
		page.Lines = append(page.Lines, logLine{Offset: line.offset, T: timestampOf(text), Severity: severityOf(text), Text: text})
	}
	if from > 0 {
		page.Start = lines[from].offset
	} else {
		page.Start = firstLineOffset
	}
	page.End = end
	page.More = page.Start > 0
	return page
}
