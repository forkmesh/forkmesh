package netlog

import (
	"bytes"
	"io"
	"math"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	tailProbeBytes  = 64
	recentWindowMs  = int64(2 * 60 * 1000)
	maxRecent       = 1024
	pendingLifetime = 30 * time.Second
	maxPending      = 4096
	maxOverrides    = 4096
)

type liveState struct {
	info   os.FileInfo
	offset int64
	tail   []byte
	counts Digest
}

type segmentState struct {
	size   int64
	mod    time.Time
	counts Digest
}

type recentLine struct {
	text    string
	bucket  int64
	badge   string
	counts  Digest
	claimed bool
}

type pendingLine struct {
	text   string
	bucket int64
	badge  string
	at     time.Time
}

type digestCache struct {
	size   int64
	mod    time.Time
	exists bool
	digest Digest
}

type AppendLine struct {
	Line  string `json:"line"`
	Badge string `json:"badge"`
}

type Tracker struct {
	LivePath string
	Now      func() time.Time

	mu        sync.Mutex
	live      liveState
	segments  map[string]*segmentState
	recent    []recentLine
	pending   []pendingLine
	overrides map[string]string
	order     []string
	base      digestCache
}

func NewTracker(livePath string) *Tracker {
	return &Tracker{LivePath: livePath, segments: map[string]*segmentState{}, overrides: map[string]string{}}
}

func (t *Tracker) now() time.Time {
	if t.Now != nil {
		return t.Now()
	}
	return time.Now()
}

func firstLine(text string) string {
	if i := strings.IndexByte(text, '\n'); i >= 0 {
		return text[:i]
	}
	return text
}

func (t *Tracker) remember(text, badge string) {
	if _, ok := t.overrides[text]; !ok {
		t.order = append(t.order, text)
	}
	t.overrides[text] = badge
	for len(t.order) > maxOverrides {
		delete(t.overrides, t.order[0])
		t.order = t.order[1:]
	}
}

func decrement(counts Digest, bucket int64, badge string) {
	row := counts[bucket]
	if row == nil {
		return
	}
	row[badge]--
	if row[badge] <= 0 {
		delete(row, badge)
	}
	if len(row) == 0 {
		delete(counts, bucket)
	}
}

func (t *Tracker) countLine(text string, counts Digest, nowMs int64) {
	ts := StoredLineTimestampMs(text)
	if ts < 0 {
		return
	}
	bucket := BucketOf(ts)
	badge := ""
	recent := ts >= nowMs-recentWindowMs
	matched := false
	if recent {
		for i, pending := range t.pending {
			if pending.text == text {
				badge = pending.badge
				matched = true
				t.pending = append(t.pending[:i], t.pending[i+1:]...)
				if badge != Badge(text) {
					t.remember(text, badge)
				}
				break
			}
		}
	}
	if badge == "" {
		if override, ok := t.overrides[text]; ok {
			badge = override
		} else {
			badge = Badge(text)
		}
	}
	counts.add(bucket, badge, 1)
	if recent {
		t.recent = append(t.recent, recentLine{text: text, bucket: bucket, badge: badge, counts: counts, claimed: matched})
		if len(t.recent) > maxRecent {
			t.recent = t.recent[len(t.recent)-maxRecent:]
		}
	}
}

func (t *Tracker) countBytes(data []byte, counts Digest, nowMs int64) {
	for _, raw := range strings.Split(string(data), "\n") {
		line := textModeLine(raw)
		if line == "" {
			continue
		}
		t.countLine(line, counts, nowMs)
	}
}

func readAt(path string, offset, length int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	buf := make([]byte, length)
	n, err := file.ReadAt(buf, offset)
	if err != nil && err != io.EOF {
		return nil, err
	}
	return buf[:n], nil
}

func (t *Tracker) resetLive() {
	t.live = liveState{counts: Digest{}}
}

func (t *Tracker) refreshLive(nowMs int64) bool {
	info, err := os.Stat(t.LivePath)
	if err != nil || !info.Mode().IsRegular() {
		moved := t.live.info != nil
		t.resetLive()
		return moved
	}
	moved := false
	if t.live.counts == nil {
		t.resetLive()
	}
	if t.live.info != nil {
		reset := !os.SameFile(t.live.info, info) || info.Size() < t.live.offset
		if !reset && len(t.live.tail) > 0 {
			probe, err := readAt(t.LivePath, t.live.offset-int64(len(t.live.tail)), int64(len(t.live.tail)))
			reset = err != nil || !bytes.Equal(probe, t.live.tail)
		}
		if reset {
			moved = !os.SameFile(t.live.info, info)
			t.resetLive()
		}
	}
	t.live.info = info
	if info.Size() <= t.live.offset {
		return moved
	}
	data, err := readAt(t.LivePath, t.live.offset, info.Size()-t.live.offset)
	if err != nil {
		return moved
	}
	end := bytes.LastIndexByte(data, '\n')
	if end < 0 {
		return moved
	}
	complete := data[:end+1]
	t.countBytes(complete, t.live.counts, nowMs)
	t.live.offset += int64(len(complete))
	src := complete
	if len(src) > tailProbeBytes {
		src = src[len(src)-tailProbeBytes:]
	}
	joined := append(append([]byte{}, t.live.tail...), src...)
	if len(joined) > tailProbeBytes {
		joined = joined[len(joined)-tailProbeBytes:]
	}
	t.live.tail = joined
	return moved
}

