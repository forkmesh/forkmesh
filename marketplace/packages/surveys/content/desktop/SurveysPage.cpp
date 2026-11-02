#include "SurveysPage.h"
#include "SurveysModel.h"

#include "DesignTokens.h"
#include "LayoutMode.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace forkmesh::surveys {

namespace {

constexpr char kApiPrefix[] = "/api/surveys";
constexpr char kWebPath[] = "/dashboard/surveys";
constexpr int kRowHeight = 26;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kResponsePage = 50;

const QString kDot = QString::fromUtf8(" \xC2\xB7 ");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QIcon tintedIcon(const QString &name, const QColor &color, int side = 14)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    QSvgRenderer renderer(path);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        if (renderer.isValid()) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&painter, QRectF(0, 0, side, side));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(QRectF(0, 0, side, side), color);
        }
        icon.addPixmap(pixmap);
    }
    return icon;
}

QLabel *mutedLabel(const QString &value = QString())
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *plainLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel *headingLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("surveysHeading"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QPushButton *ghostButton(const QString &label, const QString &icon = QString())
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty())
        button->setIcon(tintedIcon(icon, Theme::color(Theme::Role::FgMuted)));
    return button;
}

QPushButton *primaryButton(const QString &label)
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("primaryButton"));
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QToolButton *iconButton(const QString &icon, const QString &label, const QColor &color)
{
    auto *button = new QToolButton;
    button->setAutoRaise(true);
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    button->setIcon(tintedIcon(icon, color));
    return button;
}

Theme::Role statusInk(const QString &key)
{
    if (key == QLatin1String("open"))
        return Theme::Role::Success;
    if (key == QLatin1String("scheduled"))
        return Theme::Role::AccentFg;
    if (key == QLatin1String("draft"))
        return Theme::Role::Warning;
    return Theme::Role::FgMuted;
}

Theme::Role statusFill(const QString &key)
{
    if (key == QLatin1String("open"))
        return Theme::Role::SuccessSubtle;
    if (key == QLatin1String("scheduled"))
        return Theme::Role::AccentSubtle;
    if (key == QLatin1String("draft"))
        return Theme::Role::WarningSubtle;
    return Theme::Role::SurfaceActive;
}

QLabel *statusChip(const QString &key)
{
    auto *chip = new QLabel(statusLabel(key));
    chip->setObjectName(QStringLiteral("surveysStatusChip"));
    chip->setStyleSheet(QStringLiteral("QLabel#surveysStatusChip { color: %1; background: %2; border-radius: 4px;"
                                       " padding: 1px 6px; font-size: 11px; font-weight: 600; }")
                            .arg(Theme::hex(statusInk(key)), Theme::hex(statusFill(key))));
    chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return chip;
}

QTableWidgetItem *cell(const QString &value, Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter)
{
    auto *item = new QTableWidgetItem(value);
    item->setTextAlignment(align);
    item->setToolTip(value);
    return item;
}

QPlainTextEdit *textBox(const QString &value, int rows)
{
    auto *edit = new QPlainTextEdit(value);
    edit->setTabChangesFocus(true);
    edit->setFixedHeight(edit->fontMetrics().lineSpacing() * rows + 14);
    return edit;
}

QFrame *separator()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setObjectName(QStringLiteral("surveysSeparator"));
    return line;
}

QLabel *dialogError()
{
    auto *label = new QLabel;
    label->setObjectName(QStringLiteral("errorLabel"));
    label->setWordWrap(true);
    label->hide();
    return label;
}

void showDialogError(QLabel *label, const QString &value)
{
    label->setText(value);
    label->setVisible(!value.isEmpty());
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        if (QLayout *child = item->layout())
            clearLayout(child);
        delete item;
    }
}

void addBarRow(QGridLayout *grid, const ResultLine &line, qint64 total)
{
    const int row = grid->rowCount();
    QLabel *label = plainLabel(line.label);
    grid->addWidget(label, row, 0);
    auto *bar = new QProgressBar;
    bar->setObjectName(QStringLiteral("surveysBar"));
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    bar->setRange(0, int(std::max<qint64>(1, total)));
    bar->setValue(int(std::min<qint64>(line.count, std::max<qint64>(1, total))));
    bar->setAccessibleName(resultLineText(line));
    bar->setMinimumWidth(80);
    grid->addWidget(bar, row, 1);
    QLabel *count = mutedLabel(QStringLiteral("%1%2%3%").arg(line.count).arg(kDot).arg(line.percent));
    count->setWordWrap(false);
    count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(count, row, 2);
}

QDateTime nextHour()
{
    QDateTime value = QDateTime::currentDateTime().addSecs(3600);
    value.setTime(QTime(value.time().hour(), 0));
    return value;
}

struct EditorCard {
    QFrame *frame = nullptr;
    QLabel *number = nullptr;
    QComboBox *type = nullptr;
    QCheckBox *required = nullptr;
    QToolButton *up = nullptr;
    QToolButton *down = nullptr;
    QToolButton *remove = nullptr;
    QLineEdit *title = nullptr;
    QLineEdit *help = nullptr;
    QWidget *optionsBox = nullptr;
    QPlainTextEdit *options = nullptr;
    QWidget *scaleBox = nullptr;
    QSpinBox *scale = nullptr;
    QString id;
};

struct EditorState {
    QList<Question> questions;
    QList<EditorCard> cards;
    QVBoxLayout *list = nullptr;
    QPushButton *add = nullptr;
    QLabel *empty = nullptr;
};

void readCards(EditorState &state)
{
    QList<Question> questions;
    for (const EditorCard &card : std::as_const(state.cards)) {
        Question question;
        question.id = card.id;
        question.type = card.type->currentData().toString();
        question.title = card.title->text();
        question.help = card.help->text();
        question.required = card.required->isChecked();
        question.options = optionsFromText(card.options->toPlainText());
        question.scale = card.scale->value();
        questions.append(question);
    }
    state.questions = questions;
}

int cardIndex(const EditorState &state, const QFrame *frame)
{
    for (int index = 0; index < state.cards.size(); ++index) {
        if (state.cards.at(index).frame == frame)
            return index;
    }
    return -1;
}

void showTypeFields(const EditorCard &card)
{
    const QString type = card.type->currentData().toString();
    card.optionsBox->setVisible(isChoiceType(type));
    card.scaleBox->setVisible(isRatingType(type));
}

