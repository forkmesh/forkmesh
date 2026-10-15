package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"forkmesh.com/desktop-core/internal/autostart"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/paths"
)

var errNotRunning = errors.New("not running")

func callCore(project, stateDir, method string, params any) (json.RawMessage, error) {
	if stateDir == "" {
		stateDir = paths.Resolve(project, nil, "").CoreState()
	}
	endpoint, err := ipc.ReadEndpoint(stateDir)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", errNotRunning, err)
	}
	conn, err := ipc.Dial(endpoint, 3*time.Second)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", errNotRunning, err)
	}
	defer conn.Close()
	encoded, err := json.Marshal(params)
	if err != nil {
		return nil, err
	}
	hello, _ := json.Marshal(map[string]any{"id": 1, "method": "core.hello",
		"params": map[string]any{"role": "cli", "token": endpoint.Token, "pid": os.Getpid(), "protocol": ipc.Protocol}})
	request, _ := json.Marshal(map[string]any{"id": 2, "method": method, "params": json.RawMessage(encoded)})
	if _, err := conn.Write(append(append(hello, '\n'), append(request, '\n')...)); err != nil {
		return nil, err
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Minute))
	reader := bufio.NewReaderSize(conn, 1<<20)
	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			return nil, err
		}
		var message struct {
			ID     int64           `json:"id"`
			OK     *bool           `json:"ok"`
			Result json.RawMessage `json:"result"`
			Error  string          `json:"error"`
		}
		if json.Unmarshal(line, &message) != nil || message.OK == nil {
			continue
		}
		if !*message.OK {
			return nil, errors.New(message.Error)
		}
		if message.ID == 2 {
			return message.Result, nil
		}
	}
}

func printJSON(value any) {
	out, _ := json.MarshalIndent(value, "", "  ")
	fmt.Println(string(out))
}

func selfBinary() string {
	path, err := os.Executable()
	if err != nil {
		return ""
	}
	if resolved, err := filepath.EvalSymlinks(path); err == nil {
		return resolved
	}
	return path
}

func projectFlags(name string, args []string) (*flag.FlagSet, *string, *string, *string, error) {
	flags := flag.NewFlagSet(name, flag.ContinueOnError)
	project := flags.String("project", "", "project slug")
	stateDir := flags.String("state-dir", "", "override the endpoint directory")
	binary := flags.String("binary", "", "forkmesh-core path to register (autostart)")
	var positional []string
	for len(args) > 0 {
		if err := flags.Parse(args); err != nil {
			return nil, nil, nil, nil, err
		}
		args = flags.Args()
		if len(args) > 0 {
			positional = append(positional, args[0])
			args = args[1:]
		}
	}
	_ = flags.Parse(positional)
	return flags, project, stateDir, binary, nil
}

func resolvedProject(project string) string {
	return paths.Resolve(project, nil, "").Project
}

func startDetached(args []string) int {
	_, project, stateDir, _, err := projectFlags("start", args)
	if err != nil {
		return 2
	}
	if _, err := callCore(*project, *stateDir, "core.ping", nil); err == nil {
		fmt.Println("forkmesh-core is already running")
		return 0
	}
	serveArgs := []string{"serve", "--background"}
	if *project != "" {
		serveArgs = append(serveArgs, "--project", *project)
	}
	if *stateDir != "" {
		serveArgs = append(serveArgs, "--state-dir", *stateDir)
	}
	pid, err := spawnDetached(selfBinary(), serveArgs)
	if err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core: start:", err)
		return 1
	}
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
		if _, err := callCore(*project, *stateDir, "core.ping", nil); err == nil {
			fmt.Printf("forkmesh-core started (pid %d)\n", pid)
			return 0
		}
	}
	fmt.Fprintln(os.Stderr, "forkmesh-core: started but not answering yet; see logs/core/forkmesh-core.log")
	return 1
}

func background(args []string) int {
	flags, project, stateDir, _, err := projectFlags("background", args)
	if err != nil || flags.NArg() != 1 {
		usage()
		return 2
	}
	action := flags.Arg(0)
	var params map[string]any
	switch action {
	case "on":
		params = map[string]any{"background": true}
	case "off":
		params = map[string]any{"background": false}
	case "status":
	default:
		usage()
		return 2
	}
	method := "core.daemon.get"
	if params != nil {
		method = "core.daemon.set"
	}
	result, err := callCore(*project, *stateDir, method, params)
	if err == nil {
		var state any
		_ = json.Unmarshal(result, &state)
		printJSON(state)
		return 0
	}
	if !errors.Is(err, errNotRunning) {
		fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
		return 1
	}
	layout := paths.Resolve(*project, nil, "")
	path := filepath.Join(layout.Config, "core", "daemon.json")
	var config daemon.DaemonConfig
	if raw, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(raw, &config)
	}
	if params != nil {
		config.Background = action == "on"
		config.UpdatedMs = time.Now().UnixMilli()
		raw, _ := json.MarshalIndent(config, "", "  ")
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		if err := os.WriteFile(path+".tmp", append(raw, '\n'), 0o600); err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		if err := os.Rename(path+".tmp", path); err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
	}
	printJSON(map[string]any{"background": config.Background, "running": false, "configPath": path})
	return 0
}

func autostartCommand(args []string) int {
	flags, project, _, binary, err := projectFlags("autostart", args)
	if err != nil || flags.NArg() != 1 {
		usage()
		return 2
	}
	slug := resolvedProject(*project)
	manager := autostart.System()
	var status autostart.Status
	switch flags.Arg(0) {
	case "enable":
		path := *binary
		if path == "" {
			path = selfBinary()
		}
		if absolute, err := filepath.Abs(path); err == nil {
			path = absolute
		}
		status, err = manager.Enable(path, slug)
	case "disable":
		status, err = manager.Disable(slug)
	case "status":
		status = manager.Status(slug)
	default:
		usage()
		return 2
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
		return 1
	}
	printJSON(status)
	return 0
}
