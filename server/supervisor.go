package mirrornode

import (
	"context"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"
)

type ProcessSpec struct {
	Name    string
	Program string
	Args    []string
	Env     []string
	Output  io.Writer
}

type Supervisor struct {
	mu         sync.RWMutex
	running    map[string]bool
	restarts   map[string]int64
	cancel     context.CancelFunc
	restart    map[string]chan struct{}
	reload     map[string]chan struct{}
	invalidate map[string]chan struct{}
	// instances numbers every spawned child per name, so a caller can tell a
	// process it signalled apart from the replacement the supervisor started
	// after that process exited.
	instances map[string]uint64
}

func NewSupervisor() *Supervisor {
	return &Supervisor{
		instances:  map[string]uint64{},
		running:    map[string]bool{},
		restarts:   map[string]int64{},
		restart:    map[string]chan struct{}{},
		reload:     map[string]chan struct{}{},
		invalidate: map[string]chan struct{}{},
	}
}

func (s *Supervisor) Run(ctx context.Context, specs []ProcessSpec) {
	ctx, s.cancel = context.WithCancel(ctx)
	var processes sync.WaitGroup
	for _, spec := range specs {
		spec := spec
		processes.Add(1)
		go func() {
			defer processes.Done()
			s.runOne(ctx, spec)
		}()
	}
	<-ctx.Done()
	// Do not abandon child cleanup while its inherited output descriptors and
	// listener may still be live. Every runOne shutdown is bounded by its
	// five-second kill fallback.
	processes.Wait()
}

func (s *Supervisor) runOne(ctx context.Context, spec ProcessSpec) {
	for ctx.Err() == nil {
		cmd := exec.CommandContext(ctx, spec.Program, spec.Args...)
		cmd.Env = spec.Env
		if len(cmd.Env) == 0 {
			cmd.Env = os.Environ()
		}
		writer := spec.Output
		if writer == nil {
			writer = os.Stderr
		}
		cmd.Stdout, cmd.Stderr = writer, writer
		if err := cmd.Start(); err != nil {
			s.setRunning(spec.Name, false)
			log.Printf("%s could not start: %v", spec.Name, err)
			select {
			case <-ctx.Done():
				return
			case <-time.After(5 * time.Second):
				continue
			}
		}
		restart := make(chan struct{}, 1)
		reload := make(chan struct{}, 1)
		invalidate := make(chan struct{}, 1)
		s.mu.Lock()
		s.restart[spec.Name] = restart
		s.reload[spec.Name] = reload
		s.invalidate[spec.Name] = invalidate
		s.running[spec.Name] = true
		if s.instances == nil {
			s.instances = map[string]uint64{}
		}
		s.instances[spec.Name]++
		s.mu.Unlock()
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		var err error
		intentionalRestart := false
		waiting := true
		for waiting {
			select {
			case err = <-done:
				waiting = false
			case <-reload:
				log.Printf("reloading %s after configuration change", spec.Name)
				if signalErr := cmd.Process.Signal(syscall.SIGHUP); signalErr != nil {
					log.Printf("%s could not reload: %v", spec.Name, signalErr)
				}
			case <-invalidate:
				if signalErr := cmd.Process.Signal(syscall.SIGUSR1); signalErr != nil {
					log.Printf("%s could not invalidate caches: %v", spec.Name, signalErr)
				}
			case <-restart:
				intentionalRestart = true
				log.Printf("restarting %s after configuration change", spec.Name)
				_ = cmd.Process.Signal(syscall.SIGTERM)
				select {
				case err = <-done:
				case <-time.After(5 * time.Second):
					_ = cmd.Process.Kill()
					err = <-done
				}
				waiting = false
			case <-ctx.Done():
				_ = cmd.Process.Signal(syscall.SIGTERM)
				select {
				case <-done:
				case <-time.After(5 * time.Second):
					_ = cmd.Process.Kill()
					<-done
				}
				s.mu.Lock()
				if s.restart[spec.Name] == restart {
					delete(s.restart, spec.Name)
					delete(s.reload, spec.Name)
					delete(s.invalidate, spec.Name)
				}
				s.running[spec.Name] = false
				s.mu.Unlock()
				return
			}
		}
		s.mu.Lock()
		if s.restart[spec.Name] == restart {
			delete(s.restart, spec.Name)
			delete(s.reload, spec.Name)
			delete(s.invalidate, spec.Name)
		}
		s.running[spec.Name] = false
		s.mu.Unlock()
		if ctx.Err() != nil {
			return
		}
		s.mu.Lock()
		s.restarts[spec.Name]++
		s.mu.Unlock()
		if intentionalRestart {
			// Configuration reloads are expected control-plane events. Start the
			// replacement immediately; the crash backoff below is only for an
			// unexpectedly failing child.
			continue
		}
		log.Printf("%s exited: %v; retrying in 5s", spec.Name, err)
		select {
		case <-ctx.Done():
			return
		case <-time.After(5 * time.Second):
		}
	}
}

