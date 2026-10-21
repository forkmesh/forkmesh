package quic

import (
	"errors"
	"fmt"
	"os"
)

type TransportErrorCode uint64

const (
	NoError                 TransportErrorCode = 0x00
	InternalError           TransportErrorCode = 0x01
	ConnectionRefused       TransportErrorCode = 0x02
	FlowControlError        TransportErrorCode = 0x03
	StreamLimitError        TransportErrorCode = 0x04
	StreamStateError        TransportErrorCode = 0x05
	FinalSizeError          TransportErrorCode = 0x06
	FrameEncodingError      TransportErrorCode = 0x07
	TransportParameterError TransportErrorCode = 0x08
	ConnectionIDLimitError  TransportErrorCode = 0x09
	ProtocolViolation       TransportErrorCode = 0x0a
	InvalidToken            TransportErrorCode = 0x0b
	ApplicationErrorCode    TransportErrorCode = 0x0c
	CryptoBufferExceeded    TransportErrorCode = 0x0d
	KeyUpdateError          TransportErrorCode = 0x0e
	AEADLimitReached        TransportErrorCode = 0x0f
	NoViablePath            TransportErrorCode = 0x10
	CryptoErrorBase         TransportErrorCode = 0x100
)

type TransportError struct {
	Code      TransportErrorCode
	FrameType uint64
	Reason    string
	Remote    bool
}

func (e *TransportError) Error() string {
	side := "local"
	if e.Remote {
		side = "remote"
	}
	if e.Reason != "" {
		return fmt.Sprintf("quic: %s transport error 0x%x: %s", side, uint64(e.Code), e.Reason)
	}
	return fmt.Sprintf("quic: %s transport error 0x%x", side, uint64(e.Code))
}

type ApplicationError struct {
	Code   uint64
	Reason string
	Remote bool
}

func (e *ApplicationError) Error() string {
	side := "local"
	if e.Remote {
		side = "remote"
	}
	return fmt.Sprintf("quic: %s application error 0x%x: %s", side, e.Code, e.Reason)
}

type StreamError struct {
	StreamID uint64
	Code     uint64
	Remote   bool
}

func (e *StreamError) Error() string {
	if e.Remote {
		return fmt.Sprintf("quic: stream %d reset by peer with code 0x%x", e.StreamID, e.Code)
	}
	return fmt.Sprintf("quic: stream %d cancelled with code 0x%x", e.StreamID, e.Code)
}

var (
	ErrIdleTimeout         = errors.New("quic: idle timeout")
	ErrHandshakeTimeout    = errors.New("quic: handshake timeout")
	ErrStatelessReset      = errors.New("quic: stateless reset")
	ErrClosed              = errors.New("quic: connection closed")
	ErrEndpointClosed      = errors.New("quic: endpoint closed")
	ErrStreamLimit         = errors.New("quic: stream limit reached")
	ErrDatagramsDisabled   = errors.New("quic: peer does not accept datagrams")
	ErrDatagramTooLarge    = errors.New("quic: datagram too large")
	ErrDatagramQueueFull   = errors.New("quic: datagram queue full")
	ErrWriteClosed         = errors.New("quic: write on closed stream")
	ErrNotSupported        = errors.New("quic: not supported on this stream")
	ErrNoCompatibleVersion = errors.New("quic: server supports no compatible version")
	ErrUnsupportedCipher   = errors.New("quic: negotiated cipher suite is not supported")
	ErrDeadlineExceeded    = os.ErrDeadlineExceeded
	ErrMultipathDisabled   = errors.New("quic: multipath not negotiated")
	ErrNoPathAvailable     = errors.New("quic: no path id with connection ids on both sides")
	ErrPathLimit           = errors.New("quic: path limit reached")
	ErrPathValidation      = errors.New("quic: path validation failed")
	ErrPathAbandoned       = errors.New("quic: path abandoned")
	ErrUnknownPath         = errors.New("quic: unknown path")
	ErrLastPath            = errors.New("quic: cannot abandon the last usable path")
	ErrClientOnly          = errors.New("quic: only a client opens paths")
)

func transportErr(code TransportErrorCode, reason string) *TransportError {
	return &TransportError{Code: code, Reason: reason}
}

func frameErr(code TransportErrorCode, frameType uint64, reason string) *TransportError {
	return &TransportError{Code: code, FrameType: frameType, Reason: reason}
}
