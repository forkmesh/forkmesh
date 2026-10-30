#include "PokerPage.h"

#include "LayoutMode.h"
#include "PokerSound.h"
#include "PokerTableView.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringListModel>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::poker {
namespace {

constexpr int kDrawerWidth = 320;
constexpr int kTableIdRole = Qt::UserRole + 1;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

QLabel *mutedLabel(const QString &value = QString())
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QPushButton *ghostButton(const QString &label)
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("ghostButton"));
    return button;
}

QString clockText(qint64 at)
{
    if (at <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(at).toString(QStringLiteral("HH:mm"));
}

class NameListCompleter final : public QCompleter
{
public:
    using QCompleter::QCompleter;

    QStringList splitPath(const QString &path) const override
    {
        return {path.section(QLatin1Char(','), -1).trimmed()};
    }

    QString pathFromIndex(const QModelIndex &index) const override
    {
        QString typed = widget() ? static_cast<QLineEdit *>(widget())->text() : QString();
        const int comma = int(typed.lastIndexOf(QLatin1Char(',')));
        const QString head = comma >= 0 ? typed.left(comma + 1) + QLatin1Char(' ') : QString();
        return head + QCompleter::pathFromIndex(index) + QStringLiteral(", ");
    }
};

struct BuyIn {
    int seat = -1;
    qint64 amount = 0;
};

bool askBuyIn(QWidget *parent, const QString &title, const QString &note, qint64 minimum, qint64 maximum,
              qint64 value, qint64 step, const QList<int> &seats, int preferredSeat, BuyIn *result)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *heading = new QLabel(note);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *form = new QFormLayout;
    QComboBox *seatBox = nullptr;
    if (!seats.isEmpty()) {
        seatBox = new QComboBox;
        for (const int seat : seats)
            seatBox->addItem(QStringLiteral("Seat %1").arg(seat + 1), seat);
        const int found = int(seats.indexOf(preferredSeat));
        seatBox->setCurrentIndex(found >= 0 ? found : 0);
        form->addRow(QStringLiteral("Seat"), seatBox);
    }
    auto *row = new QHBoxLayout;
    auto *slider = new QSlider(Qt::Horizontal);
    auto *spin = new QSpinBox;
    slider->setRange(int(minimum), int(maximum));
    spin->setRange(int(minimum), int(maximum));
    spin->setGroupSeparatorShown(true);
    spin->setSuffix(QStringLiteral(" chips"));
    slider->setSingleStep(int(std::max<qint64>(1, step)));
    slider->setPageStep(int(std::max<qint64>(1, (maximum - minimum) / 10)));
    spin->setSingleStep(int(std::max<qint64>(1, step)));
    slider->setValue(int(std::clamp(value, minimum, maximum)));
    spin->setValue(slider->value());
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    row->addWidget(slider, 1);
    row->addWidget(spin);
    form->addRow(QStringLiteral("Buy-in"), row);
    auto *quick = new QHBoxLayout;
    for (const auto &[label, fraction] : {qMakePair(QStringLiteral("Minimum"), 0.0), qMakePair(QStringLiteral("Half"), 0.5),
                                          qMakePair(QStringLiteral("Maximum"), 1.0)}) {
        auto *button = ghostButton(label);
        QObject::connect(button, &QPushButton::clicked, slider, [slider, minimum, maximum, fraction = fraction] {
            slider->setValue(int(minimum + qint64(double(maximum - minimum) * fraction)));
        });
        quick->addWidget(button);
    }
    quick->addStretch();
    form->addRow(QString(), quick);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(title);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(420, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return false;
    result->seat = seatBox ? seatBox->currentData().toInt() : -1;
    result->amount = spin->value();
    return true;
}

bool askNewTable(QWidget *parent, const PokerLobby &lobby, QJsonObject *body)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("New poker table"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit;
    name->setMaxLength(40);
    name->setPlaceholderText(QStringLiteral("Friday night"));
    const QString owner = lobby.actor.isEmpty() ? QStringLiteral("My") : lobby.actor + QStringLiteral("'s");
    name->setText(QStringLiteral("%1 table").arg(owner));
    form->addRow(QStringLiteral("Name"), name);
    auto *stakes = new QComboBox;
    for (const PokerStake &stake : lobby.stakes)
        stakes->addItem(QStringLiteral("%1 · blinds %2 · buy-in %3 to %4")
                            .arg(stake.label.isEmpty() ? stake.id : stake.label, stakesLabel(stake.small, stake.big),
                                 amountText(stake.minBuyIn), amountText(stake.maxBuyIn)),
                        stake.id);
    form->addRow(QStringLiteral("Stakes"), stakes);
    auto *seats = new QSpinBox;
    seats->setRange(2, 9);
    seats->setValue(6);
    form->addRow(QStringLiteral("Seats"), seats);
    auto *clock = new QComboBox;
    for (const int seconds : {15, 30, 45, 60})
        clock->addItem(QStringLiteral("%1 seconds").arg(seconds), seconds);
    clock->setCurrentIndex(1);
    form->addRow(QStringLiteral("Turn clock"), clock);
    auto *bots = new QSpinBox;
    bots->setRange(0, 5);
    bots->setValue(2);
    bots->setSuffix(QStringLiteral(" bots"));
    form->addRow(QStringLiteral("Fill with"), bots);
    auto *privacy = new QCheckBox(QStringLiteral("Private: only people with the code or an invite can join"));
    form->addRow(QString(), privacy);
    auto *sit = new QCheckBox(QStringLiteral("Sit down at the table myself"));
    sit->setChecked(true);
    form->addRow(QString(), sit);
    auto *buyRow = new QHBoxLayout;
    auto *slider = new QSlider(Qt::Horizontal);
    auto *spin = new QSpinBox;
    spin->setGroupSeparatorShown(true);
    spin->setSuffix(QStringLiteral(" chips"));
    buyRow->addWidget(slider, 1);
    buyRow->addWidget(spin);
    form->addRow(QStringLiteral("Buy-in"), buyRow);
    auto *note = mutedLabel();
    form->addRow(QString(), note);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create table"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);

    const qint64 wallet = lobby.wallet.chips;
    const auto stakeFor = [&lobby, stakes] {
        const PokerStake *stake = lobby.stake(stakes->currentData().toString());
        return stake ? *stake : defaultStakes().first();
    };
    const auto sync = [&] {
        const PokerStake stake = stakeFor();
        bots->setMaximum(seats->value() - 1);
        const qint64 top = std::min(stake.maxBuyIn, wallet);
        const bool affordable = top >= stake.minBuyIn;
        slider->setEnabled(sit->isChecked() && affordable);
        spin->setEnabled(sit->isChecked() && affordable);
        slider->setRange(int(stake.minBuyIn), int(std::max(stake.minBuyIn, top)));
        spin->setRange(int(stake.minBuyIn), int(std::max(stake.minBuyIn, top)));
        slider->setSingleStep(int(stake.small));
        spin->setSingleStep(int(stake.small));
        if (spin->value() < stake.minBuyIn || spin->value() > top)
            slider->setValue(int(std::clamp<qint64>(stake.minBuyIn * 2 + (stake.maxBuyIn - stake.minBuyIn) / 4,
                                                    stake.minBuyIn, std::max(stake.minBuyIn, top))));
        if (!sit->isChecked())
            note->setText(QStringLiteral("You will watch; sit down from the table when you like."));
        else if (!affordable)
            note->setText(QStringLiteral("Your wallet holds %1 chips, less than this table's %2 minimum. Pick lower stakes or watch.")
                              .arg(amountText(wallet), amountText(stake.minBuyIn)));
        else
            note->setText(QStringLiteral("Your wallet: %1 chips.").arg(amountText(wallet)));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!sit->isChecked() || affordable);
    };
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    QObject::connect(stakes, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { sync(); });
    QObject::connect(seats, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int) { sync(); });
    QObject::connect(sit, &QCheckBox::toggled, &dialog, [&](bool) { sync(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    sync();
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *body = QJsonObject{
        {QStringLiteral("name"), name->text().trimmed().isEmpty() ? name->placeholderText() : name->text().trimmed()},
        {QStringLiteral("stakes"), stakes->currentData().toString()},
        {QStringLiteral("seats"), seats->value()},
        {QStringLiteral("private"), privacy->isChecked()},
        {QStringLiteral("turnSeconds"), clock->currentData().toInt()},
        {QStringLiteral("bots"), bots->value()},
        {QStringLiteral("buyIn"), sit->isChecked() && spin->isEnabled() ? spin->value() : 0},
    };
    return true;
}

}

QString PokerPage::apiPath(const QString &suffix)
{
    return QStringLiteral("/api/poker") + suffix;
}

PokerPage::PokerPage(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("pokerSection"));
    m_sound = new PokerSound(this);
    m_sound->setMuted(m_host.setting ? m_host.setting(QStringLiteral("soundMuted"), false).toBool() : false);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 16, 14);
    outer->setSpacing(8);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Poker"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    auto *subtitle = mutedLabel(QStringLiteral("No-limit Texas Hold'em with other ForkMesh users, for virtual chips"));
    subtitle->setWordWrap(false);
    heading->addWidget(subtitle, 1);
    m_mute = new QToolButton;
    m_mute->setObjectName(QStringLiteral("ghostButton"));
    m_mute->setCheckable(true);
    m_mute->setToolButtonStyle(Qt::ToolButtonTextOnly);
    heading->addWidget(m_mute);
    auto *refreshButton = ghostButton(QStringLiteral("Refresh"));
    heading->addWidget(refreshButton);
    outer->addLayout(heading);
    m_status = mutedLabel();
    m_status->hide();
    outer->addWidget(m_status);

    m_stack = new QStackedWidget;
    outer->addWidget(m_stack, 1);
    buildLobby();
    buildTable();
    m_stack->addWidget(m_lobbyPage);
    m_stack->addWidget(m_tablePage);
    m_stack->setCurrentWidget(m_lobbyPage);

    m_wake = new QTimer(this);
    m_wake->setSingleShot(true);
    m_wake->setTimerType(Qt::PreciseTimer);
    connect(m_wake, &QTimer::timeout, this, [this] { sendTick(); });
    m_claimTimer = new QTimer(this);
    m_claimTimer->setSingleShot(true);
    connect(m_claimTimer, &QTimer::timeout, this, [this] { refreshClaimLabel(); });

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    m_mute->setChecked(m_sound->muted());
    setMuted(m_sound->muted());
    connect(m_mute, &QToolButton::toggled, this, [this](bool muted) {
        setMuted(muted);
        if (m_host.setSetting)
            m_host.setSetting(QStringLiteral("soundMuted"), muted);
    });
    if (!PokerSound::available()) {
        m_mute->setEnabled(false);
        m_mute->setText(QStringLiteral("No sound"));
        m_mute->setToolTip(QStringLiteral("This build of ForkMesh has no audio support (Qt Multimedia), so the table is silent."));
    }

    arrange(forkmesh::ui::layoutControlTower());
}

