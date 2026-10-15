package update

import (
	"crypto/rand"
	"encoding/hex"
	"strings"
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

type intent struct {
	device    string
	action    string
	subject   string
	expiresAt int64
}

type confirmations struct {
	mu      sync.Mutex
	intents map[string]intent
}

func newConfirmations() *confirmations {
	return &confirmations{intents: map[string]intent{}}
}

func newToken() string {
	var raw [24]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func (c *confirmations) prune(nowMs int64) {
	for token, item := range c.intents {
		if item.expiresAt <= nowMs {
			delete(c.intents, token)
		}
	}
}

func (c *confirmations) issue(device, action, subject string, nowMs int64) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.prune(nowMs)
	for len(c.intents) >= confirmationMaxOutstanding {
		oldest := ""
		var oldestAt int64
		for token, item := range c.intents {
			if oldest == "" || item.expiresAt < oldestAt {
				oldest, oldestAt = token, item.expiresAt
			}
		}
		delete(c.intents, oldest)
	}
	token := newToken()
	c.intents[token] = intent{device: device, action: action, subject: subject, expiresAt: nowMs + confirmationLifetimeMs}
	return token
}

func (c *confirmations) consume(token, device, action, subject string, nowMs int64) confirmResult {
	c.mu.Lock()
	defer c.mu.Unlock()
	item, ok := c.intents[token]
	if token == "" || !ok {
		c.prune(nowMs)
		return confirmMissing
	}
	delete(c.intents, token)
	c.prune(nowMs)
	if item.expiresAt <= nowMs {
		return confirmExpired
	}
	if item.device != device || item.action != action || !strings.EqualFold(item.subject, subject) {
		return confirmMismatch
	}
	return confirmOK
}
