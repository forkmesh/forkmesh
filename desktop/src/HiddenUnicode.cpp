#include "HiddenUnicode.h"

#include <QVector>

namespace forkmesh::hiddenunicode {
namespace {

struct Range {
    char32_t low;
    char32_t high;
};

constexpr Range kDrop[] = {
    {0x0000, 0x0008}, {0x000B, 0x000C}, {0x000E, 0x001F}, {0x007F, 0x009F},
    {0x00AD, 0x00AD}, {0x034F, 0x034F}, {0x115F, 0x1160}, {0x17B4, 0x17B5},
    {0x180B, 0x180F}, {0x200B, 0x200B}, {0x202A, 0x202E}, {0x2060, 0x206F},
    {0x3164, 0x3164}, {0xD800, 0xDFFF}, {0xFDD0, 0xFDEF}, {0xFE00, 0xFE0D},
    {0xFEFF, 0xFEFF}, {0xFFA0, 0xFFA0}, {0xFFF9, 0xFFFB}, {0x1BCA0, 0x1BCA3},
    {0x1D173, 0x1D17A}, {0xE0000, 0xE007F}, {0xE0100, 0xE01EF},
};

constexpr Range kEmoji[] = {
    {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C}, {0x2049, 0x2049},
    {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2190, 0x2BFF}, {0x3030, 0x3030},
    {0x303D, 0x303D}, {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1FAFF},
};

constexpr Range kJoining[] = {
    {0x0600, 0x08FF}, {0x0900, 0x0DFF}, {0x0F00, 0x109F},
    {0x1780, 0x18AF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFC},
};

constexpr Range kRtl[] = {
    {0x0590, 0x08FF}, {0xFB1D, 0xFDFF}, {0xFE70, 0xFEFC},
    {0x10800, 0x10FFF}, {0x1E800, 0x1EFFF},
};

constexpr char32_t kNone = 0xFFFFFFFF;
constexpr char32_t kZwnj = 0x200C;
constexpr char32_t kZwj = 0x200D;
constexpr char32_t kBlackFlag = 0x1F3F4;
constexpr char32_t kCancelTag = 0xE007F;

template <std::size_t N>
bool within(char32_t cp, const Range (&ranges)[N])
{
    if (cp == kNone)
        return false;
    for (const Range &range : ranges) {
        if (cp >= range.low && cp <= range.high)
            return true;
    }
    return false;
}

bool isWhitespace(char32_t cp)
{
    return cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x20;
}

bool isSelector(char32_t cp)
{
    return cp == 0xFE0E || cp == 0xFE0F;
}

bool quickCandidate(QChar ch)
{
    const char16_t unit = ch.unicode();
    if (unit < 0x20)
        return unit != 0x09 && unit != 0x0A && unit != 0x0D;
    if (unit < 0x7F)
        return false;
    return unit <= 0x9F || unit == 0x00AD || unit == 0x034F || unit == 0x061C
        || unit == 0x115F || unit == 0x1160 || unit == 0x17B4 || unit == 0x17B5
        || (unit >= 0x180B && unit <= 0x180F) || (unit >= 0x200B && unit <= 0x200F)
        || (unit >= 0x2028 && unit <= 0x202E) || (unit >= 0x2060 && unit <= 0x206F)
        || unit == 0x3164 || (unit >= 0xD800 && unit <= 0xDFFF)
        || (unit >= 0xFDD0 && unit <= 0xFDEF) || (unit >= 0xFE00 && unit <= 0xFE0F)
        || unit == 0xFEFF || unit == 0xFFA0 || unit >= 0xFFF9;
}

bool anyCandidate(const QString &text)
{
    for (QChar ch : text) {
        if (quickCandidate(ch))
            return true;
    }
    return false;
}

struct Point {
    char32_t cp;
    int offset;
    int length;
};

QVector<Point> decode(const QString &text)
{
    QVector<Point> points;
    points.reserve(text.size());
    const int size = int(text.size());
    for (int i = 0; i < size; ++i) {
        const QChar ch = text.at(i);
        if (ch.isHighSurrogate() && i + 1 < size && text.at(i + 1).isLowSurrogate()) {
            points.append({QChar::surrogateToUcs4(ch, text.at(i + 1)), i, 2});
            ++i;
            continue;
        }
        points.append({char32_t(ch.unicode()), i, 1});
    }
    return points;
}

int flagTagLength(const QVector<Point> &points, int start)
{
    int index = start;
    while (index < points.size() && index - start < 8) {
        const char32_t cp = points.at(index).cp;
        if (cp == kCancelTag) {
            const int run = index - start;
            return run >= 2 && run <= 7 ? run + 1 : 0;
        }
        if (!((cp >= 0xE0030 && cp <= 0xE0039) || (cp >= 0xE0061 && cp <= 0xE007A)))
            return 0;
        ++index;
    }
    return 0;
}

enum class Verdict { Keep, Drop, Newline };

QVector<Verdict> judge(const QVector<Point> &points)
{
    QVector<Verdict> verdicts(points.size(), Verdict::Keep);
    char32_t prev = kNone;
    int index = 0;
    const int total = int(points.size());
    while (index < total) {
        const char32_t cp = points.at(index).cp;
        const char32_t following = index + 1 < total ? points.at(index + 1).cp : kNone;
        const int at = index++;
        bool kept = true;
        if (cp == 0x2028 || cp == 0x2029) {
            verdicts[at] = Verdict::Newline;
            prev = 0x0A;
            continue;
        }
        if (isSelector(cp)) {
            kept = prev != kNone && !isWhitespace(prev) && !isSelector(prev) && prev != kZwj
                && prev != kZwnj;
        } else if (cp == kZwj) {
            kept = ((prev == 0xFE0F || within(prev, kEmoji)) && within(following, kEmoji))
                || (within(prev, kJoining) && within(following, kJoining));
        } else if (cp == kZwnj) {
            kept = within(prev, kJoining) && within(following, kJoining);
        } else if (cp == 0x061C || cp == 0x200E || cp == 0x200F) {
            kept = within(prev, kRtl) || within(following, kRtl);
        } else if (cp == kBlackFlag) {
            const int run = flagTagLength(points, index);
            prev = run ? points.at(index + run - 1).cp : cp;
            index += run;
            continue;
        } else if (within(cp, kDrop) || (cp & 0xFFFE) == 0xFFFE) {
            kept = false;
        }
        verdicts[at] = kept ? Verdict::Keep : Verdict::Drop;
        if (kept)
            prev = cp;
    }
    return verdicts;
}

}

QString clean(const QString &text)
{
    if (text.isEmpty() || !anyCandidate(text))
        return text;
    const QVector<Point> points = decode(text);
    const QVector<Verdict> verdicts = judge(points);
    QString out;
    out.reserve(text.size());
    bool changed = false;
    for (int i = 0; i < points.size(); ++i) {
        const Point &point = points.at(i);
        switch (verdicts.at(i)) {
        case Verdict::Keep:
            out.append(QStringView(text).mid(point.offset, point.length));
            break;
        case Verdict::Newline:
            out.append(QLatin1Char('\n'));
            changed = true;
            break;
        case Verdict::Drop:
            changed = true;
            break;
        }
    }
    return changed ? out : text;
}

bool hasHidden(const QString &text)
{
    return clean(text) != text;
}

QList<std::pair<int, int>> droppedRanges(const QString &text)
{
    QList<std::pair<int, int>> ranges;
    if (text.isEmpty() || !anyCandidate(text))
        return ranges;
    const QVector<Point> points = decode(text);
    const QVector<Verdict> verdicts = judge(points);
    for (int i = 0; i < points.size(); ++i) {
        if (verdicts.at(i) != Verdict::Drop)
            continue;
        const Point &point = points.at(i);
        if (!ranges.isEmpty() && ranges.last().first + ranges.last().second == point.offset)
            ranges.last().second += point.length;
        else
            ranges.append({point.offset, point.length});
    }
    return ranges;
}

}
