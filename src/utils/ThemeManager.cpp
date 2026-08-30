#include "utils/ThemeManager.h"
#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QStyle>
#ifdef PLATFORM_WINDOWS
#include <QSettings>
#endif

ThemeManager::ThemeManager() : currentTheme(System) {}

ThemeManager& ThemeManager::instance() {
    static ThemeManager instance;
    return instance;
}

void ThemeManager::applyTheme(Theme theme) {
    currentTheme = theme;

    Theme actualTheme = theme;
    if (theme == System) {
        actualTheme = detectSystemTheme();
    }

    qApp->setPalette(getPalette(actualTheme));

    if (actualTheme == Dark) {
        qApp->setStyle(QStyleFactory::create("Fusion"));
    } else {
#ifdef PLATFORM_WINDOWS
        qApp->setStyle(QStyleFactory::create("windowsvista"));
#else
        qApp->setStyle(QStyleFactory::create("Fusion"));
#endif
    }
}

QPalette ThemeManager::getPalette(Theme theme) const {
    QPalette palette;

    if (theme == Dark) {
        palette.setColor(QPalette::Window, QColor(45, 45, 48));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Base, QColor(30, 30, 30));
        palette.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
        palette.setColor(QPalette::ToolTipBase, QColor(50, 50, 50));
        palette.setColor(QPalette::ToolTipText, Qt::white);
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Button, QColor(50, 50, 50));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(42, 130, 218));
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::black);
        palette.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(128, 128, 128));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 128, 128));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 128, 128));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(42, 130, 218));
    } else {
        palette.setColor(QPalette::Window, QColor(240, 240, 240));
        palette.setColor(QPalette::WindowText, Qt::black);
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
        palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
        palette.setColor(QPalette::ToolTipText, Qt::black);
        palette.setColor(QPalette::Text, Qt::black);
        palette.setColor(QPalette::Button, QColor(240, 240, 240));
        palette.setColor(QPalette::ButtonText, Qt::black);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(0, 0, 255));
        palette.setColor(QPalette::Highlight, QColor(0, 120, 215));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
    }

    return palette;
}

ThemeManager::Theme ThemeManager::stringToTheme(const QString& str) const {
    if (str == "Light") return Light;
    if (str == "Dark") return Dark;
    return System;
}

QString ThemeManager::themeToString(Theme theme) const {
    switch (theme) {
        case Light: return "Light";
        case Dark: return "Dark";
        default: return "System";
    }
}

ThemeManager::Theme ThemeManager::getCurrentTheme() const {
    return currentTheme;
}

ThemeManager::Theme ThemeManager::detectSystemTheme() const {
#ifdef PLATFORM_WINDOWS
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat);
    int value = settings.value("AppsUseLightTheme", 1).toInt();
    return (value == 0) ? Dark : Light;
#else
    return Light;
#endif
}