PokerPage::~PokerPage() = default;

void PokerPage::buildLobby()
{
    m_lobbyPage = new QWidget;
    auto *layout = new QVBoxLayout(m_lobbyPage);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(12);

    auto *top = new QHBoxLayout;
    top->setSpacing(12);
    auto *walletCard = new QFrame;
    walletCard->setObjectName(QStringLiteral("pokerCard"));
    auto *walletLayout = new QVBoxLayout(walletCard);
    walletLayout->addWidget(mutedLabel(QStringLiteral("Your chips")));
    m_chips = new QLabel(QStringLiteral("…"));
    m_chips->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 700;"));
    walletLayout->addWidget(m_chips);
    m_onTables = mutedLabel();
    walletLayout->addWidget(m_onTables);
    auto *claimRow = new QHBoxLayout;
    m_claim = new QPushButton(QStringLiteral("Claim free chips"));
    m_claim->setObjectName(QStringLiteral("primaryButton"));
    claimRow->addWidget(m_claim);
    claimRow->addStretch();
    walletLayout->addLayout(claimRow);
    m_claimNote = mutedLabel(QStringLiteral("Chips are play money: they cannot be bought, sold or cashed out."));
    walletLayout->addWidget(m_claimNote);
    top->addWidget(walletCard, 1);

    auto *statsCard = new QFrame;
    statsCard->setObjectName(QStringLiteral("pokerCard"));
    auto *statsLayout = new QVBoxLayout(statsCard);
    statsLayout->addWidget(mutedLabel(QStringLiteral("Your record")));
    m_stats = new QLabel;
    m_stats->setWordWrap(true);
    m_stats->setTextFormat(Qt::RichText);
    statsLayout->addWidget(m_stats);
    statsLayout->addStretch();
    top->addWidget(statsCard, 1);

    auto *joinCard = new QFrame;
    joinCard->setObjectName(QStringLiteral("pokerCard"));
    auto *joinLayout = new QVBoxLayout(joinCard);
    joinLayout->addWidget(mutedLabel(QStringLiteral("Play")));
    m_create = new QPushButton(QStringLiteral("New table…"));
    m_create->setObjectName(QStringLiteral("primaryButton"));
    joinLayout->addWidget(m_create);
    auto *codeRow = new QHBoxLayout;
    m_code = new QLineEdit;
    m_code->setPlaceholderText(QStringLiteral("Table code"));
    m_code->setMaxLength(8);
    m_code->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z \\-]{0,8}")), m_code));
    m_join = ghostButton(QStringLiteral("Join"));
    codeRow->addWidget(m_code, 1);
    codeRow->addWidget(m_join);
    joinLayout->addLayout(codeRow);
    joinLayout->addWidget(mutedLabel(QStringLiteral("A private table's six-letter code joins it.")));
    joinLayout->addStretch();
    top->addWidget(joinCard, 1);
    layout->addLayout(top);

    auto *tablesTitle = new QLabel(QStringLiteral("Tables"));
    tablesTitle->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(tablesTitle);
    m_tables = new QTableWidget(0, 6);
    m_tables->setObjectName(QStringLiteral("pokerTables"));
    m_tables->setHorizontalHeaderLabels({QStringLiteral("Table"), QStringLiteral("Stakes"), QStringLiteral("Players"),
                                         QStringLiteral("Clock"), QStringLiteral("Hands"), QString()});
    m_tables->verticalHeader()->setVisible(false);
    m_tables->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tables->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tables->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tables->setShowGrid(false);
    m_tables->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
        m_tables->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(m_tables, 1);
    m_tablesEmpty = mutedLabel(QStringLiteral("No open tables right now. Start one, add a few bots, and invite friends."));
    m_tablesEmpty->hide();
    layout->addWidget(m_tablesEmpty);

    m_lobbyPage->setStyleSheet(QStringLiteral(
        "QFrame#pokerCard { border: 1px solid palette(mid); border-radius: 8px; padding: 6px; }"));

    connect(m_claim, &QPushButton::clicked, this, [this] { claimChips(); });
    connect(m_create, &QPushButton::clicked, this, [this] { createTable(); });
    connect(m_join, &QPushButton::clicked, this, [this] { joinByCode(); });
    connect(m_code, &QLineEdit::returnPressed, this, [this] { joinByCode(); });
    connect(m_tables, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_tables->item(row, 0))
            openTable(item->data(kTableIdRole).toString());
    });
}

