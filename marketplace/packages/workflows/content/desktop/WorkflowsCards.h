#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QStyledItemDelegate>
#include <QWidget>

#include <functional>

class QGridLayout;
class QPalette;

namespace forkmesh::workflows {

enum class ViewMode { List, Thumbnails, Compact };

QString viewModeKey(ViewMode mode);
ViewMode viewModeFromKey(const QString &key);

struct Tones {
    QColor window;
    QColor surface;
    QColor sunken;
    QColor ink;
    QColor secondary;
    QColor muted;
    QColor border;
    QColor strong;
    QColor hover;
    QColor accent;
    QColor success;
    QColor danger;
    QColor attention;
    QColor agent;
    QColor account;
    bool dark = false;

    static Tones from(const QPalette &palette);
    QColor kind(const QString &kind) const;
    QColor state(const QJsonObject &entry) const;
    QString sheet() const;
};

QColor mix(const QColor &a, const QColor &b, qreal amount);
QIcon glyphIcon(const QString &name, const QColor &color, int size = 16);
QPixmap glyphPixmap(const QString &name, const QColor &color, int size, qreal ratio);
QPixmap roundFace(const QPixmap &source, const QString &kind, const Tones &tones, int side, qreal ratio);

class ClampLabel final : public QWidget
{
public:
    ClampLabel(const QString &text, int lines, QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const { return m_text; }
    void setPlaceholder(const QString &placeholder);
    void setColors(const QColor &color, const QColor &placeholder);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateHeight();

    QString m_text;
    QString m_placeholder;
    int m_lines = 1;
    QColor m_color;
    QColor m_placeholderColor;
};

class WorkflowSwitch final : public QAbstractButton
{
public:
    explicit WorkflowSwitch(const Tones &tones, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Tones m_tones;
};

class WorkflowThumbnail final : public QWidget
{
public:
    WorkflowThumbnail(const QJsonObject &entry, const QPixmap &face, const Tones &tones, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QJsonObject m_entry;
    QPixmap m_face;
    Tones m_tones;
};

class WorkflowCard final : public QFrame
{
public:
    using Action = std::function<void(const QString &action, QWidget *anchor)>;

    struct Options {
        ViewMode mode = ViewMode::Thumbnails;
        bool busy = false;
        bool canPin = false;
        bool selected = false;
    };

    WorkflowCard(const QJsonObject &entry, const QPixmap &face, const Tones &tones, const Options &options,
                 Action action, QWidget *parent = nullptr);
    QString key() const;
    QJsonObject entry() const { return m_entry; }
    void setSelected(bool selected);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QWidget *buildControls(bool compact);
    QWidget *kindChip() const;
    QString timesText() const;

    QJsonObject m_entry;
    Tones m_tones;
    Options m_options;
    Action m_action;
    QPoint m_pressAt;
    bool m_pressed = false;
};

enum StepRole {
    StepNameRole = Qt::UserRole + 1,
    StepTypeRole,
    StepDetailRole,
    StepKindRole,
    StepWarnRole,
    StepOffRole,
    StepStatusRole,
};

class StepDelegate final : public QStyledItemDelegate
{
public:
    StepDelegate(const Tones *tones, QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    const Tones *m_tones;
};

class CardGrid final : public QWidget
{
public:
    explicit CardGrid(QWidget *parent = nullptr);
    void setCards(const QList<QWidget *> &cards, ViewMode mode, QWidget *header = nullptr);
    QList<WorkflowCard *> cards() const;
    ViewMode mode() const { return m_mode; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    int columnsFor(int width) const;
    void relayout(bool force);

    QGridLayout *m_layout = nullptr;
    QList<QPointer<QWidget>> m_cards;
    QPointer<QWidget> m_header;
    ViewMode m_mode = ViewMode::Thumbnails;
    int m_columns = 0;
};

} // namespace forkmesh::workflows
