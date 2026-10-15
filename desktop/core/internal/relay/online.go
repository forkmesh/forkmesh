package relay

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/identity"
)

const (
	OnlineGap         = 10 * time.Minute
	onlineProof       = "forkmesh-account-desktop-ping-v1"
	onlinePath        = "/api/notifications/desktop"
	onlineKind        = "desktop-online"
	phonePingsSetting = "notifications/phonePings"
	onlinePingSetting = "notifications/phoneOnlinePing"
	onlineTimeout     = 15 * time.Second
)

type OnlinePing struct {
	PingID    string `json:"pingId"`
	Node      string `json:"node"`
	Kind      string `json:"kind"`
	Category  string `json:"category"`
	SessionID string `json:"sessionId"`
	Repo      string `json:"repo"`
	Warning   bool   `json:"warning"`
	Title     string `json:"title"`
	Body      string `json:"body"`
}

type onlineTracker struct {
	connected bool
	lost      time.Time
}

func NodeToken(node string) string {
	var out strings.Builder
	count := 0
	for _, r := range strings.ToLower(strings.TrimSpace(node)) {
		if count >= 64 {
			break
		}
		count++
		if (r < 128 && (r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9')) || r == '_' || r == '.' || r == '-' {
			out.WriteRune(r)
		} else {
			out.WriteByte('-')
		}
	}
	if out.Len() == 0 {
		return "desktop"
	}
	return out.String()
}

func OfflineSpan(gap time.Duration) string {
	minutes := int64(gap / time.Minute)
	if minutes < 1 {
		minutes = 1
	}
	if minutes < 60 {
		return fmt.Sprintf("%d min", minutes)
	}
	hours := minutes / 60
	if hours < 24 {
		if minutes%60 != 0 {
			return fmt.Sprintf("%d h %d min", hours, minutes%60)
		}
		return fmt.Sprintf("%d h", hours)
	}
	days := hours / 24
	if hours%24 != 0 {
		return fmt.Sprintf("%d d %d h", days, hours%24)
	}
	return fmt.Sprintf("%d d", days)
}

func NewOnlinePing(node string, lastOnline, now time.Time) OnlinePing {
	name := strings.TrimSpace(node)
	if name == "" {
		name = "Your desktop"
	}
	body := "ForkMesh started on this desktop and reached the relay."
	if !lastOnline.IsZero() && now.After(lastOnline) {
		body = "Back online after " + OfflineSpan(now.Sub(lastOnline)) + " offline."
	}
	return OnlinePing{PingID: strconv.FormatInt(now.UnixMilli(), 10) + "-online", Node: NodeToken(node),
		Kind: onlineKind, Title: name + " is online", Body: body}
}

func (p OnlinePing) resource() string {
	warning := "0"
	if p.Warning {
		warning = "1"
	}
	return strings.Join([]string{p.PingID, p.Node, p.Kind, p.Category, p.SessionID, p.Repo, warning, p.Title, p.Body}, "\n")
}

func OnlinePingURL(endpoint, owner string, id *identity.Identity, ping OnlinePing, now time.Time) (string, bool) {
	target, err := url.Parse(endpoint)
	if err != nil || target.Hostname() == "" || id == nil {
		return "", false
	}
	switch target.Scheme {
	case "wss":
		target.Scheme = "https"
	case "ws":
		target.Scheme = "http"
	default:
		return "", false
	}
	user := strings.ToLower(strings.TrimSpace(owner))
	if user == "" {
		return "", false
	}
	ts := strconv.FormatInt(now.UnixMilli(), 10)
	canonical := onlineProof + "\n" + user + "\n" + ping.resource() + "\n" + ts
	target.Path = onlinePath
	target.RawPath = ""
	target.Fragment = ""
	target.RawQuery = url.Values{"user": {user}, "ts": {ts}, "sig": {id.Sign([]byte(canonical))}}.Encode()
	return target.String(), true
}

func PostOnline(ctx context.Context, target, userAgent string, body []byte) (int, error) {
	ctx, cancel := context.WithTimeout(ctx, onlineTimeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, target, bytes.NewReader(body))
	if err != nil {
		return 0, err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("User-Agent", userAgent)
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		return 0, err
	}
	_ = response.Body.Close()
	return response.StatusCode, nil
}

func AnnounceAtStart(stopped time.Time, restart bool, now time.Time) bool {
	return !restart || stopped.IsZero() || now.Sub(stopped) >= OnlineGap
}

func (m *Module) linkChanged(connected bool) {
	now := m.Now()
	m.mu.Lock()
	if !connected {
		if m.online.connected && m.online.lost.IsZero() {
			m.online.lost = now
		}
		m.mu.Unlock()
		return
	}
	var since time.Time
	announce := false
	if !m.online.connected {
		m.online.connected = true
		stopped, restart := m.core.PreviousStop()
		since = stopped
		announce = AnnounceAtStart(stopped, restart, now)
	} else if !m.online.lost.IsZero() && now.Sub(m.online.lost) >= OnlineGap {
		since = m.online.lost
		announce = true
	}
	m.online.lost = time.Time{}
	config, id := m.config, m.identity
	m.mu.Unlock()
	if announce {
		go m.announceOnline(config, id, since, now)
	}
}

func (m *Module) announceOnline(config Config, id *identity.Identity, since, now time.Time) {
	settings := m.core.Settings.Get()
	if !settings.Bool(phonePingsSetting, true) || !settings.Bool(onlinePingSetting, true) {
		return
	}
	if !control.PhoneLinked(control.LoadDevices(control.DevicesPath(m.core.Layout.Data)), config.Owner, now) {
		return
	}
	ping := NewOnlinePing(config.Node, since, now)
	target, ok := OnlinePingURL(config.Endpoint, config.Owner, id, ping, m.Now())
	if !ok {
		return
	}
	body, _ := json.Marshal(ping)
	status, err := m.SendOnline(m.ctx, target, UserAgent(m.core.Version, config.Account, config.Node), body)
	switch {
	case err != nil:
		m.core.Logger.Printf("relay: could not tell your phone this desktop is online: %v", err)
	case status < 200 || status >= 300:
		m.core.Logger.Printf("relay: could not tell your phone this desktop is online (HTTP %d)", status)
	default:
		m.core.Logger.Printf("relay: told your phone %q", ping.Title)
	}
}
