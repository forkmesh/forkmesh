package ipc

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"os"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

func startServer(t *testing.T, registry *rpc.Registry) (*Server, Endpoint) {
	server := NewServer(registry, rpc.NewHub())
	endpoint, err := server.Listen(shortDir(t))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go server.Serve(ctx)
	return server, endpoint
}

func dialHello(t *testing.T, endpoint Endpoint, params map[string]any) (*lineClient, rpc.Message) {
	network, address := "tcp", endpoint.TCP
	if endpoint.Unix != "" {
		network, address = "unix", endpoint.Unix
	}
	raw, err := net.Dial(network, address)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { raw.Close() })
	client := &lineClient{conn: raw, reader: bufio.NewReader(raw)}
	client.send(t, map[string]any{"id": 1, "method": "core.hello", "params": params})
	return client, client.next(t)
}

func TestHelloAdvertisesProtocolAndFeatures(t *testing.T) {
	_, endpoint := startServer(t, rpc.NewRegistry())
	_, hello := dialHello(t, endpoint, map[string]any{"role": "cli", "token": endpoint.Token, "protocol": Protocol})
	if hello.OK == nil || !*hello.OK {
		t.Fatalf("hello: %+v", hello)
	}
	var reply struct {
		Protocol    int      `json:"protocol"`
		ProtocolMin int      `json:"protocolMin"`
		Features    []string `json:"features"`
	}
	if err := json.Unmarshal(hello.Result, &reply); err != nil {
		t.Fatal(err)
	}
	if reply.Protocol != Protocol || reply.ProtocolMin != ProtocolMin || len(reply.Features) == 0 {
		t.Fatalf("reply %+v", reply)
	}
}

func TestHelloMarksHeadlessDesktop(t *testing.T) {
	server, endpoint := startServer(t, rpc.NewRegistry())
	for _, tc := range []struct {
		params map[string]any
		want   bool
	}{
		{map[string]any{"role": "ui", "token": endpoint.Token, "protocol": Protocol}, false},
		{map[string]any{"role": "ui", "token": endpoint.Token, "protocol": Protocol, "headless": true}, true},
	} {
		client, hello := dialHello(t, endpoint, tc.params)
		if hello.OK == nil || !*hello.OK {
			t.Fatalf("hello: %+v", hello)
		}
		ui := server.UI()
		if ui == nil || ui.Headless() != tc.want {
			t.Fatalf("ui %v headless want %v", ui, tc.want)
		}
		client.conn.Close()
		deadline := time.Now().Add(2 * time.Second)
		for server.UI() != nil && time.Now().Before(deadline) {
			time.Sleep(5 * time.Millisecond)
		}
	}
}

func TestHelloRefusesTooOldProtocol(t *testing.T) {
	if ProtocolMin <= 1 {
		_, endpoint := startServer(t, rpc.NewRegistry())
		_, hello := dialHello(t, endpoint, map[string]any{"token": endpoint.Token, "protocol": 1})
		if hello.OK == nil || !*hello.OK {
			t.Fatalf("protocol 1 must still be accepted: %+v", hello)
		}
		return
	}
	_, endpoint := startServer(t, rpc.NewRegistry())
	_, hello := dialHello(t, endpoint, map[string]any{"token": endpoint.Token, "protocol": ProtocolMin - 1})
	if hello.Error != "protocol_unsupported" {
		t.Fatalf("got %+v", hello)
	}
}

func TestUnixSocketNeedsToken(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("no unix socket")
	}
	_, endpoint := startServer(t, rpc.NewRegistry())
	_, hello := dialHello(t, endpoint, map[string]any{"role": "ui"})
	if hello.Error != "bad_token" {
		t.Fatalf("got %+v", hello)
	}
}

