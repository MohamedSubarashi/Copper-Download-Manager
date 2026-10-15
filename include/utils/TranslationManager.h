#ifndef TRANSLATIONMANAGER_H
#define TRANSLATIONMANAGER_H

#include <QObject>
#include <QString>
#include <QList>

class QTranslator;

// Central i18n helper for the desktop app.
//
// The chosen language is persisted in the app database under the "language"
// key. It is either "system" (follow the operating-system locale, falling back
// to English when the locale is not one we ship) or one of the supported locale
// codes: "en", "fr", "ar", "es", "pt_BR".
//
// Translations themselves are compiled into the executable as Qt resources
// (":/translations/copper_<code>.qm"), so every packaging format ships them
// without a separate deploy step.
class TranslationManager : public QObject {
    Q_OBJECT
public:
    struct Language {
        QString code;    // value persisted in settings: "system", "en", ...
        QString native;  // endonym shown in the picker: "English", "Français", ...
    };

    static TranslationManager& instance();

    // The languages offered in Settings > General.
    static QList<Language> availableLanguages();

    // "system" -> best match for QLocale::system() (or "en"). Otherwise the
    // stored code is returned unchanged.
    static QString resolveLanguageCode(const QString& setting);

    // Endonym for a code ("Auto (system)" for "system").
    static QString languageDisplayName(const QString& code);

    // Installs the app + Qt translators for `setting` and applies the text
    // layout direction (right-to-left for Arabic). Call once, after the
    // QApplication exists and before any window is constructed.
    void install(const QString& setting);

private:
    TranslationManager();
    QTranslator* appTranslator = nullptr;
    QTranslator* qtTranslator = nullptr;
};

#endif // TRANSLATIONMANAGER_H