void PokerPage::buildTable()
{
    m_tablePage = new QWidget;
    auto *layout = new QVBoxLayout(m_tablePage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    m_back = ghostButton(QStringLiteral("‹ Lobby"));
    bar->addWidget(m_back);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    m_tableTitle = new QLabel;
    m_tableTitle->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    m_tableInfo = mutedLabel();
    m_tableInfo->setWordWrap(false);
    titles->addWidget(m_tableTitle);
    titles->addWidget(m_tableInfo);
    bar->addLayout(titles, 1);
    m_wallet = mutedLabel();
    m_wallet->setWordWrap(false);
    bar->addWidget(m_wallet);
    m_sit = new QPushButton(QStringLiteral("Sit down"));
    m_sit->setObjectName(QStringLiteral("primaryButton"));
    m_away = ghostButton(QStringLiteral("Sit out"));
    m_rebuy = ghostButton(QStringLiteral("Rebuy"));
    m_bots = ghostButton(QStringLiteral("Bots"));
    m_invite = ghostButton(QStringLiteral("Invite"));
    m_leave = ghostButton(QStringLiteral("Leave"));
    m_more = ghostButton(QStringLiteral("More"));
    m_drawerButton = ghostButton(QStringLiteral("Chat && log"));
    m_drawerButton->setCheckable(true);
    for (QPushButton *button : {m_sit, m_away, m_rebuy, m_bots, m_invite, m_leave, m_more, m_drawerButton})
        bar->addWidget(button);
    layout->addLayout(bar);
    m_tableStatus = mutedLabel();
    m_tableStatus->hide();
    layout->addWidget(m_tableStatus);

    m_tableRow = new QHBoxLayout;
    m_tableRow->setSpacing(8);
    m_tableColumn = new QWidget;
    auto *column = new QVBoxLayout(m_tableColumn);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    m_view = new PokerTableView;
    m_view->setFocusPolicy(Qt::StrongFocus);
    column->addWidget(m_view, 1);

    m_actionBar = new QWidget;
    m_actionBar->setObjectName(QStringLiteral("pokerActionBar"));
    m_actionBar->setAttribute(Qt::WA_StyledBackground);
    auto *actions = new QHBoxLayout(m_actionBar);
    actions->setContentsMargins(12, 8, 12, 8);
    actions->setSpacing(8);
    m_actionNote = new QLabel;
    m_actionNote->setObjectName(QStringLiteral("pokerActionNote"));
    m_actionNote->setMinimumWidth(150);
    m_actionNote->setWordWrap(true);
    actions->addWidget(m_actionNote, 1);
    m_fold = new QPushButton(QStringLiteral("Fold"));
    m_fold->setObjectName(QStringLiteral("pokerFold"));
    m_call = new QPushButton(QStringLiteral("Check"));
    m_call->setObjectName(QStringLiteral("pokerCall"));
    actions->addWidget(m_fold);
    actions->addWidget(m_call);
    auto *raiseBox = new QVBoxLayout;
    raiseBox->setSpacing(4);
    auto *quick = new QHBoxLayout;
    quick->setSpacing(4);
    m_half = new QPushButton(QStringLiteral("½ pot"));
    m_threeQuarters = new QPushButton(QStringLiteral("¾ pot"));
    m_potButton = new QPushButton(QStringLiteral("Pot"));
    m_maxButton = new QPushButton(QStringLiteral("All-in"));
    for (QPushButton *button : {m_half, m_threeQuarters, m_potButton, m_maxButton}) {
        button->setObjectName(QStringLiteral("pokerQuick"));
        quick->addWidget(button);
    }
    raiseBox->addLayout(quick);
    auto *sliderRow = new QHBoxLayout;
    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setMinimumWidth(140);
    m_amount = new QSpinBox;
    m_amount->setGroupSeparatorShown(true);
    m_amount->setMinimumWidth(96);
    m_amount->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_amount->setAlignment(Qt::AlignRight);
    sliderRow->addWidget(m_slider, 1);
    sliderRow->addWidget(m_amount);
    raiseBox->addLayout(sliderRow);
    actions->addLayout(raiseBox, 1);
    m_raise = new QPushButton(QStringLiteral("Raise"));
    m_raise->setObjectName(QStringLiteral("pokerRaise"));
    actions->addWidget(m_raise);
    for (QPushButton *button : {m_fold, m_call, m_raise}) {
        button->setMinimumHeight(44);
        button->setMinimumWidth(110);
    }
    m_fold->setToolTip(QStringLiteral("Fold (F)"));
    m_call->setToolTip(QStringLiteral("Check or call (C)"));
    m_raise->setToolTip(QStringLiteral("Bet or raise to the amount (R)"));
    m_maxButton->setToolTip(QStringLiteral("Set the amount to your whole stack (A goes all-in)"));
    m_actionBar->setStyleSheet(QStringLiteral(
        "QWidget#pokerActionBar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1d1f24, stop:1 #101114);"
        " border-top: 1px solid #3a3326; }"
        "QLabel#pokerActionNote { color: #e8e2d0; font-size: 13px; }"
        "QPushButton#pokerFold, QPushButton#pokerCall, QPushButton#pokerRaise { color: white; font-size: 14px;"
        " font-weight: 700; border-radius: 8px; padding: 6px 14px; border: 1px solid rgba(255,255,255,40); }"
        "QPushButton#pokerFold { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #b23a3a, stop:1 #7f2020); }"
        "QPushButton#pokerCall { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2f73c9, stop:1 #1b4f94); }"
        "QPushButton#pokerRaise { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #3c9a46, stop:1 #24692c); }"
        "QPushButton#pokerFold:hover, QPushButton#pokerCall:hover, QPushButton#pokerRaise:hover {"
        " border: 1px solid #e8c45c; }"
        "QPushButton#pokerFold:disabled, QPushButton#pokerCall:disabled, QPushButton#pokerRaise:disabled {"
        " background: #2a2c31; color: #6f7278; border: 1px solid #33353a; }"
        "QPushButton#pokerQuick { color: #e8e2d0; background: #2a2c31; border: 1px solid #3d4046; border-radius: 6px;"
        " padding: 3px 8px; font-size: 12px; }"
        "QPushButton#pokerQuick:hover { border-color: #e8c45c; }"
        "QPushButton#pokerQuick:disabled { color: #5d6066; }"
        "QSpinBox { color: #fff3c4; background: #0d0e10; border: 1px solid #3d4046; border-radius: 6px; padding: 3px 6px;"
        " font-weight: 700; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a2c31; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #3c9a46; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #e8c45c; width: 16px; margin: -6px 0; border-radius: 8px; }"));
    column->addWidget(m_actionBar);
    m_tableRow->addWidget(m_tableColumn, 1);

    m_side = new QWidget;
    m_side->setObjectName(QStringLiteral("pokerSide"));
    m_side->setMinimumWidth(260);
    m_side->setMaximumWidth(kDrawerWidth + 40);
    auto *sideLayout = new QVBoxLayout(m_side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    m_sideTabs = new QTabWidget;
    auto *chatPage = new QWidget;
    auto *chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(4, 4, 4, 4);
    m_chat = new QListWidget;
    m_chat->setWordWrap(true);
    m_chat->setSelectionMode(QAbstractItemView::NoSelection);
    m_chat->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    chatLayout->addWidget(m_chat, 1);
    auto *chatRow = new QHBoxLayout;
    m_chatInput = new QLineEdit;
    m_chatInput->setMaxLength(200);
    m_chatInput->setPlaceholderText(QStringLiteral("Say something to the table"));
    m_chatSend = ghostButton(QStringLiteral("Send"));
    chatRow->addWidget(m_chatInput, 1);
    chatRow->addWidget(m_chatSend);
    chatLayout->addLayout(chatRow);
    m_sideTabs->addTab(chatPage, QStringLiteral("Chat"));
    m_log = new QListWidget;
    m_log->setWordWrap(true);
    m_log->setSelectionMode(QAbstractItemView::NoSelection);
    m_sideTabs->addTab(m_log, QStringLiteral("Hand log"));
    m_lastHand = new QTextBrowser;
    m_lastHand->setOpenLinks(false);
    m_sideTabs->addTab(m_lastHand, QStringLiteral("Last hand"));
    sideLayout->addWidget(m_sideTabs);
    m_tableRow->addWidget(m_side);
    layout->addLayout(m_tableRow, 1);
    m_tableColumn->installEventFilter(this);

    connect(m_back, &QPushButton::clicked, this, [this] { showLobby(); });
    connect(m_sit, &QPushButton::clicked, this, [this] {
        const QList<int> empty = m_table.emptySeats();
        sitDown(empty.isEmpty() ? -1 : empty.first());
    });
    connect(m_away, &QPushButton::clicked, this, [this] {
        const PokerSeat *mine = m_table.mine();
        setAway(!(mine && mine->away));
    });
    connect(m_rebuy, &QPushButton::clicked, this, [this] { rebuy(); });
    connect(m_leave, &QPushButton::clicked, this, [this] { leaveTable(); });
    connect(m_bots, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        QAction *add = menu.addAction(QStringLiteral("Add a bot"));
        add->setEnabled(!m_table.emptySeats().isEmpty());
        connect(add, &QAction::triggered, this, [this] { addBot(); });
        const QList<int> bots = m_table.botSeats();
        if (!bots.isEmpty())
            menu.addSeparator();
        for (const int seat : bots) {
            const PokerSeat *entry = m_table.seat(seat);
            QAction *remove = menu.addAction(QStringLiteral("Remove %1").arg(entry ? entry->displayName() : QString()));
            connect(remove, &QAction::triggered, this, [this, seat] { removeBot(seat); });
        }
        menu.exec(m_bots->mapToGlobal(QPoint(0, m_bots->height())));
    });
    connect(m_invite, &QPushButton::clicked, this, [this] { inviteFriends(); });
    connect(m_more, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        QAction *code = menu.addAction(QStringLiteral("Copy table code %1").arg(m_table.code));
        code->setEnabled(!m_table.code.isEmpty());
        connect(code, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(m_table.code);
            setTableStatus(QStringLiteral("Copied the table code %1.").arg(m_table.code));
        });
        QAction *fair = menu.addAction(QStringLiteral("How the deck is checked"));
        connect(fair, &QAction::triggered, this, [this] {
            setDrawerOpen(true);
            m_sideTabs->setCurrentWidget(m_lastHand);
        });
        if (m_table.ownerIsMe) {
            menu.addSeparator();
            QAction *close = menu.addAction(QStringLiteral("Close table…"));
            connect(close, &QAction::triggered, this, [this] { closeTable(); });
        }
        menu.exec(m_more->mapToGlobal(QPoint(0, m_more->height())));
    });
    connect(m_drawerButton, &QPushButton::toggled, this, [this](bool open) { setDrawerOpen(open); });
    connect(m_fold, &QPushButton::clicked, this, [this] { foldHand(); });
    connect(m_call, &QPushButton::clicked, this, [this] { checkOrCall(); });
    connect(m_raise, &QPushButton::clicked, this, [this] { betOrRaise(); });
    connect(m_half, &QPushButton::clicked, this, [this] { setRaiseAmount(potFractionRaise(m_table, 0.5)); });
    connect(m_threeQuarters, &QPushButton::clicked, this, [this] { setRaiseAmount(potFractionRaise(m_table, 0.75)); });
    connect(m_potButton, &QPushButton::clicked, this, [this] { setRaiseAmount(potFractionRaise(m_table, 1.0)); });
    connect(m_maxButton, &QPushButton::clicked, this, [this] { setRaiseAmount(m_table.legal.raiseMax); });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (m_syncingAmount)
            return;
        setRaiseAmount(value);
    });
    connect(m_amount, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_syncingAmount)
            return;
        setRaiseAmount(value);
    });
    connect(m_chatSend, &QPushButton::clicked, this, [this] { sendChat(); });
    connect(m_chatInput, &QLineEdit::returnPressed, this, [this] { sendChat(); });
    connect(m_sideTabs, &QTabWidget::currentChanged, this, [this](int) {
        if (m_sideTabs->currentIndex() == 0) {
            m_unread = 0;
            renderActions();
        }
    });

    const auto shortcut = [this](Qt::Key key, std::function<void()> run) {
        auto *action = new QShortcut(QKeySequence(key), m_tablePage);
        action->setContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QShortcut::activated, this, [run = std::move(run)] { run(); });
    };
    shortcut(Qt::Key_F, [this] { foldHand(); });
    shortcut(Qt::Key_C, [this] { checkOrCall(); });
    shortcut(Qt::Key_R, [this] { betOrRaise(); });
    shortcut(Qt::Key_A, [this] { allIn(); });

    m_view->emptySeatClicked = [this](int seat) { sitDown(seat); };
    m_view->soundRequested = [this](const QString &name, float volume) { m_sound->play(name, volume); };
    m_view->setAvatarSource([this](const QString &name) {
        return m_host.accountPicture ? m_host.accountPicture(name) : QPixmap();
    });
}

