#include "SecureDriveFormats.h"

#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cmath>
#include <functional>
#include <limits>

namespace forkmesh::securedrive {

namespace {

constexpr int kMaxRows = 10000;
constexpr int kMaxColumns = 702;
constexpr int kMaxStrokes = 20000;
constexpr int kMaxPoints = 200000;

QString formatNumber(double value)
{
    if (std::isnan(value) || std::isinf(value))
        return QStringLiteral("#NUM!");
    if (value == std::floor(value) && std::fabs(value) < 1e15)
        return QString::number(qint64(value));
    return QString::number(value, 'g', 12);
}

}  // namespace

class FormulaParser
{
public:
    using Value = Sheet::Value;

    FormulaParser(const Sheet &sheet, const QString &text, QSet<qint64> *visiting)
        : m_sheet(sheet), m_text(text), m_visiting(visiting)
    {
    }

    Value run()
    {
        Value value = comparison();
        skip();
        if (value.error.isEmpty() && m_pos < m_text.size())
            return error(QStringLiteral("#ERROR!"));
        return value;
    }

private:
    static Value error(const QString &code)
    {
        Value v;
        v.error = code;
        return v;
    }

    static Value number(double n)
    {
        Value v;
        v.number = n;
        return v;
    }

    void skip()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    bool take(const QString &token)
    {
        skip();
        if (m_text.mid(m_pos, token.size()).compare(token, Qt::CaseInsensitive) == 0) {
            m_pos += int(token.size());
            return true;
        }
        return false;
    }

    static bool numeric(const Value &v, double *out)
    {
        if (!v.error.isEmpty() || v.isText)
            return false;
        *out = v.empty ? 0 : v.number;
        return true;
    }

    Value comparison()
    {
        Value left = additive();
        static const QStringList operators{QStringLiteral("<="), QStringLiteral(">="),
                                           QStringLiteral("<>"), QStringLiteral("<"),
                                           QStringLiteral(">"), QStringLiteral("=")};
        for (const QString &op : operators) {
            if (!take(op))
                continue;
            Value right = additive();
            if (!left.error.isEmpty())
                return left;
            if (!right.error.isEmpty())
                return right;
            double a = 0, b = 0;
            bool result = false;
            if (left.isText || right.isText) {
                const int order = left.text.compare(right.text, Qt::CaseInsensitive);
                result = op == QLatin1String("=") ? order == 0
                         : op == QLatin1String("<>") ? order != 0
                         : op == QLatin1String("<") ? order < 0
                         : op == QLatin1String(">") ? order > 0
                         : op == QLatin1String("<=") ? order <= 0
                                                     : order >= 0;
            } else {
                numeric(left, &a);
                numeric(right, &b);
                result = op == QLatin1String("=") ? a == b
                         : op == QLatin1String("<>") ? a != b
                         : op == QLatin1String("<") ? a < b
                         : op == QLatin1String(">") ? a > b
                         : op == QLatin1String("<=") ? a <= b
                                                     : a >= b;
            }
            return number(result ? 1 : 0);
        }
        return left;
    }

    Value additive()
    {
        Value left = multiplicative();
        for (;;) {
            const bool plus = take(QStringLiteral("+"));
            if (!plus && !take(QStringLiteral("-")))
                return left;
            Value right = multiplicative();
            double a = 0, b = 0;
            if (!left.error.isEmpty())
                return left;
            if (!right.error.isEmpty())
                return right;
            if (!numeric(left, &a) || !numeric(right, &b))
                return error(QStringLiteral("#VALUE!"));
            left = number(plus ? a + b : a - b);
        }
    }

    Value multiplicative()
    {
        Value left = power();
        for (;;) {
            const bool times = take(QStringLiteral("*"));
            if (!times && !take(QStringLiteral("/")))
                return left;
            Value right = power();
            double a = 0, b = 0;
            if (!left.error.isEmpty())
                return left;
            if (!right.error.isEmpty())
                return right;
            if (!numeric(left, &a) || !numeric(right, &b))
                return error(QStringLiteral("#VALUE!"));
            if (!times && b == 0)
                return error(QStringLiteral("#DIV/0!"));
            left = number(times ? a * b : a / b);
        }
    }

