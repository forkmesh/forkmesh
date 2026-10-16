package diag

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

type processDetail struct {
	PID         int    `json:"pid"`
	Name        string `json:"name"`
	Identity    string `json:"identity"`
	Description string `json:"description"`
	Details     string `json:"details"`
	Stoppable   bool   `json:"stoppable"`
	Token       string `json:"token"`
}

func processDescription(name string) string {
	switch name {
	case "gnome-shell":
		return "GNOME desktop and window manager"
	case "mutter-x11-frames", "Xwayland", "Xorg":
		return "Display server and window decorations"
	case "dockerd", "containerd":
		return "Runs and manages containers"
	case "sshd":
		return "Secure Shell remote access service"
	case "systemd":
		return "System and service manager"
	case "pulseaudio", "pipewire", "wireplumber":
		return "Audio and video service"
	case "chrome", "chromium", "firefox":
		return "Web browser process"
	case "python", "python3":
		return "Python program"
	case "node":
		return "JavaScript runtime"
	case "cc1plus", "cc1", "clang", "gcc":
		return "Compiler worker"
	case "ibus-x11", "ibus-daemon":
		return "Keyboard and input method service"
	case "opensnitchd":
		return "Application firewall service"
	case "forkmesh", "forkmesh-core":
		return "ForkMesh application and background services"
	}
	return "Runs " + name
}

func limitedProcessFile(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return "Unavailable: " + err.Error()
	}
	defer f.Close()
	b, _ := io.ReadAll(io.LimitReader(f, 65537))
	if len(b) > 65536 {
		return string(b[:65536]) + "\n[truncated]"
	}
	return string(b)
}

func readProcessDetail(pid int) (processDetail, error) {
	var result processDetail
	if runtime.GOOS != "linux" {
		return result, rpc.Fail("unsupported")
	}
	base := filepath.Join("/proc", strconv.Itoa(pid))
	stat, err := os.ReadFile(filepath.Join(base, "stat"))
	if err != nil {
		return result, rpc.Fail("process_exited")
	}
	end := strings.LastIndexByte(string(stat), ')')
	begin := strings.IndexByte(string(stat), '(')
	if begin < 0 || end <= begin {
		return result, rpc.Fail("process_exited")
	}
	fields := strings.Fields(string(stat[end+1:]))
	if len(fields) < 20 {
		return result, rpc.Fail("process_exited")
	}
	result.PID = pid
	result.Name = string(stat[begin+1 : end])
	result.Identity = fields[19]
	result.Description = processDescription(result.Name)
	var detail strings.Builder
	fmt.Fprintf(&detail, "%s\n%s\nPID: %d\nParent PID: %s\nStart ticks: %s\n", result.Name, result.Description, pid, fields[1], result.Identity)
	for _, key := range []string{"exe", "cwd", "root"} {
		value, err := os.Readlink(filepath.Join(base, key))
		if err == nil {
			fmt.Fprintf(&detail, "%s: %s\n", key, value)
		}
	}
	command, err := os.ReadFile(filepath.Join(base, "cmdline"))
	if err == nil {
		fmt.Fprintf(&detail, "Command: %s\n", commandSummary(strings.ReplaceAll(string(command), "\x00", " "), 32768))
	}
	for _, key := range []string{"status", "io", "limits", "cgroup", "maps"} {
		fmt.Fprintf(&detail, "\n%s\n%s\n", key, limitedProcessFile(filepath.Join(base, key)))
	}
	entries, err := os.ReadDir(filepath.Join(base, "fd"))
	if err == nil {
		detail.WriteString("\nOpen files and sockets\n")
		for i, entry := range entries {
			if i == 256 {
				detail.WriteString("[additional descriptors omitted]\n")
				break
			}
			target, err := os.Readlink(filepath.Join(base, "fd", entry.Name()))
			if err == nil {
				fmt.Fprintf(&detail, "%s: %s\n", entry.Name(), target)
			}
		}
	}
	fmt.Fprintf(&detail, "\nDirect child PIDs\n%s\n", limitedProcessFile(filepath.Join(base, "task", strconv.Itoa(pid), "children")))
	result.Details = redactSecrets(detail.String())
	return result, nil
}

func signalProcess(ctx context.Context, pid int, password string) error {
	if password == "" {
		process, err := os.FindProcess(pid)
		if err != nil {
			return err
		}
		defer process.Release()
		return process.Signal(syscall.SIGTERM)
	}
	if len(password) > 1024 || strings.ContainsAny(password, "\r\n\x00") {
		return rpc.Fail("bad_password")
	}
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "sudo", "-k", "-S", "-p", "", "--", "/bin/kill", "-TERM", strconv.Itoa(pid))
	cmd.Stdin = strings.NewReader(password + "\n")
	if err := cmd.Run(); err != nil {
		return rpc.Fail("authentication_failed")
	}
	return nil
}

func (m *Module) processForRequest(req *rpc.Request) (processDetail, string, string, error) {
	var params struct {
		PID      int    `json:"pid"`
		Identity string `json:"identity"`
		Token    string `json:"token"`
		Password string `json:"password"`
	}
	if json.Unmarshal(req.Params, &params) != nil || params.PID <= 0 || int64(params.PID) > 2147483647 {
		return processDetail{}, "", "", rpc.Fail("bad_pid")
	}
	read := m.processDetail
	if read == nil {
		read = readProcessDetail
	}
	detail, err := read(params.PID)
	if err != nil {
		return detail, "", "", err
	}
	if params.Identity != "" && params.Identity != detail.Identity {
		return detail, "", "", rpc.Fail("process_changed")
	}
	detail.Stoppable = params.PID > 1 && params.PID != os.Getpid() && params.PID != m.desktopPID()
	return detail, params.Token, params.Password, nil
}

func (m *Module) processToken(req *rpc.Request, detail processDetail) string {
	return m.processConfirms.Issue(phonefamily.DeviceOf(req).Key, "stopProcess", fmt.Sprintf("%d:%s", detail.PID, detail.Identity), time.Now().UnixMilli())
}

func (m *Module) handleProcess(ctx context.Context, req *rpc.Request) (any, error) {
	detail, _, _, err := m.processForRequest(req)
	if err != nil {
		return nil, err
	}
	if detail.Stoppable {
		detail.Token = m.processToken(req, detail)
	}
	return detail, nil
}

func (m *Module) handleProcessStop(ctx context.Context, req *rpc.Request) (any, error) {
	detail, token, password, err := m.processForRequest(req)
	if err != nil {
		return nil, err
	}
	if !detail.Stoppable {
		return nil, rpc.Fail("protected_process")
	}
	verdict := m.processConfirms.Consume(token, phonefamily.DeviceOf(req).Key, "stopProcess", fmt.Sprintf("%d:%s", detail.PID, detail.Identity), time.Now().UnixMilli())
	if verdict != phonefamily.Ok {
		return nil, rpc.Fail(phonefamily.ConfirmationError(verdict))
	}
	stop := m.stopProcess
	if stop == nil {
		stop = signalProcess
	}
	err = stop(ctx, detail.PID, password)
	if errors.Is(err, syscall.EPERM) || rpc.Code(err) == "authentication_failed" {
		return map[string]any{"passwordRequired": true, "token": m.processToken(req, detail), "message": "Enter the computer's sudo password to stop this process."}, nil
	}
	if errors.Is(err, os.ErrProcessDone) || errors.Is(err, syscall.ESRCH) {
		return nil, rpc.Fail("process_exited")
	}
	if err != nil {
		return nil, rpc.Fail("process_stop_failed")
	}
	return map[string]any{"stopped": true, "message": "Termination requested."}, nil
}
