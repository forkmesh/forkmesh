//go:build linux

package insights

import (
	"encoding/binary"
	"net/netip"
	"strings"
	"syscall"
)

const (
	netlinkSockDiag  = 4
	sockDiagByFamily = 20
	tcpListenState   = 10
	inetDiagInfo     = 2
	inetDiagMsgSize  = 72
	tcpiBytesAcked   = 120
	tcpiBytesRecv    = 128
	tcpiCopyMax      = 136
)

func diagRequest(family uint8) []byte {
	request := make([]byte, 16+56)
	binary.NativeEndian.PutUint32(request[0:], uint32(len(request)))
	binary.NativeEndian.PutUint16(request[4:], sockDiagByFamily)
	binary.NativeEndian.PutUint16(request[6:], syscall.NLM_F_REQUEST|syscall.NLM_F_DUMP)
	body := request[16:]
	body[0] = family
	body[1] = syscall.IPPROTO_TCP
	body[2] = 1 << (inetDiagInfo - 1)
	binary.NativeEndian.PutUint32(body[4:], ^uint32(0)&^(1<<tcpListenState))
	return request
}

func dumpFamily(fd int, family uint8, out *[]NetSocket) {
	if err := syscall.Sendto(fd, diagRequest(family), 0, &syscall.SockaddrNetlink{Family: syscall.AF_NETLINK}); err != nil {
		return
	}
	buffer := make([]byte, 65536)
	for {
		got, _, err := syscall.Recvfrom(fd, buffer, 0)
		if err != nil || got <= 0 {
			return
		}
		messages, err := syscall.ParseNetlinkMessage(buffer[:got])
		if err != nil {
			return
		}
		for _, message := range messages {
			if message.Header.Type == syscall.NLMSG_DONE || message.Header.Type == syscall.NLMSG_ERROR {
				return
			}
			data := message.Data
			if len(data) < inetDiagMsgSize {
				continue
			}
			inode := binary.NativeEndian.Uint32(data[68:])
			if inode == 0 {
				continue
			}
			socket := NetSocket{Inode: uint64(inode), Port: binary.BigEndian.Uint16(data[6:])}
			dst := data[24:40]
			if data[0] == syscall.AF_INET {
				socket.Peer = netip.AddrFrom4([4]byte(dst[:4])).String()
			} else {
				socket.Peer = netip.AddrFrom16([16]byte(dst)).String()
			}
			socket.Peer = strings.TrimPrefix(socket.Peer, "::ffff:")
			attributes := data[inetDiagMsgSize:]
			for len(attributes) >= 4 {
				length := int(binary.NativeEndian.Uint16(attributes[0:]))
				kind := binary.NativeEndian.Uint16(attributes[2:])
				if length < 4 || length > len(attributes) {
					break
				}
				if kind == inetDiagInfo {
					info := make([]byte, tcpiCopyMax)
					copy(info, attributes[4:length])
					socket.Sent = binary.NativeEndian.Uint64(info[tcpiBytesAcked:])
					socket.Received = binary.NativeEndian.Uint64(info[tcpiBytesRecv:])
				}
				aligned := (length + 3) &^ 3
				if aligned > len(attributes) {
					break
				}
				attributes = attributes[aligned:]
			}
			*out = append(*out, socket)
		}
	}
}

func tcpSockets() []NetSocket {
	sockets := []NetSocket{}
	fd, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, netlinkSockDiag)
	if err != nil {
		return sockets
	}
	defer syscall.Close(fd)
	dumpFamily(fd, syscall.AF_INET, &sockets)
	dumpFamily(fd, syscall.AF_INET6, &sockets)
	return sockets
}