void buildCards(const std::shared_ptr<EditorState> &state, int focusIndex = -1)
{
    for (const EditorCard &card : std::as_const(state->cards)) {
        state->list->removeWidget(card.frame);
        card.frame->hide();
        card.frame->deleteLater();
    }
    state->cards.clear();
    const QColor ink = Theme::color(Theme::Role::FgMuted);
    const std::weak_ptr<EditorState> weak = state;
    for (int index = 0; index < state->questions.size(); ++index) {
        const Question &question = state->questions.at(index);
        EditorCard card;
        card.id = question.id;
        card.frame = new QFrame;
        card.frame->setObjectName(QStringLiteral("surveysQuestionCard"));
        auto *layout = new QVBoxLayout(card.frame);
        layout->setContentsMargins(10, 8, 10, 10);
        layout->setSpacing(6);
        auto *header = new QHBoxLayout;
        header->setSpacing(6);
        card.number = headingLabel(QStringLiteral("Question %1").arg(index + 1));
        card.number->setWordWrap(false);
        header->addWidget(card.number);
        card.type = new QComboBox;
        card.type->setAccessibleName(QStringLiteral("Question type"));
        for (const QString &key : questionTypeKeys())
            card.type->addItem(questionTypeLabel(key), key);
        card.type->setCurrentIndex(std::max(0, card.type->findData(question.type)));
        header->addWidget(card.type);
        card.required = new QCheckBox(QStringLiteral("Required"));
        card.required->setChecked(question.required);
        header->addWidget(card.required);
        header->addStretch(1);
        card.up = iconButton(QStringLiteral("chevron-up"), QStringLiteral("Move up"), ink);
        card.down = iconButton(QStringLiteral("chevron-down"), QStringLiteral("Move down"), ink);
        card.remove = iconButton(QStringLiteral("trash"), QStringLiteral("Remove question"), ink);
        card.up->setEnabled(index > 0);
        card.down->setEnabled(index < state->questions.size() - 1);
        header->addWidget(card.up);
        header->addWidget(card.down);
        header->addWidget(card.remove);
        layout->addLayout(header);
        card.title = new QLineEdit(question.title);
        card.title->setMaxLength(kTitleLimit);
        card.title->setPlaceholderText(QStringLiteral("Question"));
        card.title->setAccessibleName(QStringLiteral("Question %1 title").arg(index + 1));
        layout->addWidget(card.title);
        card.help = new QLineEdit(question.help);
        card.help->setMaxLength(kHelpLimit);
        card.help->setPlaceholderText(QStringLiteral("Help text under the question (optional)"));
        card.help->setAccessibleName(QStringLiteral("Question %1 help").arg(index + 1));
        layout->addWidget(card.help);
        card.optionsBox = new QWidget;
        auto *optionsLayout = new QVBoxLayout(card.optionsBox);
        optionsLayout->setContentsMargins(0, 0, 0, 0);
        optionsLayout->setSpacing(2);
        optionsLayout->addWidget(mutedLabel(
            QStringLiteral("Options, one per line (%1 to %2)").arg(kMinOptions).arg(kMaxOptions)));
        card.options = textBox(question.options.join(QLatin1Char('\n')), 4);
        card.options->setTabChangesFocus(false);
        card.options->setPlaceholderText(QStringLiteral("First option\nSecond option"));
        card.options->setAccessibleName(QStringLiteral("Question %1 options").arg(index + 1));
        optionsLayout->addWidget(card.options);
        layout->addWidget(card.optionsBox);
        card.scaleBox = new QWidget;
        auto *scaleLayout = new QHBoxLayout(card.scaleBox);
        scaleLayout->setContentsMargins(0, 0, 0, 0);
        scaleLayout->addWidget(new QLabel(QStringLiteral("Rating from 1 to")));
        card.scale = new QSpinBox;
        card.scale->setRange(kMinScale, kMaxScale);
        card.scale->setValue(question.scale >= kMinScale && question.scale <= kMaxScale ? question.scale
                                                                                         : kDefaultScale);
        card.scale->setAccessibleName(QStringLiteral("Question %1 scale").arg(index + 1));
        scaleLayout->addWidget(card.scale);
        scaleLayout->addStretch(1);
        layout->addWidget(card.scaleBox);
        showTypeFields(card);
        state->list->addWidget(card.frame);
        const EditorCard copy = card;
        QObject::connect(card.type, &QComboBox::currentIndexChanged, card.frame, [copy] { showTypeFields(copy); });
        QFrame *frame = card.frame;
        const auto move = [weak, frame](int delta) {
            const std::shared_ptr<EditorState> current = weak.lock();
            if (!current)
                return;
            readCards(*current);
            const int at = cardIndex(*current, frame);
            if (moveQuestion(current->questions, at, delta))
                buildCards(current, at + delta);
        };
        QObject::connect(card.up, &QToolButton::clicked, card.frame, [move] { move(-1); });
        QObject::connect(card.down, &QToolButton::clicked, card.frame, [move] { move(1); });
        QObject::connect(card.remove, &QToolButton::clicked, card.frame, [weak, frame] {
            const std::shared_ptr<EditorState> current = weak.lock();
            if (!current)
                return;
            readCards(*current);
            const int at = cardIndex(*current, frame);
            if (at < 0)
                return;
            current->questions.removeAt(at);
            buildCards(current, std::min<int>(at, int(current->questions.size()) - 1));
        });
        state->cards.append(card);
    }
    state->add->setEnabled(state->questions.size() < kMaxQuestions);
    state->empty->setVisible(state->questions.isEmpty());
    if (focusIndex >= 0 && focusIndex < state->cards.size()) {
        QLineEdit *title = state->cards.at(focusIndex).title;
        QTimer::singleShot(0, title, [title] { title->setFocus(); });
    }
}

}

