package hosts

const (
	logWindow           = 4
	logMaxBufferedLines = 2000
	logMaxBatchLines    = 200
	logMaxLineChars     = 2000
	recentLinesPerHost  = 200
	maxStreamsPerPeer   = 4
)

var logSourceIDs = []string{"node", "mirror", "web", "ssh", "system", "host"}

func knownLogSource(id string) string {
	for _, candidate := range logSourceIDs {
		if candidate == id {
			return id
		}
	}
	return "host"
}

type logLine struct {
	at     int64
	source string
	text   string
}

type logStream struct {
	buffer       []logLine
	nextSeq      int64
	ackedSeq     int64
	inFlight     int
	dropped      int64
	droppedTotal int64
}

func newLogStream() *logStream { return &logStream{nextSeq: 1} }

func (s *logStream) push(atMs int64, source, text string) {
	line := logLine{at: atMs, source: leftUTF16(source, 16), text: leftUTF16(text, logMaxLineChars)}
	s.buffer = append(s.buffer, line)
	if len(s.buffer) > logMaxBufferedLines {
		excess := len(s.buffer) - logMaxBufferedLines
		s.buffer = append([]logLine(nil), s.buffer[excess:]...)
		s.dropped += int64(excess)
		s.droppedTotal += int64(excess)
	}
}

func (s *logStream) hasPending() bool { return len(s.buffer) > 0 || s.dropped > 0 }

func (s *logStream) canSend() bool { return s.inFlight < logWindow }

func (s *logStream) takeBatch() map[string]any {
	if !s.hasPending() || !s.canSend() {
		return nil
	}
	count := len(s.buffer)
	if count > logMaxBatchLines {
		count = logMaxBatchLines
	}
	lines := make([]map[string]any, 0, count)
	for _, line := range s.buffer[:count] {
		lines = append(lines, map[string]any{"t": float64(line.at), "src": line.source, "text": line.text})
	}
	s.buffer = append([]logLine(nil), s.buffer[count:]...)
	batch := map[string]any{"seq": float64(s.nextSeq), "lines": lines, "dropped": float64(s.dropped)}
	s.nextSeq++
	s.dropped = 0
	s.inFlight++
	return batch
}

func (s *logStream) ack(seq int64) {
	if seq <= s.ackedSeq || seq >= s.nextSeq {
		return
	}
	s.inFlight -= int(seq - s.ackedSeq)
	if s.inFlight < 0 {
		s.inFlight = 0
	}
	s.ackedSeq = seq
}
