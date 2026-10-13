package control

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strconv"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

type state int

const (
	stateAwaitAuth state = iota
	stateChecking
	statePending
	statePaired
)

type connection struct {
	id                   int
	sock                 net.Conn
	writer               *lanWriter
	state                state
	device               Device
	transcript           []byte
	sendKey              []byte
	receiveKey           []byte
	sendCounter          uint64
	receiveCounter       uint64
	code                 string
	handshakeTimer       *time.Timer
	incoming             map[string]*incoming
	outgoing             []*outgoing
	relaySession         string
	relaySendSeq         uint32
	relayReceiveSeq      uint32
	relayPending         []byte
	lastHeardMs          int64
	accountCheckedMs     int64
	accountCheckInFlight bool
	transport            rpc.Transport
	peer                 *phonePeer
	ctx                  context.Context
	cancel               context.CancelFunc
	proxied              map[int]struct{}
	closed               bool
}

func (c *connection) relayed() bool { return c.relaySession != "" }

func (m *Module) newConnection(transport rpc.Transport) *connection {
	conn := &connection{id: m.nextID, transport: transport, incoming: map[string]*incoming{},
		proxied: map[int]struct{}{}}
	m.nextID++
	conn.ctx, conn.cancel = context.WithCancel(m.ctx)
	conn.peer = &phonePeer{m: m, conn: conn, id: string(transport) + ":" + strconv.Itoa(conn.id),
		transport: transport, done: make(chan struct{})}
	m.conns = append(m.conns, conn)
	m.open.Add(1)
	return conn
}

func (m *Module) connectionByID(id int) *connection {
	for _, conn := range m.conns {
		if conn.id == id {
			return conn
		}
	}
	return nil
}

func (m *Module) connectionForRelay(session string) *connection {
	for _, conn := range m.conns {
		if conn.relaySession == session {
			return conn
		}
	}
	return nil
}

func (m *Module) linkOpen(conn *connection) bool {
	if conn.closed {
		return false
	}
	if conn.relayed() {
		return true
	}
	return conn.writer != nil && conn.writer.open()
}

func (m *Module) writeFrame(conn *connection, payload []byte) bool {
	if !conn.relayed() {
		if conn.writer == nil {
			return false
		}
		return conn.writer.write(Frame(payload))
	}
	if len(payload) > MaxRelayFrameBytes {
		return false
	}
	encoded := encodeStdBase64(payload)
	for at := 0; at < len(encoded); at += RelayChunkChars {
		end := min(at+RelayChunkChars, len(encoded))
		more := end < len(encoded)
		chunk := map[string]any{
			"type":    "control",
			"action":  "data",
			"session": conn.relaySession,
			"node":    m.node,
			"seq":     int64(conn.relaySendSeq),
			"data":    encoded[at:end],
			"more":    more,
		}
		conn.relaySendSeq++
		if !m.relay.enqueue(chunk, conn.relaySession, "data", len(encoded[at:end])+200) {
			return false
		}
	}
	return true
}

func (m *Module) startHandshakeTimer(conn *connection) {
	conn.handshakeTimer = time.AfterFunc(handshakeTimeout, func() {
		m.post(func() {
			if !conn.closed && conn.state == stateAwaitAuth {
				m.closeConnection(conn, "handshake timed out")
			}
		})
	})
}

func (m *Module) adopt(stream net.Conn, hello, rest []byte) {
	now := m.now()
	if (len(m.conns) >= maxConnections && !m.evictIdleRelayed(now)) || !m.ensureIdentity() {
		stream.Close()
		return
	}
	if tcp, ok := stream.(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}
	conn := m.newConnection(rpc.LAN)
	conn.sock = stream
	conn.lastHeardMs = now
	conn.writer = newLANWriter(stream, func() {
		m.post(func() {
			if !conn.closed && len(conn.outgoing) > 0 {
				m.pumpOutgoing(conn)
			}
		})
	})
	go conn.writer.run()
	if !m.acceptHello(conn, hello) {
		m.closeConnection(conn, "bad hello")
		return
	}
	m.startHandshakeTimer(conn)
	go m.readLoop(conn, rest)
}

