package mesh

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const (
	defaultBeaconInterval   = 15 * time.Second
	defaultPeerTimeout      = 60 * time.Second
	sweepInterval           = 5 * time.Second
	addressSweepEveryBeats  = 4
	addressSweepEveryAlone  = 2
	sweepBatch              = 16
	sweepBatchGap           = 500 * time.Millisecond
	sweepHeadMaxTries       = 4
	linksBlockedAfterBeats  = 3
	dialTimeout             = 8 * time.Second
	redialDelay             = 1500 * time.Millisecond
	redialMinLinkMs         = 5000
	defaultLinkReplaceAfter = 10 * time.Second
	keepAliveIdle           = 10 * time.Second
	keepAliveInterval       = 5 * time.Second
	keepAliveProbes         = 3
	unackedTimeoutMs        = 30000
	typingStop              = 4 * time.Second
	historySaveDelay        = time.Second
	discoveryWriteDeadline  = 50 * time.Millisecond
	byeFlush                = 50 * time.Millisecond
	closeFlush              = 2 * time.Second
	reasonDuplicate         = "duplicate link"
	reasonReplaced          = "replaced"
	reasonIdentityChanged   = "identity changed"
	reasonRemoteClosed      = "The remote host closed the connection"
	reasonOversized         = "oversized frame"
	reasonNetworkChanged    = "network changed"
	reasonDesktopDetached   = "desktop detached"
	defaultAccountKind      = "node"
	maxDatagramBytes        = 65535
	readChunk               = 64 << 10
)

type ChatMessage struct {
	ID           string `json:"id"`
	Conversation string `json:"conversation"`
	SenderID     string `json:"senderId"`
	SenderName   string `json:"senderName"`
	Text         string `json:"text"`
	TimestampMs  int64  `json:"timestampMs"`
	FileName     string `json:"fileName,omitempty"`
	FileMime     string `json:"fileMime,omitempty"`
	FileData     []byte `json:"file,omitempty"`
	Self         bool   `json:"self"`
	Historical   bool   `json:"historical"`
}

type Member struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	NodeName    string `json:"nodeName"`
	OwnerUser   string `json:"ownerUser"`
	AccountKind string `json:"accountKind"`
	Platform    string `json:"platform"`
	Version     string `json:"version"`
	Note        string `json:"note"`
	Self        bool   `json:"self"`
	Online      bool   `json:"online"`
}

type DropInfo struct {
	Bridge     int    `json:"link"`
	Bluetooth  bool   `json:"bluetooth"`
	Reason     string `json:"reason"`
	Superseded bool   `json:"superseded"`
	Settled    bool   `json:"settled"`
}

type Bluetooth struct {
	Active    bool   `json:"active"`
	Seen      int    `json:"seen"`
	Dials     int    `json:"dials"`
	Opened    int    `json:"opened"`
	LastError string `json:"lastError"`
}

type Hooks struct {
	Message     func(ChatMessage)
	Queued      func(id string, queued bool)
	Wake        func(tokens []string)
	Typing      func(peerID, peerName string, active bool)
	Peers       func()
	System      func(text string)
	Blocked     func()
	Inbound     func()
	SealPeers   func([]PeerKey)
	BridgeWrite func(link int, data []byte)
	BridgeClose func(link int, reason string)
	LinkDropped func(DropInfo)
	Control     func(conn net.Conn, hello, rest []byte) bool
}

type Config struct {
	UserName         string
	NodeName         string
	OwnerUser        string
	NodeID           string
	AccountKind      string
	Platform         string
	Version          string
	SealKey          []byte
	SealPeers        []PeerKey
	HistoryFile      string
	DiscoveryPort    int
	BeaconInterval   time.Duration
	PeerTimeout      time.Duration
	LinkReplaceAfter time.Duration
	DisableMulticast bool
	DisableSweep     bool
	Interfaces       func() []Interface
	Group            string
	BindHost         string
	Control          bool
}

type peer struct {
	name, nodeName, ownerUser, accountKind, platform, version string
	address                                                   net.IP
	port                                                      int
	lastSeenMs                                                int64
	unlinkedSinceMs                                           int64
	linked                                                    bool
	bluetooth                                                 bool
}

type link struct {
	conn         net.Conn
	writer       *linkWriter
	bridge       int
	buffer       []byte
	peerID       string
	named        atomic.Bool
	keyKnown     bool
	outbound     bool
	bluetooth    bool
	helloMs      int64
	pending      []Object
	closedRemote bool
}

type diagnostics struct {
	multicastSent, broadcastSent, unicastSent, sweepSent, sendErrors     int
	lastSendError                                                        string
	datagramsIn, ownBeaconsIn, offNetworkIn, undecryptableIn, peerBeacon int
	lastBeaconFrom                                                       string
	lastBeaconMs                                                         int64
	dials, dialsOK, dialsFailed                                          int
	lastDialError                                                        string
	inboundAccepted, inboundRefused                                      int
	framesIn, framesUndecryptable, framesUnsealable, chatIn, chatOut     int
	lastDropReason, discoveryError, listenError                          string
}

type Node struct {
	hooks  Hooks
	crypto *RoomCrypto
	seal   *RoomSeal
	ifaces func() []Interface

	queueMu sync.Mutex
	queue   []func()
	wake    chan struct{}
	quit    chan struct{}
	exited  chan struct{}
	closing atomic.Bool

	userName, nodeName, ownerUser, accountKind, nodeID, platform, version string
	historyPath                                                           string
	discoveryPort                                                         int
	beaconInterval, peerTimeout, linkReplaceAfter                         time.Duration
	multicast, sweep, control                                             bool
	group                                                                 net.IP
	bindHost                                                              string

	running        bool
	gen            int
	listener       net.Listener
	listenPort     int
	udp            *net.UDPConn
	links          []*link
	peers          map[string]*peer
	wakeTokens     map[string]string
	wakeOrder      []string
	history        []Object
	historyChars   int
	queued         map[string]bool
	historyLoaded  bool
	historyTimer   *time.Timer
	seen           map[string]bool
	seenOrder      []string
	dialing        map[string]bool
	sweepQueue     []net.IP
	sweepDatagram  []byte
	sweepHeadTries int
	sweepTimer     *time.Timer
	typingTimer    *time.Timer
	addrSignature  string
	beaconCount    int
	linksBlocked   bool
	inboundSeen    bool
	typingActive   bool
	diag           diagnostics
	startedMs      int64
	ble            Bluetooth
	lastOpenNote   string
}