SurveysPage::SurveysPage(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host), m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("surveysSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(next ? 24 : 16, 12, next ? 24 : 16, 12);
    outer->setSpacing(8);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(6);
    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Surveys"));
    title->setObjectName(QStringLiteral("pageTitle"));
    titleBlock->addWidget(title);
    m_status = mutedLabel(QStringLiteral("Loading Surveys…"));
    titleBlock->addWidget(m_status);
    heading->addLayout(titleBlock, 1);
    m_organization = new QComboBox;
    m_organization->setObjectName(QStringLiteral("surveysOrganization"));
    m_organization->setAccessibleName(QStringLiteral("Organization"));
    m_organization->setMinimumWidth(170);
    m_organization->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    heading->addWidget(m_organization, 0, Qt::AlignTop);
    m_newButton = primaryButton(QStringLiteral("New survey"));
    m_refreshButton = ghostButton(QStringLiteral("Refresh"), QStringLiteral("sync"));
    m_webButton = ghostButton(QStringLiteral("Open on the web"), QStringLiteral("link-external"));
    m_webButton->setToolTip(QStringLiteral("Open the Surveys page for this organization in your browser"));
    for (QPushButton *button : {m_newButton, m_refreshButton, m_webButton})
        heading->addWidget(button, 0, Qt::AlignTop);
    outer->addLayout(heading);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    m_installNote = new QLabel;
    m_installNote->setWordWrap(true);
    m_installNote->setAlignment(Qt::AlignHCenter);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch(1);
    m_installButton = primaryButton(QStringLiteral("Install Surveys"));
    installRow->addWidget(m_installButton);
    installRow->addStretch(1);
    installLayout->addLayout(installRow);
    installLayout->addStretch(1);
    outer->addWidget(m_installPanel, 1);

    m_content = new QWidget;
    auto *content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(10);
    auto *tiles = new QGridLayout;
    tiles->setHorizontalSpacing(10);
    const QStringList tileNames{QStringLiteral("Surveys"), QStringLiteral("Open"), QStringLiteral("Responses"),
                                QStringLiteral("Last 7 days")};
    for (int column = 0; column < tileNames.size(); ++column) {
        auto *tile = new QFrame;
        tile->setObjectName(QStringLiteral("surveysTile"));
        auto *tileLayout = new QVBoxLayout(tile);
        tileLayout->setSpacing(2);
        QLabel *name = mutedLabel(tileNames.at(column));
        name->setWordWrap(false);
        auto *value = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
        value->setObjectName(QStringLiteral("surveysTileValue"));
        value->setAccessibleName(tileNames.at(column));
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QLabel *detail = mutedLabel();
        tileLayout->addWidget(name);
        tileLayout->addWidget(value);
        tileLayout->addWidget(detail);
        tileLayout->addStretch(1);
        tiles->addWidget(tile, 0, column);
        tiles->setColumnStretch(column, 1);
        m_tiles.append(tile);
        m_tileValues.append(value);
        m_tileDetails.append(detail);
    }
    content->addLayout(tiles);

    m_listPanel = new QWidget;
    m_listPanel->setObjectName(QStringLiteral("surveysListPanel"));
    auto *listLayout = new QVBoxLayout(m_listPanel);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(6);
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("surveysSearch"));
    m_search->setPlaceholderText(QStringLiteral("Search survey titles"));
    m_search->setAccessibleName(QStringLiteral("Search surveys"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(tintedIcon(QStringLiteral("search"), Theme::color(Theme::Role::FgMuted)),
                        QLineEdit::LeadingPosition);
    filters->addWidget(m_search, 1);
    m_filter = new QComboBox;
    m_filter->setObjectName(QStringLiteral("surveysStatusFilter"));
    m_filter->setAccessibleName(QStringLiteral("Status"));
    for (const QString &key : filterKeys())
        m_filter->addItem(filterLabel(key), key);
    filters->addWidget(m_filter);
    listLayout->addLayout(filters);
    m_table = new QTableWidget(0, 5);
    m_table->setObjectName(QStringLiteral("surveysTable"));
    m_table->setAccessibleName(QStringLiteral("Surveys"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Survey"), QStringLiteral("Status"),
                                        QStringLiteral("Questions"), QStringLiteral("Responses"),
                                        QStringLiteral("Last response")});
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(kRowHeight);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setHighlightSections(false);
    listLayout->addWidget(m_table, 1);
    m_emptyNote = mutedLabel(QStringLiteral("No surveys match."));
    m_emptyNote->setAlignment(Qt::AlignHCenter);
    listLayout->addWidget(m_emptyNote);

    m_detailPanel = new QWidget;
    m_detailPanel->setObjectName(QStringLiteral("surveysDetailPanel"));
    auto *detailLayout = new QVBoxLayout(m_detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(6);
    m_backButton = ghostButton(QStringLiteral("All surveys"), QStringLiteral("arrow-left"));
    m_backButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    detailLayout->addWidget(m_backButton);
    m_detailPlaceholder = mutedLabel(QStringLiteral("Select a survey to see its results and responses."));
    m_detailPlaceholder->setAlignment(Qt::AlignCenter);
    detailLayout->addWidget(m_detailPlaceholder, 1);
    m_detailScroll = new QScrollArea;
    m_detailScroll->setWidgetResizable(true);
    m_detailScroll->setFrameShape(QFrame::NoFrame);
    detailLayout->addWidget(m_detailScroll, 1);
    m_detailActions = new QWidget;
    m_detailActions->setObjectName(QStringLiteral("surveysDetailActions"));
    auto *actionsLayout = new QVBoxLayout(m_detailActions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(4);
    detailLayout->addWidget(m_detailActions);

    m_modeStack = new QStackedWidget;
    m_towerSplit = new QSplitter(Qt::Horizontal);
    m_towerSplit->setObjectName(QStringLiteral("surveysTowerSplit"));
    m_towerSplit->setChildrenCollapsible(false);
    m_modeStack->addWidget(m_towerSplit);
    m_focusStack = new QStackedWidget;
    m_focusStack->setObjectName(QStringLiteral("surveysFocusStack"));
    m_focusList = new QWidget;
    new QVBoxLayout(m_focusList);
    m_focusList->layout()->setContentsMargins(0, 0, 0, 0);
    m_focusStack->addWidget(m_focusList);
    m_focusDetail = new QWidget;
    new QVBoxLayout(m_focusDetail);
    m_focusDetail->layout()->setContentsMargins(0, 0, 0, 0);
    m_focusStack->addWidget(m_focusDetail);
    m_modeStack->addWidget(m_focusStack);
    content->addWidget(m_modeStack, 1);
    outer->addWidget(m_content, 1);

    connect(m_organization, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        if (m_host.setSetting)
            m_host.setSetting(QStringLiteral("organization"), organization());
        m_overview = QJsonObject();
        m_surveys.clear();
        closeDetail();
        renderSummary();
        renderList();
        loadOverview();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this] { renderList(); });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { renderList(); });
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_table->item(row, 0))
            openSurvey(item->data(kIdRole).toString());
    });
    connect(m_table, &QTableWidget::cellActivated, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_table->item(row, 0))
            openSurvey(item->data(kIdRole).toString());
    });
    connect(m_backButton, &QPushButton::clicked, this, [this] { closeDetail(); });
    connect(m_newButton, &QPushButton::clicked, this, [this] { newSurvey(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_webButton, &QPushButton::clicked, this, [this] { openWebPage(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });

    arrange(forkmesh::ui::layoutControlTower());
    applyStyle();
    applyState();
}

void SurveysPage::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                          ReplyHandler handler)
{
    if (!m_host.request) {
        handler(false, {}, QStringLiteral("Surveys network access is unavailable."));
        return;
    }
    const QPointer<SurveysPage> guard(this);
    m_host.request(method, path, body,
                   [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                         const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString SurveysPage::organization() const
{
    return m_organization->currentData().toString();
}

QString SurveysPage::organizationPath(const QString &suffix) const
{
    return QString::fromLatin1(kApiPrefix) + QLatin1Char('/') + organization() + suffix;
}

QString SurveysPage::surveyPath(const QString &id, const QString &suffix) const
{
    return organizationPath(QStringLiteral("/items/") + id + suffix);
}

QJsonObject SurveysPage::organizationRecord() const
{
    const QString name = organization();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

bool SurveysPage::installed() const
{
    return organizationRecord().value(QStringLiteral("installed")).toBool();
}

bool SurveysPage::canManage() const
{
    if (m_overview.contains(QStringLiteral("canManagePlugin")))
        return m_overview.value(QStringLiteral("canManagePlugin")).toBool();
    const QString role = text(organizationRecord(), "role");
    return role == QLatin1String("owner") || role == QLatin1String("admin");
}

int SurveysPage::surveyIndex(const QString &id) const
{
    for (int index = 0; index < m_surveys.size(); ++index) {
        if (m_surveys.at(index).id == id)
            return index;
    }
    return -1;
}

void SurveysPage::refresh()
{
    arrange(forkmesh::ui::layoutControlTower());
    m_responsesFor.clear();
    refreshOrganizations();
}

void SurveysPage::prepareNavigationBadge()
{
    if (!m_loaded && !m_loading)
        refreshOrganizations();
}

int SurveysPage::navigationBadge() const
{
    return installed() ? openCount(m_surveys) : 0;
}

void SurveysPage::themeChanged()
{
    applyStyle();
    renderList();
    renderDetail();
}

void SurveysPage::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    arrange(forkmesh::ui::layoutControlTower());
}

void SurveysPage::setHostSearchQuery(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed == m_hostQuery)
        return;
    m_hostQuery = trimmed;
    m_search->setText(trimmed);
}

QList<forkmesh::market::NativePackageSearchHit> SurveysPage::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0 || !installed())
        return hits;
    for (const Survey &survey : m_surveys) {
        if (!matchesQuery(survey, needle))
            continue;
        const QStringList detail{statusLabel(statusKey(survey)),
                                 countPhrase(survey.responseCount, QStringLiteral("response")),
                                 QStringLiteral("Surveys in ") + organization()};
        hits.append({survey.id, surveyTitle(survey), detail.join(kDot)});
        if (hits.size() >= limit)
            break;
    }
    return hits;
}

void SurveysPage::activateSearchHit(const QString &id)
{
    if (m_host.showPage)
        m_host.showPage();
    openSurvey(id);
}

bool SurveysPage::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path =
        forkmesh::market::nativePackageLifecyclePath(organization(), m_host.packageId, QStringLiteral("export"));
    if (path.isEmpty() || !installed())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

void SurveysPage::refreshOrganizations()
{
    if (m_loading)
        return;
    m_loading = true;
    QString previous = organization();
    if (previous.isEmpty() && m_host.setting)
        previous = m_host.setting(QStringLiteral("organization"), QString()).toString();
    setStatus(QStringLiteral("Loading your organizations…"));
    request(QByteArrayLiteral("GET"), QString::fromLatin1(kApiPrefix), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            applyState();
            return setStatus(QStringLiteral("Surveys could not load: %1").arg(readableError(error)), true);
        }
        m_loaded = true;
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        int preferred = -1;
        m_organization->blockSignals(true);
        m_organization->clear();
        for (const QJsonValue &value : std::as_const(m_organizations)) {
            const QJsonObject item = value.toObject();
            QString label = text(item, "name") + kDot + text(item, "role");
            if (!item.value(QStringLiteral("installed")).toBool())
                label += kDot + QStringLiteral("not installed");
            else if (preferred < 0)
                preferred = m_organization->count();
            m_organization->addItem(label, text(item, "name"));
        }
        const int index = m_organization->findData(previous);
        m_organization->setCurrentIndex(index >= 0 ? index : std::max(0, preferred));
        m_organization->blockSignals(false);
        if (!m_organization->count()) {
            m_overview = QJsonObject();
            m_surveys.clear();
            applyState();
            return setStatus(QStringLiteral("Join or create an organization before using Surveys."), true);
        }
        loadOverview();
    });
}

void SurveysPage::loadOverview()
{
    if (organization().isEmpty())
        return;
    if (!installed()) {
        m_overview = QJsonObject();
        m_surveys.clear();
        closeDetail();
        applyState();
        if (m_host.navigationBadgeChanged)
            m_host.navigationBadgeChanged();
        return setStatus(QStringLiteral("Surveys is not installed for %1.").arg(organization()));
    }
    const int serial = ++m_serial;
    const QString name = organization();
    setStatus(QStringLiteral("Loading surveys for %1…").arg(name));
    request(QByteArrayLiteral("GET"), organizationPath(), {},
            [this, serial, name](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial || name != organization())
            return;
        if (!ok) {
            if (!notInstalledError(payload).isEmpty()) {
                for (int index = 0; index < m_organizations.size(); ++index) {
                    QJsonObject item = m_organizations.at(index).toObject();
                    if (text(item, "name") == name) {
                        item.insert(QStringLiteral("installed"), false);
                        m_organizations.replace(index, item);
                    }
                }
                m_overview = QJsonObject();
                m_surveys.clear();
                closeDetail();
                applyState();
                if (m_host.navigationBadgeChanged)
                    m_host.navigationBadgeChanged();
                return setStatus(QStringLiteral("Surveys is not installed for %1.").arg(name));
            }
            applyState();
            return setStatus(QStringLiteral("Surveys is unavailable: %1").arg(readableError(error)), true);
        }
        applyOverview(payload);
    });
}

