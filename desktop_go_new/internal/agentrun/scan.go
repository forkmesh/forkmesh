package agentrun

import (
	"bytes"
	"io"
	"os"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const scanChunk = 1 << 20

type storedEvent struct {
	so   int64
	line []byte
}

func scanBack(path string, visit func(line []byte) bool) {
	file, err := os.Open(path)
	if err != nil {
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return
	}
	end := info.Size()
	var carry []byte
	for end > 0 {
		start := end - scanChunk
		if start < 0 {
			start = 0
		}
		chunk := make([]byte, end-start, end-start+int64(len(carry)))
		if _, err := file.ReadAt(chunk, start); err != nil && err != io.EOF {
			return
		}
		chunk = append(chunk, carry...)
		for {
			cut := bytes.LastIndexByte(chunk[:max(0, len(chunk)-1)], '\n')
			if cut < 0 {
				break
			}
			line := bytes.TrimSpace(chunk[cut+1:])
			chunk = chunk[:cut+1]
			if len(line) > 0 && !visit(line) {
				return
			}
		}
		carry = chunk
		end = start
	}
	if line := bytes.TrimSpace(carry); len(line) > 0 {
		visit(line)
	}
}

func storedSince(path, key string, after int64) []storedEvent {
	out := []storedEvent{}
	scanBack(path, func(line []byte) bool {
		if !bytes.Contains(line, []byte(`"_sk":`)) {
			return true
		}
		event, ok := qtjson.ParseObject(line)
		if !ok {
			return true
		}
		if event.Get("_sk").Str() != key {
			return false
		}
		so := event.Get("_so").Int64()
		if so <= after {
			return false
		}
		out = append(out, storedEvent{so: so, line: append([]byte(nil), line...)})
		return true
	})
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

func lastStoredOffset(path, key string) int64 {
	last := int64(0)
	scanBack(path, func(line []byte) bool {
		if !bytes.Contains(line, []byte(`"_sk":`)) {
			return true
		}
		event, ok := qtjson.ParseObject(line)
		if !ok {
			return true
		}
		if event.Get("_sk").Str() != key {
			return false
		}
		last = event.Get("_so").Int64()
		return false
	})
	return last
}
