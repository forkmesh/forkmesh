package mirrornode

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// --preflight is the read-only dry run an operator (or an install, update, or
// monitoring script) runs before and after changing a node: configuration and
// identity consistency, runtime and Git compatibility, disk capacity, and
// whether backups and restore drills are meeting their objectives. It never
// changes the node, and exits non-zero only for conditions that stop the
// service from working.
const (
	minimumGitMajor              = 2
	minimumGitMinor              = 27
	defaultBackupMaxAge          = 24 * time.Hour
	defaultRestoreDrillMaxAge    = 92 * 24 * time.Hour
	preflightMinimumFreeBytes    = 512 << 20
	preflightBackupHeadroomBytes = 1 << 30
)

var gitVersionPattern = regexp.MustCompile(`git version ([0-9]+)\.([0-9]+)`)

type volumeUsage struct {
	FreeBytes   uint64
	TotalBytes  uint64
	FreeInodes  uint64
	TotalInodes uint64
}

type PreflightOptions struct {
	ConfigPath string
	// BackupMaxAge is the recovery point objective; RestoreDrillMaxAge is how
	// often a restore drill must prove the backups.
	BackupMaxAge       time.Duration
	RestoreDrillMaxAge time.Duration
	Now                func() time.Time
}

type PreflightCheck struct {
	Name   string `json:"name"`
	Status string `json:"status"`
	Detail string `json:"detail"`
}

type PreflightReport struct {
	OK          bool             `json:"ok"`
	Errors      int              `json:"errors"`
	Warnings    int              `json:"warnings"`
	GeneratedAt string           `json:"generatedAt"`
	Checks      []PreflightCheck `json:"checks"`
}

func (r *PreflightReport) add(name, status, detail string) {
	r.Checks = append(r.Checks, PreflightCheck{Name: name, Status: status, Detail: detail})
	switch status {
	case "error":
		r.Errors++
	case "warn":
		r.Warnings++
	}
	r.OK = r.Errors == 0
}

func (r PreflightReport) Text() string {
	var out strings.Builder
	for _, check := range r.Checks {
		fmt.Fprintf(&out, "[%-5s] %s: %s\n", check.Status, check.Name, check.Detail)
	}
	fmt.Fprintf(&out, "%d error(s), %d warning(s)\n", r.Errors, r.Warnings)
	return out.String()
}

func RunNodePreflight(ctx context.Context, options PreflightOptions) PreflightReport {
	now := time.Now
	if options.Now != nil {
		now = options.Now
	}
	report := PreflightReport{OK: true, GeneratedAt: now().UTC().Format(time.RFC3339)}
	cfg, err := LoadConfig(options.ConfigPath)
	if err != nil {
		report.add("config", "error", err.Error())
		return report
	}
	report.add("config", "ok", fmt.Sprintf("schema %d at %s", cfg.SchemaVersion, options.ConfigPath))
	gateway, _, err := loadGatewayConfig(cfg.GatewayConfig)
	if err != nil {
		report.add("gateway-config", "error", err.Error())
		return report
	}
	report.add("gateway-config", "ok", fmt.Sprintf("node %s serving %d repositories", gateway.Node.Name, len(gateway.Repositories)))
	if err := ValidateRuntime(cfg); err != nil {
		report.add("runtime", "error", err.Error())
	} else {
		report.add("runtime", "ok", "gateway script, Python, identity, and git are present")
	}
	preflightGit(ctx, &report)
	switch {
	case cfg.Version == "dev":
		report.add("version", "warn", "this node reports version dev, so release compatibility cannot be checked; install a release build")
	case releaseVersionPattern.MatchString(cfg.Version):
		report.add("version", "ok", "release "+cfg.Version)
	default:
		report.add("version", "warn", fmt.Sprintf("version %q is not an exact release version", cfg.Version))
	}
	identity, err := LoadIdentity(cfg.IdentityKey)
	switch {
	case err != nil:
		report.add("identity", "error", err.Error())
	case identity.PublicKey() != gateway.Node.PublicKey:
		report.add("identity", "error", "the identity key does not match the gateway node public key; the service will refuse to start")
	default:
		report.add("identity", "ok", "public key "+identity.PublicKey())
	}
	if !cfg.DisableCloudflared {
		info, err := os.Lstat(cfg.ConnectorToken)
		switch {
		case errors.Is(err, fs.ErrNotExist):
			report.add("connector-token", "warn", "no connector token yet; tunnel provisioning writes it before the service starts")
		case err != nil || !info.Mode().IsRegular():
			report.add("connector-token", "error", "the connector token is not a regular file")
		case info.Mode().Perm()&0077 != 0:
			report.add("connector-token", "error", "the connector token must be owner-only")
		default:
			report.add("connector-token", "ok", "present and owner-only")
		}
	}
	preflightCapacity(cfg, gateway, &report)
	preflightRecovery(cfg, options, now(), &report)
	return report
}

