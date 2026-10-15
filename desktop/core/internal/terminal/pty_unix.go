//go:build linux || darwin

package terminal

import (
	"errors"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const supported = true

type ptyProcess struct {
	master *os.File
	cmd    *exec.Cmd
	exited chan struct{}
	code   int
}

func ioctl(file *os.File, request uintptr, argument unsafe.Pointer) error {
	conn, err := file.SyscallConn()
	if err != nil {
		return err
	}
	var callErr error
	if err := conn.Control(func(fd uintptr) {
		if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, request, uintptr(argument)); errno != 0 {
			callErr = errno
		}
	}); err != nil {
		return err
	}
	return callErr
}

func startProcess(spec launch) (process, error) {
	master, followerName, err := openPty()
	if err != nil {
		return nil, err
	}
	follower, err := os.OpenFile(followerName, os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		master.Close()
		return nil, err
	}
	cmd := exec.Command(spec.program, spec.args...)
	cmd.Dir = spec.dir
	cmd.Env = spec.env
	cmd.Stdin, cmd.Stdout, cmd.Stderr = follower, follower, follower
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true, Setctty: true, Ctty: 0}
	p := &ptyProcess{master: master, cmd: cmd, exited: make(chan struct{})}
	_ = p.Resize(spec.cols, spec.rows)
	if err := cmd.Start(); err != nil {
		follower.Close()
		master.Close()
		return nil, err
	}
	follower.Close()
	go func() {
		err := cmd.Wait()
		var exit *exec.ExitError
		switch {
		case err == nil:
			p.code = 0
		case errors.As(err, &exit):
			p.code = exit.ExitCode()
		default:
			p.code = -1
		}
		close(p.exited)
	}()
	return p, nil
}

func (p *ptyProcess) Read(buf []byte) (int, error) { return p.master.Read(buf) }

func (p *ptyProcess) Write(buf []byte) (int, error) { return p.master.Write(buf) }

func (p *ptyProcess) Pid() int { return p.cmd.Process.Pid }

func (p *ptyProcess) Exited() <-chan struct{} { return p.exited }

func (p *ptyProcess) Code() int {
	<-p.exited
	return p.code
}

func (p *ptyProcess) Resize(cols, rows int) error {
	size := struct{ rows, cols, xpixel, ypixel uint16 }{rows: uint16(rows), cols: uint16(cols)}
	return ioctl(p.master, syscall.TIOCSWINSZ, unsafe.Pointer(&size))
}

func (p *ptyProcess) signal(sig syscall.Signal) {
	select {
	case <-p.exited:
		return
	default:
	}
	pid := p.cmd.Process.Pid
	_ = syscall.Kill(-pid, sig)
	_ = syscall.Kill(pid, sig)
}

func (p *ptyProcess) Stop() {
	p.signal(syscall.SIGHUP)
	start := time.Now()
	termSent, killSent := false, false
	ticker := time.NewTicker(5 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-p.exited:
			p.master.Close()
			return
		case <-ticker.C:
		}
		elapsed := time.Since(start)
		if !termSent && elapsed >= stopTermAfter {
			termSent = true
			p.signal(syscall.SIGTERM)
		}
		if !killSent && elapsed >= stopKillAfter {
			killSent = true
			p.signal(syscall.SIGKILL)
		}
		if elapsed >= stopGiveUpAfter {
			p.master.Close()
			return
		}
	}
}

func (p *ptyProcess) Close() error { return p.master.Close() }

func (p *ptyProcess) SetWriteDeadline(t time.Time) error { return p.master.SetWriteDeadline(t) }

func cString(buf []byte) string {
	if i := strings.IndexByte(string(buf), 0); i >= 0 {
		return string(buf[:i])
	}
	return string(buf)
}
