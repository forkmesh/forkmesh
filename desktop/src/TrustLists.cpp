#include "TrustLists.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

#include <openssl/bn.h>
#include <openssl/evp.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace forkmesh::trust {

namespace {

const QString kListSchema = QStringLiteral("forkmesh-trust-list-v1");
const QString kRevocationSchema = QStringLiteral("forkmesh-trust-list-revocation-v1");
const QString kAppealSchema = QStringLiteral("forkmesh-trust-list-appeal-v1");
const QString kResolutionSchema = QStringLiteral("forkmesh-trust-resolution-v1");
const QString kListIdDomain = QStringLiteral("forkmesh-trust-list-id-v1");

const QStringList kRoles = {
    QStringLiteral("maintainer"),
    QStringLiteral("moderator"),
    QStringLiteral("mirror-operator"),
    QStringLiteral("organization"),
    QStringLiteral("reviewer"),
    QStringLiteral("security-researcher"),
};
const QStringList kStances = {QStringLiteral("distrust"), QStringLiteral("trust")};
const QStringList kRevocationReasons = {
    QStringLiteral("compromised"), QStringLiteral("malicious"),
    QStringLiteral("retired"), QStringLiteral("superseded"),
};
const QStringList kListStates = {
    QStringLiteral("active"), QStringLiteral("conflicted"),
    QStringLiteral("expired"), QStringLiteral("missing"),
    QStringLiteral("recovered"), QStringLiteral("revoked"),
};

constexpr qint64 kSafeInteger = (qint64(1) << 53) - 1;
constexpr qsizetype kMaxDocumentBytes = 262144;
constexpr qsizetype kMaxEntries = 2000;
constexpr qsizetype kMaxEdges = 16;
constexpr qsizetype kMaxCommunities = 64;
constexpr qsizetype kMaxRevocationKeys = 4;
constexpr qsizetype kMaxNote = 280;
constexpr qsizetype kMaxStatement = 1000;
constexpr qint64 kMaxLifetimeMs = qint64(400) * 24 * 60 * 60 * 1000;
constexpr qsizetype kMaxPathLists = 4;
constexpr qint64 kFutureSkewMs = 300000;
// CPython refuses int() on more than 4300 decimal digits (a ValueError, so
// json.loads fails and the reference reports invalid_json).
constexpr qsizetype kMaxIntegerDigits = 4300;
// Deepest bracket nesting a document may have; checked on the raw text before
// any parser runs, so a deep document cannot exhaust the stack.
constexpr int kMaxNesting = 8;

const QSet<QString> kListKeys = {
    QStringLiteral("schema"), QStringLiteral("issuer"), QStringLiteral("name"),
    QStringLiteral("version"), QStringLiteral("previous"),
    QStringLiteral("issuedAt"), QStringLiteral("expiresAt"),
    QStringLiteral("roles"), QStringLiteral("communities"),
    QStringLiteral("revocationKeys"), QStringLiteral("entries"),
    QStringLiteral("edges"), QStringLiteral("signature"),
};
const QSet<QString> kRevocationKeys = {
    QStringLiteral("schema"), QStringLiteral("list"), QStringLiteral("issuer"),
    QStringLiteral("name"), QStringLiteral("fromVersion"),
    QStringLiteral("revokedBy"), QStringLiteral("revokedAt"),
    QStringLiteral("reason"), QStringLiteral("successor"),
    QStringLiteral("signature"),
};
const QSet<QString> kAppealKeys = {
    QStringLiteral("schema"), QStringLiteral("subject"), QStringLiteral("list"),
    QStringLiteral("version"), QStringLiteral("role"), QStringLiteral("scope"),
    QStringLiteral("statement"), QStringLiteral("issuedAt"),
    QStringLiteral("signature"),
};
const QSet<QString> kEntryKeys = {
    QStringLiteral("subject"), QStringLiteral("role"), QStringLiteral("scope"),
    QStringLiteral("stance"), QStringLiteral("note"),
};
const QSet<QString> kEdgeKeys = {
    QStringLiteral("kind"), QStringLiteral("issuer"), QStringLiteral("name"),
    QStringLiteral("roles"), QStringLiteral("communities"),
    QStringLiteral("expiresAt"),
};
const QSet<QString> kSuccessorKeys = {QStringLiteral("issuer"), QStringLiteral("name")};

bool fullMatch(const QString &pattern, const QString &value)
{
    // \A..\z: PCRE's $ would also match before a trailing newline, which
    // Python's fullmatch does not.
    thread_local QHash<QString, QRegularExpression> cache;
    auto it = cache.find(pattern);
    if (it == cache.end())
        it = cache.insert(pattern, QRegularExpression(
                                       QStringLiteral("\\A(?:") + pattern +
                                       QStringLiteral(")\\z")));
    return it->match(value).hasMatch();
}

bool isSlug(const QJsonValue &value)
{
    return value.isString() &&
           fullMatch(QStringLiteral("[a-z0-9][a-z0-9-]{0,63}"), value.toString());
}

bool isText(const QString &value)
{
    for (const QChar ch : value) {
        const char16_t unit = ch.unicode();
        if (unit < 0x20 || unit > 0x7e || unit == u'"' || unit == u'\\')
            return false;
    }
    return true;
}

// Deepest [ / { nesting outside strings, with the reference's lexing: inside
// a string a backslash skips the next character and a quote ends it.
int nestingDepth(const QString &raw)
{
    int depth = 0;
    int deepest = 0;
    bool inString = false;
    bool escaped = false;
    for (const QChar ch : raw) {
        const char16_t unit = ch.unicode();
        if (inString) {
            if (escaped)
                escaped = false;
            else if (unit == u'\\')
                escaped = true;
            else if (unit == u'"')
                inString = false;
        } else if (unit == u'"') {
            inString = true;
        } else if (unit == u'[' || unit == u'{') {
            deepest = std::max(deepest, ++depth);
        } else if (unit == u']' || unit == u'}') {
            --depth;
        }
    }
    return deepest;
}

QString sha256Hex(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString joinKey(const QStringList &parts)
{
    return parts.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Canonical encoding.

bool encodeCanonical(const QJsonValue &value, QString &out)
{
    switch (value.type()) {
    case QJsonValue::Null:
        out += QLatin1String("null");
        return true;
    case QJsonValue::Bool:
        out += value.toBool() ? QLatin1String("true") : QLatin1String("false");
        return true;
    case QJsonValue::Double: {
        const qint64 integer = value.toInteger();
        if (static_cast<double>(integer) != value.toDouble() ||
            integer > kSafeInteger || integer < -kSafeInteger)
            return false;
        out += QString::number(integer);
        return true;
    }
    case QJsonValue::String: {
        const QString text = value.toString();
        if (!isText(text))
            return false;
        out += QLatin1Char('"') + text + QLatin1Char('"');
        return true;
    }
    case QJsonValue::Array: {
        out += QLatin1Char('[');
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i > 0)
                out += QLatin1Char(',');
            if (!encodeCanonical(array.at(i), out))
                return false;
        }
        out += QLatin1Char(']');
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        out += QLatin1Char('{');
        bool first = true;
        for (const QString &key : std::as_const(keys)) {
            if (!isText(key))
                return false;
            if (!first)
                out += QLatin1Char(',');
            first = false;
            out += QLatin1Char('"') + key + QLatin1String("\":");
            if (!encodeCanonical(object.value(key), out))
                return false;
        }
        out += QLatin1Char('}');
        return true;
    }
    case QJsonValue::Undefined:
        break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A strict parser with json.loads' acceptance rules. QJsonDocument disagrees
// with Python at the edges (it refuses scalar top levels and overflowing
// numbers), and those edges decide between invalid_json, noncanonical, and
// unsupported_schema. Escapes, non-ASCII text, and non-integers can never
// survive the canonical comparison, so they only mark the parse noncanonical.

class StrictJsonParser
{
public:
    explicit StrictJsonParser(const QString &text) : m_text(text) {}

    bool parse(QJsonValue *out)
    {
        skipWhitespace();
        if (!parseValue(out, 0))
            return false;
        skipWhitespace();
        return m_pos == m_text.size();
    }

    bool noncanonical() const { return m_noncanonical; }

private:
    bool atEnd() const { return m_pos >= m_text.size(); }
    char16_t at(qsizetype index) const { return m_text.at(index).unicode(); }
    static bool isDigit(char16_t ch) { return ch >= u'0' && ch <= u'9'; }
    static bool isHex(char16_t ch)
    {
        return isDigit(ch) || (ch >= u'a' && ch <= u'f') || (ch >= u'A' && ch <= u'F');
    }

    void skipWhitespace()
    {
        while (!atEnd()) {
            const char16_t ch = at(m_pos);
            if (ch != u' ' && ch != u'\t' && ch != u'\n' && ch != u'\r')
                return;
            ++m_pos;
        }
    }

    bool literal(QLatin1String word, const QJsonValue &value, QJsonValue *out)
    {
        if (QStringView(m_text).mid(m_pos, word.size()) != word)
            return false;
        m_pos += word.size();
        *out = value;
        return true;
    }

    bool parseValue(QJsonValue *out, int depth)
    {
        if (depth > kMaxNesting || atEnd())
            return false;
        const char16_t ch = at(m_pos);
        switch (ch) {
        case u'"': {
            QString text;
            if (!parseString(&text))
                return false;
            *out = text;
            return true;
        }
        case u'{':
            return parseObject(out, depth + 1);
        case u'[':
            return parseArray(out, depth + 1);
        case u'n':
            return literal(QLatin1String("null"), QJsonValue(QJsonValue::Null), out);
        case u't':
            return literal(QLatin1String("true"), QJsonValue(true), out);
        case u'f':
            return literal(QLatin1String("false"), QJsonValue(false), out);
        default:
            // NaN / Infinity / -Infinity reach parse_constant, which raises.
            if (ch == u'-' || isDigit(ch))
                return parseNumber(out);
            return false;
        }
    }

    bool parseString(QString *out)
    {
        ++m_pos; // opening quote
        while (true) {
            if (atEnd())
                return false;
            const char16_t ch = at(m_pos);
            if (ch == u'"') {
                ++m_pos;
                return true;
            }
            if (ch == u'\\') {
                m_noncanonical = true;
                ++m_pos;
                if (atEnd())
                    return false;
                const char16_t escape = at(m_pos);
                if (escape == u'u') {
                    if (m_pos + 4 >= m_text.size())
                        return false;
                    for (qsizetype i = 1; i <= 4; ++i) {
                        if (!isHex(at(m_pos + i)))
                            return false;
                    }
                    m_pos += 5;
                    continue;
                }
                if (escape != u'"' && escape != u'\\' && escape != u'/' &&
                    escape != u'b' && escape != u'f' && escape != u'n' &&
                    escape != u'r' && escape != u't')
                    return false;
                ++m_pos;
                continue;
            }
            // strict=True: raw control characters are a decode error.
            if (ch < 0x20)
                return false;
            if (ch > 0x7e)
                m_noncanonical = true;
            out->append(QChar(ch));
            ++m_pos;
        }
    }

    bool parseNumber(QJsonValue *out)
    {
        const qsizetype start = m_pos;
        if (at(m_pos) == u'-')
            ++m_pos;
        const qsizetype digitsStart = m_pos;
        if (atEnd())
            return false;
        if (at(m_pos) == u'0') {
            ++m_pos;
        } else if (isDigit(at(m_pos))) {
            while (!atEnd() && isDigit(at(m_pos)))
                ++m_pos;
        } else {
            return false;
        }
        const qsizetype integerDigits = m_pos - digitsStart;
        bool isFloat = false;
        // A fraction or exponent without digits is not consumed; whatever
        // follows the integer part then fails the enclosing grammar.
        if (m_pos + 1 < m_text.size() && at(m_pos) == u'.' && isDigit(at(m_pos + 1))) {
            isFloat = true;
            m_pos += 2;
            while (!atEnd() && isDigit(at(m_pos)))
                ++m_pos;
        }
        if (!atEnd() && (at(m_pos) == u'e' || at(m_pos) == u'E')) {
            qsizetype probe = m_pos + 1;
            if (probe < m_text.size() && (at(probe) == u'+' || at(probe) == u'-'))
                ++probe;
            if (probe < m_text.size() && isDigit(at(probe))) {
                isFloat = true;
                m_pos = probe;
                while (!atEnd() && isDigit(at(m_pos)))
                    ++m_pos;
            }
        }
        if (isFloat) {
            m_noncanonical = true;
            *out = QJsonValue(0.5);
            return true;
        }
        if (integerDigits > kMaxIntegerDigits)
            return false;
        if (integerDigits > 16) {
            m_noncanonical = true;
            *out = QJsonValue(qint64(0));
            return true;
        }
        const qint64 value = QStringView(m_text).mid(start, m_pos - start).toLongLong();
        if (value > kSafeInteger || value < -kSafeInteger)
            m_noncanonical = true;
        *out = QJsonValue(value);
        return true;
    }

    bool parseObject(QJsonValue *out, int depth)
    {
        ++m_pos; // {
        QJsonObject object;
        skipWhitespace();
        if (!atEnd() && at(m_pos) == u'}') {
            ++m_pos;
            *out = object;
            return true;
        }
        while (true) {
            if (atEnd() || at(m_pos) != u'"')
                return false;
            QString key;
            if (!parseString(&key))
                return false;
            skipWhitespace();
            if (atEnd() || at(m_pos) != u':')
                return false;
            ++m_pos;
            skipWhitespace();
            QJsonValue value;
            if (!parseValue(&value, depth))
                return false;
            // Duplicate keys keep the last value, like a Python dict; the
            // canonical comparison rejects the document either way.
            object.insert(key, value);
            skipWhitespace();
            if (atEnd())
                return false;
            if (at(m_pos) == u'}') {
                ++m_pos;
                *out = object;
                return true;
            }
            if (at(m_pos) != u',')
                return false;
            ++m_pos;
            skipWhitespace();
        }
    }

    bool parseArray(QJsonValue *out, int depth)
    {
        ++m_pos; // [
        QJsonArray array;
        skipWhitespace();
        if (!atEnd() && at(m_pos) == u']') {
            ++m_pos;
            *out = array;
            return true;
        }
        while (true) {
            QJsonValue value;
            if (!parseValue(&value, depth))
                return false;
            array.append(value);
            skipWhitespace();
            if (atEnd())
                return false;
            if (at(m_pos) == u']') {
                ++m_pos;
                *out = array;
                return true;
            }
            if (at(m_pos) != u',')
                return false;
            ++m_pos;
            skipWhitespace();
        }
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    bool m_noncanonical = false;
};

// ---------------------------------------------------------------------------
// Scopes.

bool validScope(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.startsWith(QLatin1String("repo:"))) {
        const QString rest = text.mid(5);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        if (slash < 0)
            return false;
        const QString owner = rest.left(slash);
        const QString repo = rest.mid(slash + 1);
        return fullMatch(QStringLiteral("[a-z0-9][a-z0-9-]{0,38}"), owner) &&
               fullMatch(QStringLiteral("[A-Za-z0-9._-]{1,100}"), repo) &&
               repo != QLatin1String(".") && repo != QLatin1String("..");
    }
    if (text.startsWith(QLatin1String("org:")))
        return fullMatch(QStringLiteral("[a-z0-9][a-z0-9-]{0,38}"), text.mid(4));
    if (text.startsWith(QLatin1String("community:")))
        return fullMatch(QStringLiteral("[a-z0-9][a-z0-9-]{0,63}"), text.mid(10));
    return false;
}

bool covers(const QString &outer, const QString &inner)
{
    if (outer == inner)
        return true;
    return outer.startsWith(QLatin1String("org:")) &&
           inner.startsWith(QLatin1String("repo:") + outer.mid(4) + QLatin1Char('/'));
}

bool covered(const QStringList &scopes, const QString &scope)
{
    return std::any_of(scopes.cbegin(), scopes.cend(),
                       [&](const QString &outer) { return covers(outer, scope); });
}

QStringList sortedUniqueStrings(QStringList values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

QStringList normalizeScopes(const QStringList &scopes)
{
    const QStringList unique = sortedUniqueStrings(scopes);
    QStringList kept;
    for (const QString &scope : unique) {
        const bool shadowed = std::any_of(
            unique.cbegin(), unique.cend(), [&](const QString &outer) {
                return outer != scope && covers(outer, scope);
            });
        if (!shadowed)
            kept.append(scope);
    }
    return kept;
}

QStringList intersectScopes(const QStringList &left, const QStringList &right)
{
    QStringList kept;
    for (const QString &scope : left) {
        if (covered(right, scope))
            kept.append(scope);
    }
    for (const QString &scope : right) {
        if (covered(left, scope))
            kept.append(scope);
    }
    return normalizeScopes(kept);
}

QStringList intersectStrings(const QStringList &left, const QStringList &right)
{
    const QSet<QString> other(right.cbegin(), right.cend());
    QStringList kept;
    for (const QString &value : left) {
        if (other.contains(value))
            kept.append(value);
    }
    return sortedUniqueStrings(kept);
}

// Only string members: the local follow inputs are the only place a
// non-string can appear, and none can ever equal a role or cover a scope.
QStringList stringsOf(const QJsonValue &value)
{
    QStringList strings;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString())
            strings.append(item.toString());
    }
    return strings;
}

// ---------------------------------------------------------------------------
// Field checks.

bool safeInt(const QJsonValue &value, qint64 minimum = 0)
{
    if (!value.isDouble())
        return false;
    const qint64 integer = value.toInteger();
    return static_cast<double>(integer) == value.toDouble() &&
           integer >= minimum && integer <= kSafeInteger;
}

qint64 integerOf(const QJsonValue &value)
{
    return value.toInteger();
}

std::optional<QByteArray> base64UrlBytes(const QJsonValue &value, qsizetype textLength,
                                         const char *padding)
{
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    if (text.size() != textLength ||
        !fullMatch(QStringLiteral("[A-Za-z0-9_-]+"), text))
        return std::nullopt;
    const QByteArray ascii = text.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(
        ascii + padding,
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    if (decoded.decoded.toBase64(QByteArray::Base64UrlEncoding |
                                 QByteArray::OmitTrailingEquals) != ascii)
        return std::nullopt;
    return decoded.decoded;
}

std::optional<QByteArray> keyBytes(const QJsonValue &value)
{
    return base64UrlBytes(value, 43, "=");
}

std::optional<QByteArray> signatureBytes(const QJsonValue &value)
{
    return base64UrlBytes(value, 86, "==");
}

bool sortedUnique(const QJsonValue &values, const std::function<bool(const QJsonValue &)> &check,
                  qsizetype maximum, bool allowEmpty)
{
    if (!values.isArray())
        return false;
    const QJsonArray array = values.toArray();
    if (array.size() > maximum)
        return false;
    if (array.isEmpty() && !allowEmpty)
        return false;
    for (const QJsonValue &item : array) {
        if (!check(item))
            return false;
    }
    for (qsizetype i = 0; i + 1 < array.size(); ++i) {
        if (!(array.at(i).toString() < array.at(i + 1).toString()))
            return false;
    }
    return true;
}

bool isRole(const QJsonValue &value)
{
    return value.isString() && kRoles.contains(value.toString());
}

bool validRoles(const QJsonValue &values)
{
    return sortedUnique(values, isRole, kRoles.size(), false);
}

bool validCommunities(const QJsonValue &values)
{
    if (!sortedUnique(values, [](const QJsonValue &v) { return validScope(v); },
                      kMaxCommunities, false))
        return false;
    const QStringList scopes = stringsOf(values);
    return normalizeScopes(scopes) == scopes;
}

bool keysEqual(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

// ---------------------------------------------------------------------------
// Ed25519 with the reference verifier's exact acceptance rules
// (collaboration.ed25519_verify): points decode strictly (y < p, on the curve,
// no x = 0 with the sign bit set), the public key must not be the identity and
// must lie in the prime-order subgroup, and [S]B == R + [k]A in affine
// coordinates. OpenSSL performs none of the key checks, so they run first with
// BIGNUM arithmetic; OpenSSL then answers every signature whose R is outside
// the small-order torsion, and the rare small-order R takes the explicit
// equation the reference evaluates.

struct BnFree {
    void operator()(BIGNUM *value) const { BN_free(value); }
};
using Bn = std::unique_ptr<BIGNUM, BnFree>;

struct CtxFree {
    void operator()(BN_CTX *ctx) const { BN_CTX_free(ctx); }
};

// Little-endian 2^252 + 27742317777372353535851937790883648493.
const unsigned char kGroupOrderLe[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
    0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

bool scalarBelowGroupOrder(const QByteArray &scalarLe)
{
    for (int i = 31; i >= 0; --i) {
        const unsigned char byte = static_cast<unsigned char>(scalarLe.at(i));
        if (byte != kGroupOrderLe[i])
            return byte < kGroupOrderLe[i];
    }
    return false;
}

class Edwards
{
public:
    struct Affine {
        Bn x;
        Bn y;
    };
    struct Extended {
        Bn x;
        Bn y;
        Bn z;
        Bn t;
    };

    Edwards() : m_ctx(BN_CTX_new())
    {
        m_p = fresh();
        BN_set_bit(m_p.get(), 255);
        BN_sub_word(m_p.get(), 19);
        m_pMinusTwo = copy(m_p.get());
        BN_sub_word(m_pMinusTwo.get(), 2);
        m_one = word(1);
        m_two = word(2);
        const Bn negative = sub(word(0).get(), word(121665).get());
        m_d = mul(negative.get(), pow(word(121666).get(), m_pMinusTwo.get()).get());
        m_twoD = add(m_d.get(), m_d.get());
        Bn quarter = copy(m_p.get());
        BN_sub_word(quarter.get(), 1);
        BN_rshift(quarter.get(), quarter.get(), 2);
        m_i = pow(m_two.get(), quarter.get());
        m_sqrtExponent = copy(m_p.get());
        BN_add_word(m_sqrtExponent.get(), 3);
        BN_rshift(m_sqrtExponent.get(), m_sqrtExponent.get(), 3);
        m_l.reset(BN_lebin2bn(kGroupOrderLe, 32, nullptr));
        const Bn baseY = mul(word(4).get(), pow(word(5).get(), m_pMinusTwo.get()).get());
        m_baseX = xrecover(baseY.get());
        m_baseY = copy(baseY.get());
    }

    bool verify(const QByteArray &publicKey, const QByteArray &signature,
                const QByteArray &message, const std::function<bool()> &openssl)
    {
        if (publicKey.size() != 32 || signature.size() != 64 || !m_ctx)
            return false;
        const QByteArray nonceBytes = signature.left(32);
        const QByteArray scalarBytes = signature.mid(32);
        const std::optional<Affine> publicPoint = decode(publicKey);
        const std::optional<Affine> nonce = decode(nonceBytes);
        if (!publicPoint || !nonce || !scalarBelowGroupOrder(scalarBytes))
            return false;
        // Identity, small-order, and mixed-order public keys never verify,
        // however the signature was built.
        if (BN_is_zero(publicPoint->x.get()) && BN_is_one(publicPoint->y.get()))
            return false;
        if (!isIdentity(multiply(extended(*publicPoint), m_l.get())))
            return false;
        // With a prime-order A, [S]B - [k]A lies in the prime-order subgroup,
        // so OpenSSL's byte comparison against R agrees with the reference's
        // point equation unless R itself has small order.
        if (!isIdentity(multiply(extended(*nonce), word(8).get())))
            return openssl();

        const QByteArray digest = QCryptographicHash::hash(
            nonceBytes + publicKey + message, QCryptographicHash::Sha512);
        Bn challenge(BN_lebin2bn(reinterpret_cast<const unsigned char *>(digest.constData()),
                                 int(digest.size()), nullptr));
        BN_nnmod(challenge.get(), challenge.get(), m_l.get(), m_ctx.get());
        const Bn scalar(BN_lebin2bn(
            reinterpret_cast<const unsigned char *>(scalarBytes.constData()), 32, nullptr));
        const Affine base{copy(m_baseX.get()), copy(m_baseY.get())};
        const Affine left = affine(multiply(extended(base), scalar.get()));
        const Affine right = affine(addPoints(
            extended(*nonce), multiply(extended(*publicPoint), challenge.get())));
        return BN_cmp(left.x.get(), right.x.get()) == 0 &&
               BN_cmp(left.y.get(), right.y.get()) == 0;
    }

private:
    Bn fresh() const { return Bn(BN_new()); }
    Bn copy(const BIGNUM *value) const { return Bn(BN_dup(value)); }
    Bn word(BN_ULONG value) const
    {
        Bn result = fresh();
        BN_set_word(result.get(), value);
        return result;
    }
    Bn mul(const BIGNUM *a, const BIGNUM *b) const
    {
        Bn result = fresh();
        BN_mod_mul(result.get(), a, b, m_p.get(), m_ctx.get());
        return result;
    }
    Bn add(const BIGNUM *a, const BIGNUM *b) const
    {
        Bn result = fresh();
        BN_mod_add(result.get(), a, b, m_p.get(), m_ctx.get());
        return result;
    }
    Bn sub(const BIGNUM *a, const BIGNUM *b) const
    {
        Bn result = fresh();
        BN_mod_sub(result.get(), a, b, m_p.get(), m_ctx.get());
        return result;
    }
    Bn pow(const BIGNUM *a, const BIGNUM *exponent) const
    {
        Bn result = fresh();
        BN_mod_exp(result.get(), a, exponent, m_p.get(), m_ctx.get());
        return result;
    }

    Bn xrecover(const BIGNUM *y) const
    {
        const Bn yy = mul(y, y);
        const Bn numerator = sub(yy.get(), m_one.get());
        const Bn denominator = add(mul(m_d.get(), yy.get()).get(), m_one.get());
        const Bn xx = mul(numerator.get(), pow(denominator.get(), m_pMinusTwo.get()).get());
        Bn x = pow(xx.get(), m_sqrtExponent.get());
        if (!BN_is_zero(sub(mul(x.get(), x.get()).get(), xx.get()).get()))
            x = mul(x.get(), m_i.get());
        if (BN_is_odd(x.get())) {
            Bn negated = fresh();
            BN_sub(negated.get(), m_p.get(), x.get());
            x = std::move(negated);
        }
        return x;
    }

    std::optional<Affine> decode(const QByteArray &raw) const
    {
        if (raw.size() != 32)
            return std::nullopt;
        QByteArray low = raw;
        const int sign = (static_cast<unsigned char>(raw.at(31)) >> 7) & 1;
        low[31] = char(static_cast<unsigned char>(raw.at(31)) & 0x7f);
        Bn y(BN_lebin2bn(reinterpret_cast<const unsigned char *>(low.constData()), 32, nullptr));
        if (BN_cmp(y.get(), m_p.get()) >= 0)
            return std::nullopt;
        Bn x = xrecover(y.get());
        // x = 0 has no negative; with the sign bit set, q - 0 would slip an
        // unreduced identity past the identity and subgroup checks.
        if (BN_is_zero(x.get()) && sign)
            return std::nullopt;
        if ((BN_is_odd(x.get()) ? 1 : 0) != sign) {
            Bn negated = fresh();
            BN_sub(negated.get(), m_p.get(), x.get());
            x = std::move(negated);
        }
        const Bn xx = mul(x.get(), x.get());
        const Bn yy = mul(y.get(), y.get());
        Bn curve = sub(yy.get(), xx.get());
        curve = sub(curve.get(), m_one.get());
        curve = sub(curve.get(), mul(m_d.get(), mul(xx.get(), yy.get()).get()).get());
        if (!BN_is_zero(curve.get()))
            return std::nullopt;
        QByteArray encoded(32, '\0');
        BN_bn2lebinpad(y.get(), reinterpret_cast<unsigned char *>(encoded.data()), 32);
        if (BN_is_odd(x.get()))
            encoded[31] = char(static_cast<unsigned char>(encoded.at(31)) | 0x80);
        if (encoded != raw)
            return std::nullopt;
        return Affine{std::move(x), std::move(y)};
    }

    Extended extended(const Affine &point) const
    {
        return Extended{copy(point.x.get()), copy(point.y.get()), copy(m_one.get()),
                        mul(point.x.get(), point.y.get())};
    }

    Extended addPoints(const Extended &left, const Extended &right) const
    {
        const Bn a = mul(sub(left.y.get(), left.x.get()).get(),
                         sub(right.y.get(), right.x.get()).get());
        const Bn b = mul(add(left.y.get(), left.x.get()).get(),
                         add(right.y.get(), right.x.get()).get());
        const Bn c = mul(mul(m_twoD.get(), left.t.get()).get(), right.t.get());
        const Bn d = mul(mul(m_two.get(), left.z.get()).get(), right.z.get());
        const Bn e = sub(b.get(), a.get());
        const Bn f = sub(d.get(), c.get());
        const Bn g = add(d.get(), c.get());
        const Bn h = add(b.get(), a.get());
        return Extended{mul(e.get(), f.get()), mul(g.get(), h.get()),
                        mul(f.get(), g.get()), mul(e.get(), h.get())};
    }

    Extended multiply(const Extended &point, const BIGNUM *scalar) const
    {
        Extended result{word(0), word(1), word(1), word(0)};
        Extended addend{copy(point.x.get()), copy(point.y.get()), copy(point.z.get()),
                        copy(point.t.get())};
        const int bits = BN_num_bits(scalar);
        for (int i = 0; i < bits; ++i) {
            if (BN_is_bit_set(scalar, i))
                result = addPoints(result, addend);
            addend = addPoints(addend, addend);
        }
        return result;
    }

    Affine affine(const Extended &point) const
    {
        const Bn inverse = pow(point.z.get(), m_pMinusTwo.get());
        return Affine{mul(point.x.get(), inverse.get()), mul(point.y.get(), inverse.get())};
    }

    bool isIdentity(const Extended &point) const
    {
        const Affine reduced = affine(point);
        return BN_is_zero(reduced.x.get()) && BN_is_one(reduced.y.get());
    }

    std::unique_ptr<BN_CTX, CtxFree> m_ctx;
    Bn m_p, m_pMinusTwo, m_one, m_two, m_d, m_twoD, m_i, m_sqrtExponent, m_l;
    Bn m_baseX, m_baseY;
};

bool opensslVerify(const QByteArray &publicKey, const QByteArray &signature,
                   const QByteArray &message)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        size_t(publicKey.size()));
    if (!key)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = EVP_DigestVerify(
                 ctx, reinterpret_cast<const unsigned char *>(signature.constData()),
                 size_t(signature.size()),
                 reinterpret_cast<const unsigned char *>(message.constData()),
                 size_t(message.size())) == 1;
    }
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

bool ed25519Verify(const QByteArray &publicKey, const QByteArray &signature,
                   const QByteArray &message)
{
    thread_local Edwards curve;
    return curve.verify(publicKey, signature, message, [&] {
        return opensslVerify(publicKey, signature, message);
    });
}

QString signingText(const QJsonObject &document)
{
    QJsonObject unsigned_ = document;
    unsigned_.remove(QStringLiteral("signature"));
    return document.value(QStringLiteral("schema")).toString() + QLatin1Char('\n') +
           canonicalJson(unsigned_);
}

QString verifySignature(const QJsonObject &document, const QJsonValue &signer)
{
    const std::optional<QByteArray> signature =
        signatureBytes(document.value(QStringLiteral("signature")));
    if (!signature)
        return QStringLiteral("invalid_signature_encoding");
    if (!scalarBelowGroupOrder(signature->mid(32)))
        return QStringLiteral("invalid_signature");
    const std::optional<QByteArray> publicKey = keyBytes(signer);
    const QByteArray message = signingText(document).toLatin1();
    if (!publicKey || !ed25519Verify(*publicKey, *signature, message))
        return QStringLiteral("invalid_signature");
    return {};
}

// ---------------------------------------------------------------------------
// Document validation; the first failing check names the rejection.

QString validateList(const QJsonObject &doc)
{
    const QJsonValue issuer = doc.value(QStringLiteral("issuer"));
    const QJsonValue name = doc.value(QStringLiteral("name"));
    if (!keyBytes(issuer))
        return QStringLiteral("invalid_issuer");
    if (!isSlug(name))
        return QStringLiteral("invalid_name");
    const QJsonValue version = doc.value(QStringLiteral("version"));
    if (!safeInt(version, 1))
        return QStringLiteral("invalid_version");
    const QJsonValue previous = doc.value(QStringLiteral("previous"));
    if (integerOf(version) == 1) {
        if (!previous.isNull())
            return QStringLiteral("invalid_previous");
    } else if (!previous.isString() ||
               !fullMatch(QStringLiteral("sha256:[0-9a-f]{64}"), previous.toString())) {
        return QStringLiteral("invalid_previous");
    }
    const QJsonValue issued = doc.value(QStringLiteral("issuedAt"));
    const QJsonValue expires = doc.value(QStringLiteral("expiresAt"));
    if (!safeInt(issued) || !safeInt(expires) ||
        integerOf(expires) <= integerOf(issued) ||
        integerOf(expires) - integerOf(issued) > kMaxLifetimeMs)
        return QStringLiteral("invalid_time");
    if (!validRoles(doc.value(QStringLiteral("roles"))) ||
        !validCommunities(doc.value(QStringLiteral("communities"))))
        return QStringLiteral("invalid_scope");
    const QJsonValue revocationKeys = doc.value(QStringLiteral("revocationKeys"));
    if (!sortedUnique(revocationKeys,
                      [](const QJsonValue &key) { return keyBytes(key).has_value(); },
                      kMaxRevocationKeys, true) ||
        stringsOf(revocationKeys).contains(issuer.toString()))
        return QStringLiteral("invalid_revocation_keys");

    const QString issuerText = issuer.toString();
    const QString nameText = name.toString();
    const QStringList roles = stringsOf(doc.value(QStringLiteral("roles")));
    const QStringList communities = stringsOf(doc.value(QStringLiteral("communities")));

    const QJsonValue entries = doc.value(QStringLiteral("entries"));
    if (!entries.isArray() || entries.toArray().size() > kMaxEntries)
        return QStringLiteral("invalid_entry");
    std::optional<QString> previousKey;
    for (const QJsonValue &item : entries.toArray()) {
        const QJsonObject entry = item.toObject();
        const QJsonValue subject = entry.value(QStringLiteral("subject"));
        const QJsonValue role = entry.value(QStringLiteral("role"));
        const QJsonValue scope = entry.value(QStringLiteral("scope"));
        const QJsonValue stance = entry.value(QStringLiteral("stance"));
        const QJsonValue note = entry.value(QStringLiteral("note"));
        if (!item.isObject() || !keysEqual(entry, kEntryKeys) || !keyBytes(subject) ||
            subject.toString() == issuerText || !isRole(role) || !validScope(scope) ||
            !(stance.isString() && kStances.contains(stance.toString())) ||
            !note.isString() || note.toString().size() > kMaxNote)
            return QStringLiteral("invalid_entry");
        if (!roles.contains(role.toString()) || !covered(communities, scope.toString()))
            return QStringLiteral("entry_outside_scope");
        const QString key = joinKey({subject.toString(), role.toString(), scope.toString()});
        if (previousKey && key <= *previousKey)
            return QStringLiteral("unsorted_entries");
        previousKey = key;
    }

    const QJsonValue edges = doc.value(QStringLiteral("edges"));
    if (!edges.isArray() || edges.toArray().size() > kMaxEdges)
        return QStringLiteral("invalid_edge");
    previousKey.reset();
    for (const QJsonValue &item : edges.toArray()) {
        const QJsonObject edge = item.toObject();
        const QJsonValue kind = edge.value(QStringLiteral("kind"));
        const QJsonValue edgeIssuer = edge.value(QStringLiteral("issuer"));
        const QJsonValue edgeName = edge.value(QStringLiteral("name"));
        const QJsonValue expiresAt = edge.value(QStringLiteral("expiresAt"));
        const bool delegate = kind == QJsonValue(QStringLiteral("delegate"));
        const bool include = kind == QJsonValue(QStringLiteral("include"));
        if (!item.isObject() || !keysEqual(edge, kEdgeKeys) || !(delegate || include) ||
            !keyBytes(edgeIssuer) || !isSlug(edgeName) ||
            !validRoles(edge.value(QStringLiteral("roles"))) ||
            !validCommunities(edge.value(QStringLiteral("communities"))) ||
            (delegate && !safeInt(expiresAt)) || (include && !expiresAt.isNull()) ||
            (edgeIssuer.toString() == issuerText && edgeName.toString() == nameText))
            return QStringLiteral("invalid_edge");
        const QStringList edgeRoles = stringsOf(edge.value(QStringLiteral("roles")));
        const QStringList edgeCommunities =
            stringsOf(edge.value(QStringLiteral("communities")));
        const bool rolesInside = std::all_of(
            edgeRoles.cbegin(), edgeRoles.cend(),
            [&](const QString &role) { return roles.contains(role); });
        const bool communitiesInside = std::all_of(
            edgeCommunities.cbegin(), edgeCommunities.cend(),
            [&](const QString &scope) { return covered(communities, scope); });
        if (!rolesInside || !communitiesInside)
            return QStringLiteral("edge_outside_scope");
        const QString key = joinKey({edgeIssuer.toString(), edgeName.toString()});
        if (previousKey && key <= *previousKey)
            return QStringLiteral("unsorted_edges");
        previousKey = key;
    }

    return verifySignature(doc, issuer);
}

QString validateRevocation(const QJsonObject &doc)
{
    const QJsonValue issuer = doc.value(QStringLiteral("issuer"));
    const QJsonValue name = doc.value(QStringLiteral("name"));
    if (!keyBytes(issuer))
        return QStringLiteral("invalid_issuer");
    if (!isSlug(name))
        return QStringLiteral("invalid_name");
    const QJsonValue list = doc.value(QStringLiteral("list"));
    if (!(list.isString() && list.toString() == listId(issuer.toString(), name.toString())))
        return QStringLiteral("list_mismatch");
    if (!safeInt(doc.value(QStringLiteral("fromVersion")), 1))
        return QStringLiteral("invalid_version");
    const QJsonValue revokedBy = doc.value(QStringLiteral("revokedBy"));
    if (!keyBytes(revokedBy))
        return QStringLiteral("invalid_revoker");
    if (!safeInt(doc.value(QStringLiteral("revokedAt"))))
        return QStringLiteral("invalid_time");
    const QJsonValue reason = doc.value(QStringLiteral("reason"));
    if (!(reason.isString() && kRevocationReasons.contains(reason.toString())))
        return QStringLiteral("invalid_reason");
    const QJsonValue successor = doc.value(QStringLiteral("successor"));
    if (!successor.isNull()) {
        const QJsonObject next = successor.toObject();
        const QJsonValue nextIssuer = next.value(QStringLiteral("issuer"));
        const QJsonValue nextName = next.value(QStringLiteral("name"));
        if (!successor.isObject() || !keysEqual(next, kSuccessorKeys) ||
            !keyBytes(nextIssuer) || !isSlug(nextName) ||
            (nextIssuer.toString() == issuer.toString() &&
             nextName.toString() == name.toString()))
            return QStringLiteral("invalid_successor");
    }
    return verifySignature(doc, revokedBy);
}

QString validateAppeal(const QJsonObject &doc)
{
    const QJsonValue subject = doc.value(QStringLiteral("subject"));
    if (!keyBytes(subject))
        return QStringLiteral("invalid_subject");
    const QJsonValue list = doc.value(QStringLiteral("list"));
    if (!list.isString() ||
        !fullMatch(QStringLiteral("urn:forkmesh:trust-list:sha256:[0-9a-f]{64}"),
                   list.toString()))
        return QStringLiteral("invalid_list");
    if (!safeInt(doc.value(QStringLiteral("version")), 1))
        return QStringLiteral("invalid_version");
    if (!isRole(doc.value(QStringLiteral("role"))) ||
        !validScope(doc.value(QStringLiteral("scope"))))
        return QStringLiteral("invalid_scope");
    const QJsonValue statement = doc.value(QStringLiteral("statement"));
    if (!statement.isString() || statement.toString().isEmpty() ||
        statement.toString().size() > kMaxStatement)
        return QStringLiteral("invalid_statement");
    if (!safeInt(doc.value(QStringLiteral("issuedAt"))))
        return QStringLiteral("invalid_time");
    return verifySignature(doc, subject);
}

// ---------------------------------------------------------------------------
// Resolution.

struct Document {
    QString id;
    QJsonObject body;

    QString text(const char *key) const { return body.value(QLatin1String(key)).toString(); }
    qint64 integer(const char *key) const { return body.value(QLatin1String(key)).toInteger(); }
};

struct GroupState {
    QString state;
    bool hasHead = false;
    QJsonObject head;
    QString headId;
    std::optional<qint64> cutoff;
    QStringList reasons;
    QJsonValue successor = QJsonValue(QJsonValue::Null);
    QJsonArray conflicts;
    QJsonArray heldVersions;
};

GroupState listGroupState(const std::vector<Document> &docs,
                          const std::vector<Document> &revocations, qint64 now)
{
    std::map<qint64, std::vector<const Document *>> byVersion;
    for (const Document &doc : docs)
        byVersion[doc.integer("version")].push_back(&doc);
    std::vector<qint64> cutoffs;
    std::set<QString> reasons;
    GroupState group;
    const QString lid = docs.empty()
                            ? QString()
                            : listId(docs.front().text("issuer"), docs.front().text("name"));
    for (const auto &[version, held] : byVersion) {
        if (held.size() > 1) {
            cutoffs.push_back(version);
            reasons.insert(QStringLiteral("equivocation"));
            group.conflicts.append(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("equivocation")},
                {QStringLiteral("listId"), lid},
                {QStringLiteral("version"), version},
            });
            continue;
        }
        const auto prior = byVersion.find(version - 1);
        if (prior != byVersion.end() && prior->second.size() == 1) {
            const QJsonValue previous = held.front()->body.value(QStringLiteral("previous"));
            if (!(previous.isString() && previous.toString() == prior->second.front()->id)) {
                cutoffs.push_back(version);
                reasons.insert(QStringLiteral("history_fork"));
                group.conflicts.append(QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("history_fork")},
                    {QStringLiteral("listId"), lid},
                    {QStringLiteral("version"), version},
                });
            }
        }
    }
    if (!revocations.empty()) {
        reasons.insert(QStringLiteral("revoked"));
        std::vector<const Document *> ordered;
        for (const Document &revocation : revocations)
            ordered.push_back(&revocation);
        std::sort(ordered.begin(), ordered.end(), [](const Document *a, const Document *b) {
            const qint64 left = a->integer("fromVersion");
            const qint64 right = b->integer("fromVersion");
            return left != right ? left < right : a->id < b->id;
        });
        cutoffs.push_back(ordered.front()->integer("fromVersion"));
        for (const Document *revocation : ordered) {
            const QJsonValue successor = revocation->body.value(QStringLiteral("successor"));
            if (!successor.isNull()) {
                group.successor = successor.toObject();
                break;
            }
        }
    }
    if (!cutoffs.empty())
        group.cutoff = *std::min_element(cutoffs.begin(), cutoffs.end());
    for (auto it = byVersion.rbegin(); it != byVersion.rend(); ++it) {
        if (group.cutoff && it->first >= *group.cutoff)
            continue;
        if (it->second.size() == 1) {
            group.hasHead = true;
            group.head = it->second.front()->body;
            group.headId = it->second.front()->id;
            break;
        }
    }
    if (!group.hasHead) {
        group.state = !revocations.empty() ? QStringLiteral("revoked")
                      : !docs.empty()      ? QStringLiteral("conflicted")
                                           : QStringLiteral("missing");
    } else if (now >= group.head.value(QStringLiteral("expiresAt")).toInteger()) {
        group.state = QStringLiteral("expired");
    } else if (group.cutoff) {
        group.state = QStringLiteral("recovered");
    } else {
        group.state = QStringLiteral("active");
    }
    group.reasons = QStringList(reasons.begin(), reasons.end());
    for (const auto &entry : byVersion)
        group.heldVersions.append(entry.first);
    return group;
}

