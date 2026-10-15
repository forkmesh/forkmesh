package plugins

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxPluginLine   = 4 << 20
	maxLogBytes     = 1 << 20
	eventBurst      = 200
	stopGrace       = 3 * time.Second
	initMethod      = "plugin.init"
	shutdownMethod  = "plugin.shutdown"
	cancelMethod    = "$/cancel"
	progressEvent   = "$/progress"
	defaultCallWait = 2 * time.Minute
)

type reply struct {
	result json.RawMessage
	err    string
}

type process struct {
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	isolated bool
	done     chan struct{}
	exitErr  error
	writeMu  sync.Mutex
	mu       sync.Mutex
	pending  map[int64]chan reply
	progress map[int64]func(json.RawMessage)
	nextID   atomic.Int64
	stopped  atomic.Bool
	events   int
	window   time.Time
}

func (p *process) pid() int {
	if p.cmd.Process == nil {
		return 0
	}
	return p.cmd.Process.Pid
}

func pluginEnvironment(id, version, dataDir string, grants []string) []string {
	path := "/usr/local/bin:/usr/bin:/bin"
	if runtime.GOOS == "darwin" {
		path = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
	}
	env := []string{
		"PATH=" + path,
		"HOME=" + dataDir,
		"TMPDIR=" + filepath.Join(dataDir, "tmp"),
		"LANG=C.UTF-8",
		"FORKMESH_PLUGIN_ID=" + id,
		"FORKMESH_PLUGIN_VERSION=" + version,
		"FORKMESH_PLUGIN_DATA=" + dataDir,
		"FORKMESH_PLUGIN_API=" + fmt.Sprint(APIVersion),
		"FORKMESH_PLUGIN_GRANTS=" + strings.Join(grants, ","),
	}
	if runtime.GOOS == "windows" {
		for _, name := range []string{"SystemRoot", "SystemDrive", "WINDIR", "PATHEXT", "COMSPEC"} {
			if value := os.Getenv(name); value != "" {
				env = append(env, name+"="+value)
			}
		}
		env = append(env, "USERPROFILE="+dataDir, "TEMP="+filepath.Join(dataDir, "tmp"), "TMP="+filepath.Join(dataDir, "tmp"))
	}
	return env
}

func resolveInterpreter(name string) (string, error) {
	for _, dir := range []string{"/usr/bin", "/bin", "/usr/local/bin", "/opt/homebrew/bin"} {
		candidate := filepath.Join(dir, name)
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() && info.Mode()&0o111 != 0 {
			return candidate, nil
		}
	}
	return exec.LookPath(name)
}

type cappedLog struct {
	mu   sync.Mutex
	path string
	file *os.File
	size int64
}

func openCappedLog(path string) *cappedLog {
	log := &cappedLog{path: path}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err == nil {
		if info, err := os.Stat(path); err == nil && info.Size() > maxLogBytes {
			_ = os.Rename(path, path+".1")
		}
		log.file, _ = os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
		if info, err := os.Stat(path); err == nil {
			log.size = info.Size()
		}
	}
	return log
}

func (l *cappedLog) Write(data []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.file == nil {
		return len(data), nil
	}
	if l.size+int64(len(data)) > maxLogBytes {
		l.file.Close()
		_ = os.Rename(l.path, l.path+".1")
		l.file, _ = os.OpenFile(l.path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o600)
		l.size = 0
		if l.file == nil {
			return len(data), nil
		}
	}
	n, err := l.file.Write(data)
	l.size += int64(n)
	return len(data), err
}

func (l *cappedLog) Close() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.file != nil {
		l.file.Close()
		l.file = nil
	}
}

