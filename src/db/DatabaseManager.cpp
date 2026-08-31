#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

DatabaseManager::DatabaseManager() {}

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager instance;
    return instance;
}

bool DatabaseManager::init() {
    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dbPath);
    dbPath += "/copper.db";

    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        Logger::instance().error("Database open failed: " + db.lastError().text());
        return false;
    }

    // WAL keeps readers (settings, downloads queries) from blocking writers and
    // makes crash recovery robust for the progress-heavy download writes.
    QSqlQuery pragma(db);
    pragma.exec("PRAGMA journal_mode=WAL");
    pragma.exec("PRAGMA synchronous=NORMAL");

    QSqlQuery query(db);
    query.exec("CREATE TABLE IF NOT EXISTS downloads ("
               "id INTEGER PRIMARY KEY, "
               "url TEXT, "
               "filePath TEXT, "
               "type TEXT, "
               "downloadedSize INTEGER DEFAULT 0, "
               "totalSize INTEGER DEFAULT 0, "
               "status TEXT DEFAULT 'Queued', "
               "addedAt TEXT, "
               "completedAt TEXT, "
               "error TEXT, "
               "progress REAL DEFAULT 0, "
               "isFolder INTEGER DEFAULT 0, "
               "parent_id INTEGER DEFAULT -1"
               ")");

    query.exec("CREATE TABLE IF NOT EXISTS settings ("
               "key TEXT PRIMARY KEY, "
               "value TEXT"
               ")");

    migrate(getSchemaVersion());

    Logger::instance().info("Database initialized at " + dbPath);
    return true;
}

int DatabaseManager::getSchemaVersion() {
    QSqlQuery query(db);
    if (query.exec("PRAGMA user_version") && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

void DatabaseManager::migrate(int from) {
    const int targetVersion = 1;
    QSqlQuery query(db);

    // A pre-0.3.x database may predate the parent_id column (playlist / torrent
    // folder children). Add it defensively so inserts never fail after upgrade.
    if (from < 1) {
        bool hasParent = false;
        QSqlQuery cols(db);
        if (cols.exec("PRAGMA table_info(downloads)")) {
            while (cols.next()) {
                if (cols.value("name").toString() == "parent_id") { hasParent = true; break; }
            }
        }
        if (!hasParent) {
            if (!query.exec("ALTER TABLE downloads ADD COLUMN parent_id INTEGER DEFAULT -1")) {
                Logger::instance().error("Migration to v1 failed: " + query.lastError().text());
                return;
            }
        }
        from = 1;
    }

    if (from < targetVersion) {
        Logger::instance().info("Database schema migrating from " + QString::number(from) + " to " + QString::number(targetVersion));
        from = targetVersion;
    }

    query.exec("PRAGMA user_version = " + QString::number(from));
}

void DatabaseManager::addDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("INSERT INTO downloads (id, url, filePath, type, downloadedSize, totalSize, status, addedAt, completedAt, error, progress, isFolder, parent_id) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(item.id);
    query.addBindValue(item.url);
    query.addBindValue(item.filePath);
    query.addBindValue(item.type);
    query.addBindValue(item.downloadedSize);
    query.addBindValue(item.totalSize);
    query.addBindValue(item.status);
    query.addBindValue(item.addedAt.toString(Qt::ISODate));
    query.addBindValue(item.completedAt.toString(Qt::ISODate));
    query.addBindValue(item.error);
    query.addBindValue(item.progress);
    query.addBindValue(item.isFolder ? 1 : 0);
    query.addBindValue(item.parentId);

    if (!query.exec()) {
        Logger::instance().error("Add download failed: " + query.lastError().text());
    }
}

void DatabaseManager::updateDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("UPDATE downloads SET url=?, filePath=?, type=?, downloadedSize=?, totalSize=?, status=?, "
                  "addedAt=?, completedAt=?, error=?, progress=?, isFolder=?, parent_id=? WHERE id=?");
    query.addBindValue(item.url);
    query.addBindValue(item.filePath);
    query.addBindValue(item.type);
    query.addBindValue(item.downloadedSize);
    query.addBindValue(item.totalSize);
    query.addBindValue(item.status);
    query.addBindValue(item.addedAt.toString(Qt::ISODate));
    query.addBindValue(item.completedAt.toString(Qt::ISODate));
    query.addBindValue(item.error);
    query.addBindValue(item.progress);
    query.addBindValue(item.isFolder ? 1 : 0);
    query.addBindValue(item.parentId);
    query.addBindValue(item.id);

    if (!query.exec()) {
        Logger::instance().error("Update download failed: " + query.lastError().text());
    }
}

void DatabaseManager::updateDownloadProgress(int id, qint64 downloaded, qint64 total, double progress) {
    QSqlQuery query(db);
    query.prepare("UPDATE downloads SET downloadedSize=?, totalSize=?, progress=? WHERE id=?");
    query.addBindValue(downloaded);
    query.addBindValue(total);
    query.addBindValue(progress);
    query.addBindValue(id);
    query.exec();
}

void DatabaseManager::removeDownload(int id) {
    QSqlQuery query(db);
    query.prepare("DELETE FROM downloads WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        Logger::instance().error("Remove download failed: " + query.lastError().text());
    }
}

void DatabaseManager::clearCompleted() {
    QSqlQuery query(db);
    query.exec("DELETE FROM downloads WHERE status='Completed'");
    Logger::instance().info("Cleared completed downloads from database");
}

QVector<DownloadItem> DatabaseManager::getAllDownloads() {
    QVector<DownloadItem> items;
    QSqlQuery query(db);
    query.exec("SELECT * FROM downloads ORDER BY id DESC");

    while (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        item.parentId = query.value("parent_id").toInt();
        items.append(item);
    }

    return items;
}

QVector<DownloadItem> DatabaseManager::getDownloadsByStatus(const QString& status) {
    QVector<DownloadItem> items;
    QSqlQuery query(db);
    query.prepare("SELECT * FROM downloads WHERE status=? ORDER BY id DESC");
    query.addBindValue(status);
    query.exec();

    while (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        item.parentId = query.value("parent_id").toInt();
        items.append(item);
    }

    return items;
}

DownloadItem DatabaseManager::getDownload(int id) {
    QSqlQuery query(db);
    query.prepare("SELECT * FROM downloads WHERE id=?");
    query.addBindValue(id);
    query.exec();

    if (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        item.parentId = query.value("parent_id").toInt();
        return item;
    }

    return DownloadItem();
}

int DatabaseManager::getMaxDownloadId() {
    QSqlQuery query(db);
    query.exec("SELECT MAX(id) FROM downloads");
    if (query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

void DatabaseManager::resetStaleDownloads() {
    QSqlQuery query(db);
    query.exec("UPDATE downloads SET status='Failed' WHERE status IN ('Downloading','Queued','Paused','Resuming')");
    Logger::instance().info("Reset stale downloads to Failed status");
}

void DatabaseManager::saveSetting(const QString& key, const QString& value) {
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        Logger::instance().error("Save setting failed: " + query.lastError().text());
    }
}

QString DatabaseManager::getSetting(const QString& key, const QString& defaultValue) {
    QSqlQuery query(db);
    query.prepare("SELECT value FROM settings WHERE key=?");
    query.addBindValue(key);
    query.exec();

    if (query.next()) {
        return query.value("value").toString();
    }
    return defaultValue;
}

QString DatabaseManager::getUserAgent() {
    QString ua = getSetting("userAgent", "");
    if (ua.trimmed().isEmpty()) {
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 CopperDownloadManager/" + QString::fromLatin1("0.4.5");
    }
    return ua.trimmed();
}

