package quic

import (
	"time"
)

const (
	tpOriginalDCID            = 0x00
	tpMaxIdleTimeout          = 0x01
	tpStatelessResetToken     = 0x02
	tpMaxUDPPayloadSize       = 0x03
	tpInitialMaxData          = 0x04
	tpInitialMaxStreamDataBL  = 0x05
	tpInitialMaxStreamDataBR  = 0x06
	tpInitialMaxStreamDataUni = 0x07
	tpInitialMaxStreamsBidi   = 0x08
	tpInitialMaxStreamsUni    = 0x09
	tpAckDelayExponent        = 0x0a
	tpMaxAckDelay             = 0x0b
	tpDisableActiveMigration  = 0x0c
	tpPreferredAddress        = 0x0d
	tpActiveCIDLimit          = 0x0e
	tpInitialSCID             = 0x0f
	tpRetrySCID               = 0x10
	tpMaxDatagramFrameSize    = 0x20
)

const maxTransportParams = 256

type transportParams struct {
	originalDCID            []byte
	hasOriginalDCID         bool
	maxIdleTimeout          time.Duration
	statelessResetToken     []byte
	maxUDPPayloadSize       uint64
	initialMaxData          uint64
	initialMaxStreamDataBL  uint64
	initialMaxStreamDataBR  uint64
	initialMaxStreamDataUni uint64
	initialMaxStreamsBidi   uint64
	initialMaxStreamsUni    uint64
	ackDelayExponent        uint64
	maxAckDelay             time.Duration
	disableActiveMigration  bool
	hasPreferredAddress     bool
	activeCIDLimit          uint64
	initialSCID             []byte
	hasInitialSCID          bool
	retrySCID               []byte
	hasRetrySCID            bool
	maxDatagramFrameSize    uint64
}

func defaultTransportParams() transportParams {
	return transportParams{
		maxUDPPayloadSize: 65527,
		ackDelayExponent:  3,
		maxAckDelay:       25 * time.Millisecond,
		activeCIDLimit:    2,
	}
}

func appendParamVarint(b []byte, id, v uint64) []byte {
	b = AppendVarint(b, id)
	b = AppendVarint(b, uint64(VarintLen(v)))
	return AppendVarint(b, v)
}

func appendParamBytes(b []byte, id uint64, v []byte) []byte {
	b = AppendVarint(b, id)
	b = AppendVarint(b, uint64(len(v)))
	return append(b, v...)
}

func (p *transportParams) marshal(isServer bool) []byte {
	var b []byte
	if isServer && p.hasOriginalDCID {
		b = appendParamBytes(b, tpOriginalDCID, p.originalDCID)
	}
	if p.maxIdleTimeout > 0 {
		b = appendParamVarint(b, tpMaxIdleTimeout, uint64(p.maxIdleTimeout/time.Millisecond))
	}
	if isServer && len(p.statelessResetToken) == 16 {
		b = appendParamBytes(b, tpStatelessResetToken, p.statelessResetToken)
	}
	if p.maxUDPPayloadSize != 65527 {
		b = appendParamVarint(b, tpMaxUDPPayloadSize, p.maxUDPPayloadSize)
	}
	b = appendParamVarint(b, tpInitialMaxData, p.initialMaxData)
	b = appendParamVarint(b, tpInitialMaxStreamDataBL, p.initialMaxStreamDataBL)
	b = appendParamVarint(b, tpInitialMaxStreamDataBR, p.initialMaxStreamDataBR)
	b = appendParamVarint(b, tpInitialMaxStreamDataUni, p.initialMaxStreamDataUni)
	b = appendParamVarint(b, tpInitialMaxStreamsBidi, p.initialMaxStreamsBidi)
	b = appendParamVarint(b, tpInitialMaxStreamsUni, p.initialMaxStreamsUni)
	if p.ackDelayExponent != 3 {
		b = appendParamVarint(b, tpAckDelayExponent, p.ackDelayExponent)
	}
	if p.maxAckDelay != 25*time.Millisecond {
		b = appendParamVarint(b, tpMaxAckDelay, uint64(p.maxAckDelay/time.Millisecond))
	}
	if p.disableActiveMigration {
		b = appendParamBytes(b, tpDisableActiveMigration, nil)
	}
	if p.activeCIDLimit != 2 {
		b = appendParamVarint(b, tpActiveCIDLimit, p.activeCIDLimit)
	}
	b = appendParamBytes(b, tpInitialSCID, p.initialSCID)
	if isServer && p.hasRetrySCID {
		b = appendParamBytes(b, tpRetrySCID, p.retrySCID)
	}
	if p.maxDatagramFrameSize > 0 {
		b = appendParamVarint(b, tpMaxDatagramFrameSize, p.maxDatagramFrameSize)
	}
	return b
}

