package qtjson

import (
	"math"
	"sort"
)

type Kind uint8

const (
	Undefined Kind = iota
	Null
	Bool
	Integer
	Double
	String
	Array
	Object
)

type Value struct {
	kind Kind
	b    bool
	i    int64
	f    float64
	s    string
	raw  string
	arr  []Value
	obj  []Member
}

type Member struct {
	Key   string
	Value Value
}

func NewNull() Value                { return Value{kind: Null} }
func NewBool(b bool) Value          { return Value{kind: Bool, b: b} }
func NewInt(i int64) Value          { return Value{kind: Integer, i: i} }
func NewDouble(f float64) Value     { return Value{kind: Double, f: f} }
func NewString(s string) Value      { return Value{kind: String, s: s} }
func NewArray(items ...Value) Value { return Value{kind: Array, arr: append([]Value{}, items...)} }
func NewObject() Value              { return Value{kind: Object, obj: []Member{}} }

func NewStrings(items []string) Value {
	out := Value{kind: Array, arr: make([]Value, len(items))}
	for i, item := range items {
		out.arr[i] = NewString(item)
	}
	return out
}

func (v Value) Kind() Kind        { return v.kind }
func (v Value) IsUndefined() bool { return v.kind == Undefined }
func (v Value) IsNull() bool      { return v.kind == Null }
func (v Value) IsString() bool    { return v.kind == String }
func (v Value) IsObject() bool    { return v.kind == Object }
func (v Value) IsArray() bool     { return v.kind == Array }
func (v Value) IsBool() bool      { return v.kind == Bool }
func (v Value) IsNumber() bool    { return v.kind == Integer || v.kind == Double }

func (v Value) Str() string {
	if v.kind == String {
		return v.s
	}
	return ""
}

func (v Value) StrOr(def string) string {
	if v.kind == String {
		return v.s
	}
	return def
}

func (v Value) BoolValue() bool { return v.kind == Bool && v.b }

func (v Value) BoolOr(def bool) bool {
	if v.kind == Bool {
		return v.b
	}
	return def
}

func (v Value) Float() float64 {
	switch v.kind {
	case Integer:
		return float64(v.i)
	case Double:
		return v.f
	}
	return 0
}

func (v Value) IntOr(def int) int {
	switch v.kind {
	case Integer:
		if v.i >= math.MinInt32 && v.i <= math.MaxInt32 {
			return int(v.i)
		}
		return def
	case Double:
		if v.f >= math.MinInt32 && v.f <= math.MaxInt32 && float64(int32(v.f)) == v.f {
			return int(int32(v.f))
		}
		return def
	}
	return def
}

func (v Value) Int() int { return v.IntOr(0) }

func (v Value) Int64() int64 {
	switch v.kind {
	case Integer:
		return v.i
	case Double:
		if math.IsNaN(v.f) || math.IsInf(v.f, 0) {
			return 0
		}
		if v.f >= 9.2233720368547758e18 || v.f < -9.2233720368547758e18 {
			return 0
		}
		return int64(v.f)
	case Bool:
		if v.b {
			return 1
		}
	}
	return 0
}

func (v Value) Len() int {
	switch v.kind {
	case Array:
		return len(v.arr)
	case Object:
		return len(v.obj)
	}
	return 0
}

func (v Value) Items() []Value {
	if v.kind == Array {
		return v.arr
	}
	return nil
}

func (v Value) Members() []Member {
	if v.kind == Object {
		return v.obj
	}
	return nil
}

func (v Value) index(key string) (int, bool) {
	i := sort.Search(len(v.obj), func(i int) bool { return v.obj[i].Key >= key })
	return i, i < len(v.obj) && v.obj[i].Key == key
}

func (v Value) Get(key string) Value {
	if v.kind != Object {
		return Value{}
	}
	if i, ok := v.index(key); ok {
		return v.obj[i].Value
	}
	return Value{}
}

func (v Value) Has(key string) bool {
	if v.kind != Object {
		return false
	}
	_, ok := v.index(key)
	return ok
}

func (v Value) At(i int) Value {
	if v.kind != Array || i < 0 || i >= len(v.arr) {
		return Value{}
	}
	return v.arr[i]
}

