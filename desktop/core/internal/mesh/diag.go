package mesh

import (
	"fmt"
	"strings"
)

func ageText(now, then int64) string {
	if then <= 0 {
		return "never"
	}
	seconds := (now - then) / 1000
	if seconds < 0 {
		seconds = 0
	}
	switch {
	case seconds < 120:
		return fmt.Sprintf("%ds ago", seconds)
	case seconds < 7200:
		return fmt.Sprintf("%dm ago", seconds/60)
	}
	return fmt.Sprintf("%dh ago", seconds/3600)
}

func (n *Node) diagnosticsLine() string {
	if !n.running {
		return "mesh stopped"
	}
	now := nowMs()
	d := n.diag
	keyed := 0
	for _, l := range n.links {
		if l.keyKnown {
			keyed++
		}
	}
	parts := []string{}
	udp := "CLOSED"
	if n.udp != nil {
		udp = "bound"
	}
	parts = append(parts, fmt.Sprintf("udp %d %s", n.discoveryPort, udp))
	tcp := "CLOSED"
	if n.listener != nil {
		tcp = fmt.Sprint(n.listenPort)
	}
	parts = append(parts, "tcp "+tcp)
	parts = append(parts, interfaceText(n.interfaces()))
	errorsText := ""
	if d.sendErrors > 0 {
		errorsText = fmt.Sprintf(" ERR %d", d.sendErrors)
	}
	parts = append(parts, fmt.Sprintf("tx mc %d bc %d uc %d sweep %d%s", d.multicastSent, d.broadcastSent, d.unicastSent, d.sweepSent, errorsText))
	parts = append(parts, fmt.Sprintf("rx %d: own %d peer %d bad %d off-net %d", d.datagramsIn, d.ownBeaconsIn, d.peerBeacon, d.undecryptableIn, d.offNetworkIn))
	if d.lastBeaconMs != 0 {
		parts = append(parts, fmt.Sprintf("heard %s %s", d.lastBeaconFrom, ageText(now, d.lastBeaconMs)))
	} else {
		parts = append(parts, "heard nobody")
	}
	parts = append(parts, fmt.Sprintf("dial %d ok %d fail %d · in %d", d.dials, d.dialsOK, d.dialsFailed, d.inboundAccepted))
	parts = append(parts, fmt.Sprintf("links %d keyed %d · peers %d", len(n.links), keyed, len(n.peers)))
	parts = append(parts, fmt.Sprintf("frames %d bad %d · chat in %d out %d queued %d", d.framesIn, d.framesUndecryptable+d.framesUnsealable, d.chatIn, d.chatOut, len(n.queued)))
	if n.ble.Active {
		parts = append(parts, "bt on")
	} else {
		parts = append(parts, "bt off")
	}
	if n.linksBlocked {
		parts = append(parts, "LINKS BLOCKED")
	}
	return strings.Join(parts, " · ")
}

func (n *Node) diagnosticsDetail() string {
	now := nowMs()
	d := n.diag
	lines := []string{fmt.Sprintf("node %s · %s %s · up %s", left16(n.nodeID, 12), n.platform, n.version, ageText(now, n.startedMs))}
	lines = append(lines, "interfaces: "+interfaceText(n.interfaces()))
	if n.udp != nil {
		lines = append(lines, fmt.Sprintf("udp %d: bound", n.discoveryPort))
	} else {
		lines = append(lines, fmt.Sprintf("udp %d: closed %s", n.discoveryPort, d.discoveryError))
	}
	if n.listener != nil {
		lines = append(lines, fmt.Sprintf("tcp: listening on %d", n.listenPort))
	} else {
		lines = append(lines, "tcp: closed "+d.listenError)
	}
	if d.lastSendError != "" {
		lines = append(lines, "last send error: "+d.lastSendError)
	}
	if d.lastDialError != "" {
		lines = append(lines, "last dial error: "+d.lastDialError)
	}
	if d.lastDropReason != "" {
		lines = append(lines, "last link drop: "+d.lastDropReason)
	}
	if d.inboundRefused > 0 {
		lines = append(lines, fmt.Sprintf("inbound refused: %d", d.inboundRefused))
	}
	lines = append(lines, fmt.Sprintf("frames undecryptable %d · unsealable %d", d.framesUndecryptable, d.framesUnsealable))
	lines = append(lines, fmt.Sprintf("sweep queue %d", len(n.sweepQueue)))
	if n.ble.Active {
		last := ""
		if n.ble.LastError != "" {
			last = " · last " + n.ble.LastError
		}
		lines = append(lines, fmt.Sprintf("ble: seen %d · dial %d · open %d%s", n.ble.Seen, n.ble.Dials, n.ble.Opened, last))
	}
	if len(n.peers) == 0 {
		lines = append(lines, "peers: none")
	}
	for _, id := range n.peerIDs() {
		p := n.peers[id]
		state := "unlinked"
		switch {
		case p.linked && p.bluetooth:
			state = "linked (bt)"
		case p.linked:
			state = "linked"
		case p.unlinkedSinceMs > 0:
			state = "unlinked since " + ageText(now, p.unlinkedSinceMs)
		}
		name := p.name
		if name == "" {
			name = "?"
		}
		address := ""
		if p.address != nil {
			address = p.address.String()
		}
		lines = append(lines, fmt.Sprintf("peer %s (%s) %s:%d %s %s · %s · seen %s", name, left16(id, 8), address, p.port,
			p.platform, p.version, state, ageText(now, p.lastSeenMs)))
	}
	return strings.Join(lines, "\n")
}