func (h *Host) launch(p *plugin, grants []string) (*process, error) {
	service := *p.manifest.Service
	execPath, err := ExecPath(p.dir, service)
	if err != nil {
		return nil, err
	}
	argv := []string{execPath}
	if service.Interpreter != "" {
		interpreter, err := resolveInterpreter(service.Interpreter)
		if err != nil {
			return nil, fmt.Errorf("plugins: interpreter %s not found", service.Interpreter)
		}
		argv = []string{interpreter, execPath}
	}
	dataDir := filepath.Join(h.options.DataDir, p.id)
	if err := os.MkdirAll(filepath.Join(dataDir, "tmp"), 0o700); err != nil {
		return nil, err
	}
	logFile := openCappedLog(filepath.Join(h.options.LogDir, p.id+".log"))
	isolate := !subset([]string{PermissionNetwork}, grants) && networkIsolationSupported()
	start := func(isolateNetwork bool) (*process, error) {
		program, args := argv[0], argv[1:]
		if UsesShim() && h.options.ShimBinary != "" {
			program, args = h.options.ShimBinary, ShimArgs(service.MemoryMB, argv)
		}
		cmd := exec.Command(program, args...)
		cmd.Dir = dataDir
		cmd.Env = pluginEnvironment(p.id, p.manifest.Version, dataDir, grants)
		cmd.Stderr = logFile
		configureSandbox(cmd, isolateNetwork)
		stdin, err := cmd.StdinPipe()
		if err != nil {
			return nil, err
		}
		stdout, writer, err := os.Pipe()
		if err != nil {
			return nil, err
		}
		cmd.Stdout = writer
		if err := cmd.Start(); err != nil {
			stdout.Close()
			writer.Close()
			return nil, err
		}
		writer.Close()
		proc := &process{cmd: cmd, stdin: stdin, isolated: isolateNetwork, done: make(chan struct{}),
			pending: map[int64]chan reply{}, progress: map[int64]func(json.RawMessage){}}
		go func() {
			proc.readLoop(stdout, h, p, logFile)
			stdout.Close()
		}()
		go func() {
			proc.exitErr = cmd.Wait()
			logFile.Close()
			proc.failPending()
			close(proc.done)
		}()
		return proc, nil
	}
	proc, err := start(isolate)
	if err != nil && isolate {
		h.logf("%s: network isolation unavailable (%v); starting without it", p.id, err)
		proc, err = start(false)
	}
	if err != nil {
		logFile.Close()
		return nil, fmt.Errorf("plugins: start %s: %w", p.id, err)
	}
	ctx, cancel := context.WithTimeout(h.ctx, h.options.StartTimeout)
	defer cancel()
	params, _ := json.Marshal(map[string]any{"api": APIVersion, "id": p.id, "version": p.manifest.Version,
		"dataDir": dataDir, "grants": grants, "networkIsolated": proc.isolated})
	if _, err := proc.call(ctx, initMethod, params, nil); err != nil {
		proc.stop()
		return nil, fmt.Errorf("plugins: %s did not initialise: %v", p.id, err)
	}
	return proc, nil
}

func (p *process) write(value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	_, err = p.stdin.Write(append(raw, '\n'))
	return err
}

func (p *process) call(ctx context.Context, method string, params json.RawMessage, progress func(json.RawMessage)) (json.RawMessage, error) {
	id := p.nextID.Add(1)
	wait := make(chan reply, 1)
	p.mu.Lock()
	select {
	case <-p.done:
		p.mu.Unlock()
		return nil, rpc.Fail("plugin_gone")
	default:
	}
	p.pending[id] = wait
	if progress != nil {
		p.progress[id] = progress
	}
	p.mu.Unlock()
	forget := func() {
		p.mu.Lock()
		delete(p.pending, id)
		delete(p.progress, id)
		p.mu.Unlock()
	}
	if len(params) == 0 {
		params = json.RawMessage("{}")
	}
	if err := p.write(map[string]any{"id": id, "method": method, "params": params}); err != nil {
		forget()
		return nil, rpc.Fail("plugin_gone")
	}
	timer := time.NewTimer(defaultCallWait)
	defer timer.Stop()
	select {
	case answer, ok := <-wait:
		forget()
		if !ok {
			return nil, rpc.Fail("plugin_gone")
		}
		if answer.err != "" {
			return nil, rpc.Fail(answer.err)
		}
		return answer.result, nil
	case <-ctx.Done():
		forget()
		_ = p.write(map[string]any{"method": cancelMethod, "params": map[string]any{"id": id}})
		return nil, ctx.Err()
	case <-timer.C:
		forget()
		_ = p.write(map[string]any{"method": cancelMethod, "params": map[string]any{"id": id}})
		return nil, rpc.Fail("plugin_timeout")
	}
}