struct Candidate {
    QString listId;
    qint64 version = 0;
    QString stance;
    QString note;
    QStringList path;
};

// Python orders str by code point; UTF-8 byte order is the same order, while
// QString's UTF-16 order is not once local override text leaves the BMP.
bool codePointLess(const QString &left, const QString &right)
{
    return left.toUtf8() < right.toUtf8();
}

class Resolver
{
public:
    explicit Resolver(qint64 now) : m_now(now) {}

    QJsonObject run(const QStringList &documents, const QJsonArray &follows,
                    const QJsonArray &overrides);

private:
    const GroupState &group(const QString &lid);
    const QJsonObject *usable(const QString &lid);
    void admit(const QString &lid, const QStringList &path, const QStringList &roles,
               const QStringList &communities);

    qint64 m_now;
    std::map<QString, std::vector<Document>> m_docsByList;
    std::map<QString, std::vector<Document>> m_revocationsByList;
    std::map<QString, std::pair<QString, QString>> m_names;
    std::map<QString, GroupState> m_groups;
    std::set<QString> m_followed;
    std::set<QString> m_reached;
    qint64 m_expiredDelegations = 0;
    qint64 m_depthLimited = 0;
    qint64 m_cycles = 0;
    // tuple key -> listId -> best (shortest, then lexically first) path.
    std::map<QString, std::map<QString, Candidate>> m_sources;
};