func (v *Value) Set(key string, value Value) {
	if v.kind != Object {
		*v = NewObject()
	}
	i, ok := v.index(key)
	if ok {
		v.obj[i].Value = value
		return
	}
	v.obj = append(v.obj, Member{})
	copy(v.obj[i+1:], v.obj[i:])
	v.obj[i] = Member{Key: key, Value: value}
}

func (v *Value) Remove(key string) {
	if v.kind != Object {
		return
	}
	if i, ok := v.index(key); ok {
		v.obj = append(v.obj[:i], v.obj[i+1:]...)
	}
}

func (v *Value) Append(item Value) {
	if v.kind != Array {
		*v = Value{kind: Array}
	}
	v.arr = append(v.arr, item)
}

func (v Value) Clone() Value {
	switch v.kind {
	case Array:
		out := v
		out.arr = make([]Value, len(v.arr))
		for i, item := range v.arr {
			out.arr[i] = item.Clone()
		}
		return out
	case Object:
		out := v
		out.obj = make([]Member, len(v.obj))
		for i, member := range v.obj {
			out.obj[i] = Member{Key: member.Key, Value: member.Value.Clone()}
		}
		return out
	}
	return v
}

func ObjectOf(pairs ...any) Value {
	out := NewObject()
	for i := 0; i+1 < len(pairs); i += 2 {
		key, _ := pairs[i].(string)
		out.Set(key, From(pairs[i+1]))
	}
	return out
}

func From(value any) Value {
	switch v := value.(type) {
	case nil:
		return NewNull()
	case Value:
		return v
	case bool:
		return NewBool(v)
	case int:
		return NewInt(int64(v))
	case int32:
		return NewInt(int64(v))
	case int64:
		return NewInt(v)
	case float64:
		return NewDouble(v)
	case string:
		return NewString(v)
	case []string:
		return NewStrings(v)
	case []Value:
		return NewArray(v...)
	case map[string]any:
		out := NewObject()
		keys := make([]string, 0, len(v))
		for key := range v {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			out.Set(key, From(v[key]))
		}
		return out
	case []any:
		out := Value{kind: Array, arr: make([]Value, len(v))}
		for i, item := range v {
			out.arr[i] = From(item)
		}
		return out
	}
	return NewNull()
}

func (v Value) Interface() any {
	switch v.kind {
	case Null, Undefined:
		return nil
	case Bool:
		return v.b
	case Integer:
		return v.i
	case Double:
		return v.f
	case String:
		return v.s
	case Array:
		out := make([]any, len(v.arr))
		for i, item := range v.arr {
			out[i] = item.Interface()
		}
		return out
	case Object:
		out := make(map[string]any, len(v.obj))
		for _, member := range v.obj {
			out[member.Key] = member.Value.Interface()
		}
		return out
	}
	return nil
}

func (v Value) Equal(other Value) bool {
	if v.kind != other.kind {
		if v.IsNumber() && other.IsNumber() {
			return v.Float() == other.Float()
		}
		return false
	}
	switch v.kind {
	case Bool:
		return v.b == other.b
	case Integer:
		return v.i == other.i
	case Double:
		return v.f == other.f
	case String:
		return v.s == other.s
	case Array:
		if len(v.arr) != len(other.arr) {
			return false
		}
		for i := range v.arr {
			if !v.arr[i].Equal(other.arr[i]) {
				return false
			}
		}
		return true
	case Object:
		if len(v.obj) != len(other.obj) {
			return false
		}
		for i := range v.obj {
			if v.obj[i].Key != other.obj[i].Key || !v.obj[i].Value.Equal(other.obj[i].Value) {
				return false
			}
		}
		return true
	}
	return true
}

func (v Value) MapStrings(fn func(string) string) Value {
	switch v.kind {
	case String:
		return NewString(fn(v.s))
	case Array:
		out := Value{kind: Array, arr: make([]Value, len(v.arr))}
		for i, item := range v.arr {
			out.arr[i] = item.MapStrings(fn)
		}
		return out
	case Object:
		out := Value{kind: Object, obj: make([]Member, len(v.obj))}
		for i, member := range v.obj {
			out.obj[i] = Member{Key: member.Key, Value: member.Value.MapStrings(fn)}
		}
		return out
	}
	return v
}
