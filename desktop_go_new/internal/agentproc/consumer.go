package agentproc

import (
	"path/filepath"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

type Consumer interface {
	Stdout(data []byte, offset int64)
	Stdin(data []byte)
	Exit(exit map[string]any)
}

const SecretsKey = "_secrets"

type StderrConsumer interface {
	Stderr(data []byte, offset int64)
}

func takesStderr(consumer Consumer) bool {
	_, ok := consumer.(StderrConsumer)
	return ok
}

type ConsumerFactory func(key string, meta map[string]any, ack int64) (Consumer, int64, bool)

type sinkPeer struct {
	id   string
	done chan struct{}
}

func (s *sinkPeer) ID() string               { return s.id }
func (s *sinkPeer) Transport() rpc.Transport { return rpc.CLI }
func (s *sinkPeer) Device() rpc.Device       { return rpc.Device{Name: "forkmesh-core"} }
func (s *sinkPeer) Send(string, any) bool    { return true }
func (s *sinkPeer) Done() <-chan struct{}    { return s.done }

func allExitSent(p *proc) bool {
	for _, a := range p.attached {
		if !a.exitSent {
			return false
		}
	}
	return true
}

func (m *Module) SetConsumerFactory(factory ConsumerFactory) {
	m.mu.Lock()
	m.factory = factory
	all := make([]*proc, 0, len(m.procs))
	for _, p := range m.procs {
		all = append(all, p)
	}
	m.mu.Unlock()
	for _, p := range all {
		m.attachConsumer(p)
	}
}

func (m *Module) attachConsumer(p *proc) {
	m.mu.Lock()
	factory := m.factory
	if factory == nil || p.consumer != nil || p.forgotten {
		m.mu.Unlock()
		return
	}
	meta := make(map[string]any, len(p.meta))
	for key, value := range p.meta {
		meta[key] = value
	}
	if len(p.secrets) > 0 {
		secrets := make([]any, len(p.secrets))
		for i, secret := range p.secrets {
			secrets[i] = secret
		}
		meta[SecretsKey] = secrets
	}
	key, ack := p.key, p.ack.Stdout
	m.mu.Unlock()
	consumer, from, ok := factory(key, meta, ack)
	if !ok || consumer == nil {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if p.consumer != nil || p.forgotten {
		return
	}
	p.consumer = consumer
	if from < p.ack.Stdout {
		from = p.ack.Stdout
	}
	peer := &sinkPeer{id: "agentrun:" + key, done: make(chan struct{})}
	a := m.addAttachment(p, peer, from, -1, 0)
	a.sink = consumer
	a.skip[1] = !takesStderr(consumer)
}

func (m *Module) AckStderr(key string, stderr int64) bool {
	m.mu.Lock()
	p := m.procs[key]
	if p == nil {
		m.mu.Unlock()
		return false
	}
	if stderr > p.known[1] {
		stderr = p.known[1]
	}
	if stderr <= p.ack.Stderr {
		m.mu.Unlock()
		return true
	}
	p.ack.Stderr = stderr
	p.ack.Ms = time.Now().UnixMilli()
	ack, dir := p.ack, p.dir
	m.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "ack.json"), ack); err != nil {
		return false
	}
	trimSpool(dir, streamErr, ack.Stderr)
	return true
}

func (m *Module) ReadStderr(key string, from, to int64) ([]byte, int64) {
	m.mu.Lock()
	p := m.procs[key]
	if p == nil {
		m.mu.Unlock()
		return nil, from
	}
	dir := p.dir
	if to > p.known[1] {
		to = p.known[1]
	}
	m.mu.Unlock()
	var out []byte
	start := int64(-1)
	for from < to {
		data, at, err := readSpool(dir, streamErr, from, to-from)
		if err != nil || len(data) == 0 {
			break
		}
		if start < 0 {
			start = at
		}
		out = append(out, data...)
		from = at + int64(len(data))
	}
	if start < 0 {
		start = from
	}
	return out, start
}

func (m *Module) Ack(key string, stdout int64) bool {
	m.mu.Lock()
	p := m.procs[key]
	if p == nil {
		m.mu.Unlock()
		return false
	}
	if stdout > p.known[0] {
		stdout = p.known[0]
	}
	if stdout <= p.ack.Stdout {
		m.mu.Unlock()
		return true
	}
	p.ack.Stdout = stdout
	p.ack.Ms = time.Now().UnixMilli()
	ack, dir := p.ack, p.dir
	m.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "ack.json"), ack); err != nil {
		return false
	}
	trimSpool(dir, streamOut, ack.Stdout)
	return true
}

func (m *Module) MergeMeta(key string, meta map[string]any) bool {
	m.mu.Lock()
	p := m.procs[key]
	if p == nil {
		m.mu.Unlock()
		return false
	}
	m.mu.Unlock()
	p.metaMu.Lock()
	defer p.metaMu.Unlock()
	m.mu.Lock()
	if p.meta == nil {
		p.meta = map[string]any{}
	}
	for k, v := range meta {
		if v == nil {
			delete(p.meta, k)
		} else {
			p.meta[k] = v
		}
	}
	snapshot := make(map[string]any, len(p.meta))
	for k, v := range p.meta {
		snapshot[k] = v
	}
	dir := p.dir
	m.mu.Unlock()
	return writeJSONFile(filepath.Join(dir, "meta.json"), snapshot) == nil
}

func (m *Module) Running(key string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	p := m.procs[key]
	return p != nil && p.running()
}

func (m *Module) Info(key string) (map[string]any, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	p := m.procs[key]
	if p == nil {
		return nil, false
	}
	return p.info(), true
}

func (m *Module) WriteStdin(key string, data []byte, observe bool) error {
	p, err := m.lookup(key)
	if err != nil {
		return err
	}
	m.mu.Lock()
	consumer := p.consumer
	m.mu.Unlock()
	if consumer != nil && observe {
		consumer.Stdin(data)
	}
	for len(data) > 0 {
		chunk := data
		if len(chunk) > writeChunk {
			chunk = data[:writeChunk]
		}
		if err := m.sendFrame(p, frameWrite, chunk); err != nil {
			return err
		}
		data = data[len(chunk):]
	}
	return nil
}

func (m *Module) SetLauncher(shim func() (string, []string), environ func() []string) {
	if shim != nil {
		m.shimCommand = shim
	}
	if environ != nil {
		m.environ = environ
	}
}

func (m *Module) SetRoot(root string) { m.root = root }

func (m *Module) CloseStdin(key string) error {
	p, err := m.lookup(key)
	if err != nil {
		return err
	}
	return m.sendFrame(p, frameClose, nil)
}

func (m *Module) StopProcess(key string, graceMs int) error {
	p, err := m.lookup(key)
	if err != nil {
		return err
	}
	if graceMs <= 0 {
		graceMs = 1500
	}
	return m.sendFrame(p, frameStop, int32Payload(graceMs))
}
