// Code map (adhoc #187): the repository screen's "Code map" button opens a
// window showing the whole codebase at the browsed ref as a zoomable treemap —
// every folder expanded, each file's real source drawn inside its cell — with a
// toggle to a rotatable 3D view in which a folder's height follows its size.
// It is the desktop twin of the website's code map
// (app/public/assets/codemap/codemap.js).
//
// CodeMapView does the drawing and knows nothing about git. This file is the
// other half: it resolves the ref to a commit once, so every later read shows
// the same revision even if the branch moves while the map is open, lists the
// tree with one `git ls-tree`, and answers the view's requests for file text
// with batched `git cat-file --batch` reads on a worker, which also decodes
// images into the bounded previews the view draws in their cells. Nothing
// here blocks the GUI thread, and every late result checks that its window
// still exists.
#include "MainWindow.h"
#include "CodeMapModel.h"
#include "CodeMapView.h"
#include "MainWindowInternal.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

using namespace forkmesh::ui;

void MainWindow::openRepoCodeMap()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        flashMessage(QStringLiteral("Clone this repository to see its code map."), true);
        return;
    }
    // Copy what the callbacks need: the record can be reassigned while the
    // window is open.
    const int forIndex = m_repoDetailIndex;
    const QString ref = currentRef();
    const RepositoryRecord &record = m_repositories.at(forIndex);
    const QString title = record.owner.isEmpty() ? record.name
                                                 : record.owner + QLatin1Char('/') + record.name;

    auto *window = new QWidget(this, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QStringLiteral("Code map — %1 @ %2").arg(title, ref));
    window->setObjectName(QStringLiteral("codeMapWindow"));
    window->resize(qMax(900, int(width() * 0.85)), qMax(600, int(height() * 0.85)));

    auto *view = new CodeMapView(window);
    auto *crumbs = new QLabel(window);
    crumbs->setTextFormat(Qt::RichText);
    crumbs->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    crumbs->setMinimumWidth(120);
    auto *search = new QLineEdit(window);
    search->setPlaceholderText(QStringLiteral("Find file…"));
    search->setClearButtonEnabled(true);
    search->setMaximumWidth(220);
    search->setAccessibleName(QStringLiteral("Find file by path"));
    auto *flatButton = new QPushButton(QStringLiteral("2D"), window);
    auto *cityButton = new QPushButton(QStringLiteral("3D"), window);
    flatButton->setCheckable(true);
    cityButton->setCheckable(true);
    flatButton->setChecked(true);
    flatButton->setToolTip(QStringLiteral("Flat treemap: every file sized by its bytes"));
    cityButton->setToolTip(QStringLiteral("3D view: folder height follows folder size"));
    auto *modes = new QButtonGroup(window);
    modes->setExclusive(true);
    modes->addButton(flatButton);
    modes->addButton(cityButton);
    auto *zoomOutButton = new QPushButton(QStringLiteral("−"), window);
    auto *zoomInButton = new QPushButton(QStringLiteral("+"), window);
    auto *resetButton = new QPushButton(QStringLiteral("Reset"), window);
    zoomOutButton->setAccessibleName(QStringLiteral("Zoom out"));
    zoomInButton->setAccessibleName(QStringLiteral("Zoom in"));
    zoomOutButton->setToolTip(QStringLiteral("Zoom out"));
    zoomInButton->setToolTip(QStringLiteral("Zoom in"));
    for (QPushButton *button : {flatButton, cityButton, zoomOutButton, zoomInButton, resetButton})
        button->setCursor(Qt::PointingHandCursor);

    auto *legend = new QLabel(window);
    legend->setTextFormat(Qt::RichText);
    auto *status = new QLabel(QStringLiteral("Reading the file tree…"), window);
    auto *hint = new QLabel(window);
    auto *notice = new QLabel(window);
    notice->setWordWrap(true);
    notice->hide();
    auto *focusLabel = new QLabel(window);
    focusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *openButton = new QPushButton(QStringLiteral("Open file"), window);
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->hide();
    for (QLabel *label : {status, hint})
        label->setEnabled(false); // muted, in whatever the theme's disabled ink is

    auto *top = new QHBoxLayout;
    top->setContentsMargins(10, 8, 10, 8);
    top->setSpacing(8);
    top->addWidget(crumbs, 1);
    top->addWidget(search);
    top->addWidget(flatButton);
    top->addWidget(cityButton);
    top->addWidget(zoomOutButton);
    top->addWidget(zoomInButton);
    top->addWidget(resetButton);
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 6);
    bottom->setSpacing(12);
    bottom->addWidget(legend);
    bottom->addWidget(status);
    bottom->addWidget(notice);
    bottom->addStretch(1);
    bottom->addWidget(hint);
    bottom->addWidget(focusLabel);
    bottom->addWidget(openButton);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(view, 1);
    layout->addLayout(bottom);

    const QPointer<CodeMapView> viewGuard(view);
    auto setHint = [hint, view] {
        hint->setText(view->mode() == CodeMapView::Mode::Flat
            ? QStringLiteral("Scroll to zoom · drag to pan · click to zoom in · Esc to zoom out")
            : QStringLiteral("Drag to rotate · right-drag to pan · scroll to zoom · click to fly in"));
    };
    setHint();

    // Opening a file belongs to the repository screen, which may have moved on
    // to another repository since this window was opened.
    auto openInRepo = [this, forIndex, notice](const QString &path) {
        if (forIndex != m_repoDetailIndex) {
            showLocalNotice(notice, QStringLiteral("That code map belongs to a repository that is no longer open."), true);
            return;
        }
        openRepoFile(path);
        raise();
        activateWindow();
    };

    view->setOnFocusChanged([view, crumbs, focusLabel, openButton, title](int focus) {
        const auto &nodes = view->tree().nodes;
        if (focus < 0 || focus >= nodes.size())
            return;
        QVector<int> chain;
        for (int at = focus; at >= 0; at = nodes.at(at).parent)
            chain.prepend(at);
        QStringList parts;
        for (int index : std::as_const(chain)) {
            const QString name = index == 0 ? title : nodes.at(index).name;
            parts << QStringLiteral("<a href=\"%1\" style=\"text-decoration:none\">%2</a>")
                         .arg(index).arg(name.toHtmlEscaped());
        }
        crumbs->setText(parts.join(QStringLiteral(" / ")));
        const bool isFile = nodes.at(focus).type == forkmesh::codemap::Node::File;
        focusLabel->setText(focus == 0
            ? QStringLiteral("%1 · %2 files").arg(view->describeSize(0).section(QStringLiteral(" · "), 0, 0),
                                                       QLocale().toString(view->tree().fileCount))
            : nodes.at(focus).path + QStringLiteral("  ") + view->describeSize(focus));
        openButton->setVisible(isFile);
    });
    connect(crumbs, &QLabel::linkActivated, view, [view](const QString &link) {
        view->goTo(link.toInt());
        view->setFocus();
    });
    connect(openButton, &QPushButton::clicked, view, [view, openInRepo] {
        const int focus = view->focusNode();
        if (focus > 0 && focus < view->tree().nodes.size())
            openInRepo(view->tree().nodes.at(focus).path);
    });
    view->setOnOpenFile(openInRepo);
    view->setOnCloseRequested([window] { window->close(); });
    view->setOnLoadingChanged([status](int pending) {
        status->setText(pending > 0 ? QStringLiteral("Reading %1 file%2…")
                                          .arg(pending).arg(QLatin1String(pending == 1 ? "" : "s"))
                                    : QString());
    });

    connect(flatButton, &QPushButton::clicked, view, [view, setHint] {
        view->setMode(CodeMapView::Mode::Flat);
        setHint();
        view->setFocus();
    });
    connect(cityButton, &QPushButton::clicked, view, [view, setHint] {
        view->setMode(CodeMapView::Mode::City);
        setHint();
        view->setFocus();
    });
    connect(zoomInButton, &QPushButton::clicked, view, [view] { view->zoomBy(1.6); });
    connect(zoomOutButton, &QPushButton::clicked, view, [view] { view->zoomBy(1 / 1.6); });
    connect(resetButton, &QPushButton::clicked, view, [view] { view->resetView(); });
    connect(search, &QLineEdit::textChanged, view, [view, status](const QString &text) {
        const int count = view->setSearch(text);
        status->setText(text.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("%1 matching file%2 · Enter to visit")
                  .arg(QLocale().toString(count), QLatin1String(count == 1 ? "" : "s")));
    });
    connect(search, &QLineEdit::returnPressed, view, [view] { view->visitMatch(false); });

    // File text, in batches, off the GUI thread; images are decoded there too.
    // `commit` is filled in below before the view can ask for anything.
    struct DecodedImage {
        QImage image;
        QSize natural;
    };
    struct Batch {
        QHash<QString, QByteArray> blobs;
        QHash<QString, DecodedImage> images;
    };
    auto commit = std::make_shared<QString>();
    view->setOnWantContent([this, viewGuard, dir, commit](const QStringList &paths) {
        QByteArray input;
        for (const QString &path : paths)
            input += commit->toUtf8() + ':' + path.toUtf8() + '\n';
        runOffThread<Batch>(
            [dir, input, paths] {
                Batch batch;
                const GitCaptureResult result = runGitCaptureDirect(
                    dir, {QStringLiteral("cat-file"), QStringLiteral("--batch")}, &input);
                if (!result.ok)
                    return batch;
                batch.blobs = forkmesh::codemap::parseCatFileBatch(result.output, paths);
                for (auto it = batch.blobs.begin(); it != batch.blobs.end();) {
                    if (forkmesh::codemap::previewKindOf(it.key()) != forkmesh::codemap::Preview::Image) {
                        ++it;
                        continue;
                    }
                    DecodedImage decoded;
                    decoded.image = CodeMapView::decodePreviewImage(it.value(), it.key(), &decoded.natural);
                    if (decoded.image.isNull()) {
                        ++it; // the view decides what undecodable bytes show
                        continue;
                    }
                    batch.images.insert(it.key(), decoded);
                    it = batch.blobs.erase(it);
                }
                return batch;
            },
            [viewGuard, paths](Batch batch) {
                if (!viewGuard)
                    return;
                for (const QString &path : paths) {
                    const auto image = batch.images.constFind(path);
                    if (image != batch.images.constEnd()) {
                        viewGuard->setPreview(path, image->image, image->natural);
                        continue;
                    }
                    const auto found = batch.blobs.constFind(path);
                    viewGuard->setContent(path, found == batch.blobs.constEnd() ? nullptr : &found.value());
                }
                viewGuard->finishContentBatch();
            });
    });

    window->show();
    view->setFocus();

    // Pin the ref to a commit, then list that commit's whole tree.
    runGitDetached(dir, {"rev-parse", "--verify", "--quiet", ref + QStringLiteral("^{commit}")},
                   [this, viewGuard, dir, commit, title, status, legend](bool ok, const QByteArray &out) {
        if (!viewGuard)
            return;
        *commit = QString::fromLatin1(out.trimmed());
        if (!ok || commit->isEmpty()) {
            status->setText(QStringLiteral("This ref has no commit to map yet."));
            return;
        }
        runGitDetached(dir, {"ls-tree", "-r", "-l", "-z", *commit},
                       [viewGuard, title, status, legend](bool listed, const QByteArray &listing) {
            if (!viewGuard)
                return;
            if (!listed) {
                status->setText(QStringLiteral("Couldn't read the file tree."));
                return;
            }
            status->clear();
            viewGuard->setTree(forkmesh::codemap::buildTree(listing, title));
            QStringList entries;
            const auto &languages = viewGuard->tree().languages;
            for (int i = 0; i < languages.size() && i < forkmesh::codemap::kHueSlots; ++i) {
                entries << QStringLiteral("<span style=\"color:%1\">■</span> %2")
                               .arg(viewGuard->languageColor(languages.at(i).first).name(),
                                    languages.at(i).first.toHtmlEscaped());
            }
            if (languages.size() > forkmesh::codemap::kHueSlots) {
                entries << QStringLiteral("<span style=\"color:%1\">■</span> Other")
                               .arg(viewGuard->otherColor().name());
            }
            legend->setText(entries.join(QStringLiteral("&nbsp;&nbsp; ")));
        });
    });
}
