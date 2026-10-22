package dtls

import (
	"crypto/hmac"
	"crypto/sha256"
)

func prf(secret []byte, label string, seed []byte, length int) []byte {
	labelSeed := make([]byte, 0, len(label)+len(seed))
	labelSeed = append(labelSeed, label...)
	labelSeed = append(labelSeed, seed...)
	out := make([]byte, 0, length+sha256.Size)
	mac := hmac.New(sha256.New, secret)
	mac.Write(labelSeed)
	a := mac.Sum(nil)
	for len(out) < length {
		mac.Reset()
		mac.Write(a)
		mac.Write(labelSeed)
		out = mac.Sum(out)
		mac.Reset()
		mac.Write(a)
		a = mac.Sum(a[:0])
	}
	return out[:length]
}