void PokerPage::arrange(bool controlTower)
{
    if (m_arranged && controlTower == m_controlTower)
        return;
    m_arranged = true;
    m_controlTower = controlTower;
    if (controlTower) {
        m_side->setParent(m_tablePage);
        m_side->setAutoFillBackground(false);
        m_side->setStyleSheet(QString());
        m_tableRow->addWidget(m_side);
        m_side->show();
        m_drawerButton->hide();
    } else {
        m_tableRow->removeWidget(m_side);
        m_side->setParent(m_tableColumn);
        m_side->setAutoFillBackground(true);
        m_side->setStyleSheet(QStringLiteral("QWidget#pokerSide { border: 1px solid palette(mid); border-radius: 8px; }"));
        m_drawerButton->show();
        m_side->setVisible(m_drawerOpen);
        placeDrawer();
        m_side->raise();
    }
    renderActions();
}

void PokerPage::placeDrawer()
{
    if (m_controlTower || !m_side || !m_view)
        return;
    const QRect area = m_view->geometry();
    const int width = std::min(kDrawerWidth, std::max(240, area.width() / 2));
    m_side->setGeometry(area.right() - width - 8, area.top() + 8, width, std::max(160, area.height() - 16));
}

void PokerPage::setDrawerOpen(bool open)
{
    m_drawerOpen = open;
    if (m_drawerButton->isChecked() != open) {
        const QSignalBlocker block(m_drawerButton);
        m_drawerButton->setChecked(open);
    }
    if (!m_controlTower) {
        placeDrawer();
        m_side->setVisible(open);
        m_side->raise();
    }
    if (open && m_sideTabs->currentIndex() == 0)
        m_unread = 0;
    renderActions();
}

bool PokerPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tableColumn && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        placeDrawer();
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

void PokerPage::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    arrange(forkmesh::ui::layoutControlTower());
    if (m_stack->currentWidget() == m_lobbyPage && m_lobbyDirty)
        loadLobby();
    refreshClaimLabel();
}

void PokerPage::hideEvent(QHideEvent *event)
{
    m_claimTimer->stop();
    forkmesh::market::NativePackageWidget::hideEvent(event);
}

void PokerPage::request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Poker network access is unavailable."));
        return;
    }
    const QPointer<PokerPage> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload, const QString &error) {
        if (guard)
            handler(ok, payload, error);
    });
}

void PokerPage::refresh()
{
    arrange(forkmesh::ui::layoutControlTower());
    if (!m_openTableId.isEmpty())
        loadTable();
    loadLobby();
}

void PokerPage::packageChanged()
{
    if (!m_openTableId.isEmpty())
        loadTable();
    if (isVisible() && m_stack->currentWidget() == m_lobbyPage)
        loadLobby();
    else
        m_lobbyDirty = true;
}

int PokerPage::navigationBadge() const
{
    return m_badge;
}

void PokerPage::updateBadge()
{
    const int badge = m_table.myTurn() ? 1 : 0;
    if (badge == m_badge)
        return;
    m_badge = badge;
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void PokerPage::accountPicturesChanged()
{
    m_view->avatarsChanged();
}

QList<forkmesh::market::NativePackageSearchHit> PokerPage::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed().toLower();
    if (needle.size() < 2 || limit <= 0)
        return hits;
    QSet<QString> seen;
    const auto add = [&](const QString &id, const QString &name, const QString &code, const QString &detail) {
        if (hits.size() >= limit || seen.contains(id))
            return;
        if (!name.toLower().contains(needle) && code.toLower() != needle)
            return;
        seen.insert(id);
        hits.append({id, name, detail});
    };
    if (m_table.valid)
        add(m_table.id, m_table.name, m_table.code,
            QStringLiteral("Poker · blinds %1 · open now").arg(stakesLabel(m_table.small, m_table.big)));
    for (const PokerLobbyTable &table : m_lobby.tables)
        add(table.id, table.name, table.code,
            QStringLiteral("Poker · blinds %1 · %2 of %3 seated%4")
                .arg(stakesLabel(table.small, table.big))
                .arg(table.players)
                .arg(table.seats)
                .arg(table.mine ? QStringLiteral(" · you play here") : QString()));
    if (hits.size() < limit && QStringLiteral("poker hold'em holdem texas chips cards").contains(needle))
        hits.append({QStringLiteral("lobby"), QStringLiteral("Poker lobby"),
                     QStringLiteral("%1 chips in your wallet").arg(amountText(m_lobby.wallet.chips))});
    return hits;
}