    Value power()
    {
        Value base = unary();
        if (!take(QStringLiteral("^")))
            return base;
        Value exponent = power();
        double a = 0, b = 0;
        if (!base.error.isEmpty())
            return base;
        if (!exponent.error.isEmpty())
            return exponent;
        if (!numeric(base, &a) || !numeric(exponent, &b))
            return error(QStringLiteral("#VALUE!"));
        return number(std::pow(a, b));
    }

    Value unary()
    {
        if (take(QStringLiteral("-"))) {
            Value inner = unary();
            double a = 0;
            if (!inner.error.isEmpty())
                return inner;
            if (!numeric(inner, &a))
                return error(QStringLiteral("#VALUE!"));
            return number(-a);
        }
        if (take(QStringLiteral("+")))
            return unary();
        return primary();
    }

    bool reference(int *row, int *column)
    {
        skip();
        static const QRegularExpression pattern(QStringLiteral("\\$?([A-Za-z]{1,2})\\$?([0-9]{1,5})"));
        const auto match = pattern.match(m_text, m_pos, QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch())
            return false;
        if (!Sheet::parseCell(match.captured(1) + match.captured(2), row, column))
            return false;
        m_pos += int(match.capturedLength());
        return true;
    }

    bool range(QList<Value> *values)
    {
        const int start = m_pos;
        int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
        if (!reference(&r1, &c1))
            return false;
        if (!take(QStringLiteral(":"))) {
            values->append(m_sheet.cellValue(r1, c1, m_visiting));
            return true;
        }
        if (!reference(&r2, &c2)) {
            m_pos = start;
            return false;
        }
        for (int r = std::min(r1, r2); r <= std::max(r1, r2); ++r)
            for (int c = std::min(c1, c2); c <= std::max(c1, c2); ++c)
                values->append(m_sheet.cellValue(r, c, m_visiting));
        return true;
    }

    Value function(const QString &name)
    {
        QList<Value> values;
        QList<QList<Value>> arguments;
        if (!take(QStringLiteral(")"))) {
            for (;;) {
                QList<Value> argument;
                const int start = m_pos;
                if (range(&argument)) {
                    skip();
                    if (m_pos < m_text.size() && m_text.at(m_pos) != QLatin1Char(',')
                        && m_text.at(m_pos) != QLatin1Char(')')) {
                        m_pos = start;
                        argument.clear();
                        argument.append(comparison());
                    }
                } else {
                    argument.append(comparison());
                }
                arguments.append(argument);
                values.append(argument);
                if (take(QStringLiteral(",")))
                    continue;
                if (take(QStringLiteral(")")))
                    break;
                return error(QStringLiteral("#ERROR!"));
            }
        }
        for (const Value &v : std::as_const(values))
            if (!v.error.isEmpty())
                return v;
        const QString upper = name.toUpper();
        QList<double> numbers;
        for (const Value &v : std::as_const(values))
            if (!v.isText && !v.empty)
                numbers.append(v.number);
        if (upper == QLatin1String("SUM")) {
            double total = 0;
            for (double n : numbers)
                total += n;
            return number(total);
        }
        if (upper == QLatin1String("AVERAGE")) {
            if (numbers.isEmpty())
                return error(QStringLiteral("#DIV/0!"));
            double total = 0;
            for (double n : numbers)
                total += n;
            return number(total / double(numbers.size()));
        }
        if (upper == QLatin1String("MIN") || upper == QLatin1String("MAX")) {
            if (numbers.isEmpty())
                return number(0);
            double best = numbers.first();
            for (double n : numbers)
                best = upper == QLatin1String("MIN") ? std::min(best, n) : std::max(best, n);
            return number(best);
        }
        if (upper == QLatin1String("COUNT"))
            return number(double(numbers.size()));
        double a = 0, b = 0;
        if (upper == QLatin1String("ABS") && arguments.size() == 1 && arguments.first().size() == 1
            && numeric(arguments.first().first(), &a))
            return number(std::fabs(a));
        if (upper == QLatin1String("ROUND") && (arguments.size() == 1 || arguments.size() == 2)) {
            if (!numeric(arguments.at(0).value(0), &a)
                || (arguments.size() == 2 && !numeric(arguments.at(1).value(0), &b)))
                return error(QStringLiteral("#VALUE!"));
            const double scale = std::pow(10.0, std::round(b));
            return number(std::round(a * scale) / scale);
        }
        if (upper == QLatin1String("IF") && (arguments.size() == 2 || arguments.size() == 3)) {
            double test = 0;
            if (!numeric(arguments.at(0).value(0), &test))
                return error(QStringLiteral("#VALUE!"));
            if (test != 0)
                return arguments.at(1).value(0);
            return arguments.size() == 3 ? arguments.at(2).value(0) : number(0);
        }
        return error(QStringLiteral("#NAME?"));
    }

