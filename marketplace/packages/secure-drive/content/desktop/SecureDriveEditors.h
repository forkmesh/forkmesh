#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>

#include <functional>

class QCloseEvent;
class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace forkmesh::securedrive {

using SaveHandler = std::function<bool(const QByteArray &, QString *)>;

class EditorWindow : public QDialog
{
public:
    EditorWindow(const QString &name, bool readOnly, SaveHandler save, QWidget *parent);
    ~EditorWindow() override;

    QString fileId;
    bool isDirty() const { return m_dirty; }
    bool isReadOnly() const { return m_readOnly; }
    void discardAndClose();
    bool saveNow();

    static QByteArray newDocument();
    static QByteArray newSpreadsheet();
    static QByteArray newDrawing();

protected:
    void closeEvent(QCloseEvent *event) override;
    virtual QByteArray serialize() const = 0;
    void setDirty(bool dirty);
    QHBoxLayout *toolbar() const { return m_toolbar; }
    QVBoxLayout *body() const { return m_body; }

private:
    void updateTitle();

    QString m_name;
    bool m_readOnly = false;
    bool m_dirty = false;
    SaveHandler m_save;
    QHBoxLayout *m_toolbar = nullptr;
    QVBoxLayout *m_body = nullptr;
    QLabel *m_status = nullptr;
};

bool editableKind(const QString &kind);
EditorWindow *openEditor(const QString &kind, const QString &name, const QByteArray &content,
                         bool readOnly, SaveHandler save, QWidget *parent, QString *error);

}  // namespace forkmesh::securedrive