void PokerPage::activateSearchHit(const QString &id)
{
    if (id == QLatin1String("lobby") || id.isEmpty()) {
        showLobby();
        return;
    }
    openTable(id);
}

bool PokerPage::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        QJsonObject result = payload;
        const QJsonObject bundle = payload.value(QStringLiteral("export")).toObject();
        result.insert(QStringLiteral("hasData"), ok && bundle.value(QStringLiteral("wallet")).isObject());
        done(ok, result, error);
    });
    return true;
}

void PokerPage::setStatus(const QString &value, bool error)
{
    m_status->setVisible(!value.isEmpty());
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void PokerPage::setTableStatus(const QString &value, bool error)
{
    m_tableStatus->setVisible(!value.isEmpty());
    m_tableStatus->setText(value);
    m_tableStatus->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_tableStatus->style()->unpolish(m_tableStatus);
    m_tableStatus->style()->polish(m_tableStatus);
}

void PokerPage::setMuted(bool muted)
{
    m_sound->setMuted(muted);
    if (PokerSound::available()) {
        m_mute->setText(muted ? QStringLiteral("Sound off") : QStringLiteral("Sound on"));
        m_mute->setToolTip(muted ? QStringLiteral("Turn the table sounds on") : QStringLiteral("Mute the table sounds"));
    }
}

void PokerPage::loadLobby()
{
    if (m_lobbyLoading) {
        m_lobbyAgain = true;
        return;
    }
    m_lobbyLoading = true;
    m_lobbyAgain = false;
    m_lobbyDirty = false;
    request(QByteArrayLiteral("GET"), apiPath(), {}, [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_lobbyLoading = false;
        if (m_lobbyAgain)
            return loadLobby();
        if (!ok) {
            setStatus(QStringLiteral("The poker lobby could not load: %1").arg(errorText(text(payload, "error"), error)), true);
            return;
        }
        m_lobby = PokerLobby::parse(payload);
        m_lobbyOffset = m_lobby.now > 0 ? m_lobby.now - QDateTime::currentMSecsSinceEpoch() : 0;
        setStatus(QString());
        renderLobby();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void PokerPage::renderLobby()
{
    const PokerWallet &wallet = m_lobby.wallet;
    m_chips->setText(QStringLiteral("%1 chips").arg(amountText(wallet.chips)));
    m_onTables->setText(wallet.onTables > 0 ? QStringLiteral("%1 more on tables").arg(amountText(wallet.onTables))
                                            : QStringLiteral("Nothing on a table right now"));
    const int rate = wallet.handsPlayed > 0 ? int(std::lround(100.0 * wallet.handsWon / wallet.handsPlayed)) : 0;
    m_stats->setText(QStringLiteral("<b>%1</b> hands played · <b>%2</b> won (%3%)<br>Biggest pot <b>%4</b><br>Best hand <b>%5</b>")
                         .arg(amountText(wallet.handsPlayed), amountText(wallet.handsWon))
                         .arg(rate)
                         .arg(amountText(wallet.biggestPot),
                              wallet.bestHand.isEmpty() ? QStringLiteral("none yet") : wallet.bestHand.toHtmlEscaped()));
    refreshClaimLabel();

    m_tables->setRowCount(0);
    for (const PokerLobbyTable &table : m_lobby.tables) {
        const int row = m_tables->rowCount();
        m_tables->insertRow(row);
        QStringList badges;
        if (table.mine)
            badges << QStringLiteral("you play here");
        if (table.invited)
            badges << QStringLiteral("invited");
        if (table.isPrivate)
            badges << QStringLiteral("private");
        auto *name = new QTableWidgetItem(badges.isEmpty() ? table.name
                                                           : QStringLiteral("%1   ·  %2").arg(table.name, badges.join(QStringLiteral(" · "))));
        name->setData(kTableIdRole, table.id);
        name->setToolTip(QStringLiteral("Owner: %1 · code %2").arg(table.owner, table.code));
        QFont font = name->font();
        font.setBold(table.mine);
        name->setFont(font);
        m_tables->setItem(row, 0, name);
        const PokerStake *stake = m_lobby.stake(table.stakes);
        m_tables->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1 %2").arg(
                                      stake ? stake->label : table.stakes, stakesLabel(table.small, table.big))));
        m_tables->setItem(row, 2, new QTableWidgetItem(
                                      table.bots > 0 ? QStringLiteral("%1 / %2 (%3 bots)").arg(table.players).arg(table.seats).arg(table.bots)
                                                     : QStringLiteral("%1 / %2").arg(table.players).arg(table.seats)));
        m_tables->setItem(row, 3, new QTableWidgetItem(QStringLiteral("%1 s").arg(table.turnSeconds)));
        m_tables->setItem(row, 4, new QTableWidgetItem(QString::number(table.handNumber)));
        auto *open = new QPushButton(table.mine ? QStringLiteral("Play") : table.players >= table.seats
                                                                               ? QStringLiteral("Watch")
                                                                               : QStringLiteral("Join"));
        open->setObjectName(table.mine ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton"));
        const QString id = table.id;
        connect(open, &QPushButton::clicked, this, [this, id] { openTable(id); });
        m_tables->setCellWidget(row, 5, open);
    }
    m_tables->setVisible(!m_lobby.tables.isEmpty());
    m_tablesEmpty->setVisible(m_lobby.tables.isEmpty());
}

void PokerPage::refreshClaimLabel()
{
    if (!m_lobby.valid) {
        m_claim->setEnabled(false);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch() + m_lobbyOffset;
    const PokerWallet &wallet = m_lobby.wallet;
    const bool ready = wallet.canClaim || (wallet.nextClaimAt > 0 && now >= wallet.nextClaimAt);
    m_claim->setText(claimLabel(wallet, now));
    m_claim->setEnabled(ready);
    m_claimTimer->stop();
    if (!ready && wallet.nextClaimAt > now && isVisible())
        m_claimTimer->start(int(std::clamp<qint64>((wallet.nextClaimAt - now) % 60000 + 250, 1000, 60000)));
}

void PokerPage::claimChips()
{
    m_claim->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/claim")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(errorText(text(payload, "error"), error), true);
            return loadLobby();
        }
        const qint64 before = m_lobby.wallet.chips;
        m_lobby.wallet = PokerWallet::parse(payload.value(QStringLiteral("wallet")).toObject());
        setStatus(QStringLiteral("%1 free chips added to your wallet.").arg(amountText(std::max<qint64>(0, m_lobby.wallet.chips - before))));
        m_sound->play(QStringLiteral("chip"));
        renderLobby();
        loadLobby();
    });
}

void PokerPage::createTable()
{
    if (!m_lobby.valid) {
        setStatus(QStringLiteral("The lobby has not loaded yet. Try again in a moment."), true);
        return loadLobby();
    }
    QJsonObject body;
    if (!askNewTable(this, m_lobby, &body))
        return;
    m_create->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/tables")), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_create->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("The table was not created: %1").arg(errorText(text(payload, "error"), error)), true);
        const QJsonObject table = payload.value(QStringLiteral("table")).toObject();
        const QString id = text(table, "id");
        if (id.isEmpty())
            return loadLobby();
        openTable(id);
        applyTable(payload);
        loadLobby();
    });
}

void PokerPage::joinByCode()
{
    const QString code = normalizedTableCode(m_code->text());
    if (!isValidTableCode(code)) {
        setStatus(QStringLiteral("A table code is six letters, like QXRTMB."), true);
        return;
    }
    m_join->setEnabled(false);
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/codes/") + code), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_join->setEnabled(true);
        if (!ok)
            return setStatus(errorText(text(payload, "error"), error), true);
        m_code->clear();
        openTable(text(payload, "tableId"));
    });
}

