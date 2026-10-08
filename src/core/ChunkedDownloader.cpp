#include "core/ChunkedDownloader.h"
#include "utils/Logger.h"
#include "utils/FileNameSanitizer.h"
#include "db/DatabaseManager.h"
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QSaveFile>
#include <algorithm>

ChunkedDownloader::ChunkedDownloader(QObject* parent)
    : QObject(parent)
    , totalChunks(16)
    , downloadId(0)
    , downloading(false)
    , paused(false)
    , cancelled(false)
    , supportsRange(false)
    , totalBytes(0)
    , downloadedBytes(0)
    , lastSpeedBytes(0)
    , speed(0)
    , lastActivityMs(0)
    , nam(new QNetworkAccessManager(this))
    , headReply(nullptr)
    , limitBytesPerSec(0)
    , throttleBudget(0)
    , throttleRemaining(0)
    , drainTimer(new QTimer(this))
    , throttleActive(false)
{
    speedTimer = new QTimer(this);
    connect(speedTimer, &QTimer::timeout, this, &ChunkedDownloader::onSpeedTimer);

    hangTimer = new QTimer(this);
    hangTimer->setInterval(15000);
    connect(hangTimer, &QTimer::timeout, this, &ChunkedDownloader::onHangTimer);

    drainTimer->setInterval(200);
    connect(drainTimer, &QTimer::timeout, this, &ChunkedDownloader::onDrainTimer);
    throttleTimer.start();
}

ChunkedDownloader::~ChunkedDownloader() {
    cancel();
}

