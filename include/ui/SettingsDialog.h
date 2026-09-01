#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QVector>
#include <QHash>

class QTabWidget;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QSpinBox;
class QCheckBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(QWidget* parent = nullptr);
    QString getDownloadPath() const;

private slots:
    void onSave();
    void onUpdateYtDlp();
    void onUpdateFfmpeg();
    void onUpdateAria2c();
    void onCheckForUpdates();
    void onUpdateReady(const QString& version);
    void onUpdateDownloaded();
    void onUpdateMessage(const QString& error);
    void onRegisterDefaultHandler();
    void onUnregisterDefaultHandler();
    void onBrowseDownloadPath();
    void onAddDefaultTracker();
    void onClearDefaultTrackers();
    void accept() override;

private:
    void updateStartupRegistry(bool enabled);
    QTabWidget* tabWidget;
    QLineEdit* downloadPathEdit;
    QComboBox* chunkCombo;
    QHash<QString, QComboBox*> typeFilterModeCombos;
    QPlainTextEdit* defaultTrackerEdit;
    QLabel* ytdlpVersionLabel;
    QLabel* ffmpegVersionLabel;
    QLabel* aria2cVersionLabel;
    QLabel* handlerStatusLabel;
    QSpinBox* speedLimitSpin;
    QComboBox* seedTimeCombo;
    QLineEdit* userAgentEdit;
};

#endif