void PokerPage::openTable(const QString &id)
{
    if (id.isEmpty())
        return;
    const bool same = id == m_openTableId;
    m_stack->setCurrentWidget(m_tablePage);
    setStatus(QString());
    if (!same) {
        m_openTableId = id;
        m_table = PokerTable();
        m_lastSeq = -1;
        m_chatSeen = 0;
        m_unread = 0;
        m_wasMyTurn = false;
        m_wake->stop();
        m_view->clearTable();
        m_tableTitle->setText(QStringLiteral("Opening the table…"));
        m_tableInfo->clear();
        setTableStatus(QString());
        renderTable();
    }
    placeDrawer();
    loadTable();
    m_view->setFocus();
}

void PokerPage::showLobby()
{
    m_openTableId.clear();
    m_table = PokerTable();
    m_wake->stop();
    m_view->clearTable();
    m_stack->setCurrentWidget(m_lobbyPage);
    updateBadge();
    loadLobby();
}

void PokerPage::loadTable()
{
    if (m_openTableId.isEmpty())
        return;
    if (m_tableLoading) {
        m_tableAgain = true;
        return;
    }
    m_tableLoading = true;
    m_tableAgain = false;
    const QString id = m_openTableId;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/tables/") + id), {},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        m_tableLoading = false;
        if (id == m_openTableId) {
            if (ok)
                applyTable(payload);
            else
                tableFailed(payload, error);
        }
        if (m_tableAgain)
            loadTable();
    });
}

void PokerPage::tableFailed(const QJsonObject &payload, const QString &error)
{
    const QString code = text(payload, "error");
    if (code == QLatin1String("table_not_found") || code == QLatin1String("not_found")) {
        showLobby();
        setStatus(QStringLiteral("That table is closed. Every stack went back to its wallet."), true);
        return;
    }
    setTableStatus(QStringLiteral("The table could not load: %1").arg(errorText(code, error)), true);
}

void PokerPage::applyTable(const QJsonObject &payload)
{
    const PokerTable table = PokerTable::parse(payload);
    if (!table.valid || table.id != m_openTableId)
        return;
    if (m_table.valid && m_table.id == table.id && table.version > 0 && table.version < m_table.version)
        return;
    const qint64 offset = table.now > 0 ? table.now - QDateTime::currentMSecsSinceEpoch() : 0;
    m_view->setServerOffset(offset);
    const qint64 highest = highestSeq(table.events);
    QList<PokerEvent> fresh;
    const bool first = m_lastSeq < 0 || !m_table.valid;
    if (!first && highest >= m_lastSeq)
        fresh = eventsAfter(table.events, m_lastSeq);
    m_lastSeq = first || highest < m_lastSeq ? highest : std::max(m_lastSeq, highest);
    for (const PokerEvent &event : fresh)
        if (event.kind == QLatin1String("chat"))
            ++m_unread;
    const bool myTurn = table.myTurn();
    const bool newTurn = myTurn && (!m_wasMyTurn || m_table.handNumber != table.handNumber ||
                                    m_table.actStartedAt != table.actStartedAt);
    m_wasMyTurn = myTurn;
    m_table = table;
    m_view->setTable(table, fresh);
    if (newTurn && !first)
        m_sound->play(QStringLiteral("turn"));
    setTableStatus(QString());
    renderTable();
    if (newTurn)
        setRaiseAmount(table.legal.raiseMin);
    armWake();
    updateBadge();
}

void PokerPage::armWake()
{
    m_wake->stop();
    if (!m_table.valid || m_openTableId != m_table.id)
        return;
    const qint64 delay = wakeDelay(m_table.wakeAt, m_table.now);
    if (delay < 0)
        return;
    m_wake->start(int(std::min<qint64>(delay, 6LL * 60 * 60 * 1000)));
}

void PokerPage::sendTick()
{
    if (m_openTableId.isEmpty())
        return;
    const QString id = m_openTableId;
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/tables/%1/tick").arg(id)), {},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (id != m_openTableId)
            return;
        if (ok)
            applyTable(payload);
        else
            tableFailed(payload, error);
    });
}

void PokerPage::renderTable()
{
    const PokerTable &table = m_table;
    if (!table.valid) {
        for (QPushButton *button : {m_sit, m_away, m_rebuy, m_leave, m_bots, m_invite, m_more})
            button->hide();
        m_wallet->clear();
        m_actionBar->hide();
        return;
    }
    m_tableTitle->setText(table.name);
    QStringList info;
    const PokerStake *stake = m_lobby.stake(table.stakes);
    info << QStringLiteral("%1 blinds %2").arg(stake ? stake->label : table.stakes, stakesLabel(table.small, table.big));
    info << QStringLiteral("buy-in %1 to %2").arg(amountText(table.minBuyIn), amountText(table.maxBuyIn));
    info << QStringLiteral("%1 s clock").arg(table.turnSeconds);
    if (table.handNumber > 0)
        info << QStringLiteral("hand #%1").arg(table.handNumber);
    if (!table.code.isEmpty())
        info << QStringLiteral("code %1").arg(table.code);
    if (table.isPrivate)
        info << QStringLiteral("private");
    m_tableInfo->setText(info.join(QStringLiteral(" · ")));
    m_wallet->setText(QStringLiteral("Wallet %1").arg(amountText(table.wallet)));

    const PokerSeat *mine = table.mine();
    const bool seated = mine != nullptr;
    m_sit->setVisible(!seated);
    m_sit->setEnabled(!table.emptySeats().isEmpty());
    m_away->setVisible(seated);
    m_away->setText(mine && mine->away ? QStringLiteral("I'm back") : QStringLiteral("Sit out"));
    m_rebuy->setVisible(seated);
    m_rebuy->setEnabled(mine && !mine->inHand() && mine->stack < table.maxBuyIn && table.wallet > 0);
    m_leave->setVisible(seated);
    m_bots->setVisible(table.ownerIsMe);
    m_invite->setVisible(table.ownerIsMe || seated);
    m_more->setVisible(true);
    m_actionBar->setVisible(seated);
    renderActions();
    renderChat();
    renderLog();
    renderLastHand();
}

void PokerPage::renderActions()
{
    m_drawerButton->setText(m_unread > 0 && !(m_drawerOpen && m_sideTabs->currentIndex() == 0)
                                ? QStringLiteral("Chat && log (%1)").arg(m_unread)
                                : QStringLiteral("Chat && log"));
    if (!m_table.valid || !m_table.seated())
        return;
    const PokerTable &table = m_table;
    const PokerLegal &legal = table.legal;
    const bool turn = table.myTurn() && !m_acting;
    m_fold->setEnabled(turn && legal.fold);
    m_call->setEnabled(turn);
    m_call->setText(turn ? callButtonLabel(legal) : QStringLiteral("Check / Call"));
    const bool raise = turn && legal.canRaise;
    for (QWidget *widget : {static_cast<QWidget *>(m_raise), static_cast<QWidget *>(m_slider),
                            static_cast<QWidget *>(m_amount), static_cast<QWidget *>(m_half),
                            static_cast<QWidget *>(m_threeQuarters), static_cast<QWidget *>(m_potButton),
                            static_cast<QWidget *>(m_maxButton)})
        widget->setEnabled(raise);
    if (raise) {
        m_syncingAmount = true;
        const int step = int(std::max<qint64>(1, table.small));
        m_slider->setRange(int(legal.raiseMin), int(legal.raiseMax));
        m_slider->setSingleStep(step);
        m_slider->setPageStep(int(std::max<qint64>(step, table.big * 2)));
        m_amount->setRange(int(legal.raiseMin), int(legal.raiseMax));
        m_amount->setSingleStep(step);
        if (m_amount->value() < legal.raiseMin || m_amount->value() > legal.raiseMax)
            m_amount->setValue(int(legal.raiseMin));
        m_slider->setValue(m_amount->value());
        m_syncingAmount = false;
        m_raise->setText(raiseButtonLabel(legal, table.currentBet, m_amount->value()));
    } else {
        m_raise->setText(table.currentBet > 0 ? QStringLiteral("Raise") : QStringLiteral("Bet"));
    }

    const PokerSeat *mine = table.mine();
    QString note;
    if (m_acting) {
        note = QStringLiteral("Sending…");
    } else if (turn) {
        note = table.myHandName.isEmpty() ? QStringLiteral("Your turn.")
                                          : QStringLiteral("Your turn · %1").arg(table.myHandName);
    } else if (mine && mine->away) {
        note = QStringLiteral("You are sitting out. Press I'm back to play the next hand.");
    } else if (mine && mine->stack <= 0 && !mine->inHand()) {
        note = QStringLiteral("You are out of chips. Rebuy from your wallet to keep playing.");
    } else if (table.phase == QLatin1String("waiting")) {
        note = QStringLiteral("Waiting for a second player. The owner can add a bot.");
    } else if (table.phase == QLatin1String("showdown")) {
        note = QStringLiteral("The next hand starts in a moment.");
    } else if (const PokerSeat *acting = table.seat(table.toAct); acting && !acting->empty) {
        note = QStringLiteral("Waiting for %1…").arg(acting->displayName());
    } else if (mine && mine->status == QLatin1String("folded")) {
        note = QStringLiteral("You folded this hand.");
    }
    m_actionNote->setText(note);
}

