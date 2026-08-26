#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class QTabWidget;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QSpinBox;

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
    QPlainTextEdit* defaultTrackerEdit;
    QLabel* ytdlpVersionLabel;
    QLabel* ffmpegVersionLabel;
    QLabel* aria2cVersionLabel;
    QLabel* handlerStatusLabel;
    QSpinBox* speedLimitSpin;
};

#endif
