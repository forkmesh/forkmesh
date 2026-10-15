package plugins

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
)

const (
	APIVersion        = 1
	PermissionNetwork = "service.network"
	StartLazy         = "lazy"
	StartBoot         = "boot"
)

var (
	idPattern     = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$`)
	namePattern   = regexp.MustCompile(`^[a-z][A-Za-z0-9_]{0,63}$`)
	interpreters  = map[string]bool{"python3": true, "sh": true, "bash": true, "node": true}
	ErrNoService  = errors.New("plugins: package has no service")
	ErrBadService = errors.New("plugins: invalid service block")
)

type Service struct {
	API             int      `json:"api"`
	Exec            string   `json:"exec"`
	Interpreter     string   `json:"interpreter,omitempty"`
	Start           string   `json:"start,omitempty"`
	Methods         []string `json:"methods"`
	Events          []string `json:"events,omitempty"`
	RemoteMethods   []string `json:"remoteMethods,omitempty"`
	RemoteEvents    []string `json:"remoteEvents,omitempty"`
	IdleExitSeconds int      `json:"idleExitSeconds,omitempty"`
	MemoryMB        int      `json:"memoryMB,omitempty"`
}

type Permission struct {
	ID    string `json:"id"`
	Label string `json:"label"`
	Risk  string `json:"risk"`
}

type Manifest struct {
	ID          string       `json:"id"`
	Name        string       `json:"name"`
	Version     string       `json:"version"`
	Type        string       `json:"type"`
	Summary     string       `json:"summary"`
	Permissions []Permission `json:"permissions"`
	Service     *Service     `json:"service"`
}

func (m Manifest) PermissionIDs() []string {
	out := []string{}
	for _, permission := range m.Permissions {
		if permission.ID != "" {
			out = append(out, permission.ID)
		}
	}
	sort.Strings(out)
	return out
}

func (m Manifest) Wants(permission string) bool {
	for _, id := range m.PermissionIDs() {
		if id == permission {
			return true
		}
	}
	return false
}

func LoadManifest(dir string) (Manifest, error) {
	var manifest Manifest
	raw, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return manifest, err
	}
	if len(raw) > 1<<20 {
		return manifest, errors.New("plugins: manifest too large")
	}
	if err := json.Unmarshal(raw, &manifest); err != nil {
		return manifest, fmt.Errorf("plugins: manifest: %w", err)
	}
	if !idPattern.MatchString(manifest.ID) {
		return manifest, errors.New("plugins: invalid package id")
	}
	if manifest.Service == nil {
		return manifest, ErrNoService
	}
	return manifest, manifest.Service.normalize()
}

func (s *Service) normalize() error {
	if s.API != APIVersion {
		return fmt.Errorf("%w: api %d is not supported (this core speaks %d)", ErrBadService, s.API, APIVersion)
	}
	if s.Start == "" {
		s.Start = StartLazy
	}
	if s.Start != StartLazy && s.Start != StartBoot {
		return fmt.Errorf("%w: start must be lazy or boot", ErrBadService)
	}
	if s.Interpreter != "" && !interpreters[s.Interpreter] {
		return fmt.Errorf("%w: interpreter %q is not allowed", ErrBadService, s.Interpreter)
	}
	if len(s.Methods) == 0 || len(s.Methods) > 64 || len(s.Events) > 64 {
		return fmt.Errorf("%w: 1 to 64 methods and at most 64 events", ErrBadService)
	}
	for _, name := range append(append([]string{}, s.Methods...), s.Events...) {
		if !namePattern.MatchString(name) {
			return fmt.Errorf("%w: bad method or event name %q", ErrBadService, name)
		}
	}
	for _, name := range s.RemoteMethods {
		if !s.HasMethod(name) {
			return fmt.Errorf("%w: remote method %q is not a declared method", ErrBadService, name)
		}
	}
	for _, name := range s.RemoteEvents {
		if !s.HasEvent(name) {
			return fmt.Errorf("%w: remote event %q is not a declared event", ErrBadService, name)
		}
	}
	if s.IdleExitSeconds == 0 {
		s.IdleExitSeconds = 300
	}
	if s.IdleExitSeconds < 10 || s.IdleExitSeconds > 86400 {
		return fmt.Errorf("%w: idleExitSeconds must be 10..86400", ErrBadService)
	}
	if s.MemoryMB == 0 {
		s.MemoryMB = 512
	}
	if s.MemoryMB < 64 || s.MemoryMB > 4096 {
		return fmt.Errorf("%w: memoryMB must be 64..4096", ErrBadService)
	}
	if _, err := s.relativeExec(); err != nil {
		return err
	}
	return nil
}

func (s Service) relativeExec() (string, error) {
	value := strings.NewReplacer("{os}", runtime.GOOS, "{arch}", runtime.GOARCH).Replace(s.Exec)
	if value == "" || strings.Contains(value, `\`) || path.IsAbs(value) {
		return "", fmt.Errorf("%w: exec must be a relative path", ErrBadService)
	}
	clean := path.Clean(value)
	if clean != value || clean == "." || strings.HasPrefix(clean, "../") || clean == ".." {
		return "", fmt.Errorf("%w: exec must be a clean path inside the package", ErrBadService)
	}
	return clean, nil
}

func (s Service) HasMethod(name string) bool {
	for _, method := range s.Methods {
		if method == name {
			return true
		}
	}
	return false
}

func contains(list []string, name string) bool {
	for _, item := range list {
		if item == name {
			return true
		}
	}
	return false
}

func (s Service) Remote(method string) bool { return contains(s.RemoteMethods, method) }

func (s Service) RemoteEvent(event string) bool { return contains(s.RemoteEvents, event) }

func (s Service) HasEvent(name string) bool {
	for _, event := range s.Events {
		if event == name {
			return true
		}
	}
	return false
}

func ExecPath(dir string, service Service) (string, error) {
	relative, err := service.relativeExec()
	if err != nil {
		return "", err
	}
	full := filepath.Join(dir, filepath.FromSlash(relative))
	info, err := os.Lstat(full)
	if err != nil {
		return "", fmt.Errorf("%w: exec %s is missing", ErrBadService, relative)
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("%w: exec %s is not a regular file", ErrBadService, relative)
	}
	return full, nil
}