void PokerPage::setRaiseAmount(qint64 amount)
{
    if (!m_table.legal.canRaise)
        return;
    const qint64 clamped = clampRaise(m_table.legal, amount);
    m_syncingAmount = true;
    m_amount->setValue(int(clamped));
    m_slider->setValue(int(clamped));
    m_syncingAmount = false;
    m_raise->setText(raiseButtonLabel(m_table.legal, m_table.currentBet, clamped));
}

void PokerPage::renderChat()
{
    const bool atBottom = m_chat->verticalScrollBar()->value() >= m_chat->verticalScrollBar()->maximum() - 4;
    m_chat->clear();
    for (const PokerChatLine &line : m_table.chat) {
        auto *item = new QListWidgetItem(QStringLiteral("%1: %2").arg(line.name, line.text));
        item->setToolTip(clockText(line.at));
        QFont font = item->font();
        if (m_host.accountName && line.name == m_host.accountName())
            font.setBold(true);
        item->setFont(font);
        m_chat->addItem(item);
    }
    if (m_table.chat.isEmpty())
        m_chat->addItem(QStringLiteral("No chat yet. Say hello."));
    if (atBottom)
        m_chat->scrollToBottom();
    if ((m_drawerOpen || m_controlTower) && m_sideTabs->currentIndex() == 0)
        m_unread = 0;
}

void PokerPage::renderLog()
{
    m_log->clear();
    for (const PokerLogLine &line : m_table.log) {
        const QString body = line.text.isEmpty() ? QStringLiteral("%1 %2").arg(line.name, actionTag({line.kind, line.amount}))
                                                 : line.text;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(clockText(line.at), body));
        m_log->addItem(item);
    }
    if (m_table.log.isEmpty())
        m_log->addItem(QStringLiteral("The dealer has not dealt yet."));
    m_log->scrollToBottom();
}

void PokerPage::renderLastHand()
{
    const PokerLastHand &hand = m_table.lastHand;
    QString html;
    if (!hand.valid()) {
        html = QStringLiteral("<p>No hand has finished at this table yet.</p>");
    } else {
        html += QStringLiteral("<h3>Hand #%1</h3>").arg(hand.number);
        if (!hand.board.isEmpty())
            html += QStringLiteral("<p>Board: <b>%1</b></p>").arg(cardsLabel(hand.board).toHtmlEscaped());
        for (const PokerWinner &winner : hand.winners) {
            html += QStringLiteral("<p><b>%1</b> won <b>%2</b>").arg(winner.name.toHtmlEscaped(), amountText(winner.amount));
            if (!winner.handName.isEmpty())
                html += QStringLiteral(" with %1").arg(winner.handName.toHtmlEscaped());
            if (!winner.cards.isEmpty())
                html += QStringLiteral(" (%1)").arg(cardsLabel(winner.cards).toHtmlEscaped());
            html += QStringLiteral("</p>");
        }
        for (const PokerShown &shown : hand.shown)
            html += QStringLiteral("<p>%1 showed %2%3</p>")
                        .arg(shown.name.toHtmlEscaped(), cardsLabel(shown.cards).toHtmlEscaped(),
                             shown.handName.isEmpty() ? QString() : QStringLiteral(": ") + shown.handName.toHtmlEscaped());
        html += QStringLiteral("<h4>Provably fair</h4><p>Before the deal the table sealed each of the 52 shuffled deck "
                               "positions with its own secret salt and published one SHA-256 over all of them. After the "
                               "hand it reveals the salts of the cards that were shown (the board and hands at showdown, "
                               "never folded cards), so you can check each one sat where the deck said.</p>");
        html += QStringLiteral("<p>Commitment:<br><code>%1</code></p>").arg(hand.commitment.toHtmlEscaped());
        if (!hand.hashes.isEmpty())
            html += revealMatches(hand)
                ? QStringLiteral("<p><b>Checked:</b> the 52 position hashes hash to the commitment, and every revealed "
                                 "card with its salt and position hashes to its sealed position.</p>")
                : QStringLiteral("<p><b>Mismatch:</b> the revealed cards do not match the sealed deck.</p>");
        if (!hand.revealed.isEmpty()) {
            html += QStringLiteral("<p>Revealed cards:</p><table cellspacing=\"0\" cellpadding=\"2\">"
                                   "<tr><th align=\"left\">Position</th><th align=\"left\">Card</th>"
                                   "<th align=\"left\">Salt</th></tr>");
            for (const PokerRevealedCard &card : hand.revealed)
                html += QStringLiteral("<tr><td>%1</td><td>%2</td><td><code>%3</code></td></tr>")
                            .arg(card.index)
                            .arg(PokerCard::parse(card.card).valid ? PokerCard::parse(card.card).label().toHtmlEscaped()
                                                                   : card.card.toHtmlEscaped(),
                                 card.salt.toHtmlEscaped());
            html += QStringLiteral("</table>");
        }
    }
    if (!m_table.commitment.isEmpty())
        html += QStringLiteral("<p>This hand's commitment:<br><code>%1</code></p>").arg(m_table.commitment.toHtmlEscaped());
    if (!m_table.history.isEmpty()) {
        html += QStringLiteral("<h4>Recent hands</h4>");
        for (const PokerHistoryLine &line : m_table.history)
            html += QStringLiteral("<p>#%1 · %2 won %3%4</p>")
                        .arg(line.number)
                        .arg(line.winners.join(QStringLiteral(", ")).toHtmlEscaped(), amountText(line.amount),
                             line.handName.isEmpty() ? QString() : QStringLiteral(" · ") + line.handName.toHtmlEscaped());
    }
    m_lastHand->setHtml(html);
}

void PokerPage::act(const QString &action, qint64 amount)
{
    if (m_acting || !m_table.myTurn())
        return;
    m_acting = true;
    renderActions();
    QJsonObject body{{QStringLiteral("action"), action}, {QStringLiteral("hand"), m_table.handNumber}};
    if (amount > 0)
        body.insert(QStringLiteral("amount"), amount);
    const QString id = m_openTableId;
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/tables/%1/act").arg(id)), body,
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        m_acting = false;
        if (id != m_openTableId)
            return;
        if (ok) {
            applyTable(payload);
            return;
        }
        const QString code = text(payload, "error");
        setTableStatus(errorText(code, error), true);
        renderActions();
        loadTable();
    });
}

void PokerPage::foldHand()
{
    if (!m_table.myTurn() || !m_table.legal.fold)
        return;
    act(QStringLiteral("fold"));
}

void PokerPage::checkOrCall()
{
    if (!m_table.myTurn())
        return;
    if (m_table.legal.call <= 0)
        act(QStringLiteral("check"));
    else
        act(QStringLiteral("call"));
}

void PokerPage::betOrRaise()
{
    if (!m_table.myTurn() || !m_table.legal.canRaise)
        return;
    const qint64 amount = clampRaise(m_table.legal, m_amount->value());
    if (amount >= m_table.legal.raiseMax)
        return allIn();
    act(raiseAction(m_table.currentBet), amount);
}

void PokerPage::allIn()
{
    if (!m_table.myTurn() || m_table.legal.allIn <= 0)
        return;
    act(QStringLiteral("allin"));
}

