#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "utils/UserAgent.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>

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
               "parent_id INTEGER DEFAULT -1, "
               "audio_format TEXT DEFAULT '', "
               "attempts INTEGER DEFAULT 0, "
               "video_id TEXT DEFAULT '', "
               "track_index INTEGER DEFAULT 0, "
               "selected_items TEXT DEFAULT '', "
               "track_numbers INTEGER DEFAULT 1"
               ")");

    query.exec("CREATE TABLE IF NOT EXISTS settings ("
               "key TEXT PRIMARY KEY, "
               "value TEXT"
               ")");

    const int schemaVersion = getSchemaVersion();
    if (schemaVersion > 0 && schemaVersion < 3) {
        // Keep a pre-migration snapshot: if a migration is botched or the new
        // schema turns out to be wrong, the old database is one copy away
        // instead of gone. VACUUM INTO works on the open database.
        QSqlQuery backup(db);
        const QString backupPath = dbPath + ".pre-migration-v" + QString::number(schemaVersion);
        QFile::remove(backupPath);
        if (!backup.exec("VACUUM INTO '" + backupPath + "'")) {
            Logger::instance().warning("Pre-migration backup failed: " + backup.lastError().text());
        }
    }
    migrate(schemaVersion);

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
    const int targetVersion = 3;
    if (from >= targetVersion) return;

    // Every migration step runs in one transaction: a crash or a failed
    // statement halfway through must leave the old schema fully intact
    // instead of a half-migrated database.
    const bool inTransaction = db.transaction();
    auto finishMigration = [this, inTransaction](bool ok) {
        if (!inTransaction) return;
        if (ok) db.commit();
        else db.rollback();
    };

    QSqlQuery query(db);

    // Column presence check shared by the migrations below: a database created by
    // a newer build (or a partially migrated one) must never get a duplicate
    // column, so every ALTER is guarded by a PRAGMA table_info lookup.
    auto hasColumn = [this](const QString& name) {
        QSqlQuery cols(db);
        if (!cols.exec("PRAGMA table_info(downloads)")) return true; // can't tell: assume present
        while (cols.next()) {
            if (cols.value("name").toString() == name) return true;
        }
        return false;
    };

    // A pre-0.3.x database may predate the parent_id column (playlist / torrent
    // folder children). Add it defensively so inserts never fail after upgrade.
    if (from < 1) {
        if (!hasColumn("parent_id")) {
            if (!query.exec("ALTER TABLE downloads ADD COLUMN parent_id INTEGER DEFAULT -1")) {
                Logger::instance().error("Migration to v1 failed: " + query.lastError().text());
                finishMigration(false);
                return;
            }
        }
        from = 1;
    }

    // audio_format + attempts: without the requested output format a resumed
    // yt-dlp/playlist transfer would restart as MP4 after an app restart (a
    // "resume" that silently downloads the wrong thing), and the automatic retry
    // policy needs its attempt counter to survive a restart.
    if (from < 2) {
        bool ok = true;
        if (!hasColumn("audio_format")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN audio_format TEXT DEFAULT ''") && ok;
        }
        if (!hasColumn("attempts")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN attempts INTEGER DEFAULT 0") && ok;
        }
        // Needed to attribute a single playlist process's output to its rows again
        // after a restart.
        if (!hasColumn("video_id")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN video_id TEXT DEFAULT ''") && ok;
        }
        if (!hasColumn("track_index")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN track_index INTEGER DEFAULT 0") && ok;
        }
        if (!ok) {
            Logger::instance().error("Migration to v2 failed: " + query.lastError().text());
            finishMigration(false);
            return;
        }
        from = 2;
    }

    // The user's selection is a property of the JOB, not of the rows that happen
    // to exist for it: a row that loses its playlist position must never widen the
    // download back to the whole playlist, and the "add track numbers" choice has
    // to survive a restart (it changes the file name yt-dlp is told to write).
    if (from < 3) {
        bool ok = true;
        if (!hasColumn("selected_items")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN selected_items TEXT DEFAULT ''") && ok;
        }
        if (!hasColumn("track_numbers")) {
            ok = query.exec("ALTER TABLE downloads ADD COLUMN track_numbers INTEGER DEFAULT 1") && ok;
        }
        if (!ok) {
            Logger::instance().error("Migration to v3 failed: " + query.lastError().text());
            finishMigration(false);
            return;
        }
        from = 3;
    }

    if (from < targetVersion) {
        Logger::instance().info("Database schema migrating from " + QString::number(from) + " to " + QString::number(targetVersion));
        from = targetVersion;
    }

    query.exec("PRAGMA user_version = " + QString::number(from));
    finishMigration(true);
}

