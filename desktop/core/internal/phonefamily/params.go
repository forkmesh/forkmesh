package phonefamily

import (
	"encoding/json"
	"math"

	"forkmesh.com/desktop-core/internal/rpc"
)

type Params map[string]json.RawMessage

func ParamsOf(req *rpc.Request) Params {
	out := Params{}
	if req != nil && len(req.Params) > 0 {
		_ = json.Unmarshal(req.Params, &out)
	}
	return out
}

func (p Params) String(key string) string {
	var text string
	if raw, ok := p[key]; ok && json.Unmarshal(raw, &text) == nil {
		return text
	}
	return ""
}

func (p Params) Number(key string) (float64, bool) {
	raw, ok := p[key]
	if !ok {
		return 0, false
	}
	var number float64
	if json.Unmarshal(raw, &number) != nil {
		return 0, false
	}
	return number, true
}

func (p Params) Int(key string, fallback int) int {
	number, ok := p.Number(key)
	if !ok || math.IsNaN(number) || math.IsInf(number, 0) || math.Trunc(number) != number ||
		number < math.MinInt32 || number > math.MaxInt32 {
		return fallback
	}
	return int(number)
}

func (p Params) Bool(key string) (bool, bool) {
	raw, ok := p[key]
	if !ok {
		return false, false
	}
	var value bool
	if json.Unmarshal(raw, &value) != nil {
		return false, false
	}
	return value, true
}

func (p Params) Object(key string) map[string]json.RawMessage {
	out := map[string]json.RawMessage{}
	if raw, ok := p[key]; ok {
		_ = json.Unmarshal(raw, &out)
	}
	return out
}