func (m *Module) readLoop(conn *connection, initial []byte) {
	buffer := initial
	drain := func() bool {
		for {
			payload, taken := takeFrame(&buffer)
			if taken == 0 {
				return true
			}
			if taken < 0 {
				m.post(func() { m.closeConnection(conn, "oversized frame") })
				return false
			}
			m.post(func() {
				if !conn.closed {
					m.handleFrame(conn, payload)
				}
			})
		}
	}
	if !drain() {
		return
	}
	chunk := make([]byte, 64<<10)
	for {
		n, err := conn.sock.Read(chunk)
		if n > 0 {
			buffer = append(buffer, chunk[:n]...)
			m.post(func() {
				if !conn.closed {
					conn.lastHeardMs = m.now()
				}
			})
			if !drain() {
				return
			}
		}
		if err != nil {
			reason := err.Error()
			if errors.Is(err, io.EOF) {
				reason = "The remote host closed the connection"
			}
			m.post(func() { m.closeConnection(conn, reason) })
			return
		}
	}
}

func (m *Module) acceptHello(conn *connection, hello []byte) bool {
	object := parseObject(hello)
	version, _ := jsonInt(object["v"])
	if jsonString(object["t"]) != HelloTag || version != ProtocolVersion {
		return false
	}
	clientKey := jsonString(object["key"])
	clientEph := jsonString(object["eph"])
	clientNonce := jsonString(object["nonce"])
	clientEphRaw := FromBase64Url(clientEph, keyBytes)
	if FromBase64Url(clientKey, keyBytes) == nil || clientEphRaw == nil ||
		FromBase64Url(clientNonce, clientNonceBytes) == nil || clientKey == m.publicKey {
		return false
	}
	eph := NewEphemeral()
	shared := X25519(eph.PrivateKey, clientEphRaw)
	if shared == nil {
		return false
	}
	serverEph := ToBase64Url(eph.PublicKey)
	conn.transcript = Transcript(clientKey, clientEph, clientNonce, m.publicKey, serverEph)
	keys := DeriveKeys(shared, conn.transcript)
	if !keys.Valid() {
		return false
	}
	conn.sendKey = keys.ServerToClient
	conn.receiveKey = keys.ClientToServer
	conn.device.Key = clientKey
	conn.device.Name = boundedText(object, "name", maxNameChars)
	conn.device.User = boundedText(object, "user", maxNameChars)
	conn.device.Platform = boundedText(object, "platform", 16)
	sig := m.sign(ServerSignedBytes(conn.transcript))
	if sig == "" {
		return false
	}
	welcome := map[string]any{
		"t":    WelcomeTag,
		"v":    ProtocolVersion,
		"key":  m.publicKey,
		"eph":  serverEph,
		"name": m.name,
		"user": m.user,
		"sig":  sig,
	}
	return m.writeFrame(conn, encodeJSON(welcome))
}

func (m *Module) handleFrame(conn *connection, payload []byte) {
	if conn.state == stateAwaitAuth {
		m.handleAuth(conn, payload)
		return
	}
	plain := OpenFrame(conn.receiveKey, conn.receiveCounter, payload)
	if plain == nil {
		m.closeConnection(conn, "frame did not authenticate")
		return
	}
	conn.receiveCounter++
	var message map[string]any
	if err := json.Unmarshal(plain, &message); err != nil || message == nil {
		m.closeConnection(conn, "malformed message")
		return
	}
	m.handleMessage(conn, message)
}

func (m *Module) handleAuth(conn *connection, payload []byte) {
	object := parseObject(payload)
	sig := jsonString(object["sig"])
	if jsonString(object["t"]) != AuthTag || !VerifyEd25519(conn.device.Key, sig, ClientSignedBytes(conn.transcript)) {
		m.closeConnection(conn, "authentication failed")
		return
	}
	if conn.handshakeTimer != nil {
		conn.handshakeTimer.Stop()
		conn.handshakeTimer = nil
	}
	for _, other := range append([]*connection(nil), m.conns...) {
		if other != conn && other.state != stateAwaitAuth && other.device.Key == conn.device.Key {
			m.closeConnection(other, "replaced")
		}
	}
	browser := conn.device.Platform == "web"
	if _, stored := m.devices[conn.device.Key]; browser && stored {
		delete(m.devices, conn.device.Key)
		m.saveDevices()
	}
	if _, stored := m.devices[conn.device.Key]; !browser && stored && !m.needsAccountCheck(conn) {
		m.admitStored(conn)
		m.devicesChanged()
		return
	}
	conn.state = stateChecking
	conn.accountCheckInFlight = true
	key := conn.device.Key
	device := conn.device
	m.devicesChanged()
	m.checkAccount(device, func(verdict string) {
		if conn.closed || conn.state != stateChecking || conn.device.Key != key {
			return
		}
		conn.accountCheckInFlight = false
		m.settleAccountCheck(conn, verdict)
		m.devicesChanged()
	})
}

