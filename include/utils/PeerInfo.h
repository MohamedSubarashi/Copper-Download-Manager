#ifndef PEERINFO_H
#define PEERINFO_H

#include <QString>

struct PeerInfo {
    QString ip;
    int port = 0;
    bool seeder = false;
    bool amChoking = false;
    bool peerChoking = false;
    qint64 downloadSpeed = 0;
    qint64 uploadSpeed = 0;
    QString peerId;
    QString connectedVia;   // e.g. "IPv4"
};

#endif
