#pragma once

#include <QList>

namespace forkmesh::chattranscript {

inline constexpr int kBatchMessages = 120;
inline constexpr qsizetype kBatchChars = 24000;

class Budget
{
public:
    explicit Budget(int batches)
        : m_maxRows(qsizetype(qMax(1, batches)) * kBatchMessages),
          m_maxChars(qsizetype(qMax(1, batches)) * kBatchChars)
    {
    }

    bool admit(qsizetype weight)
    {
        const qsizetype chars = qMax<qsizetype>(0, weight);
        if (m_rows > 0 && (m_rows >= m_maxRows || m_chars + chars > m_maxChars))
            return false;
        ++m_rows;
        m_chars += chars;
        return true;
    }

private:
    qsizetype m_maxRows;
    qsizetype m_maxChars;
    qsizetype m_rows = 0;
    qsizetype m_chars = 0;
};

inline int firstRenderedRow(const QList<qsizetype> &rowChars, int batches)
{
    Budget budget(batches);
    int first = int(rowChars.size());
    while (first > 0 && budget.admit(rowChars.at(first - 1)))
        --first;
    return first;
}

inline bool overBudget(qsizetype renderedRows, qsizetype renderedChars,
                       int batches)
{
    const int rounds = qMax(1, batches);
    return renderedRows > 2 * qsizetype(rounds) * kBatchMessages ||
           (renderedRows > 1 &&
            renderedChars > 2 * qsizetype(rounds) * kBatchChars);
}

}
