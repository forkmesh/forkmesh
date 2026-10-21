package fmmmobile

import (
	"context"
	"net"
	"os"
	"syscall"

	"forkmesh.com/mirror-node/rtc/quic"
)

func (c *Call) AddPathFd(fd int, backup bool) (int64, error) {
	dup, err := syscall.Dup(fd)
	if err != nil {
		return 0, err
	}
	f := os.NewFile(uintptr(dup), "fmm-path")
	pc, err := net.FilePacketConn(f)
	f.Close()
	if err != nil {
		return 0, err
	}
	return c.openPath(pc, 0, backup)
}

func (c *Call) AddPathInterface(index int, backup bool) (int64, error) {
	c.mu.Lock()
	family := c.family
	c.mu.Unlock()
	if family == "" {
		return 0, ErrNotLive
	}
	pc, err := listenOn(family, index)
	if err != nil {
		return 0, err
	}
	return c.openPath(pc, index, backup)
}

func (c *Call) openPath(pc net.PacketConn, index int, backup bool) (int64, error) {
	s, err := c.session()
	if err != nil {
		pc.Close()
		return 0, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), pathTimeout)
	defer cancel()
	p, err := s.Conn.OpenPath(ctx, pc, quic.PathOptions{Backup: backup})
	if err != nil {
		pc.Close()
		return 0, err
	}
	c.mu.Lock()
	c.paths[int64(p.ID())] = pathInfo{path: p, index: index}
	if index > 0 {
		known := false
		for _, w := range c.wantPaths {
			known = known || w.index == index
		}
		if !known {
			c.wantPaths = append(c.wantPaths, pathInfo{index: index})
		}
	}
	c.mu.Unlock()
	c.emit(&Event{Kind: EventPath, Code: 1, FrameID: int64(p.ID()), Layers: index})
	return int64(p.ID()), nil
}

func (c *Call) AbandonPath(id int64) error {
	c.mu.Lock()
	p, ok := c.paths[id]
	delete(c.paths, id)
	if ok && p.index > 0 {
		keep := c.wantPaths[:0]
		for _, w := range c.wantPaths {
			if w.index != p.index {
				keep = append(keep, w)
			}
		}
		c.wantPaths = keep
	}
	c.mu.Unlock()
	if !ok {
		return ErrNoPath
	}
	return p.path.Abandon()
}

func (c *Call) SetPathBackup(id int64, backup bool) error {
	c.mu.Lock()
	p, ok := c.paths[id]
	c.mu.Unlock()
	if !ok {
		return ErrNoPath
	}
	return p.path.SetBackup(backup)
}

func (c *Call) PathCount() int {
	s, err := c.session()
	if err != nil {
		return 0
	}
	return len(s.Conn.Paths())
}

func (c *Call) UsablePaths() int {
	s, err := c.session()
	if err != nil {
		return 0
	}
	return s.Conn.Health().UsablePaths
}

func InterfaceIndex(name string) int {
	ifi, err := net.InterfaceByName(name)
	if err != nil {
		return 0
	}
	return ifi.Index
}
