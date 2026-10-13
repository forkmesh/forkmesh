#include "DestructiveCommandReview.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVector>

#include <utility>

namespace forkmesh::destructive {

namespace {

struct Word {
    QString text;
    QString raw;
};

QString code(const QString &text)
{
    return QStringLiteral("<code>%1</code>").arg(text.toHtmlEscaped());
}

QVector<Word> shellWords(const QString &text)
{
    QVector<Word> words;
    Word word;
    bool open = false;
    QChar quote;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (!quote.isNull()) {
            word.raw += c;
            if (c == quote) {
                quote = QChar();
            } else if (c == QLatin1Char('\\') && quote == QLatin1Char('"') &&
                       i + 1 < text.size()) {
                word.text += text.at(++i);
                word.raw += text.at(i);
            } else {
                word.text += c;
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            word.raw += c;
            open = true;
        } else if (c == QLatin1Char('\\') && i + 1 < text.size()) {
            word.text += text.at(++i);
            word.raw += c;
            word.raw += text.at(i);
            open = true;
        } else if (c.isSpace()) {
            if (open)
                words.append(word);
            word = Word();
            open = false;
        } else {
            word.text += c;
            word.raw += c;
            open = true;
        }
    }
    if (open)
        words.append(word);
    static const QRegularExpression redirect(QStringLiteral("^[0-9]*(>>?|<|>&|&>)(.*)$"));
    QVector<Word> out;
    for (qsizetype i = 0; i < words.size(); ++i) {
        const QRegularExpressionMatch match = redirect.match(words.at(i).raw);
        if (match.hasMatch()) {
            if (match.captured(2).isEmpty())
                ++i;
            continue;
        }
        out.append(words.at(i));
    }
    return out;
}

bool isWrapper(const QString &program)
{
    static const QSet<QString> wrappers{
        QStringLiteral("sudo"),  QStringLiteral("env"),    QStringLiteral("timeout"),
        QStringLiteral("nice"),  QStringLiteral("nohup"),  QStringLiteral("xargs"),
        QStringLiteral("time"),  QStringLiteral("command"), QStringLiteral("exec"),
        QStringLiteral("stdbuf"), QStringLiteral("ionice")};
    return wrappers.contains(program);
}

bool isAssignment(const QString &word)
{
    static const QRegularExpression assignment(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*="));
    return assignment.match(word).hasMatch();
}

bool isDuration(const QString &word)
{
    static const QRegularExpression duration(QStringLiteral("^[0-9.]+[smhd]?$"));
    return duration.match(word).hasMatch();
}

QString baseProgram(const QString &word)
{
    return QFileInfo(word).fileName().toLower();
}

qsizetype programIndex(const QVector<Word> &words)
{
    bool wrapped = false;
    for (qsizetype i = 0; i < words.size(); ++i) {
        const QString &word = words.at(i).text;
        if (isAssignment(word))
            continue;
        if (wrapped && (word.startsWith(QLatin1Char('-')) || isDuration(word)))
            continue;
        if (isWrapper(baseProgram(word))) {
            wrapped = true;
            continue;
        }
        return i;
    }
    return -1;
}

QVector<Word> operandsAfter(const QVector<Word> &words, qsizetype from)
{
    QVector<Word> operands;
    bool literal = false;
    for (qsizetype i = from; i < words.size(); ++i) {
        const QString &word = words.at(i).text;
        if (!literal && word == QLatin1String("--")) {
            literal = true;
            continue;
        }
        if (!literal && word.startsWith(QLatin1Char('-')))
            continue;
        operands.append(words.at(i));
    }
    return operands;
}

bool hasFlag(const QVector<Word> &words, qsizetype from, const QString &longFlag, QChar shortFlag)
{
    for (qsizetype i = from; i < words.size(); ++i) {
        const QString &word = words.at(i).text;
        if (word == QLatin1String("--"))
            break;
        if (!longFlag.isEmpty() && (word == longFlag || word.startsWith(longFlag + QLatin1Char('='))))
            return true;
        if (!shortFlag.isNull() && word.size() > 1 && word.startsWith(QLatin1Char('-')) &&
            !word.startsWith(QLatin1String("--")) && word.mid(1).contains(shortFlag))
            return true;
    }
    return false;
}

QString listed(const QStringList &items)
{
    QStringList shown;
    for (const QString &item : items)
        shown << code(item);
    if (shown.size() <= 1)
        return shown.join(QString());
    const QString last = shown.takeLast();
    return shown.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
}

bool protectedBranch(const QString &name)
{
    static const QRegularExpression shared(QStringLiteral(
        "^(refs/heads/)?(main|master|trunk|develop|development|prod|production|stable|"
        "release([/-].*)?)$"));
    return shared.match(name).hasMatch();
}

QString unquotedVariable(const Word &word)
{
    static const QRegularExpression variable(QStringLiteral("\\$\\{?([A-Za-z_][A-Za-z0-9_]*)"));
    QChar quote;
    for (qsizetype i = 0; i < word.raw.size(); ++i) {
        const QChar c = word.raw.at(i);
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            continue;
        }
        if (c == QLatin1Char('$')) {
            const QRegularExpressionMatch match = variable.match(word.raw, i);
            if (match.hasMatch() && match.capturedStart() == i)
                return match.captured(0) + (match.captured(0).contains(QLatin1Char('{'))
                                                ? QStringLiteral("}")
                                                : QString());
        }
    }
    return QString();
}

QString withoutVariables(const QString &text)
{
    static const QRegularExpression variable(
        QStringLiteral("\\$\\{[^}]*\\}|\\$[A-Za-z_][A-Za-z0-9_]*"));
    QString stripped = text;
    stripped.remove(variable);
    return stripped;
}

void reviewStashDrop(Review &out, const QVector<Word> &words, qsizetype program,
                     const QString &before, const QString &segment)
{
    qsizetype stash = -1;
    for (qsizetype i = program + 1; i < words.size(); ++i)
        if (words.at(i).text == QLatin1String("stash")) {
            stash = i;
            break;
        }
    const QVector<Word> operands = operandsAfter(words, stash < 0 ? words.size() : stash + 1);
    const QString action = operands.isEmpty() ? QString() : operands.first().text;
    if (action == QLatin1String("clear")) {
        out.what = QStringLiteral("%1 deletes every entry in this repository’s stash, and the "
                                  "changes saved in each one.")
                       .arg(code(QStringLiteral("git stash clear")));
        out.forDeny << QStringLiteral(
            "The stash is shared by the main checkout and every worktree of the repository, "
            "so this also deletes entries that other agents or you pushed.");
        out.undo = QStringLiteral(
            "The dropped commits stay in the repository until git gc prunes them; "
            "%1 lists them, but there is no record of which was which.")
                       .arg(code(QStringLiteral("git fsck --unreachable | grep commit")));
        out.lean = Review::Lean::Deny;
        out.leanReason = QStringLiteral("It deletes stash entries this agent did not create.");
        return;
    }
    const Word ref = operands.size() > 1 ? operands.at(1) : Word();
    const QString variable = unquotedVariable(ref);
    if (ref.text.isEmpty())
        out.what = QStringLiteral("%1 with no entry named deletes the newest stash entry "
                                  "(%2) and the changes saved in it.")
                       .arg(code(QStringLiteral("git stash drop")),
                            code(QStringLiteral("stash@{0}")));
    else
        out.what = QStringLiteral("%1 deletes stash entry %2 and the changes saved in it.")
                       .arg(code(QStringLiteral("git stash drop ") + ref.raw), code(ref.raw));
    if (ref.raw.contains(QLatin1Char('$')))
        out.what += QStringLiteral(" %1 is filled in when the command runs, so the entry it "
                                   "names is only decided then.")
                        .arg(code(ref.raw));

    static const QRegularExpression pushed(QStringLiteral(
        "stash\\s+(?:push|save)\\b[^;&|]*?(?:-m|--message)[= ]\\s*(['\"]?)([^'\"\\s;&|]+)\\1"));
    const QRegularExpressionMatch push = pushed.match(before);
    const QString tag = push.hasMatch() ? push.captured(2) : QString();
    const bool lookedUp = !tag.isEmpty() && before.mid(push.capturedEnd()).contains(tag);
    if (!tag.isEmpty())
        out.forAllow << (lookedUp
                             ? QStringLiteral("Earlier in the same command it pushes its own "
                                              "stash tagged %1 and finds the entry by that tag, "
                                              "so it drops the entry it made rather than "
                                              "whatever is newest.")
                                   .arg(code(tag))
                             : QStringLiteral("Earlier in the same command it pushes its own "
                                              "stash tagged %1.")
                                   .arg(code(tag)));
    static const QRegularExpression applied(QStringLiteral("stash\\s+(?:apply|pop)\\b"));
    const bool restored =
        applied.match(push.hasMatch() ? before.mid(push.capturedEnd()) : before).hasMatch();
    if (restored)
        out.forAllow << QStringLiteral(
            "It applies the entry back first (git stash apply), so its changes are in the "
            "working tree again before the entry is deleted.");
    else
        out.forDeny << QStringLiteral(
            "Nothing earlier in the command applies the entry back, so its changes are lost "
            "once it is dropped.");
    if (ref.text.isEmpty())
        out.forDeny << QStringLiteral(
            "With no entry named it drops whatever is newest. The stash is shared across "
            "worktrees, so another session may have pushed an entry since.");
    else if (!variable.isEmpty())
        out.forDeny << QStringLiteral(
            "%1 is not quoted: if the lookup finds no match it comes out empty, the command "
            "becomes plain %2 and deletes the newest entry instead, which may belong to "
            "another session.")
                           .arg(code(variable), code(QStringLiteral("git stash drop")));
    if (segment.contains(QLatin1String("/dev/null")))
        out.forDeny << QStringLiteral(
            "Its output goes to /dev/null, so the transcript will not record the id git prints "
            "for the dropped entry, the easiest way to get it back.");
    out.undo = QStringLiteral(
        "The entry’s commit stays in the repository until git gc prunes it. Git prints its "
        "id when it drops it; %1 restores it, and %2 finds it if the id was not kept.")
                   .arg(code(QStringLiteral("git stash apply <id>")),
                        code(QStringLiteral("git fsck --unreachable | grep commit")));
    if (!tag.isEmpty() && restored) {
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral(
            "It stashes its own changes, restores them and then drops only that entry.");
    } else if (ref.text.isEmpty() && !restored) {
        out.lean = Review::Lean::Deny;
        out.leanReason = QStringLiteral(
            "It deletes the newest entry without restoring it, and that entry may not be "
            "this agent’s.");
    }
}

QString resolveVariables(const QString &text, const QHash<QString, QString> &values,
                         bool *complete)
{
    static const QRegularExpression variable(
        QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}|\\$([A-Za-z_][A-Za-z0-9_]*)"));
    QString out;
    qsizetype from = 0;
    *complete = true;
    auto it = variable.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        out += text.mid(from, match.capturedStart() - from);
        if (values.contains(name))
            out += values.value(name);
        else
            *complete = false;
        from = match.capturedEnd();
    }
    return out + text.mid(from);
}

void reviewRecursiveDelete(Review &out, const QVector<Word> &words, qsizetype program,
                           const QString &homeDir, const QHash<QString, QString> &values,
                           const QString &scratchDir, bool proven)
{
    const QString name = baseProgram(words.at(program).text);
    QVector<Word> targets;
    if (name == QLatin1String("find")) {
        for (qsizetype i = program + 1; i < words.size(); ++i) {
            const QString &word = words.at(i).text;
            if (word.startsWith(QLatin1Char('-')) || word == QLatin1String("(") ||
                word == QLatin1String("!"))
                break;
            targets.append(words.at(i));
        }
        if (targets.isEmpty())
            targets.append({QStringLiteral("."), QStringLiteral(".")});
    } else if (name == QLatin1String("rm")) {
        targets = operandsAfter(words, program + 1);
    }
    QStringList shown;
    QStringList shownResolved;
    for (const Word &target : targets) {
        shown << target.raw;
        bool complete = false;
        const QString resolved = target.raw.contains(QLatin1Char('$'))
                                     ? resolveVariables(target.text, values, &complete)
                                     : QString();
        shownResolved << (complete && !resolved.isEmpty()
                              ? QStringLiteral("%1 (%2)").arg(code(target.raw), code(resolved))
                              : code(target.raw));
    }
    const QString targetList = [&shownResolved] {
        QStringList items = shownResolved;
        if (items.size() <= 1)
            return items.join(QString());
        const QString last = items.takeLast();
        return items.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
    }();
    if (name.startsWith(QLatin1String("python")))
        out.what = QStringLiteral("%1 in Python code deletes a whole folder tree, without going "
                                  "through the trash.")
                       .arg(code(QStringLiteral("shutil.rmtree")));
    else if (name == QLatin1String("find"))
        out.what = QStringLiteral("%1 deletes every file and folder under %2 that matches its "
                                  "expression, without going through the trash.")
                       .arg(code(QStringLiteral("find … -delete")), targetList);
    else
        out.what = QStringLiteral("%1 deletes %2 and everything inside, without going through "
                                  "the trash.")
                       .arg(code(QStringLiteral("rm -r")),
                            shown.isEmpty() ? QStringLiteral("its targets") : targetList);
    out.undo = QStringLiteral(
        "Files tracked by git come back with git restore; untracked or ignored files are "
        "gone unless they are backed up elsewhere.");

    static const QSet<QString> roots{
        QStringLiteral("/"),     QStringLiteral("/*"),      QStringLiteral("~"),
        QStringLiteral("~/"),    QStringLiteral("~/*"),     QStringLiteral("$HOME"),
        QStringLiteral("${HOME}"), QStringLiteral("$HOME/"), QStringLiteral("$HOME/*"),
        QStringLiteral("."),     QStringLiteral("./"),      QStringLiteral("./*"),
        QStringLiteral(".."),    QStringLiteral("*"),       QStringLiteral(".*")};
    static const QSet<QString> regenerable{
        QStringLiteral("build"),        QStringLiteral("dist"),   QStringLiteral("out"),
        QStringLiteral("node_modules"), QStringLiteral("__pycache__"),
        QStringLiteral(".pytest_cache"), QStringLiteral("target"), QStringLiteral(".cache"),
        QStringLiteral("coverage"),     QStringLiteral(".next"),  QStringLiteral(".venv"),
        QStringLiteral("venv"),         QStringLiteral(".tox"),   QStringLiteral(".mypy_cache"),
        QStringLiteral(".gradle")};
    static const QRegularExpression forkmeshData(QStringLiteral(
        "(projects/forkmesh/(data|config|ssh|state|logs|src)(/|$)|/\\.forkmesh(/|$)|"
        "\\.config/ForkMesh|\\.local/share/ForkMesh|projects/forkmesh-backup)"));
    static const QRegularExpression scratch(QStringLiteral(
        "^(\"?\\$\\{?(FORKMESH_AGENT_SCRATCH|TMPDIR)\\}?|/var/tmp/|/tmp/|/dev/shm/)"));
    bool dangerous = false;
    int safe = 0;
    for (const Word &target : targets) {
        const QString text = target.text;
        bool complete = false;
        const QString resolved = target.raw.contains(QLatin1Char('$'))
                                     ? resolveVariables(text, values, &complete)
                                     : QString();
        const bool known = complete && !resolved.isEmpty();
        QString expanded = known ? resolved : text;
        if (expanded.startsWith(QLatin1Char('~')) && !homeDir.isEmpty())
            expanded = homeDir + expanded.mid(1);
        expanded.replace(QStringLiteral("${HOME}"), homeDir).replace(QStringLiteral("$HOME"), homeDir);
        const QString clean = QDir::cleanPath(expanded);
        if (roots.contains(text) || roots.contains(target.raw) || clean == QLatin1String("/") ||
            (!homeDir.isEmpty() && clean == QDir::cleanPath(homeDir))) {
            out.forDeny << QStringLiteral("%1 is %2.").arg(
                code(target.raw),
                text.startsWith(QLatin1Char('.'))
                    ? QStringLiteral("the current folder (or everything in it)")
                : text.startsWith(QLatin1Char('*')) ? QStringLiteral("everything in the current folder")
                : clean == QLatin1String("/") ? QStringLiteral("the whole file system")
                                              : QStringLiteral("your home folder"));
            dangerous = true;
            continue;
        }
        if (known && !scratchDir.isEmpty() && !resolved.contains(QLatin1String("..")) &&
            clean.startsWith(QDir::cleanPath(scratchDir) + QLatin1Char('/'))) {
            out.forAllow << QStringLiteral("%1 is %2, inside the agent’s own scratch folder.")
                                .arg(code(target.raw), code(clean));
            ++safe;
            continue;
        }
        const QString variable = unquotedVariable(target);
        if (!known && (!variable.isEmpty() || target.raw.contains(QLatin1Char('$')))) {
            const QString bare = withoutVariables(text);
            const QString shownBare = bare.isEmpty() ? QStringLiteral("nothing") : code(bare);
            if (scratch.match(target.raw).hasMatch() && !text.contains(QLatin1String(".."))) {
                out.forAllow << QStringLiteral("%1 is inside the agent’s scratch or temp folder.")
                                    .arg(code(target.raw));
                ++safe;
                continue;
            }
            out.forDeny << QStringLiteral("%1 depends on a variable; if it is empty or unset the "
                                          "path becomes %2.")
                               .arg(code(target.raw), shownBare);
            if (bare.isEmpty() || QDir::cleanPath(bare) == QLatin1String("/") ||
                bare.startsWith(QLatin1Char('/')) || bare == QLatin1String("*"))
                dangerous = true;
        }
        if (text.contains(QLatin1String("..")))
            out.forDeny << QStringLiteral("%1 climbs out of the folder with “..”.")
                               .arg(code(target.raw));
        if (forkmeshData.match(clean).hasMatch()) {
            out.forDeny << QStringLiteral("%1 is ForkMesh data (identity key, settings, keys or "
                                          "agent history), which must never be erased.")
                               .arg(code(target.raw));
            dangerous = true;
        }
        if (QFileInfo(clean).fileName() == QLatin1String(".git") ||
            clean.contains(QLatin1String("/.git/")) || clean.contains(QLatin1String(".worktrees"))) {
            out.forDeny << QStringLiteral("%1 holds git history, branches or worktrees.")
                               .arg(code(target.raw));
            dangerous = true;
        }
        if (text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('?')))
            out.forDeny << QStringLiteral("%1 is a pattern: it deletes whatever matches when it "
                                          "runs.")
                               .arg(code(target.raw));
        if (scratch.match(target.raw).hasMatch() || scratch.match(clean).hasMatch()) {
            out.forAllow << QStringLiteral("%1 is inside a scratch or temp folder.")
                                .arg(code(target.raw));
            ++safe;
        } else if (regenerable.contains(QFileInfo(clean).fileName()) ||
                   QFileInfo(clean).fileName().startsWith(QLatin1String("cmake-build"))) {
            out.forAllow << QStringLiteral("%1 looks like build output or a cache that can be "
                                           "regenerated.")
                                .arg(code(target.raw));
            ++safe;
        }
    }
    if (proven) {
        out.forAllow << QStringLiteral("Nothing earlier in the command can change what these "
                                       "paths point to.");
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral("Everything it deletes is inside the agent’s own scratch "
                                        "folder.");
    } else if (dangerous) {
        out.lean = Review::Lean::Deny;
        out.leanReason = QStringLiteral("A target is a folder that should never be deleted, or "
                                        "can turn into one.");
    } else if (!targets.isEmpty() && safe == targets.size() && out.forDeny.isEmpty()) {
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral("Everything it deletes is scratch or can be rebuilt.");
    }
}

