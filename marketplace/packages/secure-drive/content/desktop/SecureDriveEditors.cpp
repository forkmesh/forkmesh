#include "SecureDriveEditors.h"

#include "SecureDriveFormats.h"

#include <QAbstractTableModel>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QStyle>
#include <QTableView>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace forkmesh::securedrive {
namespace {

constexpr double kPi = 3.14159265358979323846;

QToolButton *toolButton(const QString &text, const QString &tip, bool checkable = false)
{
    auto *button = new QToolButton;
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(checkable);
    button->setAutoRaise(true);
    return button;
}

class SafeTextEdit final : public QTextEdit
{
public:
    using QTextEdit::QTextEdit;

protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

class DocumentEditor final : public EditorWindow
{
public:
    DocumentEditor(const QString &name, const QByteArray &content, bool readOnly, SaveHandler save,
                   QWidget *parent)
        : EditorWindow(name, readOnly, std::move(save), parent)
    {
        m_edit = new SafeTextEdit;
        m_edit->setAcceptRichText(true);
        m_edit->setHtml(QString::fromUtf8(content));
        m_edit->setReadOnly(readOnly);
        m_edit->document()->setModified(false);
        body()->addWidget(m_edit, 1);
        if (!readOnly)
            buildToolbar();
        connect(m_edit->document(), &QTextDocument::modificationChanged, this,
                [this](bool changed) { setDirty(changed); });
    }

protected:
    QByteArray serialize() const override { return m_edit->toHtml().toUtf8(); }

private:
    void buildToolbar()
    {
        auto *style = new QComboBox;
        style->addItems({QStringLiteral("Paragraph"), QStringLiteral("Heading 1"),
                         QStringLiteral("Heading 2"), QStringLiteral("Heading 3")});
        style->setToolTip(QStringLiteral("Paragraph style"));
        toolbar()->addWidget(style);
        auto *bold = toolButton(QStringLiteral("B"), QStringLiteral("Bold (Ctrl+B)"), true);
        auto *italic = toolButton(QStringLiteral("I"), QStringLiteral("Italic (Ctrl+I)"), true);
        auto *underline = toolButton(QStringLiteral("U"), QStringLiteral("Underline (Ctrl+U)"), true);
        QFont boldFont = bold->font();
        boldFont.setBold(true);
        bold->setFont(boldFont);
        QFont italicFont = italic->font();
        italicFont.setItalic(true);
        italic->setFont(italicFont);
        QFont underlineFont = underline->font();
        underlineFont.setUnderline(true);
        underline->setFont(underlineFont);
        bold->setShortcut(QKeySequence::Bold);
        italic->setShortcut(QKeySequence::Italic);
        underline->setShortcut(QKeySequence::Underline);
        auto *bullets = toolButton(QStringLiteral("• List"), QStringLiteral("Bulleted list"));
        auto *numbers = toolButton(QStringLiteral("1. List"), QStringLiteral("Numbered list"));
        auto *undo = toolButton(QStringLiteral("Undo"), QStringLiteral("Undo (Ctrl+Z)"));
        auto *redo = toolButton(QStringLiteral("Redo"), QStringLiteral("Redo (Ctrl+Shift+Z)"));
        for (QToolButton *button : {bold, italic, underline, bullets, numbers, undo, redo})
            toolbar()->addWidget(button);
        toolbar()->addStretch(1);

        connect(bold, &QToolButton::clicked, this, [this](bool on) {
            QTextCharFormat format;
            format.setFontWeight(on ? QFont::Bold : QFont::Normal);
            m_edit->mergeCurrentCharFormat(format);
        });
        connect(italic, &QToolButton::clicked, this, [this](bool on) {
            QTextCharFormat format;
            format.setFontItalic(on);
            m_edit->mergeCurrentCharFormat(format);
        });
        connect(underline, &QToolButton::clicked, this, [this](bool on) {
            QTextCharFormat format;
            format.setFontUnderline(on);
            m_edit->mergeCurrentCharFormat(format);
        });
        connect(m_edit, &QTextEdit::currentCharFormatChanged, this,
                [bold, italic, underline](const QTextCharFormat &format) {
                    bold->setChecked(format.fontWeight() >= QFont::Bold);
                    italic->setChecked(format.fontItalic());
                    underline->setChecked(format.fontUnderline());
                });
        connect(style, &QComboBox::activated, this, [this](int level) {
            QTextCursor cursor = m_edit->textCursor();
            cursor.beginEditBlock();
            QTextBlockFormat block;
            block.setHeadingLevel(level);
            cursor.mergeBlockFormat(block);
            QTextCharFormat chars;
            static const qreal sizes[] = {0, 22, 17, 14};
            chars.setFontWeight(level > 0 ? QFont::Bold : QFont::Normal);
            chars.setFontPointSize(level > 0 ? sizes[level] : m_edit->font().pointSizeF());
            cursor.select(QTextCursor::BlockUnderCursor);
            cursor.mergeCharFormat(chars);
            cursor.endEditBlock();
            m_edit->setFocus();
        });
        const auto list = [this](QTextListFormat::Style styleKind) {
            QTextCursor cursor = m_edit->textCursor();
            if (QTextList *current = cursor.currentList()) {
                QTextBlockFormat block;
                block.setIndent(0);
                current->remove(cursor.block());
                cursor.mergeBlockFormat(block);
                return;
            }
            QTextListFormat format;
            format.setStyle(styleKind);
            cursor.createList(format);
        };
        connect(bullets, &QToolButton::clicked, this, [list] { list(QTextListFormat::ListDisc); });
        connect(numbers, &QToolButton::clicked, this, [list] { list(QTextListFormat::ListDecimal); });
        connect(undo, &QToolButton::clicked, m_edit, &QTextEdit::undo);
        connect(redo, &QToolButton::clicked, m_edit, &QTextEdit::redo);
    }

