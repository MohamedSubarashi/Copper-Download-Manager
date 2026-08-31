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
    totalChunks = chunks;
    downloadId = id;
    downloading = true;
    paused = false;
    cancelled = false;
    downloadedBytes = 0;
    totalBytes = 0;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();

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

    QString realName = extractFilenameFromContentDisposition(headReply->rawHeader("Content-Disposition"));
    if (!realName.isEmpty()) {
        QString dir = QFileInfo(saveFilePath).absolutePath();
        saveFilePath = dir + "/" + realName;
        Logger::instance().info("Content-Disposition filename: " + realName);
        emit filePathChanged(downloadId, saveFilePath);
    }

    int statusCode = headReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    supportsRange = (statusCode == 206) || headReply->rawHeader("Accept-Ranges").contains("bytes");

    headReply->deleteLater();
    headReply = nullptr;

    if (totalBytes <= 0 && supportsRange) {
        QNetworkRequest fullRequest{QUrl{downloadUrl}};
        fullRequest.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
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

    QString realName = extractFilenameFromContentDisposition(reply->rawHeader("Content-Disposition"));
    if (!realName.isEmpty()) {
        QString dir = QFileInfo(saveFilePath).absolutePath();
        saveFilePath = dir + "/" + realName;
        Logger::instance().info("Content-Disposition filename (fallback): " + realName);
        emit filePathChanged(downloadId, saveFilePath);
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    supportsRange = false;

    if (statusCode == 206 || reply->rawHeader("Accept-Ranges").contains("bytes")) {
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
    reply->deleteLater();

    QFile* file = new QFile(saveFilePath);
    if (!file->open(QIODevice::WriteOnly)) {
        emit downloadFailed(downloadId, "Cannot open file for writing: " + saveFilePath);
        downloading = false;
        return;
    }
    file->write(data);
    file->close();
    file->deleteLater();

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

        QFile* file = new QFile(chunkFilePath(0));
        if (!file->open(QIODevice::WriteOnly)) {
            emit downloadFailed(downloadId, "Cannot open file for writing: " + chunkFilePath(0));
            downloading = false;
            return;
        }
        chunk.file = file;

        QNetworkRequest request{QUrl{downloadUrl}};
        request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply* reply = nam->get(request);
        chunk.reply = reply;

        connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
        connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
        connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);

        chunks.append(chunk);
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

        QFile* file = new QFile(chunkFilePath(i));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit downloadFailed(downloadId, "Cannot open chunk file: " + chunkFilePath(i));
            cleanupChunks();
            downloading = false;
            return;
        }
        chunk.file = file;

        QNetworkRequest request{QUrl{downloadUrl}};
        request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QString rangeHeader = "bytes=" + QByteArray::number(chunk.startByte) + "-" + QByteArray::number(chunk.endByte);
        request.setRawHeader("Range", rangeHeader.toUtf8());

        QNetworkReply* reply = nam->get(request);
        chunk.reply = reply;

        connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
        connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
        connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);

        chunks.append(chunk);
    }

    hangTimer->start();
}