void reviewForcePush(Review &out, const QVector<Word> &words, qsizetype sub)
{
    const QVector<Word> operands = operandsAfter(words, sub + 1);
    const QString remote = operands.isEmpty() ? QStringLiteral("the remote")
                                              : operands.first().text;
    QStringList branches;
    bool deletes = hasFlag(words, sub + 1, QStringLiteral("--delete"), QLatin1Char('d'));
    for (qsizetype i = 1; i < operands.size(); ++i) {
        QString spec = operands.at(i).text;
        if (spec.startsWith(QLatin1Char('+')))
            spec = spec.mid(1);
        if (spec.startsWith(QLatin1Char(':')))
            deletes = true;
        branches << (spec.contains(QLatin1Char(':')) ? spec.section(QLatin1Char(':'), 1) : spec);
    }
    const bool mirror = hasFlag(words, sub + 1, QStringLiteral("--mirror"), QChar());
    const bool lease = hasFlag(words, sub + 1, QStringLiteral("--force-with-lease"), QChar());
    const QString shownBranches =
        branches.isEmpty() ? QStringLiteral("the current branch") : listed(branches);
    if (mirror)
        out.what = QStringLiteral("%1 makes %2 an exact copy of this repository: every branch "
                                  "and tag there that you do not have locally is deleted.")
                       .arg(code(QStringLiteral("git push --mirror")), code(remote));
    else if (deletes)
        out.what = QStringLiteral("It deletes %1 on %2.").arg(shownBranches, code(remote));
    else
        out.what = QStringLiteral("A force push replaces %1 on %2 with your local copy; commits "
                                  "there that your local branch does not have disappear from it.")
                       .arg(shownBranches, code(remote));
    out.undo = QStringLiteral(
        "The old commits survive only in clones that already have them (and for a while in the "
        "host’s activity log); otherwise they are gone from the remote.");
    bool shared = mirror;
    for (const QString &branch : branches)
        if (protectedBranch(branch)) {
            out.forDeny << QStringLiteral("%1 is a shared branch: other people’s commits on it "
                                          "would be lost.")
                               .arg(code(branch));
            shared = true;
        }
    if (mirror)
        out.forDeny << QStringLiteral("--mirror affects every branch and tag on the remote, not "
                                      "one branch.");
    if (lease)
        out.forAllow << QStringLiteral("--force-with-lease refuses if the remote branch moved "
                                       "since your last fetch, so it cannot overwrite commits "
                                       "someone else pushed.");
    else if (!deletes && !mirror)
        out.forDeny << QStringLiteral("Plain --force does not check whether someone else pushed "
                                      "in the meantime; --force-with-lease would.");
    if (!shared && !branches.isEmpty())
        out.forAllow << QStringLiteral("%1 is not a main or release branch.").arg(shownBranches);
    if (shared) {
        out.lean = Review::Lean::Deny;
        out.leanReason = QStringLiteral("It rewrites or deletes a shared branch.");
    } else if (lease && !branches.isEmpty()) {
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral("A leased push to a working branch cannot lose anyone "
                                        "else’s commits.");
    }
}