func NewNode(cfg Config, hooks Hooks) (*Node, error) {
	crypto, err := NewRoomCrypto(RoomName)
	if err != nil {
		return nil, err
	}
	seal, err := NewRoomSeal(RoomName, cfg.SealKey)
	if err != nil {
		return nil, err
	}
	seal.LoadPeers(cfg.SealPeers)
	n := &Node{hooks: hooks, crypto: crypto, seal: seal, ifaces: cfg.Interfaces,
		wake: make(chan struct{}, 1), quit: make(chan struct{}), exited: make(chan struct{}),
		userName: left16(trimmed(cfg.UserName), maxDisplayNameChars), nodeName: left16(trimmed(cfg.NodeName), maxDisplayNameChars),
		ownerUser: left16(trimmed(cfg.OwnerUser), maxDisplayNameChars), accountKind: defaultAccountKind,
		nodeID: left16(trimmed(cfg.NodeID), maxIDChars), platform: cfg.Platform, version: left16(cfg.Version, maxVersionChars),
		historyPath: cfg.HistoryFile, discoveryPort: cfg.DiscoveryPort, beaconInterval: cfg.BeaconInterval,
		peerTimeout: cfg.PeerTimeout, linkReplaceAfter: cfg.LinkReplaceAfter, multicast: !cfg.DisableMulticast,
		sweep: !cfg.DisableSweep, control: cfg.Control,
		peers: map[string]*peer{}, wakeTokens: map[string]string{}, queued: map[string]bool{}, seen: map[string]bool{},
		dialing: map[string]bool{}}
	if kind := left16(trimmed(cfg.AccountKind), 16); kind != "" {
		n.accountKind = kind
	}
	if n.nodeID == "" {
		n.nodeID = newUUID()
	}
	if n.platform == "" {
		n.platform = CurrentPlatform()
	}
	if n.discoveryPort <= 0 {
		n.discoveryPort = DefaultDiscoveryPort
	}
	if n.beaconInterval <= 0 {
		n.beaconInterval = defaultBeaconInterval
	}
	if n.peerTimeout <= 0 {
		n.peerTimeout = defaultPeerTimeout
	}
	if n.linkReplaceAfter <= 0 {
		n.linkReplaceAfter = defaultLinkReplaceAfter
	}
	if n.ifaces == nil {
		n.ifaces = SystemInterfaces
	}
	n.bindHost = cfg.BindHost
	n.group = net.ParseIP(cfg.Group).To4()
	if n.group == nil {
		n.group = net.ParseIP(MulticastGroupV4).To4()
	}
	seal.OnPersist = func(peers []PeerKey) {
		if n.hooks.SealPeers != nil {
			n.hooks.SealPeers(peers)
		}
	}
	go n.loop()
	return n, nil
}

func CurrentPlatform() string {
	switch runtime.GOOS {
	case "darwin":
		return "macos"
	case "windows", "linux", "android", "ios":
		return runtime.GOOS
	}
	return "unknown"
}

func newUUID() string {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 16)
	}
	raw[6] = raw[6]&0x0f | 0x40
	raw[8] = raw[8]&0x3f | 0x80
	text := hex.EncodeToString(raw)
	return text[0:8] + "-" + text[8:12] + "-" + text[12:16] + "-" + text[16:20] + "-" + text[20:32]
}

func nowMs() int64 { return time.Now().UnixMilli() }

func (n *Node) post(task func()) bool {
	if n.closing.Load() {
		return false
	}
	n.queueMu.Lock()
	n.queue = append(n.queue, task)
	n.queueMu.Unlock()
	select {
	case n.wake <- struct{}{}:
	default:
	}
	return true
}

func (n *Node) Call(task func() any) any {
	reply := make(chan any, 1)
	if !n.post(func() { reply <- task() }) {
		return nil
	}
	select {
	case value := <-reply:
		return value
	case <-n.exited:
		return nil
	}
}

func (n *Node) Do(task func()) bool {
	return n.post(task)
}

func (n *Node) loop() {
	defer close(n.exited)
	for {
		select {
		case <-n.quit:
			return
		case <-n.wake:
		}
		for {
			n.queueMu.Lock()
			tasks := n.queue
			n.queue = nil
			n.queueMu.Unlock()
			if len(tasks) == 0 {
				break
			}
			for _, task := range tasks {
				n.run(task)
			}
		}
	}
}

func (n *Node) run(task func()) {
	defer func() {
		_ = recover()
	}()
	task()
}

func (n *Node) Close() {
	n.Call(func() any {
		n.stop()
		return nil
	})
	if n.closing.CompareAndSwap(false, true) {
		close(n.quit)
	}
	<-n.exited
}

func (n *Node) after(delay time.Duration, task func()) *time.Timer {
	generation := n.gen
	return time.AfterFunc(delay, func() {
		n.post(func() {
			if n.gen == generation {
				task()
			}
		})
	})
}

func (n *Node) every(delay time.Duration, task func()) {
	var arm func()
	arm = func() {
		n.after(delay, func() {
			if !n.running {
				return
			}
			task()
			arm()
		})
	}
	arm()
}

func (n *Node) interfaces() []Interface { return n.ifaces() }

func (n *Node) system(text string) {
	if n.hooks.System != nil {
		n.hooks.System(text)
	}
}

func (n *Node) noteMembershipChange() {
	if n.hooks.Peers != nil {
		n.hooks.Peers()
	}
}

func (n *Node) Start() bool {
	result, _ := n.Call(func() any { return n.start() }).(bool)
	return result
}

func (n *Node) start() bool {
	if n.running {
		return true
	}
	n.loadHistory()
	n.diag = diagnostics{}
	n.startedMs = nowMs()
	n.running = true
	n.gen++
	n.addrSignature = addressSignature(n.interfaces())
	lan := n.openNetwork()
	n.every(n.beaconInterval, func() {
		n.refreshNetwork()
		n.sendBeacon()
		n.sendUnicastBeacons()
		n.sweepPeers()
	})
	n.every(sweepInterval, n.sweepPeers)
	n.beaconCount = 0
	n.sendBeacon()
	n.sendUnicastBeacons()
	if lan {
		n.lastOpenNote = fmt.Sprintf("Local mesh: %s is open over %s, end-to-end encrypted, and never leaves the room.", Conversation, "this network")
	} else {
		n.lastOpenNote = fmt.Sprintf("Local mesh: %s is waiting for a local network or Bluetooth, and opens on its own when one appears.", Conversation)
	}
	n.system(n.lastOpenNote)
	return true
}

