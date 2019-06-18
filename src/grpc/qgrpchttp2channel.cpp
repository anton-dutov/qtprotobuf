/*
 * MIT License
 *
 * Copyright (c) 2019 Alexey Edelev <semlanik@gmail.com>, Viktor Kopp <vifactor@gmail.com>
 *
 * This file is part of qtprotobuf project https://git.semlanik.org/semlanik/qtprotobuf
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies
 * or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qgrpchttp2channel.h"

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>

#include "asyncreply.h"
#include "abstractclient.h"
#include "abstractcredentials.h"

#include <unordered_map>

#include "qtprotobuflogging.h"

using namespace qtprotobuf;

namespace  {

/*!
 * This QNetworkReply::NetworkError -> AbstractChannel::StatusCode mapping should be kept in sync with original
 * <a href="https://github.com/grpc/grpc/blob/master/doc/statuscodes.md">gRPC status codes</a>
 */
const static std::unordered_map<QNetworkReply::NetworkError, QAbstractGrpcChannel::StatusCode> StatusCodeMap = {
                                                                { QNetworkReply::ConnectionRefusedError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::RemoteHostClosedError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::HostNotFoundError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::TimeoutError, QAbstractGrpcChannel::DeadlineExceeded },
                                                                { QNetworkReply::OperationCanceledError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::SslHandshakeFailedError, QAbstractGrpcChannel::PermissionDenied },
                                                                { QNetworkReply::TemporaryNetworkFailureError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::NetworkSessionFailedError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::BackgroundRequestNotAllowedError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::TooManyRedirectsError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::InsecureRedirectError, QAbstractGrpcChannel::PermissionDenied },
                                                                { QNetworkReply::UnknownNetworkError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::ProxyConnectionRefusedError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::ProxyConnectionClosedError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::ProxyNotFoundError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::ProxyTimeoutError, QAbstractGrpcChannel::DeadlineExceeded },
                                                                { QNetworkReply::ProxyAuthenticationRequiredError, QAbstractGrpcChannel::Unauthenticated },
                                                                { QNetworkReply::UnknownProxyError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::ContentAccessDenied, QAbstractGrpcChannel::PermissionDenied },
                                                                { QNetworkReply::ContentOperationNotPermittedError, QAbstractGrpcChannel::PermissionDenied },
                                                                { QNetworkReply::ContentNotFoundError, QAbstractGrpcChannel::NotFound },
                                                                { QNetworkReply::AuthenticationRequiredError, QAbstractGrpcChannel::PermissionDenied },
                                                                { QNetworkReply::ContentReSendError, QAbstractGrpcChannel::DataLoss },
                                                                { QNetworkReply::ContentConflictError, QAbstractGrpcChannel::InvalidArgument },
                                                                { QNetworkReply::ContentGoneError, QAbstractGrpcChannel::DataLoss },
                                                                { QNetworkReply::UnknownContentError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::ProtocolUnknownError, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::ProtocolInvalidOperationError, QAbstractGrpcChannel::Unimplemented },
                                                                { QNetworkReply::ProtocolFailure, QAbstractGrpcChannel::Unknown },
                                                                { QNetworkReply::InternalServerError, QAbstractGrpcChannel::Internal },
                                                                { QNetworkReply::OperationNotImplementedError, QAbstractGrpcChannel::Unimplemented },
                                                                { QNetworkReply::ServiceUnavailableError, QAbstractGrpcChannel::Unavailable },
                                                                { QNetworkReply::UnknownServerError, QAbstractGrpcChannel::Unknown }};

const char *GrpcAcceptEncodingHeader = "grpc-accept-encoding";
const char *AcceptEncodingHeader = "accept-encoding";
const char *TEHeader = "te";
const char *GrpcStatusHeader = "grpc-status";
}

namespace qtprotobuf {

struct Http2ChannelPrivate {
    struct ExpectedData {
        int expectedSize;
        QByteArray container;
    };

