package update

import (
	"os"
	"path/filepath"
	"strings"
)

func packageModuleFile(name string) bool {
	if strings.HasPrefix(name, ".") {
		return false
	}
	switch filepath.Ext(name) {
	case ".json", ".so", ".dylib", ".dll":
		return true
	}
	return false
}

func installedModuleDir(appPath string) string {
	return filepath.Clean(filepath.Join(filepath.Dir(appPath), "..", "lib", "forkmesh", "packages"))
}

func installPackageModules(built, appPath string) (int, error) {
	source := filepath.Join(filepath.Dir(built), "packages")
	entries, err := os.ReadDir(source)
	if err != nil {
		return 0, nil
	}
	target := installedModuleDir(appPath)
	if c := canonical(source); c != "" && c == canonical(target) {
		return 0, nil
	}
	if err := os.MkdirAll(target, 0o755); err != nil {
		return 0, err
	}
	wanted := map[string]bool{}
	installed := 0
	for _, entry := range entries {
		if !entry.Type().IsRegular() || !packageModuleFile(entry.Name()) {
			continue
		}
		wanted[entry.Name()] = true
		temp := filepath.Join(target, "."+entry.Name()+".update")
		_ = os.Remove(temp)
		if err := copyFile(filepath.Join(source, entry.Name()), temp); err != nil {
			return installed, err
		}
		_ = os.Chmod(temp, 0o644)
		if err := os.Rename(temp, filepath.Join(target, entry.Name())); err != nil {
			_ = os.Remove(temp)
			return installed, err
		}
		installed++
	}
	stale, _ := os.ReadDir(target)
	for _, entry := range stale {
		if entry.Type().IsRegular() && packageModuleFile(entry.Name()) && !wanted[entry.Name()] {
			_ = os.Remove(filepath.Join(target, entry.Name()))
		}
	}
	return installed, nil
}
