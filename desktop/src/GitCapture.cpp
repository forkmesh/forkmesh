#include "GitCapture.h"

#include "MainWindowInternal.h"

namespace forkmesh::git {

bool capture(const QString &dir, const QStringList &args, QByteArray *out,
             QString *err)
{
    return forkmesh::ui::runGitCapture(dir, args, out, err);
}

}
