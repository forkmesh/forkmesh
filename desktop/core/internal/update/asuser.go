package update

import (
	"os"
	"path/filepath"
	"strings"

	"forkmesh.com/desktop-core/internal/safedelete"
)

func shellSingleQuote(value string) string {
	return "'" + strings.ReplaceAll(value, "'", `'\''`) + "'"
}

func absoluteDir(path string) string {
	if absolute, err := filepath.Abs(path); err == nil {
		path = absolute
	}
	return filepath.Dir(path)
}

func asUserInstallScript(built, appPath string, darwin bool) string {
	binDir := absoluteDir(appPath)
	app := shellSingleQuote(appPath)
	script := "mkdir -p " + shellSingleQuote(binDir) + " && { [ ! -e " + app + " ] || cp -f " + app + " " + app +
		".bak-update; } && cp -f " + shellSingleQuote(built) + " " + app + " && chmod 0755 " + app
	if companion := builtMirrorNodeCompanionPath(built, darwin); companion != "" {
		target := shellSingleQuote(filepath.Join(binDir, "forkmesh-mirror-node"))
		script += " && { { [ ! -e " + target + " ] || mv -f " + target + " " + target + ".bak-update; } && cp -f " +
			shellSingleQuote(companion) + " " + target + " && chmod 0755 " + target + " || true; }"
	}
	return script
}

func (j *job) installAsUser() outcome {
	built := j.built
	appPath := j.req.AppPath
	if appPath == "" {
		appPath = built
		j.req.AppPath = built
	}
	script := asUserInstallScript(built, appPath, j.env.Darwin)
	j.out.status("Installing for "+j.req.AsUser+"...", false)
	if !j.startCheck(built) {
		if j.cancelled() {
			return outcomeCancelled
		}
		j.out.status("Update failed: the new build did not pass its start check; keeping the current version.", true)
		return outcomeFailed
	}
	if result := j.step("sudo", []string{"-u", j.req.AsUser, "-H", "sh", "-c", script}, os.TempDir(), nil); !result.ok {
		return j.failWith(result)
	}
	if j.cancelled() {
		return outcomeCancelled
	}
	j.out.percent(100)
	j.out.status("Relaunching...", false)
	return outcomeInstalled
}

func (j *job) clean() outcome {
	clientDir := j.req.ClientDir
	target := clientDir + "/build"
	j.out.phase("clearing the build cache", 0)
	j.out.status("Clearing build cache...", false)
	if reason := safedelete.Refusal(clientDir, target); reason != "" {
		j.out.status("Could not clear the build cache: "+reason, true)
		return outcomeFailed
	}
	if err := safedelete.RemoveTreeUnder(clientDir, target); err != nil {
		j.out.restartLog("could not clear the build cache completely: " + err.Error())
	}
	if j.cancelled() {
		return outcomeCancelled
	}
	return outcomeInstalled
}
