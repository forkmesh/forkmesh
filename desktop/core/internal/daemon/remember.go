package daemon

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

const ownersFile = "owners.json"

func (c *Core) recallOwners() {
	var names []string
	found, err := c.Recall(ownersFile, &names)
	if err != nil {
		c.Logger.Printf("ignoring unreadable saved node owners: %v", err)
		return
	}
	if found && c.Get("control.owners") == nil {
		c.Set("control.owners", names)
	}
}

func (c *Core) rememberedPath(name string) string {
	if c == nil || c.Layout.State == "" || name == "" || filepath.Base(name) != name {
		return ""
	}
	return filepath.Join(c.Layout.CoreState(), name)
}

func (c *Core) Remember(name string, value any) error {
	if c == nil || c.Layout.State == "" {
		return nil
	}
	path := c.rememberedPath(name)
	if path == "" {
		return errors.New("not a file name: " + name)
	}
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	temp, err := os.CreateTemp(filepath.Dir(path), "."+name+".*")
	if err != nil {
		return err
	}
	tempName := temp.Name()
	_ = temp.Chmod(0o600)
	_, writeErr := temp.Write(raw)
	closeErr := temp.Close()
	if writeErr == nil {
		writeErr = closeErr
	}
	if writeErr == nil {
		writeErr = os.Rename(tempName, path)
	}
	if writeErr != nil {
		_ = os.Remove(tempName)
	}
	return writeErr
}

func (c *Core) Recall(name string, value any) (bool, error) {
	path := c.rememberedPath(name)
	if path == "" {
		return false, nil
	}
	raw, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if err := json.Unmarshal(raw, value); err != nil {
		return false, err
	}
	return true, nil
}

func (c *Core) Forget(name string) error {
	path := c.rememberedPath(name)
	if path == "" {
		return nil
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}
