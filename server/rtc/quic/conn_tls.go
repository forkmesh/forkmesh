package quic

import (
	"bytes"
	"crypto/tls"
	"errors"
	"time"
)

type appKeys struct {
	readPhase        uint64
	writePhase       uint64
	readCur          *directionKeys
	readPrev         *directionKeys
	readNext         *directionKeys
	write            *directionKeys
	firstRecvInPhase int64
	prevUntil        time.Time
	ackedInPhase     bool
	sentInPhase      uint64
	authFailures     uint64
}

const (
	aeadIntegrityLimit   = 1 << 52
	chachaIntegrityLimit = 1 << 36
)

func levelSpace(l tls.QUICEncryptionLevel) int {
	switch l {
	case tls.QUICEncryptionLevelInitial:
		return spaceInitial
	case tls.QUICEncryptionLevelHandshake:
		return spaceHandshake
	case tls.QUICEncryptionLevelApplication:
		return spaceApp
	}
	return -1
}

func spaceLevel(s int) tls.QUICEncryptionLevel {
	switch s {
	case spaceInitial:
		return tls.QUICEncryptionLevelInitial
	case spaceHandshake:
		return tls.QUICEncryptionLevelHandshake
	}
	return tls.QUICEncryptionLevelApplication
}

func cryptoError(err error) *TransportError {
	var alert tls.AlertError
	code := TransportErrorCode(0x100 + 80)
	if errors.As(err, &alert) {
		code = CryptoErrorBase + TransportErrorCode(uint8(alert))
	}
	return &TransportError{Code: code, Reason: "tls: " + err.Error()}
}

func (c *Conn) processTLSEvents() error {
	for {
		ev := c.tls.NextEvent()
		switch ev.Kind {
		case tls.QUICNoEvent:
			return nil
		case tls.QUICSetReadSecret, tls.QUICSetWriteSecret:
			suite := suiteByID(ev.Suite)
			if suite == nil {
				return &TransportError{Code: CryptoErrorBase + 40, Reason: ErrUnsupportedCipher.Error()}
			}
			s := levelSpace(ev.Level)
			if s < 0 {
				continue
			}
			keys := newDirectionKeys(suite, ev.Data)
			sp := c.spaces[s]
			if ev.Kind == tls.QUICSetReadSecret {
				sp.open = keys
				if s == spaceApp {
					c.keys.readCur = keys
					c.keys.readNext = keys.nextGeneration()
				}
			} else {
				sp.seal = keys
				if s == spaceApp {
					c.keys.write = keys
				}
			}
		case tls.QUICWriteData:
			s := levelSpace(ev.Level)
			if s < 0 {
				continue
			}
			sp := c.spaces[s]
			if sp.discarded {
				continue
			}
			sp.crypto.write(ev.Data)
		case tls.QUICTransportParameters:
			if err := c.onPeerParams(ev.Data); err != nil {
				return err
			}
		case tls.QUICTransportParametersRequired:
			c.tls.SetTransportParameters(c.localParams.marshal(!c.isClient))
		case tls.QUICHandshakeDone:
			c.onHandshakeComplete()
		case tls.QUICRejectedEarlyData, tls.QUICResumeSession, tls.QUICStoreSession:
		default:
			return &TransportError{Code: CryptoErrorBase + 80, Reason: "tls handshake failed"}
		}
	}
}