    QTextEdit *m_edit = nullptr;
};

class TextEditor final : public EditorWindow
{
public:
    TextEditor(const QString &name, const QByteArray &content, bool readOnly, SaveHandler save,
               QWidget *parent)
        : EditorWindow(name, readOnly, std::move(save), parent)
    {
        m_edit = new QPlainTextEdit;
        m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_edit->setPlainText(QString::fromUtf8(content));
        m_edit->setReadOnly(readOnly);
        m_edit->document()->setModified(false);
        body()->addWidget(m_edit, 1);
        toolbar()->addStretch(1);
        connect(m_edit->document(), &QTextDocument::modificationChanged, this,
                [this](bool changed) { setDirty(changed); });
    }

protected:
    QByteArray serialize() const override { return m_edit->toPlainText().toUtf8(); }

private:
    QPlainTextEdit *m_edit = nullptr;
};

class SheetModel final : public QAbstractTableModel
{
public:
    SheetModel(Sheet sheet, bool readOnly, QObject *parent)
        : QAbstractTableModel(parent), m_sheet(std::move(sheet)), m_readOnly(readOnly)
    {
        m_rows = std::max(100, m_sheet.rows() + 50);
        m_columns = std::min(702, std::max(26, m_sheet.columns() + 5));
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows; }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : m_columns;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        if (role == Qt::DisplayRole)
            return m_sheet.display(index.row(), index.column());
        if (role == Qt::EditRole)
            return m_sheet.raw(index.row(), index.column());
        if (role == Qt::TextAlignmentRole) {
            bool number = false;
            m_sheet.display(index.row(), index.column()).toDouble(&number);
            return QVariant::fromValue(Qt::Alignment(Qt::AlignVCenter)
                                       | (number ? Qt::AlignRight : Qt::AlignLeft));
        }
        return {};
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || role != Qt::EditRole || m_readOnly)
            return false;
        const QString text = value.toString();
        if (text == m_sheet.raw(index.row(), index.column()))
            return false;
        m_sheet.setRaw(index.row(), index.column(), text);
        emit dataChanged(this->index(0, 0), this->index(m_rows - 1, m_columns - 1));
        if (changed)
            changed();
        return true;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags base = QAbstractTableModel::flags(index);
        return m_readOnly ? base : base | Qt::ItemIsEditable;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        return orientation == Qt::Horizontal ? Sheet::columnName(section) : QString::number(section + 1);
    }

    void grow(int rows, int columns)
    {
        if (rows > 0) {
            beginInsertRows({}, m_rows, m_rows + rows - 1);
            m_rows += rows;
            endInsertRows();
        }
        const int extra = std::min(columns, 702 - m_columns);
        if (extra > 0) {
            beginInsertColumns({}, m_columns, m_columns + extra - 1);
            m_columns += extra;
            endInsertColumns();
        }
    }