void SurveysPage::applyOverview(const QJsonObject &payload)
{
    m_overview = payload;
    m_surveys = parseSurveys(payload.value(QStringLiteral("surveys")).toArray());
    const Summary summary = parseSummary(payload.value(QStringLiteral("summary")).toObject());
    QStringList parts{countPhrase(m_surveys.size(), QStringLiteral("survey"))};
    const int taking = openCount(m_surveys);
    if (taking > 0)
        parts << QStringLiteral("%1 taking answers").arg(taking);
    if (summary.drafts > 0)
        parts << countPhrase(summary.drafts, QStringLiteral("draft"));
    parts << QStringLiteral("shared with every member of %1").arg(organization());
    setStatus(parts.join(kDot));
    applyState();
    renderSummary();
    renderList();
    if (!m_detailId.isEmpty()) {
        if (surveyIndex(m_detailId) < 0)
            closeDetail();
        else
            openSurvey(m_detailId);
    }
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void SurveysPage::applyState()
{
    const QJsonObject record = organizationRecord();
    const bool listed = !record.isEmpty();
    const bool ready = installed() && m_overview.contains(QStringLiteral("surveys"));
    m_installPanel->setVisible(listed && !installed());
    m_installButton->setVisible(canManage());
    m_installNote->setText(canManage()
        ? QStringLiteral("Surveys lets every member of %1 build surveys with choice, text and rating questions, "
                         "share a private link anyone can answer, and read the results here, on the web and on "
                         "phones. Install it for %1 to start.").arg(organization())
        : QStringLiteral("Ask an owner or admin of %1 to install Surveys.").arg(organization()));
    m_content->setVisible(ready);
    m_newButton->setVisible(ready);
    m_organization->setVisible(m_organization->count() > 0);
}

void SurveysPage::arrange(bool controlTower)
{
    const int mode = controlTower ? 1 : 0;
    if (mode == m_arranged)
        return;
    m_arranged = mode;
    if (controlTower) {
        m_towerSplit->addWidget(m_listPanel);
        m_towerSplit->addWidget(m_detailPanel);
        m_towerSplit->setStretchFactor(0, 3);
        m_towerSplit->setStretchFactor(1, 2);
        m_towerSplit->setSizes({560, 460});
        m_modeStack->setCurrentWidget(m_towerSplit);
    } else {
        m_focusList->layout()->addWidget(m_listPanel);
        m_focusDetail->layout()->addWidget(m_detailPanel);
        m_modeStack->setCurrentWidget(m_focusStack);
        m_focusStack->setCurrentWidget(m_detailId.isEmpty() ? m_focusList : m_focusDetail);
    }
    for (QWidget *panel : {m_listPanel, m_detailPanel})
        panel->show();
    m_backButton->setVisible(!controlTower);
    for (QFrame *tile : std::as_const(m_tiles))
        tile->layout()->setContentsMargins(controlTower ? 10 : 16, controlTower ? 6 : 12, controlTower ? 10 : 16,
                                           controlTower ? 6 : 12);
    applyStyle();
    renderDetail();
}

void SurveysPage::applyStyle()
{
    const bool tower = m_arranged == 1;
    const QString surface = Theme::hex(Theme::Role::Surface);
    const QString border = Theme::hex(Theme::Role::Border);
    m_content->setStyleSheet(
        QStringLiteral("QFrame#surveysTile { background: %1; border: 1px solid %2; border-radius: 8px; }"
                       "QFrame#surveysTile QLabel { background: transparent; border: none; }"
                       "QLabel#surveysTileValue { font-size: %3px; font-weight: 600; color: %4; }"
                       "QProgressBar#surveysBar { background: %5; border: none; border-radius: 4px; }"
                       "QProgressBar#surveysBar::chunk { background: %6; border-radius: 4px; }"
                       "QFrame#surveysSeparator { color: %2; background: %2; max-height: 1px; border: none; }")
            .arg(surface, border, QString::number(tower ? 15 : 20), Theme::hex(Theme::Role::Fg),
                 Theme::hex(Theme::Role::SurfaceActive), Theme::hex(Theme::Role::Accent)));
    const QColor ink = Theme::color(Theme::Role::FgMuted);
    m_refreshButton->setIcon(tintedIcon(QStringLiteral("sync"), ink));
    m_webButton->setIcon(tintedIcon(QStringLiteral("link-external"), ink));
    m_backButton->setIcon(tintedIcon(QStringLiteral("arrow-left"), ink));
}

void SurveysPage::renderSummary()
{
    const Summary summary = parseSummary(m_overview.value(QStringLiteral("summary")).toObject());
    const int taking = openCount(m_surveys);
    const QStringList values{QString::number(summary.surveys), QString::number(summary.open),
                             QString::number(summary.responses), QString::number(summary.responsesLast7Days)};
    const QStringList details{
        countPhrase(summary.drafts, QStringLiteral("draft")) + kDot + QStringLiteral("%1 closed").arg(summary.closed),
        QStringLiteral("%1 taking answers now").arg(taking),
        QStringLiteral("across every survey"),
        QStringLiteral("new responses this week"),
    };
    for (int index = 0; index < m_tileValues.size(); ++index) {
        m_tileValues.at(index)->setText(values.at(index));
        m_tileValues.at(index)->setToolTip(values.at(index));
        m_tileDetails.at(index)->setText(details.at(index));
    }
}

void SurveysPage::renderList()
{
    const QString query = m_search->text().trimmed();
    const QString filter = m_filter->currentData().toString();
    const qint64 now = nowMs();
    m_table->setRowCount(0);
    for (const Survey &survey : std::as_const(m_surveys)) {
        if (!matchesFilter(survey, filter) || !matchesQuery(survey, query))
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *name = cell(surveyTitle(survey));
        name->setData(kIdRole, survey.id);
        QFont bold = name->font();
        bold.setWeight(QFont::DemiBold);
        name->setFont(bold);
        m_table->setItem(row, 0, name);
        const QString key = statusKey(survey);
        auto *status = cell(statusLabel(key));
        status->setForeground(Theme::color(statusInk(key)));
        status->setToolTip(scheduleText(survey, now));
        m_table->setItem(row, 1, status);
        m_table->setItem(row, 2, cell(QString::number(survey.questionCount), Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(row, 3, cell(survey.settings.maxResponses > 0
                                          ? QStringLiteral("%1 / %2").arg(survey.responseCount).arg(survey.settings.maxResponses)
                                          : QString::number(survey.responseCount),
                                      Qt::AlignRight | Qt::AlignVCenter));
        auto *last = cell(survey.lastResponseAt > 0 ? relativeText(now, survey.lastResponseAt)
                                                    : QString::fromUtf8("\xE2\x80\x94"));
        if (survey.lastResponseAt > 0)
            last->setToolTip(dateTimeText(survey.lastResponseAt));
        m_table->setItem(row, 4, last);
        if (survey.id == m_detailId)
            m_table->selectRow(row);
    }
    const bool none = m_table->rowCount() == 0;
    m_emptyNote->setText(m_surveys.isEmpty() ? QStringLiteral("No surveys yet. Press New survey to write one.")
                                             : QStringLiteral("No surveys match."));
    m_emptyNote->setVisible(none);
}

void SurveysPage::openSurvey(const QString &id)
{
    const int index = surveyIndex(id);
    if (id.isEmpty() || index < 0)
        return;
    const Survey &cached = m_surveys.at(index);
    if (m_detailId != id) {
        m_responses.clear();
        m_responsesFor.clear();
        m_responsesError.clear();
        m_responseLimit = kResponsePage;
        m_responsesLoading = false;
        ++m_responsesSerial;
        m_detail = cached;
    } else if (m_detail.responseCount != cached.responseCount) {
        m_responsesFor.clear();
    }
    if (m_detail.id != id || !m_detail.detailed)
        m_detail = cached;
    m_detailId = id;
    if (m_arranged == 0)
        m_focusStack->setCurrentWidget(m_focusDetail);
    renderDetail();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0) && m_table->item(row, 0)->data(kIdRole).toString() == id) {
            m_table->selectRow(row);
            m_table->scrollToItem(m_table->item(row, 0));
        }
    }
    const int serial = ++m_detailSerial;
    request(QByteArrayLiteral("GET"), surveyPath(id), {},
            [this, serial, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_detailSerial || id != m_detailId)
            return;
        if (!ok)
            return setStatus(QStringLiteral("Could not open the survey: %1").arg(readableError(error)), true);
        const Survey loaded = parseSurvey(payload.value(QStringLiteral("survey")).toObject());
        if (loaded.id != id)
            return;
        if (loaded.responseCount != m_detail.responseCount)
            m_responsesFor.clear();
        m_detail = loaded;
        m_detail.detailed = true;
        renderDetail();
        if (m_detailTab == 1 && m_responsesFor != id && !m_responsesLoading)
            loadResponses();
    });
    if (m_detailTab == 1 && m_responsesFor != id && !m_responsesLoading)
        loadResponses();
}