func TestCancelStopsInflightRequest(t *testing.T) {
	registry := rpc.NewRegistry()
	started := make(chan struct{}, 1)
	registry.Handle("slow", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		started <- struct{}{}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(10 * time.Second):
			return map[string]any{"late": true}, nil
		}
	})
	_, endpoint := startServer(t, registry)
	client, hello := dialHello(t, endpoint, map[string]any{"token": endpoint.Token, "protocol": Protocol})
	if hello.OK == nil || !*hello.OK {
		t.Fatalf("hello %+v", hello)
	}
	client.send(t, map[string]any{"id": 7, "method": "slow"})
	<-started
	client.send(t, map[string]any{"id": 8, "method": CancelMethod, "params": map[string]any{"id": 7}})
	seen := map[int64]rpc.Message{}
	for len(seen) < 2 {
		message := client.next(t)
		seen[message.ID] = message
	}
	if seen[7].Error != "cancelled" {
		t.Fatalf("slow reply %+v", seen[7])
	}
	if string(seen[8].Result) != `{"cancelled":true,"id":7}` {
		t.Fatalf("cancel reply %s", seen[8].Result)
	}
	client.send(t, map[string]any{"id": 9, "method": CancelMethod, "params": map[string]any{"id": 7}})
	if reply := client.next(t); string(reply.Result) != `{"cancelled":false,"id":7}` {
		t.Fatalf("second cancel %s", reply.Result)
	}
}

func TestProgressReachesOnlyTheCaller(t *testing.T) {
	registry := rpc.NewRegistry()
	registry.Handle("work", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		for step := 1; step <= 3; step++ {
			req.Progress(map[string]any{"step": step, "of": 3})
		}
		return map[string]any{"done": true}, nil
	})
	_, endpoint := startServer(t, registry)
	other, _ := dialHello(t, endpoint, map[string]any{"token": endpoint.Token})
	client, _ := dialHello(t, endpoint, map[string]any{"token": endpoint.Token})
	client.send(t, map[string]any{"id": 5, "method": "work"})
	steps := 0
	for {
		message := client.next(t)
		if message.Event == rpc.ProgressEvent {
			var data struct {
				ID       int64          `json:"id"`
				Method   string         `json:"method"`
				Progress map[string]int `json:"progress"`
			}
			_ = json.Unmarshal(message.Data, &data)
			steps++
			if data.ID != 5 || data.Method != "work" || data.Progress["step"] != steps {
				t.Fatalf("progress %s", message.Data)
			}
			continue
		}
		if message.ID != 5 || steps != 3 {
			t.Fatalf("reply %+v after %d steps", message, steps)
		}
		break
	}
	_ = other.conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if line, err := other.reader.ReadBytes('\n'); err == nil {
		t.Fatalf("other peer saw %s", line)
	}
}

func TestDisconnectDoesNotCancelWork(t *testing.T) {
	registry := rpc.NewRegistry()
	finished := make(chan error, 1)
	release := make(chan struct{})
	registry.Handle("job", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		<-release
		finished <- ctx.Err()
		return nil, nil
	})
	_, endpoint := startServer(t, registry)
	client, _ := dialHello(t, endpoint, map[string]any{"token": endpoint.Token})
	client.send(t, map[string]any{"id": 3, "method": "job"})
	time.Sleep(50 * time.Millisecond)
	client.conn.Close()
	time.Sleep(50 * time.Millisecond)
	close(release)
	select {
	case err := <-finished:
		if err != nil {
			t.Fatalf("job context ended with the connection: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("job never finished")
	}
}

func TestLongStatePathFallsBackToTCP(t *testing.T) {
	dir := shortDir(t)
	long := dir + "/" + strings.Repeat("d", 120)
	t.Cleanup(func() { os.RemoveAll(long) })
	server := NewServer(rpc.NewRegistry(), rpc.NewHub())
	endpoint, err := server.Listen(long)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	if endpoint.Unix != "" || endpoint.TCP == "" {
		t.Fatalf("endpoint %+v", endpoint)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Serve(ctx)
	_, hello := dialHello(t, endpoint, map[string]any{"token": endpoint.Token})
	if hello.OK == nil || !*hello.OK {
		t.Fatalf("hello over the TCP fallback: %+v", hello)
	}
}
