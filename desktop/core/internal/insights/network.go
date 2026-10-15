package insights

import (
	"context"
	"net/netip"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/rpc"
)

type NetRoot struct {
	Session int    `json:"session"`
	Pid     int64  `json:"pid"`
	Label   string `json:"label"`
}

type NetSocket struct {
	Inode    uint64
	Peer     string
	Port     uint16
	Sent     uint64
	Received uint64
}

type NetSample struct {
	Sockets []NetSocket
	Owner   map[uint64]int
}

type peerTraffic struct {
	session  int
	peer     string
	port     uint16
	sent     int64
	received int64
}

func isLoopbackPeer(peer string) bool {
	address, err := netip.ParseAddr(peer)
	if err != nil {
		return false
	}
	return address.Unmap().IsLoopback()
}

func networkRows(traffic []peerTraffic, labels map[int]string, maxPeers int) []any {
	grouped := map[int]map[string]*peerTraffic{}
	for _, item := range traffic {
		if item.session == 0 || item.peer == "" || isLoopbackPeer(item.peer) {
			continue
		}
		bySession := grouped[item.session]
		if bySession == nil {
			bySession = map[string]*peerTraffic{}
			grouped[item.session] = bySession
		}
		slot := bySession[item.peer]
		if slot == nil {
			slot = &peerTraffic{session: item.session, peer: item.peer}
			bySession[item.peer] = slot
		}
		if slot.port == 0 {
			slot.port = item.port
		}
		slot.sent += item.sent
		slot.received += item.received
	}
	sessions := make([]int, 0, len(grouped))
	for session := range grouped {
		sessions = append(sessions, session)
	}
	sort.Ints(sessions)
	rows := []any{}
	for _, session := range sessions {
		peerNames := make([]string, 0, len(grouped[session]))
		for peer := range grouped[session] {
			peerNames = append(peerNames, peer)
		}
		sort.Strings(peerNames)
		peers := make([]*peerTraffic, 0, len(peerNames))
		for _, name := range peerNames {
			peers = append(peers, grouped[session][name])
		}
		sort.SliceStable(peers, func(i, j int) bool {
			return peers[i].sent+peers[i].received > peers[j].sent+peers[j].received
		})
		hosts := []any{}
		for i, peer := range peers {
			if i >= maxPeers {
				break
			}
			hosts = append(hosts, map[string]any{"peer": peer.peer, "name": "", "port": int(peer.port),
				"sent": float64(peer.sent), "received": float64(peer.received)})
		}
		rows = append(rows, map[string]any{"session": session, "label": left16(labels[session], 160),
			"hosts": hosts, "hostCount": len(peers)})
	}
	return rows
}

func processChildren() map[int64][]int64 {
	children := map[int64][]int64{}
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return children
	}
	for _, entry := range entries {
		pid, err := strconv.ParseInt(entry.Name(), 10, 64)
		if err != nil {
			continue
		}
		data, err := os.ReadFile(filepath.Join("/proc", entry.Name(), "stat"))
		if err != nil {
			continue
		}
		text := string(data)
		close := strings.LastIndexByte(text, ')')
		if close < 0 {
			continue
		}
		fields := strings.Fields(text[close+1:])
		if len(fields) < 2 {
			continue
		}
		parent, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			continue
		}
		children[parent] = append(children[parent], pid)
	}
	return children
}

func descendants(root int64, children map[int64][]int64) []int64 {
	out := []int64{}
	queue := []int64{root}
	seen := map[int64]bool{root: true}
	for len(queue) > 0 {
		pid := queue[0]
		queue = queue[1:]
		for _, child := range children[pid] {
			if seen[child] {
				continue
			}
			seen[child] = true
			out = append(out, child)
			queue = append(queue, child)
		}
	}
	return out
}

func socketInodes(pid int64) []uint64 {
	dir := filepath.Join("/proc", strconv.FormatInt(pid, 10), "fd")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	out := []uint64{}
	for _, entry := range entries {
		link, err := os.Readlink(filepath.Join(dir, entry.Name()))
		if err != nil || len(link) <= 9 || !strings.HasPrefix(link, "socket:[") {
			continue
		}
		inode, err := strconv.ParseUint(link[8:len(link)-1], 10, 64)
		if err == nil {
			out = append(out, inode)
		}
	}
	return out
}

func SampleNetwork(roots []NetRoot) NetSample {
	sample := NetSample{Owner: map[uint64]int{}}
	var children map[int64][]int64
	for _, root := range roots {
		if root.Pid <= 0 {
			continue
		}
		if children == nil {
			children = processChildren()
		}
		pids := append([]int64{root.Pid}, descendants(root.Pid, children)...)
		for _, pid := range pids {
			for _, inode := range socketInodes(pid) {
				sample.Owner[inode] = root.Session
			}
		}
	}
	if len(sample.Owner) > 0 {
		sample.Sockets = tcpSockets()
	}
	return sample
}

func (m *Module) networkRoots(ctx context.Context) []NetRoot {
	var reply struct {
		Roots []NetRoot `json:"roots"`
	}
	if err := m.callDesktop(ctx, "ui.insights.networkRoots", map[string]any{}, &reply); err == nil {
		return reply.Roots
	}
	return m.coreNetworkRoots()
}

func (m *Module) coreNetworkRoots() []NetRoot {
	if m.core == nil {
		return nil
	}
	procs, ok := m.core.Module("agentproc").(*agentproc.Module)
	if !ok {
		return nil
	}
	var store *agents.Store
	if module, ok := m.core.Module("agents").(*agents.Module); ok {
		store = module.Store()
	}
	sessions := map[int]agents.Session{}
	if store != nil {
		for _, session := range store.LoadAllSessions() {
			sessions[session.ID] = session
		}
	}
	bySession := map[int]NetRoot{}
	for _, process := range procs.Processes() {
		if !process.Running || process.Pid <= 0 {
			continue
		}
		sid := intOf(process.Meta["sessionId"])
		if sid <= 0 {
			continue
		}
		session, known := sessions[sid]
		if !known {
			continue
		}
		title := trimmed(session.IssueTitle)
		if title == "" {
			title = trimmed(strings.SplitN(session.Prompt, "\n", 2)[0])
		}
		if title == "" {
			title = "Agent run #" + strconv.Itoa(sid)
		} else {
			title = left16(title, 160)
		}
		bySession[sid] = NetRoot{Session: sid, Pid: int64(process.Pid), Label: title}
	}
	ids := make([]int, 0, len(bySession))
	for id := range bySession {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	roots := make([]NetRoot, 0, len(ids))
	for _, id := range ids {
		roots = append(roots, bySession[id])
	}
	return roots
}

func (m *Module) handleNetwork(ctx context.Context, req *rpc.Request) (any, error) {
	roots := m.networkRoots(ctx)
	if len(roots) == 0 {
		return map[string]any{"agents": []any{}}, nil
	}
	labels := map[int]string{}
	for _, root := range roots {
		labels[root.Session] = root.Label
	}
	sampler := m.Sample
	if sampler == nil {
		sampler = SampleNetwork
	}
	sample := sampler(roots)
	traffic := []peerTraffic{}
	for _, socket := range sample.Sockets {
		session := sample.Owner[socket.Inode]
		if session == 0 {
			continue
		}
		traffic = append(traffic, peerTraffic{session: session, peer: socket.Peer, port: socket.Port,
			sent: int64(socket.Sent), received: int64(socket.Received)})
	}
	return map[string]any{"agents": networkRows(traffic, labels, maxPeersPerAgent),
		"sampledAtMs": float64(time.Now().UnixMilli())}, nil
}