const GroupState &Resolver::group(const QString &lid)
{
    auto found = m_groups.find(lid);
    if (found != m_groups.end())
        return found->second;
    static const std::vector<Document> none;
    const auto docs = m_docsByList.find(lid);
    const auto revocations = m_revocationsByList.find(lid);
    GroupState state = listGroupState(
        docs != m_docsByList.end() ? docs->second : none,
        revocations != m_revocationsByList.end() ? revocations->second : none, m_now);
    return m_groups.emplace(lid, std::move(state)).first->second;
}

const QJsonObject *Resolver::usable(const QString &lid)
{
    const GroupState &info = group(lid);
    if (info.state == QLatin1String("active") || info.state == QLatin1String("recovered"))
        return &info.head;
    return nullptr;
}

void Resolver::admit(const QString &lid, const QStringList &path, const QStringList &roles,
                     const QStringList &communities)
{
    m_reached.insert(lid);
    const QJsonObject *head = usable(lid);
    if (!head)
        return;
    const qint64 version = head->value(QStringLiteral("version")).toInteger();
    const QString joinedPath = joinKey(path);
    for (const QJsonValue &item : head->value(QStringLiteral("entries")).toArray()) {
        const QJsonObject entry = item.toObject();
        const QString role = entry.value(QStringLiteral("role")).toString();
        const QString scope = entry.value(QStringLiteral("scope")).toString();
        if (!roles.contains(role) || !covered(communities, scope))
            continue;
        const QString tupleKey =
            joinKey({entry.value(QStringLiteral("subject")).toString(), role, scope});
        std::map<QString, Candidate> &perList = m_sources[tupleKey];
        const auto current = perList.find(lid);
        if (current == perList.end() ||
            std::make_pair(path.size(), joinedPath) <
                std::make_pair(current->second.path.size(), joinKey(current->second.path))) {
            perList[lid] = Candidate{lid, version,
                                     entry.value(QStringLiteral("stance")).toString(),
                                     entry.value(QStringLiteral("note")).toString(), path};
        }
    }
    for (const QJsonValue &item : head->value(QStringLiteral("edges")).toArray()) {
        const QJsonObject edge = item.toObject();
        if (edge.value(QStringLiteral("kind")).toString() == QLatin1String("delegate") &&
            m_now >= edge.value(QStringLiteral("expiresAt")).toInteger()) {
            ++m_expiredDelegations;
            continue;
        }
        const QString issuer = edge.value(QStringLiteral("issuer")).toString();
        const QString name = edge.value(QStringLiteral("name")).toString();
        const QString child = listId(issuer, name);
        m_names.try_emplace(child, issuer, name);
        if (path.contains(child)) {
            ++m_cycles;
            continue;
        }
        if (path.size() >= kMaxPathLists) {
            ++m_depthLimited;
            continue;
        }
        QStringList childRoles =
            intersectStrings(roles, stringsOf(edge.value(QStringLiteral("roles"))));
        QStringList childCommunities = intersectScopes(
            communities, stringsOf(edge.value(QStringLiteral("communities"))));
        if (const QJsonObject *childHead = usable(child)) {
            childRoles = intersectStrings(
                childRoles, stringsOf(childHead->value(QStringLiteral("roles"))));
            childCommunities = intersectScopes(
                childCommunities, stringsOf(childHead->value(QStringLiteral("communities"))));
        }
        if (childRoles.isEmpty() || childCommunities.isEmpty()) {
            m_reached.insert(child);
            continue;
        }
        admit(child, path + QStringList{child}, childRoles, childCommunities);
    }
}

