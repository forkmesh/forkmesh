#pragma once

#include <QByteArray>
#include <QList>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>

namespace forkmesh::securedrive {

class Sheet
{
public:
    static Sheet fromCsv(const QByteArray &csv);
    QByteArray toCsv() const;

    int rows() const { return int(m_cells.size()); }
    int columns() const;
    QString raw(int row, int column) const;
    void setRaw(int row, int column, const QString &text);
    QString display(int row, int column) const;

    static QString columnName(int column);
    static bool parseCell(const QString &reference, int *row, int *column);

private:
    struct Value
    {
        double number = 0;
        QString text;
        QString error;
        bool isText = false;
        bool empty = false;
    };

    Value cellValue(int row, int column, QSet<qint64> *visiting) const;
    Value evaluate(const QString &formula, QSet<qint64> *visiting) const;

    QVector<QVector<QString>> m_cells;

    friend class FormulaParser;
};

struct Stroke
{
    QString color = QStringLiteral("#1f2328");
    double width = 3;
    QList<QPointF> points;
};

struct Drawing
{
    int width = 1600;
    int height = 1000;
    QString background = QStringLiteral("#ffffff");
    QList<Stroke> strokes;

    QByteArray toSvg() const;
    static bool fromSvg(const QByteArray &svg, Drawing *drawing);
};

bool validColor(const QString &color);

}  // namespace forkmesh::securedrive