    QUrl url;
    QNetworkAccessManager nm;
    AbstractCredentials credentials;
    QSslConfiguration sslConfig;
    std::unordered_map<QNetworkReply *, ExpectedData> activeStreamReplies;

    QNetworkReply *post(const QString &method, const QString &service, const QByteArray &args, bool stream = false) {
        QUrl callUrl = url;
        callUrl.setPath("/" + service + "/" + method);

        qProtoDebug() << "Service call url: " << callUrl;
        QNetworkRequest request(callUrl);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/grpc");
        request.setRawHeader(GrpcAcceptEncodingHeader, "identity,deflate,gzip");
        request.setRawHeader(AcceptEncodingHeader, "identity,gzip");
        request.setRawHeader(TEHeader, "trailers");
        request.setSslConfiguration(sslConfig);
        AbstractCredentials::CredentialMap callCredentials = credentials.callCredentials();
        for (auto i = callCredentials.begin(); i != callCredentials.end(); ++i) {
            request.setRawHeader(i.key().data(), i.value().toString().toUtf8());
        }

        request.setAttribute(QNetworkRequest::Http2DirectAttribute, true);

        QByteArray msg(5, '\0');
        *(int *)(msg.data() + 1) = qToBigEndian(args.size());
        msg += args;
        qProtoDebug() << "SEND: " << msg.size();

        QNetworkReply *networkReply = nm.post(request, msg);

        QObject::connect(networkReply, &QNetworkReply::sslErrors, [networkReply](const QList<QSslError> &errors) {
           qProtoCritical() << errors;
           // TODO: filter out noncritical SSL handshake errors
           // FIXME: error due to ssl failure is not transferred to the client: last error will be Operation canceled
           Http2ChannelPrivate::abortNetworkReply(networkReply);
        });

        if (!stream) {
            //TODO: Add configurable timeout logic
            QTimer::singleShot(6000, networkReply, [networkReply]() {
                Http2ChannelPrivate::abortNetworkReply(networkReply);
            });
        }
        return networkReply;
    }

    static void abortNetworkReply(QNetworkReply *networkReply) {
        if (networkReply->isRunning()) {
            networkReply->abort();
        }
    }

    static QByteArray processReply(QNetworkReply *networkReply, QAbstractGrpcChannel::StatusCode &statusCode) {
        //Check if no network error occured
        if (networkReply->error() != QNetworkReply::NoError) {
            statusCode = StatusCodeMap.at(networkReply->error());
            return {};
        }

        //Check if server answer with error
        statusCode = static_cast<QAbstractGrpcChannel::StatusCode>(networkReply->rawHeader(GrpcStatusHeader).toInt());
        if (statusCode != QAbstractGrpcChannel::StatusCode::Ok) {
            return {};
        }

        //Message size doesn't matter for now
        return networkReply->readAll().mid(5);
    }

    Http2ChannelPrivate(const QUrl &_url, const AbstractCredentials &_credentials)
        : url(_url)
        , credentials(_credentials)
    {
        if (url.scheme() == "https") {
            if (!credentials.channelCredentials().contains(QLatin1String("sslConfig"))) {
                throw std::invalid_argument("Https connection requested but not ssl configuration provided.");
            }
            sslConfig = credentials.channelCredentials().value(QLatin1String("sslConfig")).value<QSslConfiguration>();
        } else if (url.scheme().isEmpty()) {
            url.setScheme("http");
        }
    }
};

}

QGrpcHttp2Channel::QGrpcHttp2Channel(const QUrl &url, const AbstractCredentials &credentials) : QAbstractGrpcChannel()
  , d(new Http2ChannelPrivate(url, credentials))
{
}

QGrpcHttp2Channel::~QGrpcHttp2Channel()
{
    delete d;
}