func (m *Module) checkAccount(device Device, done func(verdict string)) {
	hook := m.accountHook
	go func() {
		verdict := VerdictUnreachable
		if hook != nil {
			verdict = hook(device)
		} else {
			verdict = m.askUI(device)
		}
		m.post(func() { done(verdict) })
	}()
}

func (m *Module) askUI(device Device) string {
	if m.core.UI() == nil {
		return VerdictUnreachable
	}
	ctx, cancel := context.WithTimeout(m.ctx, accountCheckTimeout)
	defer cancel()
	raw, err := m.core.CallUI(ctx, "ui.control.accountCheck", map[string]any{"device": device.record()})
	if err != nil {
		return VerdictUnreachable
	}
	var result struct {
		Verdict string `json:"verdict"`
	}
	if json.Unmarshal(raw, &result) != nil {
		return VerdictUnreachable
	}
	switch result.Verdict {
	case VerdictOwned, VerdictNotOwned:
		return result.Verdict
	}
	return VerdictUnreachable
}

func (m *Module) needsAccountCheck(conn *connection) bool {
	if conn.device.Platform == "web" {
		return true
	}
	stored, ok := m.devices[conn.device.Key]
	if !ok || stored.Approval == approvalAccount {
		return true
	}
	return stored.Approval == "" && conn.relayed()
}

func (m *Module) recheckAccount(conn *connection) {
	conn.accountCheckInFlight = true
	key := conn.device.Key
	m.checkAccount(conn.device, func(verdict string) {
		if conn.closed || conn.device.Key != key {
			return
		}
		conn.accountCheckInFlight = false
		now := m.now()
		switch verdict {
		case VerdictUnreachable:
			conn.accountCheckedMs = now - accountRecheckMs + accountRetryMs
		case VerdictOwned:
			conn.accountCheckedMs = now
			if stored, ok := m.devices[key]; ok {
				stored.AccountCheckedMs = now
				m.devices[key] = stored
				m.saveDevices()
			}
		default:
			if stored, ok := m.devices[key]; ok && stored.Approval == approvalAccount {
				delete(m.devices, key)
				m.saveDevices()
			}
			m.closeConnection(conn, "revoked")
		}
	})
}

func (m *Module) admitStored(conn *connection) {
	stored := m.devices[conn.device.Key]
	if conn.device.Name != "" {
		stored.Name = conn.device.Name
	}
	if conn.device.User != "" && stored.Approval == approvalAccount {
		stored.User = conn.device.User
	}
	stored.Platform = conn.device.Platform
	stored.LastSeenMs = m.now()
	conn.device = stored
	m.rememberDevice(stored)
	conn.state = statePaired
	m.sendEvent(conn, "session", m.sessionInfo(conn))
	m.markPaired(conn)
}

func (m *Module) settleAccountCheck(conn *connection, verdict string) {
	now := m.now()
	browser := conn.device.Platform == "web"
	device, found := m.devices[conn.device.Key]
	stored := !browser && found
	switch verdict {
	case VerdictOwned:
		conn.accountCheckedMs = now
		if !stored {
			m.pairDevice(conn, approvalAccount)
			return
		}
		if device.Approval == "" {
			device.Approval = approvalAccount
		}
		device.AccountCheckedMs = now
		m.devices[conn.device.Key] = device
		m.admitStored(conn)
		return
	case VerdictNotOwned:
		if stored && device.Approval == approvalAccount {
			delete(m.devices, conn.device.Key)
			m.saveDevices()
			stored = false
		}
		if stored && !conn.relayed() {
			m.admitStored(conn)
		} else {
			m.requestPairing(conn)
		}
		return
	}
	if stored && (!conn.relayed() || now-device.AccountCheckedMs < accountGraceMs) {
		m.admitStored(conn)
		return
	}
	if conn.relayed() {
		m.refuseRelayed(conn, "account_check_failed")
		return
	}
	m.requestPairing(conn)
}

