package agentproc

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

const (
	streamOut = "out"
	streamErr = "err"

	defaultSegmentBytes = 4 << 20
	defaultCapBytes     = 512 << 20
)

func segmentName(stream string, start int64) string {
	return fmt.Sprintf("%s.%016d", stream, start)
}

func listSegments(dir, stream string) []int64 {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	prefix := stream + "."
	out := []int64{}
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, prefix) || entry.IsDir() {
			continue
		}
		start, err := strconv.ParseInt(name[len(prefix):], 10, 64)
		if err != nil || start < 0 || len(name)-len(prefix) != 16 {
			continue
		}
		out = append(out, start)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

type spoolWriter struct {
	dir      string
	stream   string
	segBytes int64
	capBytes int64
	file     *os.File
	segStart int64
	segSize  int64
	total    int64
}

func newSpoolWriter(dir, stream string, segBytes, capBytes int64) *spoolWriter {
	if segBytes <= 0 {
		segBytes = defaultSegmentBytes
	}
	if capBytes <= 0 {
		capBytes = defaultCapBytes
	}
	if capBytes < segBytes*2 {
		capBytes = segBytes * 2
	}
	return &spoolWriter{dir: dir, stream: stream, segBytes: segBytes, capBytes: capBytes}
}

func (w *spoolWriter) open() error {
	file, err := os.OpenFile(filepath.Join(w.dir, segmentName(w.stream, w.total)), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return err
	}
	w.file = file
	w.segStart = w.total
	w.segSize = 0
	return nil
}

func (w *spoolWriter) Write(data []byte) error {
	for len(data) > 0 {
		if w.file == nil || w.segSize >= w.segBytes {
			if w.file != nil {
				_ = w.file.Close()
				w.file = nil
			}
			if err := w.open(); err != nil {
				return err
			}
			w.enforceCap()
		}
		room := w.segBytes - w.segSize
		chunk := data
		if int64(len(chunk)) > room {
			chunk = data[:room]
		}
		n, err := w.file.Write(chunk)
		w.segSize += int64(n)
		w.total += int64(n)
		if err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

func (w *spoolWriter) enforceCap() {
	segments := listSegments(w.dir, w.stream)
	for len(segments) > 1 && w.total-segments[1] >= w.capBytes {
		_ = os.Remove(filepath.Join(w.dir, segmentName(w.stream, segments[0])))
		segments = segments[1:]
	}
}

func (w *spoolWriter) Close() {
	if w.file != nil {
		_ = w.file.Close()
		w.file = nil
	}
}

func readSpool(dir, stream string, offset, limit int64) ([]byte, int64, error) {
	if limit <= 0 {
		return nil, offset, nil
	}
	segments := listSegments(dir, stream)
	if len(segments) == 0 {
		return nil, offset, nil
	}
	if offset < segments[0] {
		offset = segments[0]
	}
	index := sort.Search(len(segments), func(i int) bool { return segments[i] > offset }) - 1
	if index < 0 {
		index = 0
	}
	start := segments[index]
	if index+1 < len(segments) && limit > segments[index+1]-offset {
		limit = segments[index+1] - offset
	}
	file, err := os.Open(filepath.Join(dir, segmentName(stream, start)))
	if err != nil {
		return nil, offset, err
	}
	defer file.Close()
	buffer := make([]byte, limit)
	n, err := file.ReadAt(buffer, offset-start)
	if err != nil && err != io.EOF {
		return nil, offset, err
	}
	return buffer[:n], offset, nil
}

func spoolLength(dir, stream string) int64 {
	segments := listSegments(dir, stream)
	if len(segments) == 0 {
		return 0
	}
	last := segments[len(segments)-1]
	info, err := os.Stat(filepath.Join(dir, segmentName(stream, last)))
	if err != nil {
		return last
	}
	return last + info.Size()
}

func trimSpool(dir, stream string, acked int64) int {
	segments := listSegments(dir, stream)
	removed := 0
	for i := 0; i+1 < len(segments); i++ {
		if segments[i+1] > acked {
			break
		}
		if os.Remove(filepath.Join(dir, segmentName(stream, segments[i]))) == nil {
			removed++
		}
	}
	return removed
}
