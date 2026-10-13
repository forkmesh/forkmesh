package mirrornode

// Per-session job homes and provider credential hand-over (docs/RUNNERS.md).
//
// A job never runs with the job user's persistent HOME: that home holds the
// provider logins, and a job that could write it (settings.json hooks, a
// shell rc file) would reach every later job. Each agent session instead
// gets <workRoot>/.homes/<workdir name>, so a Claude Code follow-up can
// still --resume its own conversation. Between jobs that directory is owned
// by the daemon (root) with mode 0700, so no other job, of any tenant, can
// read it or plant anything in it.
//
// Before a job, only the provider credential files are copied from the
// persistent home into the session home. After the job, a credential the
// CLI refreshed is copied back, only when it is still a regular file owned
// by the job user, small and valid JSON; then the copies are deleted.
// Nothing else ever flows back.
//
// Every file the daemon (root) reads from a job-writable place is opened
// with O_NOFOLLOW and checked to be a regular file owned by the job user,
// and every directory on the way is checked with Lstat. No job process runs
// while this happens: the job's scope is stopped when its main process ends.

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

const (
	jobHomesDir         = ".homes"
	jobPublishDir       = ".publish"
	jobCacheDir         = ".cache"
	jobCacheTTL         = 14 * 24 * time.Hour
	jobCredentialLimit  = 256 << 10
	jobSessionHomeTTL   = 14 * 24 * time.Hour
	jobHomeLockedMode   = 0o700
	jobHomeUnlockedMode = 0o700
)

// jobCredentialFiles are the provider login files a job may use, relative
// to a HOME. Nothing else is copied into or out of a session home.
var jobCredentialFiles = []string{
	filepath.Join(".claude", ".credentials.json"),
	filepath.Join(".codex", "auth.json"),
}

// credentialMu serialises writes to the persistent home's credentials.
var credentialMu sync.Mutex

var credentialHandedOut = map[string][]byte{}

func credentialHandOutKey(home, rel string) string { return home + "\x00" + rel }

// homeFor is the session HOME of a job workdir, or "" when dir is not a
// workdir directly under workRoot.
func (b jobSandbox) homeFor(dir string) string {
	if b.workRoot == "" || dir == "" || filepath.Dir(filepath.Clean(dir)) != b.workRoot {
		return ""
	}
	base := filepath.Base(dir)
	if strings.HasPrefix(base, ".") {
		return ""
	}
	return filepath.Join(b.workRoot, jobHomesDir, base)
}

func (b jobSandbox) isSessionHome(home string) bool {
	return b.workRoot != "" && home != "" &&
		filepath.Dir(filepath.Clean(home)) == filepath.Join(b.workRoot, jobHomesDir)
}

// persistentHome is where the provider sign-in stores credentials.
func (b jobSandbox) persistentHome() string {
	if b.home != "" {
		return b.home
	}
	if b.uid == 0 {
		return os.Getenv("HOME")
	}
	return ""
}

func (b jobSandbox) credentialSource(rel string) (string, string) {
	if dir := b.credentialDirs[rel]; dir != "" {
		return dir, filepath.Base(rel)
	}
	return b.persistentHome(), rel
}

// ensureDaemonDir makes dir (not a symlink) with mode, owned by the daemon.
func ensureDaemonDir(dir string, mode os.FileMode) error {
	info, err := os.Lstat(dir)
	if err == nil && (!info.IsDir() || info.Mode()&os.ModeSymlink != 0) {
		return fmt.Errorf("%s is not a directory", dir)
	}
	if errors.Is(err, os.ErrNotExist) {
		if err := os.Mkdir(dir, mode); err != nil {
			return err
		}
	} else if err != nil {
		return err
	}
	return os.Chmod(dir, mode)
}

