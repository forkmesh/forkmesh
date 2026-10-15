package agentrun

import (
	"regexp"
	"strings"

	"forkmesh.com/desktop-core/internal/qtjson"
)

var (
	bearerHeader         = regexp.MustCompile(`(?i)(authorization[\t\n\v\f\r ]*:[\t\n\v\f\r ]*bearer[\t\n\v\f\r ]+)[A-Za-z0-9._~+/-]{8,}`)
	credentialAssignment = regexp.MustCompile(`(?i)(["']?(?:accessToken|refreshToken|apiKey|anthropicApiKey|openAiApiKey|authorization)["']?[\t\n\v\f\r ]*[:=][\t\n\v\f\r ]*["'])[^"'\r\n]+`)
	skToken              = regexp.MustCompile(`\bsk-[A-Za-z0-9_-]{20,}`)
	assignmentKeys       = []string{"accesstoken", "refreshtoken", "apikey", "anthropicapikey", "openaiapikey", "authorization"}
	sensitiveKeys        = map[string]bool{"accesstoken": true, "refreshtoken": true, "apikey": true,
		"anthropicapikey": true, "openaiapikey": true, "authorization": true, "credentials": true}
)

func RedactText(text string, credentials []string) string {
	for _, secret := range credentials {
		if len16(secret) >= 8 && strings.Contains(text, secret) {
			text = strings.ReplaceAll(text, secret, "***")
		}
	}
	lower := strings.ToLower(text)
	if strings.Contains(lower, "authorization") {
		text = bearerHeader.ReplaceAllString(text, "${1}***")
		lower = strings.ToLower(text)
	}
	for _, key := range assignmentKeys {
		if strings.Contains(lower, key) {
			text = credentialAssignment.ReplaceAllString(text, "${1}***")
			break
		}
	}
	if strings.Contains(text, "sk-") {
		text = skToken.ReplaceAllString(text, "sk-***")
	}
	return text
}

func Redact(value qtjson.Value, credentials []string) qtjson.Value {
	switch value.Kind() {
	case qtjson.String:
		return qtjson.NewString(RedactText(value.Str(), credentials))
	case qtjson.Array:
		out := qtjson.NewArray()
		for _, item := range value.Items() {
			out.Append(Redact(item, credentials))
		}
		return out
	case qtjson.Object:
		out := qtjson.NewObject()
		for _, member := range value.Members() {
			if sensitiveKeys[strings.ToLower(member.Key)] {
				out.Set(member.Key, qtjson.NewString("***"))
			} else {
				out.Set(member.Key, Redact(member.Value, credentials))
			}
		}
		return out
	}
	return value
}
