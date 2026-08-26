#ifndef DEFAULTHANDLER_H
#define DEFAULTHANDLER_H

#include <QObject>
#include <QString>

class DefaultHandler : public QObject {
    Q_OBJECT
public:
    static DefaultHandler& instance();
    void registerAsDefault();
    void unregisterAsDefault();
    bool isRegistered();
    void autoUpdateRegistryPath();
    QString getRegisteredProtocol() const;

private:
    DefaultHandler();
};

#endif