QJsonObject Resolver::run(const QStringList &documents, const QJsonArray &follows,
                          const QJsonArray &overrides)
{
    std::map<QString, QString> unique;
    for (const QString &raw : documents)
        unique.try_emplace(documentId(raw), raw);
    const qint64 duplicates = qint64(documents.size()) - qint64(unique.size());

    std::vector<std::pair<QString, QString>> rejected; // documentId, code
    std::vector<Document> lists;
    std::vector<Document> revocations;
    std::vector<Document> appeals;
    for (const auto &[docId, raw] : unique) {
        QJsonObject body;
        const QString code = verifyDocument(raw, &body);
        if (!code.isEmpty()) {
            rejected.emplace_back(docId, code);
            continue;
        }
        const QString schema = body.value(QStringLiteral("schema")).toString();
        if (schema != kRevocationSchema &&
            body.value(QStringLiteral("issuedAt")).toInteger() > m_now + kFutureSkewMs)
            rejected.emplace_back(docId, QStringLiteral("issued_in_future"));
        else if (schema == kListSchema)
            lists.push_back({docId, body});
        else if (schema == kRevocationSchema)
            revocations.push_back({docId, body});
        else
            appeals.push_back({docId, body});
    }

    for (const Document &doc : lists) {
        const QString lid = listId(doc.text("issuer"), doc.text("name"));
        m_docsByList[lid].push_back(doc);
        m_names.insert_or_assign(lid, std::make_pair(doc.text("issuer"), doc.text("name")));
    }

    qint64 acceptedRevocations = 0;
    for (const Document &revocation : revocations) {
        const QString lid = revocation.text("list");
        const QString revokedBy = revocation.text("revokedBy");
        bool authorized = revokedBy == revocation.text("issuer");
        if (!authorized) {
            const auto docs = m_docsByList.find(lid);
            if (docs != m_docsByList.end()) {
                authorized = std::any_of(
                    docs->second.cbegin(), docs->second.cend(), [&](const Document &doc) {
                        return doc.integer("version") < revocation.integer("fromVersion") &&
                               stringsOf(doc.body.value(QStringLiteral("revocationKeys")))
                                   .contains(revokedBy);
                    });
            }
        }
        if (!authorized) {
            rejected.emplace_back(revocation.id, QStringLiteral("unauthorized_revoker"));
            continue;
        }
        m_revocationsByList[lid].push_back(revocation);
        ++acceptedRevocations;
        m_names.try_emplace(lid, revocation.text("issuer"), revocation.text("name"));
    }

    for (const auto &entry : m_docsByList)
        group(entry.first);
    for (const auto &entry : m_revocationsByList)
        group(entry.first);

    std::vector<std::pair<QString, QJsonObject>> followRoots;
    for (const QJsonValue &item : follows) {
        const QJsonObject follow = item.toObject();
        const QString issuer = follow.value(QStringLiteral("issuer")).toString();
        const QString name = follow.value(QStringLiteral("name")).toString();
        const QString lid = listId(issuer, name);
        m_names.try_emplace(lid, issuer, name);
        followRoots.emplace_back(lid, follow);
    }
    std::stable_sort(followRoots.begin(), followRoots.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[lid, follow] : followRoots) {
        m_followed.insert(lid);
        const QJsonObject *head = usable(lid);
        if (!head) {
            m_reached.insert(lid);
            continue;
        }
        QStringList roles = stringsOf(head->value(QStringLiteral("roles")));
        QStringList communities = stringsOf(head->value(QStringLiteral("communities")));
        const QJsonValue followRoles = follow.value(QStringLiteral("roles"));
        const QJsonValue followCommunities = follow.value(QStringLiteral("communities"));
        if (!followRoles.isNull() && !followRoles.isUndefined())
            roles = intersectStrings(roles, stringsOf(followRoles));
        if (!followCommunities.isNull() && !followCommunities.isUndefined())
            communities = intersectScopes(communities, stringsOf(followCommunities));
        if (!roles.isEmpty() && !communities.isEmpty())
            admit(lid, QStringList{lid}, roles, communities);
        else
            m_reached.insert(lid);
    }

    // Later overrides of the same tuple win, as in a dict comprehension.
    std::map<QString, QJsonValue> overrideMap;
    for (const QJsonValue &item : overrides) {
        const QJsonObject override_ = item.toObject();
        const QString key = joinKey({override_.value(QStringLiteral("subject")).toString(),
                                     override_.value(QStringLiteral("role")).toString(),
                                     override_.value(QStringLiteral("scope")).toString()});
        QJsonValue stance = override_.value(QStringLiteral("stance"));
        if (stance.isUndefined())
            stance = QJsonValue(QJsonValue::Null);
        overrideMap.insert_or_assign(key, stance);
    }

    struct Assertion {
        QString subject;
        QString role;
        QString scope;
        QString stance;
        QJsonValue override_;
        QJsonArray sources;
        QJsonArray appeals;
    };
    std::vector<QString> tupleKeys;
    for (const auto &entry : m_sources)
        tupleKeys.push_back(entry.first);
    for (const auto &entry : overrideMap) {
        if (m_sources.find(entry.first) == m_sources.end())
            tupleKeys.push_back(entry.first);
    }
    std::sort(tupleKeys.begin(), tupleKeys.end(), codePointLess);

    QJsonArray conflicts;
    std::vector<Assertion> assertions;
    std::map<QString, std::size_t> assertionByTuple;
    for (const QString &tupleKey : tupleKeys) {
        const QStringList parts = tupleKey.split(QLatin1Char('\n'));
        Assertion assertion;
        assertion.subject = parts.value(0);
        assertion.role = parts.value(1);
        assertion.scope = parts.value(2);
        std::set<QString> stances;
        const auto perList = m_sources.find(tupleKey);
        if (perList != m_sources.end()) {
            for (const auto &[sourceList, candidate] : perList->second) {
                stances.insert(candidate.stance);
                assertion.sources.append(QJsonObject{
                    {QStringLiteral("listId"), candidate.listId},
                    {QStringLiteral("version"), candidate.version},
                    {QStringLiteral("stance"), candidate.stance},
                    {QStringLiteral("note"), candidate.note},
                    {QStringLiteral("path"), QJsonArray::fromStringList(candidate.path)},
                });
            }
        }
        assertion.stance = stances.empty()       ? QStringLiteral("none")
                           : stances.size() == 1 ? *stances.begin()
                                                 : QStringLiteral("conflict");
        const auto override_ = overrideMap.find(tupleKey);
        assertion.override_ = override_ != overrideMap.end()
                                  ? override_->second
                                  : QJsonValue(QJsonValue::Null);
        if (assertion.stance == QLatin1String("conflict")) {
            conflicts.append(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("stance")},
                {QStringLiteral("subject"), assertion.subject},
                {QStringLiteral("role"), assertion.role},
                {QStringLiteral("scope"), assertion.scope},
                {QStringLiteral("overridden"), !assertion.override_.isNull()},
            });
        }
        assertionByTuple.insert_or_assign(
            joinKey({assertion.subject, assertion.role, assertion.scope}), assertions.size());
        assertions.push_back(std::move(assertion));
    }

    qint64 appealsOpen = 0;
    qint64 appealsResolved = 0;
    qint64 appealsUnattached = 0;
    for (const Document &appeal : appeals) {
        const QString subject = appeal.text("subject");
        const QString role = appeal.text("role");
        const QString scope = appeal.text("scope");
        const QString appealList = appeal.text("list");
        const auto found = assertionByTuple.find(joinKey({subject, role, scope}));
        const bool attached =
            found != assertionByTuple.end() &&
            std::any_of(assertions[found->second].sources.begin(),
                        assertions[found->second].sources.end(), [&](const QJsonValue &s) {
                            return s.toObject().value(QStringLiteral("listId")).toString() ==
                                   appealList;
                        });
        if (!attached) {
            ++appealsUnattached;
            continue;
        }
        const QJsonObject *head = usable(appealList);
        const qint64 appealVersion = appeal.integer("version");
        QString status = QStringLiteral("open");
        if (head && head->value(QStringLiteral("version")).toInteger() > appealVersion) {
            const QJsonArray entries = head->value(QStringLiteral("entries")).toArray();
            const bool stillDistrusted =
                std::any_of(entries.begin(), entries.end(), [&](const QJsonValue &item) {
                    const QJsonObject entry = item.toObject();
                    return entry.value(QStringLiteral("subject")).toString() == subject &&
                           entry.value(QStringLiteral("role")).toString() == role &&
                           entry.value(QStringLiteral("scope")).toString() == scope &&
                           entry.value(QStringLiteral("stance")).toString() ==
                               QLatin1String("distrust");
                });
            if (!stillDistrusted)
                status = QStringLiteral("resolved");
        }
        if (status == QLatin1String("resolved"))
            ++appealsResolved;
        else
            ++appealsOpen;
        assertions[found->second].appeals.append(QJsonObject{
            {QStringLiteral("appealId"), appeal.id},
            {QStringLiteral("listId"), appealList},
            {QStringLiteral("version"), appealVersion},
            {QStringLiteral("status"), status},
        });
    }

    std::set<QString> rowIds;
    for (const auto &entry : m_groups)
        rowIds.insert(entry.first);
    rowIds.insert(m_followed.begin(), m_followed.end());
    rowIds.insert(m_reached.begin(), m_reached.end());
    QJsonArray listRows;
    std::map<QString, qint64> stateCounts;
    for (const QString &state : kListStates)
        stateCounts[state] = 0;
    for (const QString &lid : rowIds) {
        const GroupState &info = group(lid);
        for (const QJsonValue &conflict : info.conflicts)
            conflicts.append(conflict);
        const auto names = m_names.find(lid);
        ++stateCounts[info.state];
        const QJsonValue null(QJsonValue::Null);
        listRows.append(QJsonObject{
            {QStringLiteral("listId"), lid},
            {QStringLiteral("issuer"), names != m_names.end() ? names->second.first : QString()},
            {QStringLiteral("name"), names != m_names.end() ? names->second.second : QString()},
            {QStringLiteral("state"), info.state},
            {QStringLiteral("followed"), m_followed.count(lid) > 0},
            {QStringLiteral("reachable"), m_reached.count(lid) > 0},
            {QStringLiteral("headVersion"),
             info.hasHead ? info.head.value(QStringLiteral("version")) : null},
            {QStringLiteral("headDocumentId"), info.hasHead ? QJsonValue(info.headId) : null},
            {QStringLiteral("expiresAt"),
             info.hasHead ? info.head.value(QStringLiteral("expiresAt")) : null},
            {QStringLiteral("cutoffVersion"), info.cutoff ? QJsonValue(*info.cutoff) : null},
            {QStringLiteral("cutoffReasons"), QJsonArray::fromStringList(info.reasons)},
            {QStringLiteral("successor"), info.successor},
            {QStringLiteral("heldVersions"), info.heldVersions},
        });
    }

    std::vector<std::pair<QString, QJsonValue>> keyedConflicts;
    for (const QJsonValue &conflict : std::as_const(conflicts))
        keyedConflicts.emplace_back(canonicalJson(conflict), conflict);
    std::stable_sort(keyedConflicts.begin(), keyedConflicts.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    QJsonArray sortedConflicts;
    qint64 stanceConflicts = 0;
    for (const auto &entry : keyedConflicts) {
        sortedConflicts.append(entry.second);
        if (entry.second.toObject().value(QStringLiteral("kind")).toString() ==
            QLatin1String("stance"))
            ++stanceConflicts;
    }

    std::stable_sort(rejected.begin(), rejected.end(), [](const auto &a, const auto &b) {
        return joinKey({a.first, a.second}) < joinKey({b.first, b.second});
    });
    QJsonArray rejectedRows;
    QJsonObject rejectionCodes;
    for (const auto &[docId, code] : rejected) {
        rejectedRows.append(QJsonObject{
            {QStringLiteral("documentId"), docId},
            {QStringLiteral("code"), code},
        });
        rejectionCodes.insert(code, rejectionCodes.value(code).toInteger() + 1);
    }

    QJsonArray assertionRows;
    for (const Assertion &assertion : assertions) {
        assertionRows.append(QJsonObject{
            {QStringLiteral("subject"), assertion.subject},
            {QStringLiteral("role"), assertion.role},
            {QStringLiteral("scope"), assertion.scope},
            {QStringLiteral("stance"), assertion.stance},
            {QStringLiteral("override"), assertion.override_},
            {QStringLiteral("effective"), assertion.override_.isNull()
                                              ? QJsonValue(assertion.stance)
                                              : assertion.override_},
            {QStringLiteral("sources"), assertion.sources},
            {QStringLiteral("appeals"), assertion.appeals},
        });
    }

    QJsonObject listStates;
    for (const auto &[state, count] : stateCounts)
        listStates.insert(state, count);

    return QJsonObject{
        {QStringLiteral("schema"), kResolutionSchema},
        {QStringLiteral("now"), m_now},
        {QStringLiteral("lists"), listRows},
        {QStringLiteral("assertions"), assertionRows},
        {QStringLiteral("conflicts"), sortedConflicts},
        {QStringLiteral("rejected"), rejectedRows},
        {QStringLiteral("telemetry"),
         QJsonObject{
             {QStringLiteral("documents"), qint64(unique.size())},
             {QStringLiteral("duplicateDocuments"), duplicates},
             {QStringLiteral("rejectedDocuments"), qint64(rejected.size())},
             {QStringLiteral("rejectionCodes"), rejectionCodes},
             {QStringLiteral("revocations"), acceptedRevocations},
             {QStringLiteral("listStates"), listStates},
             {QStringLiteral("followedLists"), qint64(m_followed.size())},
             {QStringLiteral("reachableLists"), qint64(m_reached.size())},
             {QStringLiteral("assertions"), qint64(assertions.size())},
             {QStringLiteral("stanceConflicts"), stanceConflicts},
             {QStringLiteral("overrides"), qint64(overrideMap.size())},
             {QStringLiteral("appeals"),
              QJsonObject{
                  {QStringLiteral("open"), appealsOpen},
                  {QStringLiteral("resolved"), appealsResolved},
                  {QStringLiteral("unattached"), appealsUnattached},
              }},
             {QStringLiteral("expiredDelegations"), m_expiredDelegations},
             {QStringLiteral("depthLimitedEdges"), m_depthLimited},
             {QStringLiteral("cycleEdges"), m_cycles},
         }},
    };
}

} // namespace