func preflightGit(ctx context.Context, report *PreflightReport) {
	out, err := exec.CommandContext(ctx, "git", "version").Output()
	if err != nil {
		report.add("git", "error", "git is unavailable")
		return
	}
	match := gitVersionPattern.FindStringSubmatch(string(out))
	if match == nil {
		report.add("git", "warn", "could not parse "+strings.TrimSpace(string(out)))
		return
	}
	major, _ := strconv.Atoi(match[1])
	minor, _ := strconv.Atoi(match[2])
	if major < minimumGitMajor || (major == minimumGitMajor && minor < minimumGitMinor) {
		report.add("git", "error", fmt.Sprintf("git %d.%d is older than %d.%d, which atomic merges and restores need", major, minor, minimumGitMajor, minimumGitMinor))
		return
	}
	report.add("git", "ok", strings.TrimSpace(string(out)))
}

func preflightCapacity(cfg Config, gateway GatewayConfig, report *PreflightReport) {
	var repositoryBytes int64
	locations := []string{filepath.Dir(cfg.GatewayConfig)}
	for _, repository := range gateway.Repositories {
		size, err := directoryBytes(repository.GitDir)
		switch {
		case errors.Is(err, fs.ErrNotExist):
			report.add("repository "+repository.Owner+"/"+repository.Name, "warn", "no local clone yet at "+repository.GitDir+"; the first sync creates it")
		case err != nil:
			report.add("repository "+repository.Owner+"/"+repository.Name, "warn", err.Error())
		default:
			repositoryBytes += size
		}
		locations = append(locations, repository.GitDir)
	}
	required := uint64(max(preflightBackupHeadroomBytes, repositoryBytes))
	seen := make(map[string]bool)
	for _, location := range locations {
		location = existingAncestor(location)
		if seen[location] {
			continue
		}
		seen[location] = true
		usage, err := volumeUsageFor(location)
		name := "capacity " + location
		if err != nil {
			report.add(name, "warn", err.Error())
			continue
		}
		switch {
		case usage.FreeBytes < preflightMinimumFreeBytes:
			report.add(name, "error", fmt.Sprintf("%s free of %s; Git writes and gateway reloads will fail", formatBytes(usage.FreeBytes), formatBytes(usage.TotalBytes)))
		case usage.FreeBytes < required:
			report.add(name, "warn", fmt.Sprintf("%s free, but backups and repacks need at least %s (the repositories hold %s)", formatBytes(usage.FreeBytes), formatBytes(required), formatBytes(uint64(repositoryBytes))))
		case usage.TotalBytes > 0 && usage.FreeBytes < usage.TotalBytes/10:
			report.add(name, "warn", fmt.Sprintf("%s free, under 10%% of %s", formatBytes(usage.FreeBytes), formatBytes(usage.TotalBytes)))
		default:
			report.add(name, "ok", fmt.Sprintf("%s free of %s", formatBytes(usage.FreeBytes), formatBytes(usage.TotalBytes)))
		}
		if usage.TotalInodes > 0 && usage.FreeInodes < usage.TotalInodes/20 {
			report.add("inodes "+location, "warn", fmt.Sprintf("%d of %d inodes free", usage.FreeInodes, usage.TotalInodes))
		}
	}
}

