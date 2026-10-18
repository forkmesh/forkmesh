package mirrornode

import (
	"encoding/json"
	"errors"
	"math"
	"regexp"
	"strconv"
	"strings"
)

const maxExprLength = 4096

var (
	errExpr            = errors.New("workflow expression is not supported")
	exprTemplate       = regexp.MustCompile(`\$\{\{(.*?)\}\}`)
	exprSecretRef      = regexp.MustCompile(`^\s*secrets\.([A-Za-z_][A-Za-z0-9_]*)\s*$`)
	exprStatusFunction = regexp.MustCompile(`(?i)\b(success|failure|always|cancelled)\s*\(`)
)

type exprContext struct {
	EventName  string
	Ref        string
	RefName    string
	SHA        string
	Repository string
	Actor      string
	Event      map[string]any
	Inputs     map[string]string
	Secrets    map[string]string
	Env        map[string]string
	Failed     bool
	Cancelled  bool
}

type exprFilter []any

type exprToken struct {
	kind  byte
	text  string
	value any
}

type exprParser struct {
	tokens []exprToken
	pos    int
	ctx    *exprContext
}

func evalExpr(source string, ctx *exprContext) (any, error) {
	if len(source) > maxExprLength {
		return nil, errExpr
	}
	tokens, err := lexExpr(source)
	if err != nil {
		return nil, err
	}
	parser := &exprParser{tokens: tokens, ctx: ctx}
	value, err := parser.or()
	if err != nil {
		return nil, err
	}
	if parser.pos != len(parser.tokens) {
		return nil, errExpr
	}
	if filter, ok := value.(exprFilter); ok {
		return []any(filter), nil
	}
	return value, nil
}

func evalCondition(condition string, ctx *exprContext) (bool, error) {
	condition = strings.TrimSpace(condition)
	if strings.HasPrefix(condition, "${{") && strings.HasSuffix(condition, "}}") &&
		!strings.Contains(condition[3:len(condition)-2], "${{") {
		condition = strings.TrimSpace(condition[3 : len(condition)-2])
	}
	if condition == "" {
		condition = "success()"
	}
	value, err := evalExpr(condition, ctx)
	if err != nil {
		return false, err
	}
	if !exprStatusFunction.MatchString(condition) && (ctx.Failed || ctx.Cancelled) {
		return false, nil
	}
	return exprTruthy(value), nil
}

func interpolateExpr(text string, ctx *exprContext) (string, error) {
	var failure error
	out := exprTemplate.ReplaceAllStringFunc(text, func(match string) string {
		value, err := evalExpr(match[3:len(match)-2], ctx)
		if err != nil {
			failure = err
			return ""
		}
		return exprString(value)
	})
	return out, failure
}

func substituteRunExpr(script string, ctx *exprContext, exported func(string) bool) (string, [][2]string, error) {
	var env [][2]string
	var failure error
	var out strings.Builder
	last := 0
	for _, loc := range exprTemplate.FindAllStringSubmatchIndex(script, -1) {
		out.WriteString(script[last:loc[0]])
		last = loc[1]
		body := script[loc[2]:loc[3]]
		quoted := shellQuoteState(script[:loc[0]])
		if match := exprSecretRef.FindStringSubmatch(body); match != nil && exported(match[1]) {
			out.WriteString(shellVarRef(match[1], quoted))
			continue
		}
		value, err := evalExpr(body, ctx)
		if err != nil && failure == nil {
			failure = err
		}
		name := "FM_EXPR_" + strconv.Itoa(len(env)+1)
		env = append(env, [2]string{name, exprString(value)})
		out.WriteString(shellVarRef(name, quoted))
	}
	out.WriteString(script[last:])
	return out.String(), env, failure
}

func shellVarRef(name string, quoted byte) string {
	switch quoted {
	case '"':
		return "${" + name + "}"
	case '\'':
		return `'"$` + name + `"'`
	}
	return `"$` + name + `"`
}

