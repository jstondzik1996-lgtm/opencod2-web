/*
 * Launcher lobby -> engine bridge.
 *
 * The browser lobby (src/web/lobby.js) picks one of the project's own servers
 * through /play/api/matchmake. At boot it passes "+connect ip:port"; when the
 * engine is already running (e.g. Quick Play retrying after "Server is full")
 * it calls this export instead.
 *
 * Only a literal "a.b.c.d:port" is accepted, so the page cannot inject other
 * console commands through this entry point. The engine's connect still goes
 * through the WebSocket relay, which only forwards to directory servers.
 */
#include <emscripten.h>
#include <stdio.h>

extern void Cbuf_AddText(const char *text);

static int Web_ParseIPv4Port(const char *s)
{
    int octets = 0;
    int value = -1;
    int port = 0;
    int digits = 0;

    if (!s)
        return 0;
    for (; *s && *s != ':'; ++s) {
        if (*s >= '0' && *s <= '9') {
            value = (value < 0 ? 0 : value) * 10 + (*s - '0');
            if (value > 255 || ++digits > 3)
                return 0;
        } else if (*s == '.' && value >= 0 && octets < 3) {
            ++octets;
            value = -1;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (octets != 3 || value < 0 || *s != ':')
        return 0;
    for (++s, digits = 0; *s; ++s) {
        if (*s < '0' || *s > '9' || ++digits > 5)
            return 0;
        port = port * 10 + (*s - '0');
    }
    return digits > 0 && port > 0 && port < 65536;
}

EMSCRIPTEN_KEEPALIVE int Web_LobbyConnect(const char *address)
{
    char cmd[64];

    if (!Web_ParseIPv4Port(address))
        return 0;
    /*
     * The lobby only passes directory-approved targets. On subsequent
     * joins/retries the engine is already running and boot-time +set args
     * no longer apply; set LAN authorization before the connect command.
     * The relay validates the project's guest session and server ticket.
     * Stock/external servers are never admitted through this endpoint.
     */
    snprintf(cmd, sizeof(cmd), "set net_lanauthorize 0\nconnect %s\n", address);
    Cbuf_AddText(cmd);
    return 1;
}