func (t *Tracker) segmentCounts(path string, nowMs int64) Digest {
	info, err := os.Stat(path)
	if err != nil {
		delete(t.segments, path)
		return nil
	}
	if cached, ok := t.segments[path]; ok && cached.size == info.Size() && cached.mod.Equal(info.ModTime()) {
		return cached.counts
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	counts := Digest{}
	t.countBytes(data, counts, nowMs)
	t.segments[path] = &segmentState{size: info.Size(), mod: info.ModTime(), counts: counts}
	return counts
}

func (t *Tracker) loadBase() (Digest, bool) {
	path := DigestPath(t.LivePath)
	info, err := os.Stat(path)
	if err != nil {
		t.base = digestCache{}
		return nil, false
	}
	if t.base.exists && t.base.size == info.Size() && t.base.mod.Equal(info.ModTime()) {
		return t.base.digest, true
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.base = digestCache{}
		return nil, false
	}
	digest := ParseDigest(string(data))
	t.base = digestCache{size: info.Size(), mod: info.ModTime(), exists: true, digest: digest}
	return digest, true
}

func (t *Tracker) expirePending(now time.Time) {
	kept := t.pending[:0]
	for _, line := range t.pending {
		if now.Sub(line.at) < pendingLifetime {
			kept = append(kept, line)
		}
	}
	t.pending = kept
	if len(t.pending) > maxPending {
		t.pending = t.pending[len(t.pending)-maxPending:]
	}
}

func merge(into, from Digest, minBucket int64) {
	for bucket, row := range from {
		if bucket < minBucket {
			continue
		}
		for badge, count := range row {
			if count > 0 {
				into.add(bucket, badge, count)
			}
		}
	}
}

func (t *Tracker) Snapshot() Digest {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := t.now()
	nowMs := now.UnixMilli()
	t.expirePending(now)
	t.refreshLive(nowMs)
	base, hasBase := t.loadBase()
	watermark := int64(math.MinInt64)
	if hasBase {
		for bucket := range base {
			if watermark == math.MinInt64 || bucket > watermark {
				watermark = bucket
			}
		}
	}
	out := Digest{}
	for bucket, row := range base {
		if bucket >= watermark {
			continue
		}
		for badge, count := range row {
			out.add(bucket, badge, count)
		}
	}
	segments := ArchiveSegments(t.LivePath)
	seen := map[string]bool{}
	if watermark == math.MinInt64 {
		budget := RebuildScanBytes
		files := append([]string{t.LivePath}, reversed(segments)...)
		for _, path := range files {
			if budget <= 0 {
				break
			}
			if info, err := os.Stat(path); err == nil {
				budget -= info.Size()
			}
			if path == t.LivePath {
				merge(out, t.live.counts, math.MinInt64)
				continue
			}
			seen[path] = true
			merge(out, t.segmentCounts(path, nowMs), math.MinInt64)
		}
	} else {
		merge(out, t.live.counts, watermark)
		for _, path := range segments {
			_, last, ok := SegmentSpan(path)
			if !ok || last < watermark {
				continue
			}
			seen[path] = true
			merge(out, t.segmentCounts(path, nowMs), watermark)
		}
	}
	for path := range t.segments {
		if !seen[path] {
			delete(t.segments, path)
		}
	}
	for _, line := range t.pending {
		if line.bucket >= watermark {
			out.add(line.bucket, line.badge, 1)
		}
	}
	cutoff := nowMs - RetentionMs
	for bucket := range out {
		if bucket < cutoff {
			delete(out, bucket)
		}
	}
	return out
}

func reversed(items []string) []string {
	out := make([]string, len(items))
	for i, item := range items {
		out[len(items)-1-i] = item
	}
	return out
}

func (t *Tracker) Append(lines []AppendLine) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := t.now()
	nowMs := now.UnixMilli()
	t.expirePending(now)
	if t.refreshLive(nowMs) {
		for _, path := range ArchiveSegments(t.LivePath) {
			if _, last, ok := SegmentSpan(path); ok && last >= nowMs-recentWindowMs {
				t.segmentCounts(path, nowMs)
			}
		}
	}
	accepted := 0
	for _, line := range lines {
		text := textModeLine(firstLine(line.Line))
		ts := StoredLineTimestampMs(text)
		if ts < 0 {
			continue
		}
		accepted++
		badge := qtTrim(line.Badge)
		if badge == "" {
			badge = Badge(text)
		}
		bucket := BucketOf(ts)
		claimed := false
		for i := range t.recent {
			entry := &t.recent[i]
			if entry.claimed || entry.text != text {
				continue
			}
			entry.claimed = true
			claimed = true
			if entry.badge != badge {
				decrement(entry.counts, entry.bucket, entry.badge)
				entry.counts.add(entry.bucket, badge, 1)
				entry.badge = badge
				t.remember(text, badge)
			}
			break
		}
		if !claimed {
			t.pending = append(t.pending, pendingLine{text: text, bucket: bucket, badge: badge, at: now})
		}
	}
	if len(t.pending) > maxPending {
		t.pending = t.pending[len(t.pending)-maxPending:]
	}
	return accepted
}
