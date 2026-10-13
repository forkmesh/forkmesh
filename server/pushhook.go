package mirrornode

import (
	"bytes"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
)

const pushWakeHookMarker = "# Managed-By: ForkMesh mirror-node\n"

// pushWakeURL returns the loopback-only control endpoint a post-receive hook
// may ring after Git has made new refs durable. Config validation applies the
// same host allowlist; repeat it here so this helper remains safe on its own.
func pushWakeURL(listen string) (string, error) {
	host, portText, err := net.SplitHostPort(listen)
	if err != nil {
		return "", errors.New("mirror control listen address is invalid")
	}
	if host != "127.0.0.1" && host != "::1" && host != "localhost" {
		return "", errors.New("mirror control endpoint is not loopback")
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return "", errors.New("mirror control port is invalid")
	}
	if host == "::1" {
		host = "[::1]"
	}
	return fmt.Sprintf(
		"http://%s:%d/v1/control/sync?local=1", host, port), nil
}

func pushWakeHook(listen string) ([]byte, error) {
	endpoint, err := pushWakeURL(listen)
	if err != nil {
		return nil, err
	}
	return []byte(
		"#!/bin/sh\n" +
			pushWakeHookMarker +
			"# A ref update is already durable. Wake the local daemon so it " +
			"publishes and reloads that exact generation before any upstream " +
			"reconciliation can replace it.\n" +
			"if [ -x /usr/bin/curl ]; then\n" +
			"  /usr/bin/curl -fsS --connect-timeout 2 --max-time 5 " +
			"-X POST " + endpoint + " >/dev/null 2>&1 || :\n" +
			"fi\n" +
			"exit 0\n"), nil
}

// ensurePushWakeHook installs only the hook owned by this service. An
// operator-authored post-receive hook is preserved byte-for-byte rather than
// being overwritten or silently composed into a shell script we do not own.
func ensurePushWakeHook(gitDir, listen string) (bool, error) {
	if !filepath.IsAbs(gitDir) {
		return false, errors.New("repository path is not absolute")
	}
	wanted, err := pushWakeHook(listen)
	if err != nil {
		return false, err
	}
	hooksDir := filepath.Join(gitDir, "hooks")
	hooksInfo, statErr := os.Lstat(hooksDir)
	if statErr == nil {
		if !hooksInfo.IsDir() || hooksInfo.Mode()&os.ModeSymlink != 0 {
			return false, errors.New("hooks path is not a directory")
		}
	} else if errors.Is(statErr, os.ErrNotExist) {
		if err := os.Mkdir(hooksDir, 0700); err != nil {
			return false, fmt.Errorf("create hooks directory: %w", err)
		}
	} else {
		return false, fmt.Errorf("inspect hooks directory: %w", statErr)
	}
	path := filepath.Join(hooksDir, "post-receive")
	if info, statErr := os.Lstat(path); statErr == nil {
		if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 {
			return false, errors.New("post-receive hook is not a regular file")
		}
		// A service-owned hook is tiny. Avoid reading an unbounded custom hook;
		// preserving it is the same safe outcome as finding no ownership marker.
		if info.Size() > 64*1024 {
			return false, nil
		}
		current, readErr := os.ReadFile(path)
		if readErr != nil {
			return false, fmt.Errorf("read post-receive hook: %w", readErr)
		}
		if bytes.Equal(current, wanted) {
			if info.Mode().Perm()&0100 == 0 {
				if chmodErr := os.Chmod(path, 0700); chmodErr != nil {
					return false, fmt.Errorf(
						"make post-receive hook executable: %w", chmodErr)
				}
				return true, nil
			}
			return false, nil
		}
		if !bytes.Contains(current, []byte(pushWakeHookMarker)) {
			return false, nil
		}
	} else if !errors.Is(statErr, os.ErrNotExist) {
		return false, fmt.Errorf("inspect post-receive hook: %w", statErr)
	}

	temporary, err := os.CreateTemp(hooksDir, ".post-receive-*")
	if err != nil {
		return false, fmt.Errorf("create post-receive hook: %w", err)
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0700); err != nil {
		temporary.Close()
		return false, err
	}
	if _, err := temporary.Write(wanted); err != nil {
		temporary.Close()
		return false, err
	}
	if err := temporary.Sync(); err != nil {
		temporary.Close()
		return false, err
	}
	if err := temporary.Close(); err != nil {
		return false, err
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return false, fmt.Errorf("install post-receive hook: %w", err)
	}
	return true, nil
}

// ensurePushWakeHooks covers upgraded nodes as well as fresh bootstrap state.
// Gateway aliases can point at one Git directory, so touch each repository at
// most once per cycle. A release-store container that has not become a bare
// repository yet is skipped; the next bootstrap retry installs the hook after
// Git initialization succeeds.
func (d *Daemon) ensurePushWakeHooks() error {
	seen := map[string]bool{}
	for _, repository := range d.gateway.Repositories {
		gitDir := filepath.Clean(repository.GitDir)
		if seen[gitDir] {
			continue
		}
		seen[gitDir] = true
		head, headErr := os.Stat(filepath.Join(gitDir, "HEAD"))
		objects, objectsErr := os.Stat(filepath.Join(gitDir, "objects"))
		if headErr != nil || objectsErr != nil || !head.Mode().IsRegular() ||
			!objects.IsDir() {
			continue
		}
		if _, err := ensurePushWakeHook(gitDir, d.config.Listen); err != nil {
			return fmt.Errorf("install push wake hook for %s: %w",
				repositoryKey(repository), err)
		}
	}
	return nil
}
