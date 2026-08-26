#ifndef ADDURLDIALOG_H
#define ADDURLDIALOG_H

#include <QDialog>

class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;

class AddUrlDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddUrlDialog(QWidget* parent = nullptr);
    void setUrl(const QString& url);

private slots:
    void onAdd();
    void onPaste();
    void onTypeChanged(int index);
    void onBrowse();
    void validateUrl();

private:
    QLineEdit* urlEdit;
    QComboBox* typeCombo;
    QComboBox* formatCombo;
    QLineEdit* pathEdit;
    QLabel* statusLabel;
    QLabel* typeInfoLabel;
    QPushButton* addBtn;
};

#endif