void SurveysPage::closeDetail()
{
    m_detailId.clear();
    m_detail = Survey();
    m_responses.clear();
    m_responsesFor.clear();
    m_responsesError.clear();
    ++m_detailSerial;
    ++m_responsesSerial;
    m_responsesLoading = false;
    if (m_table)
        m_table->clearSelection();
    if (m_arranged == 0)
        m_focusStack->setCurrentWidget(m_focusList);
    renderDetail();
}

void SurveysPage::loadResponses()
{
    const QString id = m_detailId;
    if (id.isEmpty())
        return;
    const int serial = ++m_responsesSerial;
    m_responsesLoading = true;
    m_responsesError.clear();
    request(QByteArrayLiteral("GET"), surveyPath(id, QStringLiteral("/responses")), {},
            [this, serial, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_responsesSerial || id != m_detailId)
            return;
        m_responsesLoading = false;
        if (!ok) {
            m_responsesError = QStringLiteral("Responses could not load: %1").arg(readableError(error));
        } else {
            m_responses = parseResponses(payload.value(QStringLiteral("responses")).toArray());
            m_responsesFor = id;
        }
        renderDetail();
    });
}

void SurveysPage::renderDetail()
{
    if (!m_detailScroll)
        return;
    const bool open = !m_detailId.isEmpty() && !m_detail.id.isEmpty();
    m_detailPlaceholder->setVisible(!open);
    m_detailScroll->setVisible(open);
    m_detailActions->setVisible(open);
    clearLayout(m_detailActions->layout());
    const int keepScroll = open && m_renderedId == m_detailId ? m_detailScroll->verticalScrollBar()->value() : 0;
    m_renderedId = open ? m_detailId : QString();
    if (!open) {
        if (QWidget *old = m_detailScroll->takeWidget())
            old->deleteLater();
        return;
    }
    const bool tower = m_arranged == 1;
    const Survey &survey = m_detail;
    const qint64 now = nowMs();
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("surveysDetailBody"));
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, tower ? 4 : 0, 0);
    layout->setSpacing(8);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    QLabel *title = plainLabel(surveyTitle(survey));
    title->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    titleRow->addWidget(title, 1);
    titleRow->addWidget(statusChip(statusKey(survey)), 0, Qt::AlignTop);
    layout->addLayout(titleRow);
    if (!survey.description.trimmed().isEmpty())
        layout->addWidget(plainLabel(survey.description));
    QStringList meta{responsesText(survey, now), countPhrase(survey.questionCount, QStringLiteral("question"))};
    if (!survey.createdBy.isEmpty())
        meta << QStringLiteral("by ") + survey.createdBy;
    if (survey.updatedAt > 0)
        meta << QStringLiteral("edited ") + relativeText(now, survey.updatedAt);
    layout->addWidget(mutedLabel(meta.join(kDot)));
    const QString schedule = scheduleText(survey, now);
    if (!schedule.isEmpty())
        layout->addWidget(mutedLabel(schedule));
    layout->addWidget(mutedLabel(settingsLines(survey.settings).join(kDot)));
    if (survey.detailed && survey.status != QLatin1String("draft"))
        layout->addWidget(mutedLabel(activityText(survey.activity, now)));

    if (canCopyLink(survey)) {
        auto *linkRow = new QHBoxLayout;
        linkRow->setSpacing(6);
        auto *link = new QLineEdit(survey.publicUrl);
        link->setObjectName(QStringLiteral("surveysPublicLink"));
        link->setAccessibleName(QStringLiteral("Survey link"));
        link->setReadOnly(true);
        link->setCursorPosition(0);
        linkRow->addWidget(link, 1);
        QPushButton *copy = ghostButton(QStringLiteral("Copy link"), QStringLiteral("copy"));
        QPushButton *preview = ghostButton(QStringLiteral("Open"), QStringLiteral("link-external"));
        linkRow->addWidget(copy);
        linkRow->addWidget(preview);
        layout->addLayout(linkRow);
        layout->addWidget(mutedLabel(QStringLiteral("Anyone with this link can answer while the survey is open, "
                                                    "without an account.")));
        connect(copy, &QPushButton::clicked, this, [this] { copyPublicLink(); });
        connect(preview, &QPushButton::clicked, this, [this] { openPublicLink(); });
    }

    if (!survey.detailed) {
        layout->addWidget(mutedLabel(QStringLiteral("Loading results…")));
    } else {
        auto *tabs = new QTabWidget;
        tabs->setObjectName(QStringLiteral("surveysDetailTabs"));
        tabs->setDocumentMode(true);
        tabs->addTab(resultsPage(), QStringLiteral("Results"));
        tabs->addTab(responsesPage(), QStringLiteral("Responses (%1)").arg(survey.responseCount));
        tabs->addTab(activityPage(), QStringLiteral("Activity"));
        tabs->setCurrentIndex(std::clamp(m_detailTab, 0, 2));
        connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
            m_detailTab = index;
            if (index == 1 && m_responsesFor != m_detailId && !m_responsesLoading)
                loadResponses();
        });
        layout->addWidget(tabs);
    }
    layout->addStretch(1);
    if (QWidget *old = m_detailScroll->takeWidget())
        old->deleteLater();
    m_detailScroll->setWidget(body);
    if (keepScroll > 0) {
        const QPointer<QScrollArea> scroll(m_detailScroll);
        QTimer::singleShot(0, this, [scroll, keepScroll] {
            if (scroll)
                scroll->verticalScrollBar()->setValue(keepScroll);
        });
    }

    auto *primary = new QHBoxLayout;
    primary->setSpacing(6);
    auto *secondary = new QHBoxLayout;
    secondary->setSpacing(6);
    addDetailActions(primary, secondary);
    auto *actionsLayout = static_cast<QVBoxLayout *>(m_detailActions->layout());
    actionsLayout->addLayout(primary);
    if (secondary->count())
        actionsLayout->addLayout(secondary);
    else
        delete secondary;
}

QWidget *SurveysPage::resultsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(6);
    const Survey &survey = m_detail;
    if (survey.questions.isEmpty()) {
        layout->addWidget(mutedLabel(QStringLiteral("This survey has no questions yet. Press Edit to add some.")));
        return page;
    }
    QHash<QString, QuestionResult> results;
    for (const QuestionResult &result : survey.results)
        results.insert(result.id, result);
    for (int index = 0; index < survey.questions.size(); ++index) {
        const Question &question = survey.questions.at(index);
        QuestionResult result = results.value(question.id);
        if (result.type.isEmpty())
            result.type = question.type;
        if (isRatingType(question.type) && result.distribution.isEmpty() && result.answered == 0)
            result.distribution = QList<qint64>(std::clamp(question.scale, kMinScale, kMaxScale), 0);
        if (index > 0)
            layout->addWidget(separator());
        layout->addWidget(headingLabel(QStringLiteral("%1. %2").arg(index + 1).arg(
            question.title.trimmed().isEmpty() ? QStringLiteral("Untitled question") : question.title)));
        QStringList kind{questionTypeLabel(question.type)};
        if (question.required)
            kind << QStringLiteral("required");
        kind << answeredText(result);
        layout->addWidget(mutedLabel(kind.join(kDot)));
        if (isChoiceType(question.type)) {
            auto *grid = new QGridLayout;
            grid->setHorizontalSpacing(8);
            grid->setVerticalSpacing(4);
            grid->setColumnStretch(1, 1);
            for (const ResultLine &line : optionLines(result))
                addBarRow(grid, line, result.answered);
            layout->addLayout(grid);
        } else if (isRatingType(question.type)) {
            QLabel *average = plainLabel(averageText(result));
            QFont font = average->font();
            font.setWeight(QFont::DemiBold);
            average->setFont(font);
            layout->addWidget(average);
            auto *grid = new QGridLayout;
            grid->setHorizontalSpacing(8);
            grid->setVerticalSpacing(4);
            grid->setColumnStretch(1, 1);
            for (const ResultLine &line : distributionLines(result))
                addBarRow(grid, line, result.answered);
            layout->addLayout(grid);
        } else {
            for (const QString &line : resultTextLines(result)) {
                const bool note = result.answers.isEmpty() || !result.answers.contains(line);
                layout->addWidget(note ? mutedLabel(line) : plainLabel(line));
            }
        }
    }
    return page;
}

