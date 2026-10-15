package hosts

import (
	"math/rand/v2"
	"net/url"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const (
	gatewayBaseCooldownMs    int64 = 10 * 1000
	gatewayMaxCooldownMs     int64 = 5 * 60 * 1000
	gatewayRepeatNoticeMs    int64 = 60 * 60 * 1000
	gatewayRetiredCooldownMs int64 = 6 * 60 * 60 * 1000
)

var gatewayUnreachablePhrases = []string{
	"connection timed out",
	"operation timed out",
	"connection refused",
	"no route to host",
	"network is unreachable",
	"could not resolve hostname",
	"name or service not known",
	"nodename nor servname provided",
	"temporary failure in name resolution",
	"connection closed by remote host",
	"connection reset by peer",
}

func mirrorGatewayUnreachable(stderr string) bool {
	text := strings.ToLower(stderr)
	for _, phrase := range gatewayUnreachablePhrases {
		if strings.Contains(text, phrase) {
			return true
		}
	}
	return false
}

func hostSegments(value string) []string {
	parts := []string{}
	var current strings.Builder
	for _, ch := range strings.ToLower(strings.TrimSpace(value)) {
		if ch == '-' || ch == '.' {
			if current.Len() > 0 {
				parts = append(parts, current.String())
			}
			current.Reset()
			continue
		}
		current.WriteRune(ch)
	}
	if current.Len() > 0 {
		parts = append(parts, current.String())
	}
	return parts
}

func mirrorGatewayHostBelongsToNode(host, node string) bool {
	wanted := hostSegments(node)
	have := hostSegments(host)
	if len(wanted) == 0 || len(have) == 0 || len(wanted) > len(have) {
		return false
	}
	for start := 0; start+len(wanted) <= len(have); start++ {
		match := true
		for i := range wanted {
			if have[start+i] != wanted[i] {
				match = false
				break
			}
		}
		if match {
			return true
		}
	}
	return false
}

type backoffChannel struct {
	failures      int
	nextAllowedMs int64
}

type networkBackoff struct {
	channels map[string]*backoffChannel
	jitter   func(span int64) int64
}

func (b *networkBackoff) state(channel string) *backoffChannel {
	if b.channels == nil {
		return nil
	}
	return b.channels[channel]
}

func (b *networkBackoff) ready(channel string, nowMs int64) bool {
	s := b.state(channel)
	return s == nil || s.nextAllowedMs <= nowMs
}

func (b *networkBackoff) msUntilReady(channel string, nowMs int64) int64 {
	s := b.state(channel)
	if s == nil {
		return 0
	}
	return max(0, s.nextAllowedMs-nowMs)
}

func (b *networkBackoff) noteSuccess(channel string) {
	if channel != "" && b.channels != nil {
		delete(b.channels, channel)
	}
}

func (b *networkBackoff) noteFailure(channel string, nowMs, baseMs, capMs, retryAfterMs int64) {
	if channel == "" {
		return
	}
	if b.channels == nil {
		b.channels = map[string]*backoffChannel{}
	}
	s := b.channels[channel]
	if s == nil {
		s = &backoffChannel{}
		b.channels[channel] = s
	}
	if s.failures < 30 {
		s.failures++
	}
	delay := baseMs
	for i := 1; i < s.failures && delay < capMs; i++ {
		delay <<= 1
	}
	delay = min(delay, capMs)
	if retryAfterMs > 0 {
		delay = min(max(delay, retryAfterMs), capMs)
	}
	span := max(int64(1), delay/8)
	jitter := b.jitter
	if jitter == nil {
		jitter = func(span int64) int64 { return rand.Int64N(span) }
	}
	s.nextAllowedMs = nowMs + delay + jitter(span)
}

type gatewayStreak struct {
	sinceMs        int64
	announcedMs    int64
	retiredNoticed bool
}

type gatewayNotice struct {
	cooldownMs int64
	downForMs  int64
	announce   bool
	retired    bool
	parked     bool
	persist    bool
}

type gatewayHealth struct {
	backoff networkBackoff
	streaks map[string]*gatewayStreak
}

func (h *gatewayHealth) readyToPush(host string, nowMs int64) bool {
	return h.backoff.ready(host, nowMs)
}

func (h *gatewayHealth) cooldownRemainingMs(host string, nowMs int64) int64 {
	return h.backoff.msUntilReady(host, nowMs)
}

func (h *gatewayHealth) noteUnreachable(host string, nowMs int64) gatewayNotice {
	if h.streaks == nil {
		h.streaks = map[string]*gatewayStreak{}
	}
	streak := h.streaks[host]
	if streak == nil {
		streak = &gatewayStreak{}
		h.streaks[host] = streak
	}
	notice := gatewayNotice{}
	if streak.sinceMs == 0 {
		streak.sinceMs = nowMs
		notice.persist = true
	}
	notice.downForMs = max(0, nowMs-streak.sinceMs)
	notice.parked = notice.downForMs >= gatewayRetiredAfterMs
	capMs, retryAfter := gatewayMaxCooldownMs, int64(0)
	if notice.parked {
		capMs, retryAfter = gatewayRetiredCooldownMs, gatewayRetiredCooldownMs
	}
	h.backoff.noteFailure(host, nowMs, gatewayBaseCooldownMs, capMs, retryAfter)
	notice.cooldownMs = h.backoff.msUntilReady(host, nowMs)
	notice.announce = !notice.parked && nowMs-streak.announcedMs >= gatewayRepeatNoticeMs
	if notice.announce {
		streak.announcedMs = nowMs
		notice.persist = true
	}
	if !streak.retiredNoticed && notice.parked {
		streak.retiredNoticed = true
		notice.retired = true
		notice.persist = true
	}
	return notice
}

func (h *gatewayHealth) noteReachable(host string) bool {
	h.backoff.noteSuccess(host)
	if _, ok := h.streaks[host]; ok {
		delete(h.streaks, host)
		return true
	}
	return false
}

func (h *gatewayHealth) forget(host string) bool {
	if _, ok := h.streaks[host]; ok {
		delete(h.streaks, host)
		return true
	}
	return false
}

func (h *gatewayHealth) persistedState() []string {
	records := []string{}
	for host, streak := range h.streaks {
		if host == "" || streak.sinceMs == 0 {
			continue
		}
		retired := "0"
		if streak.retiredNoticed {
			retired = "1"
		}
		records = append(records, host+"\t"+strconv.FormatInt(streak.sinceMs, 10)+"\t"+
			strconv.FormatInt(streak.announcedMs, 10)+"\t"+retired)
	}
	sort.Strings(records)
	return records
}

func (h *gatewayHealth) restoreState(records []string, nowMs int64) {
	h.streaks = map[string]*gatewayStreak{}
	for _, record := range records {
		fields := strings.Split(record, "\t")
		if len(fields) < 4 {
			continue
		}
		host := strings.TrimSpace(fields[0])
		if host == "" {
			continue
		}
		since, _ := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)
		announced, _ := strconv.ParseInt(strings.TrimSpace(fields[2]), 10, 64)
		if since <= 0 || since > nowMs {
			continue
		}
		if announced > nowMs {
			announced = 0
		}
		h.streaks[host] = &gatewayStreak{sinceMs: since, announcedMs: announced, retiredNoticed: strings.TrimSpace(fields[3]) == "1"}
	}
}

