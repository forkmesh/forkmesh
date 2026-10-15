package control

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"unicode"
	"unicode/utf8"
)

type incoming struct {
	file      *os.File
	partPath  string
	finalPath string
	name      string
	size      int64
	received  int64
}

func (in *incoming) discard() {
	in.file.Close()
	_ = os.Remove(in.partPath)
}

type outgoing struct {
	file       *os.File
	transferID string
	size       int64
	pos        int64
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func uniquePath(dir, name string) string {
	stem := name
	suffix := ""
	if index := strings.LastIndex(name, "."); index >= 0 {
		stem = name[:index]
		if index+1 < len(name) {
			suffix = "." + name[index+1:]
		}
	}
	candidate := filepath.Join(dir, name)
	for n := 2; exists(candidate) || exists(candidate+".part"); n++ {
		candidate = filepath.Join(dir, stem+" ("+strconv.Itoa(n)+")"+suffix)
	}
	return candidate
}

func (m *Module) handleFileRequest(conn *connection, requestID int, method string, params map[string]any) {
	transfer := jsonString(params["transfer"])
	if method == "files.begin" {
		size := int64(jsonDouble(params["size"], 0))
		if m.ReceiveDir == "" || size < 0 || size > maxFileBytes || len(conn.incoming) >= 4 {
			m.respondError(conn, requestID, "refused")
			return
		}
		if err := os.MkdirAll(m.ReceiveDir, 0o755); err != nil {
			m.respondError(conn, requestID, "no_space")
			return
		}
		in := &incoming{name: SafeFileName(jsonString(params["name"])), size: size}
		in.finalPath = uniquePath(m.ReceiveDir, in.name)
		in.partPath = in.finalPath + ".part"
		file, err := os.OpenFile(in.partPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
		if err != nil {
			m.respondError(conn, requestID, "cannot_write")
			return
		}
		in.file = file
		id := newUUID()
		conn.incoming[id] = in
		m.respond(conn, requestID, encodeJSON(map[string]any{"transfer": id}))
		return
	}
	in, ok := conn.incoming[transfer]
	if !ok {
		m.respondError(conn, requestID, "unknown_transfer")
		return
	}
	switch method {
	case "files.chunk":
		data := lenientBase64(jsonString(params["data"]))
		failed := len(data) == 0 || in.received+int64(len(data)) > in.size
		if !failed {
			written, err := in.file.Write(data)
			failed = err != nil || written != len(data)
		}
		if failed {
			in.discard()
			delete(conn.incoming, transfer)
			m.respondError(conn, requestID, "write_failed")
			return
		}
		in.received += int64(len(data))
		m.respond(conn, requestID, encodeJSON(map[string]any{"received": in.received}))
	case "files.end":
		delete(conn.incoming, transfer)
		closeErr := in.file.Close()
		complete := in.received == in.size
		renamed := false
		if complete && closeErr == nil && !exists(in.finalPath) {
			renamed = os.Rename(in.partPath, in.finalPath) == nil
		}
		if !renamed {
			_ = os.Remove(in.partPath)
			code := "incomplete"
			if complete {
				code = "cannot_write"
			}
			m.respondError(conn, requestID, code)
			return
		}
		m.respond(conn, requestID, encodeJSON(map[string]any{"path": in.finalPath,
			"name": filepath.Base(in.finalPath)}))
		m.logf("control: received %s from %s", in.finalPath, conn.device.Name)
		m.notifyUI("ui.control.fileReceived", map[string]any{"device": conn.device.record(),
			"path": in.finalPath}, nil)
	case "files.cancel":
		in.discard()
		delete(conn.incoming, transfer)
		m.respond(conn, requestID, json.RawMessage("{}"))
	default:
		m.respondError(conn, requestID, "unsupported")
	}
}

func openErrorString(err error) string {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return capitalize(errno.Error())
	}
	var pathErr *os.PathError
	if errors.As(err, &pathErr) {
		return capitalize(pathErr.Err.Error())
	}
	return capitalize(err.Error())
}

func capitalize(text string) string {
	if text == "" {
		return "Unknown error"
	}
	r, size := utf8.DecodeRuneInString(text)
	return string(unicode.ToUpper(r)) + text[size:]
}

func (m *Module) sendFile(key, path string) string {
	var target *connection
	for _, conn := range m.conns {
		if conn.state == statePaired && conn.device.Key == key && !conn.relayed() {
			target = conn
		}
	}
	if target == nil {
		return "That phone is not connected."
	}
	file, err := os.Open(path)
	if err != nil {
		return openErrorString(err)
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return openErrorString(err)
	}
	if info.IsDir() {
		file.Close()
		return "file to open is a directory"
	}
	if info.Size() > maxFileBytes {
		file.Close()
		return "The file is too large."
	}
	out := &outgoing{file: file, size: info.Size(), transferID: newUUID()}
	m.sendEvent(target, "file.begin", map[string]any{"transfer": out.transferID, "name": filepath.Base(path),
		"size": out.size})
	target.outgoing = append(target.outgoing, out)
	m.pumpOutgoing(target)
	return ""
}

func (m *Module) pumpOutgoing(conn *connection) {
	buffer := make([]byte, FileChunkBytes)
	queued := 0
	for len(conn.outgoing) > 0 && !conn.closed && conn.writer != nil && !conn.relayed() &&
		queued < outgoingHighWater && conn.writer.buffered() < outgoingHighWater {
		out := conn.outgoing[0]
		n, err := io.ReadFull(out.file, buffer)
		if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
			n = 0
		}
		if n > 0 {
			out.pos += int64(n)
			queued += n
			m.sendEvent(conn, "file.chunk", map[string]any{"transfer": out.transferID,
				"data": base64.StdEncoding.EncodeToString(buffer[:n])})
		}
		if n == 0 || out.pos >= out.size {
			event := "file.cancel"
			if out.pos == out.size {
				event = "file.end"
			}
			m.sendEvent(conn, event, map[string]any{"transfer": out.transferID})
			out.file.Close()
			conn.outgoing = conn.outgoing[1:]
		}
	}
}
