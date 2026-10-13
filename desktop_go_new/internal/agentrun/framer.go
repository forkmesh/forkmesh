package agentrun

import (
	"bytes"
	"strconv"
)

const MaxFrameBytes = 8 * 1024 * 1024

type Framer struct {
	buffer     []byte
	max        int
	discarding bool
}

func NewFramer(max int) *Framer {
	if max < 1 {
		max = 1
	}
	return &Framer{max: max}
}

func (f *Framer) Reset() {
	f.buffer = nil
	f.discarding = false
}

func (f *Framer) Buffered() int { return len(f.buffer) }

func (f *Framer) tooLarge() string {
	return "Runtime frame exceeded the " + strconv.Itoa(f.max) + "-byte limit and was discarded."
}

func (f *Framer) Append(data []byte, flush bool) (frames [][]byte, errs []string) {
	searchFrom := len(f.buffer)
	f.buffer = append(f.buffer, data...)
	for {
		newline := bytes.IndexByte(f.buffer[searchFrom:], '\n')
		if newline < 0 {
			break
		}
		newline += searchFrom
		searchFrom = 0
		line := append([]byte(nil), f.buffer[:newline]...)
		f.buffer = f.buffer[newline+1:]
		if f.discarding {
			f.discarding = false
			continue
		}
		frames, errs = f.accept(line, frames, errs)
	}
	if !f.discarding && len(f.buffer) > f.max {
		f.buffer = nil
		f.discarding = true
		errs = append(errs, f.tooLarge())
	}
	if flush {
		if f.discarding {
			f.discarding = false
		} else if len(f.buffer) > 0 {
			frames, errs = f.accept(f.buffer, frames, errs)
		}
		f.buffer = nil
	}
	if len(f.buffer) == 0 {
		f.buffer = nil
	}
	return frames, errs
}

func (f *Framer) accept(line []byte, frames [][]byte, errs []string) ([][]byte, []string) {
	line = bytes.TrimSuffix(line, []byte("\r"))
	if len(line) > f.max {
		return frames, append(errs, f.tooLarge())
	}
	return append(frames, line), errs
}
