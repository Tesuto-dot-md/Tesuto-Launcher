#include "JvmArgsUtil.h"
#include <QRegularExpression>

namespace JvmArgsUtil {

static bool isMemFlag(const QString& a) {
    static const QRegularExpression re(R"(^-Xms?\b)", QRegularExpression::CaseInsensitiveOption);
    return re.match(a).hasMatch();
}

void enforceMemoryFlags(QStringList& args, int xmxMiB)
{
    // 1) вычистим все старые -Xmx/-Xms
    QStringList clean;
    clean.reserve(args.size());
    for (const auto& a : args)
        if (!isMemFlag(a)) clean << a;

    // 2) добавим наши в самом конце (последнее слово побеждает)
    const int xms = qMax(512, xmxMiB/4);
    clean << QString("-Xmx%1m").arg(xmxMiB);
    clean << QString("-Xms%1m").arg(xms);

    args.swap(clean);
}

} // namespace JvmArgsUtil
