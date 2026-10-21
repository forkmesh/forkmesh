package facestream

import (
	"encoding/binary"
	"errors"
)

const (
	KindAtlas = 3

	MouthClosed = 0
	MouthOpen   = 1
	MouthWide   = 2
	MouthRound  = 3

	MouthPointCount = 20
	MaxAtlasTiles   = 8
	MinAtlasSide    = 8
	MaxAtlasSide    = 128
	MaxAtlasPacket  = 64 * 1024
	atlasFixed      = 9
	atlasTileBytes  = 3 + MouthPointCount*4
	atlasSteps      = 16
)

var ErrNotAtlas = errors.New("facestream: not a mouth atlas packet")

type AtlasTile struct {
	State  int
	Mouth  float64
	Smile  float64
	Points [MouthPointCount][2]float64
}

type Atlas struct {
	ID     int
	Time   uint32
	TileW  int
	TileH  int
	Format int
	Tiles  []AtlasTile
	Image  []byte
}

func validAtlas(a *Atlas) bool {
	if a.ID < 0 || a.ID > 15 || a.Format < 0 || a.Format > 2 {
		return false
	}
	if a.TileW < MinAtlasSide || a.TileW > MaxAtlasSide || a.TileH < MinAtlasSide || a.TileH > MaxAtlasSide {
		return false
	}
	if len(a.Tiles) < 1 || len(a.Tiles) > MaxAtlasTiles {
		return false
	}
	for _, t := range a.Tiles {
		if t.State < MouthClosed || t.State > MouthRound {
			return false
		}
	}
	return len(a.Image) > 0
}

func atlasLevel(v float64) int {
	return clampInt(floorHalf(v*atlasSteps), 0, atlasSteps)
}

func EncodeAtlas(a *Atlas) ([]byte, error) {
	if !validAtlas(a) {
		return nil, ErrInput
	}
	out := make([]byte, 0, atlasFixed+len(a.Tiles)*atlasTileBytes+len(a.Image))
	out = append(out, header(KindAtlas, a.ID))
	out = binary.BigEndian.AppendUint32(out, a.Time)
	out = append(out, byte(a.TileW), byte(a.TileH), byte(len(a.Tiles)), byte(a.Format))
	for _, t := range a.Tiles {
		if !finite(t.Mouth) || !finite(t.Smile) {
			return nil, ErrInput
		}
		out = append(out, byte(t.State), byte(atlasLevel(t.Mouth)), byte(atlasLevel(t.Smile)))
		for _, p := range t.Points {
			for axis := 0; axis < 2; axis++ {
				if !finite(p[axis]) {
					return nil, ErrInput
				}
				out = binary.BigEndian.AppendUint16(out, uint16(int16(clampInt(floorHalf(p[axis]*atlasSteps), -32768, 32767))))
			}
		}
	}
	out = append(out, a.Image...)
	if len(out) > MaxAtlasPacket {
		return nil, ErrLong
	}
	return out, nil
}

func DecodeAtlas(packet []byte) (*Atlas, error) {
	if len(packet) < atlasFixed+atlasTileBytes+1 {
		return nil, ErrShort
	}
	if len(packet) > MaxAtlasPacket {
		return nil, ErrLong
	}
	if int(packet[0]>>4)&3 != Version {
		return nil, ErrVersion
	}
	if int(packet[0]>>6) != KindAtlas {
		return nil, ErrNotAtlas
	}
	a := &Atlas{ID: int(packet[0] & 15), Time: binary.BigEndian.Uint32(packet[1:5]), TileW: int(packet[5]), TileH: int(packet[6]), Format: int(packet[8])}
	n := int(packet[7])
	if n < 1 || n > MaxAtlasTiles || len(packet) <= atlasFixed+n*atlasTileBytes {
		return nil, ErrCorrupt
	}
	at := atlasFixed
	for k := 0; k < n; k++ {
		t := AtlasTile{State: int(packet[at])}
		mouth, smile := int(packet[at+1]), int(packet[at+2])
		if mouth > atlasSteps || smile > atlasSteps {
			return nil, ErrCorrupt
		}
		t.Mouth = float64(mouth) / atlasSteps
		t.Smile = float64(smile) / atlasSteps
		at += 3
		for i := range t.Points {
			for axis := 0; axis < 2; axis++ {
				t.Points[i][axis] = float64(int16(binary.BigEndian.Uint16(packet[at:at+2]))) / atlasSteps
				at += 2
			}
		}
		a.Tiles = append(a.Tiles, t)
	}
	a.Image = packet[at:]
	if !validAtlas(a) {
		return nil, ErrCorrupt
	}
	return a, nil
}
