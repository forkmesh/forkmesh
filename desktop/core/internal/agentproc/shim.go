package agentproc

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

type shimLaunch struct {
	Program      string   `json:"program"`
	Args         []string `json:"args"`
	Env          []string `json:"env"`
	Cwd          string   `json:"cwd"`
	Merge        bool     `json:"merge"`
	Socket       string   `json:"socket"`
	Transport    string   `json:"transport,omitempty"`
	SegmentBytes int64    `json:"segmentBytes"`
	CapBytes     int64    `json:"capBytes"`
	DrainMs      int      `json:"drainMs"`
}

type runInfo struct {
	Protocol  int    `json:"protocol"`
	ShimPid   int    `json:"shimPid"`
	Pid       int    `json:"pid"`
	Pgid      int    `json:"pgid"`
	StartedMs int64  `json:"startedMs"`
	Boot      string `json:"boot,omitempty"`
}

type exitInfo struct {
	Code     int   `json:"code"`
	Signal   int   `json:"signal,omitempty"`
	Crashed  bool  `json:"crashed"`
	Lost     bool  `json:"lost,omitempty"`
	ExitedMs int64 `json:"exitedMs"`
	Stdout   int64 `json:"stdout"`
	Stderr   int64 `json:"stderr"`
}

type helloInfo struct {
	runInfo
	Stdout int64     `json:"stdout"`
	Stderr int64     `json:"stderr"`
	Exit   *exitInfo `json:"exit,omitempty"`
}

type handshake struct {
	Pid     int    `json:"pid,omitempty"`
	ShimPid int    `json:"shimPid,omitempty"`
	Error   string `json:"error,omitempty"`
}

func writeJSONFile(path string, value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func readJSONFile(path string, target any) bool {
	raw, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	return json.Unmarshal(raw, target) == nil
}

func isExecutable(path string) bool {
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return false
	}
	return runtime.GOOS == "windows" || info.Mode()&0o111 != 0
}

func resolveProgram(program string, env []string) (string, error) {
	if program == "" {
		return "", errors.New("no program")
	}
	if strings.ContainsRune(program, '/') || strings.ContainsRune(program, os.PathSeparator) {
		return program, nil
	}
	if runtime.GOOS != "windows" {
		if path, ok := envValue(env, "PATH"); ok {
			for _, dir := range filepath.SplitList(path) {
				if dir == "" {
					dir = "."
				}
				if candidate := filepath.Join(dir, program); isExecutable(candidate) {
					return candidate, nil
				}
			}
		}
	}
	return exec.LookPath(program)
}

type stdinQueue struct {
	mu     sync.Mutex
	cond   *sync.Cond
	chunks [][]byte
	closed bool
	file   *os.File
}

func newStdinQueue(file *os.File) *stdinQueue {
	q := &stdinQueue{file: file}
	q.cond = sync.NewCond(&q.mu)
	return q
}

func (q *stdinQueue) push(data []byte) {
	q.mu.Lock()
	if !q.closed && len(data) > 0 {
		q.chunks = append(q.chunks, data)
	}
	q.mu.Unlock()
	q.cond.Broadcast()
}

func (q *stdinQueue) close() {
	q.mu.Lock()
	q.closed = true
	q.mu.Unlock()
	q.cond.Broadcast()
}

func (q *stdinQueue) run() {
	for {
		q.mu.Lock()
		for len(q.chunks) == 0 && !q.closed {
			q.cond.Wait()
		}
		if len(q.chunks) == 0 {
			q.mu.Unlock()
			_ = q.file.Close()
			return
		}
		chunk := q.chunks[0]
		q.chunks = q.chunks[1:]
		q.mu.Unlock()
		if _, err := q.file.Write(chunk); err != nil {
			q.mu.Lock()
			q.chunks = nil
			q.closed = true
			q.mu.Unlock()
			_ = q.file.Close()
			return
		}
	}
}

type shim struct {
	mu        sync.Mutex
	dir       string
	out       *spoolWriter
	err       *spoolWriter
	run       runInfo
	exit      *exitInfo
	stdin     *stdinQueue
	clients   chan net.Conn
	wake      chan struct{}
	delivered chan struct{}
	deliverMu sync.Once
	logger    *log.Logger
}

func (s *shim) poke() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

func (s *shim) markDelivered() {
	s.deliverMu.Do(func() { close(s.delivered) })
}

func (s *shim) snapshot() helloInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	hello := helloInfo{runInfo: s.run, Stdout: s.out.total, Stderr: s.err.total}
	if s.exit != nil {
		exit := *s.exit
		hello.Exit = &exit
	}
	return hello
}