QWidget *SurveysPage::activityPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(6);
    const SurveyActivity &activity = m_detail.activity;
    const qint64 now = nowMs();
    layout->addWidget(plainLabel(activityText(activity, now)));
    if (activity.recent.isEmpty()) {
        layout->addWidget(mutedLabel(m_detail.status == QLatin1String("draft")
            ? QStringLiteral("Open the survey and share its link; visits show up here.")
            : QStringLiteral("When someone opens the link, what they do shows up here. Opening it from this page "
                             "does not count.")));
        return page;
    }
    for (int index = 0; index < activity.recent.size(); ++index) {
        if (index > 0)
            layout->addWidget(separator());
        layout->addWidget(plainLabel(visitEventText(activity.recent.at(index), now)));
    }
    layout->addWidget(mutedLabel(QStringLiteral("Visitors are counted by a one-way hash of their connection, never "
                                                "their address. ForkMesh keeps the newest 1,000 events per survey "
                                                "for 90 days.")));
    return page;
}

QWidget *SurveysPage::responsesPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(6);
    const QColor danger = Theme::color(Theme::Role::Danger);
    if (m_responsesFor != m_detailId) {
        layout->addWidget(m_responsesError.isEmpty() ? mutedLabel(QStringLiteral("Loading responses…"))
                                                     : mutedLabel(m_responsesError));
        if (!m_responsesError.isEmpty()) {
            QPushButton *retry = ghostButton(QStringLiteral("Try again"), QStringLiteral("sync"));
            retry->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            connect(retry, &QPushButton::clicked, this, [this] {
                loadResponses();
                renderDetail();
            });
            layout->addWidget(retry);
        }
        return page;
    }
    if (m_responses.isEmpty()) {
        layout->addWidget(mutedLabel(m_detail.status == QLatin1String("draft")
            ? QStringLiteral("No responses yet. Open the survey and share its link to collect answers.")
            : QStringLiteral("No responses yet. Share the link to collect answers.")));
        return page;
    }
    if (m_detail.responseCount > m_responses.size())
        layout->addWidget(mutedLabel(QStringLiteral("Showing the %1 newest responses; Export CSV has all %2.")
                                         .arg(m_responses.size())
                                         .arg(m_detail.responseCount)));
    const int shown = std::min<int>(m_responseLimit, int(m_responses.size()));
    for (int index = 0; index < shown; ++index) {
        const SurveyResponse &response = m_responses.at(index);
        if (index > 0)
            layout->addWidget(separator());
        auto *header = new QHBoxLayout;
        header->setSpacing(6);
        QLabel *when = headingLabel(dateTimeText(response.submittedAt));
        when->setToolTip(relativeText(nowMs(), response.submittedAt));
        header->addWidget(when, 1);
        QToolButton *remove = iconButton(QStringLiteral("trash"), QStringLiteral("Delete response"), danger);
        const QString responseId = response.id;
        connect(remove, &QToolButton::clicked, this, [this, responseId] { deleteResponse(responseId); });
        header->addWidget(remove);
        layout->addLayout(header);
        auto *grid = new QFormLayout;
        grid->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        grid->setRowWrapPolicy(m_arranged == 1 ? QFormLayout::WrapAllRows : QFormLayout::WrapLongRows);
        grid->setVerticalSpacing(4);
        for (const auto &[question, answer] : responseLines(response, m_detail.questions)) {
            QLabel *label = mutedLabel(question);
            QLabel *value = plainLabel(answer);
            grid->addRow(label, value);
        }
        layout->addLayout(grid);
    }
    if (shown < m_responses.size()) {
        QPushButton *more = ghostButton(QStringLiteral("Show %1 more").arg(
            std::min<int>(kResponsePage, int(m_responses.size()) - shown)));
        more->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        connect(more, &QPushButton::clicked, this, [this] {
            m_responseLimit += kResponsePage;
            renderDetail();
        });
        layout->addWidget(more);
    }
    return page;
}

void SurveysPage::addDetailActions(QHBoxLayout *primary, QHBoxLayout *secondary)
{
    const Survey &survey = m_detail;
    const bool tower = m_arranged == 1;
    struct Action {
        QString label;
        QString icon;
        std::function<void()> run;
        bool danger = false;
    };
    QList<Action> main;
    QList<Action> more;
    if (canOpen(survey))
        main.append({QStringLiteral("Open survey"), QStringLiteral("play"), [this] { runStatusAction(QStringLiteral("open")); }});
    if (canReopen(survey))
        main.append({QStringLiteral("Reopen"), QStringLiteral("issue-reopened"), [this] { runStatusAction(QStringLiteral("open")); }});
    if (canClose(survey) && canCopyLink(survey))
        main.append({QStringLiteral("Copy link"), QStringLiteral("link"), [this] { copyPublicLink(); }});
    main.append({QStringLiteral("Edit"), QStringLiteral("pencil"), [this] { editSurvey(); }});
    if (canClose(survey))
        main.append({QStringLiteral("Close survey"), QStringLiteral("stop"), [this] { runStatusAction(QStringLiteral("close")); }});
    if (canDuplicate(survey))
        more.append({QStringLiteral("Duplicate"), QStringLiteral("copy"), [this] { duplicateSurvey(); }});
    if (canExport(survey))
        more.append({QStringLiteral("Export CSV"), QStringLiteral("download"), [this] { exportCsv(); }});
    if (canCopyLink(survey))
        more.append({QStringLiteral("Open link"), QStringLiteral("link-external"), [this] { openPublicLink(); }});
    if (canDeleteSurvey(survey))
        more.append({QStringLiteral("Delete"), QStringLiteral("trash"), [this] { deleteSurvey(); }, true});

    const QColor ink = Theme::color(Theme::Role::FgMuted);
    bool first = true;
    for (const Action &action : std::as_const(main)) {
        QPushButton *button = first ? primaryButton(action.label) : ghostButton(action.label);
        if (!first)
            button->setIcon(tintedIcon(action.icon, ink));
        first = false;
        connect(button, &QPushButton::clicked, this, action.run);
        primary->addWidget(button);
    }
    if (tower) {
        primary->addStretch(1);
        for (const Action &action : std::as_const(more)) {
            QPushButton *button = ghostButton(action.label);
            button->setIcon(tintedIcon(action.icon, action.danger ? Theme::color(Theme::Role::Danger) : ink));
            if (action.danger)
                button->setObjectName(QStringLiteral("dangerButton"));
            connect(button, &QPushButton::clicked, this, action.run);
            secondary->addWidget(button);
        }
        secondary->addStretch(1);
        return;
    }
    auto *menuButton = new QToolButton;
    menuButton->setObjectName(QStringLiteral("surveysMoreButton"));
    menuButton->setAccessibleName(QStringLiteral("More actions"));
    menuButton->setToolTip(QStringLiteral("More actions"));
    menuButton->setIcon(tintedIcon(QStringLiteral("kebab-horizontal"), ink));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setAutoRaise(true);
    auto *menu = new QMenu(menuButton);
    for (const Action &action : std::as_const(more)) {
        QAction *item = menu->addAction(
            tintedIcon(action.icon, action.danger ? Theme::color(Theme::Role::Danger) : ink), action.label);
        connect(item, &QAction::triggered, this, action.run);
    }
    menuButton->setMenu(menu);
    menuButton->setVisible(!more.isEmpty());
    primary->addWidget(menuButton);
    primary->addStretch(1);
}

void SurveysPage::surveyChanged(const QJsonObject &object, const QString &notice)
{
    if (!object.isEmpty()) {
        const Survey survey = parseSurvey(object);
        const int index = surveyIndex(survey.id);
        if (index >= 0)
            m_surveys.replace(index, survey);
        else
            m_surveys.prepend(survey);
        const bool complete = survey.detailed && object.value(QStringLiteral("results")).isArray();
        if (m_detailId != survey.id) {
            m_responses.clear();
            m_responsesFor.clear();
            m_responsesError.clear();
            m_responseLimit = kResponsePage;
            m_responsesLoading = false;
            ++m_responsesSerial;
            m_detailTab = 0;
        }
        Survey merged = survey;
        if (!complete && m_detail.id == survey.id && m_detail.detailed) {
            merged.questions = m_detail.questions;
            merged.results = m_detail.results;
            merged.activity = m_detail.activity;
            merged.detailed = true;
        } else if (!complete) {
            merged.detailed = false;
        }
        m_detailId = survey.id;
        m_detail = merged;
        if (m_arranged == 0)
            m_focusStack->setCurrentWidget(m_focusDetail);
        renderList();
        renderDetail();
        if (!complete)
            openSurvey(survey.id);
    }
    if (!notice.isEmpty() && m_host.flashMessage)
        m_host.flashMessage(notice, false);
    const int serial = ++m_serial;
    const QString name = organization();
    request(QByteArrayLiteral("GET"), organizationPath(), {},
            [this, serial, name, notice](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial || name != organization())
            return;
        if (!ok)
            return setStatus(QStringLiteral("Surveys could not reload: %1").arg(readableError(error)), true);
        applyOverview(payload);
        if (!notice.isEmpty())
            setStatus(notice);
    });
}

