package paths

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const HomeProject = "forkmesh"

var slugPattern = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,46}[a-z0-9])?$`)

func IsValidSlug(slug string) bool {
	return slugPattern.MatchString(slug)
}

type Layout struct {
	ProjectsRoot        string
	HomeDir             string
	Project             string
	ProjectDir          string
	RerootAllowed       bool
	RerootBlockedReason string
	Data                string
	Config              string
	Cache               string
	State               string
	Logs                string
	SSH                 string
	LegacyData          string
	LegacyConfig        string
	LegacyCache         string
	LegacyState         string
}

func (l Layout) IsHomeProject() bool { return l.Project == HomeProject }

func (l Layout) SettingsFile() string {
	return filepath.Join(l.Config, "ForkMesh", "ForkMesh.conf")
}

func (l Layout) Diagnostics() string { return filepath.Join(l.Logs, "diagnostics") }

func (l Layout) IdentityKey() string {
	return filepath.Join(l.Data, "identity", "ed25519.pem")
}

func (l Layout) Source() string { return filepath.Join(l.HomeDir, "src") }

func (l Layout) CoreState() string { return filepath.Join(l.State, "core") }

func (l Layout) CoreLogs() string { return filepath.Join(l.Logs, "core") }

type Env func(string) string

func env(get Env, name string) string { return strings.TrimSpace(get(name)) }

func xdg(get Env, variable, home, fallback string) string {
	if value := env(get, variable); value != "" {
		return value
	}
	return filepath.Join(home, fallback)
}

func xdgSandbox(get Env, home string) bool {
	defaults := [][2]string{
		{"XDG_DATA_HOME", ".local/share"},
		{"XDG_CONFIG_HOME", ".config"},
		{"XDG_CACHE_HOME", ".cache"},
	}
	for _, pair := range defaults {
		value := env(get, pair[0])
		if value != "" && filepath.Clean(value) != filepath.Clean(filepath.Join(home, pair[1])) {
			return true
		}
	}
	return false
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func real(path string) bool {
	info, err := os.Lstat(path)
	if err != nil {
		return false
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return false
	}
	return true
}

func pick(modern, legacy string, hasLegacy, allowed bool, probe, modernProbe string) string {
	if hasLegacy && !allowed {
		return legacy
	}
	check := modern
	if modernProbe != "" {
		check = modernProbe
	}
	if exists(check) {
		return modern
	}
	if hasLegacy {
		target := legacy
		if probe != "" {
			target = probe
		}
		if real(target) {
			return legacy
		}
	}
	return modern
}

func Resolve(project string, get Env, home string) Layout {
	if get == nil {
		get = os.Getenv
	}
	if home == "" {
		home, _ = os.UserHomeDir()
	}
	projectsRoot := env(get, "FORKMESH_PROJECTS_DIR")
	if projectsRoot == "" {
		projectsRoot = filepath.Join(home, "projects")
	}
	homeDir := env(get, "FORKMESH_HOME")
	if homeDir == "" {
		homeDir = filepath.Join(projectsRoot, HomeProject)
	}
	slug := strings.TrimSpace(project)
	if slug == "" {
		slug = env(get, "FORKMESH_PROJECT")
	}
	slug = strings.ToLower(strings.TrimSpace(slug))
	if !IsValidSlug(slug) {
		slug = HomeProject
	}
	isHome := slug == HomeProject
	projectDir := homeDir
	if !isHome {
		projectDir = filepath.Join(projectsRoot, slug)
	}

	legacyData := filepath.Join(xdg(get, "XDG_DATA_HOME", home, ".local/share"), "ForkMesh", "ForkMesh")
	legacyConfig := filepath.Join(xdg(get, "XDG_CONFIG_HOME", home, ".config"), "ForkMesh")
	legacyCache := filepath.Join(xdg(get, "XDG_CACHE_HOME", home, ".cache"), "ForkMesh", "ForkMesh")
	legacyState := filepath.Join(home, ".forkmesh")

	blocked := exists(filepath.Join(projectDir, ".git"))
	reason := ""
	if blocked {
		reason = projectDir + " is still a git checkout. Move the checkout into " + projectDir +
			"/src (tools/forkmesh_layout.py) before application data can live beside it."
	} else if isHome && xdgSandbox(get, home) && env(get, "FORKMESH_HOME") == "" && env(get, "FORKMESH_PROJECTS_DIR") == "" {
		blocked = true
		reason = "XDG_DATA_HOME, XDG_CONFIG_HOME or XDG_CACHE_HOME points away from its default, so this run keeps its files there. Set FORKMESH_HOME to place the projects layout explicitly."
	}
	allowed := !blocked || !isHome

	legacyProbe := func(path string) string {
		if isHome {
			return path
		}
		return ""
	}

	data := pick(filepath.Join(projectDir, "data"), legacyData, isHome, allowed, "", "")
	config := pick(filepath.Join(projectDir, "config"), filepath.Dir(legacyConfig), isHome, allowed,
		legacyProbe(legacyConfig), filepath.Join(projectDir, "config", "ForkMesh"))
	cache := pick(filepath.Join(projectDir, "cache"), legacyCache, isHome, allowed, "", "")
	state := pick(filepath.Join(projectDir, "state"), legacyState, isHome, allowed,
		legacyProbe(filepath.Join(legacyState, "forkmesh.lock")), "")
	logs := pick(filepath.Join(projectDir, "logs"), legacyState, isHome, allowed,
		legacyProbe(filepath.Join(legacyState, "diagnostics")), "")
	ssh := pick(filepath.Join(projectDir, "ssh"), filepath.Join(legacyData, "ssh"), isHome, allowed, "", "")

	spawnedFor := env(get, "FORKMESH_PROJECT")
	if spawnedFor == "" {
		spawnedFor = HomeProject
	}
	if spawnedFor == slug {
		if value := env(get, "FORKMESH_DATA_DIR"); value != "" {
			data = value
		}
		if value := env(get, "FORKMESH_CONFIG_DIR"); value != "" {
			config = value
		}
	}

	return Layout{
		ProjectsRoot: projectsRoot, HomeDir: homeDir, Project: slug, ProjectDir: projectDir,
		RerootAllowed: !blocked, RerootBlockedReason: reason,
		Data: data, Config: config, Cache: cache, State: state, Logs: logs, SSH: ssh,
		LegacyData: legacyData, LegacyConfig: legacyConfig, LegacyCache: legacyCache, LegacyState: legacyState,
	}
}
