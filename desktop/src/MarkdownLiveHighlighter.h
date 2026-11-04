#pragma once

#include <QSyntaxHighlighter>

#include <functional>

class QTextCharFormat;

class MarkdownLiveHighlighter final : public QSyntaxHighlighter
{
public:
    explicit MarkdownLiveHighlighter(QObject *parent = nullptr);

    void setActiveBlock(int block);
    int activeBlock() const { return m_active; }

protected:
    void highlightBlock(const QString &text) override;

private:
    void change(int start, int length, const std::function<void(QTextCharFormat &)> &style);
    void marker(int start, int length);

    int m_active = -1;
    bool m_hide = true;
};
