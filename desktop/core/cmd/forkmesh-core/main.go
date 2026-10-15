package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/paths"
)

var version = "dev"

func usage() {
	fmt.Fprintf(os.Stderr, `forkmesh-core %s: the ForkMesh desktop's background engine.

Usage:
  forkmesh-core serve [--project <slug>] [--linger 2m]
  forkmesh-core call [--project <slug>] <method> [json params]
  forkmesh-core status [--project <slug>]
  forkmesh-core stop [--project <slug>]
  forkmesh-core version
`, version)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	switch os.Args[1] {
	case "agentproc-shim":
		os.Exit(agentproc.ShimMain(os.Args[2:], os.Stdin, os.Stdout))
	case "serve":
		os.Exit(serve(os.Args[2:]))
	case "call":
		os.Exit(call(os.Args[2:]))
	case "status":
		os.Exit(call(append(os.Args[2:], "core.status")))
	case "stop":
		os.Exit(call(append(os.Args[2:], "core.shutdown")))
	case "version", "--version", "-v":
		fmt.Println(version)
	case "help", "--help", "-h":
		usage()
	default:
		usage()
		os.Exit(2)
	}
}

func serve(args []string) int {
	flags := flag.NewFlagSet("serve", flag.ContinueOnError)
	project := flags.String("project", "", "project slug")
	linger := flags.Duration("linger", 2*time.Minute, "how long to keep running with no desktop attached (0 runs until stopped)")
	stateDir := flags.String("state-dir", "", "override the endpoint directory")
	foreground := flags.Bool("log-stderr", false, "log to stderr instead of logs/core")
	if err := flags.Parse(args); err != nil {
		return 2
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	options := daemon.Options{Project: *project, Version: version, Linger: *linger, StateDir: *stateDir}
	if *foreground {
		options.LogWriter = os.Stderr
	}
	if err := daemon.Run(ctx, options, modules()); err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
		return 1
	}
	return 0
}

func call(args []string) int {
	flags := flag.NewFlagSet("call", flag.ContinueOnError)
	project := flags.String("project", "", "project slug")
	stateDir := flags.String("state-dir", "", "override the endpoint directory")
	if err := flags.Parse(args); err != nil {
		return 2
	}
	rest := flags.Args()
	if len(rest) < 1 {
		usage()
		return 2
	}
	dir := *stateDir
	if dir == "" {
		dir = paths.Resolve(*project, nil, "").CoreState()
	}
	endpoint, err := ipc.ReadEndpoint(dir)
	if err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core: not running:", err)
		return 1
	}
	conn, err := ipc.Dial(endpoint, 3*time.Second)
	if err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core: not reachable:", err)
		return 1
	}
	defer conn.Close()
	params := json.RawMessage("{}")
	if len(rest) > 1 {
		params = json.RawMessage(rest[1])
	}
	hello, _ := json.Marshal(map[string]any{"id": 1, "method": "core.hello",
		"params": map[string]any{"role": "cli", "token": endpoint.Token, "pid": os.Getpid()}})
	request, _ := json.Marshal(map[string]any{"id": 2, "method": rest[0], "params": params})
	if _, err := conn.Write(append(append(hello, '\n'), append(request, '\n')...)); err != nil {
		fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
		return 1
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Minute))
	reader := bufio.NewReaderSize(conn, 1<<20)
	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		var message struct {
			ID     int64           `json:"id"`
			OK     *bool           `json:"ok"`
			Result json.RawMessage `json:"result"`
			Error  string          `json:"error"`
		}
		if json.Unmarshal(line, &message) != nil || message.ID != 2 || message.OK == nil {
			continue
		}
		if !*message.OK {
			fmt.Fprintln(os.Stderr, "error:", message.Error)
			return 1
		}
		var pretty any
		_ = json.Unmarshal(message.Result, &pretty)
		out, _ := json.MarshalIndent(pretty, "", "  ")
		fmt.Println(string(out))
		return 0
	}
}
