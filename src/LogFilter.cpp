#include "LogFilter.h"
#include <QRegularExpression>
#include <QMutex>
#include <QAtomicInt>
#include <QDir>
#include <QtGlobal>

namespace {
    QtMessageHandler g_prevHandler = nullptr;
    QAtomicInt g_enabled {0};
    QString g_homeDir;
    QString g_gameDir;

    QString maskLine(QString s) {
        if (!g_enabled.loadRelaxed()) return s;

        // 1) Маска токена в CLI-аргах и JSON-логах
        s.replace(QRegularExpression(QStringLiteral(R"(--accessToken\s+\S+)")),
                  QStringLiteral("--accessToken <hidden>"));
        s.replace(QRegularExpression(QStringLiteral(R"("accessToken"\s*:\s*"[^\"]*")")),
                  QStringLiteral(R"("accessToken":"<hidden>")"));

        // 2) Маска UUID (32 или 36 шестнадцатеричных символов)
        s.replace(QRegularExpression(QStringLiteral(R"(\b[0-9a-fA-F]{8}\-?[0-9a-fA-F]{4}\-?[0-9a-fA-F]{4}\-?[0-9a-fA-F]{4}\-?[0-9a-fA-F]{12}\b)")),
                  QStringLiteral("<uuid>"));

        // 3) Маска XUID (длинные числовые идентификаторы)
        s.replace(QRegularExpression(QStringLiteral(R"(\b\d{15,20}\b)")), QStringLiteral("<xuid>"));

        // 4) Маска домашнего каталога и каталога инстанса
        if (!g_gameDir.isEmpty()) {
            const QString quoted = QRegularExpression::escape(g_gameDir);
            s.replace(QRegularExpression(quoted), QStringLiteral("<gameDir>"));
        }
        if (!g_homeDir.isEmpty()) {
            const QString quotedHome = QRegularExpression::escape(g_homeDir);
            s.replace(QRegularExpression(quotedHome), QStringLiteral("~"));
        }

        return s;
    }

    void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const QString masked = maskLine(msg);
        if (g_prevHandler) {
            g_prevHandler(type, ctx, masked);
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
            fprintf(stderr, "%s\n", masked.toLocal8Bit().constData());
            fflush(stderr);
#else
            qDebug().noquote() << masked;
#endif
        }
    }
}

namespace LogFilter {

void install(bool enabled, const QString& homeDir) {
    g_homeDir = homeDir;
    g_prevHandler = qInstallMessageHandler(handler);
    g_enabled.storeRelaxed(enabled ? 1 : 0);
}

void setGameDir(const QString& dir) {
    g_gameDir = dir;
}

void setEnabled(bool enabled) {
    g_enabled.storeRelaxed(enabled ? 1 : 0);
}

}
