package mirrornode

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Node backups capture everything a node's configuration names — the service
// and gateway configuration, the identity key, the connector token, every
// repository, release store, fork registry, dashboard replica, and operator
// policy state — as one sealed archive. Repositories travel as `git bundle
// --all` snapshots so a backup taken while the service keeps serving is still
// internally consistent; their non-object state (HEAD, config, hooks, release
// stores) is archived beside the bundle. A restore never overwrites an
// existing file, verifies every byte, ref, and key against the manifest, and
// removes what it created when any check fails.
const (
	nodeBackupSchema             = "forkmesh-node-backup-v1"
	nodeOpsDirectoryName         = "node-ops"
	lastBackupRecordName         = "last-backup.json"
	lastRestoreDrillRecordName   = "last-restore-drill.json"
	backupManifestEntry          = "manifest.json"
	backupStatePrefix            = "state"
	backupBundlePrefix           = "bundles/"
	defaultRecoveryTimeObjective = 30 * time.Minute
	maximumBackupManifestBytes   = 64 << 20
)

var (
	backupBundleNamePattern = regexp.MustCompile(`^[0-9]{4,9}\.bundle$`)
	// Gateway configuration keys whose string values name node state.
	backupPathKeys = map[string]bool{
		"gitDir": true, "releaseStore": true, "manifestPath": true,
		"actionsSummaryPath": true, "privateReplicaStore": true,
		"dynamicRepositoriesPath": true, "replicaPath": true,
		"writableRepositoryRoot": true, "policyPath": true,
		"stateDirectory": true,
	}
	// Scratch and in-flight temporary names written by the daemon, the
	// gateway, and these operations; none is recoverable state.
	backupSkippedPrefixes = []string{
		".apply-", ".gateway-config-", ".config-", ".identity-",
		".post-receive-", ".forkmesh-sealed-", ".forkmesh-backup-scratch-",
		".forkmesh-restore-",
	}
)

type nodeBackupManifest struct {
	Schema        string             `json:"schema"`
	CreatedAt     string             `json:"createdAt"`
	Node          string             `json:"node"`
	PublicKey     string             `json:"publicKey"`
	Version       string             `json:"version"`
	ConfigPath    string             `json:"configPath"`
	GatewayConfig string             `json:"gatewayConfig"`
	IdentityKey   string             `json:"identityKey"`
	Roots         []string           `json:"roots"`
	Directories   []backupDirectory  `json:"directories"`
	Files         []backupFile       `json:"files"`
	Repositories  []backupRepository `json:"repositories"`
}

type backupDirectory struct {
	Path string `json:"path"`
	Mode uint32 `json:"mode"`
}

