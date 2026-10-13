package ipc

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

func shortDir(t *testing.T) string {
	dir, err := os.MkdirTemp("", "fmc")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		for _, name := range []string{"core.sock", "endpoint.json"} {
			os.Remove(filepath.Join(dir, name))
		}
		os.Remove(dir)
	})
	return dir
}

type lineClient struct {
	conn   net.Conn
	reader *bufio.Reader
}

func (c *lineClient) send(t *testing.T, value any) {
	raw, _ := json.Marshal(value)
	if _, err := c.conn.Write(append(raw, '\n')); err != nil {
		t.Fatal(err)
	}
}

func (c *lineClient) next(t *testing.T) rpc.Message {
	_ = c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	line, err := c.reader.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var message rpc.Message
	if err := json.Unmarshal(line, &message); err != nil {
		t.Fatal(err)
	}
	return message
}

func TestRoundTripBothDirections(t *testing.T) {
	registry := rpc.NewRegistry()
	hub := rpc.NewHub()
	server := NewServer(registry, hub)
	server.Version = "test"
	attached := make(chan *Conn, 1)
	server.OnUI = func(conn *Conn, on bool) {
		if on {
			attached <- conn
		}
	}
	registry.Handle("echo", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		return req.Object(), nil
	})
	registry.Handle("boom", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		return nil, rpc.Fail("nope")
	})
	endpoint, err := server.Listen(shortDir(t))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Serve(ctx)

	for _, network := range []string{"unix", "tcp"} {
		address := endpoint.Unix
		if network == "tcp" {
			address = endpoint.TCP
		}
		if address == "" {
			continue
		}
		raw, err := net.Dial(network, address)
		if err != nil {
			t.Fatal(err)
		}
		client := &lineClient{conn: raw, reader: bufio.NewReader(raw)}
		client.send(t, map[string]any{"id": 1, "method": "core.hello", "params": map[string]any{"role": "ui", "token": endpoint.Token}})
		if hello := client.next(t); hello.ID != 1 || hello.OK == nil || !*hello.OK {
			t.Fatalf("%s hello: %+v", network, hello)
		}
		ui := <-attached
		client.send(t, map[string]any{"id": 2, "method": "echo", "params": map[string]any{"a": 1}})
		if reply := client.next(t); reply.ID != 2 || string(reply.Result) != `{"a":1}` {
			t.Fatalf("%s echo: %+v", network, reply)
		}
		client.send(t, map[string]any{"id": 3, "method": "boom"})
		if reply := client.next(t); reply.Error != "nope" {
			t.Fatalf("%s boom: %+v", network, reply)
		}
		client.send(t, map[string]any{"id": 4, "method": "missing"})
		if reply := client.next(t); reply.Error != "unsupported" {
			t.Fatalf("%s missing: %+v", network, reply)
		}
		done := make(chan json.RawMessage, 1)
		go func() {
			result, err := ui.Call(context.Background(), "ui.ask", map[string]any{"q": "?"})
			if err != nil {
				t.Errorf("call: %v", err)
			}
			done <- result
		}()
		request := client.next(t)
		if request.Method != "ui.ask" {
			t.Fatalf("%s reverse: %+v", network, request)
		}
		client.send(t, map[string]any{"id": request.ID, "ok": true, "result": map[string]any{"answer": 42}})
		if got := <-done; string(got) != `{"answer":42}` {
			t.Fatalf("%s reverse result %s", network, got)
		}
		raw.Close()
	}
}

func TestTCPNeedsToken(t *testing.T) {
	server := NewServer(rpc.NewRegistry(), rpc.NewHub())
	endpoint, err := server.Listen(shortDir(t))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Serve(ctx)
	raw, err := net.Dial("tcp", endpoint.TCP)
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	client := &lineClient{conn: raw, reader: bufio.NewReader(raw)}
	client.send(t, map[string]any{"id": 1, "method": "core.hello", "params": map[string]any{"token": "wrong"}})
	if reply := client.next(t); reply.Error != "bad_token" {
		t.Fatalf("got %+v", reply)
	}
}

func TestAdoptPipe(t *testing.T) {
	server := NewServer(rpc.NewRegistry(), rpc.NewHub())
	adopted := make(chan []byte, 1)
	server.OnAdopt = func(stream net.Conn, params AdoptParams) {
		buffer := make([]byte, 5)
		n, _ := stream.Read(buffer)
		adopted <- append([]byte(params.Peer+"|"), buffer[:n]...)
		stream.Close()
	}
	endpoint, err := server.Listen(shortDir(t))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go server.Serve(ctx)
	raw, err := net.Dial("tcp", endpoint.TCP)
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	line, _ := json.Marshal(map[string]any{"id": 1, "method": "control.adopt", "params": map[string]any{"token": endpoint.Token, "peer": "1.2.3.4:5", "pass": "pipe"}})
	raw.Write(append(append(line, '\n'), []byte("hello")...))
	select {
	case got := <-adopted:
		if string(got) != "1.2.3.4:5|hello" {
			t.Fatalf("got %q", got)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("not adopted")
	}
}
