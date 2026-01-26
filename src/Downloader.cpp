#include "Downloader.h"

Downloader::Downloader(Net& net) : net_(net) {}

QByteArray Downloader::getWithMirrors(const QList<QUrl>& bases, const QString& rel) {
    const int to = qEnvironmentVariableIntValue("TESUTO_NET_TIMEOUT_MS") > 0
                   ? qgetenv("TESUTO_NET_TIMEOUT_MS").toInt()
                   : 12000;
    Net::HeaderList h = { {"Accept-Encoding", "identity"} };

    for (const auto& base : bases) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                QUrl u = base;
                if (!rel.isEmpty()) {
                    QString b = base.toString();
                    if (b.endsWith('/')) b.chop(1);
                    u = QUrl(b + "/" + rel);
                }
                return net_.getBytes(u, to, h);
            } catch (const std::exception&) {
                // пробуем ещё/следующий
            } catch (...) {
                // на всякий случай: не даём неизвестным исключениям пробить установку без текста
            }
        }
    }
    throw std::runtime_error("All mirrors failed");
}
