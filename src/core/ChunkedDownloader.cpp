#include "core/ChunkedDownloader.h"
#include "utils/Logger.h"
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

ChunkedDownloader::ChunkedDownloader(QObject* parent)
    : QObject(parent)
    , totalChunks(16)
    , downloadId(0)
    , downloading(false)
    , paused(false)
    , supportsRange(false)
    , totalBytes(0)
    , downloadedBytes(0)
    , lastSpeedBytes(0)
    , speed(0)
    , nam(new QNetworkAccessManager(this))
    , headReply(nullptr)
{
    speedTimer = new QTimer(this);
    connect(speedTimer, &QTimer::timeout, this, &ChunkedDownloader::onSpeedTimer);
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
            if (!decoded.isEmpty() && !decoded.contains('/') && !decoded.contains('\\')) return decoded;
        }
    }

    QRegularExpression re2("filename=\"([^\"]+)\"", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m2 = re2.match(cdStr);
    if (m2.hasMatch()) {
        QString name = m2.captured(1).trimmed();
        if (!name.isEmpty() && !name.contains('/') && !name.contains('\\')) return name;
    }

    QRegularExpression re3("filename=([^;\\s]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m3 = re3.match(cdStr);
    if (m3.hasMatch()) {
        QString name = m3.captured(1).trimmed();
        if (!name.isEmpty() && !name.contains('/') && !name.contains('\\')) return name;
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
    downloadedBytes = 0;
    totalBytes = 0;

    Logger::instance().info("Starting chunked download: " + url + " -> " + filePath + " (" + QString::number(chunks) + " chunks)");

    QDir().mkpath(QFileInfo(filePath).absolutePath());

    QNetworkRequest request{QUrl{url}};
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
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
        request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
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
        request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply* reply = nam->get(request);
        chunk.reply = reply;

        connect(reply, &QNetworkReply::readyRead, this, &ChunkedDownloader::onChunkReadyRead);
        connect(reply, &QNetworkReply::finished, this, &ChunkedDownloader::onChunkFinished);
        connect(reply, &QNetworkReply::errorOccurred, this, &ChunkedDownloader::onChunkError);

        chunks.append(chunk);
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
        request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
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
}

void ChunkedDownloader::onChunkReadyRead() {
    if (paused) return;

    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

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
        speed = 0;

        bool hasError = false;
        QString errorMsg;
        for (const ChunkState& chunk : chunks) {
            if (chunk.reply && chunk.reply->error() != QNetworkReply::NoError) {
                hasError = true;
                errorMsg = chunk.reply->errorString();
            }
        }

        if (hasError) {
            emit downloadFailed(downloadId, errorMsg);
        } else {
            mergeChunks();
            emit downloadProgress(downloadId, totalBytes, totalBytes);
            emit downloadFinished(downloadId);
        }

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
    if (chunks.size() == 1) {
        QFile::rename(chunkFilePath(0), saveFilePath);
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

    speedTimer->stop();
    Logger::instance().info("Download paused (id: " + QString::number(downloadId) + ")");
}

void ChunkedDownloader::resume() {
    if (!paused) return;
    paused = false;

    for (ChunkState& chunk : chunks) {
        if (chunk.reply == nullptr && chunk.downloaded < (chunk.endByte - chunk.startByte + 1)) {
            QNetworkRequest request{QUrl{downloadUrl}};
            request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CopperDownloadManager/1.0");
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
    Logger::instance().info("Download resumed (id: " + QString::number(downloadId) + ")");
}

void ChunkedDownloader::cancel() {
    downloading = false;
    paused = false;
    speedTimer->stop();
    cleanupChunks();
    cleanupTempFiles();
}

bool ChunkedDownloader::isDownloading() const { return downloading && !paused; }
bool ChunkedDownloader::isPaused() const { return paused; }
qint64 ChunkedDownloader::getDownloadedBytes() const { return downloadedBytes; }
qint64 ChunkedDownloader::getTotalBytes() const { return totalBytes; }
qint64 ChunkedDownloader::getSpeed() const { return speed; }

void ChunkedDownloader::setSpeedLimit(qint64 bytesPerSecond) {
    Q_UNUSED(bytesPerSecond);
}
