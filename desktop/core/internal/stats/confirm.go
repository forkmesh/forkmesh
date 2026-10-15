package stats

import (
	"crypto/rand"
	"encoding/hex"
	"sync"
)

const (
	confirmationLifetimeMs     = 60 * 1000
	confirmationMaxOutstanding = 32
)

type confirmResult int

const (
	confirmOK confirmResult = iota
	confirmMissing
	confirmExpired
	confirmMismatch
)

func confirmationError(result confirmResult) string {
	switch result {
	case confirmOK:
		return ""
	case confirmExpired:
		return "confirmation_expired"
	case confirmMismatch:
		return "confirmation_mismatch"
	}
	return "confirmation_required"
}

type ratchetIntent struct {
	holder    string
	repo      string
	enabled   bool
	expiresAt int64
}

type ratchetConfirmations struct {
	mu      sync.Mutex
	intents map[string]ratchetIntent
}

func newRatchetConfirmations() *ratchetConfirmations {
	return &ratchetConfirmations{intents: map[string]ratchetIntent{}}
}

func newToken() string {
	var raw [24]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func ratchetAction(enabled bool) string {
	if enabled {
		return "ratchet.on"
	}
	return "ratchet.off"
}

func (c *ratchetConfirmations) prune(nowMs int64) {
	for token, intent := range c.intents {
		if intent.expiresAt <= nowMs {
			delete(c.intents, token)
		}
	}
}

func (c *ratchetConfirmations) issue(holder, repo string, enabled bool, nowMs int64) (string, int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.prune(nowMs)
	for len(c.intents) >= confirmationMaxOutstanding {
		oldest := ""
		var oldestAt int64
		for token, intent := range c.intents {
			if oldest == "" || intent.expiresAt < oldestAt {
				oldest, oldestAt = token, intent.expiresAt
			}
		}
		delete(c.intents, oldest)
	}
	token := newToken()
	expires := nowMs + confirmationLifetimeMs
	c.intents[token] = ratchetIntent{holder: holder, repo: repo, enabled: enabled, expiresAt: expires}
	return token, expires
}

func (c *ratchetConfirmations) consume(token, holder string, nowMs int64) (confirmResult, string, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	intent, ok := c.intents[token]
	if token == "" || !ok || intent.repo == "" {
		delete(c.intents, token)
		c.prune(nowMs)
		return confirmMissing, "", false
	}
	delete(c.intents, token)
	c.prune(nowMs)
	if intent.expiresAt <= nowMs {
		return confirmExpired, "", false
	}
	if intent.holder != holder {
		return confirmMismatch, "", false
	}
	return confirmOK, intent.repo, intent.enabled
}

func (c *ratchetConfirmations) outstanding() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.intents)
}