func (c *Conn) onPeerParams(b []byte) *TransportError {
	p, err := parseTransportParams(b, c.isClient)
	if err != nil {
		return err
	}
	if c.isClient {
		if !bytes.Equal(p.originalDCID, c.odcid) {
			return transportErr(TransportParameterError, "original_destination_connection_id mismatch")
		}
		if !bytes.Equal(p.initialSCID, c.peerInitialSCID) {
			return transportErr(TransportParameterError, "initial_source_connection_id mismatch")
		}
		if c.retried != p.hasRetrySCID || (c.retried && !bytes.Equal(p.retrySCID, c.retrySCID)) {
			return transportErr(TransportParameterError, "retry_source_connection_id mismatch")
		}
	} else {
		if !bytes.Equal(p.initialSCID, c.peerInitialSCID) {
			return transportErr(TransportParameterError, "initial_source_connection_id mismatch")
		}
	}
	if p.hasMaxPathID && len(c.peerInitialSCID) == 0 {
		return transportErr(ProtocolViolation, "initial_max_path_id with a zero-length connection id")
	}
	c.peerParams = p
	c.havePeerParams = true
	if p.hasMaxPathID && c.config.Multipath {
		c.enableMultipath(p.maxPathID)
	}
	if p.maxIdleTimeout > 0 && p.maxIdleTimeout < c.idleTimeout {
		c.idleTimeout = p.maxIdleTimeout
	}
	c.sendMaxData = p.initialMaxData
	c.peerMaxBidi = p.initialMaxStreamsBidi
	c.peerMaxUni = p.initialMaxStreamsUni
	c.peerMaxDatagram = p.maxDatagramFrameSize
	size := c.config.MaxPacketSize
	if p.maxUDPPayloadSize < uint64(size) {
		size = int(p.maxUDPPayloadSize)
	}
	c.peerMaxPacket = size
	if c.isClient && len(p.statelessResetToken) == 16 && len(c.peerCIDs) > 0 {
		pc := c.peerCIDs[0]
		copy(pc.token[:], p.statelessResetToken)
		pc.hasToken = true
		c.ep.addResetToken(pc.token, c)
	}
	for _, s := range c.streams {
		c.applyInitialStreamLimits(s)
	}
	return nil
}

func (c *Conn) onHandshakeComplete() {
	if c.handshakeDone {
		return
	}
	c.handshakeDone = true
	if c.state == stateHandshake {
		c.state = stateActive
	}
	now := time.Now()
	c.idleStart = now
	if !c.isClient {
		c.confirmed = true
		c.maxSendSize = c.peerMaxPacket
		c.handshakeDoneDue = true
		c.active.validated = true
		c.discardSpace(spaceHandshake)
		c.ep.handshakeFinished(c)
	}
	c.issueConnectionIDs()
	c.issuePathCIDs()
	c.completeHandshake()
	if !c.isClient && !c.ep.enqueueAccept(c) {
		c.closeTransport(transportErr(ConnectionRefused, "accept queue full"))
	}
}

func (c *Conn) onConfirmed() {
	if c.confirmed {
		return
	}
	c.confirmed = true
	c.maxSendSize = c.peerMaxPacket
	c.peerValidatedUs = true
	c.discardSpace(spaceHandshake)
	c.mpNotify.broadcast()
}

func (c *Conn) discardSpace(s int) {
	sp := c.spaces[s]
	if sp.discarded {
		return
	}
	sp.discard(&c.cc)
	c.ptoCount = 0
	c.setLossTimer(time.Now())
}

func (c *Conn) feedCrypto(s int, f *frame) *TransportError {
	sp := c.spaces[s]
	end := f.offset + uint64(len(f.data))
	if end <= sp.cryptoRecv.base {
		return nil
	}
	if end-sp.cryptoRecv.base > maxCryptoBuffer {
		return frameErr(CryptoBufferExceeded, frameCrypto, "crypto data too far ahead")
	}
	if !sp.cryptoRecv.write(f.offset, f.data) {
		return frameErr(CryptoBufferExceeded, frameCrypto, "crypto data too fragmented")
	}
	data := sp.cryptoRecv.peek()
	if len(data) == 0 {
		return nil
	}
	buf := append([]byte(nil), data...)
	sp.cryptoRecv.consume(len(data))
	if err := c.tls.HandleData(spaceLevel(s), buf); err != nil {
		return cryptoError(err)
	}
	if err := c.processTLSEvents(); err != nil {
		var te *TransportError
		if errors.As(err, &te) {
			return te
		}
		return cryptoError(err)
	}
	return nil
}
