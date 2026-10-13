#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace NoteListEntry {

inline QString storageLabel(const QString &mode)
{
    if (mode == QLatin1String("cloud"))
        return QStringLiteral("Cloud");
    if (mode == QLatin1String("both"))
        return QStringLiteral("Local + cloud");
    return QStringLiteral("Local only");
}

inline QString visibilityLabel(const QJsonObject &note,
                               const QJsonObject &cloud, const QString &mode,
                               bool synced)
{
    if (mode == QLatin1String("local"))
        return QStringLiteral("On this computer");
    if (!synced)
        return QStringLiteral("Not published yet");
    // The local mirror carries the last known visibility, so the row still
    // reads correctly while the cloud listing is unavailable (e.g. signed
    // out) and only the sharing and view details go missing.
    const QJsonObject &status = cloud.isEmpty() ? note : cloud;
    if (status.value(QLatin1String("visibility")).toString() ==
        QLatin1String("public"))
        return QStringLiteral("Public");
    if (!cloud.value(QLatin1String("shares")).toArray().isEmpty())
        return QStringLiteral("Shared");
    return QStringLiteral("Private");
}

inline QString sharedWithLabel(const QJsonObject &cloud)
{
    QStringList names;
    const QJsonArray shares = cloud.value(QLatin1String("shares")).toArray();
    for (const QJsonValue &value : shares) {
        const QJsonObject share = value.toObject();
        const QString name = share.value(QLatin1String("name")).toString();
        if (name.isEmpty())
            continue;
        names << QStringLiteral("%1 (%2)").arg(
            name, share.value(QLatin1String("role")).toString(
                      QStringLiteral("viewer")));
    }
    return names.join(QStringLiteral(", "));
}

inline QString viewsLabel(const QJsonObject &cloud)
{
    const int views = cloud.value(QLatin1String("views")).toInt();
    const int readers = cloud.value(QLatin1String("readers")).toInt();
    if (views <= 0)
        return QString();
    const QString counted = views == 1 ? QStringLiteral("1 view")
                                       : QStringLiteral("%1 views").arg(views);
    return readers > 1 ? QStringLiteral("%1 from %2 readers")
                             .arg(counted).arg(readers)
                       : counted;
}

// Punctuation for a clause that ends a sentence: relay errors arrive both as
// bare fragments ("HTTP 409") and as whole sentences, and a notice that ends in
// "failed: HTTP 409" without a stop reads like it was cut off.
inline QString endSentence(QString text)
{
    if (text.isEmpty())
        return text;
    const QChar last = text.at(text.size() - 1);
    if (last != QLatin1Char('.') && last != QLatin1Char('!') &&
        last != QLatin1Char('?'))
        text += QLatin1Char('.');
    return text;
}

// One sentence naming every destination a save actually reached. Saving a note
// used to confirm itself only in a muted label in the page heading, which is
// easy to miss and never said which half of a "Local + cloud" save took the
// write — so the alert spells the whole outcome out: this computer, the cloud,
// the public web, and the people the note is shared with. `cloud` is the note
// as the relay stored it, and is only consulted when the cloud copy landed.
inline QString savedAlert(const QString &title, const QString &mode,
                          bool savedLocal, bool savedCloud,
                          const QJsonObject &cloud = QJsonObject(),
                          const QString &cloudError = QString())
{
    const QString named = QStringLiteral("“%1”").arg(
        title.trimmed().isEmpty() ? QStringLiteral("Untitled note")
                                  : title.trimmed());
    const bool wantedCloud = mode != QLatin1String("local");
    const bool wantedLocal = mode != QLatin1String("cloud");
    if (!savedLocal && !savedCloud) {
        if (wantedCloud && !cloudError.isEmpty())
            return endSentence(QStringLiteral("Could not save %1 anywhere: %2")
                                   .arg(named, cloudError));
        return QStringLiteral("Could not save %1 on this computer.").arg(named);
    }
    QStringList where;
    if (savedLocal)
        where << QStringLiteral("on this computer");
    if (savedCloud)
        where << QStringLiteral("in the cloud");
    QString text = QStringLiteral("Saved %1 %2").arg(
        named, where.join(QStringLiteral(" and ")));
    // Reaching one of two asked-for destinations is still a partial failure:
    // name the one that was missed, because "Saved" on its own would claim a
    // copy that does not exist.
    if (wantedCloud && !savedCloud)
        return endSentence(
            text + QStringLiteral(" only — the cloud copy failed: %1")
                       .arg(cloudError.isEmpty()
                                ? QStringLiteral("no answer from the relay")
                                : cloudError));
    if (wantedLocal && !savedLocal)
        return QStringLiteral(
            "%1 only — the copy on this computer could not be written.")
            .arg(text);
    text += QLatin1Char('.');
    if (savedCloud) {
        if (cloud.value(QLatin1String("visibility")).toString() ==
            QLatin1String("public"))
            text += QStringLiteral(" Published on the web.");
        const QString shared = sharedWithLabel(cloud);
        if (!shared.isEmpty())
            text += QStringLiteral(" Shared with %1.").arg(shared);
    }
    return text;
}

