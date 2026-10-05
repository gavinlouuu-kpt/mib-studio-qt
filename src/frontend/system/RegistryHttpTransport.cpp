#include "frontend/system/RegistryHttpTransport.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace mib::frontend {

backend::profiles::RegistryHttpTransport makeQtRegistryHttpTransport() {
    return [](const backend::profiles::RegistryHttpRequest& in) {
        backend::profiles::RegistryHttpResponse out; // status 0 = transport failure
        const QUrl url(QString::fromStdString(in.url));
        if (!url.isValid() || url.scheme() != QStringLiteral("https")) return out;

        // The loop comes first: on the registry worker (a plain std::thread
        // Qt adopts) it creates the event dispatcher the manager and timers use.
        QEventLoop loop;
        QNetworkAccessManager manager;
        QNetworkRequest request(url);
        // A 3xx comes back as its own status, which the registry rejects.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(static_cast<int>(in.timeoutMs));
        for (const auto& [name, value] : in.headers)
            request.setRawHeader(QByteArray::fromStdString(name), QByteArray::fromStdString(value));

        QNetworkReply* reply = manager.post(request, QByteArray::fromStdString(in.body));
        QByteArray body;
        bool oversized = false;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            body += reply->readAll();
            if (static_cast<size_t>(body.size()) > in.maxResponseBytes) {
                oversized = true;
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
        deadline.start(static_cast<int>(in.timeoutMs));
        QTimer cancelPoll;
        QObject::connect(&cancelPoll, &QTimer::timeout, reply, [&] {
            if (in.cancelled && in.cancelled()) reply->abort();
        });
        cancelPoll.start(50);
        if (!reply->isFinished()) loop.exec();
        cancelPoll.stop();
        deadline.stop();

        if (!oversized) body += reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
        reply->deleteLater();
        if (oversized) {
            // Keep cap+1 bytes: the registry reports "response exceeds limit".
            out.status = status > 0 ? static_cast<unsigned>(status) : 200u;
            out.body.assign(body.constData(), in.maxResponseBytes + 1);
            return out;
        }
        if (aborted || status <= 0) return out; // timeout, cancel, DNS/TLS/connect failure
        out.status = static_cast<unsigned>(status);
        out.body.assign(body.constData(), static_cast<size_t>(body.size()));
        return out;
    };
}

} // namespace mib::frontend