func (n *Node) openNetwork() bool {
	if !n.running || len(meshInterfaces(n.interfaces())) == 0 || !n.openListener() {
		return false
	}
	if !n.openDiscovery() {
		n.system("Local mesh: network discovery could not open; listening only.")
	}
	return true
}

func (n *Node) refreshNetwork() {
	if !n.running {
		return
	}
	all := n.interfaces()
	signature := addressSignature(all)
	changed := signature != n.addrSignature
	n.addrSignature = signature
	if n.listener == nil {
		if n.openNetwork() {
			n.beaconCount = 0
			n.system("Local mesh: joined this network, looking for devices.")
		}
		return
	}
	if n.udp == nil {
		n.openDiscovery()
	}
	if !changed {
		return
	}
	n.beaconCount = 0
	addresses := allAddresses(all)
	stranded := []*link{}
	for _, l := range n.links {
		if l.conn == nil {
			continue
		}
		local, ok := l.conn.LocalAddr().(*net.TCPAddr)
		if !ok || local.IP == nil {
			continue
		}
		if !addresses[normalIP(local.IP).String()] {
			stranded = append(stranded, l)
		}
	}
	for _, l := range stranded {
		n.dropLink(l, reasonNetworkChanged)
	}
}

func (n *Node) openListener() bool {
	if n.listener != nil {
		return true
	}
	listener, err := net.Listen("tcp", net.JoinHostPort(n.bindHost, strconv.Itoa(n.discoveryPort)))
	if err != nil {
		listener, err = net.Listen("tcp", net.JoinHostPort(n.bindHost, "0"))
	}
	if err != nil {
		n.diag.listenError = err.Error()
		n.system(fmt.Sprintf("Local mesh: could not listen for peers (%s).", err.Error()))
		return false
	}
	n.listener = listener
	n.listenPort = listener.Addr().(*net.TCPAddr).Port
	go n.acceptLoop(listener)
	return true
}

func (n *Node) acceptLoop(listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			var timeout interface{ Timeout() bool }
			if errors.As(err, &timeout) && timeout.Timeout() {
				continue
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		if !n.post(func() { n.onNewConnection(listener, conn) }) {
			conn.Close()
		}
	}
}

func (n *Node) openDiscovery() bool {
	if n.udp != nil {
		return true
	}
	if !n.multicast {
		return false
	}
	config := net.ListenConfig{Control: controlReuse}
	host := n.bindHost
	if host == "" {
		host = "0.0.0.0"
	}
	packet, err := config.ListenPacket(context.Background(), "udp4", net.JoinHostPort(host, strconv.Itoa(n.discoveryPort)))
	if err != nil {
		n.diag.discoveryError = err.Error()
		return false
	}
	n.diag.discoveryError = ""
	udp := packet.(*net.UDPConn)
	setupDiscovery(udp)
	n.udp = udp
	go n.readDatagrams(udp)
	n.joinMulticastGroups()
	return true
}

