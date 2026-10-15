package autostart

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/paths"
)

const (
	MechanismSystemd     = "systemd-user"
	MechanismXDG         = "xdg-autostart"
	MechanismLaunchAgent = "launch-agent"
	MechanismRunKey      = "run-key"
	runKey               = `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
)

var (
	ErrBadProject = errors.New("autostart: invalid project slug")
	ErrBadBinary  = errors.New("autostart: binary must be an absolute path to an executable")
)

type Status struct {
	Enabled   bool   `json:"enabled"`
	Mechanism string `json:"mechanism"`
	Location  string `json:"location"`
	Binary    string `json:"binary,omitempty"`
	Project   string `json:"project"`
	AtBoot    bool   `json:"atBoot"`
}

type Manager struct {
	GOOS     string
	Home     string
	Env      func(string) string
	Run      func(name string, args ...string) ([]byte, error)
	LookPath func(string) (string, error)
	UID      int
}

func System() Manager {
	home, _ := os.UserHomeDir()
	return Manager{GOOS: runtime.GOOS, Home: home, Env: os.Getenv, UID: os.Getuid(),
		LookPath: exec.LookPath,
		Run: func(name string, args ...string) ([]byte, error) {
			return exec.Command(name, args...).CombinedOutput()
		}}
}

func (m Manager) env(name string) string {
	if m.Env == nil {
		return ""
	}
	return strings.TrimSpace(m.Env(name))
}

func (m Manager) configHome() string {
	if value := m.env("XDG_CONFIG_HOME"); filepath.IsAbs(value) {
		return value
	}
	return filepath.Join(m.Home, ".config")
}

func Label(project string) string { return "forkmesh-core-" + project }

func (m Manager) systemdUnit(project string) string {
	return filepath.Join(m.configHome(), "systemd", "user", Label(project)+".service")
}

func (m Manager) xdgEntry(project string) string {
	return filepath.Join(m.configHome(), "autostart", Label(project)+".desktop")
}

func (m Manager) lingerMarker() string {
	return filepath.Join(m.configHome(), "systemd", "user", "forkmesh-core.linger")
}

func (m Manager) launchAgent(project string) string {
	return filepath.Join(m.Home, "Library", "LaunchAgents", "com.forkmesh.core."+project+".plist")
}

func (m Manager) hasSystemd() bool {
	if m.LookPath == nil || m.Run == nil {
		return false
	}
	if _, err := m.LookPath("systemctl"); err != nil {
		return false
	}
	_, err := m.Run("systemctl", "--user", "show-environment")
	return err == nil
}

func (m Manager) lingering() bool {
	if m.LookPath == nil || m.Run == nil {
		return false
	}
	if _, err := m.LookPath("loginctl"); err != nil {
		return false
	}
	out, err := m.Run("loginctl", "show-user", strconv.Itoa(m.UID), "--property=Linger", "--value")
	return err == nil && strings.TrimSpace(string(out)) == "yes"
}

func (m Manager) enableLinger() {
	if m.lingering() {
		return
	}
	if _, err := m.LookPath("loginctl"); err != nil {
		return
	}
	if _, err := m.Run("loginctl", "--no-ask-password", "enable-linger", strconv.Itoa(m.UID)); err != nil {
		return
	}
	_ = writeFile(m.lingerMarker(), "")
}

func (m Manager) releaseLinger() {
	if _, err := os.Stat(m.lingerMarker()); err != nil {
		return
	}
	if units, _ := filepath.Glob(filepath.Join(filepath.Dir(m.lingerMarker()), "forkmesh-core-*.service")); len(units) > 0 {
		return
	}
	if m.Run != nil {
		if _, err := m.Run("loginctl", "--no-ask-password", "disable-linger", strconv.Itoa(m.UID)); err != nil {
			return
		}
	}
	_ = removeFile(m.lingerMarker())
}

func Arguments(project string) []string {
	return []string{"serve", "--project", project, "--background"}
}

func validate(binary, project string) error {
	if !paths.IsValidSlug(project) {
		return ErrBadProject
	}
	if !filepath.IsAbs(binary) || strings.ContainsAny(binary, "\n\r\x00") {
		return ErrBadBinary
	}
	info, err := os.Stat(binary)
	if err != nil || info.IsDir() {
		return ErrBadBinary
	}
	return nil
}

func (m Manager) Status(project string) Status {
	status := Status{Project: project}
	switch m.GOOS {
	case "darwin":
		status.Mechanism, status.Location = MechanismLaunchAgent, m.launchAgent(project)
		status.Enabled, status.Binary = fileBinary(status.Location, plistBinary)
	case "windows":
		status.Mechanism, status.Location = MechanismRunKey, runKey+`\`+Label(project)
		if m.Run != nil {
			if out, err := m.Run("reg", "query", runKey, "/v", Label(project)); err == nil {
				status.Enabled = true
				status.Binary = regBinary(string(out))
			}
		}
	default:
		unit, entry := m.systemdUnit(project), m.xdgEntry(project)
		if enabled, binary := fileBinary(unit, execStartBinary); enabled {
			return Status{Enabled: true, Mechanism: MechanismSystemd, Location: unit, Binary: binary, Project: project,
				AtBoot: m.lingering()}
		}
		if enabled, binary := fileBinary(entry, desktopExecBinary); enabled {
			return Status{Enabled: true, Mechanism: MechanismXDG, Location: entry, Binary: binary, Project: project}
		}
		if m.hasSystemd() {
			status.Mechanism, status.Location = MechanismSystemd, unit
		} else {
			status.Mechanism, status.Location = MechanismXDG, entry
		}
	}
	return status
}

func (m Manager) Enable(binary, project string) (Status, error) {
	if err := validate(binary, project); err != nil {
		return m.Status(project), err
	}
	switch m.GOOS {
	case "darwin":
		path := m.launchAgent(project)
		if err := writeFile(path, LaunchAgentPlist(binary, project, m.Home)); err != nil {
			return m.Status(project), err
		}
		if m.Run != nil {
			target := "gui/" + strconv.Itoa(m.UID)
			_, _ = m.Run("launchctl", "bootout", target+"/com.forkmesh.core."+project)
			_, _ = m.Run("launchctl", "bootstrap", target, path)
		}
	case "windows":
		if m.Run == nil {
			return m.Status(project), errors.New("autostart: no command runner")
		}
		if out, err := m.Run("reg", "add", runKey, "/v", Label(project), "/t", "REG_SZ", "/d",
			WindowsCommand(binary, project), "/f"); err != nil {
			return m.Status(project), fmt.Errorf("autostart: reg add: %v: %s", err, strings.TrimSpace(string(out)))
		}
	default:
		if m.hasSystemd() {
			if err := writeFile(m.systemdUnit(project), SystemdUnit(binary, project)); err != nil {
				return m.Status(project), err
			}
			_ = removeFile(m.xdgEntry(project))
			if _, err := m.Run("systemctl", "--user", "daemon-reload"); err != nil {
				return m.Status(project), fmt.Errorf("autostart: systemctl daemon-reload: %v", err)
			}
			if out, err := m.Run("systemctl", "--user", "enable", Label(project)+".service"); err != nil {
				return m.Status(project), fmt.Errorf("autostart: systemctl enable: %v: %s", err, strings.TrimSpace(string(out)))
			}
			m.enableLinger()
		} else if err := writeFile(m.xdgEntry(project), DesktopEntry(binary, project)); err != nil {
			return m.Status(project), err
		}
	}
	return m.Status(project), nil
}

func (m Manager) Disable(project string) (Status, error) {
	if !paths.IsValidSlug(project) {
		return Status{Project: project}, ErrBadProject
	}
	var failure error
	switch m.GOOS {
	case "darwin":
		if m.Run != nil {
			_, _ = m.Run("launchctl", "bootout", "gui/"+strconv.Itoa(m.UID)+"/com.forkmesh.core."+project)
		}
		failure = removeFile(m.launchAgent(project))
	case "windows":
		if m.Run != nil {
			if _, err := m.Run("reg", "query", runKey, "/v", Label(project)); err == nil {
				if out, err := m.Run("reg", "delete", runKey, "/v", Label(project), "/f"); err != nil {
					failure = fmt.Errorf("autostart: reg delete: %v: %s", err, strings.TrimSpace(string(out)))
				}
			}
		}
	default:
		unit := m.systemdUnit(project)
		if _, err := os.Stat(unit); err == nil {
			if m.Run != nil {
				_, _ = m.Run("systemctl", "--user", "disable", Label(project)+".service")
			}
			failure = removeFile(unit)
			if m.Run != nil {
				_, _ = m.Run("systemctl", "--user", "daemon-reload")
			}
		}
		m.releaseLinger()
		if err := removeFile(m.xdgEntry(project)); err != nil && failure == nil {
			failure = err
		}
	}
	return m.Status(project), failure
}

func writeFile(path, content string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	temp := path + ".tmp"
	if err := os.WriteFile(temp, []byte(content), 0o644); err != nil {
		return err
	}
	return os.Rename(temp, path)
}

func removeFile(path string) error {
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

func fileBinary(path string, parse func(string) string) (bool, string) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return false, ""
	}
	return true, parse(string(raw))
}

func systemdQuote(value string) string {
	value = strings.ReplaceAll(value, `\`, `\\`)
	value = strings.ReplaceAll(value, `"`, `\"`)
	value = strings.ReplaceAll(value, "%", "%%")
	value = strings.ReplaceAll(value, "$", "$$")
	return `"` + value + `"`
}