func shellQuoteState(prefix string) byte {
	single, double := false, false
	for i := 0; i < len(prefix); i++ {
		c := prefix[i]
		switch {
		case single:
			if c == '\'' {
				single = false
			}
		case c == '\\':
			i++
		case double:
			if c == '"' {
				double = false
			}
		case c == '\'':
			single = true
		case c == '"':
			double = true
		case c == '#' && (i == 0 || strings.IndexByte(" \t\n;", prefix[i-1]) >= 0):
			end := strings.IndexByte(prefix[i:], '\n')
			if end < 0 {
				return 0
			}
			i += end
		}
	}
	switch {
	case single:
		return '\''
	case double:
		return '"'
	}
	return 0
}

func exprTruthy(value any) bool {
	switch v := value.(type) {
	case nil:
		return false
	case bool:
		return v
	case float64:
		return v != 0 && !math.IsNaN(v)
	case string:
		return v != ""
	}
	return true
}

func exprString(value any) string {
	switch v := value.(type) {
	case nil:
		return ""
	case bool:
		if v {
			return "true"
		}
		return "false"
	case float64:
		if v == math.Trunc(v) && math.Abs(v) < 1e15 {
			return strconv.FormatInt(int64(v), 10)
		}
		return strconv.FormatFloat(v, 'g', -1, 64)
	case string:
		return v
	case exprFilter:
		return exprString([]any(v))
	}
	raw, err := json.Marshal(value)
	if err != nil {
		return ""
	}
	return string(raw)
}

func exprNumber(value any) float64 {
	switch v := value.(type) {
	case nil:
		return 0
	case bool:
		if v {
			return 1
		}
		return 0
	case float64:
		return v
	case string:
		text := strings.TrimSpace(v)
		if text == "" {
			return 0
		}
		if n, err := strconv.ParseFloat(text, 64); err == nil {
			return n
		}
		if strings.HasPrefix(text, "0x") {
			if n, err := strconv.ParseInt(text[2:], 16, 64); err == nil {
				return float64(n)
			}
		}
	}
	return math.NaN()
}

func exprEqual(left, right any) bool {
	if l, ok := left.(string); ok {
		if r, ok := right.(string); ok {
			return strings.EqualFold(l, r)
		}
	}
	switch left.(type) {
	case nil, bool, float64, string:
	default:
		return false
	}
	switch right.(type) {
	case nil, bool, float64, string:
	default:
		return false
	}
	if left == nil && right == nil {
		return true
	}
	return exprNumber(left) == exprNumber(right)
}

func lexExpr(source string) ([]exprToken, error) {
	var tokens []exprToken
	for i := 0; i < len(source); {
		c := source[i]
		switch {
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			i++
		case c == '\'':
			var text strings.Builder
			j := i + 1
			for {
				if j >= len(source) {
					return nil, errExpr
				}
				if source[j] == '\'' {
					if j+1 < len(source) && source[j+1] == '\'' {
						text.WriteByte('\'')
						j += 2
						continue
					}
					break
				}
				text.WriteByte(source[j])
				j++
			}
			tokens = append(tokens, exprToken{kind: 'v', value: text.String()})
			i = j + 1
		case c >= '0' && c <= '9' || (c == '-' && i+1 < len(source) && source[i+1] >= '0' && source[i+1] <= '9' &&
			(len(tokens) == 0 || strings.IndexByte("o(,!", tokens[len(tokens)-1].kind) >= 0)):
			j := i + 1
			for j < len(source) && (isExprWord(source[j]) || source[j] == '.') {
				j++
			}
			n := exprNumber(source[i:j])
			if math.IsNaN(n) {
				return nil, errExpr
			}
			tokens = append(tokens, exprToken{kind: 'v', value: n})
			i = j
		case isExprWord(c):
			j := i
			for j < len(source) && (isExprWord(source[j]) || source[j] == '-') {
				j++
			}
			word := source[i:j]
			switch word {
			case "true":
				tokens = append(tokens, exprToken{kind: 'v', value: true})
			case "false":
				tokens = append(tokens, exprToken{kind: 'v', value: false})
			case "null":
				tokens = append(tokens, exprToken{kind: 'v', value: nil})
			default:
				tokens = append(tokens, exprToken{kind: 'w', text: word})
			}
			i = j
		default:
			two := ""
			if i+1 < len(source) {
				two = source[i : i+2]
			}
			switch {
			case two == "==" || two == "!=" || two == "&&" || two == "||" || two == "<=" || two == ">=":
				tokens = append(tokens, exprToken{kind: 'o', text: two})
				i += 2
			case c == '<' || c == '>':
				tokens = append(tokens, exprToken{kind: 'o', text: string(c)})
				i++
			case strings.IndexByte(".*[](),!", c) >= 0:
				tokens = append(tokens, exprToken{kind: c, text: string(c)})
				i++
			default:
				return nil, errExpr
			}
		}
	}
	return tokens, nil
}

