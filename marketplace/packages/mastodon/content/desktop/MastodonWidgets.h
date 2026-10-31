#pragma once

#include <QFrame>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPixmap>
#include <QString>

#include <functional>
#include <memory>

class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace forkmesh::mastodon {

class Client;

QIcon icon(const QString &name, const QColor &color, int side = 16);
QPixmap roundedPixmap(const QImage &image, int side, qreal dpr, bool circle);
QPixmap letterAvatar(const QString &name, int side, qreal dpr);
QColor mutedColor(const QWidget *widget);
QColor accentColor(const QWidget *widget);
QColor favouriteColor();
QColor boostColor();
QToolButton *iconButton(const QString &iconName, const QString &tip, QWidget *parent = nullptr);
QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr);
QLabel *richLabel(const QString &html, QWidget *parent = nullptr);

struct CardContext {
    Client *client = nullptr;
    QString instance;
    QString selfId;
    QString filterContext;
    std::function<void(const QJsonObject &)> reply;
    std::function<void(const QJsonObject &, const QString &)> act;
    std::function<void(const QJsonObject &, QToolButton *)> more;
    std::function<void(const QString &)> openThread;
    std::function<void(const QJsonObject &)> openAccount;
    std::function<void(const QString &)> openLink;
    std::function<void(const QJsonArray &, int)> openMedia;
    std::function<void(const QJsonObject &, const QList<int> &)> vote;
};

using CardContextPtr = std::shared_ptr<CardContext>;

class AvatarLabel : public QLabel
{
public:
    AvatarLabel(int side, QWidget *parent = nullptr);
    void setAccount(Client *client, const QJsonObject &account);
    void setSource(Client *client, const QString &url, const QString &name);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

public:
    std::function<void()> clicked;

private:
    int m_side;
    QString m_url;
};

class MediaTile : public QWidget
{
public:
    MediaTile(const QJsonObject &attachment, bool sensitive, QWidget *parent = nullptr);
    void setImage(const QImage &image);
    std::function<void()> clicked;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QJsonObject m_attachment;
    QImage m_image;
    bool m_hidden = false;
};

class PostCard : public QFrame
{
public:
    PostCard(const QJsonObject &status, CardContextPtr context, bool focused = false,
             QWidget *parent = nullptr);
    void setStatus(const QJsonObject &status);
    QJsonObject status() const { return m_status; }
    QJsonObject shown() const;
    QString shownId() const;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void build();
    QWidget *buildPoll(const QJsonObject &poll);
    QWidget *buildMedia(const QJsonArray &media, bool sensitive);
    QWidget *buildCardPreview(const QJsonObject &card);
    QToolButton *actionButton(const QString &iconName, const QString &tip, const QString &count,
                              bool active, const QColor &activeColor);

    QJsonObject m_status;
    CardContextPtr m_context;
    bool m_focused = false;
    bool m_revealed = false;
    bool m_filterRevealed = false;
    QVBoxLayout *m_layout = nullptr;
};

class AccountRow : public QFrame
{
public:
    AccountRow(const QJsonObject &account, CardContextPtr context, bool showNote = true,
               QWidget *parent = nullptr);
    void addTrailing(QWidget *widget);
    QJsonObject account() const { return m_account; }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QJsonObject m_account;
    CardContextPtr m_context;
    QLayout *m_trailing = nullptr;
};

}