func mirrorPushRetryDelayMs(health *gatewayHealth, skipped []string, nowMs, periodicPassMs int64) int64 {
	soonest := int64(0)
	for _, host := range skipped {
		remaining := health.cooldownRemainingMs(host, nowMs)
		if remaining <= 0 || remaining >= periodicPassMs {
			continue
		}
		if soonest == 0 || remaining < soonest {
			soonest = remaining
		}
	}
	if soonest > 0 {
		return soonest + 250
	}
	return 0
}

type refPushReservation struct {
	readyHosts   map[string]bool
	replayQueued bool
}

type refPushGate struct {
	inFlight      map[string]bool
	replayPending map[string]bool
}

func gateKey(repository, host string) string { return repository + "\x1f" + host }

func (g *refPushGate) reserve(repository string, hosts []string) refPushReservation {
	repo := trimmedLower(repository)
	result := refPushReservation{readyHosts: map[string]bool{}}
	if repo == "" {
		return result
	}
	if g.inFlight == nil {
		g.inFlight = map[string]bool{}
		g.replayPending = map[string]bool{}
	}
	seen := map[string]bool{}
	for _, raw := range hosts {
		host := trimmedLower(raw)
		if host == "" || seen[host] {
			continue
		}
		seen[host] = true
		key := gateKey(repo, host)
		if g.inFlight[key] {
			result.replayQueued = true
			continue
		}
		g.inFlight[key] = true
		result.readyHosts[host] = true
	}
	if result.replayQueued {
		g.replayPending[repo] = true
	}
	return result
}

func (g *refPushGate) finish(repository, host string) bool {
	repo := trimmedLower(repository)
	key := gateKey(repo, trimmedLower(host))
	if repo == "" || !g.inFlight[key] {
		return false
	}
	delete(g.inFlight, key)
	prefix := repo + "\x1f"
	for other := range g.inFlight {
		if strings.HasPrefix(other, prefix) {
			return false
		}
	}
	if g.replayPending[repo] {
		delete(g.replayPending, repo)
		return true
	}
	return false
}

func (g *refPushGate) busy(repository string, hosts []string) bool {
	repo := trimmedLower(repository)
	for _, host := range hosts {
		if g.inFlight[gateKey(repo, trimmedLower(host))] {
			return true
		}
	}
	return false
}

type mirrorPushOutcome struct {
	updated      []string
	heldBack     []string
	onlyHeldBack bool
}

