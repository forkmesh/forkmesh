package phonefamily

import (
	"crypto/rand"
	"encoding/hex"
	"strings"
	"sync"
)

const (
	ConfirmationLifetimeMs = 60 * 1000
	maxOutstanding         = 32
)

type Verdict int

const (
	Ok Verdict = iota
	Missing
	Expired
	Mismatch
)

func ConfirmationError(verdict Verdict) string {
	switch verdict {
	case Ok:
		return ""
	case Expired:
		return "confirmation_expired"
	case Mismatch:
		return "confirmation_mismatch"
	}
	return "confirmation_required"
}

type confirmation struct {
	holder    string
	action    string
	target    string
	expiresAt int64
}

type Confirmations struct {
	mu     sync.Mutex
	tokens map[string]confirmation
}

func NewConfirmations() *Confirmations {
	return &Confirmations{tokens: map[string]confirmation{}}
}

func newToken() string {
	var raw [24]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func (c *Confirmations) prune(nowMs int64) {
	for token, entry := range c.tokens {
		if entry.expiresAt <= nowMs {
			delete(c.tokens, token)
		}
	}
}

func (c *Confirmations) Issue(holder, action, target string, nowMs int64) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.tokens == nil {
		c.tokens = map[string]confirmation{}
	}
	c.prune(nowMs)
	for len(c.tokens) >= maxOutstanding {
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
	c.tokens[token] = confirmation{holder: holder, action: action, target: target, expiresAt: nowMs + ConfirmationLifetimeMs}
	return token
}

func (c *Confirmations) Consume(token, holder, action, target string, nowMs int64) Verdict {
	c.mu.Lock()
	defer c.mu.Unlock()
	entry, ok := c.tokens[token]
	if token == "" || !ok {
		c.prune(nowMs)
		return Missing
	}
	delete(c.tokens, token)
	c.prune(nowMs)
	if entry.expiresAt <= nowMs {
		return Expired
	}
	if entry.holder != holder || entry.action != action || !strings.EqualFold(entry.target, target) {
		return Mismatch
	}
	return Ok
}

func (c *Confirmations) Outstanding() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.tokens)
}