    Value primary()
    {
        skip();
        if (m_pos >= m_text.size())
            return error(QStringLiteral("#ERROR!"));
        if (take(QStringLiteral("("))) {
            Value inner = comparison();
            if (!take(QStringLiteral(")")))
                return error(QStringLiteral("#ERROR!"));
            return inner;
        }
        if (m_text.at(m_pos) == QLatin1Char('"')) {
            QString text;
            ++m_pos;
            while (m_pos < m_text.size()) {
                const QChar c = m_text.at(m_pos++);
                if (c == QLatin1Char('"')) {
                    if (m_pos < m_text.size() && m_text.at(m_pos) == QLatin1Char('"')) {
                        text.append(c);
                        ++m_pos;
                        continue;
                    }
                    Value v;
                    v.isText = true;
                    v.text = text;
                    return v;
                }
                text.append(c);
            }
            return error(QStringLiteral("#ERROR!"));
        }
        static const QRegularExpression numberPattern(
            QStringLiteral("[0-9]+(\\.[0-9]*)?([eE][-+]?[0-9]+)?|\\.[0-9]+([eE][-+]?[0-9]+)?"));
        const auto numberMatch = numberPattern.match(m_text, m_pos, QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
        if (numberMatch.hasMatch()) {
            m_pos += int(numberMatch.capturedLength());
            return number(numberMatch.captured(0).toDouble());
        }
        static const QRegularExpression namePattern(QStringLiteral("[A-Za-z]+(?=\\s*\\()"));
        const auto nameMatch = namePattern.match(m_text, m_pos, QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
        if (nameMatch.hasMatch()) {
            m_pos += int(nameMatch.capturedLength());
            take(QStringLiteral("("));
            return function(nameMatch.captured(0));
        }
        int row = 0, column = 0;
        if (reference(&row, &column))
            return m_sheet.cellValue(row, column, m_visiting);
        return error(QStringLiteral("#NAME?"));
    }

    const Sheet &m_sheet;
    QString m_text;
    QSet<qint64> *m_visiting;
    int m_pos = 0;
};

Sheet Sheet::fromCsv(const QByteArray &csv)
{
    Sheet sheet;
    QString text = QString::fromUtf8(csv);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    QVector<QString> row;
    QString field;
    bool quoted = false, any = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field.append(c);
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.append(c);
            }
            continue;
        }
        if (c == QLatin1Char('"') && field.isEmpty()) {
            quoted = true;
            any = true;
        } else if (c == QLatin1Char(',')) {
            row.append(field);
            field.clear();
            any = true;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;
            row.append(field);
            field.clear();
            if (sheet.m_cells.size() < kMaxRows)
                sheet.m_cells.append(row.mid(0, kMaxColumns));
            row.clear();
            any = false;
        } else {
            field.append(c);
            any = true;
        }
    }
    if (any || !field.isEmpty() || !row.isEmpty()) {
        row.append(field);
        if (sheet.m_cells.size() < kMaxRows)
            sheet.m_cells.append(row.mid(0, kMaxColumns));
    }
    return sheet;
}