void reviewResetHard(Review &out, const QVector<Word> &words, qsizetype sub, const QString &before)
{
    const QVector<Word> operands = operandsAfter(words, sub + 1);
    const QString target = operands.isEmpty() ? QString() : operands.first().text;
    out.what = QStringLiteral("%1 throws away every uncommitted change to tracked files in this "
                              "worktree%2.")
                   .arg(code(QStringLiteral("git reset --hard")),
                        target.isEmpty() ? QString()
                                         : QStringLiteral(" and moves the current branch to %1")
                                               .arg(code(target)));
    out.undo = QStringLiteral("Commits come back from %1; uncommitted changes do not.")
                   .arg(code(QStringLiteral("git reflog")));
    out.forDeny << QStringLiteral("Uncommitted edits, the agent’s or yours, are gone for good.");
    const bool moves = !target.isEmpty() && target != QLatin1String("HEAD");
    if (moves)
        out.forDeny << QStringLiteral("Commits after %1 leave the branch (they stay in the "
                                      "reflog for a while).")
                           .arg(code(target));
    else
        out.forAllow << QStringLiteral("It resets to the current commit, so no commits leave the "
                                       "branch.");
    static const QRegularExpression saved(QStringLiteral("git\\s+(stash(\\s+push)?|commit)\\b"));
    if (saved.match(before).hasMatch()) {
        out.forAllow << QStringLiteral("Earlier in the command it saves work first (stash or "
                                       "commit).");
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral("The work it discards was saved just before.");
    }
}