QString canonicalJson(const QJsonValue &value, bool *ok)
{
    QString out;
    const bool encoded = encodeCanonical(value, out);
    if (ok)
        *ok = encoded;
    return encoded ? out : QString();
}

QString documentId(const QString &raw)
{
    return QStringLiteral("sha256:") + sha256Hex(raw);
}

QString listId(const QString &issuer, const QString &name)
{
    return QStringLiteral("urn:forkmesh:trust-list:sha256:") +
           sha256Hex(kListIdDomain + QLatin1Char('\n') + issuer + QLatin1Char('\n') + name);
}

QString verifyDocument(const QString &raw, QJsonObject *document)
{
    // toUtf8 writes a lone surrogate as three bytes, the same count as
    // Python's surrogatepass encoding.
    if (raw.toUtf8().size() > kMaxDocumentBytes)
        return QStringLiteral("too_large");
    if (nestingDepth(raw) > kMaxNesting)
        return QStringLiteral("invalid_json");
    StrictJsonParser parser(raw);
    QJsonValue parsed;
    if (!parser.parse(&parsed))
        return QStringLiteral("invalid_json");
    bool canonical = false;
    if (parser.noncanonical() || canonicalJson(parsed, &canonical) != raw || !canonical)
        return QStringLiteral("noncanonical");
    if (!parsed.isObject())
        return QStringLiteral("unsupported_schema");
    const QJsonObject doc = parsed.toObject();
    const QJsonValue schemaValue = doc.value(QStringLiteral("schema"));
    const QString schema = schemaValue.isString() ? schemaValue.toString() : QString();
    QString code;
    if (schemaValue.isString() && schema == kListSchema) {
        if (!keysEqual(doc, kListKeys))
            return QStringLiteral("invalid_keys");
        code = validateList(doc);
    } else if (schemaValue.isString() && schema == kRevocationSchema) {
        if (!keysEqual(doc, kRevocationKeys))
            return QStringLiteral("invalid_keys");
        code = validateRevocation(doc);
    } else if (schemaValue.isString() && schema == kAppealSchema) {
        if (!keysEqual(doc, kAppealKeys))
            return QStringLiteral("invalid_keys");
        code = validateAppeal(doc);
    } else {
        return QStringLiteral("unsupported_schema");
    }
    if (!code.isEmpty())
        return code;
    if (document)
        *document = doc;
    return {};
}

QJsonObject resolve(const QStringList &documents, qint64 now, const QJsonArray &follows,
                    const QJsonArray &overrides)
{
    return Resolver(now).run(documents, follows, overrides);
}

} // namespace forkmesh::trust