QByteArray Sheet::toCsv() const
{
    int lastRow = -1;
    for (int r = 0; r < rows(); ++r)
        for (const QString &cell : m_cells.at(r))
            if (!cell.isEmpty())
                lastRow = r;
    QStringList lines;
    for (int r = 0; r <= lastRow; ++r) {
        QVector<QString> row = m_cells.at(r);
        while (!row.isEmpty() && row.last().isEmpty())
            row.removeLast();
        QStringList fields;
        for (QString cell : std::as_const(row)) {
            if (cell.contains(QLatin1Char(',')) || cell.contains(QLatin1Char('"'))
                || cell.contains(QLatin1Char('\n')) || cell.contains(QLatin1Char('\r'))
                || cell != cell.trimmed()) {
                cell.replace(QLatin1Char('"'), QStringLiteral("\"\""));
                cell = QLatin1Char('"') + cell + QLatin1Char('"');
            }
            fields << cell;
        }
        lines << fields.join(QLatin1Char(','));
    }
    return lines.isEmpty() ? QByteArray() : (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8();
}

int Sheet::columns() const
{
    int most = 0;
    for (const auto &row : m_cells)
        most = std::max(most, int(row.size()));
    return most;
}

QString Sheet::raw(int row, int column) const
{
    if (row < 0 || row >= rows() || column < 0 || column >= m_cells.at(row).size())
        return {};
    return m_cells.at(row).at(column);
}

void Sheet::setRaw(int row, int column, const QString &text)
{
    if (row < 0 || column < 0 || row >= kMaxRows || column >= kMaxColumns)
        return;
    if (row >= rows()) {
        if (text.isEmpty())
            return;
        m_cells.resize(row + 1);
    }
    QVector<QString> &cells = m_cells[row];
    if (column >= cells.size()) {
        if (text.isEmpty())
            return;
        cells.resize(column + 1);
    }
    cells[column] = text;
}

QString Sheet::display(int row, int column) const
{
    QSet<qint64> visiting;
    const QString text = raw(row, column);
    if (!text.startsWith(QLatin1Char('=')))
        return text;
    const Value value = cellValue(row, column, &visiting);
    if (!value.error.isEmpty())
        return value.error;
    if (value.isText)
        return value.text;
    return formatNumber(value.empty ? 0 : value.number);
}

Sheet::Value Sheet::cellValue(int row, int column, QSet<qint64> *visiting) const
{
    Value value;
    const QString text = raw(row, column).trimmed();
    if (text.isEmpty()) {
        value.empty = true;
        return value;
    }
    if (text.startsWith(QLatin1Char('='))) {
        const qint64 key = (qint64(row) << 20) | column;
        if (visiting->contains(key) || visiting->size() > 4096) {
            value.error = QStringLiteral("#CYCLE!");
            return value;
        }
        visiting->insert(key);
        value = evaluate(text.mid(1), visiting);
        visiting->remove(key);
        return value;
    }
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (ok) {
        value.number = number;
        return value;
    }
    value.isText = true;
    value.text = text;
    return value;
}

Sheet::Value Sheet::evaluate(const QString &formula, QSet<qint64> *visiting) const
{
    return FormulaParser(*this, formula, visiting).run();
}

QString Sheet::columnName(int column)
{
    QString name;
    int n = column + 1;
    while (n > 0) {
        const int rem = (n - 1) % 26;
        name.prepend(QChar(QLatin1Char('A').unicode() + rem));
        n = (n - 1) / 26;
    }
    return name;
}

bool Sheet::parseCell(const QString &reference, int *row, int *column)
{
    static const QRegularExpression pattern(QStringLiteral("^([A-Za-z]{1,2})([0-9]{1,5})$"));
    const auto match = pattern.match(reference);
    if (!match.hasMatch())
        return false;
    int c = 0;
    for (const QChar ch : match.captured(1).toUpper())
        c = c * 26 + (ch.unicode() - 'A' + 1);
    const int r = match.captured(2).toInt();
    if (r < 1 || r > kMaxRows || c < 1 || c > kMaxColumns)
        return false;
    *row = r - 1;
    *column = c - 1;
    return true;
}

bool validColor(const QString &color)
{
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return pattern.match(color).hasMatch();
}

QByteArray Drawing::toSvg() const
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("svg"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.w3.org/2000/svg"));
    xml.writeAttribute(QStringLiteral("width"), QString::number(width));
    xml.writeAttribute(QStringLiteral("height"), QString::number(height));
    xml.writeAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 %1 %2").arg(width).arg(height));
    xml.writeAttribute(QStringLiteral("data-forkmesh-drawing"), QStringLiteral("1"));
    xml.writeEmptyElement(QStringLiteral("rect"));
    xml.writeAttribute(QStringLiteral("width"), QString::number(width));
    xml.writeAttribute(QStringLiteral("height"), QString::number(height));
    xml.writeAttribute(QStringLiteral("fill"), validColor(background) ? background : QStringLiteral("#ffffff"));
    for (const Stroke &stroke : strokes) {
        if (stroke.points.isEmpty())
            continue;
        QStringList d;
        for (int i = 0; i < stroke.points.size(); ++i) {
            const QPointF &p = stroke.points.at(i);
            d << QStringLiteral("%1%2 %3")
                     .arg(i == 0 ? QStringLiteral("M") : QStringLiteral("L"))
                     .arg(QString::number(p.x(), 'f', 1), QString::number(p.y(), 'f', 1));
        }
        if (stroke.points.size() == 1)
            d << QStringLiteral("L%1 %2").arg(QString::number(stroke.points.first().x(), 'f', 1),
                                              QString::number(stroke.points.first().y(), 'f', 1));
        xml.writeEmptyElement(QStringLiteral("path"));
        xml.writeAttribute(QStringLiteral("d"), d.join(QLatin1Char(' ')));
        xml.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        xml.writeAttribute(QStringLiteral("stroke"), validColor(stroke.color) ? stroke.color : QStringLiteral("#1f2328"));
        xml.writeAttribute(QStringLiteral("stroke-width"), QString::number(stroke.width, 'f', 1));
        xml.writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
        xml.writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

bool Drawing::fromSvg(const QByteArray &svg, Drawing *drawing)
{
    Drawing parsed;
    QXmlStreamReader xml(svg);
    bool marked = false;
    bool background = false;
    int points = 0;
    static const QRegularExpression command(
        QStringLiteral("([ML])\\s*(-?[0-9]+(?:\\.[0-9]+)?)[\\s,]+(-?[0-9]+(?:\\.[0-9]+)?)"));
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto attributes = xml.attributes();
        const QStringView name = xml.name();
        if (name == QLatin1String("svg")) {
            marked = attributes.value(QStringLiteral("data-forkmesh-drawing")) == QLatin1String("1");
            parsed.width = std::clamp(attributes.value(QStringLiteral("width")).toInt(), 1, 20000);
            parsed.height = std::clamp(attributes.value(QStringLiteral("height")).toInt(), 1, 20000);
        } else if (name == QLatin1String("rect") && !background) {
            background = true;
            const QString fill = attributes.value(QStringLiteral("fill")).toString();
            if (validColor(fill))
                parsed.background = fill;
        } else if (name == QLatin1String("path")) {
            if (parsed.strokes.size() >= kMaxStrokes)
                return false;
            Stroke stroke;
            const QString color = attributes.value(QStringLiteral("stroke")).toString();
            if (validColor(color))
                stroke.color = color;
            bool ok = false;
            const double width = attributes.value(QStringLiteral("stroke-width")).toDouble(&ok);
            if (ok)
                stroke.width = std::clamp(width, 0.5, 200.0);
            auto it = command.globalMatch(attributes.value(QStringLiteral("d")).toString());
            while (it.hasNext()) {
                const auto match = it.next();
                stroke.points.append(QPointF(match.captured(2).toDouble(), match.captured(3).toDouble()));
                if (++points > kMaxPoints)
                    return false;
            }
            if (stroke.points.size() == 2 && stroke.points.at(0) == stroke.points.at(1))
                stroke.points.removeLast();
            if (!stroke.points.isEmpty())
                parsed.strokes.append(stroke);
        }
    }
    if (xml.hasError() || !marked)
        return false;
    *drawing = parsed;
    return true;
}

}  // namespace forkmesh::securedrive