void ChunkedDownloader::onChunkReadyRead() {
    if (paused) return;

    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

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

    for (ChunkState& chunk : chunks) {
        if (chunk.reply == reply) {
            if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError) {
                Logger::instance().error("Chunk " + QString::number(chunk.index) + " error: " + reply->errorString());
                chunk.error = true;
                chunk.errorMessage = reply->errorString();
            }

            // A clean finish can leave trailing bytes buffered that never arrived
            // via readyRead; drain them into the file or they are lost.
            if (!paused && !cancelled && reply->error() == QNetworkReply::NoError) {
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

    bool allDone = true;
    for (const ChunkState& chunk : chunks) {
        if (chunk.reply != nullptr || chunk.file != nullptr) {
            allDone = false;
            break;
        }
    }

    if (allDone) {
        speedTimer->stop();
        hangTimer->stop();
        speed = 0;

        if (paused || cancelled) {
            downloading = false;
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

        mergeChunks();

        qint64 finalSize = QFileInfo(saveFilePath).size();
        if (totalBytes > 0 && finalSize < totalBytes) {
            Logger::instance().error("Download incomplete: " + QString::number(finalSize) + " of " + QString::number(totalBytes) + " bytes");
            emit downloadFailed(downloadId,
                QString("Download incomplete: received %1 of %2 bytes. The connection dropped before all data arrived.")
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

void ChunkedDownloader::mergeChunks() {
    // Close any open chunk handles first (resume path can merge while files are open).
    for (ChunkState& chunk : chunks) {
        if (chunk.file) {
            chunk.file->close();
            chunk.file->deleteLater();
            chunk.file = nullptr;
        }
    }

    if (chunks.size() == 1) {
        QFile::remove(saveFilePath);
        if (!QFile::rename(chunkFilePath(0), saveFilePath)) {
            if (!QFile::copy(chunkFilePath(0), saveFilePath)) {
                Logger::instance().error("Cannot finalize download file: " + saveFilePath);
            }
            QFile::remove(chunkFilePath(0));
        }
        cleanupTempFiles();
        return;
    }

    QFile outputFile(saveFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::instance().error("Cannot create output file: " + saveFilePath);
        return;
    }

    for (int i = 0; i < chunks.size(); i++) {
        QFile chunkFile(chunkFilePath(i));
        if (chunkFile.open(QIODevice::ReadOnly)) {
            while (!chunkFile.atEnd()) {
                outputFile.write(chunkFile.read(1024 * 1024));
            }
            chunkFile.close();
        }
    }

    outputFile.close();
    cleanupTempFiles();
}

void ChunkedDownloader::cleanupChunks() {
    for (ChunkState& chunk : chunks) {
        if (chunk.reply) {
            chunk.reply->abort();
            chunk.reply->deleteLater();
            chunk.reply = nullptr;
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
    for (int i = 0; i < totalChunks; i++) {
        QFile::remove(chunkFilePath(i));
    }
    QFile::remove(resumeStatePath());
}

QString ChunkedDownloader::chunkFilePath(int index) {
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

    for (ChunkState& chunk : chunks) {
        if (chunk.reply == nullptr && chunk.downloaded < (chunk.endByte - chunk.startByte + 1)) {
            QNetworkRequest request{QUrl{downloadUrl}};
            request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

            qint64 resumeFrom = chunk.startByte + chunk.downloaded;
            QString rangeHeader = "bytes=" + QByteArray::number(resumeFrom) + "-" + QByteArray::number(chunk.endByte);
            request.setRawHeader("Range", rangeHeader.toUtf8());

            QFile* file = new QFile(chunkFilePath(chunk.index));
            if (file->open(QIODevice::WriteOnly | QIODevice::Append)) {
                chunk.file = file;
            }

            QNetworkReply* reply = nam->get(request);
            chunk.reply = reply;

            connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
            connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
            connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);
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
    totalChunks = numChunks;
    downloadId = id;
    totalBytes = totalSize;
    supportsRange = range;
    downloading = true;
    paused = false;
    cancelled = false;
    lastActivityMs = QDateTime::currentMSecsSinceEpoch();

    resetThrottleState();

    // If the target already exists and is complete, this download finished
    // before the app stopped; just report it done and drop the stale state.
    if (QFile::exists(saveFilePath) && totalBytes > 0 && QFileInfo(saveFilePath).size() >= totalBytes) {
        cleanupTempFiles();
        QFile::remove(resumeStatePath());
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

        // Recover how much of this chunk was written before the app stopped and
        // reopen the file in append so new data continues where it left off.
        chunk.downloaded = 0;
        QFileInfo fi(chunkFilePath(i));
        if (fi.exists()) {
            chunk.downloaded = qBound<qint64>(0, fi.size(), chunk.endByte - chunk.startByte + 1);
        }
        downloadedBytes += chunk.downloaded;

        QFile* file = new QFile(chunkFilePath(i));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
            emit downloadFailed(downloadId, "Cannot open chunk file for resume: " + chunkFilePath(i));
            cleanupChunks();
            downloading = false;
            return;
        }
        chunk.file = file;

        if (chunk.downloaded < (chunk.endByte - chunk.startByte + 1)) {
            QNetworkRequest request{QUrl{downloadUrl}};
            request.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

            qint64 resumeFrom = chunk.startByte + chunk.downloaded;
            QString rangeHeader = "bytes=" + QByteArray::number(resumeFrom) + "-" + QByteArray::number(chunk.endByte);
            request.setRawHeader("Range", rangeHeader.toUtf8());

            QNetworkReply* reply = nam->get(request);
            chunk.reply = reply;

            connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
            connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
            connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);
        }

        chunks.append(chunk);
    }

    if (downloadedBytes >= totalBytes) {
        // All chunks were already fully written; just merge and finish.
        mergeChunks();
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
    state["url"] = downloadUrl;
    state["filePath"] = saveFilePath;
    state["chunks"] = totalChunks;
    state["totalBytes"] = QString::number(totalBytes);
    state["supportsRange"] = supportsRange;
    state["downloaded"] = QString::number(downloadedBytes);

    QFile f(resumeStatePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
    f.close();
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