func isExprWord(c byte) bool {
	return c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9'
}

func (p *exprParser) peek() exprToken {
	if p.pos < len(p.tokens) {
		return p.tokens[p.pos]
	}
	return exprToken{}
}

func (p *exprParser) operator(text string) bool {
	if token := p.peek(); token.kind == 'o' && token.text == text {
		p.pos++
		return true
	}
	return false
}

func (p *exprParser) or() (any, error) {
	left, err := p.and()
	if err != nil {
		return nil, err
	}
	for p.operator("||") {
		right, err := p.and()
		if err != nil {
			return nil, err
		}
		if !exprTruthy(left) {
			left = right
		}
	}
	return left, nil
}

func (p *exprParser) and() (any, error) {
	left, err := p.compare()
	if err != nil {
		return nil, err
	}
	for p.operator("&&") {
		right, err := p.compare()
		if err != nil {
			return nil, err
		}
		if exprTruthy(left) {
			left = right
		}
	}
	return left, nil
}

func (p *exprParser) compare() (any, error) {
	left, err := p.unary()
	if err != nil {
		return nil, err
	}
	for {
		token := p.peek()
		if token.kind != 'o' || token.text == "&&" || token.text == "||" {
			return left, nil
		}
		p.pos++
		right, err := p.unary()
		if err != nil {
			return nil, err
		}
		switch token.text {
		case "==":
			left = exprEqual(left, right)
		case "!=":
			left = !exprEqual(left, right)
		default:
			left = exprOrder(token.text, left, right)
		}
	}
}

func exprOrder(op string, left, right any) bool {
	if l, ok := left.(string); ok {
		if r, ok := right.(string); ok {
			l, r = strings.ToLower(l), strings.ToLower(r)
			switch op {
			case "<":
				return l < r
			case "<=":
				return l <= r
			case ">":
				return l > r
			}
			return l >= r
		}
	}
	l, r := exprNumber(left), exprNumber(right)
	switch op {
	case "<":
		return l < r
	case "<=":
		return l <= r
	case ">":
		return l > r
	}
	return l >= r
}

func (p *exprParser) unary() (any, error) {
	if p.peek().kind == '!' {
		p.pos++
		value, err := p.unary()
		if err != nil {
			return nil, err
		}
		return !exprTruthy(value), nil
	}
	return p.postfix()
}

func (p *exprParser) postfix() (any, error) {
	value, err := p.primary()
	if err != nil {
		return nil, err
	}
	for {
		switch p.peek().kind {
		case '.':
			p.pos++
			token := p.peek()
			switch token.kind {
			case '*':
				p.pos++
				value = exprStar(value)
			case 'w':
				p.pos++
				value = exprProperty(value, token.text)
			default:
				return nil, errExpr
			}
		case '[':
			p.pos++
			if p.peek().kind == '*' {
				p.pos++
				value = exprStar(value)
			} else {
				index, err := p.or()
				if err != nil {
					return nil, err
				}
				value = exprIndex(value, index)
			}
			if p.peek().kind != ']' {
				return nil, errExpr
			}
			p.pos++
		default:
			return value, nil
		}
	}
}

func exprStar(value any) any {
	switch v := value.(type) {
	case []any:
		return exprFilter(v)
	case exprFilter:
		var out exprFilter
		for _, item := range v {
			if list, ok := item.([]any); ok {
				out = append(out, list...)
			}
		}
		return out
	case map[string]any:
		var out exprFilter
		for _, item := range v {
			out = append(out, item)
		}
		return out
	}
	return exprFilter(nil)
}