void SurveysPage::installPlugin()
{
    const QString path =
        forkmesh::market::nativePackageLifecyclePath(organization(), m_host.packageId, QStringLiteral("install"));
    if (path.isEmpty())
        return setStatus(QStringLiteral("Select a valid organization before installing Surveys."), true);
    m_installButton->setEnabled(false);
    setStatus(QStringLiteral("Installing Surveys…"));
    request(QByteArrayLiteral("POST"), path, {}, [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Surveys was not installed: %1").arg(readableError(error)), true);
        setStatus(QStringLiteral("Surveys installed for this organization."));
        refreshOrganizations();
    });
}

void SurveysPage::newSurvey()
{
    Survey survey;
    survey.thankYou = QStringLiteral("Thanks for answering.");
    survey.questions.append(Question{});
    openEditor(survey, false);
}

void SurveysPage::editSurvey()
{
    if (m_detail.id.isEmpty())
        return;
    if (m_detail.detailed)
        return openEditor(m_detail, true);
    const QString id = m_detail.id;
    setStatus(QStringLiteral("Loading the survey…"));
    request(QByteArrayLiteral("GET"), surveyPath(id), {},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not open the survey: %1").arg(readableError(error)), true);
        Survey loaded = parseSurvey(payload.value(QStringLiteral("survey")).toObject());
        loaded.detailed = true;
        setStatus(QString());
        if (loaded.id == m_detailId) {
            m_detail = loaded;
            renderDetail();
        }
        openEditor(loaded, true);
    });
}

void SurveysPage::openEditor(const Survey &survey, bool editing)
{
    if (!installed() || organization().isEmpty())
        return;
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("surveysEditor"));
    dialog->setWindowTitle(editing ? QStringLiteral("Edit %1").arg(surveyTitle(survey))
                                   : QStringLiteral("New survey"));
    dialog->resize(760, 820);
    dialog->setStyleSheet(QStringLiteral("QFrame#surveysQuestionCard { background: %1; border: 1px solid %2;"
                                         " border-radius: 8px; }"
                                         "QFrame#surveysQuestionCard QLabel { background: transparent; border: none; }")
                              .arg(Theme::hex(Theme::Role::Surface), Theme::hex(Theme::Role::Border)));
    auto *outer = new QVBoxLayout(dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(10);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *title = new QLineEdit(survey.title);
    title->setMaxLength(kTitleLimit);
    title->setPlaceholderText(QStringLiteral("Team offsite feedback"));
    title->setAccessibleName(QStringLiteral("Survey title"));
    QPlainTextEdit *description = textBox(survey.description, 3);
    description->setPlaceholderText(QStringLiteral("Shown above the questions (optional)"));
    description->setAccessibleName(QStringLiteral("Description"));
    QPlainTextEdit *thankYou = textBox(survey.thankYou, 2);
    thankYou->setPlaceholderText(QStringLiteral("Shown after someone answers"));
    thankYou->setAccessibleName(QStringLiteral("Thank-you text"));
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Thank-you text"), thankYou);
    layout->addLayout(form);

    layout->addWidget(headingLabel(QStringLiteral("Settings")));
    auto *settingsForm = new QFormLayout;
    settingsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const SurveySettings &settings = survey.settings;
    auto *onePerPerson = new QCheckBox(QStringLiteral("One response per person"));
    onePerPerson->setChecked(settings.oneResponsePerPerson);
    onePerPerson->setToolTip(QStringLiteral("A second answer from the same connection is refused. Instances in "
                                            "privacy mode cannot tell people apart, so there it has no effect."));
    auto *notify = new QCheckBox(QStringLiteral("Ping me when responses arrive (at most once an hour)"));
    notify->setChecked(settings.notifyOnResponse);
    auto *notifyVisit = new QCheckBox(QStringLiteral("Ping me when someone opens or starts it"));
    notifyVisit->setChecked(settings.notifyOnVisit);
    auto *shuffle = new QCheckBox(QStringLiteral("Show choice options in random order"));
    shuffle->setChecked(settings.shuffleOptions);
    settingsForm->addRow(onePerPerson);
    settingsForm->addRow(notify);
    settingsForm->addRow(notifyVisit);
    settingsForm->addRow(shuffle);
    const auto dateRow = [](qint64 stamp, const QString &label, QCheckBox **check, QDateTimeEdit **edit) {
        auto *row = new QHBoxLayout;
        *check = new QCheckBox(label);
        (*check)->setChecked(stamp > 0);
        *edit = new QDateTimeEdit(stamp > 0 ? QDateTime::fromMSecsSinceEpoch(stamp) : nextHour());
        (*edit)->setCalendarPopup(true);
        (*edit)->setDisplayFormat(QStringLiteral("d MMM yyyy HH:mm"));
        (*edit)->setAccessibleName(label);
        (*edit)->setEnabled(stamp > 0);
        QDateTimeEdit *target = *edit;
        QObject::connect(*check, &QCheckBox::toggled, target, &QWidget::setEnabled);
        row->addWidget(*check);
        row->addWidget(*edit, 1);
        return row;
    };
    QCheckBox *opensCheck = nullptr;
    QDateTimeEdit *opensEdit = nullptr;
    QCheckBox *closesCheck = nullptr;
    QDateTimeEdit *closesEdit = nullptr;
    settingsForm->addRow(QStringLiteral("Opens"), dateRow(settings.opensAt, QStringLiteral("Open at"), &opensCheck, &opensEdit));
    settingsForm->addRow(QStringLiteral("Closes"), dateRow(settings.closesAt, QStringLiteral("Close at"), &closesCheck, &closesEdit));
    auto *maxResponses = new QSpinBox;
    maxResponses->setRange(0, kMaxResponsesLimit);
    maxResponses->setSpecialValueText(QStringLiteral("No limit"));
    maxResponses->setValue(settings.maxResponses);
    maxResponses->setAccessibleName(QStringLiteral("Maximum responses"));
    settingsForm->addRow(QStringLiteral("Maximum responses"), maxResponses);
    layout->addLayout(settingsForm);

    auto *questionsHeader = new QHBoxLayout;
    questionsHeader->addWidget(headingLabel(QStringLiteral("Questions")), 1);
    layout->addLayout(questionsHeader);
    if (editing && survey.responseCount > 0)
        layout->addWidget(mutedLabel(QStringLiteral(
            "This survey has responses. Changing a question's type or options changes what its results show; "
            "the stored responses stay as they were.")));
    auto state = std::make_shared<EditorState>();
    state->questions = survey.questions;
    state->list = new QVBoxLayout;
    state->list->setSpacing(8);
    state->empty = mutedLabel(QStringLiteral("No questions yet. A survey needs at least one before it opens."));
    layout->addWidget(state->empty);
    layout->addLayout(state->list);
    auto *addRow = new QHBoxLayout;
    state->add = ghostButton(QStringLiteral("Add question"), QStringLiteral("plus"));
    addRow->addWidget(state->add);
    addRow->addWidget(mutedLabel(QStringLiteral("Up to %1 questions").arg(kMaxQuestions)), 1);
    layout->addLayout(addRow);
    buildCards(state);
    connect(state->add, &QPushButton::clicked, dialog, [state] {
        readCards(*state);
        if (state->questions.size() >= kMaxQuestions)
            return;
        state->questions.append(Question{});
        buildCards(state, int(state->questions.size()) - 1);
    });
    layout->addStretch(1);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);
    QLabel *error = dialogError();
    outer->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    QPushButton *save = buttons->addButton(editing ? QStringLiteral("Save") : QStringLiteral("Create draft"),
                                           QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QString surveyId = editing ? survey.id : QString();
    connect(save, &QPushButton::clicked, dialog, [=] {
        readCards(*state);
        if (title->text().trimmed().isEmpty())
            return showDialogError(error, QStringLiteral("Give the survey a title."));
        QList<Question> questions;
        const QString problem = questionsError(state->questions, &questions);
        if (!problem.isEmpty())
            return showDialogError(error, problem);
        SurveySettings chosen;
        chosen.oneResponsePerPerson = onePerPerson->isChecked();
        chosen.notifyOnResponse = notify->isChecked();
        chosen.notifyOnVisit = notifyVisit->isChecked();
        chosen.shuffleOptions = shuffle->isChecked();
        chosen.opensAt = opensCheck->isChecked() ? opensEdit->dateTime().toMSecsSinceEpoch() : 0;
        chosen.closesAt = closesCheck->isChecked() ? closesEdit->dateTime().toMSecsSinceEpoch() : 0;
        chosen.maxResponses = maxResponses->value();
        if (chosen.opensAt > 0 && chosen.closesAt > 0 && chosen.closesAt <= chosen.opensAt)
            return showDialogError(error, QStringLiteral("The survey closes before it opens."));
        const QJsonObject body{
            {QStringLiteral("title"), title->text().trimmed()},
            {QStringLiteral("description"), description->toPlainText().trimmed()},
            {QStringLiteral("thankYou"), thankYou->toPlainText().trimmed()},
            {QStringLiteral("questions"), questionsJson(questions)},
            {QStringLiteral("settings"), settingsJson(chosen)},
        };
        save->setEnabled(false);
        showDialogError(error, QString());
        const QPointer<QDialog> alive(dialog);
        request(surveyId.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
                surveyId.isEmpty() ? organizationPath(QStringLiteral("/items")) : surveyPath(surveyId), body,
                [this, alive, save, error](bool ok, const QJsonObject &payload, const QString &message) {
            if (!alive)
                return;
            save->setEnabled(true);
            if (!ok)
                return showDialogError(error, QStringLiteral("The survey was not saved: %1.")
                                                  .arg(readableError(message)));
            const QJsonObject saved = payload.value(QStringLiteral("survey")).toObject();
            alive->accept();
            m_detailTab = 0;
            surveyChanged(saved, QStringLiteral("Survey %1 saved.").arg(surveyTitle(parseSurvey(saved))));
        });
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
    QTimer::singleShot(0, title, [title] { title->setFocus(); });
}

void SurveysPage::runStatusAction(const QString &action)
{
    const Survey survey = m_detail;
    if (survey.id.isEmpty())
        return;
    const bool reopening = action == QLatin1String("open") && survey.status == QLatin1String("closed");
    request(QByteArrayLiteral("POST"), surveyPath(survey.id, QLatin1Char('/') + action), {},
            [this, action, reopening](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            const QString verb = action == QLatin1String("close") ? QStringLiteral("close")
                                 : reopening                     ? QStringLiteral("reopen")
                                                                 : QStringLiteral("open");
            return setStatus(QStringLiteral("Could not %1 the survey: %2").arg(verb, readableError(error)), true);
        }
        const QJsonObject updated = payload.value(QStringLiteral("survey")).toObject();
        const QString name = surveyTitle(parseSurvey(updated));
        surveyChanged(updated, action == QLatin1String("close")
                                   ? QStringLiteral("Survey %1 closed.").arg(name)
                                   : (reopening ? QStringLiteral("Survey %1 reopened.").arg(name)
                                                : QStringLiteral("Survey %1 is open; copy its link to share it.").arg(name)));
    });
}

void SurveysPage::duplicateSurvey()
{
    if (m_detail.id.isEmpty())
        return;
    request(QByteArrayLiteral("POST"), surveyPath(m_detail.id, QStringLiteral("/duplicate")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not duplicate the survey: %1").arg(readableError(error)), true);
        const QJsonObject copy = payload.value(QStringLiteral("survey")).toObject();
        m_detailTab = 0;
        surveyChanged(copy, QStringLiteral("Created %1 as a draft.").arg(surveyTitle(parseSurvey(copy))));
    });
}

void SurveysPage::deleteSurvey()
{
    const Survey survey = m_detail;
    if (!canDeleteSurvey(survey))
        return;
    const QString question = survey.responseCount > 0
        ? QStringLiteral("Delete %1 and its %2? This cannot be undone.")
              .arg(surveyTitle(survey), countPhrase(survey.responseCount, QStringLiteral("response")))
        : QStringLiteral("Delete %1? This cannot be undone.").arg(surveyTitle(survey));
    if (QMessageBox::question(this, QStringLiteral("Delete survey"), question) != QMessageBox::Yes)
        return;
    const QString id = survey.id;
    const QString name = surveyTitle(survey);
    request(QByteArrayLiteral("DELETE"), surveyPath(id), {},
            [this, id, name](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not delete the survey: %1").arg(readableError(error)), true);
        const int index = surveyIndex(id);
        if (index >= 0)
            m_surveys.removeAt(index);
        closeDetail();
        renderList();
        surveyChanged({}, QStringLiteral("Survey %1 deleted.").arg(name));
    });
}

void SurveysPage::deleteResponse(const QString &responseId)
{
    const QString id = m_detailId;
    if (id.isEmpty() || responseId.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete response"),
                              QStringLiteral("Delete this response? Its answers leave the results. This cannot be "
                                             "undone.")) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), surveyPath(id, QStringLiteral("/responses/") + responseId), {},
            [this, id, responseId](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not delete the response: %1").arg(readableError(error)), true);
        if (id != m_detailId)
            return;
        for (int index = int(m_responses.size()) - 1; index >= 0; --index) {
            if (m_responses.at(index).id == responseId)
                m_responses.removeAt(index);
        }
        const QJsonObject updated = payload.value(QStringLiteral("survey")).toObject();
        if (updated.isEmpty()) {
            m_detail.responseCount = std::max<qint64>(0, m_detail.responseCount - 1);
            renderDetail();
        }
        surveyChanged(updated, QStringLiteral("Response deleted."));
    });
}