type backupFile struct {
	Path   string `json:"path"`
	Mode   uint32 `json:"mode"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type backupRepository struct {
	GitDir       string `json:"gitDir"`
	Bundle       string `json:"bundle,omitempty"`
	BundleSHA256 string `json:"bundleSha256,omitempty"`
	Refs         int    `json:"refs"`
	RefsSHA256   string `json:"refsSha256"`
}

type NodeBackupOptions struct {
	ConfigPath string
	Output     string
	Passphrase []byte
	Now        func() time.Time
}

type NodeBackupReport struct {
	Archive        string `json:"archive"`
	Bytes          int64  `json:"bytes"`
	SHA256         string `json:"sha256"`
	Node           string `json:"node"`
	PublicKey      string `json:"publicKey"`
	Version        string `json:"version"`
	Files          int    `json:"files"`
	Repositories   int    `json:"repositories"`
	Refs           int    `json:"refs"`
	CreatedAt      string `json:"createdAt"`
	DurationMillis int64  `json:"durationMs"`
}

// CreateNodeBackup writes a sealed backup of the node at options.ConfigPath
// and records it for /v1/status and --preflight.
func CreateNodeBackup(ctx context.Context, options NodeBackupOptions) (NodeBackupReport, error) {
	now := time.Now
	if options.Now != nil {
		now = options.Now
	}
	started := now()
	var report NodeBackupReport
	if !filepath.IsAbs(options.Output) || !filepath.IsAbs(options.ConfigPath) {
		return report, errors.New("backup configuration and output paths must be absolute")
	}
	if _, err := os.Lstat(options.Output); err == nil {
		return report, fmt.Errorf("refusing to overwrite existing %s", options.Output)
	}
	cfg, err := LoadConfig(options.ConfigPath)
	if err != nil {
		return report, err
	}
	gateway, gatewayRaw, err := loadGatewayConfig(cfg.GatewayConfig)
	if err != nil {
		return report, fmt.Errorf("gateway config: %w", err)
	}
	identity, err := LoadIdentity(cfg.IdentityKey)
	if err != nil {
		return report, err
	}
	if identity.PublicKey() != gateway.Node.PublicKey {
		return report, errors.New("the identity key does not match the gateway node public key; refusing to back up a split identity")
	}
	roots, err := nodeBackupRoots(options.ConfigPath, cfg, gatewayRaw)
	if err != nil {
		return report, err
	}
	scratch, err := os.MkdirTemp(filepath.Dir(options.Output), ".forkmesh-backup-scratch-")
	if err != nil {
		return report, err
	}
	defer removeTreeUnder(filepath.Dir(options.Output), scratch)
	manifest := nodeBackupManifest{
		Schema: nodeBackupSchema, CreatedAt: started.UTC().Format(time.RFC3339Nano),
		Node: gateway.Node.Name, PublicKey: identity.PublicKey(), Version: cfg.Version,
		ConfigPath: options.ConfigPath, GatewayConfig: cfg.GatewayConfig,
		IdentityKey: cfg.IdentityKey, Roots: roots,
	}
	size, digest, err := writeSealedFile(options.Output, options.Passphrase, func(out io.Writer) error {
		compressed, err := gzip.NewWriterLevel(out, gzip.BestSpeed)
		if err != nil {
			return err
		}
		archive := &backupArchive{
			ctx: ctx, tar: tar.NewWriter(compressed), scratch: scratch,
			output: options.Output, manifest: &manifest,
		}
		for _, root := range roots {
			if err := archive.addRoot(root); err != nil {
				return err
			}
		}
		raw, err := json.MarshalIndent(manifest, "", "  ")
		if err != nil {
			return err
		}
		if err := archive.tar.WriteHeader(&tar.Header{
			Typeflag: tar.TypeReg, Name: backupManifestEntry, Mode: 0600,
			Size: int64(len(raw)), ModTime: started, Format: tar.FormatPAX,
		}); err != nil {
			return err
		}
		if _, err := archive.tar.Write(raw); err != nil {
			return err
		}
		if err := archive.tar.Close(); err != nil {
			return err
		}
		return compressed.Close()
	})
	if err != nil {
		return report, fmt.Errorf("backup: %w", err)
	}
	report = NodeBackupReport{
		Archive: options.Output, Bytes: size, SHA256: digest,
		Node: manifest.Node, PublicKey: manifest.PublicKey, Version: manifest.Version,
		Files: len(manifest.Files), Repositories: len(manifest.Repositories),
		CreatedAt: manifest.CreatedAt, DurationMillis: now().Sub(started).Milliseconds(),
	}
	for _, repository := range manifest.Repositories {
		report.Refs += repository.Refs
	}
	if err := RecordNodeOperation(cfg, lastBackupRecordName, report); err != nil {
		return report, fmt.Errorf("backup written to %s but not recorded: %w", options.Output, err)
	}
	return report, nil
}

// nodeBackupRoots returns the minimal set of existing paths that covers every
// piece of state the service and gateway configuration name.
func nodeBackupRoots(configPath string, cfg Config, gatewayRaw []byte) ([]string, error) {
	candidates := []string{configPath, cfg.IdentityKey, filepath.Dir(cfg.GatewayConfig)}
	if cfg.ConnectorToken != "" {
		candidates = append(candidates, cfg.ConnectorToken)
	}
	var document any
	if err := json.Unmarshal(gatewayRaw, &document); err != nil {
		return nil, fmt.Errorf("gateway config: %w", err)
	}
	collectBackupPaths(document, &candidates)
	seen := make(map[string]bool)
	var existing []string
	for _, candidate := range candidates {
		if !filepath.IsAbs(candidate) {
			continue
		}
		candidate = filepath.Clean(candidate)
		if candidate == string(filepath.Separator) {
			return nil, errors.New("refusing to back up the filesystem root")
		}
		if seen[candidate] {
			continue
		}
		seen[candidate] = true
		if _, err := os.Lstat(candidate); err != nil {
			if errors.Is(err, fs.ErrNotExist) {
				continue
			}
			return nil, err
		}
		existing = append(existing, candidate)
	}
	sort.Strings(existing)
	var roots []string
	for _, candidate := range existing {
		covered := false
		for _, root := range roots {
			if pathWithin(candidate, root) {
				covered = true
				break
			}
		}
		if !covered {
			roots = append(roots, candidate)
		}
	}
	return roots, nil
}

func collectBackupPaths(value any, out *[]string) {
	switch typed := value.(type) {
	case map[string]any:
		for key, child := range typed {
			if text, ok := child.(string); ok {
				if backupPathKeys[key] {
					*out = append(*out, text)
				}
				continue
			}
			collectBackupPaths(child, out)
		}
	case []any:
		for _, child := range typed {
			collectBackupPaths(child, out)
		}
	}
}

func pathWithin(candidate, parent string) bool {
	return candidate == parent || strings.HasPrefix(candidate, parent+string(filepath.Separator))
}

func isBackupRepository(directory string) bool {
	head, err := os.Lstat(filepath.Join(directory, "HEAD"))
	if err != nil || !head.Mode().IsRegular() {
		return false
	}
	for _, name := range []string{"objects", "refs"} {
		info, err := os.Lstat(filepath.Join(directory, name))
		if err != nil || !info.IsDir() {
			return false
		}
	}
	return true
}

func nodeOpsGit(ctx context.Context, args ...string) (string, error) {
	hardened := []string{"-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false",
		"-c", "maintenance.auto=false", "-c", "gc.auto=0"}
	command := exec.CommandContext(ctx, "git", append(hardened, args...)...)
	command.Env = append(os.Environ(), "GIT_TERMINAL_PROMPT=0")
	var stderr bytes.Buffer
	command.Stderr = &stderr
	out, err := command.Output()
	if err != nil {
		detail := strings.TrimSpace(stderr.String())
		if len(detail) > 500 {
			detail = detail[:500]
		}
		return "", fmt.Errorf("git %s: %w: %s", strings.Join(args, " "), err, detail)
	}
	return string(out), nil
}

// refsListingSHA256 digests "<oid> <refname>" lines — the shared output shape
// of for-each-ref and bundle list-heads — ignoring HEAD and line order.
func refsListingSHA256(listing string) (string, int) {
	var lines []string
	for _, line := range strings.Split(listing, "\n") {
		parts := strings.Fields(line)
		if len(parts) != 2 || !strings.HasPrefix(parts[1], "refs/") {
			continue
		}
		lines = append(lines, parts[0]+" "+parts[1])
	}
	sort.Slice(lines, func(i, j int) bool {
		return strings.SplitN(lines[i], " ", 2)[1] < strings.SplitN(lines[j], " ", 2)[1]
	})
	digest := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	return hex.EncodeToString(digest[:]), len(lines)
}

type backupArchive struct {
	ctx      context.Context
	tar      *tar.Writer
	scratch  string
	output   string
	manifest *nodeBackupManifest
}

func (b *backupArchive) addRoot(root string) error {
	source := root
	info, err := os.Lstat(root)
	if err != nil {
		return err
	}
	// A configured path may itself be a link to another disk; its contents are
	// archived under the configured name. Links inside the tree are refused.
	if info.Mode()&os.ModeSymlink != 0 {
		if source, err = filepath.EvalSymlinks(root); err != nil {
			return fmt.Errorf("resolve %s: %w", root, err)
		}
		if info, err = os.Lstat(source); err != nil {
			return err
		}
	}
	switch {
	case info.IsDir():
		return b.addTree(root, source)
	case info.Mode().IsRegular():
		return b.addFile(root, source)
	default:
		return fmt.Errorf("%s is not a regular file or directory", root)
	}
}

func (b *backupArchive) addTree(name, source string) error {
	if isBackupRepository(source) {
		return b.addRepository(name, source)
	}
	return b.addDirectory(name, source, nil)
}

func (b *backupArchive) addDirectory(name, source string, skip func(string) bool) error {
	info, err := os.Lstat(source)
	if err != nil {
		return err
	}
	b.manifest.Directories = append(b.manifest.Directories,
		backupDirectory{Path: name, Mode: uint32(info.Mode().Perm())})
	entries, err := os.ReadDir(source)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if err := b.ctx.Err(); err != nil {
			return err
		}
		childName := filepath.Join(name, entry.Name())
		childSource := filepath.Join(source, entry.Name())
		if (skip != nil && skip(entry.Name())) || skipBackupEntry(entry, source) ||
			childSource == b.output || childName == b.output {
			continue
		}
		switch {
		case entry.Type()&fs.ModeSymlink != 0:
			return fmt.Errorf("refusing to back up symbolic link %s", childName)
		case entry.IsDir():
			if err := b.addTree(childName, childSource); err != nil {
				return err
			}
		case entry.Type().IsRegular():
			if err := b.addFile(childName, childSource); err != nil {
				return err
			}
		}
		// Sockets and named pipes are runtime endpoints, not state.
	}
	return nil
}

func skipBackupEntry(entry fs.DirEntry, parent string) bool {
	name := entry.Name()
	for _, prefix := range backupSkippedPrefixes {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	if !entry.IsDir() {
		return false
	}
	return name == "tmp" || name == ".cache" ||
		(name == "work" && filepath.Base(parent) == agentJobsDirectoryName)
}

func (b *backupArchive) addRepository(name, source string) error {
	listing, err := nodeOpsGit(b.ctx, "--git-dir="+source, "for-each-ref", "--format=%(objectname) %(refname)")
	if err != nil {
		return err
	}
	repository := backupRepository{GitDir: name}
	repository.RefsSHA256, repository.Refs = refsListingSHA256(listing)
	if repository.Refs > 0 {
		// The bundle's own ref list is the snapshot that was captured, so its
		// digest (not the earlier listing) is what a restore must reproduce.
		bundle := filepath.Join(b.scratch, "repository.bundle")
		if _, err := nodeOpsGit(b.ctx, "--git-dir="+source, "bundle", "create", bundle, "--all"); err != nil {
			return err
		}
		heads, err := nodeOpsGit(b.ctx, "bundle", "list-heads", bundle)
		if err != nil {
			return err
		}
		repository.RefsSHA256, repository.Refs = refsListingSHA256(heads)
		repository.Bundle = fmt.Sprintf("%s%04d.bundle", backupBundlePrefix, len(b.manifest.Repositories)+1)
		repository.BundleSHA256, _, _, err = b.copyFile(repository.Bundle, bundle)
		os.Remove(bundle)
		if err != nil {
			return err
		}
	}
	b.manifest.Repositories = append(b.manifest.Repositories, repository)
	return b.addDirectory(name, source, func(entry string) bool {
		switch entry {
		case "objects", "refs", "logs", "packed-refs", "FETCH_HEAD":
			return true
		}
		return strings.HasSuffix(entry, ".lock")
	})
}

func (b *backupArchive) addFile(name, source string) error {
	digest, size, mode, err := b.copyFile(backupStatePrefix+filepath.ToSlash(name), source)
	if err != nil {
		return err
	}
	b.manifest.Files = append(b.manifest.Files,
		backupFile{Path: name, Mode: uint32(mode), Size: size, SHA256: digest})
	return nil
}

func (b *backupArchive) copyFile(entry, source string) (string, int64, fs.FileMode, error) {
	file, err := os.Open(source)
	if err != nil {
		return "", 0, 0, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return "", 0, 0, err
	}
	if !info.Mode().IsRegular() {
		return "", 0, 0, fmt.Errorf("%s is not a regular file", source)
	}
	if err := b.tar.WriteHeader(&tar.Header{
		Typeflag: tar.TypeReg, Name: entry, Mode: int64(info.Mode().Perm()),
		Size: info.Size(), ModTime: info.ModTime(), Format: tar.FormatPAX,
	}); err != nil {
		return "", 0, 0, err
	}
	digest := sha256.New()
	copied, err := io.CopyN(io.MultiWriter(b.tar, digest), file, info.Size())
	if errors.Is(err, io.EOF) {
		return "", 0, 0, fmt.Errorf("%s shrank while it was being backed up; retry the backup", source)
	}
	if err != nil {
		return "", 0, 0, err
	}
	return hex.EncodeToString(digest.Sum(nil)), copied, info.Mode().Perm(), nil
}

type NodeRestoreOptions struct {
	Archive    string
	Passphrase []byte
	// ConfigPath must equal the configuration path recorded in the backup.
	ConfigPath string
	// Root prefixes every restored path; "/" restores in place.
	Root string
	// Drill restores into a scratch root under WorkDirectory, verifies it, and
	// deletes it. Nothing on the node is touched.
	Drill                 bool
	WorkDirectory         string
	RecoveryTimeObjective time.Duration
	Now                   func() time.Time
}

type NodeRestoreReport struct {
	Archive                     string `json:"archive"`
	Drill                       bool   `json:"drill"`
	Root                        string `json:"root,omitempty"`
	Node                        string `json:"node"`
	PublicKey                   string `json:"publicKey"`
	Version                     string `json:"version"`
	BackupCreatedAt             string `json:"backupCreatedAt"`
	BackupAgeSeconds            int64  `json:"backupAgeSeconds"`
	Files                       int    `json:"files"`
	Repositories                int    `json:"repositories"`
	Refs                        int    `json:"refs"`
	CompletedAt                 string `json:"completedAt"`
	DurationMillis              int64  `json:"durationMs"`
	RecoveryTimeObjectiveMillis int64  `json:"recoveryTimeObjectiveMs"`
	WithinObjective             bool   `json:"withinObjective"`
}

// RestoreNodeBackup restores (or, as a drill, rehearses restoring) a sealed
// node backup and verifies files, repositories, identity, and configuration.
func RestoreNodeBackup(ctx context.Context, options NodeRestoreOptions) (report NodeRestoreReport, err error) {
	now := time.Now
	if options.Now != nil {
		now = options.Now
	}
	started := now()
	objective := options.RecoveryTimeObjective
	if objective <= 0 {
		objective = defaultRecoveryTimeObjective
	}
	if !filepath.IsAbs(options.Archive) || !filepath.IsAbs(options.ConfigPath) {
		return report, errors.New("restore archive and configuration paths must be absolute")
	}
	workDirectory := options.WorkDirectory
	if workDirectory == "" {
		workDirectory = os.TempDir()
	}
	scratch, err := os.MkdirTemp(workDirectory, ".forkmesh-restore-")
	if err != nil {
		return report, err
	}
	defer removeTreeUnder(workDirectory, scratch)
	root := options.Root
	if options.Drill {
		root = filepath.Join(scratch, "root")
		if err := os.Mkdir(root, 0700); err != nil {
			return report, err
		}
	} else {
		if !filepath.IsAbs(root) {
			return report, errors.New("--restore-root must be absolute")
		}
		if info, statErr := os.Stat(root); statErr != nil || !info.IsDir() {
			return report, fmt.Errorf("restore root %s is not a directory", root)
		}
		if os.Geteuid() == 0 {
			return report, errors.New("run the restore as the node's service account (for example `runuser -u forkmesh-node -- forkmesh-mirror-node ...`) so restored files keep the owner the service needs; use --restore-drill to rehearse as root")
		}
	}
	restorer := &nodeRestorer{
		root: filepath.Clean(root), scratch: scratch,
		files: make(map[string]backupFile), bundles: make(map[string]restoredBundle),
		created: make(map[string]bool),
	}
	defer func() {
		if err != nil && !options.Drill {
			restorer.rollback()
		}
	}()
	manifest, err := restorer.extract(options.Archive, options.Passphrase)
	if err != nil {
		return report, err
	}
	if manifest.Schema != nodeBackupSchema {
		return report, fmt.Errorf("unsupported backup schema %q", manifest.Schema)
	}
	if manifest.ConfigPath != options.ConfigPath {
		return report, fmt.Errorf("the backup is of the node configured at %s, not %s", manifest.ConfigPath, options.ConfigPath)
	}
	refs, err := restorer.verify(ctx, manifest)
	if err != nil {
		return report, err
	}
	completed := now()
	duration := completed.Sub(started)
	report = NodeRestoreReport{
		Archive: options.Archive, Drill: options.Drill,
		Node: manifest.Node, PublicKey: manifest.PublicKey, Version: manifest.Version,
		BackupCreatedAt: manifest.CreatedAt, Files: len(manifest.Files),
		Repositories: len(manifest.Repositories), Refs: refs,
		CompletedAt:                 completed.UTC().Format(time.RFC3339Nano),
		DurationMillis:              duration.Milliseconds(),
		RecoveryTimeObjectiveMillis: objective.Milliseconds(),
		WithinObjective:             duration <= objective,
	}
	if !options.Drill {
		report.Root = restorer.root
	}
	if created, parseErr := time.Parse(time.RFC3339Nano, manifest.CreatedAt); parseErr == nil {
		report.BackupAgeSeconds = int64(completed.Sub(created).Seconds())
	}
	return report, nil
}

type restoredBundle struct {
	path   string
	sha256 string
}

type nodeRestorer struct {
	root               string
	scratch            string
	files              map[string]backupFile
	bundles            map[string]restoredBundle
	created            map[string]bool
	createdFiles       []string
	createdDirectories []string
}

func (r *nodeRestorer) target(name string) (string, error) {
	if name == "/" || !path.IsAbs(filepath.ToSlash(name)) ||
		path.Clean(filepath.ToSlash(name)) != filepath.ToSlash(name) ||
		strings.ContainsRune(name, 0) {
		return "", fmt.Errorf("backup path %q is not a clean absolute path", name)
	}
	return filepath.Join(r.root, name), nil
}

func (r *nodeRestorer) extract(archivePath string, passphrase []byte) (nodeBackupManifest, error) {
	var manifest nodeBackupManifest
	file, err := os.Open(archivePath)
	if err != nil {
		return manifest, err
	}
	defer file.Close()
	opened, err := newOpenReader(bufio.NewReaderSize(file, 1<<20), passphrase)
	if err != nil {
		return manifest, err
	}
	compressed, err := gzip.NewReader(opened)
	if err != nil {
		return manifest, fmt.Errorf("read backup archive: %w", err)
	}
	archive := tar.NewReader(compressed)
	haveManifest := false
	for {
		header, err := archive.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return manifest, fmt.Errorf("read backup archive: %w", err)
		}
		if haveManifest {
			return manifest, errors.New("backup archive has entries after its manifest")
		}
		if header.Typeflag != tar.TypeReg || header.Size < 0 {
			return manifest, fmt.Errorf("backup entry %q is not a regular file", header.Name)
		}
		switch {
		case header.Name == backupManifestEntry:
			if header.Size > maximumBackupManifestBytes {
				return manifest, errors.New("backup manifest is too large")
			}
			decoder := json.NewDecoder(archive)
			decoder.DisallowUnknownFields()
			if err := decoder.Decode(&manifest); err != nil {
				return manifest, fmt.Errorf("backup manifest: %w", err)
			}
			haveManifest = true
		case strings.HasPrefix(header.Name, backupBundlePrefix):
			base := strings.TrimPrefix(header.Name, backupBundlePrefix)
			if !backupBundleNamePattern.MatchString(base) {
				return manifest, fmt.Errorf("backup entry %q is invalid", header.Name)
			}
			bundle := filepath.Join(r.scratch, base)
			digest, err := writeExclusiveFile(bundle, archive, 0600, header.Size)
			if err != nil {
				return manifest, err
			}
			r.bundles[header.Name] = restoredBundle{path: bundle, sha256: digest}
		case strings.HasPrefix(header.Name, backupStatePrefix+"/"):
			name := filepath.FromSlash(strings.TrimPrefix(header.Name, backupStatePrefix))
			target, err := r.target(name)
			if err != nil {
				return manifest, err
			}
			if err := r.ensureDirectory(filepath.Dir(target)); err != nil {
				return manifest, err
			}
			mode := fs.FileMode(header.Mode) & 0777
			digest, err := writeExclusiveFile(target, archive, mode, header.Size)
			if errors.Is(err, fs.ErrExist) {
				return manifest, fmt.Errorf("refusing to overwrite existing %s", target)
			}
			if err != nil {
				return manifest, err
			}
			r.createdFiles = append(r.createdFiles, target)
			r.files[name] = backupFile{Path: name, Mode: uint32(mode), Size: header.Size, SHA256: digest}
		default:
			return manifest, fmt.Errorf("backup entry %q is not recognized", header.Name)
		}
	}
	// Drain to the end so the gzip checksum and the sealed final chunk are
	// both verified; a tar stream alone can end before a truncation point.
	if _, err := io.Copy(io.Discard, compressed); err != nil {
		return manifest, fmt.Errorf("read backup archive: %w", err)
	}
	if !haveManifest {
		return manifest, errors.New("backup archive has no manifest")
	}
	return manifest, nil
}

func writeExclusiveFile(target string, source io.Reader, mode fs.FileMode, size int64) (string, error) {
	file, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return "", err
	}
	digest := sha256.New()
	_, writeErr := io.CopyN(io.MultiWriter(file, digest), source, size)
	if writeErr == nil {
		writeErr = file.Chmod(mode)
	}
	if writeErr == nil {
		writeErr = file.Sync()
	}
	if closeErr := file.Close(); writeErr == nil {
		writeErr = closeErr
	}
	if writeErr != nil {
		os.Remove(target)
		return "", writeErr
	}
	return hex.EncodeToString(digest.Sum(nil)), nil
}

// ensureDirectory creates directory and any missing parents, recording each
// one it created. Existing ancestors are used as they are.
func (r *nodeRestorer) ensureDirectory(directory string) error {
	if info, err := os.Stat(directory); err == nil {
		if !info.IsDir() {
			return fmt.Errorf("%s is not a directory", directory)
		}
		return nil
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	if directory == r.root {
		return fmt.Errorf("restore root %s does not exist", r.root)
	}
	if err := r.ensureDirectory(filepath.Dir(directory)); err != nil {
		return err
	}
	if err := os.Mkdir(directory, 0700); err != nil {
		return err
	}
	r.created[directory] = true
	r.createdDirectories = append(r.createdDirectories, directory)
	return nil
}

func (r *nodeRestorer) verify(ctx context.Context, manifest nodeBackupManifest) (int, error) {
	if len(manifest.Files) != len(r.files) {
		return 0, errors.New("backup archive files do not match its manifest")
	}
	for _, file := range manifest.Files {
		restored, ok := r.files[file.Path]
		if !ok || restored.Size != file.Size || restored.SHA256 != file.SHA256 || restored.Mode != file.Mode {
			return 0, fmt.Errorf("restored %s does not match the backup manifest", file.Path)
		}
	}
	for _, directory := range manifest.Directories {
		target, err := r.target(directory.Path)
		if err != nil {
			return 0, err
		}
		if err := r.ensureDirectory(target); err != nil {
			return 0, err
		}
	}
	refs := 0
	for _, repository := range manifest.Repositories {
		target, err := r.target(repository.GitDir)
		if err != nil {
			return 0, err
		}
		if !r.created[target] {
			return 0, fmt.Errorf("refusing to restore into existing repository directory %s", target)
		}
		if _, err := nodeOpsGit(ctx, "init", "--bare", "--quiet", "--template=", target); err != nil {
			return 0, err
		}
		if repository.Bundle != "" {
			bundle, ok := r.bundles[repository.Bundle]
			if !ok || bundle.sha256 != repository.BundleSHA256 {
				return 0, fmt.Errorf("repository bundle for %s is missing or damaged", repository.GitDir)
			}
			if _, err := nodeOpsGit(ctx, "--git-dir="+target, "fetch", "--quiet",
				"--update-head-ok", bundle.path, "+refs/*:refs/*"); err != nil {
				return 0, err
			}
			os.Remove(filepath.Join(target, "FETCH_HEAD"))
		}
		listing, err := nodeOpsGit(ctx, "--git-dir="+target, "for-each-ref", "--format=%(objectname) %(refname)")
		if err != nil {
			return 0, err
		}
		digest, count := refsListingSHA256(listing)
		if digest != repository.RefsSHA256 || count != repository.Refs {
			return 0, fmt.Errorf("restored refs of %s do not match the backup", repository.GitDir)
		}
		if count > 0 {
			if _, err := nodeOpsGit(ctx, "--git-dir="+target, "fsck",
				"--connectivity-only", "--no-dangling", "--no-progress"); err != nil {
				return 0, fmt.Errorf("restored repository %s is not connected: %w", repository.GitDir, err)
			}
		}
		refs += count
	}
	identityPath, err := r.target(manifest.IdentityKey)
	if err != nil {
		return 0, err
	}
	identity, err := LoadIdentity(identityPath)
	if err != nil {
		return 0, fmt.Errorf("restored identity: %w", err)
	}
	if identity.PublicKey() != manifest.PublicKey {
		return 0, errors.New("restored identity key does not match the backup's public key")
	}
	configPath, err := r.target(manifest.ConfigPath)
	if err != nil {
		return 0, err
	}
	cfg, err := LoadConfig(configPath)
	if err != nil {
		return 0, fmt.Errorf("restored configuration: %w", err)
	}
	if cfg.IdentityKey != manifest.IdentityKey || cfg.GatewayConfig != manifest.GatewayConfig {
		return 0, errors.New("restored configuration does not name the backed-up identity and gateway")
	}
	gatewayPath, err := r.target(manifest.GatewayConfig)
	if err != nil {
		return 0, err
	}
	gateway, _, err := loadGatewayConfig(gatewayPath)
	if err != nil {
		return 0, fmt.Errorf("restored gateway config: %w", err)
	}
	if gateway.Node.Name != manifest.Node || gateway.Node.PublicKey != manifest.PublicKey {
		return 0, errors.New("restored gateway config does not match the backed-up node identity")
	}
	// Directory modes last: a read-only directory would block the writes above.
	for _, directory := range manifest.Directories {
		target, _ := r.target(directory.Path)
		if r.created[target] {
			if err := os.Chmod(target, fs.FileMode(directory.Mode)&0777); err != nil {
				return 0, err
			}
		}
	}
	return refs, nil
}

// rollback removes exactly what a failed restore created. Pre-existing files
// and directories are never touched.
func (r *nodeRestorer) rollback() {
	for i := len(r.createdFiles) - 1; i >= 0; i-- {
		os.Remove(r.createdFiles[i])
	}
	for i := len(r.createdDirectories) - 1; i >= 0; i-- {
		_ = removeTreeUnder(filepath.Dir(r.createdDirectories[i]), r.createdDirectories[i])
	}
}

// RecordNodeOperation stores a bounded, secret-free operation record beside
// the gateway configuration, where /v1/status and --preflight read it.
func RecordNodeOperation(cfg Config, name string, value any) error {
	gatewayDirectory := filepath.Dir(cfg.GatewayConfig)
	directory := filepath.Join(gatewayDirectory, nodeOpsDirectoryName)
	record := filepath.Join(directory, name)
	if err := writePrivateJSON(record, value); err != nil {
		return err
	}
	// Run as root, keep the records readable by the service account that owns
	// the gateway directory.
	if err := alignOwner(directory, gatewayDirectory); err != nil {
		return err
	}
	return alignOwner(record, gatewayDirectory)
}

type RecoveryStatus struct {
	LastBackupAt                    string `json:"lastBackupAt,omitempty"`
	LastBackupAgeSeconds            int64  `json:"lastBackupAgeSeconds,omitempty"`
	LastBackupBytes                 int64  `json:"lastBackupBytes,omitempty"`
	LastBackupDurationMillis        int64  `json:"lastBackupDurationMs,omitempty"`
	LastRestoreDrillAt              string `json:"lastRestoreDrillAt,omitempty"`
	LastRestoreDrillAgeSeconds      int64  `json:"lastRestoreDrillAgeSeconds,omitempty"`
	LastRestoreDrillDurationMillis  int64  `json:"lastRestoreDrillDurationMs,omitempty"`
	RecoveryTimeObjectiveMillis     int64  `json:"recoveryTimeObjectiveMs,omitempty"`
	LastRestoreDrillWithinObjective bool   `json:"lastRestoreDrillWithinObjective"`
}

// ReadRecoveryStatus summarizes the last recorded backup and restore drill.
// Missing or unreadable records simply leave their fields empty.
func ReadRecoveryStatus(gatewayConfig string, now time.Time) RecoveryStatus {
	var status RecoveryStatus
	directory := filepath.Join(filepath.Dir(gatewayConfig), nodeOpsDirectoryName)
	var backup NodeBackupReport
	if readNodeOperationRecord(filepath.Join(directory, lastBackupRecordName), &backup) {
		status.LastBackupAt = backup.CreatedAt
		status.LastBackupBytes = backup.Bytes
		status.LastBackupDurationMillis = backup.DurationMillis
		if at, err := time.Parse(time.RFC3339Nano, backup.CreatedAt); err == nil {
			status.LastBackupAgeSeconds = int64(now.Sub(at).Seconds())
		}
	}
	var drill NodeRestoreReport
	if readNodeOperationRecord(filepath.Join(directory, lastRestoreDrillRecordName), &drill) {
		status.LastRestoreDrillAt = drill.CompletedAt
		status.LastRestoreDrillDurationMillis = drill.DurationMillis
		status.RecoveryTimeObjectiveMillis = drill.RecoveryTimeObjectiveMillis
		status.LastRestoreDrillWithinObjective = drill.WithinObjective
		if at, err := time.Parse(time.RFC3339Nano, drill.CompletedAt); err == nil {
			status.LastRestoreDrillAgeSeconds = int64(now.Sub(at).Seconds())
		}
	}
	return status
}

func readNodeOperationRecord(path string, value any) bool {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 64<<10 {
		return false
	}
	raw, err := os.ReadFile(path)
	return err == nil && json.Unmarshal(raw, value) == nil
}