func exprProperty(value any, name string) any {
	switch v := value.(type) {
	case map[string]any:
		if found, ok := v[name]; ok {
			return found
		}
		for key, found := range v {
			if strings.EqualFold(key, name) {
				return found
			}
		}
	case exprFilter:
		out := exprFilter{}
		for _, item := range v {
			if found := exprProperty(item, name); found != nil {
				out = append(out, found)
			}
		}
		return out
	}
	return nil
}

func exprIndex(value, index any) any {
	switch v := value.(type) {
	case []any:
		n := exprNumber(index)
		if n >= 0 && n == math.Trunc(n) && int(n) < len(v) {
			return v[int(n)]
		}
		return nil
	case exprFilter:
		return exprIndex([]any(v), index)
	}
	if name, ok := index.(string); ok {
		return exprProperty(value, name)
	}
	return nil
}

func (p *exprParser) primary() (any, error) {
	token := p.peek()
	switch token.kind {
	case 'v':
		p.pos++
		return token.value, nil
	case '(':
		p.pos++
		value, err := p.or()
		if err != nil {
			return nil, err
		}
		if p.peek().kind != ')' {
			return nil, errExpr
		}
		p.pos++
		return value, nil
	case 'w':
		p.pos++
		if p.peek().kind == '(' {
			p.pos++
			var args []any
			for p.peek().kind != ')' {
				if len(args) > 0 {
					if p.peek().kind != ',' {
						return nil, errExpr
					}
					p.pos++
				}
				arg, err := p.or()
				if err != nil {
					return nil, err
				}
				args = append(args, arg)
				if p.pos >= len(p.tokens) {
					return nil, errExpr
				}
			}
			p.pos++
			return p.call(token.text, args)
		}
		return p.context(token.text), nil
	}
	return nil, errExpr
}

func (p *exprParser) context(name string) any {
	ctx := p.ctx
	if ctx == nil {
		return nil
	}
	switch strings.ToLower(name) {
	case "github":
		return map[string]any{
			"event_name": ctx.EventName, "ref": ctx.Ref, "ref_name": ctx.RefName, "sha": ctx.SHA,
			"repository": ctx.Repository, "actor": ctx.Actor, "event": exprEventMap(ctx.Event),
		}
	case "inputs":
		return exprStringMap(ctx.Inputs)
	case "secrets":
		return exprStringMap(ctx.Secrets)
	case "env":
		return exprStringMap(ctx.Env)
	}
	return ""
}

func exprEventMap(event map[string]any) map[string]any {
	if event == nil {
		return map[string]any{}
	}
	return event
}

func exprStringMap(values map[string]string) map[string]any {
	out := make(map[string]any, len(values))
	for key, value := range values {
		out[key] = value
	}
	return out
}

func (p *exprParser) call(name string, args []any) (any, error) {
	switch strings.ToLower(name) {
	case "success", "failure", "always", "cancelled":
		if len(args) != 0 {
			return nil, errExpr
		}
		ctx := p.ctx
		if ctx == nil {
			ctx = &exprContext{}
		}
		switch strings.ToLower(name) {
		case "success":
			return !ctx.Failed && !ctx.Cancelled, nil
		case "failure":
			return ctx.Failed && !ctx.Cancelled, nil
		case "cancelled":
			return ctx.Cancelled, nil
		}
		return true, nil
	case "contains":
		if len(args) != 2 {
			return nil, errExpr
		}
		switch haystack := args[0].(type) {
		case []any:
			return exprListContains(haystack, args[1]), nil
		case exprFilter:
			return exprListContains(haystack, args[1]), nil
		}
		return strings.Contains(strings.ToLower(exprString(args[0])), strings.ToLower(exprString(args[1]))), nil
	case "startswith":
		if len(args) != 2 {
			return nil, errExpr
		}
		return strings.HasPrefix(strings.ToLower(exprString(args[0])), strings.ToLower(exprString(args[1]))), nil
	case "endswith":
		if len(args) != 2 {
			return nil, errExpr
		}
		return strings.HasSuffix(strings.ToLower(exprString(args[0])), strings.ToLower(exprString(args[1]))), nil
	}
	return nil, errExpr
}

func exprListContains(list []any, item any) bool {
	for _, candidate := range list {
		if exprEqual(candidate, item) {
			return true
		}
	}
	return false
}