// openSession gives the job user its session home: a fresh tmp, the
// provider credentials, and ownership. Old session homes are pruned.
func (b jobSandbox) openSession(home string) error {
	if home == "" {
		return nil
	}
	homes := filepath.Dir(home)
	if err := ensureDaemonDir(homes, 0o711); err != nil {
		return err
	}
	b.pruneSessionHomes(homes, home)
	info, err := os.Lstat(home)
	if err == nil && (!info.IsDir() || info.Mode()&os.ModeSymlink != 0) {
		if err := removeTreeUnder(homes, home); err != nil {
			return err
		}
		err = os.ErrNotExist
	}
	if errors.Is(err, os.ErrNotExist) {
		if err := os.Mkdir(home, jobHomeLockedMode); err != nil {
			return err
		}
	} else if err != nil {
		return err
	}
	// A fresh TMPDIR for every job; whatever the last job left is gone.
	tmp := filepath.Join(home, "tmp")
	if err := removeTreeUnder(home, tmp); err != nil {
		return err
	}
	if err := os.Mkdir(tmp, 0o700); err != nil {
		return err
	}
	if b.uid != 0 {
		if err := os.Lchown(tmp, b.uid, b.gidOrUID()); err != nil {
			return err
		}
	}
	for _, rel := range jobCredentialFiles {
		if err := b.copyCredentialIn(home, rel); err != nil {
			return err
		}
	}
	now := time.Now()
	_ = os.Chtimes(home, now, now)
	if b.uid != 0 {
		if err := os.Lchown(home, b.uid, b.gidOrUID()); err != nil {
			return err
		}
	}
	return os.Chmod(home, jobHomeUnlockedMode)
}

// closeSession copies back refreshed credentials, deletes the copies and
// the job's tmp, and takes the home away from the job user.
func (b jobSandbox) closeSession(home string) {
	if home == "" {
		return
	}
	if info, err := os.Lstat(home); err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return
	}
	// Lock first: from here on no job-user process can change the tree.
	if b.uid != 0 {
		_ = os.Lchown(home, os.Geteuid(), os.Getegid())
	}
	_ = os.Chmod(home, jobHomeLockedMode)
	for _, rel := range jobCredentialFiles {
		b.copyCredentialBack(home, rel)
		_ = os.Remove(filepath.Join(home, rel))
	}
	_ = removeTreeUnder(home, filepath.Join(home, "tmp"))
	b.accounts.closed(home)
	now := time.Now()
	_ = os.Chtimes(home, now, now)
}

func (b jobSandbox) gidOrUID() int {
	if b.gid != 0 {
		return b.gid
	}
	return b.uid
}

// pruneSessionHomes removes session homes unused for jobSessionHomeTTL.
func (b jobSandbox) pruneSessionHomes(homes, keep string) {
	entries, err := os.ReadDir(homes)
	if err != nil {
		return
	}
	for _, entry := range entries {
		path := filepath.Join(homes, entry.Name())
		if path == keep {
			continue
		}
		if info, err := os.Lstat(path); err == nil && time.Since(info.ModTime()) > jobSessionHomeTTL {
			_ = removeOwnCloneUnder(homes, path)
		}
	}
}

// safeDirChain reports whether every directory from base down to the parent
// of base/rel exists as a real directory (no symlinks).
func safeDirChain(base, rel string) bool {
	path := base
	info, err := os.Lstat(path)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return false
	}
	parts := strings.Split(filepath.Dir(rel), string(filepath.Separator))
	for _, part := range parts {
		if part == "." || part == "" {
			continue
		}
		path = filepath.Join(path, part)
		info, err := os.Lstat(path)
		if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return false
		}
	}
	return true
}

// readOwnedFile reads a regular, non-symlink file of at most limit bytes,
// owned by uid when uid is not 0. It never blocks on a FIFO.
func readOwnedFile(path string, uid int, limit int64) ([]byte, error) {
	file, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file", filepath.Base(path))
	}
	if stat, ok := info.Sys().(*syscall.Stat_t); ok && uid != 0 && int(stat.Uid) != uid {
		return nil, fmt.Errorf("%s is not owned by the job user", filepath.Base(path))
	}
	if info.Size() > limit {
		return nil, fmt.Errorf("%s is too large", filepath.Base(path))
	}
	data, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("%s is too large", filepath.Base(path))
	}
	return data, nil
}

// writeOwnedFile atomically replaces dir/name with data, mode 0600, owned
// by uid (when not 0). dir must already be checked with safeDirChain.
func writeOwnedFile(dir, name string, data []byte, uid, gid int) error {
	temp := filepath.Join(dir, "."+name+".fmnext-tmp")
	_ = os.Remove(temp)
	file, err := os.OpenFile(temp, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o600)
	if err != nil {
		return err
	}
	if _, err := file.Write(data); err != nil {
		file.Close()
		os.Remove(temp)
		return err
	}
	if uid != 0 {
		if err := file.Chown(uid, gid); err != nil {
			file.Close()
			os.Remove(temp)
			return err
		}
	}
	if err := file.Close(); err != nil {
		os.Remove(temp)
		return err
	}
	// rename replaces a symlink at the target itself; it never follows it.
	return os.Rename(temp, filepath.Join(dir, name))
}