func (m *Module) refuseRelayed(conn *connection, reason string) {
	conn.state = statePending
	m.sendEvent(conn, "pairing-denied", map[string]any{"reason": reason})
	m.closeConnection(conn, reason)
}

func (m *Module) requestPairing(conn *connection) {
	conn.state = statePending
	if conn.relayed() {
		m.sendEvent(conn, "pairing-denied", map[string]any{"reason": "pair_on_lan"})
		m.closeConnection(conn, "Approve this phone on the desktop's own network first, or sign "+
			"both in to the same ForkMesh account.")
		return
	}
	conn.code = PairingCode(conn.transcript)
	m.sendEvent(conn, "session", m.sessionInfo(conn))
	m.notifyUI("ui.control.pairing", map[string]any{"connection": conn.id, "device": conn.device.record(),
		"code": conn.code}, nil)
}

func (m *Module) pairDevice(conn *connection, approval string) {
	now := m.now()
	conn.device.PairedAtMs = now
	conn.device.LastSeenMs = now
	conn.device.Approval = approval
	conn.device.AccountCheckedMs = 0
	if approval == approvalAccount {
		conn.device.AccountCheckedMs = now
	}
	if conn.device.Platform != "web" {
		m.rememberDevice(conn.device)
	}
	conn.state = statePaired
	conn.code = ""
	m.sendEvent(conn, "session", m.sessionInfo(conn))
	m.markPaired(conn)
}

func (m *Module) markPaired(conn *connection) {
	conn.peer.setDevice(conn.device)
	conn.peer.paired.Store(true)
	m.core.Hub.Add(conn.peer)
	m.peers.Store(conn.id, conn.peer)
	m.notifyUI("ui.control.opened", m.openedParams(conn), nil)
}

func (m *Module) sessionInfo(conn *connection) map[string]any {
	transport := "lan"
	if conn.relayed() {
		transport = "relay"
	}
	info := map[string]any{
		"paired":    conn.state == statePaired,
		"name":      m.name,
		"user":      m.user,
		"key":       m.publicKey,
		"protocol":  ProtocolVersion,
		"transport": transport,
	}
	if conn.state == statePending {
		info["code"] = conn.code
	}
	return info
}

func (m *Module) approvePairing(id int) {
	conn := m.connectionByID(id)
	if conn == nil || conn.state != statePending {
		return
	}
	m.pairDevice(conn, approvalCode)
	m.notifyUI("ui.control.pairingClosed", map[string]any{"connection": id}, nil)
	m.devicesChanged()
}

func (m *Module) denyPairing(id int) {
	conn := m.connectionByID(id)
	if conn == nil || conn.state != statePending {
		return
	}
	m.sendEvent(conn, "pairing-denied", map[string]any{})
	m.closeConnection(conn, "pairing denied")
}

func (m *Module) forget(key string) {
	if _, ok := m.devices[key]; !ok {
		return
	}
	delete(m.devices, key)
	m.saveDevices()
	for _, conn := range append([]*connection(nil), m.conns...) {
		if conn.device.Key == key {
			m.closeConnection(conn, "forgotten")
		}
	}
	m.devicesChanged()
}

func isFileTransfer(method string) bool {
	const browse = "files.browse."
	if len(method) < 6 || method[:6] != "files." {
		return false
	}
	return !(len(method) > len(browse) && method[:len(browse)] == browse)
}

func (m *Module) handleMessage(conn *connection, message map[string]any) {
	requestID, _ := jsonInt(message["id"])
	method := jsonString(message["method"])
	if requestID <= 0 || method == "" {
		return
	}
	params := jsonObject(message["params"])
	if method == "ping" {
		m.respond(conn, requestID, json.RawMessage("{}"))
		return
	}
	if conn.state != statePaired {
		m.respondError(conn, requestID, "not_paired")
		return
	}
	if isFileTransfer(method) {
		if conn.relayed() {
			m.respondError(conn, requestID, "lan_only")
			return
		}
		m.handleFileRequest(conn, requestID, method, params)
		return
	}
	m.dispatch(conn, requestID, method, params)
}

