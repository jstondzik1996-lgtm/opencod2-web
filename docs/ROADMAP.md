# WWII Browser Multiplayer — roadmap

Working name only; verify name and all component licences before any public launch.
Status words: **done** = verified as described, **code** = implemented and unit/cloud tested but not on the real stack, **todo**.

## Default match (decision 2026-10-26)

**1 real browser player + 15 server-controlled bots, difficulty `normal`** (16 participants total). This is a **target requirement, not currently implemented**. The first playable gate is 1+1 with genuine movement/damage/death/respawn, then 1+4, 1+8, and the full 1+15 with measured FPS and server tick health. Humans joining later displace bots without displacing humans.

See **[Bot match requirements](BOT_MATCH_REQUIREMENTS.md)** for the config contract, AI/controller architecture, existing QA maxPlayers=4 discrepancy, performance/security rules and PASS/FAIL acceptance tests.

## Tracks

**A. CoD2 1.3 compatible port.** The existing client and reconstructed dedicated server, running on original game files each player supplies locally (folder picker / browser storage). Original IWDs, models, sounds, textures, keys and Steam files are never published or served by project hosts. Used for engine diagnostics and private testing.

**B. Standalone public version.** Same engine, networking, lobby and servers, but with original or properly licensed maps, weapons, models, UI, sounds, textures and branding, so it can be offered publicly. Do not recreate recognisable protected elements under a new name.

Shared code is the point: gameplay, netcode, server directory, matchmaking and identity must not depend on which asset set is loaded.

## Priorities

| ID | Item | Status |
|----|------|--------|
| P0 | Two clients, movement, firing | done (earlier run) |
| P0 | Bullet hits player, HP drops, death, respawn, disconnect/reconnect without server hang | code: collision fix + extended harness; needs Work's real run |
| P1 | Project server directory + `GET /play/api/servers` | code |
| P1 | Quick Play `POST /play/api/matchmake` with reservations, full -> next server | code |
| P1 | Launcher lobby (Quick Play, list, join, full-server retry) | code |
| P1 | Two+ dedicated instances locally, A full -> B | todo (Work, steps in HANDOFF) |
| P1 | Private rooms (password or invite code, hidden from Quick Play) | todo; directory already supports `private` entries |
| P1 | Load tests 4 -> 8 -> 16 -> 32 players (server CPU, bandwidth, snapshot loss, client FPS) | todo; 32 is a target, not a measured result |
| P2 | Own player identity on project servers (guest nick + signed short-lived session token; bans/kicks/rate limits) | todo; design next |
| P3 | Public hosting hardening: WSS/TLS, relay limited to directory servers (done in code), per-IP limits, bandwidth caps, abuse handling, no `COD2_RELAY_ALLOW_ANY` | partial |
| B1 | Asset abstraction: list every asset type the client/server loads (maps/BSP, xmodels, materials, sounds, menus, scripts, weapon files, localized strings) and the code paths that load them | todo |
| B2 | Minimal original content set: one small map, one weapon, player model, HUD, sounds, under a clear licence | todo |
| B3 | Asset pack format/manifest independent of IWD names; launcher loads pack A (user files) or pack B (public) | todo |

## P2 design constraints (to be detailed)

- Applies only to servers the project controls. The client must know whether it joins a project server or an external one.
- No CD key in JavaScript, logs, WebSocket traffic or the master list; no shared key for many players; no imitation of the publisher's authorization service.
- Server side: our backend issues a token bound to nick, a random persistent player id and the target server; the dedicated server validates it at connect (replacing, for project servers only, the external key check) and the backend handles bans and rate limits.
- Acceptance: two fresh browser profiles join without typing a key, get distinct identities, disconnect/reconnect, and a ban applies.