    const Sheet &sheet() const { return m_sheet; }
    std::function<void()> changed;

private:
    Sheet m_sheet;
    bool m_readOnly = false;
    int m_rows = 0;
    int m_columns = 0;
};

class SheetEditor final : public EditorWindow
{
public:
    SheetEditor(const QString &name, const QByteArray &content, bool readOnly, SaveHandler save,
                QWidget *parent)
        : EditorWindow(name, readOnly, std::move(save), parent)
    {
        m_model = new SheetModel(Sheet::fromCsv(content), readOnly, this);
        m_model->changed = [this] { setDirty(true); };
        auto *cell = new QLabel(QStringLiteral("A1"));
        cell->setMinimumWidth(48);
        m_formula = new QLineEdit;
        m_formula->setPlaceholderText(QStringLiteral("Value or formula, like =SUM(A1:A5)"));
        m_formula->setReadOnly(readOnly);
        toolbar()->addWidget(cell);
        toolbar()->addWidget(m_formula, 1);
        if (!readOnly) {
            auto *rows = toolButton(QStringLiteral("+ Rows"), QStringLiteral("Add 50 rows"));
            auto *columns = toolButton(QStringLiteral("+ Column"), QStringLiteral("Add a column"));
            toolbar()->addWidget(rows);
            toolbar()->addWidget(columns);
            connect(rows, &QToolButton::clicked, this, [this] { m_model->grow(50, 0); });
            connect(columns, &QToolButton::clicked, this, [this] { m_model->grow(0, 1); });
        }
        m_view = new QTableView;
        m_view->setModel(m_model);
        m_view->horizontalHeader()->setDefaultSectionSize(110);
        m_view->verticalHeader()->setDefaultSectionSize(24);
        m_view->installEventFilter(this);
        body()->addWidget(m_view, 1);
        auto *help = new QLabel(QStringLiteral(
            "Formulas: + - * / ^, comparisons, SUM, AVERAGE, MIN, MAX, COUNT, ROUND, ABS and IF over cells like B2 or ranges like B2:B9. Saved as CSV, which Excel, Numbers and LibreOffice open."));
        help->setObjectName(QStringLiteral("mutedLabel"));
        help->setWordWrap(true);
        body()->addWidget(help);
        connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this, cell](const QModelIndex &current) {
                    cell->setText(Sheet::columnName(current.column()) + QString::number(current.row() + 1));
                    m_formula->setText(m_model->data(current, Qt::EditRole).toString());
                });
        connect(m_formula, &QLineEdit::returnPressed, this, [this] {
            const QModelIndex current = m_view->currentIndex();
            if (!current.isValid())
                return;
            m_model->setData(current, m_formula->text(), Qt::EditRole);
            m_view->setCurrentIndex(m_model->index(current.row() + 1, current.column()));
            m_view->setFocus();
        });
        connect(m_model, &QAbstractItemModel::dataChanged, this, [this] {
            const QModelIndex current = m_view->currentIndex();
            if (current.isValid() && !m_formula->hasFocus())
                m_formula->setText(m_model->data(current, Qt::EditRole).toString());
        });
        m_view->setCurrentIndex(m_model->index(0, 0));
    }

protected:
    QByteArray serialize() const override { return m_model->sheet().toCsv(); }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_view && event->type() == QEvent::KeyPress && !isReadOnly()) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
                for (const QModelIndex &index : m_view->selectionModel()->selectedIndexes())
                    m_model->setData(index, QString(), Qt::EditRole);
                return true;
            }
        }
        return EditorWindow::eventFilter(watched, event);
    }

private:
    SheetModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    QLineEdit *m_formula = nullptr;
};

class Canvas final : public QWidget
{
public:
    enum Tool { Pen, Line, Rectangle, Ellipse, Eraser };

    explicit Canvas(Drawing drawing, bool readOnly) : m_drawing(std::move(drawing)), m_readOnly(readOnly)
    {
        setMinimumSize(480, 320);
        setMouseTracking(false);
        setCursor(readOnly ? Qt::ArrowCursor : Qt::CrossCursor);
    }

    Tool tool = Pen;
    QString color = QStringLiteral("#1f2328");
    double width = 4;
    std::function<void()> changed;

    const Drawing &drawing() const { return m_drawing; }

    void undo()
    {
        if (m_history.isEmpty())
            return;
        m_drawing = m_history.takeLast();
        update();
        if (changed)
            changed();
    }

