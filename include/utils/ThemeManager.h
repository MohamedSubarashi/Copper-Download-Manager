#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QString>
#include <QPalette>

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();
    enum Theme { Light, Dark, System };
    void applyTheme(Theme theme);
    QPalette getPalette(Theme theme) const;
    Theme stringToTheme(const QString& str) const;
    QString themeToString(Theme theme) const;
    Theme getCurrentTheme() const;

private:
    ThemeManager();
    Theme detectSystemTheme() const;

    Theme currentTheme;
};

#endif
