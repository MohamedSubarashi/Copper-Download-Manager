#ifndef APITOKEN_H
#define APITOKEN_H

#include <QString>

// Install-scoped bearer token for the localhost HTTP API (0.4.0 hardening).
//
// The local API accepts connections from any process on the machine, so
// before 0.4.0 anything running as the user could list/queue/forward
// downloads without consent. Every non-OPTIONS request must now carry
// "X-Copper-Token: <token>". The token is a 64-hex secret generated once per
// install and persisted in copper_host_config.json - the same shared config
// the native messaging host reads, which is how the browser extension obtains
// it (native-messaging action "getToken") without the secret ever crossing
// the network. A web page can never read that file (same-origin policy), so
// it can never authenticate.
class ApiToken {
public:
    // The install's token: loaded from the host config, generated and
    // persisted on first use. Cached for the process lifetime.
    static QString token();

    // Constant-time check of a presented token against the current one. On a
    // mismatch the persisted config is re-read once, so a token rewritten by
    // another process (profile restore, config repair) is picked up instead of
    // permanently rejecting every client.
    static bool matches(const QString& provided);

private:
    static bool loadFromDisk();
    static bool persist(const QString& value);
};

#endif