    void clear()
    {
        if (m_drawing.strokes.isEmpty())
            return;
        remember();
        m_drawing.strokes.clear();
        update();
        if (changed)
            changed();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().window());
        const QRectF page = pageRect();
        painter.fillRect(page, QColor(m_drawing.background));
        painter.setPen(QPen(palette().mid().color(), 1));
        painter.drawRect(page);
        painter.setClipRect(page);
        const double scale = page.width() / m_drawing.width;
        const auto draw = [&](const Stroke &stroke) {
            if (stroke.points.isEmpty())
                return;
            QPen pen(QColor(stroke.color), std::max(1.0, stroke.width * scale));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            if (stroke.points.size() == 1) {
                painter.drawPoint(toScreen(stroke.points.first()));
                return;
            }
            QPainterPath path(toScreen(stroke.points.first()));
            for (int i = 1; i < stroke.points.size(); ++i)
                path.lineTo(toScreen(stroke.points.at(i)));
            painter.drawPath(path);
        };
        for (const Stroke &stroke : m_drawing.strokes)
            draw(stroke);
        if (m_stroking)
            draw(m_current);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_readOnly || event->button() != Qt::LeftButton)
            return;
        const QPointF point = toDrawing(event->position());
        remember();
        if (tool == Eraser) {
            m_erased = false;
            erase(point);
            return;
        }
        m_start = point;
        m_current = Stroke{color, width, {point}};
        m_stroking = true;
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_readOnly || !(event->buttons() & Qt::LeftButton))
            return;
        const QPointF point = toDrawing(event->position());
        if (tool == Eraser) {
            erase(point);
            return;
        }
        if (!m_stroking)
            return;
        if (tool == Pen) {
            if (QLineF(m_current.points.last(), point).length() >= 1.0)
                m_current.points.append(point);
        } else {
            m_current.points = shape(m_start, point);
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_readOnly || event->button() != Qt::LeftButton)
            return;
        if (tool == Eraser) {
            if (!m_erased && !m_history.isEmpty())
                m_history.removeLast();
            return;
        }
        if (!m_stroking)
            return;
        m_stroking = false;
        m_drawing.strokes.append(m_current);
        m_current = Stroke();
        update();
        if (changed)
            changed();
    }

private:
    QRectF pageRect() const
    {
        const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);
        const double scale = std::min(area.width() / m_drawing.width, area.height() / m_drawing.height);
        const QSizeF size(m_drawing.width * scale, m_drawing.height * scale);
        return QRectF(area.center() - QPointF(size.width() / 2, size.height() / 2), size);
    }

    QPointF toScreen(const QPointF &point) const
    {
        const QRectF page = pageRect();
        const double scale = page.width() / m_drawing.width;
        return page.topLeft() + point * scale;
    }

    QPointF toDrawing(const QPointF &point) const
    {
        const QRectF page = pageRect();
        const double scale = page.width() / m_drawing.width;
        const QPointF p = (point - page.topLeft()) / scale;
        return QPointF(std::clamp(p.x(), 0.0, double(m_drawing.width)),
                       std::clamp(p.y(), 0.0, double(m_drawing.height)));
    }

    QList<QPointF> shape(const QPointF &a, const QPointF &b) const
    {
        if (tool == Line)
            return {a, b};
        if (tool == Rectangle)
            return {a, QPointF(b.x(), a.y()), b, QPointF(a.x(), b.y()), a};
        QList<QPointF> points;
        const QPointF centre = (a + b) / 2;
        const double rx = std::fabs(b.x() - a.x()) / 2, ry = std::fabs(b.y() - a.y()) / 2;
        for (int i = 0; i <= 72; ++i) {
            const double angle = i * kPi * 2 / 72;
            points.append(centre + QPointF(rx * std::cos(angle), ry * std::sin(angle)));
        }
        return points;
    }

    void erase(const QPointF &point)
    {
        const double radius = std::max(8.0, width * 2);
        const qsizetype before = m_drawing.strokes.size();
        m_drawing.strokes.removeIf([&](const Stroke &stroke) {
            for (const QPointF &p : stroke.points)
                if (QLineF(p, point).length() <= radius + stroke.width / 2)
                    return true;
            return false;
        });
        if (m_drawing.strokes.size() != before) {
            m_erased = true;
            update();
            if (changed)
                changed();
        }
    }

    void remember()
    {
        m_history.append(m_drawing);
        if (m_history.size() > 100)
            m_history.removeFirst();
    }

    Drawing m_drawing;
    bool m_readOnly = false;
    bool m_stroking = false;
    bool m_erased = false;
    QPointF m_start;
    Stroke m_current;
    QList<Drawing> m_history;
};

