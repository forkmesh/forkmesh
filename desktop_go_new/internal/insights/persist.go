package insights

import (
	"os"
	"path/filepath"
	"sync"
)

var writeMu sync.Mutex

func writeAtomic(path string, data []byte) error {
	writeMu.Lock()
	defer writeMu.Unlock()
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	file, err := os.CreateTemp(dir, "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	temp := file.Name()
	if _, err := file.Write(data); err != nil {
		file.Close()
		os.Remove(temp)
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		os.Remove(temp)
		return err
	}
	if err := file.Close(); err != nil {
		os.Remove(temp)
		return err
	}
	_ = os.Chmod(temp, 0o644)
	if err := os.Rename(temp, path); err != nil {
		os.Remove(temp)
		return err
	}
	return nil
}