void reviewClean(Review &out, const QVector<Word> &words, qsizetype sub)
{
    const bool folders = hasFlag(words, sub + 1, QString(), QLatin1Char('d'));
    const bool ignored = hasFlag(words, sub + 1, QString(), QLatin1Char('x'));
    const bool onlyIgnored = hasFlag(words, sub + 1, QString(), QLatin1Char('X'));
    const bool dryRun = hasFlag(words, sub + 1, QStringLiteral("--dry-run"), QLatin1Char('n'));
    out.what = QStringLiteral("%1 deletes %2%3.")
                   .arg(code(QStringLiteral("git clean -f")),
                        onlyIgnored ? QStringLiteral("files git ignores (build output, caches)")
                                    : QStringLiteral("files git does not track"),
                        QString(folders ? QStringLiteral(", whole untracked folders too") : QString()) +
                            (ignored ? QStringLiteral(", and ignored files such as build output, "
                                                      ".env files and local settings")
                                     : QString()));
    out.undo = QStringLiteral("They were never in git, so git cannot restore them.");
    if (dryRun) {
        out.forAllow << QStringLiteral("-n is a dry run: it lists what it would delete and "
                                       "deletes nothing.");
        out.lean = Review::Lean::Allow;
        out.leanReason = QStringLiteral("A dry run deletes nothing.");
        return;
    }
    if (onlyIgnored)
        out.forAllow << QStringLiteral("-X deletes only ignored files; new source files stay.");
    else
        out.forDeny << QStringLiteral("New files nobody has added to git yet are deleted for "
                                      "good.");
    if (ignored) {
        out.forDeny << QStringLiteral("-x also deletes ignored files: .env files, local "
                                      "configuration and caches.");
        out.lean = Review::Lean::Deny;
        out.leanReason = QStringLiteral("It deletes ignored files that are often secrets or "
                                        "local settings.");
    }
}

void reviewBranchDelete(Review &out, const QVector<Word> &words, qsizetype sub)
{
    QStringList names;
    for (const Word &word : operandsAfter(words, sub + 1))
        names << word.text;
    out.what = QStringLiteral("%1 deletes local branch %2 even if its commits are not merged "
                              "anywhere.")
                   .arg(code(QStringLiteral("git branch -D")),
                        names.isEmpty() ? QStringLiteral("(none named)") : listed(names));
    out.undo = QStringLiteral("%1 shows the branch’s last commit for about 90 days; %2 brings "
                              "it back.")
                   .arg(code(QStringLiteral("git reflog")),
                        code(QStringLiteral("git branch <name> <commit>")));
    out.forDeny << QStringLiteral("Commits that are only on that branch stop being reachable.");
    out.forAllow << QStringLiteral("Only the local branch goes; a copy on the remote is "
                                   "untouched.");
    for (const QString &name : names)
        if (protectedBranch(name)) {
            out.forDeny << QStringLiteral("%1 is a main or release branch.").arg(code(name));
            out.lean = Review::Lean::Deny;
            out.leanReason = QStringLiteral("It deletes a shared branch.");
        }
}