class DrawingEditor final : public EditorWindow
{
public:
    DrawingEditor(const QString &name, Drawing drawing, bool readOnly, SaveHandler save, QWidget *parent)
        : EditorWindow(name, readOnly, std::move(save), parent)
    {
        m_canvas = new Canvas(std::move(drawing), readOnly);
        m_canvas->changed = [this] { setDirty(true); };
        if (!readOnly) {
            auto *group = new QButtonGroup(this);
            const QList<QPair<QString, Canvas::Tool>> tools{{QStringLiteral("Pen"), Canvas::Pen},
                                                            {QStringLiteral("Line"), Canvas::Line},
                                                            {QStringLiteral("Box"), Canvas::Rectangle},
                                                            {QStringLiteral("Oval"), Canvas::Ellipse},
                                                            {QStringLiteral("Eraser"), Canvas::Eraser}};
            for (const auto &tool : tools) {
                auto *button = toolButton(tool.first, tool.first, true);
                group->addButton(button, tool.second);
                toolbar()->addWidget(button);
                if (tool.second == Canvas::Pen)
                    button->setChecked(true);
            }
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int id) { m_canvas->tool = Canvas::Tool(id); });
            auto *colors = new QButtonGroup(this);
            const QStringList palette{QStringLiteral("#1f2328"), QStringLiteral("#cf222e"),
                                      QStringLiteral("#bf8700"), QStringLiteral("#1a7f37"),
                                      QStringLiteral("#0969da"), QStringLiteral("#8250df"),
                                      QStringLiteral("#ffffff")};
            for (int i = 0; i < palette.size(); ++i) {
                auto *swatch = new QToolButton;
                swatch->setCheckable(true);
                swatch->setFixedSize(22, 22);
                swatch->setToolTip(palette.at(i));
                swatch->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid #8c959f;border-radius:4px;}"
                                                     "QToolButton:checked{border:2px solid #0969da;}")
                                          .arg(palette.at(i)));
                colors->addButton(swatch, i);
                toolbar()->addWidget(swatch);
                if (i == 0)
                    swatch->setChecked(true);
            }
            connect(colors, &QButtonGroup::idClicked, this,
                    [this, palette](int id) { m_canvas->color = palette.at(id); });
            auto *width = new QSpinBox;
            width->setRange(1, 40);
            width->setValue(4);
            width->setSuffix(QStringLiteral(" px"));
            width->setToolTip(QStringLiteral("Line width"));
            toolbar()->addWidget(width);
            connect(width, &QSpinBox::valueChanged, this, [this](int value) { m_canvas->width = value; });
            auto *undo = toolButton(QStringLiteral("Undo"), QStringLiteral("Undo (Ctrl+Z)"));
            undo->setShortcut(QKeySequence::Undo);
            auto *clear = toolButton(QStringLiteral("Clear"), QStringLiteral("Remove every stroke"));
            toolbar()->addWidget(undo);
            toolbar()->addWidget(clear);
            connect(undo, &QToolButton::clicked, this, [this] { m_canvas->undo(); });
            connect(clear, &QToolButton::clicked, this, [this] { m_canvas->clear(); });
        }
        toolbar()->addStretch(1);
        body()->addWidget(m_canvas, 1);
    }

protected:
    QByteArray serialize() const override { return m_canvas->drawing().toSvg(); }

private:
    Canvas *m_canvas = nullptr;
};

class ImageViewer final : public EditorWindow
{
public:
    ImageViewer(const QString &name, const QImage &image, QWidget *parent)
        : EditorWindow(name, true, {}, parent)
    {
        auto *label = new QLabel;
        label->setAlignment(Qt::AlignCenter);
        QImage shown = image;
        if (shown.width() > 2400 || shown.height() > 2400)
            shown = shown.scaled(2400, 2400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(QPixmap::fromImage(shown));
        auto *scroll = new QScrollArea;
        scroll->setWidget(label);
        scroll->setWidgetResizable(true);
        body()->addWidget(scroll, 1);
        toolbar()->addWidget(new QLabel(QStringLiteral("%1 × %2").arg(image.width()).arg(image.height())));
        toolbar()->addStretch(1);
    }

protected:
    QByteArray serialize() const override { return {}; }
};

}  // namespace