func (s *shim) notifier() {
	var conn net.Conn
	var sentOut, sentErr int64 = -1, -1
	exitSent := false
	send := func(kind byte, payload []byte) bool {
		if conn == nil {
			return false
		}
		_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if err := writeFrame(conn, kind, payload); err != nil {
			conn.Close()
			conn = nil
			return false
		}
		return true
	}
	for {
		select {
		case next := <-s.clients:
			if conn != nil {
				conn.Close()
			}
			conn = next
			hello := s.snapshot()
			raw, _ := json.Marshal(hello)
			if !send(frameHello, raw) {
				continue
			}
			sentOut, sentErr = hello.Stdout, hello.Stderr
			exitSent = hello.Exit != nil
			if exitSent {
				s.markDelivered()
			}
		case <-s.wake:
		}
		if conn == nil {
			s.mu.Lock()
			exited := s.exit != nil
			s.mu.Unlock()
			if exited {
				s.markDelivered()
			}
			continue
		}
		hello := s.snapshot()
		if hello.Stdout != sentOut || hello.Stderr != sentErr {
			if !send(frameData, lengthsPayload(hello.Stdout, hello.Stderr)) {
				continue
			}
			sentOut, sentErr = hello.Stdout, hello.Stderr
		}
		if hello.Exit != nil && !exitSent {
			raw, _ := json.Marshal(hello.Exit)
			if send(frameExit, raw) {
				exitSent = true
				s.markDelivered()
			}
		}
	}
}

func (s *shim) serveClient(conn net.Conn, token string) {
	reader := bufio.NewReaderSize(conn, 256<<10)
	if !shimAuthorize(conn, reader, token) {
		conn.Close()
		return
	}
	s.clients <- conn
	for {
		kind, payload, err := readFrame(reader)
		if err != nil {
			return
		}
		s.command(kind, payload)
	}
}

func (s *shim) command(kind byte, payload []byte) {
	s.mu.Lock()
	pid, pgid, exited := s.run.Pid, s.run.Pgid, s.exit != nil
	s.mu.Unlock()
	switch kind {
	case frameWrite:
		s.stdin.push(payload)
	case frameClose:
		s.stdin.close()
	case frameSignal:
		if !exited {
			_ = signalPid(pid, payloadInt32(payload))
		}
	case frameGroup:
		if !exited {
			_ = signalGroup(pgid, payloadInt32(payload))
		}
	case frameKill:
		if !exited {
			s.killAll(pid, pgid)
		}
	case frameStop:
		if !exited {
			s.stopWithGrace(time.Duration(payloadInt32(payload)) * time.Millisecond)
		}
	}
}

func (s *shim) killAll(pid, pgid int) {
	_ = signalGroup(pgid, sigKill)
	_ = signalPid(pid, sigKill)
}

func (s *shim) stopWithGrace(grace time.Duration) {
	s.mu.Lock()
	pid, pgid := s.run.Pid, s.run.Pgid
	s.mu.Unlock()
	s.stdin.close()
	_ = signalPid(pid, sigTerm)
	if grace <= 0 {
		grace = 1500 * time.Millisecond
	}
	time.AfterFunc(grace, func() {
		s.mu.Lock()
		exited := s.exit != nil
		s.mu.Unlock()
		if !exited {
			s.killAll(pid, pgid)
		}
	})
}

func shimFail(handshakeOut io.Writer, err error) int {
	raw, _ := json.Marshal(handshake{Error: err.Error()})
	_, _ = handshakeOut.Write(append(raw, '\n'))
	return 127
}