// Reload asks a live process to adopt its new configuration without closing
// its listener. The process acknowledges the exact generation separately; a
// caller can retain Restart as the fallback when that acknowledgement stalls.
//
// It reports whether a reload is queued for the live process. A request that
// finds one already queued coalesces with it: the queued signal has not been
// delivered yet, so the process will read the newest configuration anyway.
// Only an absent process (nothing to signal) reports false.
func (s *Supervisor) Reload(name string) bool {
	s.mu.RLock()
	ch := s.reload[name]
	s.mu.RUnlock()
	if ch == nil {
		return false
	}
	select {
	case ch <- struct{}{}:
	default:
	}
	return true
}

// Instance reports the spawn number of the named child and whether it is
// running now. The number advances every time the supervisor starts a new
// process for that name.
func (s *Supervisor) Instance(name string) (uint64, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.instances[name], s.running[name]
}

func (s *Supervisor) Restart(name string) bool {
	s.mu.RLock()
	ch := s.restart[name]
	s.mu.RUnlock()
	if ch == nil {
		return false
	}
	select {
	case ch <- struct{}{}:
		return true
	default:
		return false
	}
}

// Invalidate asks a live process to refresh its memory-resident, non-config
// data after the daemon atomically replaces a durable projection.
func (s *Supervisor) Invalidate(name string) bool {
	s.mu.RLock()
	ch := s.invalidate[name]
	s.mu.RUnlock()
	if ch == nil {
		return false
	}
	select {
	case ch <- struct{}{}:
		return true
	default:
		return false
	}
}

func (s *Supervisor) setRunning(name string, value bool) {
	s.mu.Lock()
	s.running[name] = value
	s.mu.Unlock()
}

func (s *Supervisor) Snapshot() (map[string]bool, map[string]int64) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	running := make(map[string]bool, len(s.running))
	restarts := make(map[string]int64, len(s.restarts))
	for key, value := range s.running {
		running[key] = value
	}
	for key, value := range s.restarts {
		restarts[key] = value
	}
	return running, restarts
}

func (s *Supervisor) Stop() {
	if s.cancel != nil {
		s.cancel()
	}
}

func tokenEnvironment(path string) ([]string, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Size() <= 0 || info.Size() > 16<<10 || info.Mode().Perm()&0077 != 0 {
		return nil, fmt.Errorf("connector token must be a small owner-only regular file")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	token := string(raw)
	for len(token) > 0 && (token[len(token)-1] == '\n' || token[len(token)-1] == '\r') {
		token = token[:len(token)-1]
	}
	if token == "" || strings.ContainsAny(token, "\r\n") {
		return nil, fmt.Errorf("connector token is invalid")
	}
	environment := os.Environ()
	filtered := environment[:0]
	for _, value := range environment {
		upper := strings.ToUpper(value)
		if strings.HasPrefix(upper, "CLOUDFLARE_API_TOKEN=") || strings.HasPrefix(upper, "CF_API_TOKEN=") || strings.HasPrefix(upper, "CLOUDFLARE_TOKEN=") || strings.HasPrefix(upper, "CF_TOKEN=") || strings.HasPrefix(upper, "TUNNEL_TOKEN=") {
			continue
		}
		filtered = append(filtered, value)
	}
	return append(filtered, "TUNNEL_TOKEN="+token), nil
}
