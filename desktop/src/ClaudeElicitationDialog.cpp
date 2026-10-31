#include "ClaudeElicitationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *plainLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString jsonText(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble())
        return QString::number(value.toDouble());
    return value.toString();
}

} // namespace

ClaudeElicitationDialog::ClaudeElicitationDialog(const QString &sessionTitle,
                                                 const QJsonObject &requestEvent,
                                                 QWidget *parent)
    : QDialog(parent)
{
    namespace cc = forkmesh::claudecontrol;
    setObjectName(QStringLiteral("claudeElicitationDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    const cc::Prompt prompt = cc::promptFor(requestEvent);
    m_key = prompt.key;
    const QJsonObject body = requestEvent.value(QStringLiteral("request")).toObject();
    const QString server = body.value(QStringLiteral("mcp_server_name")).toString().left(80);
    const QString displayName = body.value(QStringLiteral("display_name")).toString().left(80);
    const QString title = body.value(QStringLiteral("title")).toString().left(160);
    m_urlMode = body.value(QStringLiteral("mode")).toString() == QLatin1String("url");
    setWindowTitle(sessionTitle.trimmed().isEmpty()
                       ? QStringLiteral("Claude Code needs your input")
                       : QStringLiteral("%1 — input needed").arg(sessionTitle.left(80)));

    auto *layout = new QVBoxLayout(this);
    auto *heading = plainLabel(
        !title.isEmpty() ? title
                         : QStringLiteral("%1 asks for input")
                               .arg(!displayName.isEmpty()
                                        ? displayName
                                        : (server.isEmpty() ? QStringLiteral("An MCP server")
                                                            : server)),
        this);
    heading->setObjectName(QStringLiteral("claudeElicitationHeading"));
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    layout->addWidget(heading);
    const QString message = body.value(QStringLiteral("message")).toString().left(2000);
    if (!message.trimmed().isEmpty())
        layout->addWidget(plainLabel(message, this));
    const QString description = body.value(QStringLiteral("description")).toString().left(600);
    if (!description.trimmed().isEmpty())
        layout->addWidget(plainLabel(description, this));

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *accept = nullptr;
    if (m_urlMode) {
        const QString url = body.value(QStringLiteral("url")).toString().left(2000);
        auto *link = plainLabel(url, this);
        link->setObjectName(QStringLiteral("claudeElicitationUrl"));
        layout->addWidget(link);
        const QUrl parsed(url);
        auto *open = buttons->addButton(QStringLiteral("Open link"), QDialogButtonBox::ActionRole);
        open->setObjectName(QStringLiteral("claudeElicitationOpenButton"));
        const bool web = parsed.isValid() && (parsed.scheme() == QLatin1String("https") ||
                                              parsed.scheme() == QLatin1String("http"));
        open->setEnabled(web);
        connect(open, &QPushButton::clicked, this,
                [parsed] { QDesktopServices::openUrl(parsed); });
        accept = buttons->addButton(QStringLiteral("Done"), QDialogButtonBox::AcceptRole);
    } else {
        m_fields = cc::elicitationFields(requestEvent);
        auto *form = new QFormLayout;
        for (const cc::ElicitationField &field : std::as_const(m_fields)) {
            QWidget *editor = nullptr;
            if (!field.choices.isEmpty()) {
                auto *combo = new QComboBox(this);
                if (!field.required)
                    combo->addItem(QString(), QString());
                for (int i = 0; i < field.choices.size(); ++i)
                    combo->addItem(field.choiceLabels.value(i, field.choices.at(i)),
                                   field.choices.at(i));
                const int at = combo->findData(jsonText(field.defaultValue));
                if (at >= 0)
                    combo->setCurrentIndex(at);
                editor = combo;
            } else if (field.type == QLatin1String("boolean")) {
                auto *check = new QCheckBox(this);
                check->setChecked(field.defaultValue.toBool());
                editor = check;
            } else {
                auto *edit = new QLineEdit(this);
                edit->setText(jsonText(field.defaultValue));
                if (field.secret)
                    edit->setEchoMode(QLineEdit::Password);
                if (!field.description.isEmpty())
                    edit->setPlaceholderText(field.description.left(120));
                editor = edit;
            }
            editor->setObjectName(QStringLiteral("claudeElicitationField_%1").arg(field.name));
            if (!field.description.isEmpty())
                editor->setToolTip(field.description);
            m_editors.append(editor);
            form->addRow(field.required ? field.label + QStringLiteral(" *") : field.label,
                         editor);
        }
        layout->addLayout(form);
        accept = buttons->addButton(QStringLiteral("Send"), QDialogButtonBox::AcceptRole);
    }
    accept->setObjectName(QStringLiteral("claudeElicitationAcceptButton"));
    accept->setDefault(true);
    auto *decline = buttons->addButton(QStringLiteral("Decline"), QDialogButtonBox::RejectRole);
    decline->setObjectName(QStringLiteral("claudeElicitationDeclineButton"));
    auto *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("claudeElicitationCancelButton"));

    m_error = new QLabel(this);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setObjectName(QStringLiteral("claudeElicitationError"));
    m_error->hide();
    layout->addWidget(m_error);
    layout->addWidget(buttons);

    connect(accept, &QPushButton::clicked, this, [this] { finishWith(QStringLiteral("accept")); });
    connect(decline, &QPushButton::clicked, this,
            [this] { finishWith(QStringLiteral("decline")); });
    connect(cancel, &QPushButton::clicked, this, [this] { finishWith(QStringLiteral("cancel")); });
    connect(this, &QDialog::rejected, this, [this] {
        if (!m_finished) {
            m_finished = true;
            emit answered(m_key, QStringLiteral("cancel"), {});
        }
    });
    resize(qMax(sizeHint().width(), 420), sizeHint().height());
}

QMap<QString, QString> ClaudeElicitationDialog::values() const
{
    QMap<QString, QString> out;
    for (int i = 0; i < m_fields.size() && i < m_editors.size(); ++i) {
        QWidget *editor = m_editors.at(i);
        QString value;
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            value = combo->currentData().toString();
        else if (auto *check = qobject_cast<QCheckBox *>(editor))
            value = check->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
        else if (auto *edit = qobject_cast<QLineEdit *>(editor))
            value = edit->text();
        out.insert(m_fields.at(i).name, value);
    }
    return out;
}

void ClaudeElicitationDialog::setValueForTest(const QString &name, const QString &value)
{
    for (int i = 0; i < m_fields.size() && i < m_editors.size(); ++i) {
        if (m_fields.at(i).name != name)
            continue;
        QWidget *editor = m_editors.at(i);
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            combo->setCurrentIndex(qMax(0, combo->findData(value)));
        else if (auto *check = qobject_cast<QCheckBox *>(editor))
            check->setChecked(value == QLatin1String("true"));
        else if (auto *edit = qobject_cast<QLineEdit *>(editor))
            edit->setText(value);
    }
}

void ClaudeElicitationDialog::finishWith(const QString &action)
{
    if (m_finished)
        return;
    QJsonObject content;
    if (action == QLatin1String("accept") && !m_urlMode) {
        QString error;
        content = forkmesh::claudecontrol::elicitationContent(m_fields, values(), &error);
        if (!error.isEmpty()) {
            m_error->setText(error);
            m_error->show();
            return;
        }
    }
    m_finished = true;
    emit answered(m_key, action, content);
    accept();
}