void reviewHistoryRewrite(Review &out, const QVector<Word> &words, qsizetype sub)
{
    const QString name = words.at(sub).text;
    const QVector<Word> operands = operandsAfter(words, sub + 1);
    if (name == QLatin1String("filter-branch") || name == QLatin1String("filter-repo")) {
        out.what = QStringLiteral("%1 rewrites every commit on the branches it touches, giving "
                                  "them new ids.")
                       .arg(code(QStringLiteral("git ") + name));
        out.forDeny << QStringLiteral("Every other clone, worktree and open pull request is left "
                                      "on the old history.");
        out.undo = name == QLatin1String("filter-branch")
                       ? QStringLiteral("filter-branch keeps the old branches under "
                                        "refs/original until you delete them.")
                       : QStringLiteral("filter-repo keeps no backup; only another clone has the "
                                        "old history.");
    } else if (name == QLatin1String("reflog")) {
        out.what = QStringLiteral("%1 deletes reflog entries, the record git uses to recover "
                                  "dropped commits.")
                       .arg(code(QStringLiteral("git reflog expire")));
        out.forDeny << QStringLiteral("It removes the safety net for earlier hard resets, deleted "
                                      "branches and dropped stashes.");
        out.undo = QStringLiteral("Not recoverable.");
    } else if (name == QLatin1String("gc")) {
        out.what = QStringLiteral("%1 deletes unreachable commits now instead of after git’s "
                                  "usual two-week grace period.")
                       .arg(code(QStringLiteral("git gc --prune=now")));
        out.forDeny << QStringLiteral("Dropped stashes, deleted branches and reset commits "
                                      "can no longer be recovered.");
        out.undo = QStringLiteral("Not recoverable.");
    } else {
        out.what = QStringLiteral("%1 deletes ref %2 directly, without the checks git branch and "
                                  "git tag make.")
                       .arg(code(QStringLiteral("git update-ref -d")),
                            operands.isEmpty() ? QStringLiteral("(none named)")
                                               : code(operands.first().text));
        out.forDeny << QStringLiteral("Commits only that ref pointed to stop being reachable.");
        out.undo = QStringLiteral("%1 may still show where it pointed.")
                       .arg(code(QStringLiteral("git reflog")));
    }
    out.lean = Review::Lean::Deny;
    out.leanReason = QStringLiteral("History rewrites are rarely part of an agent’s task; leave "
                                    "them to a person.");
}

void reviewDiskWipe(Review &out, const QVector<Word> &words, qsizetype program)
{
    const QString name = baseProgram(words.at(program).text);
    const QVector<Word> operands = operandsAfter(words, program + 1);
    QString target = operands.isEmpty() ? QString() : operands.last().text;
    for (const Word &word : words)
        if (word.text.startsWith(QLatin1String("of=")))
            target = word.text.mid(3);
    const QString shown = target.isEmpty() ? QStringLiteral("its target") : code(target);
    if (name == QLatin1String("dd"))
        out.what = QStringLiteral("%1 writes straight onto device %2, overwriting the file "
                                  "system on it.")
                       .arg(code(QStringLiteral("dd")), shown);
    else if (name.startsWith(QLatin1String("mkfs")))
        out.what = QStringLiteral("%1 makes a new, empty file system on %2, erasing what was "
                                  "there.")
                       .arg(code(name), shown);
    else if (name == QLatin1String("wipefs"))
        out.what = QStringLiteral("%1 erases the file system signatures on %2, so its data no "
                                  "longer mounts.")
                       .arg(code(name), shown);
    else
        out.what = QStringLiteral("%1 overwrites %2 repeatedly so it cannot be recovered.")
                       .arg(code(name), shown);
    out.undo = QStringLiteral("Not recoverable.");
    out.forDeny << QStringLiteral("An agent working on code almost never needs to wipe a disk "
                                  "or shred a file.");
    out.lean = Review::Lean::Deny;
    out.leanReason = QStringLiteral("The data is gone for good.");
}

void reviewRepoDelete(Review &out, const QVector<Word> &words, qsizetype program)
{
    const QVector<Word> operands = operandsAfter(words, program + 1);
    const QString repo = operands.size() > 2 ? operands.at(2).text : QString();
    out.what = QStringLiteral("%1 deletes the hosted repository %2 with its issues, pull "
                              "requests, wiki, releases and settings.")
                   .arg(code(baseProgram(words.at(program).text) + QStringLiteral(" repo delete")),
                        repo.isEmpty() ? QStringLiteral("of the current folder") : code(repo));
    out.undo = QStringLiteral("GitHub keeps a deleted repository restorable for about 90 days "
                              "(Settings → Repositories → Deleted repositories); other hosts "
                              "may not.");
    out.forDeny << QStringLiteral("Every clone loses its remote, and collaborators lose the "
                                  "issues and reviews.");
    out.lean = Review::Lean::Deny;
    out.leanReason = QStringLiteral("Deleting a repository is not an agent’s call.");
}

enum class Kind { Plain, Program, Sub, Flag, String, Variable, Operator, Path, Comment, Assign };

struct Style {
    QString colour;
    bool bold = false;
    bool italic = false;
};

Style styleFor(Kind kind, bool dark)
{
    switch (kind) {
    case Kind::Program:
        return {dark ? QStringLiteral("#79c0ff") : QStringLiteral("#0550ae"), true};
    case Kind::Sub:
        return {dark ? QStringLiteral("#d2a8ff") : QStringLiteral("#8250df"), true};
    case Kind::Flag:
        return {dark ? QStringLiteral("#ffa657") : QStringLiteral("#953800")};
    case Kind::String:
        return {dark ? QStringLiteral("#a5d6ff") : QStringLiteral("#0a3069")};
    case Kind::Variable:
    case Kind::Assign:
        return {dark ? QStringLiteral("#7ee787") : QStringLiteral("#116329")};
    case Kind::Operator:
        return {dark ? QStringLiteral("#ff7b72") : QStringLiteral("#cf222e"), true};
    case Kind::Path:
        return {dark ? QStringLiteral("#e3b341") : QStringLiteral("#6f4e00")};
    case Kind::Comment:
        return {dark ? QStringLiteral("#8b949e") : QStringLiteral("#6e7781"), false, true};
    case Kind::Plain:
        break;
    }
    return {};
}

struct Mark {
    Kind kind = Kind::Plain;
    int link = -1;
};