QString ChunkedDownloader::extractFilenameFromContentDisposition(const QByteArray& cdHeader) {
    if (cdHeader.isEmpty()) return {};
    QString cdStr = QString::fromUtf8(cdHeader);

    QRegularExpression re1("filename\\*=([^;]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m1 = re1.match(cdStr);
    if (m1.hasMatch()) {
        QString val = m1.captured(1).trimmed();
        QRegularExpression re1val("UTF-8''(.+)", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch m1v = re1val.match(val);
        if (m1v.hasMatch()) {
            QString decoded = QUrl::fromPercentEncoding(m1v.captured(1).toUtf8());
            if (!decoded.isEmpty() && !decoded.contains('/') && !decoded.contains('\\')) return sanitizeFileName(decoded);
        }
    }

    QRegularExpression re2("filename=\"([^\"]+)\"", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m2 = re2.match(cdStr);
    if (m2.hasMatch()) {
        QString name = m2.captured(1).trimmed();
        if (!name.isEmpty() && !name.contains('/') && !name.contains('\\')) return sanitizeFileName(name);
    }

    QRegularExpression re3("filename=([^;\\s]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m3 = re3.match(cdStr);
    if (m3.hasMatch()) {
        QString name = m3.captured(1).trimmed();
        if (!name.isEmpty() && !name.contains('/') && !name.contains('\\')) return sanitizeFileName(name);
    }

    return {};
}

QString ChunkedDownloader::extractUrlFromHtml(const QByteArray& html) {
    QString content = QString::fromUtf8(html);

    QRegularExpression re("id=\"uc-download-link\"[^>]*>.*?<a\\s+href=\"([^\"]+)\"", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = re.match(content);
    if (m.hasMatch()) {
        QString href = m.captured(1).replace("&amp;", "&");
        Logger::instance().info("Found download link in HTML: " + href);
        return href;
    }

    QRegularExpression re2("<form[^>]*action=\"([^\"]*export=download[^\"]*)\"[^>]*>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m2 = re2.match(content);
    if (m2.hasMatch()) {
        QString action = m2.captured(1).replace("&amp;", "&");
        Logger::instance().info("Found download form action in HTML: " + action);
        return action;
    }

    QRegularExpression re3("href=\"([^\"]*\\/uc\\?[^\"]*export=download[^\"]*)\"", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m3 = re3.match(content);
    if (m3.hasMatch()) {
        QString href = m3.captured(1).replace("&amp;", "&");
        Logger::instance().info("Found uc download link in HTML: " + href);
        return href;
    }

    QRegularExpression re4("href=\"([^\"]*confirm=[^\"]*id=[^\"]*|[^\"]*id=[^\"]*confirm=[^\"]*)\"", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m4 = re4.match(content);
    if (m4.hasMatch()) {
        QString href = m4.captured(1).replace("&amp;", "&");
        Logger::instance().info("Found confirm+id link in HTML: " + href);
        return href;
    }

    return {};
}

bool ChunkedDownloader::isHtmlResponse(QNetworkReply* reply) {
    if (!reply) return false;
    QByteArray contentType = reply->rawHeader("Content-Type");
    return contentType.toLower().contains("text/html");
}

void ChunkedDownloader::startDownload(const QString& url, const QString& filePath, int chunks, int id) {
    downloadUrl = url;
    saveFilePath = filePath;
    totalChunks = qBound(1, chunks, 128);
    downloadId = id;
    downloading = true;
    paused = false;
    cancelled = false;
    downloadedBytes = 0;
    totalBytes = 0;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    // Fresh attempt: validators and any in-flight violation state are re-learned
    // from the new HEAD response. rangeViolationRestarted/forceNoRange are kept
    // on purpose - they record what we already learned about this server so a
    // range-ignoring endpoint cannot put us into a restart loop.
    respEtag.clear();
    respLastModified.clear();
    attachingIfRange = false;
    pendingRangeViolation = false;

    resetThrottleState();

    Logger::instance().info("Starting chunked download: " + url + " -> " + filePath + " (" + QString::number(chunks) + " chunks)");

    QDir().mkpath(QFileInfo(filePath).absolutePath());

    QNetworkRequest request{QUrl{url}};
    request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    headReply = nam->head(request);
    connect(headReply, &QNetworkReply::finished, this, &ChunkedDownloader::onHeadFinished);
}

void ChunkedDownloader::onHeadFinished() {
    if (!headReply) return;

    if (headReply->error() != QNetworkReply::NoError) {
        Logger::instance().info("HEAD request failed, falling back to direct GET: " + headReply->errorString());
        headReply->deleteLater();
        headReply = nullptr;

        QNetworkRequest request{QUrl{downloadUrl}};
        request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply* fallbackReply = nam->get(request);
        connect(fallbackReply, &QNetworkReply::finished, this, [this, fallbackReply]() {
            checkFallbackReply(fallbackReply);
        });
        return;
    }

    if (isHtmlResponse(headReply)) {
        Logger::instance().info("HEAD returned HTML content, attempting to extract real download URL");
        QByteArray htmlData = headReply->readAll();
        headReply->deleteLater();
        headReply = nullptr;

        QString realUrl = extractUrlFromHtml(htmlData);
        if (!realUrl.isEmpty()) {
            QUrl baseUrl(downloadUrl);
            QUrl resolved = baseUrl.resolved(QUrl(realUrl));
            QString newUrl = resolved.toString();
            Logger::instance().info("Restarting download with extracted URL: " + newUrl);
            downloading = false;
            startDownload(newUrl, saveFilePath, totalChunks, downloadId);
            return;
        }

        Logger::instance().warning("Could not extract real download URL from HTML response");
        emit downloadFailed(downloadId, "Server returned an HTML page instead of the file. The URL may require browser authentication or a CAPTCHA.");
        downloading = false;
        return;
    }

    totalBytes = headReply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    respEtag = QString::fromLatin1(headReply->rawHeader("ETag")).trimmed();
    respLastModified = QString::fromLatin1(headReply->rawHeader("Last-Modified")).trimmed();

    QString realName = extractFilenameFromContentDisposition(headReply->rawHeader("Content-Disposition"));
    if (!realName.isEmpty()) {
        QString dir = QFileInfo(saveFilePath).absolutePath();
        saveFilePath = dir + "/" + realName;
        Logger::instance().info("Content-Disposition filename: " + realName);
        emit filePathChanged(downloadId, saveFilePath);
    }

    int statusCode = headReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    supportsRange = (statusCode == 206) || headReply->rawHeader("Accept-Ranges").contains("bytes");
    if (forceNoRange) {
        // A previous attempt proved this server answers ranged requests with
        // 200 OK; never trust Accept-Ranges from it again.
        supportsRange = false;
    }

    headReply->deleteLater();
    headReply = nullptr;

    if (totalBytes <= 0 && supportsRange) {
        QNetworkRequest fullRequest{QUrl{downloadUrl}};
        fullRequest.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
        fullRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply* reply = nam->get(fullRequest);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            totalBytes = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            supportsRange = false;
            reply->deleteLater();

            totalChunks = 1;
            setupChunks(totalBytes);
        });
        return;
    }

    if (!supportsRange || totalBytes <= 0) {
        totalChunks = 1;
    }

    setupChunks(totalBytes);
    speedTimer->start(1000);
}

void ChunkedDownloader::checkFallbackReply(QNetworkReply* reply) {
    if (!reply) return;

    if (reply->error() != QNetworkReply::NoError) {
        Logger::instance().error("Fallback GET failed: " + reply->errorString());
        emit downloadFailed(downloadId, reply->errorString());
        reply->deleteLater();
        downloading = false;
        return;
    }

    if (isHtmlResponse(reply)) {
        Logger::instance().info("Fallback GET returned HTML content, attempting to extract real download URL");
        QByteArray htmlData = reply->readAll();
        reply->deleteLater();

        QString realUrl = extractUrlFromHtml(htmlData);
        if (!realUrl.isEmpty()) {
            QUrl baseUrl(downloadUrl);
            QUrl resolved = baseUrl.resolved(QUrl(realUrl));
            QString newUrl = resolved.toString();
            Logger::instance().info("Restarting download with extracted URL: " + newUrl);
            downloading = false;
            startDownload(newUrl, saveFilePath, totalChunks, downloadId);
            return;
        }

        Logger::instance().warning("Could not extract real download URL from HTML fallback response");
        emit downloadFailed(downloadId, "Server returned an HTML page instead of the file. The URL may require browser authentication or a CAPTCHA.");
        downloading = false;
        return;
    }

    totalBytes = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    respEtag = QString::fromLatin1(reply->rawHeader("ETag")).trimmed();
    respLastModified = QString::fromLatin1(reply->rawHeader("Last-Modified")).trimmed();

    QString realName = extractFilenameFromContentDisposition(reply->rawHeader("Content-Disposition"));
    if (!realName.isEmpty()) {
        QString dir = QFileInfo(saveFilePath).absolutePath();
        saveFilePath = dir + "/" + realName;
        Logger::instance().info("Content-Disposition filename (fallback): " + realName);
        emit filePathChanged(downloadId, saveFilePath);
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    supportsRange = false;

    if (!forceNoRange && (statusCode == 206 || reply->rawHeader("Accept-Ranges").contains("bytes"))) {
        qint64 contentLength = totalBytes;
        reply->deleteLater();

        if (contentLength > 0) {
            supportsRange = true;
            totalBytes = contentLength;
        }
        setupChunks(totalBytes);
        speedTimer->start(1000);
        return;
    }

    QByteArray data = reply->readAll();
    QVariant contentLengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
    reply->deleteLater();

    // The server promised a length: refuse to commit a body that does not match
    // it exactly (identity transfer, so no chunked-decoding ambiguity).
    if (contentLengthHeader.isValid()) {
        qint64 promised = contentLengthHeader.toLongLong();
        if (promised >= 0 && promised != data.size()) {
            emit downloadFailed(downloadId, QString("Incomplete response: received %1 of %2 bytes.")
                                                 .arg(data.size()).arg(promised));
            downloading = false;
            return;
        }
    }

    // Stage through QSaveFile so the destination only ever appears complete:
    // a failure/crash mid-write leaves the previous file (if any) untouched.
    QSaveFile outFile(saveFilePath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        emit downloadFailed(downloadId, "Cannot open file for writing: " + saveFilePath);
        downloading = false;
        return;
    }
    if (outFile.write(data) != data.size()) {
        emit downloadFailed(downloadId, "Cannot write file: " + saveFilePath);
        downloading = false;
        return;   // QSaveFile discards the staged temp file on destruction
    }
    if (!outFile.commit()) {
        emit downloadFailed(downloadId, "Cannot finalize file: " + saveFilePath);
        downloading = false;
        return;
    }

    downloadedBytes = data.size();
    totalBytes = data.size();
    emit downloadProgress(downloadId, downloadedBytes, totalBytes);
    emit downloadFinished(downloadId);
    downloading = false;
    Logger::instance().info("Fallback direct download completed: " + saveFilePath + " (" + QString::number(downloadedBytes) + " bytes)");
}

void ChunkedDownloader::setupChunks(qint64 totalSize) {
    chunks.clear();

    if (totalSize <= 0 || !supportsRange) {
        ChunkState chunk;
        chunk.index = 0;
        chunk.startByte = 0;
        chunk.endByte = -1;
        chunk.downloaded = 0;
        chunks.append(chunk);

        if (!startChunkRequest(chunks.last())) {
            downloading = false;
            cleanupChunks();
            return;
        }
        hangTimer->start();
        return;
    }

    qint64 chunkSize = totalSize / totalChunks;
    if (chunkSize < 1024) {
        totalChunks = 1;
        chunkSize = totalSize;
    }

    for (int i = 0; i < totalChunks; i++) {
        ChunkState chunk;
        chunk.index = i;
        chunk.startByte = i * chunkSize;
        chunk.endByte = (i == totalChunks - 1) ? (totalSize - 1) : ((i + 1) * chunkSize - 1);
        chunk.downloaded = 0;
        chunks.append(chunk);

        if (!startChunkRequest(chunks.last())) {
            downloading = false;
            cleanupChunks();
            return;
        }
    }

    hangTimer->start();
}

QNetworkRequest ChunkedDownloader::buildChunkRequest(qint64 fromByte, qint64 toByte) const {
    QNetworkRequest request{QUrl{downloadUrl}};
    request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (toByte >= 0) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(fromByte) + "-" + QByteArray::number(toByte));
        // On resume, If-Range makes the server answer 200 OK (full body) when
        // the remote file changed since the partial data was written; the
        // response validation then restarts the download instead of appending
        // bytes of a different file to the stale chunks.
        if (attachingIfRange) {
            if (!respEtag.isEmpty()) {
                request.setRawHeader("If-Range", respEtag.toUtf8());
            } else if (!respLastModified.isEmpty()) {
                request.setRawHeader("If-Range", respLastModified.toUtf8());
            }
        }
    }
    return request;
}

bool ChunkedDownloader::startChunkRequest(ChunkState& chunk) {
    // Already fully on disk: nothing to request (the file handle stays closed,
    // so the all-done check counts it as finished).
    qint64 expected = (chunk.endByte >= 0) ? (chunk.endByte - chunk.startByte + 1) : -1;
    if (expected >= 0 && chunk.downloaded >= expected) return true;

    bool append = chunk.downloaded > 0;
    QFile* file = new QFile(chunkFilePath(chunk.index));
    QIODevice::OpenMode mode = QIODevice::WriteOnly | (append ? QIODevice::Append : QIODevice::Truncate);
    if (!file->open(mode)) {
        Logger::instance().error("Cannot open chunk file: " + chunkFilePath(chunk.index));
        emit downloadFailed(downloadId, "Cannot open chunk file: " + chunkFilePath(chunk.index));
        delete file;
        downloading = false;
        return false;
    }
    chunk.file = file;

    chunk.requestedRange = (chunk.endByte >= 0);
    chunk.requestStart = chunk.requestedRange ? (chunk.startByte + chunk.downloaded) : -1;
    // Unranged requests have no response contract to verify.
    chunk.responseValidated = !chunk.requestedRange;

    QNetworkRequest request = chunk.requestedRange
        ? buildChunkRequest(chunk.requestStart, chunk.endByte)
        : buildChunkRequest(0, -1);

    QNetworkReply* reply = nam->get(request);
    chunk.reply = reply;

    connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
    connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
    connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);
    return true;
}

bool ChunkedDownloader::validateChunkResponse(ChunkState& chunk) {
    QNetworkReply* reply = chunk.reply;
    if (!reply) return true;
    if (chunk.requestStart < 0) return true;   // unranged request: nothing to check

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
        // The server either ignores Range headers entirely or, on an If-Range
        // resume, reports that the remote file changed. Appending this full-body
        // response to ranged chunks would silently corrupt the file, so flag it:
        // onChunkFinished restarts the download once and fails if it recurs.
        pendingRangeViolation = true;
        chunk.error = true;
        chunk.errorMessage = "Server answered 200 OK to a ranged request (Range ignored or remote file changed)";
        Logger::instance().warning("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
        return false;
    }
    if (status != 206) {
        chunk.error = true;
        chunk.errorMessage = "Unexpected HTTP status " + QString::number(status) + " for a ranged request (expected 206)";
        Logger::instance().error("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
        return false;
    }

    QByteArray contentRange = reply->rawHeader("Content-Range");   // bytes s-e/total|*
    static const QRegularExpression contentRangeRe(
        QStringLiteral("^bytes (\\d+)-(\\d+)/(\\d+|\\*)$"), QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = contentRangeRe.match(QString::fromLatin1(contentRange.trimmed()));
    if (!m.hasMatch()) {
        chunk.error = true;
        chunk.errorMessage = "Missing or malformed Content-Range header: " + QString::fromLatin1(contentRange);
        Logger::instance().error("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
        return false;
    }

    qint64 start = m.captured(1).toLongLong();
    qint64 end = m.captured(2).toLongLong();
    qint64 total = (m.captured(3) == QLatin1String("*")) ? -1 : m.captured(3).toLongLong();

    if (start != chunk.requestStart || end != chunk.endByte) {
        chunk.error = true;
        chunk.errorMessage = QString("Content-Range mismatch: got bytes %1-%2, requested bytes %3-%4")
                                 .arg(start).arg(end).arg(chunk.requestStart).arg(chunk.endByte);
        Logger::instance().error("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
        return false;
    }
    if (total >= 0 && totalBytes > 0 && total != totalBytes) {
        chunk.error = true;
        chunk.errorMessage = QString("Remote file size changed: Content-Range total %1 != expected %2")
                                 .arg(total).arg(totalBytes);
        Logger::instance().error("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
        return false;
    }

    QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
    if (cl.isValid()) {
        qint64 len = cl.toLongLong();
        qint64 promised = end - start + 1;
        if (len >= 0 && len != promised) {
            chunk.error = true;
            chunk.errorMessage = QString("Content-Length %1 does not match the promised range size %2")
                                     .arg(len).arg(promised);
            Logger::instance().error("Chunk " + QString::number(chunk.index) + ": " + chunk.errorMessage);
            return false;
        }
    }

    return true;
}

void ChunkedDownloader::onChunkReadyRead() {
    if (paused) return;

    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    // Verify the response contract (206 + exact Content-Range) BEFORE a single
    // body byte is written: a violated contract must never touch the file.
    ChunkState* target = nullptr;
    for (ChunkState& chunk : chunks) {
        if (chunk.reply == reply) {
            target = &chunk;
            break;
        }
    }
    if (!target) return;

    if (!target->responseValidated) {
        if (!validateChunkResponse(*target)) {
            // This chunk cannot complete; abort every transfer first into a
            // local list so the all-done handling (which may restart the whole
            // download and clear `chunks`) runs outside this loop.
            QList<QNetworkReply*> pending;
            for (ChunkState& chunk : chunks) {
                if (chunk.reply) pending.append(chunk.reply);
            }
            for (QNetworkReply* pendingReply : pending) {
                pendingReply->abort();
            }
            return;
        }
        target->responseValidated = true;
    }

    lastActivityMs = QDateTime::currentMSecsSinceEpoch();

    if (throttleActive) {
        refreshThrottleBudget();
        if (throttleRemaining > 0) {
            drainAvailableData(throttleRemaining);
            throttleRemaining = 0;
        }
        // If data remains buffered (over the limit), keep the drain timer running.
        bool buffered = false;
        for (const ChunkState& chunk : chunks) {
            if (chunk.reply && chunk.file && chunk.reply->bytesAvailable() > 0) {
                buffered = true;
                break;
            }
        }
        if (buffered) {
            if (!drainTimer->isActive()) drainTimer->start();
        }
        return;
    }

    for (ChunkState& chunk : chunks) {
        if (chunk.reply == reply) {
            QByteArray data = reply->readAll();
            if (chunk.file) {
                chunk.file->write(data);
            }
            chunk.downloaded += data.size();
            downloadedBytes += data.size();
            emit downloadProgress(downloadId, downloadedBytes, totalBytes);
            break;
        }
    }
}

void ChunkedDownloader::onChunkFinished() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    bool matched = false;
    for (ChunkState& chunk : chunks) {
        if (chunk.reply == reply) {
            matched = true;
            // Validate here as well: a reply can finish without ever emitting
            // readyRead (e.g. an immediate 200/4xx with an empty body).
            if (!chunk.responseValidated && reply->error() == QNetworkReply::NoError) {
                if (validateChunkResponse(chunk)) {
                    chunk.responseValidated = true;
                } else {
                    // This chunk cannot complete; abort the siblings (collected
                    // into a local list first: the all-done handling below may
                    // restart the download and clear `chunks`).
                    QList<QNetworkReply*> pending;
                    for (ChunkState& sibling : chunks) {
                        if (sibling.reply && sibling.reply != reply) pending.append(sibling.reply);
                    }
                    for (QNetworkReply* pendingReply : pending) {
                        pendingReply->abort();
                    }
                }
                // On failure chunk.error is already set by the validator and the
                // tail drain below is skipped (it only runs when validated).
            }

            if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError && !chunk.error) {
                Logger::instance().error("Chunk " + QString::number(chunk.index) + " error: " + reply->errorString());
                chunk.error = true;
                chunk.errorMessage = reply->errorString();
            }

            // A clean finish can leave trailing bytes buffered that never arrived
            // via readyRead; drain them into the file or they are lost. Never
            // drain an unvalidated response: it may be a full-body 200 reply to a
            // ranged request, and writing it would corrupt the chunk file.
            if (!paused && !cancelled && reply->error() == QNetworkReply::NoError && chunk.responseValidated) {
                QByteArray tail = reply->readAll();
                if (!tail.isEmpty() && chunk.file) {
                    chunk.file->write(tail);
                    chunk.downloaded += tail.size();
                    downloadedBytes += tail.size();
                    lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                    emit downloadProgress(downloadId, downloadedBytes, totalBytes);
                }
            }

            if (chunk.file) {
                chunk.file->close();
                chunk.file->deleteLater();
                chunk.file = nullptr;
            }
            reply->deleteLater();
            chunk.reply = nullptr;
            break;
        }
    }

    // A reply that was already detached (aborted during cleanup or a restart)
    // must not drive the all-done logic: the state it describes is no longer ours.
    if (!matched) return;

    bool allDone = true;
    for (const ChunkState& chunk : chunks) {
        if (chunk.reply != nullptr || chunk.file != nullptr) {
            allDone = false;
            break;
        }
    }

    if (allDone) {
        // A failure/cancel path that already gave up on this attempt may have
        // arrived here through a re-entrant abort chain; never merge in that case.
        if (!downloading) return;

        speedTimer->stop();
        hangTimer->stop();
        speed = 0;

        if (paused || cancelled) {
            downloading = false;
            return;
        }

        // Range-contract violation: restart the whole attempt ONCE instead of
        // failing outright - the first time it is almost always a server that
        // advertises Accept-Ranges but ignores Range, or an If-Range resume
        // whose validator reported the remote file changed.
        if (pendingRangeViolation && !rangeViolationRestarted) {
            rangeViolationRestarted = true;
            bool remoteChanged = attachingIfRange;
            if (!remoteChanged) {
                // Remember that this server ignores Range headers: the restart
                // must go out as a single stream (startDownload re-reads the
                // HEAD response, and forceNoRange keeps supportsRange false).
                forceNoRange = true;
            }
            Logger::instance().warning(
                "Ranged request was answered with 200 OK; restarting download as "
                + QString(remoteChanged ? "a fresh transfer (remote file changed)"
                                        : "a single-stream transfer"));
            pendingRangeViolation = false;
            cleanupChunks();          // replies/files are already null here
            cleanupTempFiles();       // drop stale partial data - never reuse it
            downloadedBytes = 0;
            startDownload(downloadUrl, saveFilePath, totalChunks, downloadId);
            return;
        }

        bool hasError = false;
        QString errorMsg;
        for (const ChunkState& chunk : chunks) {
            if (chunk.error) {
                hasError = true;
                errorMsg = chunk.errorMessage;
            }
        }

        if (hasError) {
            emit downloadFailed(downloadId, errorMsg);
            downloading = false;
            return;
        }

        if (!mergeChunks()) {
            emit downloadFailed(downloadId,
                "Failed to assemble the final file: on-disk size verification failed (see log for details).");
            downloading = false;
            return;
        }

        qint64 finalSize = QFileInfo(saveFilePath).size();
        if (totalBytes > 0 && finalSize != totalBytes) {
            Logger::instance().error("Download size mismatch: " + QString::number(finalSize) + " vs expected " + QString::number(totalBytes) + " bytes");
            emit downloadFailed(downloadId,
                QString("Download size mismatch: the final file is %1 bytes, expected %2 bytes.")
                    .arg(finalSize)
                    .arg(totalBytes));
            downloading = false;
            return;
        }

        qint64 finalTotal = totalBytes > 0 ? totalBytes : finalSize;
        emit downloadProgress(downloadId, finalTotal, finalTotal);
        emit downloadFinished(downloadId);
        downloading = false;
    }
}

void ChunkedDownloader::onChunkError(QNetworkReply::NetworkError error) {
    if (error == QNetworkReply::OperationCanceledError) return;

    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply) {
        Logger::instance().error("Chunk download error: " + reply->errorString());
    }
}

void ChunkedDownloader::onSpeedTimer() {
    qint64 currentBytes = downloadedBytes;
    speed = currentBytes - lastSpeedBytes;
    lastSpeedBytes = currentBytes;
    emit speedUpdated(speed);
}

bool ChunkedDownloader::mergeChunks() {
    // Close any open chunk handles first (resume path can merge while files are open).
    for (ChunkState& chunk : chunks) {
        if (chunk.file) {
            chunk.file->close();
            chunk.file->deleteLater();
            chunk.file = nullptr;
        }
    }

    // Verify every chunk file BEFORE touching the destination: each ranged
    // chunk must hold exactly its requested byte range, and the sizes must sum
    // to the expected total. Nothing is written unless the input is complete.
    qint64 actualTotal = 0;
    for (const ChunkState& chunk : chunks) {
        qint64 size = QFileInfo(chunkFilePath(chunk.index)).size();
        if (chunk.endByte >= 0) {
            qint64 expected = chunk.endByte - chunk.startByte + 1;
            if (size != expected) {
                Logger::instance().error(QString("Chunk %1 size mismatch: %2 bytes on disk, expected %3 bytes")
                                              .arg(chunk.index).arg(size).arg(expected));
                return false;
            }
        } else if (totalBytes > 0 && size != totalBytes) {
            Logger::instance().error(QString("Download size mismatch: %1 bytes on disk, expected %2 bytes")
                                         .arg(size).arg(totalBytes));
            return false;
        }
        actualTotal += size;
    }
    if (totalBytes > 0 && actualTotal != totalBytes) {
        Logger::instance().error(QString("Chunk sizes sum to %1 bytes, expected %2 bytes")
                                     .arg(actualTotal).arg(totalBytes));
        return false;
    }

    if (chunks.size() == 1) {
        // Fast path: the single chunk is the whole file. Rename it into place
        // when the destination does not exist (atomic); otherwise stage through
        // QSaveFile below so an existing file is only ever replaced by a
        // verified one - never removed up front.
        QString chunkPath = chunkFilePath(0);
        if (!QFile::exists(saveFilePath) && QFile::rename(chunkPath, saveFilePath)) {
            cleanupTempFiles();
            return true;
        }
    }

    // Assemble through QSaveFile: data goes to a temporary file that is
    // atomically swapped in on commit, so a failure or crash mid-merge leaves
    // any previous file intact and never exposes a partial result.
    QSaveFile out(saveFilePath);
    if (!out.open(QIODevice::WriteOnly)) {
        Logger::instance().error("Cannot create output file: " + saveFilePath);
        return false;
    }

    for (const ChunkState& chunk : chunks) {
        QFile chunkFile(chunkFilePath(chunk.index));
        if (!chunkFile.open(QIODevice::ReadOnly)) {
            Logger::instance().error("Cannot read chunk file: " + chunkFilePath(chunk.index));
            return false;   // QSaveFile destructor discards the staged temp file
        }
        while (!chunkFile.atEnd()) {
            QByteArray block = chunkFile.read(1024 * 1024);
            if (block.isEmpty()) break;
            if (out.write(block) != block.size()) {
                Logger::instance().error("Write failed while assembling: " + saveFilePath);
                return false;
            }
        }
        chunkFile.close();
    }

    if (!out.commit()) {
        Logger::instance().error("Cannot finalize download file: " + saveFilePath);
        return false;
    }

    if (QFileInfo(saveFilePath).size() != actualTotal) {
        Logger::instance().error("Assembled file size differs from the sum of chunk sizes");
        return false;
    }

    cleanupTempFiles();
    return true;
}

void ChunkedDownloader::cleanupChunks() {
    for (ChunkState& chunk : chunks) {
        if (chunk.reply) {
            QNetworkReply* reply = chunk.reply;
            // Detach first: abort() may emit finished synchronously, and the
            // handler must not see a half-torn-down chunk (or null this pointer
            // out from under us).
            chunk.reply = nullptr;
            reply->abort();
            reply->deleteLater();
        }
        if (chunk.file) {
            chunk.file->close();
            chunk.file->deleteLater();
            chunk.file = nullptr;
        }
    }
    chunks.clear();
}

void ChunkedDownloader::cleanupTempFiles() {
    // Pattern-based so stray chunk files beyond the current chunk count (a
    // previous session with a different layout) cannot be left behind.
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    QDirIterator it(tempDir, QStringList() << QString::number(downloadId) + "_*.chunk", QDir::Files);
    while (it.hasNext()) {
        QFile::remove(it.next());
    }
    QFile::remove(resumeStatePath());
}

QString ChunkedDownloader::chunkFilePath(int index) const {
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    QDir().mkpath(tempDir);
    QString safeName = QString::number(downloadId) + "_" + QString::number(index);
    return tempDir + "/" + safeName + ".chunk";
}

void ChunkedDownloader::pause() {
    if (!downloading || paused) return;
    paused = true;

    for (ChunkState& chunk : chunks) {
        if (chunk.reply) {
            chunk.reply->abort();
        }
    }

    drainTimer->stop();
    speedTimer->stop();
    hangTimer->stop();
    persistResumeState();
    Logger::instance().info("Download paused (id: " + QString::number(downloadId) + ")");
}

void ChunkedDownloader::resume() {
    if (!paused) return;
    paused = false;
    downloading = true;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();

    // Without Range support the partial chunks cannot be continued; restart from scratch.
    if (!supportsRange) {
        Logger::instance().info("Server does not support resume, restarting download (id: " + QString::number(downloadId) + ")");
        startDownload(downloadUrl, saveFilePath, totalChunks, downloadId);
        return;
    }

    // Stale partial data exists: attach If-Range validators so a server-side
    // change since the pause answers 200 OK instead of mixing file versions.
    attachingIfRange = true;

    for (ChunkState& chunk : chunks) {
        if (chunk.reply == nullptr && chunk.downloaded < (chunk.endByte - chunk.startByte + 1)) {
            if (!startChunkRequest(chunk)) {
                downloading = false;
                cleanupChunks();
                return;
            }
        }
    }

    speedTimer->start(1000);
    hangTimer->start();
    Logger::instance().info("Download resumed (id: " + QString::number(downloadId) + ")");
}

void ChunkedDownloader::resumeFromState(const QString& url, const QString& filePath, int numChunks, qint64 totalSize, bool range, int id) {
    Logger::instance().info("Resuming interrupted download from saved chunks (id: " + QString::number(id) + ")");

    downloadUrl = url;
    saveFilePath = filePath;
    totalChunks = qBound(1, numChunks, 128);
    downloadId = id;
    totalBytes = totalSize;
    supportsRange = range;
    downloading = true;
    paused = false;
    cancelled = false;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();

    resetThrottleState();

    // Validators recorded when the download first ran: If-Range on the resumed
    // requests makes the server refuse (200 OK) if the remote file changed, so
    // stale chunks can never be extended with bytes from a different file.
    QJsonObject state = readPersistedState(downloadId);
    respEtag = state.value("etag").toString();
    respLastModified = state.value("lastModified").toString();
    attachingIfRange = true;
    pendingRangeViolation = false;
    rangeViolationRestarted = false;
    forceNoRange = false;

    // If the target already exists and is EXACTLY complete, this download
    // finished before the app stopped. Anything else (short OR oversized) is
    // not a verified result and must not be reported as done.
    if (QFile::exists(saveFilePath) && totalBytes > 0 && QFileInfo(saveFilePath).size() == totalBytes) {
        cleanupTempFiles();
        emit downloadProgress(id, totalBytes, totalBytes);
        emit downloadFinished(id);
        downloading = false;
        return;
    }

    if (!supportsRange || totalBytes <= 0) {
        Logger::instance().info("Server does not support resume after restart, restarting (id: " + QString::number(id) + ")");
        startDownload(downloadUrl, saveFilePath, totalChunks, downloadId);
        return;
    }

    QDir().mkpath(QFileInfo(saveFilePath).absolutePath());

    qint64 chunkSize = totalBytes / totalChunks;
    if (chunkSize < 1024) {
        totalChunks = 1;
        chunkSize = totalBytes;
    }

    downloadedBytes = 0;
    for (int i = 0; i < totalChunks; i++) {
        ChunkState chunk;
        chunk.index = i;
        chunk.startByte = i * chunkSize;
        chunk.endByte = (i == totalChunks - 1) ? (totalBytes - 1) : ((i + 1) * chunkSize - 1);

        // Recover how much of this chunk was written before the app stopped.
        // A file LARGER than its range means the on-disk data is not what the
        // range promised (interrupted write under a different layout, or a
        // corrupted/trimmed file): restart that chunk from scratch instead of
        // appending to bytes that cannot be part of a valid result.
        qint64 expected = chunk.endByte - chunk.startByte + 1;
        qint64 onDisk = 0;
        QFileInfo fi(chunkFilePath(i));
        if (fi.exists()) onDisk = fi.size();
        if (onDisk > expected) {
            Logger::instance().warning(QString("Chunk %1 on disk (%2 bytes) exceeds its range (%3 bytes); restarting that chunk")
                                           .arg(i).arg(onDisk).arg(expected));
            onDisk = 0;
        }
        chunk.downloaded = onDisk;
        downloadedBytes += chunk.downloaded;
        chunks.append(chunk);

        // startChunkRequest opens the file (append when partially written,
        // truncate for a restarted chunk) and issues the exact remaining range;
        // complete chunks are left untouched with no reply.
        if (!startChunkRequest(chunks.last())) {
            downloading = false;
            cleanupChunks();
            return;
        }
    }

    if (downloadedBytes >= totalBytes) {
        // All chunks were already fully written; verify and merge them.
        if (!mergeChunks()) {
            emit downloadFailed(downloadId,
                "Failed to assemble the final file: on-disk size verification failed (see log for details).");
            downloading = false;
            return;
        }
        emit downloadProgress(downloadId, totalBytes, totalBytes);
        emit downloadFinished(downloadId);
        downloading = false;
        return;
    }

    speedTimer->start(1000);
    hangTimer->start();
    emit downloadProgress(downloadId, downloadedBytes, totalBytes);
}

void ChunkedDownloader::cancel() {
    downloading = false;
    paused = false;
    cancelled = true;
    speedTimer->stop();
    drainTimer->stop();
    hangTimer->stop();
    cleanupChunks();
    // Do NOT delete the partial .chunk files here: they are the resume data for
    // a subsequent app session. Explicit removal happens in discardPartialData()
    // (user cancel/remove) or after a completed merge (cleanupTempFiles).
    persistResumeState();
}

void ChunkedDownloader::discardPartialData() {
    cleanupTempFiles();
    QFile::remove(resumeStatePath());
}

void ChunkedDownloader::persistResumeState() const {
    if (downloadId <= 0) return;
    // Nothing to resume for a completed transfer: its chunk files are already
    // merged/removed, and a stale state file would make a future restore think
    // there is resumable data.
    if (totalBytes > 0 && downloadedBytes >= totalBytes) return;
    if (totalBytes > 0 && QFile::exists(saveFilePath) && QFileInfo(saveFilePath).size() >= totalBytes) return;

    QJsonObject state;
    state["version"] = 2;
    state["url"] = downloadUrl;
    state["filePath"] = saveFilePath;
    state["chunks"] = totalChunks;
    state["totalBytes"] = QString::number(totalBytes);
    state["supportsRange"] = supportsRange;
    state["downloaded"] = QString::number(downloadedBytes);
    // Integrity metadata for the resume: the validators let the next session
    // send If-Range, and the per-chunk sizes record exactly what was on disk so
    // a resume can detect a chunk that was corrupted or written past its range.
    state["etag"] = respEtag;
    state["lastModified"] = respLastModified;
    {
        QJsonArray sizes;
        for (int i = 0; i < totalChunks; i++) {
            QFileInfo fi(chunkFilePath(i));
            sizes.append(QString::number(fi.exists() ? fi.size() : 0));
        }
        state["chunkSizes"] = sizes;
    }

    // QSaveFile writes a temporary and atomically renames it into place, so a
    // crash mid-write can never leave a truncated/corrupt state file behind.
    QSaveFile f(resumeStatePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    if (f.write(QJsonDocument(state).toJson(QJsonDocument::Compact)) < 0) return;
    if (!f.commit()) {
        Logger::instance().warning("Could not write resume state atomically: " + resumeStatePath());
    }
}

bool ChunkedDownloader::hasPersistedData(int downloadId) {
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    QDir().mkpath(tempDir);
    if (!QFile::exists(tempDir + "/" + QString::number(downloadId) + ".resume.json")) return false;
    QDirIterator it(tempDir, QStringList() << QString::number(downloadId) + "_*.chunk", QDir::Files);
    return it.hasNext();
}

QJsonObject ChunkedDownloader::readPersistedState(int downloadId) {
    QJsonObject empty;
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    QFile f(tempDir + "/" + QString::number(downloadId) + ".resume.json");
    if (!f.open(QIODevice::ReadOnly)) return empty;
    QByteArray data = f.readAll();
    f.close();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return empty;
    return doc.object();
}

QString ChunkedDownloader::resumeStatePath() const {
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    return tempDir + "/" + QString::number(downloadId) + ".resume.json";
}

void ChunkedDownloader::onHangTimer() {
    if (paused || cancelled || !downloading) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastActivityMs < 90000) return;

    // No data received for 90s: fail the stalled chunks instead of hanging forever.
    QList<QNetworkReply*> toAbort;
    for (ChunkState& chunk : chunks) {
        if (chunk.reply && !chunk.error) {
            chunk.error = true;
            chunk.errorMessage = "Chunk timed out (no data received for 90s)";
            toAbort.append(chunk.reply);
        }
    }
    for (QNetworkReply* reply : toAbort) {
        reply->abort();
    }
    emit downloadProgress(downloadId, downloadedBytes, totalBytes);
}

bool ChunkedDownloader::isDownloading() const { return downloading && !paused; }
bool ChunkedDownloader::isPaused() const { return paused; }
qint64 ChunkedDownloader::getDownloadedBytes() const { return downloadedBytes; }
qint64 ChunkedDownloader::getTotalBytes() const { return totalBytes; }
qint64 ChunkedDownloader::getSpeed() const { return speed; }

void ChunkedDownloader::setSpeedLimit(qint64 bytesPerSecond) {
    if (bytesPerSecond == limitBytesPerSec) return;

    limitBytesPerSec = qMax<qint64>(0, bytesPerSecond);
    throttleActive = limitBytesPerSec > 0;

    if (throttleActive) {
        refreshThrottleBudget();
    } else {
        // Unlimited: stop throttling and immediately drain any buffered data.
        drainTimer->stop();
        throttleRemaining = 0;
        drainAvailableData(Q_INT64_C(1) << 62);
    }
}

void ChunkedDownloader::refreshThrottleBudget() {
    if (!throttleActive) return;
    qint64 elapsedMs = qMax<qint64>(1, throttleTimer.elapsed());
    // Guard against overflow for absurdly large limits.
    qint64 allowance = (limitBytesPerSec > Q_INT64_C(0x7FFFFFFF))
        ? Q_INT64_C(1) << 62
        : (limitBytesPerSec * elapsedMs) / 1000;
    if (allowance > throttleRemaining) {
        throttleRemaining = allowance - throttleRemaining;
        throttleBudget = throttleRemaining;
    }
    throttleTimer.restart();
}

void ChunkedDownloader::drainAvailableData(qint64 maxBytes) {
    qint64 budgetLeft = maxBytes;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    for (ChunkState& chunk : chunks) {
        if (budgetLeft <= 0) break;
        if (!chunk.reply || !chunk.file) continue;
        if (chunk.reply->bytesAvailable() <= 0) continue;

        qint64 toRead = qMin<qint64>(budgetLeft, chunk.reply->bytesAvailable());
        QByteArray data = chunk.reply->read(toRead);
        if (data.isEmpty()) continue;
        chunk.file->write(data);
        chunk.downloaded += data.size();
        downloadedBytes += data.size();
        budgetLeft -= data.size();
        emit downloadProgress(downloadId, downloadedBytes, totalBytes);
    }
}

void ChunkedDownloader::onDrainTimer() {
    if (!downloading || paused || !throttleActive) {
        drainTimer->stop();
        return;
    }

    refreshThrottleBudget();
    if (throttleRemaining > 0) {
        drainAvailableData(throttleRemaining);
        throttleRemaining = 0;
    }

    // If any chunk still has buffered data, keep the drain timer running so the
    // data is consumed across windows. Otherwise stop it.
    bool buffered = false;
    for (const ChunkState& chunk : chunks) {
        if (chunk.reply && chunk.file && chunk.reply->bytesAvailable() > 0) {
            buffered = true;
            break;
        }
    }
    if (!buffered) {
        drainTimer->stop();
    }
}

void ChunkedDownloader::resetThrottleState() {
    limitBytesPerSec = 0;
    throttleActive = false;
    throttleRemaining = 0;
    throttleBudget = 0;
    drainTimer->stop();
    throttleTimer.restart();
}
