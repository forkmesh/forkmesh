package mesh

import (
	"os"
	"path/filepath"
)

func entryCost(entry Object) int {
	return len16(str(entry["file"])) + len16(str(entry["text"])) + 64
}

func appendBounded(history []Object, message Object, total *int) ([]Object, []string) {
	if entryCost(message) > maxCharsPerChannel {
		copied := Object{}
		for key, value := range message {
			if key != "file" {
				copied[key] = value
			}
		}
		message = copied
	}
	history = append(history, message)
	running := *total + entryCost(message)
	evicted := []string{}
	for len(history) > 1 && (len(history) > maxEntriesPerChannel || running > maxCharsPerChannel) {
		running -= entryCost(history[0])
		if id := str(history[0]["id"]); id != "" {
			evicted = append(evicted, id)
		}
		history = history[1:]
	}
	*total = running
	return history, evicted
}

type historyFile struct {
	History []Object
	Queued  map[string]bool
}

func readHistory(path string) (historyFile, bool) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return historyFile{}, false
	}
	root, ok := parseObject(raw)
	if !ok || qtInt(root["v"]) != historyFileVersion {
		return historyFile{}, false
	}
	out := historyFile{Queued: map[string]bool{}}
	if ids, ok := root["queued"].([]any); ok {
		for _, id := range ids {
			out.Queued[str(id)] = true
		}
	}
	if entries, ok := root["history"].([]any); ok {
		for _, value := range entries {
			entry, _ := value.(Object)
			if entry == nil {
				entry = Object{}
			}
			out.History = append(out.History, entry)
		}
	}
	return out, true
}

func writeHistory(path string, history []Object, queued map[string]bool) error {
	entries := make([]any, 0, len(history))
	for _, entry := range history {
		entries = append(entries, entry)
	}
	ids := make([]any, 0, len(queued))
	for id := range queued {
		ids = append(ids, id)
	}
	data := marshal(Object{"v": historyFileVersion, "history": entries, "queued": ids})
	return writeFileAtomic(path, data, 0o600)
}

func writeFileAtomic(path string, data []byte, mode os.FileMode) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	temp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	name := temp.Name()
	_ = temp.Chmod(mode)
	if _, err := temp.Write(data); err != nil {
		temp.Close()
		os.Remove(name)
		return err
	}
	if err := temp.Sync(); err != nil {
		temp.Close()
		os.Remove(name)
		return err
	}
	if err := temp.Close(); err != nil {
		os.Remove(name)
		return err
	}
	if err := os.Rename(name, path); err != nil {
		os.Remove(name)
		return err
	}
	return nil
}