bool operatorChar(QChar c)
{
    return c == QLatin1Char('|') || c == QLatin1Char(';') || c == QLatin1Char('&') ||
           c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('(') ||
           c == QLatin1Char(')') || c == QLatin1Char('`');
}

class Lexer
{
public:
    Lexer(const QString &command, const QString &cwd, const QString &homeDir)
        : m_text(command), m_cwd(cwd), m_home(homeDir), m_marks(command.size())
    {
    }

    void run()
    {
        const qsizetype n = m_text.size();
        qsizetype i = 0;
        while (i < n) {
            const QChar c = m_text.at(i);
            if (c == QLatin1Char('\n')) {
                startCommand();
                ++i;
            } else if (c.isSpace()) {
                ++i;
            } else if (c == QLatin1Char('#') && (i == 0 || m_text.at(i - 1).isSpace())) {
                qsizetype end = m_text.indexOf(QLatin1Char('\n'), i);
                if (end < 0)
                    end = n;
                paint(i, end, Kind::Comment);
                i = end;
            } else if (c == QLatin1Char('$') && i + 1 < n && m_text.at(i + 1) == QLatin1Char('(')) {
                paint(i, i + 2, Kind::Operator);
                m_stack.append(m_state);
                startCommand();
                i += 2;
            } else if (c == QLatin1Char(')') && !m_stack.isEmpty()) {
                paint(i, i + 1, Kind::Operator);
                m_state = m_stack.takeLast();
                ++i;
            } else if (operatorChar(c) ||
                       (c.isDigit() && i + 1 < n &&
                        (m_text.at(i + 1) == QLatin1Char('>') || m_text.at(i + 1) == QLatin1Char('<')) &&
                        (i == 0 || m_text.at(i - 1).isSpace()))) {
                i = lexOperator(i);
            } else {
                i = lexWord(i);
            }
        }
    }

    const QVector<Mark> &marks() const { return m_marks; }
    const QStringList &links() const { return m_links; }

private:
    struct State {
        bool atCommand = true;
        bool wrapped = false;
        bool redirect = false;
        bool valueNext = false;
        QString program;
        QStringList operands;
    };

    void startCommand() { m_state = State(); }

    void paint(qsizetype from, qsizetype to, Kind kind, int link = -1)
    {
        for (qsizetype k = from; k < to && k < m_marks.size(); ++k)
            m_marks[k] = {kind, link};
    }

    int addLink(const QString &url)
    {
        if (url.isEmpty())
            return -1;
        m_links.append(url);
        return int(m_links.size() - 1);
    }

    qsizetype lexOperator(qsizetype i)
    {
        const qsizetype n = m_text.size();
        qsizetype end = i;
        while (end < n && m_text.at(end).isDigit())
            ++end;
        const QChar c = m_text.at(end);
        if (c == QLatin1Char('>') || c == QLatin1Char('<')) {
            ++end;
            if (end < n && (m_text.at(end) == QLatin1Char('>') || m_text.at(end) == QLatin1Char('&')))
                ++end;
            if (end < n && m_text.at(end - 1) == QLatin1Char('&') &&
                (m_text.at(end).isDigit() || m_text.at(end) == QLatin1Char('-'))) {
                ++end;
            } else {
                m_state.redirect = true;
            }
            paint(i, end, Kind::Operator);
            return end;
        }
        if (c == QLatin1Char('&') && end + 1 < n && m_text.at(end + 1) == QLatin1Char('>')) {
            end += 2;
            m_state.redirect = true;
            paint(i, end, Kind::Operator);
            return end;
        }
        ++end;
        if (end < n && (m_text.at(end) == c || (c == QLatin1Char('|') && m_text.at(end) == QLatin1Char('&'))) &&
            c != QLatin1Char('(') && c != QLatin1Char(')'))
            ++end;
        paint(i, end, Kind::Operator);
        if (c != QLatin1Char(')'))
            startCommand();
        return end;
    }