func ShimMain(args []string, launchIn io.Reader, handshakeOut io.WriteCloser) int {
	if len(args) < 1 {
		return shimFail(handshakeOut, errors.New("usage: agentproc-shim <dir>"))
	}
	dir := args[0]
	logger := log.New(os.Stderr, "agentproc-shim: ", log.LstdFlags|log.Lmicroseconds)
	raw, err := io.ReadAll(io.LimitReader(launchIn, 64<<20))
	if err != nil {
		return shimFail(handshakeOut, err)
	}
	var launch shimLaunch
	if err := json.Unmarshal(raw, &launch); err != nil {
		return shimFail(handshakeOut, fmt.Errorf("bad launch: %w", err))
	}
	terms, stopSignals := shimSignals()
	defer stopSignals()

	listener, token, closeListener, err := shimListen(dir, launch)
	if err != nil {
		return shimFail(handshakeOut, fmt.Errorf("listen: %w", err))
	}
	defer closeListener()

	program, err := resolveProgram(launch.Program, launch.Env)
	if err != nil {
		return shimFail(handshakeOut, err)
	}
	stdinR, stdinW, err := os.Pipe()
	if err != nil {
		return shimFail(handshakeOut, err)
	}
	outR, outW, err := os.Pipe()
	if err != nil {
		return shimFail(handshakeOut, err)
	}
	errR, errW := outR, outW
	if !launch.Merge {
		if errR, errW, err = os.Pipe(); err != nil {
			return shimFail(handshakeOut, err)
		}
	}
	env := launch.Env
	if len(env) == 0 {
		env = releasedEnv(os.Environ())
	}
	cmd := &exec.Cmd{Path: program, Args: append([]string{launch.Program}, launch.Args...), Env: env,
		Dir: launch.Cwd, Stdin: stdinR, Stdout: outW, Stderr: errW, SysProcAttr: childAttr()}
	if err := cmd.Start(); err != nil {
		stdinR.Close()
		stdinW.Close()
		outR.Close()
		outW.Close()
		if !launch.Merge {
			errR.Close()
			errW.Close()
		}
		return shimFail(handshakeOut, err)
	}
	stdinR.Close()
	outW.Close()
	if !launch.Merge {
		errW.Close()
	}

	s := &shim{dir: dir, out: newSpoolWriter(dir, streamOut, launch.SegmentBytes, launch.CapBytes),
		err:     newSpoolWriter(dir, streamErr, launch.SegmentBytes, launch.CapBytes),
		stdin:   newStdinQueue(stdinW),
		clients: make(chan net.Conn), wake: make(chan struct{}, 1), delivered: make(chan struct{}), logger: logger}
	s.run = runInfo{Protocol: shimProtocol, ShimPid: os.Getpid(), Pid: cmd.Process.Pid, Pgid: cmd.Process.Pid,
		StartedMs: time.Now().UnixMilli(), Boot: bootID()}
	if err := writeJSONFile(filepath.Join(dir, "run.json"), s.run); err != nil {
		logger.Printf("run.json: %v", err)
	}
	reply, _ := json.Marshal(handshake{Pid: s.run.Pid, ShimPid: s.run.ShimPid})
	_, _ = handshakeOut.Write(append(reply, '\n'))
	_ = handshakeOut.Close()

	go s.stdin.run()
	go s.notifier()
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go s.serveClient(conn, token)
		}
	}()

	var readers sync.WaitGroup
	pump := func(file *os.File, spool *spoolWriter) {
		defer readers.Done()
		buffer := make([]byte, 64<<10)
		for {
			n, err := file.Read(buffer)
			if n > 0 {
				s.mu.Lock()
				if werr := spool.Write(buffer[:n]); werr != nil {
					logger.Printf("spool %s: %v", spool.stream, werr)
				}
				s.mu.Unlock()
				s.poke()
			}
			if err != nil {
				return
			}
		}
	}
	readers.Add(1)
	go pump(outR, s.out)
	if !launch.Merge {
		readers.Add(1)
		go pump(errR, s.err)
	}

	waited := make(chan error, 1)
	go func() { waited <- cmd.Wait() }()
	var waitErr error
	for done := false; !done; {
		select {
		case waitErr = <-waited:
			done = true
		case <-terms:
			s.stopWithGrace(1500 * time.Millisecond)
		}
	}
	drained := make(chan struct{})
	go func() {
		readers.Wait()
		close(drained)
	}()
	drain := time.Duration(launch.DrainMs) * time.Millisecond
	if drain <= 0 {
		drain = 500 * time.Millisecond
	}
	select {
	case <-drained:
	case <-time.After(drain):
		outR.Close()
		if !launch.Merge {
			errR.Close()
		}
		<-drained
	}
	s.stdin.close()
	code, sig, crashed := -1, 0, true
	if cmd.ProcessState != nil {
		code, sig, crashed = exitStatus(cmd.ProcessState)
	} else if waitErr != nil {
		logger.Printf("wait: %v", waitErr)
	}
	s.mu.Lock()
	s.out.Close()
	s.err.Close()
	exit := exitInfo{Code: code, Signal: sig, Crashed: crashed, ExitedMs: time.Now().UnixMilli(),
		Stdout: s.out.total, Stderr: s.err.total}
	s.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "exit.json"), exit); err != nil {
		logger.Printf("exit.json: %v", err)
	}
	s.mu.Lock()
	s.exit = &exit
	s.mu.Unlock()
	s.poke()
	select {
	case <-s.delivered:
	case <-time.After(2 * time.Second):
	}
	time.Sleep(50 * time.Millisecond)
	return 0
}
