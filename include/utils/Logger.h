#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QMutex>

class Logger : public QObject {
    Q_OBJECT
public:
    static Logger& instance();
    void info(const QString& message);
    void error(const QString& message);
    void debug(const QString& message);
    void warning(const QString& message);
    QString getLogFilePath() const;
    void setLogDirectory(const QString& dir);

private:
    Logger();
    void writeLog(const QString& level, const QString& message);
    void rotateLogs();

    QString logFilePath;
    QString logDir;
    QMutex mutex;
    qint64 maxLogSize;
    int maxLogFiles;
};

#endif