func preflightRecovery(cfg Config, options PreflightOptions, now time.Time, report *PreflightReport) {
	backupMaxAge := options.BackupMaxAge
	if backupMaxAge <= 0 {
		backupMaxAge = defaultBackupMaxAge
	}
	drillMaxAge := options.RestoreDrillMaxAge
	if drillMaxAge <= 0 {
		drillMaxAge = defaultRestoreDrillMaxAge
	}
	status := ReadRecoveryStatus(cfg.GatewayConfig, now)
	backupAge := time.Duration(status.LastBackupAgeSeconds) * time.Second
	switch {
	case status.LastBackupAt == "":
		report.add("backup", "warn", "no backup has been recorded; run --backup")
	case backupAge > backupMaxAge:
		report.add("backup", "warn", fmt.Sprintf("the last backup is %s old, beyond the %s recovery point objective", formatAge(backupAge), formatAge(backupMaxAge)))
	default:
		report.add("backup", "ok", fmt.Sprintf("last backup %s ago (%s)", formatAge(backupAge), formatBytes(uint64(status.LastBackupBytes))))
	}
	drillAge := time.Duration(status.LastRestoreDrillAgeSeconds) * time.Second
	drillDuration := time.Duration(status.LastRestoreDrillDurationMillis) * time.Millisecond
	objective := time.Duration(status.RecoveryTimeObjectiveMillis) * time.Millisecond
	switch {
	case status.LastRestoreDrillAt == "":
		report.add("restore-drill", "warn", "no restore drill has been recorded; run --restore-drill against a recent backup")
	case !status.LastRestoreDrillWithinObjective:
		report.add("restore-drill", "warn", fmt.Sprintf("the last restore drill took %s, beyond its %s objective", formatAge(drillDuration), formatAge(objective)))
	case drillAge > drillMaxAge:
		report.add("restore-drill", "warn", fmt.Sprintf("the last restore drill was %s ago; one is due every %s", formatAge(drillAge), formatAge(drillMaxAge)))
	default:
		report.add("restore-drill", "ok", fmt.Sprintf("last drill restored in %s (objective %s), %s ago", formatAge(drillDuration), formatAge(objective), formatAge(drillAge)))
	}
}

// RecordRestoreDrill stores a drill result for /v1/status and --preflight.
func RecordRestoreDrill(cfg Config, report NodeRestoreReport) error {
	return RecordNodeOperation(cfg, lastRestoreDrillRecordName, report)
}

func directoryBytes(root string) (int64, error) {
	var total int64
	err := filepath.WalkDir(root, func(_ string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.Type().IsRegular() {
			if info, infoErr := entry.Info(); infoErr == nil {
				total += info.Size()
			}
		}
		return nil
	})
	return total, err
}

func existingAncestor(path string) string {
	for {
		if _, err := os.Stat(path); err == nil {
			return path
		}
		parent := filepath.Dir(path)
		if parent == path {
			return path
		}
		path = parent
	}
}

func formatBytes(value uint64) string {
	const unit = 1024
	if value < unit {
		return fmt.Sprintf("%d B", value)
	}
	divisor, exponent := uint64(unit), 0
	for next := value / unit; next >= unit; next /= unit {
		divisor *= unit
		exponent++
	}
	return fmt.Sprintf("%.1f %ciB", float64(value)/float64(divisor), "KMGTPE"[exponent])
}

func formatAge(duration time.Duration) string {
	switch {
	case duration >= 48*time.Hour:
		return fmt.Sprintf("%dd", int(duration.Hours()/24))
	case duration >= 2*time.Hour:
		return fmt.Sprintf("%dh", int(duration.Hours()))
	default:
		return duration.Round(time.Second).String()
	}
}
