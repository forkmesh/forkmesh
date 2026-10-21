package quic

import (
	"crypto/tls"
	"time"
)

type Config struct {
	TLSConfig                   *tls.Config
	MaxIdleTimeout              time.Duration
	HandshakeTimeout            time.Duration
	KeepAlivePeriod             time.Duration
	MaxIncomingStreams          int64
	MaxIncomingUniStreams       int64
	StreamReceiveWindow         uint64
	ConnectionReceiveWindow     uint64
	StreamSendBuffer            int
	MaxDatagramFrameSize        int
	MaxPendingDatagrams         int
	MaxPacketSize               int
	ActiveConnectionIDLimit     int
	InitialRTT                  time.Duration
	KeyUpdateInterval           uint64
	RequireAddressValidation    bool
	RetryThreshold              int
	MaxConnections              int
	MaxConnectionsPerIP         int
	AcceptQueue                 int
	StatelessResetKey           []byte
	TokenKey                    []byte
	DisableStatelessReset       bool
	MaxStatelessResetsPerSecond int
	Multipath                   bool
	MaxPaths                    int
	PathKeepAlivePeriod         time.Duration
	DatagramDedupKey            func(datagram []byte) (uint64, bool)
}

const (
	defaultIdleTimeout       = 30 * time.Second
	defaultHandshakeTimeout  = 10 * time.Second
	defaultMaxStreams        = 100
	defaultStreamWindow      = 512 << 10
	defaultConnWindow        = 4 << 20
	defaultStreamSendBuffer  = 1 << 20
	defaultPendingDatagrams  = 128
	defaultMaxPacketSize     = 1200
	maxAllowedPacketSize     = 1452
	defaultActiveCIDLimit    = 4
	maxActiveCIDLimit        = 8
	defaultKeyUpdateInterval = 1 << 22
	defaultRetryThreshold    = 128
	defaultMaxConnections    = 10000
	defaultMaxPerIP          = 64
	defaultAcceptQueue       = 64
	defaultResetsPerSecond   = 200
	maxStreamWindow          = 16 << 20
	maxConnWindow            = 64 << 20
	maxDatagramFrameLimit    = 65535
	defaultMaxPaths          = 4
	maxPathsLimit            = 8
	defaultPathKeepAlive     = time.Second
)

func (c *Config) clone() *Config {
	if c == nil {
		return &Config{}
	}
	n := *c
	return &n
}

func (c *Config) withDefaults() *Config {
	n := c.clone()
	if n.MaxIdleTimeout <= 0 {
		n.MaxIdleTimeout = defaultIdleTimeout
	}
	if n.HandshakeTimeout <= 0 {
		n.HandshakeTimeout = defaultHandshakeTimeout
	}
	if n.MaxIncomingStreams == 0 {
		n.MaxIncomingStreams = defaultMaxStreams
	}
	if n.MaxIncomingStreams < 0 {
		n.MaxIncomingStreams = 0
	}
	if n.MaxIncomingStreams > 1<<16 {
		n.MaxIncomingStreams = 1 << 16
	}
	if n.MaxIncomingUniStreams == 0 {
		n.MaxIncomingUniStreams = defaultMaxStreams
	}
	if n.MaxIncomingUniStreams < 0 {
		n.MaxIncomingUniStreams = 0
	}
	if n.MaxIncomingUniStreams > 1<<16 {
		n.MaxIncomingUniStreams = 1 << 16
	}
	if n.StreamReceiveWindow == 0 {
		n.StreamReceiveWindow = defaultStreamWindow
	}
	if n.StreamReceiveWindow > maxStreamWindow {
		n.StreamReceiveWindow = maxStreamWindow
	}
	if n.ConnectionReceiveWindow == 0 {
		n.ConnectionReceiveWindow = defaultConnWindow
	}
	if n.ConnectionReceiveWindow > maxConnWindow {
		n.ConnectionReceiveWindow = maxConnWindow
	}
	if n.StreamSendBuffer <= 0 {
		n.StreamSendBuffer = defaultStreamSendBuffer
	}
	if n.MaxDatagramFrameSize < 0 {
		n.MaxDatagramFrameSize = 0
	}
	if n.MaxDatagramFrameSize > maxDatagramFrameLimit {
		n.MaxDatagramFrameSize = maxDatagramFrameLimit
	}
	if n.MaxPendingDatagrams <= 0 {
		n.MaxPendingDatagrams = defaultPendingDatagrams
	}
	if n.MaxPacketSize <= 0 {
		n.MaxPacketSize = defaultMaxPacketSize
	}
	if n.MaxPacketSize < minInitialSize {
		n.MaxPacketSize = minInitialSize
	}
	if n.MaxPacketSize > maxAllowedPacketSize {
		n.MaxPacketSize = maxAllowedPacketSize
	}
	if n.ActiveConnectionIDLimit < 2 {
		n.ActiveConnectionIDLimit = defaultActiveCIDLimit
	}
	if n.ActiveConnectionIDLimit > maxActiveCIDLimit {
		n.ActiveConnectionIDLimit = maxActiveCIDLimit
	}
	if n.InitialRTT <= 0 {
		n.InitialRTT = defaultInitialRTT
	}
	if n.KeyUpdateInterval == 0 {
		n.KeyUpdateInterval = defaultKeyUpdateInterval
	}
	if n.RetryThreshold <= 0 {
		n.RetryThreshold = defaultRetryThreshold
	}
	if n.MaxConnections <= 0 {
		n.MaxConnections = defaultMaxConnections
	}
	if n.MaxConnectionsPerIP <= 0 {
		n.MaxConnectionsPerIP = defaultMaxPerIP
	}
	if n.AcceptQueue <= 0 {
		n.AcceptQueue = defaultAcceptQueue
	}
	if n.MaxPaths <= 0 {
		n.MaxPaths = defaultMaxPaths
	}
	if n.MaxPaths > maxPathsLimit {
		n.MaxPaths = maxPathsLimit
	}
	if n.PathKeepAlivePeriod == 0 {
		n.PathKeepAlivePeriod = defaultPathKeepAlive
	}
	if n.MaxStatelessResetsPerSecond <= 0 {
		n.MaxStatelessResetsPerSecond = defaultResetsPerSecond
	}
	return n
}
