package control

const (
	DefaultRelayControlWindowMs = 10000
	DefaultRelayControlMax      = 200
)

type SendBudget struct {
	windowMs int64
	limit    int
	sent     []int64
}

func NewSendBudget() *SendBudget {
	return &SendBudget{windowMs: 10000, limit: 180}
}

func (b *SendBudget) Configure(windowMs int64, maximum int) {
	b.windowMs = max(1000, windowMs)
	top := max(2, maximum)
	b.limit = max(1, top-max(1, top/10))
}

func (b *SendBudget) WaitMs(nowMs int64) int64 {
	for len(b.sent) > 0 && nowMs-b.sent[0] >= b.windowMs {
		b.sent = b.sent[1:]
	}
	if len(b.sent) < b.limit {
		return 0
	}
	return max(1, b.sent[0]+b.windowMs-nowMs)
}

func (b *SendBudget) Spend(nowMs int64) { b.sent = append(b.sent, nowMs) }

func (b *SendBudget) Reset() { b.sent = nil }

func (b *SendBudget) Limit() int { return b.limit }

func (b *SendBudget) WindowMs() int64 { return b.windowMs }

func FollowIntervalMs(relayed bool) int {
	if relayed {
		return 80
	}
	return 15
}
