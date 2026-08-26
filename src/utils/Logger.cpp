#include "utils/Logger.h"
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>

Logger::Logger() : maxLogSize(5 * 1024 * 1024), maxLogFiles(3) {
    logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(logDir);
    logFilePath = logDir + "/copper.log";
}

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::setLogDirectory(const QString& dir) {
    QMutexLocker locker(&mutex);
    logDir = dir;
    QDir().mkpath(logDir);
    logFilePath = logDir + "/copper.log";
}

QString Logger::getLogFilePath() const {
    return logFilePath;
}

void Logger::info(const QString& message) {
    writeLog("INFO", message);
}

void Logger::error(const QString& message) {
    writeLog("ERROR", message);
}

void Logger::debug(const QString& message) {
    writeLog("DEBUG", message);
}

void Logger::warning(const QString& message) {
    writeLog("WARN", message);
}

void Logger::writeLog(const QString& level, const QString& message) {
    QMutexLocker locker(&mutex);

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString line = QString("[%1] %2 - %3").arg(level, timestamp, message);

    qDebug().noquote() << line;

    QFile file(logFilePath);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write((line + "\n").toUtf8());
        file.close();
    }

    if (file.size() > maxLogSize) {
        rotateLogs();
    }
}

void Logger::rotateLogs() {
    for (int i = maxLogFiles - 1; i >= 1; i--) {
        QString oldFile = logFilePath + "." + QString::number(i);
        QString newFile = logFilePath + "." + QString::number(i + 1);
        if (QFile::exists(oldFile)) {
            QFile::remove(newFile);
            QFile::rename(oldFile, newFile);
        }
    }
    if (QFile::exists(logFilePath)) {
        QFile::rename(logFilePath, logFilePath + ".1");
    }
}