func (m *Module) dispatch(conn *connection, requestID int, method string, params map[string]any) {
	raw := encodeJSON(params)
	if handler, ok := m.core.Registry.Resolve(method); ok {
		peer := conn.peer
		ctx := conn.ctx
		go func() {
			var result any
			var err error
			func() {
				defer func() {
					if recovered := recover(); recovered != nil {
						m.logf("control: %s panicked: %v", method, recovered)
						err = rpc.Fail("internal")
					}
				}()
				result, err = handler(ctx, &rpc.Request{Peer: peer, ID: int64(requestID), Method: method, Params: raw})
			}()
			if rpc.Delegated(err) {
				forwarded := raw
				if params, ok := rpc.DelegatedParams(err); ok && len(params) > 0 {
					forwarded = params
				}
				m.post(func() {
					if !conn.closed && conn.state == statePaired {
						m.proxy(conn, requestID, method, forwarded)
					}
				})
				return
			}
			if err != nil {
				code := rpc.Code(err)
				m.post(func() { m.respondError(conn, requestID, code) })
				return
			}
			encoded := encodeJSON(result)
			m.post(func() { m.respond(conn, requestID, encoded) })
		}()
		return
	}
	m.proxy(conn, requestID, method, raw)
}

func (m *Module) proxy(conn *connection, requestID int, method string, raw []byte) {
	if m.core.UI() == nil {
		m.respondError(conn, requestID, "desktop_unavailable")
		return
	}
	if len(conn.proxied) >= maxProxiedPerConn {
		for id := range conn.proxied {
			delete(conn.proxied, id)
			break
		}
	}
	conn.proxied[requestID] = struct{}{}
	m.notifyUI("ui.control.request", map[string]any{
		"connection": conn.id,
		"id":         requestID,
		"method":     method,
		"params":     json.RawMessage(raw),
		"device":     conn.device.record(),
		"relayed":    conn.relayed(),
	}, func() {
		m.post(func() {
			if _, waiting := conn.proxied[requestID]; waiting && !conn.closed {
				delete(conn.proxied, requestID)
				m.respondError(conn, requestID, "desktop_unavailable")
			}
		})
	})
}

func (m *Module) reply(id int, message json.RawMessage) {
	conn := m.connectionByID(id)
	if conn == nil || conn.state != statePaired {
		return
	}
	var fields map[string]json.RawMessage
	if json.Unmarshal(message, &fields) != nil || fields == nil {
		return
	}
	if rawID, ok := fields["id"]; ok {
		if _, isResponse := fields["ok"]; isResponse {
			var value any
			_ = json.Unmarshal(rawID, &value)
			if requestID, ok := jsonInt(value); ok {
				delete(conn.proxied, requestID)
			}
		}
	}
	plain := encodeJSON(fields)
	if plain == nil {
		return
	}
	m.writeJSON(conn, plain)
}

func (m *Module) writeJSON(conn *connection, plain []byte) bool {
	if conn.closed || conn.state == stateAwaitAuth || !m.linkOpen(conn) {
		return false
	}
	sealed := SealFrame(conn.sendKey, conn.sendCounter, plain)
	if sealed == nil || len(sealed) > MaxFrameBytes || (conn.relayed() && len(sealed) > MaxRelayFrameBytes) {
		return false
	}
	conn.sendCounter++
	if !m.writeFrame(conn, sealed) {
		if conn.relayed() {
			m.post(func() { m.closeConnection(conn, "relay unavailable") })
		}
		return false
	}
	return true
}

func (m *Module) sendEvent(conn *connection, event string, data map[string]any) bool {
	if data == nil {
		data = map[string]any{}
	}
	return m.writeJSON(conn, compactEventBytes(event, encodeJSON(data)))
}

func (m *Module) respond(conn *connection, requestID int, result json.RawMessage) {
	if conn.closed {
		return
	}
	m.writeJSON(conn, responseBytes(requestID, result))
}

func (m *Module) respondError(conn *connection, requestID int, code string) {
	if conn.closed {
		return
	}
	m.writeJSON(conn, errorBytes(requestID, code))
}

