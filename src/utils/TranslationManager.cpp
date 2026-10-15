#include "utils/TranslationManager.h"
#include "utils/Logger.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

TranslationManager& TranslationManager::instance() {
    static TranslationManager manager;
    return manager;
}

TranslationManager::TranslationManager() = default;

QList<TranslationManager::Language> TranslationManager::availableLanguages() {
    // Native names are deliberately NOT translated: a language picker should
    // read in its own language so users can find theirs even in a UI they
    // cannot read yet.
    return {
        {QStringLiteral("system"), QStringLiteral("Auto (system)")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("fr"), QStringLiteral("Français")},
        {QStringLiteral("ar"), QStringLiteral("العربية")},
        {QStringLiteral("es"), QStringLiteral("Español")},
        {QStringLiteral("pt_BR"), QStringLiteral("Português (Brasil)")},
    };
}

QString TranslationManager::languageDisplayName(const QString& code) {
    for (const Language& language : availableLanguages()) {
        if (language.code == code) return language.native;
    }
    return code;
}

QString TranslationManager::resolveLanguageCode(const QString& setting) {
    if (setting.isEmpty() || setting == QLatin1String("system")) {
        const QString systemName = QLocale::system().name(); // e.g. "pt_BR", "fr_FR"
        // Exact match first (region-aware), then language-only.
        for (const Language& language : availableLanguages()) {
            if (language.code != QLatin1String("system") &&
                language.code.compare(systemName, Qt::CaseInsensitive) == 0) {
                return language.code;
            }
        }
        const QString systemLanguage = systemName.left(2).toLower();
        for (const Language& language : availableLanguages()) {
            if (language.code != QLatin1String("system") &&
                language.code.left(2).toLower() == systemLanguage) {
                return language.code;
            }
        }
        return QStringLiteral("en");
    }
    return setting;
}

void TranslationManager::install(const QString& setting) {
    const QString code = resolveLanguageCode(setting);
    Logger::instance().info("Language: setting='" + setting + "' resolved='" + code + "'");

    // Mirror the whole UI for right-to-left locales (Arabic today).
    const bool rightToLeft = code.startsWith(QLatin1String("ar"));
    qApp->setLayoutDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);

    if (!appTranslator) appTranslator = new QTranslator(this);
    if (!qtTranslator) qtTranslator = new QTranslator(this);

    // Our own strings, embedded in the executable by the build.
    const QString appPath = QStringLiteral(":/translations/copper_%1.qm").arg(code);
    if (appTranslator->load(appPath)) {
        qApp->installTranslator(appTranslator);
        Logger::instance().info("Loaded app translation: " + appPath);
    } else if (code != QLatin1String("en")) {
        // English is the source language, so a missing en catalog is expected.
        Logger::instance().warning("App translation not found: " + appPath);
    }

    // Qt's own dialogs (standard buttons, QFileDialog, ...) come from the
    // deployed Qt translations. Try the deployed directory first, then Qt's
    // install location; missing files are not fatal (English fallback).
    const QString qtBase = QStringLiteral("qtbase_%1").arg(code);
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/translations/") + qtBase + QStringLiteral(".qm"),
        appDir + QStringLiteral("/../Resources/translations/") + qtBase + QStringLiteral(".qm"), // macOS bundle
        QLibraryInfo::path(QLibraryInfo::TranslationsPath) + QStringLiteral("/") + qtBase + QStringLiteral(".qm"),
    };
    for (const QString& candidate : candidates) {
        if (qtTranslator->load(candidate)) {
            qApp->installTranslator(qtTranslator);
            Logger::instance().info("Loaded Qt translation: " + candidate);
            break;
        }
    }
}
