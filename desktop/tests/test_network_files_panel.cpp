#include "FakeSmbServer.h"
#include "NetworkFilesPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>

using namespace forkmesh::smb;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}

void testPanel(const QString &dir)
{
    auto server = std::make_shared<fakesmb::Server>();
    server->files.insert(QStringLiteral("Movies"), fakesmb::Node{true, {}});
    server->files.insert(QStringLiteral("readme.md"), fakesmb::Node{false, "# Router disk\n"});
    server->files.insert(QStringLiteral("photo.png"), fakesmb::Node{false, QByteArray("\x89PNG\r\n\x1a\n", 8)});
    server->files.insert(QStringLiteral(".hidden"), fakesmb::Node{false, "x"});
    server->files.insert(QStringLiteral("big.bin"), fakesmb::Node{false, QByteArray(300000, 'b')});

    NetworkFilesPanel panel(dir + QStringLiteral("/secrets.json"));
    panel.setTransportFactory(fakesmb::factory(server));
    panel.setConfirm([](const QString &, const QString &) { return true; });
    QString nextName;
    panel.setTextPrompt([&nextName](const QString &, const QString &, QString *value) {
        *value = nextName;
        return true;
    });
    QString typedPassword = QStringLiteral("secret");
    int prompts = 0;
    panel.setPasswordPrompt([&](const SavedShare &, QString *password) {
        ++prompts;
        *password = typedPassword;
        return true;
    });
    QString external;
    panel.setOpenExternal([&external](const QString &path) { external = path; });
    QString openedUrl;
    QString openedText;
    panel.openText = [&](const QString &url, const QString &, const QString &text, bool) {
        openedUrl = url;
        openedText = text;
    };

    check(panel.status().contains(QStringLiteral("Add network share")), QStringLiteral("with no shares the panel says how to add one"));

    SavedShare share;
    share.host = QStringLiteral("nas");
    share.share = QStringLiteral("media");
    share.user = QStringLiteral("alice");
    share.remember = true;
    panel.addShare(share, QStringLiteral("secret"));
    check(waitFor([&] { return panel.visibleNames().size() == 4; }), QStringLiteral("adding a share lists its root"));
    check(panel.visibleNames() == QStringList({QStringLiteral("Movies"), QStringLiteral("big.bin"), QStringLiteral("photo.png"), QStringLiteral("readme.md")}),
          QStringLiteral("folders come first, names sort, hidden files stay hidden"));
    check(panel.store().password(panel.currentShare().id) == QStringLiteral("secret") && prompts == 0,
          QStringLiteral("a remembered password is kept in the password file and not asked for"));

    QCheckBox *hidden = nullptr;
    for (QCheckBox *box : panel.findChildren<QCheckBox *>()) {
        if (box->text() == QStringLiteral("Show hidden files"))
            hidden = box;
    }
    QLineEdit *filter = nullptr;
    for (QLineEdit *field : panel.findChildren<QLineEdit *>()) {
        if (field->placeholderText() == QStringLiteral("Filter this folder"))
            filter = field;
    }
    check(hidden && filter, QStringLiteral("the panel has a hidden-files switch and a filter"));
    if (!hidden || !filter)
        return;
    hidden->setChecked(true);
    check(waitFor([&] { return panel.visibleNames().contains(QStringLiteral(".hidden")); }), QStringLiteral("Show hidden files lists dotfiles"));
    filter->setText(QStringLiteral("READ"));
    check(panel.visibleNames() == QStringList({QStringLiteral("readme.md")}), QStringLiteral("the filter narrows the folder by name"));
    filter->clear();

    panel.openEntry(QStringLiteral("readme.md"), false, 14);
    check(waitFor([&] { return !openedUrl.isEmpty(); }) && openedUrl == QStringLiteral("smb://nas/media/readme.md")
              && openedText == QStringLiteral("# Router disk\n"),
          QStringLiteral("a text file opens in the editor with its smb URL"));
    panel.openEntry(QStringLiteral("photo.png"), false, 8);
    check(waitFor([&] { return !external.isEmpty(); }) && readFile(external) == QByteArray("\x89PNG\r\n\x1a\n", 8),
          QStringLiteral("a picture is downloaded to a temporary file and opened with the system app"));

    bool saved = false;
    panel.saveText(openedUrl, QStringLiteral("# Edited\n"), [&saved](bool ok, const QString &) { saved = ok; });
    check(waitFor([&] { return saved; }) && server->read(QStringLiteral("readme.md")) == "# Edited\n", QStringLiteral("saving writes the file back over SMB"));

    nextName = QStringLiteral("Docs");
    panel.newFolder({});
    check(waitFor([&] { return server->exists(QStringLiteral("Docs")) && !panel.busy(); }), QStringLiteral("New folder creates a folder"));
    nextName = QStringLiteral("notes.md");
    panel.renameEntry(QStringLiteral("readme.md"));
    check(waitFor([&] { return server->exists(QStringLiteral("notes.md")) && !server->exists(QStringLiteral("readme.md")) && !panel.busy(); }),
          QStringLiteral("Rename renames on the share"));
    nextName = QStringLiteral("todo.txt");
    openedUrl.clear();
    panel.newFile(QStringLiteral("Docs"));
    check(waitFor([&] { return server->exists(QStringLiteral("Docs/todo.txt")) && openedUrl == QStringLiteral("smb://nas/media/Docs/todo.txt"); }),
          QStringLiteral("New file creates an empty file and opens it"));
    waitFor([&] { return !panel.busy(); });

    panel.copyEntries({QStringLiteral("notes.md")}, {false}, false);
    panel.paste(QStringLiteral("Docs"));
    check(waitFor([&] { return server->exists(QStringLiteral("Docs/notes.md")) && !panel.busy(); }) && server->exists(QStringLiteral("notes.md")),
          QStringLiteral("Copy and Paste copies within the share"));
    panel.copyEntries({QStringLiteral("Docs")}, {true}, true);
    panel.paste(QStringLiteral("Movies"));
    check(waitFor([&] { return server->exists(QStringLiteral("Movies/Docs/notes.md")) && !server->exists(QStringLiteral("Docs")) && !panel.busy(); }),
          QStringLiteral("Move and Paste moves a folder with its contents"));
    check(fileClipboard().items.isEmpty(), QStringLiteral("a finished move empties the clipboard"));

    const QString upload = QDir(dir).filePath(QStringLiteral("upload.txt"));
    writeFile(upload, "from the desktop");
    panel.uploadLocal({upload}, QStringLiteral("Movies"));
    check(waitFor([&] { return server->read(QStringLiteral("Movies/upload.txt")) == "from the desktop" && !panel.busy(); }),
          QStringLiteral("Upload copies a local file to the share"));

    const QString downloads = QDir(dir).filePath(QStringLiteral("downloads"));
    QDir().mkpath(downloads);
    panel.downloadEntry(QStringLiteral("Movies"), true, QDir(downloads).filePath(QStringLiteral("Movies")));
    check(waitFor([&] { return readFile(QDir(downloads).filePath(QStringLiteral("Movies/Docs/notes.md"))) == "# Edited\n" && !panel.busy(); }),
          QStringLiteral("Download copies a folder and everything in it to this computer"));

    panel.copyEntries({QStringLiteral("notes.md")}, {false}, false);
    bool pasted = false;
    panel.pasteIntoLocal(downloads, [&pasted](bool ok) { pasted = ok; });
    check(waitFor([&] { return pasted; }) && readFile(QDir(downloads).filePath(QStringLiteral("notes.md"))) == "# Edited\n",
          QStringLiteral("a network file pastes into a local folder"));
    pasted = false;
    panel.pasteIntoLocal(downloads, [&pasted](bool ok) { pasted = ok; });
    check(waitFor([&] { return pasted; }) && QFile::exists(QDir(downloads).filePath(QStringLiteral("notes (2).md"))),
          QStringLiteral("pasting again keeps both copies"));

    FileClipboard &clipboard = fileClipboard();
    clipboard.items = {ClipboardItem{false, upload, {}, {}, QStringLiteral("upload.txt"), false}};
    clipboard.move = false;
    panel.paste({});
    check(waitFor([&] { return server->read(QStringLiteral("upload.txt")) == "from the desktop" && !panel.busy(); }),
          QStringLiteral("a local file pastes into the share"));
    waitFor([&] { return !panel.busy(); });

    server->readDelayMs = 50;
    panel.downloadEntry(QStringLiteral("big.bin"), false, QDir(downloads).filePath(QStringLiteral("big.bin")));
    panel.cancel();
    check(waitFor([&] { return !panel.busy(); }) && panel.status() == QStringLiteral("Cancelled.")
              && !QFile::exists(QDir(downloads).filePath(QStringLiteral("big.bin"))),
          QStringLiteral("a cancelled download stops and leaves no partial file"));
    server->readDelayMs = 0;

    panel.deleteEntries({QStringLiteral("Movies")}, {true});
    check(waitFor([&] { return !server->exists(QStringLiteral("Movies")) && !panel.busy(); }) && !server->exists(QStringLiteral("Movies/Docs/notes.md")),
          QStringLiteral("Delete removes a folder and everything in it"));

    SavedShare other;
    other.host = QStringLiteral("nas");
    other.share = QStringLiteral("backup");
    other.user = QStringLiteral("alice");
    typedPassword = QStringLiteral("wrong");
    const QString keptId = panel.currentShare().id;
    SavedShare stored = panel.store().save(other);
    panel.selectShare(stored.id);
    check(waitFor([&] { return panel.status() == QStringLiteral("The user name or password is wrong."); }) && prompts == 1,
          QStringLiteral("a share without a remembered password asks for it and reports a wrong one"));
    check(panel.store().password(stored.id).isEmpty(), QStringLiteral("a password that is not remembered never reaches the password file"));
    panel.selectShare(keptId);
    check(waitFor([&] { return panel.visibleNames().contains(QStringLiteral("notes.md")); }), QStringLiteral("switching back lists the first share again"));

    panel.openUrl(QStringLiteral("smb://nas/media/"), false);
    check(panel.currentShare().id == keptId && panel.store().shares().size() == 2, QStringLiteral("opening a saved share's URL reuses it"));
    server->allowGuest = true;
    panel.openUrl(QStringLiteral("smb://192.168.1.1/media/"), true);
    check(panel.store().shares().size() == 3 && panel.currentShare().guest && panel.currentShare().host == QStringLiteral("192.168.1.1"),
          QStringLiteral("a router share opened as a guest is saved and browsed"));
    check(waitFor([&] { return panel.visibleNames().contains(QStringLiteral("notes.md")); }), QStringLiteral("the guest share lists its files"));
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshNetworkFilesTests"));
    QCoreApplication::setApplicationName(QStringLiteral("network-files-tests"));
    testPanel(dir.path());
    return failures == 0 ? 0 : 1;
}