// The job's selection is stored as a comma-separated list of playlist positions.
// Keeping it on the job (instead of deriving it from the rows on every start) is
// what makes "download only what was picked" survive restarts and later edits.
static QString encodeSelection(const QVector<int>& indices) {
    QStringList parts;
    for (int idx : indices) parts << QString::number(idx);
    return parts.join(",");
}

static QVector<int> decodeSelection(const QString& text) {
    QVector<int> out;
    for (const QString& part : text.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const int value = part.trimmed().toInt(&ok);
        if (ok && value > 0 && !out.contains(value)) out.append(value);
    }
    return out;
}

void DatabaseManager::addDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("INSERT INTO downloads (id, url, filePath, type, downloadedSize, totalSize, status, addedAt, completedAt, error, progress, isFolder, parent_id, audio_format, attempts, video_id, track_index, selected_items, track_numbers) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
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
    query.addBindValue(item.audioFormat);
    query.addBindValue(item.attempts);
    query.addBindValue(item.videoId);
    query.addBindValue(item.trackIndex);
    query.addBindValue(encodeSelection(item.selectedIndices));
    query.addBindValue(item.trackNumbers ? 1 : 0);

    if (!query.exec()) {
        Logger::instance().error("Add download failed: " + query.lastError().text());
    }
}

void DatabaseManager::updateDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("UPDATE downloads SET url=?, filePath=?, type=?, downloadedSize=?, totalSize=?, status=?, "
                  "addedAt=?, completedAt=?, error=?, progress=?, isFolder=?, parent_id=?, audio_format=?, "
                  "attempts=?, video_id=?, track_index=?, selected_items=?, track_numbers=? WHERE id=?");
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
    query.addBindValue(item.audioFormat);
    query.addBindValue(item.attempts);
    query.addBindValue(item.videoId);
    query.addBindValue(item.trackIndex);
    query.addBindValue(encodeSelection(item.selectedIndices));
    query.addBindValue(item.trackNumbers ? 1 : 0);
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
        item.audioFormat = query.value("audio_format").toString();
        item.attempts = query.value("attempts").toInt();
        item.videoId = query.value("video_id").toString();
        item.trackIndex = query.value("track_index").toInt();
        item.selectedIndices = decodeSelection(query.value("selected_items").toString());
        item.trackNumbers = query.value("track_numbers").isValid() ? query.value("track_numbers").toBool() : true;
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
        item.audioFormat = query.value("audio_format").toString();
        item.attempts = query.value("attempts").toInt();
        item.videoId = query.value("video_id").toString();
        item.trackIndex = query.value("track_index").toInt();
        item.selectedIndices = decodeSelection(query.value("selected_items").toString());
        item.trackNumbers = query.value("track_numbers").isValid() ? query.value("track_numbers").toBool() : true;
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
        item.audioFormat = query.value("audio_format").toString();
        item.attempts = query.value("attempts").toInt();
        item.videoId = query.value("video_id").toString();
        item.trackIndex = query.value("track_index").toInt();
        item.selectedIndices = decodeSelection(query.value("selected_items").toString());
        item.trackNumbers = query.value("track_numbers").isValid() ? query.value("track_numbers").toBool() : true;
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
        // Default browser-like UA; the version part derives from the
        // single-source application version (see utils/UserAgent.h).
        return Copper::copperUserAgent();
    }
    ua = ua.trimmed();
    // A stored value that carries one of our own tokens with a stale version
    // (e.g. the old "... CopperDownloadManager/0.3.1" default or the bare
    // "CopperDownloadManager/1.0") is refreshed to the current single-source
    // UA; anything else is a deliberate custom UA and is kept.
    if (ua.contains(QLatin1String("CopperDownloadManager/"), Qt::CaseInsensitive)
        && !ua.endsWith(QCoreApplication::applicationVersion())) {
        return Copper::copperUserAgent();
    }
    return ua;
}