func (m *Module) broadcast(plain []byte) {
	for _, conn := range m.conns {
		if conn.state == statePaired {
			m.writeJSON(conn, plain)
		}
	}
}

func (m *Module) closeConnection(conn *connection, reason string) {
	if conn == nil || conn.closed {
		return
	}
	index := -1
	for i, candidate := range m.conns {
		if candidate == conn {
			index = i
			break
		}
	}
	if index >= 0 {
		m.conns = append(m.conns[:index], m.conns[index+1:]...)
	}
	conn.closed = true
	m.open.Add(-1)
	if conn.relayed() {
		session := conn.relaySession
		m.relayToldGone = noteBounded(m.relayToldGone, session)
		m.relay.enqueueClose(map[string]any{
			"type":    "control",
			"action":  "close",
			"session": session,
			"node":    m.node,
			"reason":  leftUTF16(reason, 160),
		}, session, func() {
			m.post(func() { m.relayUndelivered = noteBounded(m.relayUndelivered, session) })
		})
	}
	wasPending := conn.state == statePending
	wasVisible := conn.state != stateAwaitAuth
	for _, in := range conn.incoming {
		in.discard()
	}
	conn.incoming = map[string]*incoming{}
	for _, out := range conn.outgoing {
		out.file.Close()
	}
	conn.outgoing = nil
	if conn.handshakeTimer != nil {
		conn.handshakeTimer.Stop()
		conn.handshakeTimer = nil
	}
	if conn.writer != nil {
		conn.writer.finish()
	}
	conn.cancel()
	conn.peer.close()
	m.core.Hub.Remove(conn.peer)
	m.peers.Delete(conn.id)
	conn.proxied = map[int]struct{}{}
	if wasVisible {
		m.notifyUI("ui.control.closed", map[string]any{"connection": conn.id, "reason": reason}, nil)
	}
	if wasPending {
		m.notifyUI("ui.control.pairingClosed", map[string]any{"connection": conn.id}, nil)
	}
	if wasVisible {
		m.devicesChanged()
	}
}

func noteBounded(list []string, value string) []string {
	for _, existing := range list {
		if existing == value {
			return list
		}
	}
	list = append(list, value)
	for len(list) > maxRelayNotes {
		list = list[1:]
	}
	return list
}

func removeString(list []string, value string) []string {
	out := list[:0]
	for _, existing := range list {
		if existing != value {
			out = append(out, existing)
		}
	}
	return out
}

func containsString(list []string, value string) bool {
	for _, existing := range list {
		if existing == value {
			return true
		}
	}
	return false
}

func (m *Module) rememberDevice(device Device) {
	m.devices[device.Key] = device
	m.saveDevices()
}

func (m *Module) saveDevices() {
	if err := SaveDevices(m.StorePath, m.devices); err != nil {
		m.logf("control: saving paired devices: %v", err)
	}
}

func (m *Module) maintain(nowMs int64) {
	for session, since := range m.relayOpeningSince {
		if nowMs-since >= relayOpeningTimeoutMs {
			delete(m.relayOpening, session)
			delete(m.relayOpeningSeq, session)
			delete(m.relayOpeningSince, session)
		}
	}
	var idle, recheck []*connection
	for _, conn := range m.conns {
		if conn.state != statePaired {
			continue
		}
		if conn.relayed() && nowMs-conn.lastHeardMs >= relayIdleMs {
			idle = append(idle, conn)
		} else if !conn.accountCheckInFlight && m.needsAccountCheck(conn) &&
			nowMs-conn.accountCheckedMs >= accountRecheckMs {
			recheck = append(recheck, conn)
		}
	}
	for _, conn := range idle {
		m.closeConnection(conn, "idle")
	}
	for _, conn := range recheck {
		if !conn.closed {
			m.recheckAccount(conn)
		}
	}
}

func (m *Module) evictIdleRelayed(nowMs int64) bool {
	var oldest *connection
	for _, conn := range m.conns {
		if conn.relayed() && nowMs-conn.lastHeardMs >= relayEvictIdleMs &&
			(oldest == nil || conn.lastHeardMs < oldest.lastHeardMs) {
			oldest = conn
		}
	}
	if oldest == nil {
		return false
	}
	m.closeConnection(oldest, "idle")
	return true
}
