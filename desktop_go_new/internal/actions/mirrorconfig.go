package actions

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	mirrorConfigRequestType    = "forkmesh.mirror-actions-configuration"
	mirrorConfigResultType     = "forkmesh.mirror-actions-configuration-result"
	mirrorConfigJournalType    = "forkmesh.mirror-actions-configuration-transaction"
	mirrorGatewayType          = "forkmesh.headless-mirror-refresh"
	mirrorGatewayDefaultPath   = "/var/lib/forkmesh-mirror/gateway/mirror-refresh.json"
	maximumVariableValueBytes  = 16 * 1024
	maximumVariableBytes       = 48 * 1024
	maximumVariables           = 64
	maximumJournalBytes        = 2 * 1024 * 1024
	defaultConfigLockTimeout   = 30 * time.Second
	configLockStaleAfter       = 30 * time.Second
	mirrorConfiguredAtSetting  = "actions/mirrorConfiguredAt"
	mirrorRefreshConfigSetting = "actions/mirrorRefreshConfigPath"
	actionsVariablesSetting    = "actions/variables"
)

var (
	mirrorRequestIDPattern  = regexp.MustCompile(`^[a-f0-9]{32}$`)
	mirrorRepoNamePattern   = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)
	mirrorVariableName      = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]{0,63}$`)
	mirrorBranchBodyPattern = regexp.MustCompile(`^[A-Za-z0-9._/-]{1,120}$`)
	sha256HexPattern        = regexp.MustCompile(`^[a-f0-9]{64}$`)
	base64URLPattern        = regexp.MustCompile(`^[A-Za-z0-9_-]*$`)
)

var configurationSettingKeys = []string{
	mirrorEnabledSetting,
	mirrorGenerationSetting,
	mirrorNodeSetting,
	mirrorRefreshConfigSetting,
	mirrorStatePathSetting,
	mirrorSummaryPathSetting,
	mirrorConfiguredAtSetting,
	actionsVariablesSetting,
}

type mirrorConfigState struct {
	mu        sync.Mutex
	attempted bool
	ok        bool
	errorCode string
}

func (m *Module) mirrorRecoveryFailed() bool {
	if m.mirrorConfig == nil {
		return false
	}
	m.mirrorConfig.mu.Lock()
	defer m.mirrorConfig.mu.Unlock()
	return m.mirrorConfig.attempted && !m.mirrorConfig.ok
}

type MirrorConfigEnv struct {
	SettingsPath      string
	SettingsFormat    int
	DataRoot          string
	Write             SettingsWriter
	GatewayCandidates []string
	LockTimeout       time.Duration
	Vault             func() *vault.Store
	VaultChanged      func(names []string)
}

func validMirrorBranch(branch string) bool {
	return mirrorBranchBodyPattern.MatchString(branch) && !strings.HasPrefix(branch, "/") && !strings.HasSuffix(branch, "/") &&
		!strings.Contains(branch, "..") && !strings.Contains(branch, "//")
}

func mirrorConfigResult(requestID, node string, enabled, replaced bool, count int, ok bool, errorCode string) map[string]any {
	out := map[string]any{
		"type":              mirrorConfigResultType,
		"schemaVersion":     1,
		"ok":                ok,
		"requestId":         requestID,
		"node":              node,
		"actionsEnabled":    enabled,
		"variablesReplaced": replaced,
		"variableCount":     count,
	}
	if errorCode != "" {
		out["errorCode"] = errorCode
	}
	return out
}

func exactKeys(object map[string]any, required, optional []string) bool {
	allowed := map[string]bool{}
	for _, key := range required {
		if _, ok := object[key]; !ok {
			return false
		}
		allowed[key] = true
	}
	for _, key := range optional {
		allowed[key] = true
	}
	for key := range object {
		if !allowed[key] {
			return false
		}
	}
	return true
}

func safeAbsolutePath(value string) bool {
	if len(utf16Units(value)) < 2 || len(utf16Units(value)) > 4096 || strings.ContainsAny(value, "\x00\r\n") {
		return false
	}
	if !filepath.IsAbs(value) {
		return false
	}
	info, err := os.Lstat(value)
	return err != nil || info.Mode()&os.ModeSymlink == 0
}

func (env MirrorConfigEnv) identity() string {
	sum := sha256.Sum256([]byte(strconv.Itoa(env.SettingsFormat) + ":" + env.SettingsPath))
	return hex.EncodeToString(sum[:])[:24]
}

func (env MirrorConfigEnv) transactionDirectory() string {
	if env.SettingsPath != "" && filepath.IsAbs(env.SettingsPath) {
		return filepath.Dir(env.SettingsPath)
	}
	return filepath.Join(env.DataRoot, "actions")
}

func (env MirrorConfigEnv) basePath() string {
	return filepath.Join(env.transactionDirectory(), ".forkmesh-actions-configuration-"+env.identity())
}

func (env MirrorConfigEnv) JournalPath() string { return env.basePath() + ".json" }
func (env MirrorConfigEnv) LockPath() string    { return env.basePath() + ".lock" }

func (env MirrorConfigEnv) load() *qsettings.Settings {
	settings, err := qsettings.Load(env.SettingsPath)
	if err != nil {
		return qsettings.Parse(nil)
	}
	return settings
}

func (env MirrorConfigEnv) write(writes []daemon.SettingWrite) error {
	if env.Write == nil {
		return errDesktopUnavailable
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	return env.Write(ctx, writes)
}

func (env MirrorConfigEnv) prepareDirectory() string {
	dir := env.transactionDirectory()
	if dir == "" || !filepath.IsAbs(dir) {
		return "configuration_lock_unavailable"
	}
	if info, err := os.Lstat(dir); err == nil {
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "configuration_lock_unavailable"
		}
	} else if os.MkdirAll(dir, 0o700) != nil {
		return "configuration_lock_unavailable"
	}
	if runtime.GOOS != "windows" && os.Chmod(dir, 0o700) != nil {
		return "configuration_lock_unavailable"
	}
	return ""
}

type configLock struct {
	path string
}

func lockContents() []byte {
	host, _ := os.Hostname()
	name := filepath.Base(os.Args[0])
	return []byte(fmt.Sprintf("%d\n%s\n%s\n", os.Getpid(), name, host))
}

func lockHolder(path string) (int, string, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, "", false
	}
	lines := strings.Split(string(data), "\n")
	if len(lines) < 3 {
		return 0, "", false
	}
	pid, err := strconv.Atoi(strings.TrimSpace(lines[0]))
	if err != nil || pid <= 0 {
		return 0, "", false
	}
	return pid, strings.TrimSpace(lines[2]), true
}

func lockIsStale(path string) bool {
	info, err := os.Stat(path)
	if err != nil {
		return true
	}
	if pid, host, ok := lockHolder(path); ok {
		if local, _ := os.Hostname(); host == local && !processAlive(pid) {
			return true
		}
	}
	return time.Since(info.ModTime()) > configLockStaleAfter
}

func acquireConfigLock(path string, timeout time.Duration) (*configLock, string) {
	deadline := time.Now().Add(timeout)
	for {
		file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err == nil {
			_, werr := file.Write(lockContents())
			cerr := file.Close()
			if werr != nil || cerr != nil {
				_ = os.Remove(path)
				return nil, "configuration_lock_unavailable"
			}
			return &configLock{path: path}, ""
		}
		if !os.IsExist(err) {
			return nil, "configuration_lock_unavailable"
		}
		if lockIsStale(path) {
			_ = os.Remove(path)
			continue
		}
		if time.Now().After(deadline) {
			return nil, "configuration_busy"
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func (l *configLock) release() {
	if l != nil {
		_ = os.Remove(l.path)
	}
}

type configSnapshot struct {
	repositoryValues map[string]map[string]any
	directValues     map[string]map[string]any
	present          []string
}

func encodeSettingValue(value qsettings.Value) (map[string]any, bool) {
	switch value.Kind {
	case "text":
		return map[string]any{"type": "string", "value": value.Text}, true
	case "list":
		items := make([]any, len(value.List))
		for index, item := range value.List {
			items[index] = item
		}
		return map[string]any{"type": "strings", "value": items}, true
	case "bytes":
		return map[string]any{"type": "bytes", "value": base64.RawURLEncoding.EncodeToString(value.Bytes)}, true
	}
	return nil, false
}

func captureConfiguration(settings *qsettings.Settings) (configSnapshot, bool) {
	snapshot := configSnapshot{repositoryValues: map[string]map[string]any{}, directValues: map[string]map[string]any{}}
	for _, key := range settings.Keys(repositoriesArray + "/") {
		value, _ := settings.Value(key)
		encoded, ok := encodeSettingValue(value)
		if !ok {
			return snapshot, false
		}
		snapshot.repositoryValues[key] = encoded
	}
	for _, key := range configurationSettingKeys {
		value, present := settings.Value(key)
		if !present {
			continue
		}
		encoded, ok := encodeSettingValue(value)
		if !ok {
			return snapshot, false
		}
		snapshot.directValues[key] = encoded
		snapshot.present = append(snapshot.present, key)
	}
	sort.Strings(snapshot.present)
	return snapshot, true
}

func (s configSnapshot) object() map[string]any {
	repositories := map[string]any{}
	for key, value := range s.repositoryValues {
		repositories[key] = value
	}
	direct := map[string]any{}
	for key, value := range s.directValues {
		direct[key] = value
	}
	present := make([]any, len(s.present))
	for index, key := range s.present {
		present[index] = key
	}
	return map[string]any{"repositoryValues": repositories, "directValues": direct, "presentDirectKeys": present}
}

func decodeVariantWrite(key string, encoded any) (daemon.SettingWrite, bool) {
	object, ok := encoded.(map[string]any)
	if !ok || !exactKeys(object, []string{"type", "value"}, nil) {
		return daemon.SettingWrite{}, false
	}
	kind, _ := object["type"].(string)
	raw := object["value"]
	switch kind {
	case "bool":
		value, ok := raw.(bool)
		return daemon.SettingWrite{Key: key, Type: "bool", Value: value}, ok
	case "string":
		value, ok := raw.(string)
		return daemon.SettingWrite{Key: key, Type: "string", Value: value}, ok
	case "bytes":
		text, ok := raw.(string)
		if !ok || !base64URLPattern.MatchString(text) {
			return daemon.SettingWrite{}, false
		}
		data, err := base64.RawURLEncoding.DecodeString(text)
		if err != nil || base64.RawURLEncoding.EncodeToString(data) != text {
			return daemon.SettingWrite{}, false
		}
		return daemon.SettingWrite{Key: key, Type: "bytes", Value: base64.StdEncoding.EncodeToString(data)}, true
	case "int", "uint", "longlong", "ulonglong":
		text, ok := raw.(string)
		if !ok {
			return daemon.SettingWrite{}, false
		}
		value, err := strconv.ParseInt(text, 10, 64)
		if err != nil {
			return daemon.SettingWrite{}, false
		}
		return daemon.SettingWrite{Key: key, Type: "int", Value: value}, true
	case "double":
		text, ok := raw.(string)
		if !ok {
			return daemon.SettingWrite{}, false
		}
		value, err := strconv.ParseFloat(text, 64)
		if err != nil {
			return daemon.SettingWrite{}, false
		}
		return daemon.SettingWrite{Key: key, Type: "double", Value: value}, true
	case "strings":
		items, ok := raw.([]any)
		if !ok {
			return daemon.SettingWrite{}, false
		}
		values := make([]string, 0, len(items))
		for _, item := range items {
			text, ok := item.(string)
			if !ok {
				return daemon.SettingWrite{}, false
			}
			values = append(values, text)
		}
		return daemon.SettingWrite{Key: key, Type: "stringList", Value: values}, true
	}
	return daemon.SettingWrite{}, false
}

func decodeSnapshot(object map[string]any) (configSnapshot, bool) {
	snapshot := configSnapshot{repositoryValues: map[string]map[string]any{}, directValues: map[string]map[string]any{}}
	if !exactKeys(object, []string{"repositoryValues", "directValues", "presentDirectKeys"}, nil) {
		return snapshot, false
	}
	repositories, ok1 := object["repositoryValues"].(map[string]any)
	direct, ok2 := object["directValues"].(map[string]any)
	present, ok3 := object["presentDirectKeys"].([]any)
	if !ok1 || !ok2 || !ok3 {
		return snapshot, false
	}
	for key, value := range repositories {
		if !strings.HasPrefix(key, repositoriesArray+"/") {
			return snapshot, false
		}
		if _, ok := decodeVariantWrite(key, value); !ok {
			return snapshot, false
		}
		snapshot.repositoryValues[key] = value.(map[string]any)
	}
	for key, value := range direct {
		if !containsString(configurationSettingKeys, key) {
			return snapshot, false
		}
		if _, ok := decodeVariantWrite(key, value); !ok {
			return snapshot, false
		}
		snapshot.directValues[key] = value.(map[string]any)
	}
	seen := map[string]bool{}
	for _, item := range present {
		key, ok := item.(string)
		if !ok || !containsString(configurationSettingKeys, key) || seen[key] {
			return snapshot, false
		}
		seen[key] = true
		snapshot.present = append(snapshot.present, key)
	}
	if len(snapshot.present) != len(snapshot.directValues) {
		return snapshot, false
	}
	for key := range snapshot.directValues {
		if !seen[key] {
			return snapshot, false
		}
	}
	sort.Strings(snapshot.present)
	return snapshot, true
}

func journalPayload(requestID string, previous configSnapshot) map[string]any {
	return map[string]any{
		"type":          mirrorConfigJournalType,
		"schemaVersion": 2,
		"requestId":     requestID,
		"previous":      previous.object(),
	}
}

func EncodeMirrorJournal(requestID string, previous configSnapshot) []byte {
	payload := journalPayload(requestID, previous)
	canonical := qtJSONCompact(payload)
	sum := sha256.Sum256(canonical)
	payload["sha256"] = hex.EncodeToString(sum[:])
	encoded := qtJSONCompact(payload)
	if len(encoded) > maximumJournalBytes {
		return nil
	}
	return encoded
}

func decodeJSONValue(data []byte) (any, bool) {
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	var value any
	if decoder.Decode(&value) != nil {
		return nil, false
	}
	return value, true
}

func DecodeMirrorJournal(encoded []byte) (string, configSnapshot, bool) {
	if len(encoded) == 0 || len(encoded) > maximumJournalBytes {
		return "", configSnapshot{}, false
	}
	value, ok := decodeJSONValue(encoded)
	payload, isObject := value.(map[string]any)
	if !ok || !isObject || !exactKeys(payload, []string{"type", "schemaVersion", "requestId", "previous", "sha256"}, nil) {
		return "", configSnapshot{}, false
	}
	if kind, _ := payload["type"].(string); kind != mirrorConfigJournalType {
		return "", configSnapshot{}, false
	}
	if version, _ := payload["schemaVersion"].(float64); version != 2 {
		return "", configSnapshot{}, false
	}
	previousObject, ok := payload["previous"].(map[string]any)
	if !ok {
		return "", configSnapshot{}, false
	}
	checksum, _ := payload["sha256"].(string)
	checksum = strings.ToLower(checksum)
	delete(payload, "sha256")
	if !sha256HexPattern.MatchString(checksum) {
		return "", configSnapshot{}, false
	}
	sum := sha256.Sum256(qtJSONCompact(payload))
	if hex.EncodeToString(sum[:]) != checksum {
		return "", configSnapshot{}, false
	}
	requestID, _ := payload["requestId"].(string)
	previous, ok := decodeSnapshot(previousObject)
	if !mirrorRequestIDPattern.MatchString(requestID) || !ok {
		return "", configSnapshot{}, false
	}
	return requestID, previous, true
}

func syncDir(path string) bool {
	if runtime.GOOS == "windows" {
		return true
	}
	dir, err := os.Open(path)
	if err != nil {
		return false
	}
	defer dir.Close()
	return dir.Sync() == nil
}

func (env MirrorConfigEnv) writeJournal(requestID string, previous configSnapshot) bool {
	encoded := EncodeMirrorJournal(requestID, previous)
	if encoded == nil {
		return false
	}
	path := env.JournalPath()
	parent := filepath.Dir(path)
	if os.MkdirAll(parent, 0o700) != nil {
		return false
	}
	temp, err := os.CreateTemp(parent, "."+filepath.Base(path)+".*")
	if err != nil {
		return false
	}
	name := temp.Name()
	if temp.Chmod(0o600) != nil || writeAll(temp, encoded) != nil || temp.Sync() != nil {
		_ = temp.Close()
		_ = os.Remove(name)
		return false
	}
	if temp.Close() != nil || os.Rename(name, path) != nil {
		_ = os.Remove(name)
		return false
	}
	if os.Chmod(path, 0o600) != nil || !syncDir(parent) {
		return false
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	decodedID, decoded, ok := DecodeMirrorJournal(data)
	return ok && decodedID == requestID && snapshotsEqual(decoded, previous)
}

func writeAll(file *os.File, data []byte) error {
	_, err := file.Write(data)
	return err
}

func snapshotsEqual(a, b configSnapshot) bool {
	return string(qtJSONCompact(a.object())) == string(qtJSONCompact(b.object()))
}

type journalRead int

const (
	journalMissing journalRead = iota
	journalLoaded
	journalInvalid
)

func (env MirrorConfigEnv) readJournal() (string, configSnapshot, journalRead) {
	path := env.JournalPath()
	info, err := os.Lstat(path)
	if err != nil {
		return "", configSnapshot{}, journalMissing
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > maximumJournalBytes {
		return "", configSnapshot{}, journalInvalid
	}
	if runtime.GOOS != "windows" && (info.Mode().Perm()&0o077 != 0 || !ownedByEffectiveUser(info)) {
		return "", configSnapshot{}, journalInvalid
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", configSnapshot{}, journalInvalid
	}
	id, snapshot, ok := DecodeMirrorJournal(data)
	if !ok {
		return "", configSnapshot{}, journalInvalid
	}
	return id, snapshot, journalLoaded
}

func (env MirrorConfigEnv) removeJournal() bool {
	path := env.JournalPath()
	if pathExists(path) && os.Remove(path) != nil {
		return false
	}
	return !pathExists(path) && syncDir(filepath.Dir(path))
}

func (env MirrorConfigEnv) restrictSettingsFile() bool {
	if env.SettingsPath == "" {
		return false
	}
	if !pathExists(env.SettingsPath) {
		return false
	}
	return os.Chmod(env.SettingsPath, 0o600) == nil
}

func normalizedValue(value qsettings.Value) []string {
	switch value.Kind {
	case "list":
		return value.List
	case "invalid":
		return []string{}
	case "bytes":
		return []string{"\x01" + string(value.Bytes)}
	}
	return []string{value.Text}
}

func normalizedWrite(write daemon.SettingWrite) []string {
	switch write.Type {
	case "bool":
		if write.Value == true {
			return []string{"true"}
		}
		return []string{"false"}
	case "int":
		switch v := write.Value.(type) {
		case int64:
			return []string{strconv.FormatInt(v, 10)}
		case int:
			return []string{strconv.Itoa(v)}
		}
	case "double":
		value, _ := write.Value.(float64)
		return []string{strconv.FormatFloat(value, 'g', -1, 64)}
	case "stringList":
		values, _ := write.Value.([]string)
		return values
	case "bytes":
		data, _ := base64.StdEncoding.DecodeString(fmt.Sprint(write.Value))
		return []string{"\x01" + string(data)}
	case "json":
		return []string{"\x01" + string(qtJSONCompact(write.Value))}
	}
	return []string{fmt.Sprint(write.Value)}
}

func sameNormalized(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for index := range a {
		if a[index] != b[index] {
			return false
		}
	}
	return true
}

func (env MirrorConfigEnv) durableMatches(expected map[string][]string, absent []string) bool {
	settings := env.load()
	actual := map[string][]string{}
	for _, key := range settings.Keys(repositoriesArray + "/") {
		value, _ := settings.Value(key)
		actual[key] = normalizedValue(value)
	}
	for _, key := range configurationSettingKeys {
		if value, ok := settings.Value(key); ok {
			actual[key] = normalizedValue(value)
		}
	}
	for key, want := range expected {
		got, ok := actual[key]
		if !ok || !sameNormalized(got, want) {
			return false
		}
	}
	for key := range actual {
		if _, ok := expected[key]; !ok && strings.HasPrefix(key, repositoriesArray+"/") {
			return false
		}
	}
	for _, key := range absent {
		if _, ok := actual[key]; ok {
			return false
		}
	}
	return true
}

func (env MirrorConfigEnv) restore(snapshot configSnapshot) bool {
	writes := []daemon.SettingWrite{{Key: repositoriesArray, Remove: true}}
	expected := map[string][]string{}
	keys := make([]string, 0, len(snapshot.repositoryValues))
	for key := range snapshot.repositoryValues {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		write, ok := decodeVariantWrite(key, snapshot.repositoryValues[key])
		if !ok {
			return false
		}
		writes = append(writes, write)
		expected[key] = normalizedWrite(write)
	}
	var absent []string
	for _, key := range configurationSettingKeys {
		if containsString(snapshot.present, key) {
			write, ok := decodeVariantWrite(key, snapshot.directValues[key])
			if !ok {
				return false
			}
			writes = append(writes, write)
			expected[key] = normalizedWrite(write)
		} else {
			writes = append(writes, daemon.SettingWrite{Key: key, Remove: true})
			absent = append(absent, key)
		}
	}
	if env.write(writes) != nil {
		return false
	}
	if pathExists(env.SettingsPath) && os.Chmod(env.SettingsPath, 0o600) != nil {
		return false
	}
	return env.durableMatches(expected, absent)
}

func (env MirrorConfigEnv) recoverLocked() string {
	requestID, previous, state := env.readJournal()
	switch state {
	case journalMissing:
		if env.Vault != nil {
			return env.reconcileMirrorVault("", false)
		}
		return ""
	case journalInvalid:
		return "configuration_journal_invalid"
	}
	if env.Vault != nil {
		if code := env.reconcileMirrorVault(requestID, true); code != "" {
			return code
		}
	}
	if !env.restore(previous) {
		return "configuration_local_rollback_failed"
	}
	if !env.removeJournal() {
		return "configuration_journal_cleanup_failed"
	}
	return ""
}

func (env MirrorConfigEnv) timeout() time.Duration {
	if env.LockTimeout > 0 {
		return env.LockTimeout
	}
	return defaultConfigLockTimeout
}

func (env MirrorConfigEnv) RecoverPending() string {
	if code := env.prepareDirectory(); code != "" {
		return code
	}
	lock, code := acquireConfigLock(env.LockPath(), env.timeout())
	if lock == nil {
		return code
	}
	defer lock.release()
	return env.recoverLocked()
}

func (env MirrorConfigEnv) ResetRecoveryFiles() ([]string, string) {
	removed := []string{}
	removeExact := func(path string) bool {
		info, err := os.Lstat(path)
		if err != nil {
			return true
		}
		if info.IsDir() {
			return false
		}
		if os.Remove(path) != nil {
			return false
		}
		removed = append(removed, path)
		return true
	}
	lockPath, journalPath := env.LockPath(), env.JournalPath()
	ok := true
	if lock, _ := acquireConfigLock(lockPath, 0); lock != nil {
		ok = removeExact(journalPath)
		lock.release()
	} else {
		if pid, _, readable := lockHolder(lockPath); readable && pid > 0 && processAlive(pid) {
			return removed, "configuration_busy"
		}
		ok = removeExact(journalPath) && removeExact(lockPath)
	}
	if !ok {
		return removed, "configuration_reset_failed"
	}
	return removed, ""
}

func readGatewayConfiguration(candidates []string) (map[string]any, string) {
	for _, path := range candidates {
		if !safeAbsolutePath(path) {
			continue
		}
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > 64*1024 {
			continue
		}
		if info.Mode().Perm()&0o022 != 0 {
			continue
		}
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		object, ok := decodeJSONObject(data)
		if !ok || jsonInt(object, "schemaVersion", 0) != 1 || jsonString(object, "type") != mirrorGatewayType {
			continue
		}
		return object, path
	}
	return nil, ""
}

func gatewayOwner(gateway map[string]any) string {
	node := qtTrim(jsonString(gateway, "nodeOwner"))
	for _, value := range jsonArray(gateway, "ownerAliases") {
		alias, _ := value.(string)
		alias = qtTrim(alias)
		if alias != node && summaryNodePattern.MatchString(alias) {
			return alias
		}
	}
	return node
}

func actionMirrorPath(dataRoot, node, owner, repository string) string {
	sum := sha256.Sum256([]byte(node + "/" + owner + "/" + repository))
	return filepath.Join(dataRoot, "actions", "mirrors", hex.EncodeToString(sum[:])[:24]+".git")
}

func prepareActionMirror(source, destination string) bool {
	if !safeAbsolutePath(source) || !dirIsReal(source) || !safeAbsolutePath(destination) {
		return false
	}
	if os.MkdirAll(filepath.Dir(destination), 0o700) != nil {
		return false
	}
	if !pathExists(destination) {
		_, code := gitCaptureMerged("", 30*time.Second, "clone", "--mirror", "--no-hardlinks", source, destination)
		return code == 0
	}
	if info, err := os.Stat(filepath.Join(destination, "HEAD")); err != nil || !info.Mode().IsRegular() {
		return false
	}
	_, code := gitCaptureMerged("", 30*time.Second, "--git-dir", destination, "fetch", "--prune", source,
		"+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*")
	return code == 0
}

type repoRecordValue struct {
	key   string
	write daemon.SettingWrite
}

func readRepositoryRecords(settings *qsettings.Settings) ([][]repoRecordValue, bool) {
	count := int(settings.Int(repositoriesArray+"/size", 0))
	records := make([][]repoRecordValue, 0, count)
	for index := 1; index <= count; index++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(index) + "/"
		var keys []string
		for _, key := range settings.Keys(prefix) {
			child := key[len(prefix):]
			if child != "" && !strings.Contains(child, "/") {
				keys = append(keys, child)
			}
		}
		sort.Strings(keys)
		var record []repoRecordValue
		for _, key := range keys {
			value, _ := settings.Value(prefix + key)
			encoded, ok := encodeSettingValue(value)
			if !ok {
				return nil, false
			}
			write, ok := decodeVariantWrite(key, encoded)
			if !ok {
				return nil, false
			}
			record = append(record, repoRecordValue{key, write})
		}
		records = append(records, record)
	}
	return records, true
}

func recordString(record []repoRecordValue, key string) string {
	for _, value := range record {
		if value.key == key {
			return fmt.Sprint(value.write.Value)
		}
	}
	return ""
}

func configuredRepositories(current [][]repoRecordValue, owner, repository, source, mirror, branch string, enabled bool) [][]repoRecordValue {
	configured := []repoRecordValue{
		{"owner", daemon.SettingWrite{Type: "string", Value: owner}},
		{"name", daemon.SettingWrite{Type: "string", Value: repository}},
		{"cloneUrl", daemon.SettingWrite{Type: "string", Value: source}},
		{"localPath", daemon.SettingWrite{Type: "string", Value: ""}},
		{"mirrorPath", daemon.SettingWrite{Type: "string", Value: mirror}},
		{"publishToNetwork", daemon.SettingWrite{Type: "bool", Value: false}},
		{"isPrivate", daemon.SettingWrite{Type: "bool", Value: false}},
		{"actionsEnabled", daemon.SettingWrite{Type: "bool", Value: enabled}},
		{"externallyManagedActions", daemon.SettingWrite{Type: "bool", Value: true}},
		{"externalActionsSource", daemon.SettingWrite{Type: "string", Value: source}},
		{"externalActionsRef", daemon.SettingWrite{Type: "string", Value: "refs/heads/" + branch}},
		{"secretScanningEnabled", daemon.SettingWrite{Type: "bool", Value: true}},
	}
	records := make([][]repoRecordValue, len(current))
	copy(records, current)
	match := -1
	for index, record := range records {
		if recordString(record, "owner") == owner && recordString(record, "name") == repository {
			match = index
			break
		}
	}
	if match < 0 {
		return append(records, configured)
	}
	merged := append([]repoRecordValue(nil), records[match]...)
	for _, value := range configured {
		replaced := false
		for index := range merged {
			if merged[index].key == value.key {
				merged[index] = value
				replaced = true
			}
		}
		if !replaced {
			merged = append(merged, value)
		}
	}
	sort.Slice(merged, func(i, j int) bool { return merged[i].key < merged[j].key })
	records[match] = merged
	return records
}

func ApplyMirrorConfiguration(request map[string]any, env MirrorConfigEnv) map[string]any {
	requestID := qtTrim(jsonString(request, "requestId"))
	if !mirrorRequestIDPattern.MatchString(requestID) {
		requestID = strings.Repeat("0", 32)
	}
	node := qtTrim(jsonString(request, "node"))
	if !summaryNodePattern.MatchString(node) {
		node = ""
	}
	enabled := jsonBool(request, "actionsEnabled")
	fail := func(code string) map[string]any {
		return mirrorConfigResult(requestID, node, enabled, false, 0, false, code)
	}
	_, enabledIsBool := request["actionsEnabled"].(bool)
	if !exactKeys(request, []string{"type", "schemaVersion", "requestId", "node", "actionsEnabled"}, []string{"variables"}) ||
		jsonString(request, "type") != mirrorConfigRequestType || jsonInt(request, "schemaVersion", 0) != 1 || node == "" || !enabledIsBool {
		return fail("invalid_request")
	}
	settings := env.load()
	localNode := qtTrim(settings.String(machineNodeNameSetting, ""))
	if localNode == "" {
		localNode = qtTrim(settings.String(accountNodeNameSetting, ""))
	}
	if !summaryNodePattern.MatchString(localNode) || localNode != node {
		return fail("node_mismatch")
	}
	if code := env.prepareDirectory(); code != "" {
		return fail(code)
	}
	lock, code := acquireConfigLock(env.LockPath(), env.timeout())
	if lock == nil {
		return fail(code)
	}
	defer lock.release()
	if code := env.recoverLocked(); code != "" {
		return fail(code)
	}
	if !env.restrictSettingsFile() {
		return fail("settings_write_failed")
	}
	replaceVariables := false
	variables := map[string]any{}
	if envelopeValue, present := request["variables"]; present {
		envelope, ok := envelopeValue.(map[string]any)
		if !ok || !exactKeys(envelope, []string{"mode", "values"}, nil) || jsonString(envelope, "mode") != "replace" {
			return fail("invalid_variables")
		}
		values, ok := envelope["values"].(map[string]any)
		if !ok || len(values) > maximumVariables {
			return fail("invalid_variables")
		}
		total := 0
		for name, raw := range values {
			value, isString := raw.(string)
			if !mirrorVariableName.MatchString(name) || !isString {
				return fail("invalid_variables")
			}
			total += len(value)
			if strings.ContainsRune(value, 0) || len(value) > maximumVariableValueBytes || total > maximumVariableBytes {
				return fail("invalid_variables")
			}
			variables[name] = value
		}
		replaceVariables = true
	}
	sealed := replaceVariables && env.Vault != nil
	if sealed && !vaultUsable(env.vaultStore()) {
		return fail("variables_vault_unavailable")
	}
	candidates := env.GatewayCandidates
	if candidates == nil {
		if configured := strings.TrimSpace(os.Getenv("FORKMESH_MIRROR_REFRESH_CONFIG")); configured != "" {
			candidates = append(candidates, configured)
		}
		candidates = append(candidates, mirrorGatewayDefaultPath)
	}
	gateway, gatewayPath := readGatewayConfiguration(candidates)
	source := qtTrim(jsonString(gateway, "sourceRepository"))
	repository := qtTrim(jsonString(gateway, "repositoryName"))
	owner := gatewayOwner(gateway)
	catalog, _ := gateway["catalog"].(map[string]any)
	branch := qtTrim(jsonString(catalog, "branch"))
	if branch == "" {
		branch = "main"
	}
	if gateway == nil || qtTrim(jsonString(gateway, "nodeOwner")) != node || owner == "" ||
		!mirrorRepoNamePattern.MatchString(repository) || !validMirrorBranch(branch) || !safeAbsolutePath(source) {
		return fail("mirror_config_unavailable")
	}
	mirror := actionMirrorPath(env.DataRoot, node, owner, repository)
	if !prepareActionMirror(source, mirror) {
		return fail("actions_mirror_unavailable")
	}
	current, ok := readRepositoryRecords(settings)
	if !ok {
		return fail("configuration_journal_write_failed")
	}
	records := configuredRepositories(current, owner, repository, source, mirror, branch, enabled)
	previous, ok := captureConfiguration(settings)
	if !ok {
		return fail("configuration_journal_write_failed")
	}
	if sealed {
		next := map[string]string{}
		for name, value := range variables {
			next[name], _ = value.(string)
		}
		before, err := env.sealMirrorVariables(settings, requestID, next)
		if err != nil {
			return fail("variables_vault_unavailable")
		}
		if _, present := previous.directValues[actionsVariablesSetting]; present || len(before) > 0 {
			previous.directValues[actionsVariablesSetting] = markerSnapshotValue(before)
			if !containsString(previous.present, actionsVariablesSetting) {
				previous.present = append(previous.present, actionsVariablesSetting)
				sort.Strings(previous.present)
			}
		}
	}
	if !env.writeJournal(requestID, previous) {
		if sealed && env.rollbackMirrorTxn() != nil {
			return fail("configuration_recovery_failed")
		}
		return fail("configuration_journal_write_failed")
	}
	writes := []daemon.SettingWrite{{Key: repositoriesArray, Remove: true}}
	expected := map[string][]string{}
	add := func(write daemon.SettingWrite) {
		writes = append(writes, write)
		expected[write.Key] = normalizedWrite(write)
	}
	for index, record := range records {
		prefix := repositoriesArray + "/" + strconv.Itoa(index+1) + "/"
		for _, value := range record {
			write := value.write
			write.Key = prefix + value.key
			add(write)
		}
	}
	add(daemon.SettingWrite{Key: repositoriesArray + "/size", Type: "int", Value: int64(len(records))})
	gatewayDir := filepath.Dir(gatewayPath)
	add(daemon.SettingWrite{Key: mirrorEnabledSetting, Type: "bool", Value: enabled})
	add(daemon.SettingWrite{Key: mirrorNodeSetting, Type: "string", Value: node})
	add(daemon.SettingWrite{Key: mirrorRefreshConfigSetting, Type: "string", Value: gatewayPath})
	add(daemon.SettingWrite{Key: mirrorStatePathSetting, Type: "string", Value: filepath.Join(gatewayDir, stateFileName)})
	add(daemon.SettingWrite{Key: mirrorSummaryPathSetting, Type: "string", Value: filepath.Join(gatewayDir, summaryFileName)})
	add(daemon.SettingWrite{Key: mirrorConfiguredAtSetting, Type: "int", Value: nowMs()})
	if sealed {
		names := map[string]string{}
		for name := range variables {
			names[name] = ""
		}
		add(daemon.SettingWrite{Key: actionsVariablesSetting, Type: "json", Value: markerMap(names)})
	} else if replaceVariables {
		add(daemon.SettingWrite{Key: actionsVariablesSetting, Type: "json", Value: variables})
	}
	add(daemon.SettingWrite{Key: mirrorGenerationSetting, Type: "string", Value: requestID})
	if !replaceVariables {
		if value, present := settings.Value(actionsVariablesSetting); present {
			expected[actionsVariablesSetting] = normalizedValue(value)
		}
	}
	variableCount := 0
	if replaceVariables {
		variableCount = len(variables)
	}
	writeErr := env.write(writes)
	if writeErr != nil || !env.restrictSettingsFile() || !env.durableMatches(expected, nil) {
		if rollback := env.recoverLocked(); rollback != "" {
			return fail(rollback)
		}
		return fail("settings_write_failed")
	}
	if !env.removeJournal() {
		if rollback := env.recoverLocked(); rollback != "" {
			return fail(rollback)
		}
		return fail("configuration_finalize_failed")
	}
	if env.Vault != nil {
		_ = env.commitMirrorTxn()
	}
	return mirrorConfigResult(requestID, node, enabled, replaceVariables, variableCount, true, "")
}

func (m *Module) mirrorConfigEnv() MirrorConfigEnv {
	env := MirrorConfigEnv{Write: m.settingsWriter(), SettingsFormat: 0}
	if runtime.GOOS == "darwin" || runtime.GOOS == "windows" {
		env.SettingsFormat = 1
	}
	if m.core != nil {
		env.DataRoot = m.core.Layout.Data
		if m.core.Settings != nil {
			env.SettingsPath = m.core.Settings.Path
		}
	}
	env.Vault = m.vaultStore
	if m.Vault == nil && m.core != nil {
		if _, ok := m.core.Module("vault").(*vault.Module); !ok {
			env.Vault = nil
		}
	} else if m.Vault == nil {
		env.Vault = nil
	}
	env.VaultChanged = func(names []string) {
		if m.core != nil {
			m.core.BroadcastUI("vault.changed", map[string]any{"namespace": vault.ActionsNamespace, "names": names})
		}
	}
	if m.MirrorConfig != nil {
		env = m.MirrorConfig(env)
	}
	return env
}

func (m *Module) vaultStore() *vault.Store {
	if m.Vault != nil {
		return m.Vault()
	}
	if m.core == nil {
		return nil
	}
	module, ok := m.core.Module("vault").(*vault.Module)
	if !ok || module == nil {
		return nil
	}
	return module.Store()
}

func (m *Module) recordRecovery(code string) {
	if m.mirrorConfig == nil {
		return
	}
	m.mirrorConfig.mu.Lock()
	m.mirrorConfig.attempted = true
	m.mirrorConfig.ok = code == ""
	m.mirrorConfig.errorCode = code
	m.mirrorConfig.mu.Unlock()
}

func (m *Module) registerMirrorConfig(handle func(string, rpc.Handler)) {
	if m.mirrorConfig == nil {
		m.mirrorConfig = &mirrorConfigState{}
	}
	handle("core.actions.mirrorConfig.apply", m.local(m.handleMirrorConfigApply))
	handle("core.actions.mirrorConfig.recover", m.local(m.handleMirrorConfigRecover))
	handle("core.actions.mirrorConfig.reset", m.local(m.handleMirrorConfigReset))
}

func (m *Module) settingsWriterAttached() bool {
	return m.WriteSettings != nil || (m.core != nil && m.core.UI() != nil)
}

func (m *Module) handleMirrorConfigApply(ctx context.Context, req *rpc.Request) (any, error) {
	if !m.settingsWriterAttached() {
		return nil, errDesktopUnavailable
	}
	request := req.Object()
	if inner, ok := request["request"].(map[string]any); ok {
		request = inner
	}
	result := ApplyMirrorConfiguration(request, m.mirrorConfigEnv())
	if result["ok"] == true {
		m.logUI("Actions: mirror Actions configuration %s applied by forkmesh-core.", jsonString(result, "requestId"))
	}
	return result, nil
}

func (m *Module) handleMirrorConfigRecover(ctx context.Context, req *rpc.Request) (any, error) {
	if !m.settingsWriterAttached() {
		return nil, errDesktopUnavailable
	}
	env := m.mirrorConfigEnv()
	code := env.RecoverPending()
	m.recordRecovery(code)
	out := map[string]any{"ok": code == "", "lockPath": env.LockPath(), "journalPath": env.JournalPath()}
	if code != "" {
		out["errorCode"] = code
	}
	return out, nil
}

func (m *Module) handleMirrorConfigReset(ctx context.Context, req *rpc.Request) (any, error) {
	removed, code := m.mirrorConfigEnv().ResetRecoveryFiles()
	out := map[string]any{"removed": removed}
	if code != "" {
		out["errorCode"] = code
	} else {
		m.recordRecovery("")
	}
	return out, nil
}

func (m *Module) reconcileMirrorVaultAtStart() {
	env := m.mirrorConfigEnv()
	if env.Vault == nil || env.SettingsPath == "" || pathExists(env.JournalPath()) {
		return
	}
	if _, pending, err := env.pendingMirrorTxn(); err != nil || !pending {
		return
	}
	if code := env.RecoverPending(); code != "" {
		m.logf("mirror Actions configuration: the vault side of an interrupted transaction could not be settled (%s)", code)
		return
	}
	m.logf("mirror Actions configuration: settled the vault side of an interrupted transaction")
}

func (m *Module) recoverMirrorConfigAtAttach() {
	env := m.mirrorConfigEnv()
	if env.SettingsPath == "" || !pathExists(env.JournalPath()) {
		m.reconcileMirrorVaultAtStart()
		return
	}
	code := env.RecoverPending()
	m.recordRecovery(code)
	if code != "" {
		m.logUI("Actions: an interrupted mirror Actions configuration could not be rolled back (%s); mirror Actions stay suspended. Journal: %s", code, env.JournalPath())
	} else {
		m.logUI("Actions: rolled back an interrupted mirror Actions configuration.")
	}
}
