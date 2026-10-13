#include "ScreenshotMarkupWindow.h"

#include "MarkupCanvas.h"

#include <QButtonGroup>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSize>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>


static QToolButton *makeColorSwatch(const QColor &color, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setCheckable(true);
    constexpr int sz = 22;
    btn->setFixedSize(sz, sz);

    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pm(qMax(1, qRound(sz * dpr)), qMax(1, qRound(sz * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(color);
        p.setPen(QPen(QColor(255, 255, 255, 80), 1));
        p.drawEllipse(QRect(2, 2, sz - 4, sz - 4));
    }

    btn->setIcon(QIcon(pm));
    btn->setIconSize(QSize(sz, sz));
    btn->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; padding: 0; background: transparent; }"
        "QToolButton:checked { border: 2px solid #2f81f7; border-radius: 13px; }"));
    return btn;
}

ScreenshotMarkupWindow::ScreenshotMarkupWindow(const QImage &screenshot, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Annotate Screenshot"));
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose);

    m_canvas = new MarkupCanvas(screenshot, this);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(m_canvas);
    scrollArea->setWidgetResizable(false);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    const QSize screenSz = QGuiApplication::primaryScreen()
                               ? QGuiApplication::primaryScreen()->availableSize()
                               : QSize(1920, 1080);
    const int maxW = qRound(screenSz.width()  * 0.90);
    const int maxH = qRound(screenSz.height() * 0.85);
    constexpr int kToolbarH = 48;
    constexpr int kFooterH  = 52;
    constexpr int kPad      = 24;
    const QSize imgSz = screenshot.size();
    resize(qMin(imgSz.width()  + kPad, maxW),
           qMin(imgSz.height() + kToolbarH + kFooterH + kPad, maxH));

    auto *toolbar = new QWidget(this);
    toolbar->setFixedHeight(kToolbarH);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 4, 8, 4);
    toolbarLayout->setSpacing(4);

    auto *btnPencil  = new QToolButton(this);
    auto *btnLine    = new QToolButton(this);
    auto *btnArrow   = new QToolButton(this);
    auto *btnRect    = new QToolButton(this);
    auto *btnEllipse = new QToolButton(this);
    auto *btnFill    = new QToolButton(this);
    auto *btnText    = new QToolButton(this);
    auto *btnMove    = new QToolButton(this);
    for (auto *b :
         {btnPencil, btnLine, btnArrow, btnRect, btnEllipse, btnText, btnMove}) {
        b->setCheckable(true);
        b->setObjectName("topNavButton");
    }
    btnFill->setCheckable(true);
    btnFill->setObjectName("topNavButton");
    btnPencil->setText(QStringLiteral("Pencil"));
    btnLine->setText(QStringLiteral("Line"));
    btnArrow->setText(QStringLiteral("Arrow"));
    btnRect->setText(QStringLiteral("Rectangle"));
    btnEllipse->setText(QStringLiteral("Ellipse"));
    btnText->setText(QStringLiteral("Text"));
    btnMove->setText(QStringLiteral("Move"));
    btnFill->setText(QStringLiteral("Fill"));
    btnFill->setToolTip(QStringLiteral("Fill new rectangles and ellipses with the selected color"));
    btnText->setToolTip(QStringLiteral("Add multiline text labels"));
    btnMove->setToolTip(
        QStringLiteral("Move text or pasted image layers. Drag a selected layer's lower-right handle to resize it."));
    btnPencil->setChecked(true);

    auto *toolGroup = new QButtonGroup(this);
    toolGroup->addButton(btnPencil);
    toolGroup->addButton(btnLine);
    toolGroup->addButton(btnArrow);
    toolGroup->addButton(btnRect);
    toolGroup->addButton(btnEllipse);
    toolGroup->addButton(btnText);
    toolGroup->addButton(btnMove);

    connect(btnPencil,  &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Pencil); });
    connect(btnLine,    &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Line); });
    connect(btnArrow,   &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Arrow); });
    connect(btnRect,    &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Rect); });
    connect(btnEllipse, &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Ellipse); });
    connect(btnText,    &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Text); });
    connect(btnMove,    &QToolButton::clicked, this,
            [this] { m_canvas->setTool(MarkupTool::Move); });
    connect(btnFill, &QToolButton::toggled, this,
            [this](bool filled) { m_canvas->setShapeFilled(filled); });

    toolbarLayout->addWidget(btnPencil);
    toolbarLayout->addWidget(btnLine);
    toolbarLayout->addWidget(btnArrow);
    toolbarLayout->addWidget(btnRect);
    toolbarLayout->addWidget(btnEllipse);
    toolbarLayout->addWidget(btnFill);
    toolbarLayout->addWidget(btnText);
    toolbarLayout->addWidget(btnMove);

    auto *btnPaste = new QToolButton(this);
    btnPaste->setText(QStringLiteral("Paste layer"));
    btnPaste->setObjectName("ghostButton");
    btnPaste->setToolTip(QStringLiteral("Paste an image from the clipboard as a movable layer (Ctrl+V)"));
    toolbarLayout->addWidget(btnPaste);

    QClipboard *clipboard = QGuiApplication::clipboard();
    const auto updatePasteAvailability = [clipboard, btnPaste] {
        const QMimeData *mimeData = clipboard ? clipboard->mimeData() : nullptr;
        btnPaste->setEnabled(mimeData && mimeData->hasImage());
    };
    const auto pasteLayer = [this, clipboard, btnMove] {
        if (!clipboard)
            return;
        const QMimeData *mimeData = clipboard->mimeData();
        if (!mimeData || !mimeData->hasImage())
            return;
        if (m_canvas->pasteImage(clipboard->image())) {
            btnMove->setChecked(true);
            m_canvas->setTool(MarkupTool::Move);
        }
    };
    connect(btnPaste, &QToolButton::clicked, this, pasteLayer);
    if (clipboard) {
        connect(clipboard, &QClipboard::dataChanged, this, updatePasteAvailability);
        updatePasteAvailability();
    }
    auto *pasteShortcut = new QShortcut(QKeySequence::Paste, this);
    pasteShortcut->setContext(Qt::WindowShortcut);
    connect(pasteShortcut, &QShortcut::activated, this, pasteLayer);

    auto *textSize = new QComboBox(this);
    textSize->setToolTip(
        QStringLiteral("Text size for new labels. Drag a selected label's lower-right handle to resize it."));
    textSize->addItem(QStringLiteral("Text: S"), 14);
    textSize->addItem(QStringLiteral("Text: M"), 18);
    textSize->addItem(QStringLiteral("Text: L"), 24);
    textSize->addItem(QStringLiteral("Text: XL"), 32);
    textSize->setCurrentIndex(1);
    connect(textSize, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, textSize](int) {
                m_canvas->setTextPointSize(textSize->currentData().toInt());
            });
    toolbarLayout->addWidget(textSize);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep);

    const QList<QColor> colors = {
        QColor(255,  50,  50), // red (default)
        QColor(255, 165,   0), // orange
        QColor(255, 230,   0), // yellow
        QColor( 50, 200,  80), // green
        QColor( 50, 130, 255), // blue
        QColor(200,  50, 255), // purple
        QColor(  0,   0,   0), // black
        QColor(255, 255, 255), // white
    };
    auto *colorGroup = new QButtonGroup(this);
    bool firstColor  = true;
    for (const QColor &c : colors) {
        auto *swatch = makeColorSwatch(c, this);
        swatch->setChecked(firstColor);
        colorGroup->addButton(swatch);
        toolbarLayout->addWidget(swatch);
        const QColor cap = c;
        connect(swatch, &QToolButton::clicked, this,
                [this, cap] { m_canvas->setColor(cap); });
        firstColor = false;
    }

    toolbarLayout->addStretch();

    auto *btnUndo = new QToolButton(this);
    btnUndo->setText(QStringLiteral("Undo"));
    btnUndo->setObjectName("ghostButton");
    connect(btnUndo, &QToolButton::clicked, this, [this] { m_canvas->undo(); });
    toolbarLayout->addWidget(btnUndo);
    auto *undoShortcut = new QShortcut(QKeySequence::Undo, this);
    undoShortcut->setContext(Qt::WindowShortcut);
    connect(undoShortcut, &QShortcut::activated,
            this, [this] { m_canvas->undo(); });

    auto *footer = new QWidget(this);
    footer->setFixedHeight(kFooterH);
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(8, 6, 8, 6);

    auto *btnDiscard = new QPushButton(QStringLiteral("Discard"), this);
    btnDiscard->setObjectName("ghostButton");
    connect(btnDiscard, &QPushButton::clicked, this, &QDialog::reject);

    auto *btnAdd = new QPushButton(QStringLiteral("Add to Prompt"), this);
    btnAdd->setObjectName("primaryButton");
    btnAdd->setDefault(true);
    connect(btnAdd, &QPushButton::clicked, this,
            &ScreenshotMarkupWindow::onAccept);

    footerLayout->addStretch();
    auto *btnCopy = new QPushButton(QStringLiteral("Copy screenshot"), this);
    btnCopy->setObjectName("ghostButton");
    btnCopy->setToolTip(
        QStringLiteral("Copy the screenshot with its current annotations"));
    connect(btnCopy, &QPushButton::clicked, this, [this, btnCopy] {
        QGuiApplication::clipboard()->setImage(m_canvas->flattenedImage());
        btnCopy->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1200, btnCopy, [btnCopy] {
            btnCopy->setText(QStringLiteral("Copy screenshot"));
        });
    });
    footerLayout->addWidget(btnCopy);
    footerLayout->addWidget(btnDiscard);
    footerLayout->addWidget(btnAdd);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addWidget(scrollArea, 1);
    root->addWidget(footer);
}

void ScreenshotMarkupWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        // An accidental Escape must not discard an annotation session. The
        // visible Discard button remains the intentional way out.
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ScreenshotMarkupWindow::onAccept()
{
    emit imageAccepted(m_canvas->flattenedImage());
    accept();
}
