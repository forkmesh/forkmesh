package update

import (
	"os"
	"path/filepath"
	"testing"
)

func TestInstallPackageModulesReplacesAndPrunes(t *testing.T) {
	root := t.TempDir()
	build := filepath.Join(root, "build")
	app := filepath.Join(root, "home", ".local", "bin", "forkmesh")
	if err := os.MkdirAll(filepath.Join(build, "packages"), 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range map[string]string{"crm.so": "new", "crm.json": "{}", "notes.txt": "x"} {
		os.WriteFile(filepath.Join(build, "packages", name), []byte(body), 0o644)
	}
	os.WriteFile(filepath.Join(build, "forkmesh"), []byte("app"), 0o755)
	target := installedModuleDir(app)
	os.MkdirAll(target, 0o755)
	os.WriteFile(filepath.Join(target, "crm.so"), []byte("old"), 0o644)
	os.WriteFile(filepath.Join(target, "gone.so"), []byte("old"), 0o644)
	os.WriteFile(filepath.Join(target, "keep.txt"), []byte("mine"), 0o644)
	count, err := installPackageModules(filepath.Join(build, "forkmesh"), app)
	if err != nil || count != 2 {
		t.Fatalf("installed %d, %v", count, err)
	}
	if raw, _ := os.ReadFile(filepath.Join(target, "crm.so")); string(raw) != "new" {
		t.Fatalf("crm.so = %q", raw)
	}
	if _, err := os.Stat(filepath.Join(target, "gone.so")); !os.IsNotExist(err) {
		t.Fatal("stale module kept")
	}
	if _, err := os.Stat(filepath.Join(target, "keep.txt")); err != nil {
		t.Fatal("a file that is not a module was removed")
	}
	if _, err := os.Stat(filepath.Join(target, "notes.txt")); !os.IsNotExist(err) {
		t.Fatal("a non-module file was installed")
	}
	if count, _ := installPackageModules(filepath.Join(root, "nowhere", "forkmesh"), app); count != 0 {
		t.Fatal("installed from a build without modules")
	}
}
