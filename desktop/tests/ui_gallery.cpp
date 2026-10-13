#include "DesignTokens.h"
#include "Theme.h"
#include "UiKit.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace forkmesh::ui;

namespace {

void applyDesign(Theme::Design design, bool dark)
{
    Theme::setActiveDesign(design, dark);
    static const QFont classicFont = qApp->font();
    qApp->setFont(Theme::interfaceFont(design, classicFont));
    Theme::setActivePalette(design == Theme::Design::Next
                                ? Theme::nextPalette(dark)
                                : QMap<QString, QString>());
    qApp->setPalette(Theme::paletteForDesign(design, dark));
    qApp->setStyleSheet(Theme::styleSheetForDesign(design, dark));
    kit::syncStockWidgets();
}

bool saveFloating(QWidget *surface, const QString &path)
{
    const QPixmap grabbed = surface->grab();
    const qreal ratio = grabbed.devicePixelRatio();
    constexpr int kMargin = 24;
    QImage canvas(QSize(surface->width() + 2 * kMargin, surface->height() + 2 * kMargin) *
                      ratio,
                  QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(Theme::color(Theme::Role::Bg));
    QPainter painter(&canvas);
    painter.drawPixmap(kMargin, kMargin, grabbed);
    painter.end();
    return canvas.save(path);
}

QPushButton *button(const QString &text, const char *objectName = nullptr,
                    const char *size = nullptr)
{
    auto *widget = new QPushButton(text);
    if (objectName)
        widget->setObjectName(QLatin1String(objectName));
    if (size)
        widget->setProperty("buttonSize", QLatin1String(size));
    return widget;
}

QHBoxLayout *row(const QList<QWidget *> &widgets)
{
    auto *layout = new QHBoxLayout;
    layout->setSpacing(10);
    for (QWidget *widget : widgets)
        layout->addWidget(widget, 0, Qt::AlignVCenter);
    layout->addStretch();
    return layout;
}

QWidget *signInCard()
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("welcomeCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedWidth(420);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(28, 26, 28, 24);
    layout->setSpacing(12);
    auto *brand = new QLabel(QStringLiteral("FORKMESH"));
    brand->setObjectName(QStringLiteral("welcomeBrand"));
    layout->addWidget(brand);
    auto *title = new QLabel(QStringLiteral("Sign in"));
    title->setObjectName(QStringLiteral("welcomeTitle"));
    layout->addWidget(title);
    auto *lede = new QLabel(QStringLiteral(
        "Use an account from this or another ForkMesh instance."));
    lede->setObjectName(QStringLiteral("welcomeLede"));
    lede->setWordWrap(true);
    layout->addWidget(lede);
    auto *user = new QLineEdit;
    user->setPlaceholderText(QStringLiteral("username"));
    layout->addWidget(kit::labeledField(QStringLiteral("ForkMesh username"), user));
    auto *instance = new QLineEdit(QStringLiteral("app.forkmesh.com"));
    layout->addWidget(kit::labeledField(QStringLiteral("Instance"), instance));
    auto *notice = new kit::InlineNotice;
    notice->showMessage(QStringLiteral("Incorrect email or password."),
                        kit::Tone::Danger);
    layout->addWidget(notice);
    layout->addLayout(row({button(QStringLiteral("Continue"), "primaryButton"),
                           button(QStringLiteral("Back"))}));
    auto *footnote = new QFrame;
    footnote->setObjectName(QStringLiteral("welcomeFootnote"));
    footnote->setAttribute(Qt::WA_StyledBackground, true);
    auto *footLayout = new QVBoxLayout(footnote);
    footLayout->setContentsMargins(12, 10, 12, 10);
    auto *footText = new QLabel(QStringLiteral(
        "You can keep using ForkMesh as a guest and sign in later from the top bar."));
    footText->setWordWrap(true);
    footLayout->addWidget(footText);
    layout->addWidget(footnote);
    return card;
}

QWidget *buildPage()
{
    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setContentsMargins(32, 28, 32, 28);
    root->setSpacing(32);

    auto *left = new QVBoxLayout;
    left->setSpacing(14);
    left->addWidget(kit::pageTitle(QStringLiteral("Interface kit")));
    left->addWidget(kit::pageLede(QStringLiteral(
        "Stock widgets and shared components in the active design.")));

    left->addWidget(kit::groupHeading(QStringLiteral("Buttons")));
    auto *disabled = button(QStringLiteral("Disabled"));
    disabled->setEnabled(false);
    left->addLayout(row({button(QStringLiteral("Continue"), "primaryButton"),
                         button(QStringLiteral("Reload app")),
                         button(QStringLiteral("Ghost"), "ghostButton"),
                         button(QStringLiteral("Delete"), "dangerButton"),
                         button(QStringLiteral("Approve"), "successButton"),
                         disabled}));
    left->addLayout(row({button(QStringLiteral("Small"), nullptr, "sm"),
                         button(QStringLiteral("Small primary"), "primaryButton", "sm"),
                         button(QStringLiteral("Tiny"), nullptr, "xs")}));

    left->addWidget(kit::groupHeading(QStringLiteral("Inputs")));
    auto *empty = new QLineEdit;
    empty->setPlaceholderText(QStringLiteral("Paste a one-time token"));
    auto *filled = new QLineEdit(QStringLiteral("forkmesh/forkmesh"));
    auto *off = new QLineEdit(QStringLiteral("Disabled"));
    off->setEnabled(false);
    left->addLayout(row({empty, filled, off}));
    auto *combo = new QComboBox;
    combo->setObjectName(QStringLiteral("galleryCombo"));
    combo->addItems({QStringLiteral("Current checkout"),
                     QStringLiteral("New worktree"),
                     QStringLiteral("Detached copy"),
                     QStringLiteral("Remote host")});
    auto *spin = new QSpinBox;
    spin->setValue(3);
    left->addLayout(row({combo, spin}));

    left->addWidget(kit::groupHeading(QStringLiteral("Choices")));
    auto *checked = new QCheckBox(QStringLiteral("Checked"));
    checked->setChecked(true);
    auto *radioOn = new QRadioButton(QStringLiteral("Selected"));
    radioOn->setChecked(true);
    auto *toggleOn = new kit::Toggle;
    toggleOn->setChecked(true);
    left->addLayout(row({checked, new QCheckBox(QStringLiteral("Unchecked")),
                         radioOn, new QRadioButton(QStringLiteral("Other")),
                         toggleOn, new kit::Toggle,
                         new kit::Toggle(QStringLiteral("With label"))}));
    auto *segmented = new kit::SegmentedControl(
        {QStringLiteral("System"), QStringLiteral("Light"), QStringLiteral("Dark")});
    left->addLayout(row({segmented}));

    left->addWidget(kit::groupHeading(QStringLiteral("Range")));
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setValue(60);
    slider->setFixedWidth(220);
    auto *progress = new QProgressBar;
    progress->setValue(45);
    progress->setTextVisible(false);
    progress->setFixedSize(220, 6);
    left->addLayout(row({slider, progress}));

    left->addWidget(kit::groupHeading(QStringLiteral("Badges and keys")));
    left->addLayout(row({new kit::Badge(QStringLiteral("Neutral")),
                         new kit::Badge(QStringLiteral("Current"), kit::Tone::Accent),
                         new kit::Badge(QStringLiteral("Available"), kit::Tone::Success),
                         new kit::Badge(QStringLiteral("Coming soon"), kit::Tone::Warning, true),
                         new kit::Badge(QStringLiteral("Failed"), kit::Tone::Danger),
                         new kit::KeyHint(QStringLiteral("Enter")),
                         new kit::KeyHint(QStringLiteral("Esc"))}));

    left->addWidget(kit::groupHeading(QStringLiteral("New threads")));
    auto *card = new kit::Card;
    auto *model = new QComboBox;
    model->addItems({QStringLiteral("Automatic"), QStringLiteral("Manual")});
    card->addRow(new kit::SettingsRow(
        QStringLiteral("Model"),
        QStringLiteral("Default model for new threads. Projects can override it."),
        model));
    auto *settle = new kit::Toggle;
    settle->setChecked(true);
    card->addRow(new kit::SettingsRow(
        QStringLiteral("Auto-settle merged threads"),
        QStringLiteral("Settle a thread when its pull request merges."), settle));
    auto *days = new QSpinBox;
    days->setValue(3);
    card->addRow(new kit::SettingsRow(
        QStringLiteral("Days of inactivity before auto-settle"),
        QStringLiteral("Any new activity un-settles a thread automatically."), days));
    card->addRow(new kit::SettingsRow(QStringLiteral("Working section"), QString(),
                                      new kit::Toggle));
    left->addWidget(card);

    auto *notice = new kit::InlineNotice;
    notice->showMessage(QStringLiteral("This environment is desktop-managed."),
                        kit::Tone::Accent);
    left->addWidget(notice);
    left->addStretch();
    root->addLayout(left, 3);

    auto *right = new QVBoxLayout;
    right->setSpacing(14);
    right->addWidget(signInCard(), 0, Qt::AlignTop);

    auto *tabs = new QTabWidget;
    auto *table = new QTableWidget(3, 3);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Status"), QStringLiteral("Updated")});
    const QStringList cells = {
        QStringLiteral("desktop"), QStringLiteral("Open"), QStringLiteral("6d"),
        QStringLiteral("server"),  QStringLiteral("Merged"), QStringLiteral("1w"),
        QStringLiteral("world"),   QStringLiteral("Closed"), QStringLiteral("3w")};
    for (int index = 0; index < cells.size(); ++index)
        table->setItem(index / 3, index % 3, new QTableWidgetItem(cells.at(index)));
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->selectRow(1);
    table->setFixedHeight(150);
    tabs->addTab(table, QStringLiteral("Pull requests"));
    tabs->addTab(new QWidget, QStringLiteral("Issues"));
    tabs->addTab(new QWidget, QStringLiteral("Actions"));
    right->addWidget(tabs);

    auto *files = new QTabWidget;
    files->setTabsClosable(true);
    files->setUsesScrollButtons(true);
    files->setFixedHeight(44);
    for (const char *name : {"CONTRIBUTING.md", "PULL_REQUEST_TEMPLATE.md", "LICENSE",
                             "CMakeLists.txt", ".gitignore", ".mcp.json"})
        files->addTab(new QWidget, QString::fromLatin1(name));
    files->setCurrentIndex(1);
    right->addWidget(files);

    auto *data = new QTableWidget(4, 4);
    data->setProperty("dataTable", true);
    data->setHorizontalHeaderLabels(
        {QStringLiteral("Title"), QStringLiteral("Channel"),
         QStringLiteral("Status"), QStringLiteral("Updated")});
    const QStringList dataCells = {
        QStringLiteral("Three small servers and a long September"),
        QStringLiteral("Blog"), QStringLiteral("published"),
        QStringLiteral("2026-09-24 10:12"),
        QStringLiteral("Signals that lead somewhere"), QStringLiteral("Blog"),
        QStringLiteral("published"), QStringLiteral("2026-09-01 08:40"),
        QStringLiteral("Proof before promises"), QStringLiteral("Newsletter"),
        QStringLiteral("draft"), QStringLiteral("2026-09-01 08:02"),
        QStringLiteral("The mirror becomes a front door"), QStringLiteral("Blog"),
        QStringLiteral("scheduled"), QStringLiteral("2026-08-31 17:25")};
    for (int index = 0; index < dataCells.size(); ++index)
        data->setItem(index / 4, index % 4,
                      new QTableWidgetItem(dataCells.at(index)));
    data->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    data->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->setAlternatingRowColors(true);
    data->selectRow(1);
    data->setFixedHeight(196);
    right->addWidget(data);

    auto *emptyCard = new kit::Card;
    auto *emptyState = new kit::EmptyState(
        QStringLiteral("git-pull-request"), QStringLiteral("No pull requests"),
        QStringLiteral("Pull requests from every project appear here."));
    emptyState->addAction(QStringLiteral("Check again"));
    emptyCard->body()->addWidget(emptyState);
    right->addWidget(emptyCard);
    right->addStretch();
    root->addLayout(right, 2);
    return page;
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const QString outDir =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("ui-snapshots");
    QDir().mkpath(outDir);
    if (app.arguments().contains(QStringLiteral("--timing"))) {
        QElapsedTimer timer;
        timer.start();
        const QString first = Theme::nextStyleSheet(true);
        const qint64 firstNs = timer.nsecsElapsed();
        timer.restart();
        constexpr int kRuns = 20;
        for (int run = 0; run < kRuns; ++run)
            Theme::nextStyleSheet(run % 2 == 0);
        const qint64 averageNs = timer.nsecsElapsed() / kRuns;
        timer.restart();
        Theme::ensureBundledFonts();
        const qint64 fontsNs = timer.nsecsElapsed();
        timer.restart();
        qApp->setStyleSheet(first);
        QWidget probe;
        probe.ensurePolished();
        const qint64 applyNs = timer.nsecsElapsed();
        qInfo("next sheet: %lld chars, first build %.2f ms, later builds %.2f ms, "
              "fonts %.2f ms, apply and polish %.2f ms",
              static_cast<long long>(first.size()), firstNs / 1e6,
              averageNs / 1e6, fontsNs / 1e6, applyNs / 1e6);
        const QString classic = QString::fromLatin1(Theme::styleSheetForDark(true));
        timer.restart();
        qApp->setStyleSheet(classic);
        QWidget classicProbe;
        classicProbe.ensurePolished();
        qInfo("classic sheet: %lld chars, apply and polish %.2f ms",
              static_cast<long long>(classic.size()), timer.nsecsElapsed() / 1e6);
        return 0;
    }

    struct Variant {
        Theme::Design design;
        bool dark;
        const char *name;
    };
    const Variant variants[] = {
        {Theme::Design::Next, true, "gallery-next-dark"},
        {Theme::Design::Next, false, "gallery-next-light"},
        {Theme::Design::Classic, true, "gallery-classic-dark"},
        {Theme::Design::Classic, false, "gallery-classic-light"},
    };
    int failures = 0;
    for (const Variant &variant : variants) {
        applyDesign(variant.design, variant.dark);
        QWidget *page = buildPage();
        page->resize(1240, 1160);
        page->show();
        QApplication::processEvents();
        QApplication::processEvents();
        const QString path =
            QDir(outDir).filePath(QString::fromLatin1(variant.name) +
                                  QStringLiteral(".png"));
        if (!page->grab().save(path))
            ++failures;
        const QString base = QDir(outDir).filePath(QString::fromLatin1(variant.name));
        if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("galleryCombo"))) {
            kit::prepareComboPopup(combo);
            combo->setCurrentIndex(1);
            combo->showPopup();
            QApplication::processEvents();
            QApplication::processEvents();
            if (!saveFloating(combo->view()->window(), base + QStringLiteral("-combo.png")))
                ++failures;
            combo->hidePopup();
        }
        {
            QMenu menu(page);
            menu.addAction(QStringLiteral("To an organization task\u2026"));
            menu.addAction(QStringLiteral("To a repository issue\u2026"));
            menu.addSeparator();
            QAction *pinned = menu.addAction(QStringLiteral("Pinned"));
            pinned->setCheckable(true);
            pinned->setChecked(true);
            QAction *muted = menu.addAction(QStringLiteral("Muted"));
            muted->setCheckable(true);
            QMenu *more = menu.addMenu(QStringLiteral("Move to"));
            more->addAction(QStringLiteral("Archive"));
            QAction *off = menu.addAction(QStringLiteral("Unavailable"));
            off->setEnabled(false);
            menu.setActiveAction(menu.actions().at(1));
            menu.popup(page->mapToGlobal(QPoint(60, 60)));
            QApplication::processEvents();
            QApplication::processEvents();
            if (!saveFloating(&menu, base + QStringLiteral("-menu.png")))
                ++failures;
            menu.hide();
        }
        delete page;
    }
    return failures == 0 ? 0 : 1;
}
