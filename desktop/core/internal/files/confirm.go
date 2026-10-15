package files

import (
	"crypto/rand"
	"encoding/hex"
	"strings"
	"sync"
)

const (
	confirmationLifetimeMs = 60 * 1000
	maxOutstandingTokens   = 32
)

type verdict int

const (
	verdictOK verdict = iota
	verdictMissing
	verdictExpired
	verdictMismatch
)

func confirmationError(result verdict) string {
	switch result {
	case verdictOK:
		return ""
	case verdictMissing:
		return "confirmation_required"
	case verdictExpired:
		return "confirmation_expired"
	case verdictMismatch:
		return "confirmation_mismatch"
	}
	return "confirmation_required"
}

type confirmationEntry struct {
	deviceKey string
	action    string
	subject   string
	expiresAt int64
}

type confirmations struct {
	mu     sync.Mutex
	tokens map[string]confirmationEntry
}

func newConfirmations() *confirmations {
	return &confirmations{tokens: map[string]confirmationEntry{}}
}

func newToken() string {
	raw := make([]byte, 24)
	_, _ = rand.Read(raw)
	return hex.EncodeToString(raw)
}

func (c *confirmations) pruneLocked(nowMs int64) {
	for token, entry := range c.tokens {
		if entry.expiresAt <= nowMs {
			delete(c.tokens, token)
		}
	}
}

func (c *confirmations) issue(deviceKey, action, subject string, nowMs int64) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.pruneLocked(nowMs)
	for len(c.tokens) >= maxOutstandingTokens {
		oldest := ""
		var oldestAt int64
		for token, entry := range c.tokens {
			if oldest == "" || entry.expiresAt < oldestAt {
				oldest, oldestAt = token, entry.expiresAt
			}
		}
		delete(c.tokens, oldest)
	}
	token := newToken()
	for _, taken := c.tokens[token]; taken; _, taken = c.tokens[token] {
		token = newToken()
	}
	c.tokens[token] = confirmationEntry{deviceKey: deviceKey, action: action, subject: subject,
		expiresAt: nowMs + confirmationLifetimeMs}
	return token
}

func (c *confirmations) consume(token, deviceKey, action, subject string, nowMs int64) verdict {
	c.mu.Lock()
	defer c.mu.Unlock()
	entry, ok := c.tokens[token]
	if token == "" || !ok {
		c.pruneLocked(nowMs)
		return verdictMissing
	}
	delete(c.tokens, token)
	c.pruneLocked(nowMs)
	if entry.expiresAt <= nowMs {
		return verdictExpired
	}
	if entry.deviceKey != deviceKey || entry.action != action || !strings.EqualFold(entry.subject, subject) {
		return verdictMismatch
	}
	return verdictOK
}

func (c *confirmations) outstanding() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.tokens)
}
