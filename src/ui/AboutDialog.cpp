#include "ui/AboutDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QApplication>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("About Copper Download Manager");
    setFixedSize(420, 380);
    setWindowIcon(QIcon(":/icons/app.png"));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(30, 30, 30, 30);

    QLabel* iconLabel = new QLabel(this);
    iconLabel->setPixmap(QIcon(":/icons/app.png").pixmap(64, 64));
    iconLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(iconLabel);

    QLabel* nameLabel = new QLabel("Copper Download Manager", this);
    nameLabel->setAlignment(Qt::AlignCenter);
    QFont nameFont = nameLabel->font();
    nameFont.setPointSize(16);
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);
    mainLayout->addWidget(nameLabel);

    QLabel* versionLabel = new QLabel("Version " + QApplication::applicationVersion(), this);
    versionLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(versionLabel);

    QLabel* descLabel = new QLabel("A high-performance, cross-platform download manager\nfor Windows, Linux, and macOS.", this);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    mainLayout->addWidget(descLabel);

    mainLayout->addSpacing(10);

    QLabel* devLabel = new QLabel("Developer:", this);
    devLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(devLabel);

    QPushButton* githubBtn = new QPushButton("Mohamed Subarashi", this);
    githubBtn->setStyleSheet("QPushButton { color: #4298e8; text-decoration: underline; border: none; font-size: 13px; }"
                             "QPushButton:hover { color: #6ab0ff; }");
    connect(githubBtn, &QPushButton::clicked, this, &AboutDialog::onGitHubClicked);
    mainLayout->addWidget(githubBtn);

    mainLayout->addSpacing(10);

    QPushButton* kofiBtn = new QPushButton("Support on Ko-fi", this);
    kofiBtn->setStyleSheet("QPushButton { background-color: #FF5E5B; color: white; border: none; border-radius: 6px; padding: 8px 20px; font-size: 13px; font-weight: bold; }"
                           "QPushButton:hover { background-color: #FF7E7B; }");
    kofiBtn->setCursor(Qt::PointingHandCursor);
    connect(kofiBtn, &QPushButton::clicked, this, &AboutDialog::onKoFiClicked);
    mainLayout->addWidget(kofiBtn, 0, Qt::AlignCenter);

    mainLayout->addStretch();

    QPushButton* okBtn = new QPushButton("OK", this);
    okBtn->setFixedWidth(100);
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    mainLayout->addWidget(okBtn, 0, Qt::AlignCenter);
}

void AboutDialog::onGitHubClicked() {
    QDesktopServices::openUrl(QUrl("https://github.com/MohamedSubarashi"));
}

void AboutDialog::onKoFiClicked() {
    QDesktopServices::openUrl(QUrl("https://ko-fi.com/mohamedsubarashi"));
}
