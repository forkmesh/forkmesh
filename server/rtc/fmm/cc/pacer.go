package cc

const (
	ClassAudio = 0
	ClassRTX   = 1
	ClassVideo = 2
	classes    = 3

	PacingFactor = 2.5

	CongestedPacingFactor = 1.25
	CongestedVideoQueueUs = 200_000
	MaxVideoQueueUs       = 400_000
	maxBurstUs            = 10_000
	drainTargetUs         = 500_000
	maxQueueBytes         = 2 << 20
	PaddingSize           = 1200
	maxProbeBudget        = 64
)

type queued struct {
	data []byte
	at   int64
}

type fifo struct {
	items []queued
	head  int
}

func (f *fifo) push(q queued) { f.items = append(f.items, q) }

func (f *fifo) empty() bool { return f.head >= len(f.items) }

func (f *fifo) peek() *queued { return &f.items[f.head] }

func (f *fifo) pop() queued {
	q := f.items[f.head]
	f.items[f.head] = queued{}
	f.head++
	if f.head > 64 && f.head*2 > len(f.items) {
		n := copy(f.items, f.items[f.head:])
		f.items = f.items[:n]
		f.head = 0
	}
	return q
}

type Pacer struct {
	q           [classes]fifo
	rate        float64
	budget      float64
	last        int64
	queuedBytes int
	probeRate   float64
	probeLeft   int
	probeSize   int
	probeSent   int
	drainCap    float64
	Dropped     int
}

func (p *Pacer) SetDrainCap(bps int) { p.drainCap = float64(bps) }

func (p *Pacer) DropOlder(ageUs int64, now int64) {
	for _, c := range []int{ClassRTX, ClassVideo} {
		for !p.q[c].empty() && now-p.q[c].peek().at > ageUs {
			d := p.q[c].pop()
			p.queuedBytes -= len(d.data)
			p.Dropped++
		}
	}
}

func (p *Pacer) SetRate(bps int) { p.rate = float64(bps) }

func (p *Pacer) SetProbe(bps, bytes int) {
	p.probeRate = float64(bps)
	p.probeLeft = bytes
	p.probeSize = bytes / probeMinPackets
	p.probeSent = 0
}

func (p *Pacer) Probing() bool { return p.probeLeft > 0 }

func (p *Pacer) QueuedBytes() int { return p.queuedBytes }

func (p *Pacer) OldestUs(now int64) int64 {
	var o int64
	for i := range p.q {
		if !p.q[i].empty() {
			o = max(o, now-p.q[i].peek().at)
		}
	}
	return o
}

func (p *Pacer) Push(class int, b []byte, now int64) {
	class = min(max(class, 0), classes-1)
	p.q[class].push(queued{data: b, at: now})
	p.queuedBytes += len(b)
	for p.queuedBytes > maxQueueBytes && !p.q[ClassVideo].empty() {
		d := p.q[ClassVideo].pop()
		p.queuedBytes -= len(d.data)
		p.Dropped++
	}
}

func (p *Pacer) Tick(now int64, send func(b []byte, probe bool), pad func(size int) []byte) {
	if p.last == 0 {
		p.last = now
	}
	dt := float64(now - p.last)
	p.last = now
	rate := p.rate
	if p.probeLeft > 0 {
		rate = max(rate, p.probeRate)
	}
	for _, c := range []int{ClassRTX, ClassVideo} {
		for !p.q[c].empty() && now-p.q[c].peek().at > MaxVideoQueueUs {
			d := p.q[c].pop()
			p.queuedBytes -= len(d.data)
			p.Dropped++
		}
	}
	drain := float64(p.queuedBytes) * 8e6 / drainTargetUs
	if p.drainCap > 0 {
		drain = min(drain, max(p.drainCap, rate))
	}
	rate = max(rate, drain)
	p.budget += rate * dt / 8e6
	p.budget = min(p.budget, rate*maxBurstUs/8e6+PaddingSize)
	for p.budget > 0 {
		var item queued
		for c := range p.q {
			if !p.q[c].empty() {
				item = p.q[c].pop()
				break
			}
		}
		if item.data == nil {
			break
		}
		p.queuedBytes -= len(item.data)
		p.budget -= float64(len(item.data))
		probing := p.probeLeft > 0
		if probing {
			p.probeSent++
			p.probeLeft = max(p.probeLeft-len(item.data), (probeMinPackets-p.probeSent)*lowProbePacket)
		}
		send(item.data, probing)
	}
	for n := 0; p.probeLeft > 0 && p.budget > 0 && pad != nil && n < maxProbeBudget; n++ {
		b := pad(min(max(min(p.probeSize, p.probeLeft), lowProbePacket), PaddingSize))
		p.budget -= float64(len(b))
		p.probeSent++
		p.probeLeft = max(p.probeLeft-len(b), (probeMinPackets-p.probeSent)*lowProbePacket)
		send(b, true)
	}
	if p.probeLeft <= 0 {
		p.probeLeft = 0
	}
	for !p.q[ClassAudio].empty() && p.budget > -float64(PaddingSize) {
		item := p.q[ClassAudio].pop()
		p.queuedBytes -= len(item.data)
		p.budget -= float64(len(item.data))
		send(item.data, false)
	}
}
