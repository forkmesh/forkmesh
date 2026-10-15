package agentproc

import (
	"bufio"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

const (
	transportTCP  = "tcp"
	tcpFile       = "tcp.json"
	frameAuth     = byte('a')
	authTimeout   = 5 * time.Second
	defaultUseTCP = runtime.GOOS == "windows"
)

type tcpEndpoint struct {
	Addr  string `json:"addr"`
	Token string `json:"token"`
}

func shimListen(dir string, launch shimLaunch) (net.Listener, string, func(), error) {
	if launch.Transport == transportTCP {
		listener, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			return nil, "", nil, err
		}
		raw := make([]byte, 24)
		if _, err := rand.Read(raw); err != nil {
			listener.Close()
			return nil, "", nil, err
		}
		token := hex.EncodeToString(raw)
		path := filepath.Join(dir, tcpFile)
		if err := writeJSONFile(path, tcpEndpoint{Addr: listener.Addr().String(), Token: token}); err != nil {
			listener.Close()
			return nil, "", nil, err
		}
		return listener, token, func() {
			listener.Close()
			_ = os.Remove(path)
		}, nil
	}
	socket := launch.Socket
	if socket == "" {
		socket = filepath.Join(dir, "s")
	}
	_ = os.Remove(socket)
	listener, err := net.Listen("unix", socket)
	if err != nil {
		return nil, "", nil, err
	}
	_ = os.Chmod(socket, 0o600)
	return listener, "", func() {
		listener.Close()
		_ = os.Remove(socket)
	}, nil
}

func shimAuthorize(conn net.Conn, reader *bufio.Reader, token string) bool {
	if token == "" {
		return true
	}
	_ = conn.SetReadDeadline(time.Now().Add(authTimeout))
	kind, payload, err := readFrame(reader)
	_ = conn.SetReadDeadline(time.Time{})
	return err == nil && kind == frameAuth && subtle.ConstantTimeCompare(payload, []byte(token)) == 1
}

func dialShimOnce(sp spec, dir string, timeout time.Duration) (net.Conn, error) {
	if sp.Transport != transportTCP {
		if sp.Socket == "" {
			return nil, errors.New("agentproc: no shim socket")
		}
		return net.DialTimeout("unix", sp.Socket, timeout)
	}
	var endpoint tcpEndpoint
	if !readJSONFile(filepath.Join(dir, tcpFile), &endpoint) || endpoint.Addr == "" || endpoint.Token == "" {
		return nil, errors.New("agentproc: the shim has not published its port")
	}
	conn, err := net.DialTimeout("tcp", endpoint.Addr, timeout)
	if err != nil {
		return nil, err
	}
	_ = conn.SetWriteDeadline(time.Now().Add(authTimeout))
	if err := writeFrame(conn, frameAuth, []byte(endpoint.Token)); err != nil {
		conn.Close()
		return nil, err
	}
	_ = conn.SetWriteDeadline(time.Time{})
	return conn, nil
}

func dialShim(sp spec, dir string, timeout time.Duration) (net.Conn, error) {
	deadline := time.Now().Add(timeout)
	for {
		conn, err := dialShimOnce(sp, dir, 500*time.Millisecond)
		if err == nil {
			return conn, nil
		}
		if time.Now().After(deadline) {
			return nil, err
		}
		time.Sleep(20 * time.Millisecond)
	}
}