EditorWindow::EditorWindow(const QString &name, bool readOnly, SaveHandler save, QWidget *parent)
    : QDialog(parent), m_name(name), m_readOnly(readOnly), m_save(std::move(save))
{
    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setModal(false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    m_toolbar = new QHBoxLayout;
    m_toolbar->setSpacing(4);
    layout->addLayout(m_toolbar);
    m_body = new QVBoxLayout;
    layout->addLayout(m_body, 1);
    auto *footer = new QHBoxLayout;
    m_status = new QLabel(readOnly ? QStringLiteral("Read only. Decrypted in memory; nothing is written to this computer's disk.")
                                   : QStringLiteral("Decrypted in memory; nothing is written to this computer's disk. Each save is a new encrypted version on the drive."));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    if (!readOnly) {
        auto *saveButton = new QPushButton(QStringLiteral("Save to drive"));
        saveButton->setObjectName(QStringLiteral("primaryButton"));
        footer->addWidget(saveButton);
        connect(saveButton, &QPushButton::clicked, this, [this] { saveNow(); });
        auto *shortcut = new QShortcut(QKeySequence::Save, this);
        connect(shortcut, &QShortcut::activated, this, [this] { saveNow(); });
    }
    layout->addLayout(footer);
    resize(1000, 720);
    updateTitle();
}

EditorWindow::~EditorWindow() = default;

void EditorWindow::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    updateTitle();
}

void EditorWindow::updateTitle()
{
    setWindowTitle((m_dirty ? QStringLiteral("• ") : QString()) + m_name + QStringLiteral(" — Secure Drive"));
}

bool EditorWindow::saveNow()
{
    if (m_readOnly || !m_save)
        return false;
    QByteArray bytes = serialize();
    QString error;
    const bool ok = m_save(bytes, &error);
    bytes.fill('\0');
    m_status->setObjectName(ok ? QStringLiteral("mutedLabel") : QStringLiteral("errorLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setText(ok ? QStringLiteral("Saved a new encrypted version to the drive.") : error);
    if (ok)
        setDirty(false);
    return ok;
}

void EditorWindow::discardAndClose()
{
    m_dirty = false;
    close();
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    if (m_dirty && !m_readOnly) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Save changes?"),
            QStringLiteral("%1 has changes that are not on the drive yet.").arg(m_name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !saveNow())) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

QByteArray EditorWindow::newDocument()
{
    return QByteArrayLiteral("<html><body><p></p></body></html>");
}

QByteArray EditorWindow::newSpreadsheet() { return {}; }

QByteArray EditorWindow::newDrawing() { return Drawing().toSvg(); }

bool editableKind(const QString &kind)
{
    return kind == QLatin1String("document") || kind == QLatin1String("text")
           || kind == QLatin1String("spreadsheet") || kind == QLatin1String("drawing");
}

EditorWindow *openEditor(const QString &kind, const QString &name, const QByteArray &content,
                         bool readOnly, SaveHandler save, QWidget *parent, QString *error)
{
    if (kind == QLatin1String("document"))
        return new DocumentEditor(name, content, readOnly, std::move(save), parent);
    if (kind == QLatin1String("text"))
        return new TextEditor(name, content, readOnly, std::move(save), parent);
    if (kind == QLatin1String("spreadsheet"))
        return new SheetEditor(name, content, readOnly, std::move(save), parent);
    if (kind == QLatin1String("drawing")) {
        Drawing drawing;
        if (Drawing::fromSvg(content, &drawing))
            return new DrawingEditor(name, std::move(drawing), readOnly, std::move(save), parent);
    }
    if (kind == QLatin1String("image") || kind == QLatin1String("drawing")) {
        const QImage image = QImage::fromData(content);
        if (!image.isNull())
            return new ImageViewer(name, image, parent);
        if (error)
            *error = QStringLiteral("This picture cannot be shown here. Use Open in another app or Save a decrypted copy.");
        return nullptr;
    }
    if (error)
        *error = QStringLiteral("There is no built-in viewer for this kind of file. Use Open in another app or Save a decrypted copy.");
    return nullptr;
}

}  // namespace forkmesh::securedrive