    qsizetype lexWord(qsizetype start)
    {
        const qsizetype n = m_text.size();
        qsizetype i = start;
        QString text;
        QVector<std::pair<qsizetype, qsizetype>> strings;
        QVector<std::pair<qsizetype, qsizetype>> variables;
        static const QRegularExpression variable(
            QStringLiteral("\\$(\\{[^}]*\\}|[A-Za-z_][A-Za-z0-9_]*|[0-9#?@*$!-])"));
        while (i < n) {
            const QChar c = m_text.at(i);
            if (c.isSpace() || operatorChar(c))
                break;
            if (c == QLatin1Char('$') && i + 1 < n && m_text.at(i + 1) == QLatin1Char('('))
                break;
            if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
                qsizetype close = i + 1;
                while (close < n && m_text.at(close) != c) {
                    if (c == QLatin1Char('"') && m_text.at(close) == QLatin1Char('\\'))
                        ++close;
                    ++close;
                }
                close = qMin(close + 1, n);
                strings.append({i, close});
                if (c == QLatin1Char('"')) {
                    QRegularExpressionMatchIterator it = variable.globalMatch(
                        m_text.left(close), i);
                    while (it.hasNext()) {
                        const QRegularExpressionMatch match = it.next();
                        variables.append({match.capturedStart(), match.capturedEnd()});
                    }
                }
                text += m_text.mid(i + 1, close - i - 2);
                i = close;
                continue;
            }
            if (c == QLatin1Char('\\') && i + 1 < n) {
                text += m_text.at(i + 1);
                i += 2;
                continue;
            }
            if (c == QLatin1Char('$')) {
                const QRegularExpressionMatch match = variable.match(m_text, i);
                if (match.hasMatch() && match.capturedStart() == i) {
                    variables.append({i, match.capturedEnd()});
                    text += match.captured(0);
                    i = match.capturedEnd();
                    continue;
                }
            }
            text += c;
            ++i;
        }
        if (i == start)
            return start + 1;
        const QString raw = m_text.mid(start, i - start);
        classify(start, i, text, raw);
        for (const auto &range : strings)
            paint(range.first, range.second, Kind::String, linkAt(range.first));
        for (const auto &range : variables)
            paint(range.first, range.second, Kind::Variable, linkAt(range.first));
        return i;
    }

    int linkAt(qsizetype at) const { return at < m_marks.size() ? m_marks.at(at).link : -1; }

    void classify(qsizetype from, qsizetype to, const QString &text, const QString &raw)
    {
        State &s = m_state;
        if (s.redirect) {
            s.redirect = false;
            paint(from, to, Kind::Path, pathLink(text));
            return;
        }
        if (s.atCommand) {
            if (isAssignment(raw)) {
                const qsizetype eq = raw.indexOf(QLatin1Char('='));
                paint(from, from + eq + 1, Kind::Assign);
                return;
            }
            if (s.wrapped && (text.startsWith(QLatin1Char('-')) || isDuration(text))) {
                paint(from, to, text.startsWith(QLatin1Char('-')) ? Kind::Flag : Kind::Plain);
                return;
            }
            const QString program = baseProgram(text);
            int link = addLink(programUrl(program));
            if (link < 0 && text.contains(QLatin1Char('/')))
                link = pathLink(text);
            paint(from, to, Kind::Program, link);
            s.program = program;
            s.operands.clear();
            s.wrapped = isWrapper(program);
            s.atCommand = s.wrapped;
            return;
        }
        if (s.valueNext) {
            s.valueNext = false;
            paint(from, to, text.contains(QLatin1Char('/')) ? Kind::Path : Kind::Plain,
                  text.contains(QLatin1Char('/')) ? pathLink(text) : -1);
            return;
        }
        if (text.startsWith(QLatin1Char('-')) && text.size() > 1) {
            paint(from, to, Kind::Flag);
            if (s.program == QLatin1String("git") && s.operands.isEmpty() &&
                (text == QLatin1String("-C") || text == QLatin1String("-c")))
                s.valueNext = true;
            if (s.program.startsWith(QLatin1String("python")) && text == QLatin1String("-m")) {
                s.operands << QStringLiteral("-m");
            }
            return;
        }
        s.operands << text;
        const int index = int(s.operands.size()) - 1;
        if (s.program.startsWith(QLatin1String("python")) && index == 1 &&
            s.operands.first() == QLatin1String("-m")) {
            paint(from, to, Kind::Sub, addLink(programUrl(text)));
            return;
        }
        static const QSet<QString> nested{
            QStringLiteral("stash"),  QStringLiteral("worktree"), QStringLiteral("remote"),
            QStringLiteral("reflog"), QStringLiteral("submodule"), QStringLiteral("notes"),
            QStringLiteral("bisect"), QStringLiteral("sparse-checkout")};
        const bool gitSub = s.program == QLatin1String("git") &&
                            (index == 0 || (index == 1 && nested.contains(s.operands.first())));
        const bool ghSub = (s.program == QLatin1String("gh") || s.program == QLatin1String("glab")) &&
                           index <= 1;
        if (gitSub || ghSub) {
            paint(from, to, Kind::Sub, addLink(subcommandUrl(s.program, s.operands)));
            return;
        }
        if (text.contains(QLatin1Char('/')) || text.startsWith(QLatin1Char('~')) ||
            text.startsWith(QLatin1Char('.'))) {
            paint(from, to, Kind::Path, pathLink(text));
            return;
        }
        const int link = pathLink(text, false);
        paint(from, to, link >= 0 ? Kind::Path : Kind::Plain, link);
    }

    int pathLink(const QString &text, bool allowAbsolute = true)
    {
        if (text.isEmpty() || text.contains(QLatin1Char('$')) || text.contains(QLatin1Char('*')) ||
            text.contains(QLatin1Char('?')) || text.startsWith(QLatin1String("/dev/")))
            return -1;
        QString path = text;
        if (path.startsWith(QLatin1Char('~')) && !m_home.isEmpty())
            path = m_home + path.mid(1);
        if (QDir::isRelativePath(path)) {
            if (m_cwd.isEmpty())
                return -1;
            path = QDir(m_cwd).filePath(path);
        } else if (!allowAbsolute) {
            return -1;
        }
        const QFileInfo info(path);
        if (!info.exists())
            return -1;
        return addLink(QUrl::fromLocalFile(info.absoluteFilePath()).toString());
    }

    QString m_text;
    QString m_cwd;
    QString m_home;
    QVector<Mark> m_marks;
    QStringList m_links;
    State m_state;
    QVector<State> m_stack;
};

} // namespace

QStringList kinds()
{
    return {QStringLiteral("recursive delete"),     QStringLiteral("scratch delete"),
            QStringLiteral("force push"),
            QStringLiteral("discard changes"),      QStringLiteral("delete untracked files"),
            QStringLiteral("branch delete"),        QStringLiteral("stash drop"),
            QStringLiteral("history rewrite"),      QStringLiteral("disk wipe"),
            QStringLiteral("repository delete")};
}

QString kindExample(const QString &kind)
{
    static const QHash<QString, QString> examples{
        {QStringLiteral("recursive delete"), QStringLiteral("rm -r, find -delete")},
        {QStringLiteral("scratch delete"),
         QStringLiteral("rm -r of folders proven to be inside the agent’s own scratch folder")},
        {QStringLiteral("force push"), QStringLiteral("git push --force, remote branch deletes")},
        {QStringLiteral("discard changes"), QStringLiteral("git reset --hard")},
        {QStringLiteral("delete untracked files"), QStringLiteral("git clean -f")},
        {QStringLiteral("branch delete"), QStringLiteral("git branch -D")},
        {QStringLiteral("stash drop"), QStringLiteral("git stash drop, git stash clear")},
        {QStringLiteral("history rewrite"),
         QStringLiteral("filter-branch, reflog expire, gc --prune=now")},
        {QStringLiteral("disk wipe"), QStringLiteral("dd of=/dev/…, mkfs, wipefs, shred")},
        {QStringLiteral("repository delete"), QStringLiteral("gh repo delete")}};
    return examples.value(kind);
}

Review review(const QString &command, const QString &kind, qsizetype matchStart,
              qsizetype matchLength, const QHash<QString, QString> &values,
              const QString &scratch)
{
    Review out;
    const bool matched = matchStart >= 0 && matchLength > 0 &&
                         matchStart + matchLength <= command.size();
    const QString segment = matched ? command.mid(matchStart, matchLength) : command;
    const QString before = matched ? command.left(matchStart) : QString();
    const QVector<Word> words = shellWords(segment);
    const qsizetype program = programIndex(words);
    qsizetype sub = -1;
    if (program >= 0 && baseProgram(words.at(program).text) == QLatin1String("git")) {
        for (qsizetype i = program + 1; i < words.size(); ++i) {
            const QString &word = words.at(i).text;
            if (word == QLatin1String("-C") || word == QLatin1String("-c")) {
                ++i;
                continue;
            }
            if (!word.startsWith(QLatin1Char('-'))) {
                sub = i;
                break;
            }
        }
    }
    if (program < 0) {
        out.what = QStringLiteral("The command matches the “%1” rule.").arg(kind.toHtmlEscaped());
        return out;
    }
    if (kind == QLatin1String("stash drop"))
        reviewStashDrop(out, words, program, before, segment);
    else if (kind == QLatin1String("recursive delete") || kind == QLatin1String("scratch delete"))
        reviewRecursiveDelete(out, words, program, QDir::homePath(), values, scratch,
                              kind == QLatin1String("scratch delete"));
    else if (kind == QLatin1String("force push") && sub >= 0)
        reviewForcePush(out, words, sub);
    else if (kind == QLatin1String("discard changes") && sub >= 0)
        reviewResetHard(out, words, sub, before);
    else if (kind == QLatin1String("delete untracked files") && sub >= 0)
        reviewClean(out, words, sub);
    else if (kind == QLatin1String("branch delete") && sub >= 0)
        reviewBranchDelete(out, words, sub);
    else if (kind == QLatin1String("history rewrite") && sub >= 0)
        reviewHistoryRewrite(out, words, sub);
    else if (kind == QLatin1String("disk wipe"))
        reviewDiskWipe(out, words, program);
    else if (kind == QLatin1String("repository delete"))
        reviewRepoDelete(out, words, program);
    else
        out.what = QStringLiteral("%1 matches the “%2” rule.")
                       .arg(code(segment.simplified()), kind.toHtmlEscaped());
    return out;
}