func SystemdUnit(binary, project string) string {
	command := []string{systemdQuote(binary)}
	for _, argument := range Arguments(project) {
		command = append(command, systemdQuote(argument))
	}
	return strings.Join([]string{
		"[Unit]",
		"Description=ForkMesh background engine (" + project + ")",
		"After=network-online.target",
		"StartLimitIntervalSec=600",
		"StartLimitBurst=5",
		"",
		"[Service]",
		"Type=simple",
		"ExecStart=" + strings.Join(command, " "),
		"Restart=on-failure",
		"RestartSec=5",
		"KillMode=process",
		"TimeoutStopSec=20",
		"Nice=5",
		"IOSchedulingClass=idle",
		"NoNewPrivileges=yes",
		"UnsetEnvironment=FORKMESH_HOME FORKMESH_PROJECTS_DIR FORKMESH_PROJECT FORKMESH_PROJECT_DIR",
		"",
		"[Install]",
		"WantedBy=default.target",
		"",
	}, "\n")
}

func execStartBinary(unit string) string {
	for _, line := range strings.Split(unit, "\n") {
		if rest, ok := strings.CutPrefix(line, "ExecStart="); ok {
			rest = strings.TrimSpace(rest)
			if strings.HasPrefix(rest, `"`) {
				var out strings.Builder
				for i := 1; i < len(rest); i++ {
					switch {
					case rest[i] == '\\' && i+1 < len(rest):
						i++
						out.WriteByte(rest[i])
					case rest[i] == '%' && i+1 < len(rest) && rest[i+1] == '%':
						i++
						out.WriteByte('%')
					case rest[i] == '$' && i+1 < len(rest) && rest[i+1] == '$':
						i++
						out.WriteByte('$')
					case rest[i] == '"':
						return out.String()
					default:
						out.WriteByte(rest[i])
					}
				}
			}
			return strings.Fields(rest)[0]
		}
	}
	return ""
}

