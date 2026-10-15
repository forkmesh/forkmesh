package stats

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	codemagicCacheMs       = 5 * 60 * 1000
	codemagicTokenVariable = "CODEMAGIC_API_TOKEN"
	codemagicUserURL       = "https://api.codemagic.io/user"
	codemagicTimeout       = 15 * time.Second
	codemagicMaxBody       = 8 << 20
)

type codemagicFlight struct {
	done   chan struct{}
	usage  map[string]any
	status int
}

type codemagicState struct {
	mu     sync.Mutex
	cached map[string]any
	token  string
	atMs   int64
	flight *codemagicFlight
}

func object(value any) map[string]any {
	if out, ok := value.(map[string]any); ok {
		return out
	}
	return map[string]any{}
}

func isNumber(value any) bool {
	_, ok := value.(float64)
	return ok
}

func positive(value float64) float64 {
	if value < 0 {
		return 0
	}
	return value
}

func freeSeconds(buildTime map[string]any) float64 {
	total := 0.0
	for key, value := range buildTime {
		if strings.HasSuffix(key, "_free") {
			total += positive(number(value))
		}
	}
	return total
}

func paidSeconds(buildTime map[string]any) float64 {
	total := 0.0
	for key, value := range buildTime {
		if !strings.HasSuffix(key, "_free") {
			total += positive(number(value))
		}
	}
	return total
}

func nextUTCMonthStartMs(nowMs int64) int64 {
	now := time.UnixMilli(nowMs).UTC()
	first := time.Date(now.Year(), now.Month(), 1, 0, 0, 0, 0, time.UTC).AddDate(0, 1, 0)
	return first.UnixMilli()
}

func codemagicUsage(reply map[string]any, nowMs int64) map[string]any {
	user := reply
	if value, ok := reply["user"]; ok {
		user = object(value)
	}
	buildTimes := object(user["buildTimes"])
	usage := object(object(user["billing"])["usage"])
	current := object(object(usage["currentPeriod"])["buildTime"])
	previous := object(object(usage["previousPeriod"])["buildTime"])
	limit := number(buildTimes["monthlyFreeBuildTimeLimit"])
	if limit <= 0 {
		limit = number(object(usage["freeLimit"])["buildTime"])
	}
	if limit <= 0 {
		return map[string]any{}
	}
	currentFree := object(buildTimes["currentPeriod"])["free"]
	previousFree := object(buildTimes["previousPeriod"])["free"]
	used := freeSeconds(current)
	if isNumber(currentFree) {
		used = number(currentFree)
	}
	used = positive(used)
	before := freeSeconds(previous)
	if isNumber(previousFree) {
		before = number(previousFree)
	}
	before = positive(before)
	return map[string]any{
		"limitSeconds":     limit,
		"usedSeconds":      used,
		"remainingSeconds": positive(limit - used),
		"previousSeconds":  before,
		"paidSeconds":      paidSeconds(current),
		"resetsAt":         float64(nextUTCMonthStartMs(nowMs)),
		"fetchedAt":        float64(nowMs),
	}
}

func codemagicError(status int) string {
	if status == 401 || status == 403 {
		return "codemagic_unauthorized"
	}
	if status >= 200 && status < 300 {
		return "bad_answer"
	}
	return "codemagic_unreachable"
}

func (m *Module) httpClient() *http.Client {
	if m.HTTPClient != nil {
		return m.HTTPClient
	}
	return &http.Client{Timeout: codemagicTimeout}
}

func (m *Module) codemagicURL() string {
	if m.CodemagicURL != "" {
		return m.CodemagicURL
	}
	return codemagicUserURL
}

func (m *Module) fetchCodemagic(token string) (map[string]any, int) {
	ctx, cancel := context.WithTimeout(context.Background(), codemagicTimeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, m.codemagicURL(), nil)
	if err != nil {
		return map[string]any{}, 0
	}
	request.Header.Set("x-auth-token", token)
	request.Header.Set("Accept", "application/json")
	response, err := m.httpClient().Do(request)
	if err != nil {
		return map[string]any{}, 0
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, codemagicMaxBody))
	status := response.StatusCode
	if err != nil {
		return map[string]any{}, 0
	}
	if status < 200 || status >= 300 {
		return map[string]any{}, status
	}
	var reply map[string]any
	if json.Unmarshal(body, &reply) != nil || reply == nil {
		reply = map[string]any{}
	}
	return codemagicUsage(reply, m.now().UnixMilli()), status
}

func (m *Module) handleCodemagic(ctx context.Context, req *rpc.Request) (any, error) {
	token := strings.TrimSpace(vault.Variables(m.core)[codemagicTokenVariable])
	if token == "" {
		return nil, rpc.Fail("codemagic_not_configured")
	}
	state := &m.codemagic
	now := m.now().UnixMilli()
	state.mu.Lock()
	if len(state.cached) > 0 && state.token == token && now-state.atMs < codemagicCacheMs {
		cached := state.cached
		state.mu.Unlock()
		return cached, nil
	}
	flight := state.flight
	if flight == nil {
		flight = &codemagicFlight{done: make(chan struct{})}
		state.flight = flight
		done := m.track()
		go func() {
			defer done()
			usage, status := m.fetchCodemagic(token)
			at := m.now().UnixMilli()
			state.mu.Lock()
			flight.usage = usage
			flight.status = status
			if state.flight == flight {
				state.flight = nil
			}
			if len(usage) > 0 {
				state.cached = usage
				state.token = token
				state.atMs = at
			}
			state.mu.Unlock()
			close(flight.done)
		}()
	}
	state.mu.Unlock()
	select {
	case <-flight.done:
	case <-ctx.Done():
		return nil, rpc.Fail("codemagic_unreachable")
	}
	if len(flight.usage) == 0 {
		return nil, rpc.Fail(codemagicError(flight.status))
	}
	return flight.usage, nil
}