QString programUrl(const QString &program)
{
    static const QSet<QString> section1{
        QStringLiteral("rm"),       QStringLiteral("rmdir"),   QStringLiteral("find"),
        QStringLiteral("dd"),       QStringLiteral("shred"),   QStringLiteral("cat"),
        QStringLiteral("sort"),     QStringLiteral("grep"),    QStringLiteral("cut"),
        QStringLiteral("diff"),     QStringLiteral("tail"),    QStringLiteral("head"),
        QStringLiteral("sed"),      QStringLiteral("ls"),      QStringLiteral("cp"),
        QStringLiteral("mv"),       QStringLiteral("mkdir"),   QStringLiteral("chmod"),
        QStringLiteral("chown"),    QStringLiteral("ln"),      QStringLiteral("xargs"),
        QStringLiteral("tar"),      QStringLiteral("timeout"), QStringLiteral("env"),
        QStringLiteral("nice"),     QStringLiteral("nohup"),   QStringLiteral("tee"),
        QStringLiteral("wc"),       QStringLiteral("uniq"),    QStringLiteral("tr"),
        QStringLiteral("echo"),     QStringLiteral("touch"),   QStringLiteral("du"),
        QStringLiteral("df"),       QStringLiteral("stat"),    QStringLiteral("readlink"),
        QStringLiteral("realpath"), QStringLiteral("basename"), QStringLiteral("dirname"),
        QStringLiteral("sleep"),    QStringLiteral("kill"),    QStringLiteral("printf"),
        QStringLiteral("test"),     QStringLiteral("true"),    QStringLiteral("false"),
        QStringLiteral("rsync"),    QStringLiteral("ssh"),     QStringLiteral("scp"),
        QStringLiteral("curl"),     QStringLiteral("make"),    QStringLiteral("stdbuf")};
    static const QSet<QString> section8{QStringLiteral("wipefs"), QStringLiteral("mkfs"),
                                        QStringLiteral("ionice")};
    if (program == QLatin1String("git"))
        return QStringLiteral("https://git-scm.com/docs/git");
    if (program == QLatin1String("gh"))
        return QStringLiteral("https://cli.github.com/manual/gh");
    if (program == QLatin1String("glab"))
        return QStringLiteral("https://docs.gitlab.com/cli/");
    if (program == QLatin1String("python") || program.startsWith(QLatin1String("python3")))
        return QStringLiteral("https://docs.python.org/3/using/cmdline.html");
    if (program == QLatin1String("pytest"))
        return QStringLiteral("https://docs.pytest.org/en/stable/how-to/usage.html");
    if (program == QLatin1String("sudo"))
        return QStringLiteral("https://www.sudo.ws/docs/man/sudo.man/");
    if (program.startsWith(QLatin1String("mkfs")))
        return QStringLiteral("https://man7.org/linux/man-pages/man8/mkfs.8.html");
    if (section8.contains(program))
        return QStringLiteral("https://man7.org/linux/man-pages/man8/%1.8.html").arg(program);
    if (section1.contains(program))
        return QStringLiteral("https://man7.org/linux/man-pages/man1/%1.1.html").arg(program);
    return QString();
}

QString subcommandUrl(const QString &program, const QStringList &operands)
{
    static const QRegularExpression safe(QStringLiteral("^[a-z][a-z0-9-]*$"));
    QStringList parts;
    for (const QString &operand : operands.mid(0, 2))
        if (safe.match(operand).hasMatch())
            parts << operand;
        else
            break;
    if (parts.isEmpty())
        return QString();
    if (program == QLatin1String("git"))
        return QStringLiteral("https://git-scm.com/docs/git-%1").arg(parts.first());
    if (program == QLatin1String("gh"))
        return QStringLiteral("https://cli.github.com/manual/gh_%1").arg(parts.join(QLatin1Char('_')));
    if (program == QLatin1String("glab"))
        return QStringLiteral("https://docs.gitlab.com/cli/%1/").arg(parts.join(QLatin1Char('/')));
    return QString();
}

QString commandHtml(const QString &command, qsizetype matchStart, qsizetype matchLength,
                    const QString &cwd, const QString &homeDir, bool dark)
{
    Lexer lexer(command, cwd, homeDir);
    lexer.run();
    const QVector<Mark> &marks = lexer.marks();
    const QStringList &links = lexer.links();
    const bool matched = matchStart >= 0 && matchLength > 0 &&
                         matchStart + matchLength <= command.size();
    const auto inMatch = [&](qsizetype at) {
        return matched && at >= matchStart && at < matchStart + matchLength;
    };
    QString html = QStringLiteral("<p style=\"white-space:pre-wrap; margin:0\">");
    qsizetype i = 0;
    while (i < command.size()) {
        const Mark mark = marks.at(i);
        const bool hot = inMatch(i);
        qsizetype end = i + 1;
        while (end < command.size() && marks.at(end).kind == mark.kind &&
               marks.at(end).link == mark.link && inMatch(end) == hot)
            ++end;
        if (matched && i == matchStart)
            html += QStringLiteral("<a name=\"match\"></a>");
        const Style style = styleFor(mark.kind, dark);
        QStringList css;
        if (hot) {
            css << QStringLiteral("background-color:#ffd166") << QStringLiteral("color:#201800")
                << QStringLiteral("font-weight:bold");
        } else {
            if (!style.colour.isEmpty())
                css << QStringLiteral("color:%1").arg(style.colour);
            if (style.bold)
                css << QStringLiteral("font-weight:bold");
        }
        if (style.italic)
            css << QStringLiteral("font-style:italic");
        QString run = command.mid(i, end - i).toHtmlEscaped();
        if (!css.isEmpty())
            run = QStringLiteral("<span style=\"%1\">%2</span>").arg(css.join(QLatin1Char(';')), run);
        if (mark.link >= 0)
            run = QStringLiteral("<a href=\"%1\" style=\"text-decoration:underline\">%2</a>")
                      .arg(links.at(mark.link).toHtmlEscaped(), run);
        html += run;
        i = end;
    }
    html += QStringLiteral("</p>");
    return html;
}

} // namespace forkmesh::destructive