func desktopQuote(value string) string {
	value = strings.ReplaceAll(value, `\`, `\\\\`)
	for _, special := range []string{`"`, "`", "$"} {
		value = strings.ReplaceAll(value, special, `\\`+special)
	}
	value = strings.ReplaceAll(value, "%", "%%")
	return `"` + value + `"`
}

func DesktopEntry(binary, project string) string {
	command := []string{desktopQuote(binary)}
	command = append(command, Arguments(project)...)
	return strings.Join([]string{
		"[Desktop Entry]",
		"Type=Application",
		"Name=ForkMesh background engine (" + project + ")",
		"Exec=" + strings.Join(command, " "),
		"NoDisplay=true",
		"Terminal=false",
		"X-GNOME-Autostart-enabled=true",
		"X-GNOME-Autostart-Delay=5",
		"",
	}, "\n")
}

func desktopExecBinary(entry string) string {
	for _, line := range strings.Split(entry, "\n") {
		if rest, ok := strings.CutPrefix(line, "Exec="); ok {
			if strings.HasPrefix(rest, `"`) {
				var out strings.Builder
				for i := 1; i < len(rest); i++ {
					switch {
					case strings.HasPrefix(rest[i:], `\\\\`):
						out.WriteByte('\\')
						i += 3
					case strings.HasPrefix(rest[i:], `\\`) && i+2 < len(rest):
						out.WriteByte(rest[i+2])
						i += 2
					case strings.HasPrefix(rest[i:], "%%"):
						out.WriteByte('%')
						i++
					case rest[i] == '"':
						return out.String()
					default:
						out.WriteByte(rest[i])
					}
				}
			}
			return strings.Fields(rest)[0]
		}
	}
	return ""
}

func xmlEscape(value string) string {
	return strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", `"`, "&quot;", "'", "&apos;").Replace(value)
}

func LaunchAgentPlist(binary, project, home string) string {
	var arguments strings.Builder
	for _, value := range append([]string{binary}, Arguments(project)...) {
		arguments.WriteString("\t\t<string>" + xmlEscape(value) + "</string>\n")
	}
	return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.forkmesh.core.` + xmlEscape(project) + `</string>
	<key>ProgramArguments</key>
	<array>
` + arguments.String() + `	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<dict>
		<key>SuccessfulExit</key>
		<false/>
	</dict>
	<key>ThrottleInterval</key>
	<integer>10</integer>
	<key>ProcessType</key>
	<string>Background</string>
	<key>LowPriorityIO</key>
	<true/>
	<key>AbandonProcessGroup</key>
	<true/>
	<key>WorkingDirectory</key>
	<string>` + xmlEscape(home) + `</string>
	<key>StandardOutPath</key>
	<string>/dev/null</string>
	<key>StandardErrorPath</key>
	<string>/dev/null</string>
</dict>
</plist>
`
}

func plistBinary(plist string) string {
	_, after, ok := strings.Cut(plist, "<key>ProgramArguments</key>")
	if !ok {
		return ""
	}
	_, after, ok = strings.Cut(after, "<string>")
	if !ok {
		return ""
	}
	value, _, _ := strings.Cut(after, "</string>")
	return strings.NewReplacer("&lt;", "<", "&gt;", ">", "&quot;", `"`, "&apos;", "'", "&amp;", "&").Replace(value)
}

func WindowsCommand(binary, project string) string {
	return `"` + binary + `" ` + strings.Join(append(Arguments(project), "--hide-console"), " ")
}

func regBinary(output string) string {
	_, after, ok := strings.Cut(output, "REG_SZ")
	if !ok {
		return ""
	}
	after = strings.TrimSpace(after)
	if strings.HasPrefix(after, `"`) {
		value, _, _ := strings.Cut(after[1:], `"`)
		return value
	}
	fields := strings.Fields(after)
	if len(fields) == 0 {
		return ""
	}
	return fields[0]
}