func (p *process) failPending() {
	p.mu.Lock()
	defer p.mu.Unlock()
	for id, wait := range p.pending {
		close(wait)
		delete(p.pending, id)
	}
}

func (p *process) readLoop(stdout io.Reader, h *Host, owner *plugin, log io.Writer) {
	reader := bufio.NewReaderSize(stdout, 64<<10)
	for {
		line, err := readBounded(reader)
		if err != nil {
			if errors.Is(err, errLineTooLong) {
				fmt.Fprintf(log, "forkmesh-core: line over %d bytes; stopping the plug-in\n", maxPluginLine)
				go p.stop()
			}
			return
		}
		if len(line) == 0 {
			continue
		}
		var message rpc.Message
		if json.Unmarshal(line, &message) != nil {
			fmt.Fprintf(log, "forkmesh-core: ignored non-JSON stdout line\n")
			continue
		}
		switch {
		case message.IsResponse():
			p.mu.Lock()
			wait := p.pending[message.ID]
			delete(p.pending, message.ID)
			p.mu.Unlock()
			if wait != nil {
				wait <- reply{result: message.Result, err: errorCode(message)}
			}
		case message.Event == progressEvent:
			var data struct {
				ID int64 `json:"id"`
			}
			_ = json.Unmarshal(message.Data, &data)
			p.mu.Lock()
			send := p.progress[data.ID]
			p.mu.Unlock()
			if send != nil {
				send(message.Data)
			}
		case message.Event != "":
			if p.allowEvent() {
				h.emit(owner, message.Event, message.Data)
			}
		case message.Method != "":
			result, err := hostRequest(h, owner, message, log)
			if message.ID > 0 {
				_ = p.write(rpc.Response(message.ID, result, err))
			}
		}
	}
}

func errorCode(message rpc.Message) string {
	if message.OK != nil && !*message.OK {
		if message.Error == "" {
			return "plugin_error"
		}
		return message.Error
	}
	return ""
}

func (p *process) allowEvent() bool {
	now := time.Now()
	if now.Sub(p.window) > time.Second {
		p.window = now
		p.events = 0
	}
	p.events++
	return p.events <= eventBurst
}

func hostRequest(h *Host, owner *plugin, message rpc.Message, log io.Writer) (any, error) {
	switch message.Method {
	case "host.log":
		var params struct {
			Text string `json:"text"`
		}
		_ = json.Unmarshal(message.Params, &params)
		text := params.Text
		if len(text) > 4096 {
			text = text[:4096]
		}
		fmt.Fprintf(log, "%s %s\n", time.Now().Format(time.RFC3339), strings.TrimSpace(text))
		return map[string]any{}, nil
	case "host.info":
		info := map[string]any{"id": owner.id, "version": owner.manifest.Version, "api": APIVersion}
		if h.core != nil {
			info["coreVersion"] = h.core.Version
			info["project"] = h.core.Layout.Project
			info["background"] = h.core.Background()
			info["ui"] = h.core.UI() != nil
		}
		return info, nil
	}
	return nil, rpc.Fail("forbidden")
}

var errLineTooLong = errors.New("plugins: line too long")

func readBounded(reader *bufio.Reader) ([]byte, error) {
	var line []byte
	for {
		chunk, isPrefix, err := reader.ReadLine()
		line = append(line, chunk...)
		if len(line) > maxPluginLine {
			return nil, errLineTooLong
		}
		if err != nil {
			return nil, err
		}
		if !isPrefix {
			return line, nil
		}
	}
}

func (p *process) wait() error {
	<-p.done
	return p.exitErr
}

func (p *process) stop() {
	if !p.stopped.CompareAndSwap(false, true) {
		<-p.done
		return
	}
	_ = p.write(map[string]any{"method": shutdownMethod})
	_ = p.stdin.Close()
	select {
	case <-p.done:
		return
	case <-time.After(stopGrace):
	}
	if pid := p.pid(); pid > 0 {
		killGroup(pid, syscall.SIGTERM)
	}
	select {
	case <-p.done:
		return
	case <-time.After(2 * time.Second):
	}
	if pid := p.pid(); pid > 0 {
		killGroup(pid, syscall.SIGKILL)
	}
	if p.cmd.Process != nil {
		_ = p.cmd.Process.Kill()
	}
	<-p.done
}