func parseTransportParams(b []byte, fromServer bool) (transportParams, *TransportError) {
	p := defaultTransportParams()
	seen := make(map[uint64]bool)
	bad := func(reason string) (transportParams, *TransportError) {
		return transportParams{}, transportErr(TransportParameterError, reason)
	}
	for len(b) > 0 {
		id, n := ReadVarint(b)
		if n == 0 {
			return bad("truncated parameter id")
		}
		b = b[n:]
		l, n := ReadVarint(b)
		if n == 0 || l > uint64(len(b)-n) {
			return bad("truncated parameter")
		}
		val := b[n : n+int(l)]
		b = b[n+int(l):]
		if seen[id] {
			return bad("duplicate parameter")
		}
		if len(seen) >= maxTransportParams {
			return bad("too many parameters")
		}
		seen[id] = true
		varint := func() (uint64, bool) {
			v, m := ReadVarint(val)
			return v, m != 0 && m == len(val)
		}
		switch id {
		case tpOriginalDCID:
			if !fromServer || len(val) > maxCIDLen {
				return bad("original_destination_connection_id")
			}
			p.originalDCID, p.hasOriginalDCID = append([]byte{}, val...), true
		case tpMaxIdleTimeout:
			v, ok := varint()
			if !ok {
				return bad("max_idle_timeout")
			}
			if v > uint64(24*time.Hour/time.Millisecond) {
				v = uint64(24 * time.Hour / time.Millisecond)
			}
			p.maxIdleTimeout = time.Duration(v) * time.Millisecond
		case tpStatelessResetToken:
			if !fromServer || len(val) != 16 {
				return bad("stateless_reset_token")
			}
			p.statelessResetToken = append([]byte{}, val...)
		case tpMaxUDPPayloadSize:
			v, ok := varint()
			if !ok || v < 1200 {
				return bad("max_udp_payload_size")
			}
			p.maxUDPPayloadSize = v
		case tpInitialMaxData:
			v, ok := varint()
			if !ok {
				return bad("initial_max_data")
			}
			p.initialMaxData = v
		case tpInitialMaxStreamDataBL:
			v, ok := varint()
			if !ok {
				return bad("initial_max_stream_data_bidi_local")
			}
			p.initialMaxStreamDataBL = v
		case tpInitialMaxStreamDataBR:
			v, ok := varint()
			if !ok {
				return bad("initial_max_stream_data_bidi_remote")
			}
			p.initialMaxStreamDataBR = v
		case tpInitialMaxStreamDataUni:
			v, ok := varint()
			if !ok {
				return bad("initial_max_stream_data_uni")
			}
			p.initialMaxStreamDataUni = v
		case tpInitialMaxStreamsBidi:
			v, ok := varint()
			if !ok || v > maxStreamsValue {
				return bad("initial_max_streams_bidi")
			}
			p.initialMaxStreamsBidi = v
		case tpInitialMaxStreamsUni:
			v, ok := varint()
			if !ok || v > maxStreamsValue {
				return bad("initial_max_streams_uni")
			}
			p.initialMaxStreamsUni = v
		case tpAckDelayExponent:
			v, ok := varint()
			if !ok || v > 20 {
				return bad("ack_delay_exponent")
			}
			p.ackDelayExponent = v
		case tpMaxAckDelay:
			v, ok := varint()
			if !ok || v >= 1<<14 {
				return bad("max_ack_delay")
			}
			p.maxAckDelay = time.Duration(v) * time.Millisecond
		case tpDisableActiveMigration:
			if len(val) != 0 {
				return bad("disable_active_migration")
			}
			p.disableActiveMigration = true
		case tpPreferredAddress:
			if !fromServer || len(val) < 4+2+16+2+1+16 {
				return bad("preferred_address")
			}
			cl := int(val[24])
			if cl < 1 || cl > maxCIDLen || len(val) != 25+cl+16 {
				return bad("preferred_address")
			}
			p.hasPreferredAddress = true
		case tpActiveCIDLimit:
			v, ok := varint()
			if !ok || v < 2 {
				return bad("active_connection_id_limit")
			}
			p.activeCIDLimit = v
		case tpInitialSCID:
			if len(val) > maxCIDLen {
				return bad("initial_source_connection_id")
			}
			p.initialSCID, p.hasInitialSCID = append([]byte{}, val...), true
		case tpRetrySCID:
			if !fromServer || len(val) > maxCIDLen {
				return bad("retry_source_connection_id")
			}
			p.retrySCID, p.hasRetrySCID = append([]byte{}, val...), true
		case tpMaxDatagramFrameSize:
			v, ok := varint()
			if !ok {
				return bad("max_datagram_frame_size")
			}
			p.maxDatagramFrameSize = v
		}
	}
	if !p.hasInitialSCID {
		return bad("missing initial_source_connection_id")
	}
	if fromServer && !p.hasOriginalDCID {
		return bad("missing original_destination_connection_id")
	}
	return p, nil
}
