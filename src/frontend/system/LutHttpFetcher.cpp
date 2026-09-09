#include "frontend/system/LutHttpFetcher.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <cstdint>

namespace mib::frontend {

backend::HttpGetFn makeQtLutHttpGet()
{
    return [](const std::string& url, int timeoutMs) -> backend::HttpGetResult {
        backend::HttpGetResult out;

        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(timeoutMs);

        QNetworkReply* reply = manager.get(request);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(timeoutMs);
        QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start();
        loop.exec();
        timeout.stop();

        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const std::string errorString = reply->errorString().toStdString();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        out.statusCode = httpStatus;
        out.body.assign(reinterpret_cast<const std::uint8_t*>(body.constData()),
                        reinterpret_cast<const std::uint8_t*>(body.constData()) + body.size());
        if (error != QNetworkReply::NoError) {
            out.ok = false;
            out.error = errorString;
        } else {
            out.ok = true;
        }
        return out;
    };
}

} // namespace mib::frontend