func (n *Node) readDatagrams(udp *net.UDPConn) {
	buffer := make([]byte, maxDatagramBytes)
	for {
		size, from, err := udp.ReadFromUDP(buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if size <= 0 {
			continue
		}
		datagram := append([]byte(nil), buffer[:size]...)
		if !n.post(func() {
			if n.udp == udp {
				n.onDatagram(datagram, from)
			}
		}) {
			return
		}
	}
}

func (n *Node) writeDiscovery(datagram []byte, to net.IP, port int, counter *int) bool {
	if n.udp == nil {
		return false
	}
	_ = n.udp.SetWriteDeadline(time.Now().Add(discoveryWriteDeadline))
	_, err := n.udp.WriteToUDP(datagram, &net.UDPAddr{IP: to, Port: port})
	if err != nil {
		n.diag.sendErrors++
		n.diag.lastSendError = to.String() + ": " + err.Error()
		return false
	}
	*counter++
	return true
}

func (n *Node) joinMulticastGroups() {
	if n.udp == nil {
		return
	}
	group := n.group
	for _, iface := range meshInterfaces(n.interfaces()) {
		if local := firstIPv4(iface); local != nil {
			_ = joinGroup(n.udp, group, local)
		}
	}
	_ = joinGroup(n.udp, group, nil)
}

func (n *Node) sendBeacon() {
	if !n.running || n.listener == nil {
		return
	}
	n.joinMulticastGroups()
	datagram := n.beaconDatagram(false)
	if datagram == nil || n.udp == nil {
		return
	}
	group := n.group
	interfaces := meshInterfaces(n.interfaces())
	for _, iface := range interfaces {
		if local := firstIPv4(iface); local != nil {
			_ = setMulticastInterface(n.udp, local)
		}
		n.writeDiscovery(datagram, group, n.discoveryPort, &n.diag.multicastSent)
	}
	if !n.sweep {
		return
	}
	for _, iface := range interfaces {
		if !iface.Sweepable {
			continue
		}
		for _, addr := range iface.Addrs {
			if addr.IP.To4() == nil || addr.Broadcast == nil || !IsLocalNetworkAddress(addr.IP) {
				continue
			}
			n.writeDiscovery(datagram, addr.Broadcast, n.discoveryPort, &n.diag.broadcastSent)
		}
	}
}

func (n *Node) advertisesControl() bool {
	return n.control && n.hooks.Control != nil
}

func (n *Node) beaconDatagram(reply bool) []byte {
	if n.listener == nil {
		return nil
	}
	beacon := Beacon{NodeID: n.nodeID, Name: n.userName, NodeName: n.nodeName, OwnerUser: n.ownerUser,
		AccountKind: n.accountKind, Platform: n.platform, Version: n.version, Port: n.listenPort,
		TimestampMs: nowMs(), Reply: reply, Control: n.advertisesControl()}
	envelope := n.crypto.Encrypt(BeaconObject(beacon))
	if envelope == nil {
		return nil
	}
	return marshal(envelope)
}

func (n *Node) sendUnicastBeacons() {
	if !n.running || n.listener == nil || n.udp == nil {
		return
	}
	datagram := n.beaconDatagram(false)
	if datagram == nil {
		return
	}
	sent := map[string]bool{}
	for _, id := range n.peerIDs() {
		p := n.peers[id]
		if p.address == nil || n.linkForPeer(id) != nil || sent[p.address.String()] {
			continue
		}
		sent[p.address.String()] = true
		n.writeDiscovery(datagram, p.address, n.discoveryPort, &n.diag.unicastSent)
	}
	every := addressSweepEveryBeats
	if len(n.links) == 0 {
		every = addressSweepEveryAlone
	}
	if !n.sweep {
		return
	}
	count := n.beaconCount
	n.beaconCount++
	if count%every != 0 {
		return
	}
	n.sweepQueue = nil
	n.sweepHeadTries = 0
	for _, iface := range meshInterfaces(n.interfaces()) {
		if !iface.Sweepable {
			continue
		}
		for _, addr := range iface.Addrs {
			for _, host := range SweepAddresses(addr.IP, addr.Prefix) {
				if sent[host.String()] {
					continue
				}
				sent[host.String()] = true
				n.sweepQueue = append(n.sweepQueue, host)
			}
		}
	}
	n.sweepDatagram = datagram
	n.drainSweep()
}

func (n *Node) drainSweep() {
	if !n.running || n.udp == nil || n.sweepDatagram == nil {
		n.sweepQueue = nil
		n.sweepDatagram = nil
		return
	}
	for sent := 0; sent < sweepBatch && len(n.sweepQueue) > 0; sent++ {
		if !n.writeDiscovery(n.sweepDatagram, n.sweepQueue[0], n.discoveryPort, &n.diag.sweepSent) {
			n.sweepHeadTries++
			if n.sweepHeadTries < sweepHeadMaxTries {
				break
			}
		}
		n.sweepQueue = n.sweepQueue[1:]
		n.sweepHeadTries = 0
	}
	if len(n.sweepQueue) == 0 {
		n.sweepDatagram = nil
		return
	}
	if n.sweepTimer != nil {
		n.sweepTimer.Stop()
	}
	n.sweepTimer = n.after(sweepBatchGap, n.drainSweep)
}

func (n *Node) onDatagram(datagram []byte, from *net.UDPAddr) {
	if !n.running {
		return
	}
	n.diag.datagramsIn++
	if from == nil || !IsLocalNetworkAddress(from.IP) {
		n.diag.offNetworkIn++
		return
	}
	var plain Object
	if envelope, ok := parseObject(datagram); ok {
		plain = n.crypto.Decrypt(envelope)
	}
	beacon, ok := ParseBeacon(plain)
	if plain == nil || !ok {
		n.diag.undecryptableIn++
		return
	}
	if beacon.NodeID == n.nodeID {
		n.diag.ownBeaconsIn++
		return
	}
	n.diag.peerBeacon++
	n.diag.lastBeaconMs = nowMs()
	label := beacon.Name
	if label == "" {
		label = left16(beacon.NodeID, 8)
	}
	reply := ""
	if beacon.Reply {
		reply = " (reply)"
	}
	sender := normalIP(from.IP)
	n.diag.lastBeaconFrom = fmt.Sprintf("%s %s:%d%s", label, sender.String(), beacon.Port, reply)
	advert := Object{"sender": beacon.Name, "nodeName": beacon.NodeName, "ownerUser": beacon.OwnerUser,
		"accountKind": beacon.AccountKind, "platform": beacon.Platform, "version": beacon.Version}
	n.rememberPeer(beacon.NodeID, advert, false, false)
	if !n.running || n.udp == nil {
		return
	}
	p := n.peers[beacon.NodeID]
	p.address = sender
	p.port = beacon.Port
	if n.linkForPeer(beacon.NodeID) != nil {
		p.unlinkedSinceMs = 0
		return
	}
	if !beacon.Reply {
		if answer := n.beaconDatagram(true); answer != nil {
			n.writeDiscovery(answer, sender, from.Port, &n.diag.unicastSent)
		}
	}
	now := nowMs()
	if p.unlinkedSinceMs == 0 {
		p.unlinkedSinceMs = now
	}
	if n.nodeID < beacon.NodeID || now-p.unlinkedSinceMs >= n.beaconInterval.Milliseconds()/2 {
		n.connectToPeer(sender, beacon.Port)
	}
}

func (n *Node) AnnounceNow() {
	n.post(n.announceNow)
}

func (n *Node) announceNow() {
	if !n.running {
		return
	}
	n.refreshNetwork()
	n.beaconCount = 0
	n.sendBeacon()
	n.sendUnicastBeacons()
}

func (n *Node) ConnectToPeer(ip net.IP, port int) bool {
	result, _ := n.Call(func() any { return n.connectToPeer(ip, port) }).(bool)
	return result
}

func (n *Node) connectToPeer(ip net.IP, port int) bool {
	if !n.running || port <= 0 || port > 65535 || !IsLocalNetworkAddress(ip) {
		return false
	}
	if len(n.links) >= maxLinks {
		return false
	}
	ip = normalIP(ip)
	key := ip.String() + ":" + strconv.Itoa(port)
	if n.dialing[key] {
		return false
	}
	n.dialing[key] = true
	n.diag.dials++
	generation := n.gen
	go func() {
		dialer := net.Dialer{Timeout: dialTimeout}
		conn, err := dialer.Dial("tcp", net.JoinHostPort(ip.String(), strconv.Itoa(port)))
		posted := n.post(func() {
			delete(n.dialing, key)
			if err != nil {
				n.diag.dialsFailed++
				reason := err.Error()
				var timeout interface{ Timeout() bool }
				if errors.As(err, &timeout) && timeout.Timeout() {
					reason = "timed out"
				}
				n.diag.lastDialError = key + ": " + reason
				return
			}
			if !n.running || n.gen != generation {
				conn.Close()
				return
			}
			n.diag.dialsOK++
			n.attachTCP(conn, true)
		})
		if !posted && conn != nil {
			conn.Close()
		}
	}()
	return true
}

func (n *Node) onNewConnection(listener net.Listener, conn net.Conn) {
	remote, _ := conn.RemoteAddr().(*net.TCPAddr)
	if !n.running || n.listener != listener || len(n.links) >= maxLinks || remote == nil || !IsLocalNetworkAddress(remote.IP) {
		n.diag.inboundRefused++
		conn.Close()
		return
	}
	fromNetwork := !remote.IP.IsLoopback()
	n.diag.inboundAccepted++
	n.attachTCP(conn, false)
	if fromNetwork && !n.inboundSeen {
		n.inboundSeen = true
		if n.hooks.Inbound != nil {
			n.hooks.Inbound()
		}
	}
}

func (n *Node) attachTCP(conn net.Conn, outbound bool) {
	l := &link{conn: conn, outbound: outbound}
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.SetKeepAliveConfig(net.KeepAliveConfig{Enable: true, Idle: keepAliveIdle, Interval: keepAliveInterval, Count: keepAliveProbes})
		setUnackedTimeout(tcp, unackedTimeoutMs)
	}
	l.writer = newLinkWriter(conn, func(err error) {
		n.post(func() { n.dropLink(l, err.Error()) })
	})
	n.links = append(n.links, l)
	go n.readLink(l)
	n.sendKeyFrame(l)
	n.sendHello(l)
}

