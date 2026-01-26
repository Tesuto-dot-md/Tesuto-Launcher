#include "Net.h"
#include <QEventLoop>
#include <QTimer>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QJsonDocument>
#include <QSettings>

Net::Net(QObject* p) : QObject(p) {
    QSettings s("Tesuto", "TesutoLauncher");
    const bool useSystemProxy = s.value("network/useSystemProxy", true).toBool();
    const QString noProxy = s.value("network/noProxy").toString().trimmed();
    if (!noProxy.isEmpty()) qputenv("no_proxy", noProxy.toUtf8());

    QNetworkProxyFactory::setUseSystemConfiguration(useSystemProxy);
    if (!useSystemProxy) {
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    } else {
        QNetworkProxy::setApplicationProxy(QNetworkProxy()); // reset
    }
}

void Net::setConcurrency(int n) { concurrency_ = qBound(1, n, 4); }

static void applyHeaders(QNetworkRequest& req, const Net::HeaderList& h) {
    for (const auto& kv : h) req.setRawHeader(kv.first, kv.second);
}

QByteArray Net::getBytes(const QUrl& url, int timeoutMs, const HeaderList& headers) {
    QNetworkRequest req(url);
    applyHeaders(req, headers);
    QNetworkReply* rep = nam_.get(req);

    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]{ rep->abort(); });
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (rep->error() != QNetworkReply::NoError) {
        rep->deleteLater();
        throw std::runtime_error(("GET failed: " + url.toString()).toStdString());
    }

    const QByteArray out = rep->readAll();
    rep->deleteLater();
    return out;
}

QJsonObject Net::getJson(const QUrl& url, int timeoutMs, const HeaderList& headers) {
    const auto data = getBytes(url, timeoutMs, headers);
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) throw std::runtime_error("Invalid JSON (not an object)");
    return doc.object();
}

QJsonObject Net::postJson(const QUrl& url, const QJsonObject& body, int timeoutMs,
                          const HeaderList& headers)
{
    QNetworkRequest req(url);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);

    auto rep = nam_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]{ rep->abort(); });
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    const int httpStatus = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString reason = rep->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    const QByteArray bytes = rep->readAll();

    if (rep->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
        rep->deleteLater();
        throw std::runtime_error(
            QString("POST failed: %1 (HTTP %2 %3) body=%4")
                .arg(url.toString())
                .arg(httpStatus)
                .arg(reason)
                .arg(QString::fromUtf8(bytes))
                .toStdString());
    }

    rep->deleteLater();
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
        throw std::runtime_error("Invalid JSON (not an object)");
    return doc.object();
}

QJsonObject Net::postForm(const QUrl& url, const QUrlQuery& form, int timeoutMs,
                          const HeaderList& headers)
{
    QNetworkRequest req(url);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);

    const QByteArray body = form.query(QUrl::FullyEncoded).toUtf8();
    auto rep = nam_.post(req, body);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]{ rep->abort(); });
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    const int httpStatus = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString reason = rep->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    const QByteArray bytes = rep->readAll();

    if (rep->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
        rep->deleteLater();
        throw std::runtime_error(
            QString("POST failed: %1 (HTTP %2 %3) body=%4")
                .arg(url.toString())
                .arg(httpStatus)
                .arg(reason)
                .arg(QString::fromUtf8(bytes))
                .toStdString());
    }

    rep->deleteLater();
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
        throw std::runtime_error("Invalid JSON (not an object)");
    return doc.object();
}

Net::ResponseAny Net::postFormAny(const QUrl& url, const QUrlQuery& form, int timeoutMs,
                                  const HeaderList& headers) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    applyHeaders(req, headers);

    QNetworkReply* rep = nam_.post(req, form.query(QUrl::FullyEncoded).toUtf8());

    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]{ rep->abort(); });
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    ResponseAny out;
    out.status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body   = rep->readAll();

    // Пытаемся разобрать как объект
    const auto doc = QJsonDocument::fromJson(out.body);
    if (doc.isObject()) out.json = doc.object();
    rep->deleteLater();
    return out; // НИЧЕГО не бросаем — вызывающий сам решит
}