func classifyMirrorPush(porcelain string) mirrorPushOutcome {
	outcome := mirrorPushOutcome{}
	sawRejection, sawHard := false, false
	for _, line := range strings.Split(porcelain, "\n") {
		if line == "" || strings.HasPrefix(line, "To ") || strings.HasPrefix(line, "Done") {
			continue
		}
		flag := line[0]
		fields := strings.Split(line, "\t")
		if len(fields) < 2 || flag == '=' {
			continue
		}
		ref := fields[1]
		if index := strings.LastIndexByte(ref, ':'); index >= 0 {
			ref = ref[index+1:]
		}
		if strings.HasPrefix(ref, "refs/heads/") {
			ref = ref[len("refs/heads/"):]
		} else if strings.HasPrefix(ref, "refs/tags/") {
			ref = ref[len("refs/tags/"):]
		}
		if flag != '!' {
			outcome.updated = append(outcome.updated, ref)
			continue
		}
		sawRejection = true
		summary := ""
		if len(fields) > 2 {
			summary = fields[2]
		}
		protectedRewind := strings.Contains(summary, "[rejected]") &&
			(strings.Contains(summary, "non-fast-forward") || strings.Contains(summary, "fetch first") || strings.Contains(summary, "stale info"))
		if protectedRewind {
			outcome.heldBack = append(outcome.heldBack, ref)
		} else {
			sawHard = true
		}
	}
	outcome.onlyHeldBack = sawRejection && !sawHard
	return outcome
}

var exactDigestPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

const managedArtifactSeedTemplate = `set -eu; umask 077; [ "$(id -u)" = 0 ] || { printf 'Release artifact seeding requires root.\n' >&2; exit 77; }; ` +
	`id forkmesh-node >/dev/null 2>&1 || { printf 'The managed mirror service account is missing.\n' >&2; exit 76; }; ` +
	`dir=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/forkmesh-releases/sha256/%1/%2; ` +
	`install -d -o forkmesh-node -g forkmesh-node -m 0700 "$dir"; ` +
	`exec runuser -u forkmesh-node -- /bin/sh -c 'set -eu; umask 077; ` +
	`dir=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/forkmesh-releases/sha256/%1/%2; ` +
	`install -d -m 0700 "$dir"; tmp="$(mktemp "$dir/.seed.XXXXXX")" || exit 70; ` +
	`cleanup() { rm -f -- "$tmp"; }; trap cleanup EXIT HUP INT TERM; cat >"$tmp"; ` +
	`actual="$(sha256sum "$tmp")"; actual="${actual%% *}"; ` +
	`[ "$actual" = %2 ] || { printf "Release artifact checksum verification failed.\n" >&2; exit 74; }; ` +
	`chmod 0600 "$tmp"; mv -f -- "$tmp" "$dir/data"; trap - EXIT HUP INT TERM'`

const managedArtifactInventoryTemplate = `set -eu; [ "$(id -u)" = 0 ] || { printf 'Release artifact inventory requires root.\n' >&2; exit 77; }; ` +
	`id forkmesh-node >/dev/null 2>&1 || { printf 'The managed mirror service account is missing.\n' >&2; exit 76; }; ` +
	`exec runuser -u forkmesh-node -- /bin/sh -c 'set -eu; ` +
	`root=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/forkmesh-releases/sha256; ` +
	`for hash in %1; do shard="$(printf %s "$hash" | cut -c1-2)"; path="$root/$shard/$hash/data"; ` +
	`if [ ! -f "$path" ] || [ -L "$path" ]; then printf "%s\n" "$hash"; continue; fi; ` +
	`actual="$(sha256sum "$path")"; actual="${actual%% *}"; [ "$actual" = "$hash" ] || printf "%s\n" "$hash"; done'`

func managedReleaseArtifactSeedCommand(sha256 string) string {
	hash := trimmedLower(sha256)
	if !exactDigestPattern.MatchString(hash) {
		return ""
	}
	return qtArg(managedArtifactSeedTemplate, hash[:2], hash)
}

func managedReleaseArtifactInventoryCommand(sha256s []string) string {
	if len(sha256s) == 0 || len(sha256s) > 512 {
		return ""
	}
	hashes := make([]string, 0, len(sha256s))
	seen := map[string]bool{}
	for _, value := range sha256s {
		hash := trimmedLower(value)
		if !exactDigestPattern.MatchString(hash) || seen[hash] {
			return ""
		}
		hashes = append(hashes, hash)
		seen[hash] = true
	}
	return qtArg(managedArtifactInventoryTemplate, strings.Join(hashes, " "))
}

func hostSSHCommandLine(command sshCommand) string {
	if command.Program == "" {
		return ""
	}
	words := []string{shq(command.Program)}
	for _, arg := range command.Args {
		words = append(words, shq(arg))
	}
	return strings.Join(words, " ")
}

func urlHost(raw string) string {
	parsed, err := url.Parse(raw)
	if err != nil {
		return ""
	}
	return strings.ToLower(parsed.Hostname())
}

func managedPushURL(user, host string) string {
	clean := strings.ToLower(strings.TrimSpace(host))
	if clean == "" {
		return ""
	}
	if strings.Contains(clean, ":") {
		clean = "[" + clean + "]"
	}
	return "ssh://" + url.PathEscape(user) + "@" + clean + "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"
}
