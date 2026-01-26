#pragma once
#include <QtCore>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

class Net : public QObject {
    Q_OBJECT
public:
    using Header = std::pair<QByteArray, QByteArray>;
    using HeaderList = QList<Header>;

    explicit Net(QObject* parent = nullptr);

    void setConcurrency(int n);

    QJsonObject getJson(const QUrl& url, int timeoutMs = 20000,
                        const HeaderList& headers = HeaderList());

    QJsonObject postJson(const QUrl& url, const QJsonObject& body, int timeoutMs = 20000,
                         const HeaderList& headers = HeaderList());

    QJsonObject postForm(const QUrl& url, const QUrlQuery& form, int timeoutMs = 20000,
                         const HeaderList& headers = HeaderList());

    // Мягкий вариант: не бросаем по HTTP 4xx/5xx, возвращаем статус+тело
    struct ResponseAny {
        int status = -1;
        QByteArray body;
        QJsonObject json; // если парсится в объект
    };
    ResponseAny postFormAny(const QUrl& url, const QUrlQuery& form, int timeoutMs = 20000,
                            const HeaderList& headers = HeaderList());

    QByteArray getBytes(const QUrl& url, int timeoutMs = 20000,
                        const HeaderList& headers = HeaderList());

private:
    QNetworkAccessManager nam_;
    int concurrency_ = 2;
};
