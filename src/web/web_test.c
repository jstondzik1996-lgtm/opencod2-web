/* Included only with COD2_WEB_TESTING. Read-only state for local smoke tests. */
#include "common_types.h"
#include "imports.h"
#include <emscripten.h>
#include <stdio.h>
#include <math.h>
extern int BG_ClipForWeapon(int weapon);

EMSCRIPTEN_KEEPALIVE const char *Web_TestClientState(void)
{
    static char json[1024];
    clientConnection_t *conn = *(clientConnection_t **)imp_clc;
    clientActive_t *active = *(clientActive_t **)imp_cl;
    if (!conn || !active)
        return "{\"ready\":false}";
    playerState_t *ps = &active->snap.ps;
    int weapon = ps->weapon;
    int clipIndex = weapon > 0 && weapon < 128 ? BG_ClipForWeapon(weapon) : -1;
    int clip = clipIndex >= 0 && clipIndex < 128 ? ps->ammoclip[clipIndex] : -1;
    snprintf(json, sizeof(json),
             "{\"ready\":true,\"state\":%d,\"client\":%d,\"serverTime\":%d,"
             "\"snapshotTime\":%d,\"commandTime\":%d,\"pmType\":%d,"
             "\"origin\":[%.3f,%.3f,%.3f],\"velocity\":[%.3f,%.3f,%.3f],"
             "\"weapon\":%d,\"clip\":%d,\"health\":%d,\"keyCatchers\":%d,\"psClient\":%d,\"entities\":%d,"
             "\"viewAngles\":[%.3f,%.3f,%.3f],\"inputAngles\":[%.3f,%.3f,%.3f],\"viewHeight\":%.3f}",
             conn->state, conn->clientNum, active->serverTime, active->snap.serverTime,
             ps->commandTime, ps->pm_type,
             ps->origin[0], ps->origin[1], ps->origin[2],
             ps->velocity[0], ps->velocity[1], ps->velocity[2],
             weapon, clip, ps->stats[0], active->keyCatchers, ps->clientNum, active->snap.numEntities,
             ps->viewangles[0],ps->viewangles[1],ps->viewangles[2],
             active->viewangles[0],active->viewangles[1],active->viewangles[2],ps->viewHeightCurrent);
    return json;
}

/* Aim the test bot through the normal client user-command path. Damage,
 * collision, ammunition and hit detection remain authoritative on the server. */
EMSCRIPTEN_KEEPALIVE void Web_TestAimAt(float x, float y, float z)
{
    clientActive_t *active = *(clientActive_t **)imp_cl;
    playerState_t *ps = &active->snap.ps;
    float dx = x - ps->origin[0], dy = y - ps->origin[1];
    float dz = z - ps->origin[2] - ps->viewHeightCurrent;
    active->viewangles[0] = -atan2f(dz, sqrtf(dx*dx + dy*dy)) * 57.2957795f
                          - ps->delta_angles[0] * (360.0f/65536.0f);
    active->viewangles[1] = atan2f(dy, dx) * 57.2957795f
                          - ps->delta_angles[1] * (360.0f/65536.0f);
}