void SurveysPage::exportCsv()
{
    const Survey survey = m_detail;
    if (!canExport(survey))
        return;
    setStatus(QStringLiteral("Preparing the CSV…"));
    request(QByteArrayLiteral("GET"), surveyPath(survey.id, QStringLiteral("/export")), {},
            [this, survey](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not export the responses: %1").arg(readableError(error)), true);
        QString name = QFileInfo(text(payload, "filename")).fileName();
        if (name.isEmpty())
            name = QStringLiteral("survey.csv");
        QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (folder.isEmpty())
            folder = QDir::homePath();
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export responses"),
                                                          QDir(folder).filePath(name),
                                                          QStringLiteral("CSV files (*.csv);;All files (*)"));
        if (path.isEmpty())
            return setStatus(QString());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(text(payload, "csv").toUtf8()) < 0 || !file.commit())
            return setStatus(QStringLiteral("Could not write %1: %2").arg(path, file.errorString()), true);
        setStatus(QStringLiteral("Exported %1 to %2.")
                      .arg(countPhrase(survey.responseCount, QStringLiteral("response")), QDir::toNativeSeparators(path)));
    });
}

void SurveysPage::copyPublicLink()
{
    if (!canCopyLink(m_detail))
        return;
    QApplication::clipboard()->setText(m_detail.publicUrl);
    setStatus(QStringLiteral("Survey link copied."));
    if (m_host.flashMessage)
        m_host.flashMessage(QStringLiteral("Survey link copied."), false);
}

void SurveysPage::openPublicLink()
{
    QUrl url(m_detail.publicUrl);
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return;
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("preview"), QStringLiteral("1"));
    url.setQuery(query);
    QDesktopServices::openUrl(url);
}

void SurveysPage::openWebPage()
{
    QString path = QString::fromLatin1(kWebPath);
    QStringList query;
    if (!organization().isEmpty())
        query << QStringLiteral("org=") + QString::fromUtf8(QUrl::toPercentEncoding(organization()));
    if (!m_detailId.isEmpty())
        query << QStringLiteral("survey=") + m_detailId;
    if (!query.isEmpty())
        path += QLatin1Char('?') + query.join(QLatin1Char('&'));
    if (m_host.openWebPage)
        m_host.openWebPage(path);
}

void SurveysPage::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

}

namespace {

const forkmesh::market::NativePackageRegistration surveysRegistration({
    QStringLiteral("builtin:surveys"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Surveys"),
    QStringLiteral("checklist"),
    QStringLiteral("/api/surveys"),
    QStringLiteral("forkmesh-surveys-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::surveys::SurveysPage(host, parent);
    },
});

}
