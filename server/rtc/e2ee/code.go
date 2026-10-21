package e2ee

import (
	"bytes"
	"crypto/sha512"
	"encoding/binary"
	"fmt"
	"sort"
	"strings"
)

const (
	codeIterations = 5200
	codeGroups     = 6
)

type Member struct {
	IDK  []byte
	Acct string
}

func VerificationCode(call string, members []Member) (string, error) {
	if !validCall(call) || len(members) == 0 {
		return "", ErrInvalid
	}
	list := make([]Member, 0, len(members))
	for _, m := range members {
		if len(m.IDK) != IdentitySize || !validAcct(m.Acct) {
			return "", ErrInvalid
		}
		list = append(list, m)
	}
	sort.Slice(list, func(i, j int) bool {
		if c := bytes.Compare(list[i].IDK, list[j].IDK); c != 0 {
			return c < 0
		}
		return list[i].Acct < list[j].Acct
	})
	var entries bytes.Buffer
	for i, m := range list {
		if i > 0 && bytes.Equal(m.IDK, list[i-1].IDK) && m.Acct == list[i-1].Acct {
			continue
		}
		entries.Write(m.IDK)
		var n [2]byte
		binary.BigEndian.PutUint16(n[:], uint16(len(m.Acct)))
		entries.Write(n[:])
		entries.WriteString(m.Acct)
	}
	h := sha512.New()
	h.Write([]byte(codeLabel + "\n" + call + "\n"))
	h.Write(entries.Bytes())
	sum := h.Sum(nil)
	for i := 1; i < codeIterations; i++ {
		h.Reset()
		h.Write(sum)
		h.Write(entries.Bytes())
		sum = h.Sum(sum[:0])
	}
	groups := make([]string, codeGroups)
	for i := range groups {
		var v uint64
		for _, b := range sum[i*5 : i*5+5] {
			v = v<<8 | uint64(b)
		}
		groups[i] = fmt.Sprintf("%05d", v%100000)
	}
	return strings.Join(groups, " "), nil
}
