#pragma once

#include <QList>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>

class QMenu;
class QAbstractButton;

namespace forkmesh::ui {

class OverflowActionRow : public QWidget
{
public:
    explicit OverflowActionRow(const QString &usageKey, QWidget *parent = nullptr);

    void addItem(QWidget *widget, int rank, const QString &icon = QString(),
                 const QString &menuLabel = QString());
    void addPinned(QWidget *widget);
    void addStretch();
    void setInlineLimit(int limit);
    void setSpacing(int spacing);
    void setMoreButton(QAbstractButton *button);
    void setIconProvider(std::function<QIcon(const QString &)> provider);

    QAbstractButton *moreButton() const { return m_more; }
    QList<QWidget *> inlineWidgets() const;
    QList<QWidget *> overflowWidgets() const;
    bool isInline(const QWidget *widget) const;
    int usesOf(const QWidget *widget) const;
    void fillMenu(QMenu *menu) const;
    void relayout();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Item {
        QPointer<QWidget> widget;
        int rank = 0;
        bool pinned = false;
        bool right = true;
        QString icon;
        QString label;
        QString key;
        int uses = 0;
    };

    QList<int> chooseInline(int width, QList<int> *overflow) const;
    QSize itemSize(const Item &item) const;
    QString settingKey(const Item &item) const;
    void noteUse(QWidget *widget);
    void showMenu();

    QString m_usageKey;
    QList<Item> m_items;
    QPointer<QAbstractButton> m_more;
    std::function<QIcon(const QString &)> m_iconProvider;
    QTimer m_pending;
    bool m_stretchAdded = false;
    bool m_placing = false;
    int m_limit = 5;
    int m_spacing = 6;
};

}