func (n *Node) readLink(l *link) {
	var buffer []byte
	chunk := make([]byte, readChunk)
	for {
		count, err := l.conn.Read(chunk)
		if count > 0 {
			buffer = append(buffer, chunk[:count]...)
			for {
				payload, status := TakeFrame(&buffer)
				if status == 0 {
					break
				}
				if status < 0 {
					n.post(func() { n.dropLink(l, reasonOversized) })
					return
				}
				if !l.outbound && !l.bluetooth && !l.named.Load() && IsControlHello(payload) {
					rest := append([]byte(nil), buffer...)
					if !n.post(func() { n.handoff(l, payload, rest) }) {
						l.conn.Close()
					}
					return
				}
				n.post(func() {
					if n.hasLink(l) {
						n.handleFrame(l, payload)
					}
				})
			}
		}
		if err != nil {
			reason := err.Error()
			if errors.Is(err, io.EOF) {
				reason = reasonRemoteClosed
			}
			n.post(func() { n.dropLink(l, reason) })
			return
		}
	}
}

func (n *Node) handoff(l *link, hello, rest []byte) {
	if !n.removeLink(l) {
		return
	}
	if !n.advertisesControl() {
		l.writer.close(0)
		return
	}
	control := n.hooks.Control
	go func() {
		l.writer.detach(closeFlush)
		if !control(l.conn, hello, rest) {
			l.conn.Close()
		}
	}()
}

func (n *Node) hasLink(l *link) bool {
	for _, candidate := range n.links {
		if candidate == l {
			return true
		}
	}
	return false
}

func (n *Node) removeLink(l *link) bool {
	for i, candidate := range n.links {
		if candidate == l {
			n.links = append(n.links[:i], n.links[i+1:]...)
			return true
		}
	}
	return false
}

func (n *Node) linkForPeer(peerID string) *link {
	if peerID == "" {
		return nil
	}
	for _, l := range n.links {
		if l.peerID == peerID {
			return l
		}
	}
	return nil
}

func (n *Node) linkForBridge(id int) *link {
	for _, l := range n.links {
		if l.conn == nil && l.bridge == id {
			return l
		}
	}
	return nil
}

func (n *Node) write(l *link, data []byte) bool {
	if l.conn == nil {
		if l.closedRemote || n.hooks.BridgeWrite == nil {
			return false
		}
		n.hooks.BridgeWrite(l.bridge, data)
		return true
	}
	return l.writer.write(data)
}

func (n *Node) shutdownLink(l *link, reason string, flush time.Duration) {
	if l.conn == nil {
		if !l.closedRemote && n.hooks.BridgeClose != nil {
			n.hooks.BridgeClose(l.bridge, reason)
		}
		return
	}
	l.writer.close(flush)
}

func (n *Node) dropLink(l *link, reason string) {
	if !n.removeLink(l) {
		return
	}
	label := "unnamed link"
	if l.peerID != "" {
		label = left16(l.peerID, 8)
	}
	shown := reason
	if shown == "" {
		shown = "closed"
	}
	n.diag.lastDropReason = label + ": " + shown
	settled := l.helloMs > 0 && nowMs()-l.helloMs >= redialMinLinkMs
	n.shutdownLink(l, reason, closeFlush)
	superseded := reason == reasonDuplicate || reason == reasonReplaced || reason == reasonIdentityChanged
	if l.peerID != "" && n.linkForPeer(l.peerID) == nil {
		if p, ok := n.peers[l.peerID]; ok {
			p.linked = false
			n.noteMembershipChange()
			if !superseded && settled && n.running && p.address != nil {
				peerID := l.peerID
				n.after(redialDelay, func() { n.redialPeer(peerID) })
			}
		}
	}
	if n.hooks.LinkDropped != nil {
		n.hooks.LinkDropped(DropInfo{Bridge: l.bridge, Bluetooth: l.bluetooth, Reason: reason, Superseded: superseded,
			Settled: settled && n.running})
	}
}

func (n *Node) redialPeer(peerID string) {
	if !n.running || n.linkForPeer(peerID) != nil {
		return
	}
	p, ok := n.peers[peerID]
	if !ok || p.address == nil || p.port == 0 {
		return
	}
	n.connectToPeer(p.address, p.port)
}

func (n *Node) sweepPeers() {
	now := nowMs()
	changed := false
	blocked := false
	interval := n.beaconInterval.Milliseconds()
	for _, id := range n.peerIDs() {
		p := n.peers[id]
		if n.linkForPeer(id) != nil {
			continue
		}
		if now-p.lastSeenMs > n.peerTimeout.Milliseconds() {
			delete(n.peers, id)
			changed = true
			continue
		}
		if !p.bluetooth && p.unlinkedSinceMs > 0 && now-p.unlinkedSinceMs >= interval*linksBlockedAfterBeats &&
			now-p.lastSeenMs <= interval*2 {
			blocked = true
		}
	}
	if blocked != n.linksBlocked {
		n.linksBlocked = blocked
		if blocked && n.hooks.Blocked != nil {
			n.hooks.Blocked()
		}
	}
	if changed {
		n.noteMembershipChange()
	}
}