QAbstractGrpcChannel::StatusCode QGrpcHttp2Channel::call(const QString &method, const QString &service, const QByteArray &args, QByteArray &ret)
{
    QEventLoop loop;

    QNetworkReply *networkReply = d->post(method, service, args);
    QObject::connect(networkReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    //If reply was finished in same stack it doesn't make sense to start event loop
    if (!networkReply->isFinished()) {
        loop.exec();
    }

    StatusCode grpcStatus = StatusCode::Unknown;
    ret = d->processReply(networkReply, grpcStatus);

    qProtoDebug() << __func__ << "RECV: " << ret.toHex() << "grpcStatus" << grpcStatus;
    return grpcStatus;
}

void QGrpcHttp2Channel::call(const QString &method, const QString &service, const QByteArray &args, qtprotobuf::AsyncReply *reply)
{
    QNetworkReply *networkReply = d->post(method, service, args);

    auto connection = QObject::connect(networkReply, &QNetworkReply::finished, reply, [reply, networkReply]() {
        StatusCode grpcStatus = StatusCode::Unknown;
        QByteArray data = Http2ChannelPrivate::processReply(networkReply, grpcStatus);

        qProtoDebug() << "RECV: " << data;
        if (StatusCode::Ok == grpcStatus ) {
            reply->setData(data);
            reply->finished();
        } else {
            reply->setData({});
            reply->error(grpcStatus);
        }
    });

    QObject::connect(reply, &AsyncReply::error, networkReply, [networkReply, connection](QAbstractGrpcChannel::StatusCode code) {
        QObject::disconnect(connection);
        Http2ChannelPrivate::abortNetworkReply(networkReply);
    });
}

void QGrpcHttp2Channel::subscribe(const QString &method, const QString &service, const QByteArray &args, AbstractClient *client, const std::function<void (const QByteArray &)> &handler)
{
    QNetworkReply *networkReply = d->post(method, service, args, true);

    auto connection = QObject::connect(networkReply, &QNetworkReply::readyRead, client, [networkReply, handler, this]() {
        auto replyIt = d->activeStreamReplies.find(networkReply);

        QByteArray data = networkReply->readAll();
        qProtoDebug() << "RECV" << data.size();

        if (replyIt == d->activeStreamReplies.end()) {
            qProtoDebug() << data.toHex();
            int expectedDataSize = qFromBigEndian(*(int *)(data.data() + 1)) + 5;
            qProtoDebug() << "First chunk received: " << data.size() << " expectedDataSize: " << expectedDataSize;

            if (expectedDataSize == 0) {
                handler(QByteArray());
                return;
            }

            Http2ChannelPrivate::ExpectedData dataContainer{expectedDataSize, QByteArray{}};
            d->activeStreamReplies.insert({networkReply, dataContainer});
            replyIt = d->activeStreamReplies.find(networkReply);
        }

        Http2ChannelPrivate::ExpectedData &dataContainer = replyIt->second;
        dataContainer.container.append(data);

        qProtoDebug() << "Proceed chunk: " << data.size() << " dataContainer: " << dataContainer.container.size() << " capacity: " << dataContainer.expectedSize;
        if (dataContainer.container.size() == dataContainer.expectedSize) {
            qProtoDebug() << "Full data received: " << data.size() << " dataContainer: " << dataContainer.container.size() << " capacity: " << dataContainer.expectedSize;
            handler(dataContainer.container.mid(5));
            d->activeStreamReplies.erase(replyIt);
        }
    });

    QObject::connect(client, &AbstractClient::destroyed, networkReply, [networkReply, connection, this](){
        d->activeStreamReplies.erase(networkReply);
        QObject::disconnect(connection);
        Http2ChannelPrivate::abortNetworkReply(networkReply);
    });

    //TODO: seems this connection might be invalid in case if this destroyed.
    //Think about correct handling of this situation
    QObject::connect(networkReply, &QNetworkReply::finished, [networkReply, this]() {
        d->activeStreamReplies.erase(networkReply);
        //TODO: implement error handling and subscription recovery here
        Http2ChannelPrivate::abortNetworkReply(networkReply);
    });
}

void QGrpcHttp2Channel::abort(AsyncReply *reply)
{
    assert(reply != nullptr);
    reply->setData({});
    reply->error(StatusCode::Aborted);
}