void PokerPage::tableWrite(const QString &suffix, const QJsonObject &body, const QString &failure,
                           std::function<void(const QJsonObject &)> done)
{
    if (m_openTableId.isEmpty())
        return;
    const QString id = m_openTableId;
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/tables/%1/%2").arg(id, suffix)), body,
            [this, id, failure, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (id != m_openTableId)
            return;
        if (!ok) {
            setTableStatus(QStringLiteral("%1: %2").arg(failure, errorText(text(payload, "error"), error)), true);
            return loadTable();
        }
        applyTable(payload);
        if (done)
            done(payload);
    });
}

void PokerPage::sitDown(int seat)
{
    if (!m_table.valid || m_table.seated())
        return;
    const QList<int> empty = m_table.emptySeats();
    if (empty.isEmpty()) {
        setTableStatus(errorText(QStringLiteral("table_full"), {}), true);
        return;
    }
    const qint64 top = std::min(m_table.maxBuyIn, m_table.wallet);
    if (top < m_table.minBuyIn) {
        setTableStatus(QStringLiteral("This table's buy-in starts at %1 chips and your wallet holds %2. Claim free chips in the lobby.")
                           .arg(amountText(m_table.minBuyIn), amountText(m_table.wallet)),
                       true);
        return;
    }
    BuyIn choice;
    const qint64 suggested = std::clamp<qint64>(m_table.big * 100, m_table.minBuyIn, top);
    if (!askBuyIn(this, QStringLiteral("Sit down"),
                  QStringLiteral("%1 · blinds %2. Your wallet holds %3 chips; what you bring sits on the table until you leave.")
                      .arg(m_table.name, stakesLabel(m_table.small, m_table.big), amountText(m_table.wallet)),
                  m_table.minBuyIn, top, suggested, m_table.small, empty, seat, &choice))
        return;
    tableWrite(QStringLiteral("sit"), QJsonObject{{QStringLiteral("seat"), choice.seat}, {QStringLiteral("buyIn"), choice.amount}},
               QStringLiteral("You were not seated"), [this](const QJsonObject &) { m_sound->play(QStringLiteral("chip")); });
}

void PokerPage::setAway(bool away)
{
    tableWrite(QStringLiteral("away"), QJsonObject{{QStringLiteral("away"), away}},
               away ? QStringLiteral("You could not sit out") : QStringLiteral("You could not come back"));
}

void PokerPage::rebuy()
{
    const PokerSeat *mine = m_table.mine();
    if (!mine)
        return;
    const qint64 room = m_table.maxBuyIn - mine->stack;
    const qint64 top = std::min(room, m_table.wallet);
    const qint64 bottom = std::max<qint64>(1, std::min(top, std::max<qint64>(m_table.big, m_table.minBuyIn - mine->stack)));
    if (top <= 0) {
        setTableStatus(room <= 0 ? QStringLiteral("Your stack is already at this table's maximum.")
                                 : QStringLiteral("Your wallet is empty. Claim free chips in the lobby."),
                       true);
        return;
    }
    BuyIn choice;
    if (!askBuyIn(this, QStringLiteral("Rebuy"),
                  QStringLiteral("Add chips from your wallet (%1) to your stack (%2). The table allows up to %3.")
                      .arg(amountText(m_table.wallet), amountText(mine->stack), amountText(m_table.maxBuyIn)),
                  bottom, top, top, m_table.small, {}, -1, &choice))
        return;
    tableWrite(QStringLiteral("rebuy"), QJsonObject{{QStringLiteral("amount"), choice.amount}},
               QStringLiteral("The rebuy did not go through"), [this](const QJsonObject &) { m_sound->play(QStringLiteral("chip")); });
}

void PokerPage::leaveTable()
{
    const PokerSeat *mine = m_table.mine();
    if (!mine)
        return;
    const bool inHand = mine->inHand() && m_table.handRunning();
    if (QMessageBox::question(this, QStringLiteral("Leave the table"),
                              inHand ? QStringLiteral("Leave %1? Your hand is folded and your %2 chips go back to your wallet when it ends.")
                                           .arg(m_table.name, amountText(mine->stack))
                                     : QStringLiteral("Leave %1? Your %2 chips go back to your wallet.")
                                           .arg(m_table.name, amountText(mine->stack)),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes)
        return;
    const QString name = m_table.name;
    tableWrite(QStringLiteral("leave"), {}, QStringLiteral("You could not leave"), [this, name](const QJsonObject &) {
        showLobby();
        setStatus(QStringLiteral("You left %1. Your chips are back in your wallet.").arg(name));
    });
}

void PokerPage::addBot()
{
    tableWrite(QStringLiteral("bots"), QJsonObject{{QStringLiteral("add"), true}}, QStringLiteral("No bot was added"));
}

void PokerPage::removeBot(int seat)
{
    tableWrite(QStringLiteral("bots"), QJsonObject{{QStringLiteral("remove"), seat}}, QStringLiteral("The bot was not removed"));
}

void PokerPage::inviteFriends()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Invite to %1").arg(m_table.name));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("ForkMesh account names, separated by commas. Each gets a Ping with the table.")));
    auto *names = new QLineEdit;
    names->setPlaceholderText(QStringLiteral("alice, bob"));
    QStringList directory = m_host.userDirectory ? m_host.userDirectory() : QStringList();
    directory.removeAll(m_host.accountName ? m_host.accountName() : QString());
    auto *completer = new NameListCompleter(directory, names);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    names->setCompleter(completer);
    layout->addWidget(names);
    if (!m_table.code.isEmpty())
        layout->addWidget(mutedLabel(QStringLiteral("Or share the code %1.").arg(m_table.code)));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Invite"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(420, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonArray list;
    for (const QString &part : names->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        QString name = part.trimmed();
        if (name.startsWith(QLatin1Char('@')))
            name.remove(0, 1);
        if (!name.isEmpty() && list.size() < 20)
            list.append(name);
    }
    if (list.isEmpty())
        return;
    tableWrite(QStringLiteral("invite"), QJsonObject{{QStringLiteral("names"), list}}, QStringLiteral("No invites were sent"),
               [this](const QJsonObject &payload) {
        QStringList invited;
        QStringList skipped;
        for (const QJsonValue &value : payload.value(QStringLiteral("invited")).toArray())
            invited << (value.isObject() ? text(value.toObject(), "name") : value.toString());
        for (const QJsonValue &value : payload.value(QStringLiteral("skipped")).toArray())
            skipped << (value.isObject() ? text(value.toObject(), "name") : value.toString());
        QString message = invited.isEmpty() ? QStringLiteral("Nobody new was invited.")
                                            : QStringLiteral("Invited %1.").arg(invited.join(QStringLiteral(", ")));
        if (!skipped.isEmpty())
            message += QStringLiteral(" Skipped %1.").arg(skipped.join(QStringLiteral(", ")));
        setTableStatus(message);
    });
}

void PokerPage::sendChat()
{
    const QString line = m_chatInput->text().trimmed();
    if (line.isEmpty() || m_openTableId.isEmpty())
        return;
    m_chatSend->setEnabled(false);
    tableWrite(QStringLiteral("chat"), QJsonObject{{QStringLiteral("text"), line}}, QStringLiteral("The message was not sent"),
               [this](const QJsonObject &) {
        m_chatInput->clear();
        m_chat->scrollToBottom();
    });
    m_chatSend->setEnabled(true);
}

void PokerPage::closeTable()
{
    if (!m_table.ownerIsMe)
        return;
    if (QMessageBox::warning(this, QStringLiteral("Close the table"),
                             QStringLiteral("Close %1 for everyone? Every stack goes back to its wallet.").arg(m_table.name),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const QString id = m_openTableId;
    const QString name = m_table.name;
    request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/tables/") + id), {},
            [this, name](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setTableStatus(QStringLiteral("The table was not closed: %1").arg(errorText(text(payload, "error"), error)), true);
        showLobby();
        setStatus(QStringLiteral("%1 is closed. Every stack went back to its wallet.").arg(name));
    });
}

}

namespace {
const forkmesh::market::NativePackageRegistration pokerRegistration({
    QStringLiteral("builtin:poker"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Poker"),
    QStringLiteral("spade"),
    QStringLiteral("/api/poker"),
    QStringLiteral("forkmesh-poker-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::poker::PokerPage(host, parent);
    },
});
}