func credentialExpiry(rel string, data []byte) int64 {
	codex := runnerAccountProvider(rel) == "codex"
	status := inspectCredential(codex, data, time.UnixMilli(0))
	if codex {
		var auth struct {
			LastRefresh string `json:"last_refresh"`
		}
		if json.Unmarshal(data, &auth) == nil {
			if at, err := time.Parse(time.RFC3339Nano, auth.LastRefresh); err == nil {
				return max(status.AccessExpiresAt, at.UnixMilli())
			}
		}
	}
	return status.AccessExpiresAt
}

func credentialNewer(rel string, candidate, current []byte) bool {
	return credentialExpiry(rel, candidate) > credentialExpiry(rel, current)
}

func validCredential(data []byte) bool {
	var value map[string]any
	return len(bytes.TrimSpace(data)) != 0 && json.Unmarshal(data, &value) == nil && len(value) != 0
}

// copyCredentialIn puts the persistent login rel into the session home.
func (b jobSandbox) copyCredentialIn(home, rel string) error {
	target := filepath.Join(home, rel)
	// Whatever the session home holds there now is not trusted.
	if parent := filepath.Dir(target); parent != home {
		if info, err := os.Lstat(parent); err == nil && (!info.IsDir() || info.Mode()&os.ModeSymlink != 0) {
			if err := os.Remove(parent); err != nil {
				return err
			}
		}
	}
	_ = os.Remove(target)
	source, sourceRel := b.credentialSource(rel)
	if source == "" || !safeDirChain(source, sourceRel) {
		return nil
	}
	credentialMu.Lock()
	b.accounts.noteOpenedLocked(home, rel)
	data, err := readOwnedFile(filepath.Join(source, sourceRel), b.uid, jobCredentialLimit)
	delete(credentialHandedOut, credentialHandOutKey(home, rel))
	if err == nil && validCredential(data) {
		credentialHandedOut[credentialHandOutKey(home, rel)] = data
	}
	credentialMu.Unlock()
	if err != nil || !validCredential(data) {
		return nil
	}
	parent := filepath.Dir(target)
	if err := os.MkdirAll(parent, 0o700); err != nil {
		return err
	}
	if b.uid != 0 {
		if err := os.Lchown(parent, b.uid, b.gidOrUID()); err != nil {
			return err
		}
	}
	return writeOwnedFile(parent, filepath.Base(target), data, b.uid, b.gidOrUID())
}

// copyCredentialBack keeps a refreshed login: the session copy replaces
// the persistent one when it differs and still looks like a credential.
func (b jobSandbox) copyCredentialBack(home, rel string) {
	persistent, persistentRel := b.credentialSource(rel)
	if persistent == "" || !safeDirChain(home, rel) {
		return
	}
	data, err := readOwnedFile(filepath.Join(home, rel), b.uid, jobCredentialLimit)
	credentialMu.Lock()
	defer credentialMu.Unlock()
	handedOut, known := credentialHandedOut[credentialHandOutKey(home, rel)]
	delete(credentialHandedOut, credentialHandOutKey(home, rel))
	if err != nil || !usableCredential(runnerAccountProvider(rel), data) || (known && bytes.Equal(handedOut, data)) {
		return
	}
	if b.accounts.copyBackLocked(home, rel, data) {
		return
	}
	if !safeDirChain(persistent, persistentRel) {
		return
	}
	current, err := readOwnedFile(filepath.Join(persistent, persistentRel), b.uid, jobCredentialLimit)
	if err != nil || bytes.Equal(current, data) {
		return
	}
	if (!known || !bytes.Equal(current, handedOut)) && !credentialNewer(rel, data, current) {
		return
	}
	_ = writeOwnedFile(filepath.Dir(filepath.Join(persistent, persistentRel)), filepath.Base(persistentRel), data,
		b.uid, b.gidOrUID())
}