// Peels the Markdown syntax off one line so it can stand alone as a title:
// heading hashes, blockquote arrows, list bullets, and a task-list box.
inline QString plainMarkdownLine(QString line)
{
    line = line.trimmed();
    for (bool peeled = true; peeled && !line.isEmpty();) {
        peeled = false;
        if (QStringLiteral("#>*+-").contains(line.at(0))) {
            line = line.mid(1).trimmed();
            peeled = true;
        } else if (line.size() >= 3 && line.at(0) == QLatin1Char('[') &&
                   line.at(2) == QLatin1Char(']') &&
                   (line.at(1) == QLatin1Char(' ') ||
                    line.at(1).toLower() == QLatin1Char('x'))) {
            line = line.mid(3).trimmed();
            peeled = true;
        }
    }
    return line;
}

// The one-line title a task or issue inherits when a note is converted into
// one. The note's own title is the heading its writer already chose, so it
// wins; an untitled note falls back to its first line of prose with the
// Markdown peeled off, because "## Ship the installer" reads as a paste
// accident in a column of one-line titles.
inline QString conversionTitle(const QString &title, const QString &markdown,
                               int limit)
{
    QString chosen = title.trimmed();
    if (chosen.isEmpty() || chosen == QLatin1String("Untitled note")) {
        const QStringList sourceLines = markdown.split(QLatin1Char('\n'));
        for (const QString &source : sourceLines) {
            const QString plain = plainMarkdownLine(source);
            if (plain.isEmpty())
                continue;
            chosen = plain;
            break;
        }
    }
    chosen = chosen.simplified();
    if (chosen.isEmpty())
        return QStringLiteral("Untitled note");
    if (limit > 1 && chosen.size() > limit)
        chosen = chosen.left(limit - 1).trimmed() + QStringLiteral("…");
    return chosen;
}

// The body that same conversion carries over: the note itself, then one line
// naming where it came from. Converting leaves the note where it is, so
// without that line the two copies drift apart with nothing left to say they
// were ever the same text. `publicUrl` is only known for a published note.
inline QString conversionDetails(const QString &title, const QString &markdown,
                                 const QString &publicUrl = QString())
{
    QString footer = QStringLiteral("Converted from the note “%1”").arg(
        title.trimmed().isEmpty() ? QStringLiteral("Untitled note")
                                  : title.trimmed());
    const QString address = publicUrl.trimmed();
    if (!address.isEmpty())
        footer += QStringLiteral(" (published at %1)").arg(address);
    footer += QLatin1Char('.');
    const QString body = markdown.trimmed();
    return body.isEmpty() ? footer
                          : body + QStringLiteral("\n\n---\n") + footer;
}

inline QStringList lines(const QJsonObject &note, const QJsonObject &cloud,
                         const QString &mode, bool synced,
                         const QString &stamp = QString())
{
    QStringList status{
        QStringLiteral("Storage: %1").arg(storageLabel(mode)),
        QStringLiteral("Visibility: %1").arg(
            visibilityLabel(note, cloud, mode, synced))};
    const QString views = viewsLabel(cloud);
    if (!views.isEmpty())
        status << QStringLiteral("Views: %1").arg(views);
    status << QStringLiteral("Rev %1").arg(
        qMax(1, note.value(QLatin1String("version")).toInt(1)));
    if (!stamp.isEmpty())
        status << stamp;

    QStringList result{note.value(QLatin1String("title")).toString(
                           QStringLiteral("Untitled note")),
                       status.join(QStringLiteral(" · "))};
    const QString shared = sharedWithLabel(cloud);
    if (!shared.isEmpty())
        result << QStringLiteral("Shared with %1").arg(shared);
    else if (synced)
        result << QStringLiteral("Not shared with anyone");
    return result;
}

} // namespace NoteListEntry
