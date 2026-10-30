package facestream

import "encoding/binary"

const (
	FormatJPEG = 0
	FormatHEIC = 1
	FormatWebP = 2

	MaxReferenceSide   = 1024
	MaxMaskSide        = 128
	MaxReferencePacket = 256 * 1024
	referenceFixed     = 1 + 4 + 12 + LandmarkCount*4 + 3 + 2
	pointSteps         = 16
)

type Reference struct {
	ID     int
	Format int
	Time   uint32
	CropX  int
	CropY  int
	CropW  int
	CropH  int
	Width  int
	Height int
	Points [LandmarkCount][2]float64
	MaskW  int
	MaskH  int
	Mask   []byte
	Image  []byte
}

func maskContext(mask []byte, w, x, y int) int {
	at := func(px, py int) int {
		if px < 0 || py < 0 || px >= w {
			return 0
		}
		return int(mask[py*w+px])
	}
	return at(x-1, y) | at(x, y-1)<<1 | at(x-1, y-1)<<2 | at(x+1, y-1)<<3
}

func encodeMask(mask []byte, w, h int) []byte {
	var models [16]prob
	for i := range models {
		models[i] = probHalf
	}
	bits := make([]byte, w*h)
	for i, v := range mask[:w*h] {
		if v != 0 {
			bits[i] = 1
		}
	}
	enc := newRangeEncoder(nil)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			enc.bit(&models[maskContext(bits, w, x, y)], int(bits[y*w+x]))
		}
	}
	return enc.finish()
}

func decodeMask(data []byte, w, h int) []byte {
	var models [16]prob
	for i := range models {
		models[i] = probHalf
	}
	out := make([]byte, w*h)
	dec := newRangeDecoder(data)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			out[y*w+x] = byte(dec.bit(&models[maskContext(out, w, x, y)]))
		}
	}
	return out
}

func validReference(r *Reference) bool {
	if r.ID < 0 || r.ID > 15 || r.Format < 0 || r.Format > 2 {
		return false
	}
	if r.Width < 1 || r.Width > MaxReferenceSide || r.Height < 1 || r.Height > MaxReferenceSide {
		return false
	}
	if r.CropW < 1 || r.CropH < 1 || r.CropX < 0 || r.CropY < 0 || r.CropX+r.CropW > 65535 || r.CropY+r.CropH > 65535 {
		return false
	}
	if (r.MaskW == 0) != (r.MaskH == 0) || r.MaskW < 0 || r.MaskW > MaxMaskSide || r.MaskH < 0 || r.MaskH > MaxMaskSide {
		return false
	}
	return len(r.Image) > 0
}

func EncodeReference(r *Reference) ([]byte, error) {
	if !validReference(r) || (r.MaskW > 0 && len(r.Mask) < r.MaskW*r.MaskH) {
		return nil, ErrInput
	}
	out := make([]byte, 0, referenceFixed+len(r.Image)+256)
	out = append(out, header(KindReference, r.ID))
	out = binary.BigEndian.AppendUint32(out, r.Time)
	for _, v := range []int{r.CropX, r.CropY, r.CropW, r.CropH, r.Width, r.Height} {
		out = binary.BigEndian.AppendUint16(out, uint16(v))
	}
	for _, p := range r.Points {
		for axis := 0; axis < 2; axis++ {
			if !finite(p[axis]) {
				return nil, ErrInput
			}
			out = binary.BigEndian.AppendUint16(out, uint16(int16(clampInt(floorHalf(p[axis]*pointSteps), -32768, 32767))))
		}
	}
	out = append(out, byte(r.Format), byte(r.MaskW), byte(r.MaskH))
	var mask []byte
	if r.MaskW > 0 {
		mask = encodeMask(r.Mask, r.MaskW, r.MaskH)
	}
	out = binary.BigEndian.AppendUint16(out, uint16(len(mask)))
	out = append(out, mask...)
	out = append(out, r.Image...)
	if len(out) > MaxReferencePacket {
		return nil, ErrLong
	}
	return out, nil
}

func DecodeReference(packet []byte) (*Reference, error) {
	if len(packet) < referenceFixed+1 {
		return nil, ErrShort
	}
	if len(packet) > MaxReferencePacket {
		return nil, ErrLong
	}
	if int(packet[0]>>4)&3 != Version {
		return nil, ErrVersion
	}
	if int(packet[0]>>6) != KindReference {
		return nil, ErrNotPicture
	}
	r := &Reference{ID: int(packet[0] & 15), Time: binary.BigEndian.Uint32(packet[1:5])}
	at := 5
	u16 := func() int {
		v := int(binary.BigEndian.Uint16(packet[at : at+2]))
		at += 2
		return v
	}
	r.CropX, r.CropY, r.CropW, r.CropH, r.Width, r.Height = u16(), u16(), u16(), u16(), u16(), u16()
	for i := range r.Points {
		for axis := 0; axis < 2; axis++ {
			r.Points[i][axis] = float64(int16(u16())) / pointSteps
		}
	}
	r.Format = int(packet[at])
	r.MaskW = int(packet[at+1])
	r.MaskH = int(packet[at+2])
	at += 3
	maskLen := u16()
	if at+maskLen >= len(packet) {
		return nil, ErrCorrupt
	}
	if r.MaskW > 0 && r.MaskH > 0 {
		r.Mask = decodeMask(packet[at:at+maskLen], r.MaskW, r.MaskH)
	} else if maskLen != 0 {
		return nil, ErrCorrupt
	}
	at += maskLen
	r.Image = packet[at:]
	if !validReference(r) {
		return nil, ErrCorrupt
	}
	return r, nil
}

func MapToReference(r *Reference, p [2]float64) [2]float64 {
	return [2]float64{
		(p[0] - float64(r.CropX)) * (float64(r.Width) / float64(r.CropW)),
		(p[1] - float64(r.CropY)) * (float64(r.Height) / float64(r.CropH)),
	}
}