func (n *Node) peerIDs() []string {
	ids := make([]string, 0, len(n.peers))
	for id := range n.peers {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	return ids
}

func (n *Node) makeMessage(kind string) Object {
	message := Object{"type": kind, "id": newUUID(), "senderId": n.nodeID, "sender": n.userName,
		"conversation": Conversation, "ts": float64(nowMs())}
	for key, value := range map[string]string{"nodeName": n.nodeName, "ownerUser": n.ownerUser,
		"accountKind": n.accountKind, "platform": n.platform, "version": n.version} {
		if value != "" {
			message[key] = value
		}
	}
	return message
}

func (n *Node) writePlain(l *link, plain Object) bool {
	sealed := n.seal.Seal(plain)
	if sealed == nil {
		return false
	}
	envelope := n.crypto.Encrypt(sealed)
	if envelope == nil {
		return false
	}
	payload := marshal(envelope)
	if len(payload) > MaxFrameBytes {
		return false
	}
	if !n.write(l, FrameBytes(payload)) {
		return false
	}
	if len(n.queued) > 0 {
		id := str(plain["id"])
		if n.queued[id] {
			delete(n.queued, id)
			n.scheduleHistorySave()
			if n.hooks.Queued != nil {
				n.hooks.Queued(id, false)
			}
		}
	}
	return true
}

func (n *Node) sendToAll(plain Object) {
	for _, l := range append([]*link(nil), n.links...) {
		if !l.keyKnown {
			if len(l.pending) < maxPendingFrames {
				l.pending = append(l.pending, plain)
			}
			continue
		}
		n.writePlain(l, plain)
	}
}

func (n *Node) sendKeyFrame(l *link) {
	envelope := n.crypto.Encrypt(n.seal.KeyFrame())
	if envelope == nil {
		return
	}
	n.write(l, FrameBytes(marshal(envelope)))
}

func (n *Node) sendHello(l *link) {
	hello := n.makeMessage("hello")
	if n.listener != nil {
		hello["port"] = n.listenPort
	}
	if n.advertisesControl() {
		hello["control"] = true
	}
	if l.keyKnown {
		n.writePlain(l, hello)
	} else {
		l.pending = append(l.pending, hello)
	}
}

func (n *Node) sendHistoryTo(l *link) {
	for _, entry := range n.history {
		n.writePlain(l, entry)
	}
}

func (n *Node) handleFrame(l *link, payload []byte) {
	n.diag.framesIn++
	var decrypted Object
	if envelope, ok := parseObject(payload); ok {
		decrypted = n.crypto.Decrypt(envelope)
	}
	if len(decrypted) == 0 {
		n.diag.framesUndecryptable++
		return
	}
	if IsKeyFrame(decrypted) {
		if n.seal.AcceptKeyFrame(decrypted) {
			n.sendKeyFrame(l)
		}
		if !l.keyKnown {
			l.keyKnown = true
			pending := l.pending
			l.pending = nil
			for _, plain := range pending {
				n.writePlain(l, plain)
			}
		}
		return
	}
	plain := n.seal.Unseal(decrypted)
	if len(plain) == 0 {
		n.diag.framesUnsealable++
		return
	}
	n.handlePlain(l, plain)
}

func (n *Node) handlePlain(l *link, plain Object) {
	kind := str(plain["type"])
	senderID := left16(str(plain["senderId"]), maxIDChars)
	if senderID == "" || senderID == n.nodeID || str(plain["conversation"]) != Conversation {
		return
	}
	switch {
	case kind == "hello":
		if l.peerID == "" {
			other := n.linkForPeer(senderID)
			now := nowMs()
			l.peerID = senderID
			l.named.Store(true)
			l.helloMs = now
			switch {
			case other != nil && other.bluetooth != l.bluetooth:
				if l.bluetooth {
					n.dropLink(l, reasonDuplicate)
					return
				}
				n.dropLink(other, reasonDuplicate)
			case other != nil && now-other.helloMs >= n.linkReplaceAfter.Milliseconds():
				n.dropLink(other, reasonReplaced)
			case other != nil:
				mine := senderID
				if l.outbound {
					mine = n.nodeID
				}
				theirs := senderID
				if other.outbound {
					theirs = n.nodeID
				}
				if mine < theirs {
					n.dropLink(l, reasonDuplicate)
					return
				}
				n.dropLink(other, reasonDuplicate)
			}
		} else if l.peerID != senderID {
			n.dropLink(l, reasonIdentityChanged)
			return
		}
	case (kind == "typing" || kind == "bye") && l.peerID != senderID:
		return
	}

	n.rememberPeer(senderID, plain, l.peerID == senderID, l.bluetooth)
	n.rememberWake(senderID, plain)

	if kind == "typing" {
		if n.hooks.Typing != nil {
			n.hooks.Typing(senderID, boundedText(plain, "sender", maxDisplayNameChars), boolean(plain["active"]))
		}
		return
	}
	id := left16(str(plain["id"]), maxIDChars)
	if id == "" || !n.markSeen(id) {
		return
	}
	switch kind {
	case "hello":
		n.sendHistoryTo(l)
		return
	case "bye":
		if _, ok := n.peers[senderID]; ok {
			delete(n.peers, senderID)
			n.noteMembershipChange()
		}
		return
	case "chat":
	default:
		return
	}
	n.diag.chatIn++
	n.storeHistory(plain)
	n.emitChat(plain, false)
	for _, other := range append([]*link(nil), n.links...) {
		if other == l || !other.keyKnown {
			continue
		}
		n.writePlain(other, plain)
	}
}

func (n *Node) rememberPeer(peerID string, plain Object, linked, bluetooth bool) {
	if peerID == "" || peerID == n.nodeID {
		return
	}
	p, known := n.peers[peerID]
	if !known {
		p = &peer{}
		n.peers[peerID] = p
	}
	name := boundedText(plain, "sender", maxDisplayNameChars)
	nodeName := boundedText(plain, "nodeName", maxDisplayNameChars)
	ownerUser := boundedText(plain, "ownerUser", maxDisplayNameChars)
	accountKind := boundedText(plain, "accountKind", 16)
	platform := boundedText(plain, "platform", 16)
	version := boundedText(plain, "version", maxVersionChars)
	changed := !known || (name != "" && name != p.name) || (nodeName != "" && nodeName != p.nodeName) ||
		(ownerUser != "" && ownerUser != p.ownerUser) || (linked && !p.linked)
	if name != "" {
		p.name = name
	}
	if nodeName != "" {
		p.nodeName = nodeName
	}
	if ownerUser != "" {
		p.ownerUser = ownerUser
	}
	if accountKind != "" {
		p.accountKind = accountKind
	}
	if platform != "" {
		p.platform = platform
	}
	if version != "" {
		p.version = version
	}
	if linked {
		p.linked = true
		p.bluetooth = bluetooth
		p.unlinkedSinceMs = 0
	}
	p.lastSeenMs = nowMs()
	if changed {
		n.noteMembershipChange()
	}
}

func (n *Node) storeHistory(plain Object) {
	var evicted []string
	n.history, evicted = appendBounded(n.history, plain, &n.historyChars)
	for _, id := range evicted {
		delete(n.queued, id)
	}
	n.scheduleHistorySave()
}

func (n *Node) loadHistory() {
	if n.historyLoaded || n.historyPath == "" {
		return
	}
	n.historyLoaded = true
	file, ok := readHistory(n.historyPath)
	if !ok {
		return
	}
	for _, entry := range file.History {
		id := left16(str(entry["id"]), maxIDChars)
		if id == "" || str(entry["type"]) != "chat" || str(entry["conversation"]) != Conversation || !n.markSeen(id) {
			continue
		}
		n.history, _ = appendBounded(n.history, entry, &n.historyChars)
		n.rememberWake(left16(str(entry["senderId"]), maxIDChars), entry)
		if file.Queued[id] && str(entry["senderId"]) == n.nodeID {
			n.queued[id] = true
		}
		n.emitChat(entry, true)
	}
}

func (n *Node) scheduleHistorySave() {
	if n.historyPath == "" {
		return
	}
	if n.historyTimer != nil {
		n.historyTimer.Stop()
	}
	n.historyTimer = time.AfterFunc(historySaveDelay, func() {
		n.post(func() {
			n.historyTimer = nil
			n.saveHistory()
		})
	})
}

func (n *Node) saveHistory() {
	if n.historyPath == "" {
		return
	}
	queued := map[string]bool{}
	for id := range n.queued {
		queued[id] = true
	}
	_ = writeHistory(n.historyPath, n.history, queued)
}

func (n *Node) sendOwn(message Object) string {
	id := str(message["id"])
	n.markSeen(id)
	n.storeHistory(message)
	n.queued[id] = true
	n.diag.chatOut++
	n.sendToAll(message)
	if wake := n.wakeTargets(); len(wake) > 0 && n.hooks.Wake != nil {
		n.hooks.Wake(wake)
	}
	n.emitChat(message, false)
	if n.queued[id] && n.hooks.Queued != nil {
		n.hooks.Queued(id, true)
	}
	return id
}

func (n *Node) rememberWake(senderID string, plain Object) {
	if senderID == "" || senderID == n.nodeID {
		return
	}
	token := str(plain["wake"])
	expires := WakeTokenExpiry(token)
	if expires <= nowMs() {
		return
	}
	if known, ok := n.wakeTokens[senderID]; ok && WakeTokenExpiry(known) >= expires {
		return
	}
	n.wakeTokens[senderID] = token
	n.wakeOrder = removeString(n.wakeOrder, senderID)
	n.wakeOrder = append(n.wakeOrder, senderID)
	for len(n.wakeOrder) > maxWakeTokens {
		delete(n.wakeTokens, n.wakeOrder[0])
		n.wakeOrder = n.wakeOrder[1:]
	}
}

func (n *Node) wakeTargets() []string {
	now := nowMs()
	tokens := []string{}
	for i := len(n.wakeOrder) - 1; i >= 0; i-- {
		token := n.wakeTokens[n.wakeOrder[i]]
		if WakeTokenExpiry(token) > now && !containsString(tokens, token) {
			tokens = append(tokens, token)
		}
		if len(tokens) >= maxWakeTokens {
			break
		}
	}
	return tokens
}

func containsString(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func (n *Node) markSeen(id string) bool {
	if n.seen[id] {
		return false
	}
	n.seen[id] = true
	n.seenOrder = append(n.seenOrder, id)
	for len(n.seenOrder) > seenCacheLimit {
		delete(n.seen, n.seenOrder[0])
		n.seenOrder = n.seenOrder[1:]
	}
	return true
}

func (n *Node) chatMessage(plain Object, historical bool) ChatMessage {
	out := ChatMessage{Historical: historical, ID: left16(str(plain["id"]), maxIDChars), Conversation: Conversation,
		SenderID: left16(str(plain["senderId"]), maxIDChars), SenderName: boundedText(plain, "sender", maxDisplayNameChars),
		Text: boundedText(plain, "text", maxTextChars), TimestampMs: int64(num(plain["ts"]))}
	if _, ok := plain["fileName"]; ok {
		out.FileName = SafeFileName(str(plain["fileName"]))
		out.FileMime = boundedText(plain, "fileMime", maxMimeChars)
		out.FileData = boundedBase64(plain, "file")
		if len(out.FileData) == 0 {
			out.FileName = ""
		}
	}
	out.Self = out.SenderID == n.nodeID
	return out
}

func (n *Node) emitChat(plain Object, historical bool) {
	if n.hooks.Message != nil {
		n.hooks.Message(n.chatMessage(plain, historical))
	}
}

func (n *Node) stop() {
	if !n.running && n.listener == nil && n.udp == nil {
		return
	}
	if n.running && len(n.links) > 0 {
		n.sendToAll(n.makeMessage("bye"))
	}
	n.running = false
	if n.historyTimer != nil {
		n.historyTimer.Stop()
		n.historyTimer = nil
		n.saveHistory()
	}
	n.gen++
	if n.sweepTimer != nil {
		n.sweepTimer.Stop()
	}
	n.sweepQueue = nil
	n.sweepDatagram = nil
	if n.typingTimer != nil {
		n.typingTimer.Stop()
	}
	links := n.links
	n.links = nil
	for _, l := range links {
		n.shutdownLink(l, "", byeFlush)
	}
	n.dialing = map[string]bool{}
	for _, p := range n.peers {
		p.linked = false
	}
	if n.udp != nil {
		n.udp.Close()
		n.udp = nil
	}
	if n.listener != nil {
		n.listener.Close()
		n.listener = nil
	}
	n.noteMembershipChange()
}

func (n *Node) Stop() {
	n.Call(func() any {
		n.stop()
		return nil
	})
}

func (n *Node) SendChat(id, text string) string {
	result, _ := n.Call(func() any {
		trimmedText := left16(trimmed(text), maxTextChars)
		if !n.running || trimmedText == "" {
			return ""
		}
		message := n.makeMessage("chat")
		n.useID(message, id)
		message["text"] = trimmedText
		return n.sendOwn(message)
	}).(string)
	return result
}

func (n *Node) SendFile(id, fileName, mime string, data []byte) string {
	result, _ := n.Call(func() any {
		if !n.running || len(data) == 0 || len(data) > maxFileBytes {
			return ""
		}
		message := n.makeMessage("chat")
		n.useID(message, id)
		message["fileName"] = SafeFileName(fileName)
		message["fileMime"] = left16(mime, maxMimeChars)
		message["file"] = stdBase64(data)
		return n.sendOwn(message)
	}).(string)
	return result
}

func (n *Node) useID(message Object, id string) {
	id = trimmed(id)
	if id != "" && len16(id) <= maxIDChars && !n.seen[id] {
		message["id"] = id
	}
}

func (n *Node) SendTyping(active bool) {
	n.post(func() { n.sendTyping(active) })
}

func (n *Node) sendTyping(active bool) {
	if !n.running || (!active && !n.typingActive) {
		return
	}
	n.typingActive = active
	message := n.makeMessage("typing")
	message["active"] = active
	n.sendToAll(message)
	if n.typingTimer != nil {
		n.typingTimer.Stop()
		n.typingTimer = nil
	}
	if active {
		n.typingTimer = n.after(typingStop, func() { n.sendTyping(false) })
	}
}

func (n *Node) SetIdentity(userName, nodeName, ownerUser, accountKind string) {
	n.post(func() {
		if name := left16(trimmed(userName), maxDisplayNameChars); name != "" && name != n.userName {
			n.userName = name
			if n.running {
				n.sendBeacon()
				n.noteMembershipChange()
			}
		}
		node := left16(trimmed(nodeName), maxDisplayNameChars)
		owner := left16(trimmed(ownerUser), maxDisplayNameChars)
		if node != n.nodeName || owner != n.ownerUser {
			if node != "" {
				n.nodeName = node
			}
			if owner != "" {
				n.ownerUser = owner
			}
			if n.running {
				n.sendBeacon()
				n.noteMembershipChange()
			}
		}
		if kind := left16(trimmed(accountKind), 16); kind != "" && kind != n.accountKind {
			n.accountKind = kind
			if n.running {
				n.sendBeacon()
			}
		}
	})
}

func (n *Node) SetControl(enabled bool) {
	n.post(func() { n.control = enabled })
}

func (n *Node) SetBluetooth(info Bluetooth) {
	n.post(func() { n.ble = info })
}

func (n *Node) BridgeOpen(id int, outbound bool) bool {
	result, _ := n.Call(func() any {
		if !n.running || len(n.links) >= maxLinks || n.linkForBridge(id) != nil {
			return false
		}
		l := &link{bridge: id, outbound: outbound, bluetooth: true}
		n.links = append(n.links, l)
		n.sendKeyFrame(l)
		n.sendHello(l)
		return true
	}).(bool)
	return result
}

func (n *Node) BridgeData(id int, data []byte) {
	n.post(func() {
		l := n.linkForBridge(id)
		if l == nil {
			return
		}
		l.buffer = append(l.buffer, data...)
		for {
			payload, status := TakeFrame(&l.buffer)
			if status == 0 {
				return
			}
			if status < 0 {
				n.dropLink(l, reasonOversized)
				return
			}
			n.handleFrame(l, payload)
			if !n.hasLink(l) {
				return
			}
		}
	})
}

func (n *Node) BridgeClosed(id int, reason string) {
	n.post(func() {
		if l := n.linkForBridge(id); l != nil {
			l.closedRemote = true
			n.dropLink(l, reason)
		}
	})
}

func (n *Node) DetachBridges() {
	n.post(func() {
		for _, l := range append([]*link(nil), n.links...) {
			if l.conn == nil {
				l.closedRemote = true
				n.dropLink(l, reasonDesktopDetached)
			}
		}
	})
}

type State struct {
	Running      bool     `json:"running"`
	NodeID       string   `json:"nodeId"`
	SealKeyID    string   `json:"sealKeyId"`
	ListenPort   int      `json:"listenPort"`
	PeerCount    int      `json:"peerCount"`
	LinksBlocked bool     `json:"linksBlocked"`
	Queued       []string `json:"queued"`
	WakeTargets  []string `json:"wakeTargets"`
	Members      []Member `json:"members"`
}

func (n *Node) state() State {
	queued := make([]string, 0, len(n.queued))
	for id := range n.queued {
		queued = append(queued, id)
	}
	sort.Strings(queued)
	port := 0
	if n.listener != nil {
		port = n.listenPort
	}
	return State{Running: n.running, NodeID: n.nodeID, SealKeyID: n.seal.KeyID(), ListenPort: port,
		PeerCount: n.peerCount(), LinksBlocked: n.linksBlocked, Queued: queued, WakeTargets: n.wakeTargets(),
		Members: n.members()}
}

func (n *Node) StateSnapshot() State {
	result, _ := n.Call(func() any { return n.state() }).(State)
	return result
}

func (n *Node) members() []Member {
	now := nowMs()
	roster := []Member{{ID: n.nodeID, Name: n.userName, NodeName: n.nodeName, OwnerUser: n.ownerUser,
		AccountKind: n.accountKind, Platform: n.platform, Version: n.version, Self: true, Online: n.running}}
	for _, id := range n.peerIDs() {
		p := n.peers[id]
		name := p.name
		if name == "" {
			name = id
		}
		note := "On this network"
		if p.bluetooth {
			note = "Over Bluetooth"
		}
		roster = append(roster, Member{ID: id, Name: name, NodeName: p.nodeName, OwnerUser: p.ownerUser,
			AccountKind: p.accountKind, Platform: p.platform, Version: p.version, Note: note,
			Online: p.linked || now-p.lastSeenMs <= n.peerTimeout.Milliseconds()})
	}
	return roster
}

func (n *Node) peerCount() int {
	linked := 0
	for _, p := range n.peers {
		if p.linked {
			linked++
		}
	}
	return linked
}

func (n *Node) History() []ChatMessage {
	result, _ := n.Call(func() any {
		out := make([]ChatMessage, 0, len(n.history))
		for _, entry := range n.history {
			out = append(out, n.chatMessage(entry, true))
		}
		return out
	}).([]ChatMessage)
	return result
}

func (n *Node) Matches(nodeID, historyFile, sealKeyID string) bool {
	result, _ := n.Call(func() any {
		return n.nodeID == left16(trimmed(nodeID), maxIDChars) && n.historyPath == historyFile && n.seal.KeyID() == sealKeyID
	}).(bool)
	return result
}

func (n *Node) OpenNote() string {
	result, _ := n.Call(func() any { return n.lastOpenNote }).(string)
	return result
}

func (n *Node) Running() bool {
	result, _ := n.Call(func() any { return n.running }).(bool)
	return result
}

func (n *Node) Diagnostics() (string, string) {
	result, _ := n.Call(func() any { return [2]string{n.diagnosticsLine(), n.diagnosticsDetail()} }).([2]string)
	return result[0], result[1]
}
