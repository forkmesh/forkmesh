package agentrun

const memoryEvents = 4096

func (r *run) rememberLocked(so int64, line []byte) {
	if len(r.memory) >= memoryEvents {
		r.memory = append(r.memory[:0], r.memory[len(r.memory)-memoryEvents/2:]...)
	}
	r.memory = append(r.memory, storedEvent{so: so, line: append([]byte(nil), line...)})
}

func (r *run) storedSinceLocked(after int64) []storedEvent {
	if r.dir != "" {
		return storedSince(r.eventsPath(), r.key, after)
	}
	out := []storedEvent{}
	for _, event := range r.memory {
		if event.so > after {
			out = append(out, event)
		}
	}
	return out
}
