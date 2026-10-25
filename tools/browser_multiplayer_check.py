"""Two real browser clients against a local devmap dedicated server.

Checks: distinct clients, spawn, movement, firing, damage to the other player,
death, respawn, disconnect without hanging the server, and reconnect. Aim and
setviewpos are test fixtures; collision, hit detection and damage run in the
real server. A failing check exits non-zero and must be reported as failing.
Windows-only (default: reads the installed key from the registry and redacts
it from logs). Set COD2_TEST_NO_KEY=1 to test a clean browser with no CD-Key
in its preferences against a project-owned LAN-mode server. No-key mode uses a
fresh browser profile to avoid silently reusing credentials from prior runs.
Set COD2_TEST_SESSION=1 along with NO_KEY to acquire a guest session and a
server-bound join ticket from the project's own /play/api endpoints (the
server directory and --sessions must be enabled for this option).
"""
import json
import os
import sys
from pathlib import Path
import time
import math
import tempfile
from playwright.sync_api import sync_playwright
from browser_mp_redact import sanitize_test_log

from contextlib import contextmanager

@contextmanager
def _existing_profile(root_dir):
    """Preserve the previous persistent test profile for installed-key runs."""
    yield str(root_dir / 'browser-profile')

root = Path(os.environ.get('COD2_TEST_OUTPUT', str(Path(__file__).resolve().parent / '.runtime' / 'multiplayer'))).resolve()
root.mkdir(parents=True, exist_ok=True)
url = os.environ.get('COD2_TEST_URL', 'http://127.0.0.1:8081/cod2.html')
server = os.environ.get('COD2_TEST_SERVER', '127.0.0.1:28961')
no_key = os.environ.get('COD2_TEST_NO_KEY', '').strip().lower() in ('1', 'true', 'yes')
session_mode = os.environ.get('COD2_TEST_SESSION', '').strip().lower() in ('1', 'true', 'yes')
if session_mode and not no_key:
    raise RuntimeError('COD2_TEST_SESSION requires COD2_TEST_NO_KEY=1')
key = None
if not no_key:
    # Never read the installed CD-Key when explicitly testing project-owned
    # servers with anonymous LAN-mode connections.
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\Activision\Call of Duty 2') as registry:
        key = winreg.QueryValueEx(registry, 'codkey')[0].replace('-', '').strip()
    if len(key) != 20:
        raise RuntimeError('Unexpected local game key length')
log = (root / 'multiplayer-test.log').open('w', encoding='utf-8', buffering=1)
def emit(kind, value):
    text = json.dumps(dict(time=time.time(), kind=kind, value=value), ensure_ascii=False)
    log.write(sanitize_test_log(text, key) + '\n')
def state(page):
    return page.evaluate('JSON.parse(Module.ccall("Web_TestClientState", "string", [], []))')
def command(page, text):
    page.evaluate('text => Module.ccall("Cbuf_AddText", null, ["string"], [text + "\\n"])', text)

with tempfile.TemporaryDirectory(prefix='cod2-no-key-profile-', dir=root) if no_key else _existing_profile(root) as profile_dir, sync_playwright() as p:
    # COD2_TEST_BROWSER: path to an installed Chrome/Chromium when Playwright's
    # own browser build is not downloaded (e.g. offline test machines).
    browser_exe = os.environ.get('COD2_TEST_BROWSER') or None
    context = p.chromium.launch_persistent_context(str(profile_dir), headless=True,
        executable_path=browser_exe,
        args=['--enable-unsafe-swiftshader', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'],
        viewport=dict(width=1280, height=800))
    pages = []
    movement_results, fire_results = [], []
    for index in range(2):
        page = context.new_page()
        page.on('console', lambda msg, i=index: emit('console' + str(i), dict(type=msg.type, text=msg.text)))
        page.on('pageerror', lambda err, i=index: emit('error' + str(i), err.stack))
        page.on('websocket', lambda ws, i=index: emit('ws' + str(i), ws.url))
        page.goto(url, wait_until='domcontentloaded')
        page.wait_for_function('typeof Cod2Fs !== "undefined" && typeof Module.callMain === "function"')
        if index == 0:
            page.evaluate('async () => { await Cod2Fs.ingestRemote({manifestUrl:"assets.json",baseUrl:"/"}); }')
            page.reload(wait_until='domcontentloaded')
        page.wait_for_function('!document.getElementById("beginBtn").disabled', timeout=60000)
        if session_mode:
            # A private-session test must be authorized by OUR master and relay.
            # Use a different randomly generated guest identity per browser page.
            # Do not log the guest key, session token or server-bound join ticket.
            admission = page.evaluate("""async data => {
              const keyBytes = crypto.getRandomValues(new Uint8Array(24));
              const playerKey = btoa(String.fromCharCode(...keyBytes))
                .replace(/\\+/g, '-').replace(/\\//g, '_').replace(/=+$/, '');
              async function post(path, payload) {
                const response = await fetch('/play/api/' + path, {
                  method: 'POST', headers: {'Content-Type':'application/json'},
                  body: JSON.stringify(payload)
                });
                if (!response.ok) throw new Error(path + ': HTTP ' + response.status);
                return response.json();
              }
              const session = await post('session', {nick: data.name, playerKey});
              const serversResponse = await fetch('/play/api/servers');
              if (!serversResponse.ok) throw new Error('servers: HTTP ' + serversResponse.status);
              const servers = (await serversResponse.json()).servers || [];
              const target = servers.find(s => s.connect === data.server);
              if (!target) throw new Error('configured server not in project directory: ' + data.server);
              const joined = await post('join', {session: session.token, serverId: target.id});
              if (joined.connect !== data.server || !joined.ticket)
                throw new Error('join did not return the expected server/ticket');
              window.COD2_RELAY_SESSION = session.token;
              window.COD2_RELAY_TICKETS = {[data.server]: joined.ticket};
              Module.COD2_RELAY_SESSION = session.token;
              Module.COD2_RELAY_TICKETS = window.COD2_RELAY_TICKETS;
              return {server: data.server, nick: session.nick, ticketPresent: true};
            }""", dict(name='Browser' + str(index + 1), server=server))
            emit('admission' + str(index), admission)
        page.evaluate('data => {const orig = Module.callMain; Module.callMain = function(args) {FS.mkdirTree("/cod2home/.cod2"); if (data.key) FS.writeFile("/cod2home/.cod2/preferences", "codkey="+data.key+"\\n"); return orig(args.concat(["+set","name",data.name,"+set","net_lanauthorize","0","+connect",data.server]));};}', dict(key=key, name='Browser' + str(index + 1), server=server))
        page.locator('#beginBtn').click()
        for attempt in range(50):
            page.wait_for_timeout(1000)
            current = state(page)
            emit('state' + str(index), current)
            if current.get('state') == 8:
                break
        page.locator('#consoleToggleBtn').click()
        page.screenshot(path=str(root / ('player' + str(index + 1) + '-connected.png')))
        print('Client', index + 1, current, flush=True)
        pages.append(page)
    for index, page in enumerate(pages):
        page.bring_to_front()
        page.keyboard.press('Enter')
        page.wait_for_timeout(1500)
        page.screenshot(path=str(root / ('player' + str(index + 1) + '-enter.png')))
        emit('after_enter' + str(index), state(page))
        page.keyboard.press('2' if index == 0 else '3')
        page.wait_for_timeout(1000)
        page.screenshot(path=str(root / ('player' + str(index + 1) + '-weapon-menu.png')))
        page.keyboard.press('1')
        page.wait_for_timeout(3000)
        page.screenshot(path=str(root / ('player' + str(index + 1) + '-spawned.png')))
        emit('spawned' + str(index), state(page))
    for index, page in enumerate(pages):
        page.bring_to_front()
        before = state(page)
        page.keyboard.down('w')
        page.wait_for_timeout(1500)
        page.keyboard.up('w')
        page.wait_for_timeout(500)
        movement_result = dict(before=before, after=state(page))
        movement_results.append(movement_result)
        emit('movement' + str(index), movement_result)
        page.mouse.move(640, 400)
        before = state(page)
        page.mouse.down()
        page.wait_for_timeout(700)
        page.mouse.up()
        page.wait_for_timeout(1000)
        fire_result = dict(before=before, after=state(page))
        fire_results.append(fire_result)
        emit('fire' + str(index), fire_result)
    # Put both clients on a short segment actually traversed by client one.
    # The local server runs devmap solely to permit the setviewpos fixture.
    a = movement_results[0]['before']['origin']
    b = movement_results[0]['after']['origin']
    distance = math.sqrt(sum((b[i]-a[i])**2 for i in range(3)))
    if distance > 80:
        b = [a[i] + (b[i]-a[i])*80/distance for i in range(3)]
    for page, position in zip(pages, [a, b]):
        command(page, 'cmd setviewpos %.3f %.3f %.3f 0' % (position[0], position[1], position[2] + 60))
    pages[0].wait_for_timeout(2000)
    target = state(pages[1])
    shooter = pages[0]
    shooter.bring_to_front()
    shooter.evaluate('p => Module.ccall("Web_TestAimAt", null, ["number","number","number"], [p[0],p[1],p[2]+35])', target['origin'])
    shooter.wait_for_timeout(500)
    duel_before = [state(page) for page in pages]
    command(shooter, '+attack')
    shooter.wait_for_timeout(1500)
    command(shooter, '-attack')
    shooter.wait_for_timeout(800)
    duel_after = [state(page) for page in pages]
    emit('duel', dict(before=duel_before, after=duel_after))

    # ---- Lifecycle: kill, respawn, disconnect, reconnect ------------------
    # Only normal client commands are sent. Health is read from the client's
    # snapshot (server authoritative); nothing here sets health directly.
    victim = pages[1]
    lifecycle = dict(kill=[], respawn=[], alive_during_disconnect=[], reconnect=[])
    died = False
    death_state = None
    deadline = time.time() + 25
    while time.time() < deadline and not died:
        target = state(victim)
        if target.get('health', 100) <= 0:
            died, death_state = True, target
            break
        shooter.evaluate('p => Module.ccall("Web_TestAimAt", null, ["number","number","number"], [p[0],p[1],p[2]+35])', target['origin'])
        if state(shooter).get('clip', 1) == 0:
            command(shooter, '+reload')
            shooter.wait_for_timeout(200)
            command(shooter, '-reload')
            shooter.wait_for_timeout(2500)
        command(shooter, '+attack')
        shooter.wait_for_timeout(600)
        command(shooter, '-attack')
        shooter.wait_for_timeout(300)
        sample = state(victim)
        lifecycle['kill'].append(dict(health=sample.get('health'), pmType=sample.get('pmType'),
                                      shooterClip=state(shooter).get('clip')))
        if sample.get('health', 100) <= 0:
            died, death_state = True, sample
    emit('death', dict(died=died, state=death_state))

    respawned = False
    respawn_state = None
    if died:
        victim.bring_to_front()
        victim.wait_for_timeout(3000)
        deadline = time.time() + 25
        while time.time() < deadline:
            command(victim, '+attack')     # DM: fire to respawn after the death delay
            victim.wait_for_timeout(300)
            command(victim, '-attack')
            victim.wait_for_timeout(1200)
            sample = state(victim)
            lifecycle['respawn'].append(dict(health=sample.get('health'), pmType=sample.get('pmType'),
                                             origin=sample.get('origin')))
            # Health was <= 0; full health again means a new spawn.
            if sample.get('health', 0) >= 100:
                respawned, respawn_state = True, sample
                break
    emit('respawn', dict(respawned=respawned, state=respawn_state))

    # Disconnect the victim; the remaining client must keep receiving
    # advancing snapshots (the earlier bug hung the server on unlink).
    command(victim, 'disconnect')
    alive = True
    last = state(shooter)['snapshotTime']
    for _ in range(6):
        shooter.wait_for_timeout(1000)
        now = state(shooter)
        lifecycle['alive_during_disconnect'].append(now['snapshotTime'])
        if now['snapshotTime'] <= last:
            alive = False
        last = now['snapshotTime']
    emit('disconnect', dict(server_alive=alive))

    reconnected = False
    command(victim, 'connect ' + server)
    for attempt in range(40):
        victim.wait_for_timeout(1000)
        sample = state(victim)
        lifecycle['reconnect'].append(sample.get('state'))
        if sample.get('state') == 8:
            reconnected = True
            break
    shooter_after = state(shooter)
    still_alive_after_reconnect = shooter_after['snapshotTime'] > last
    emit('reconnect', dict(reconnected=reconnected, shooter=shooter_after))

    checks = {
        'two_distinct_clients': duel_before[0]['client'] != duel_before[1]['client'],
        'both_spawned': all(s['health'] > 0 and s['weapon'] > 0 for s in duel_before),
        'both_move': all(sum((r['after']['origin'][i]-r['before']['origin'][i])**2 for i in range(2)) > 1 and r['after']['snapshotTime'] > r['before']['snapshotTime'] for r in movement_results),
        'both_fire': all(r['after']['clip'] < r['before']['clip'] for r in fire_results),
        'damage_to_other_player': duel_after[1]['health'] < duel_before[1]['health'],
        'target_died': died,
        'target_respawned': respawned,
        'server_alive_after_disconnect': alive,
        'reconnected': reconnected,
        'server_alive_after_reconnect': still_alive_after_reconnect,
    }
    emit('checks', checks)
    (root / 'multiplayer-result.json').write_text(json.dumps(dict(key_mode='none' if no_key else 'installed-key',
                                                      admission_mode='project-session' if session_mode else 'direct-LAN',
                                                      checks=checks, movements=movement_results, fire=fire_results, duel=dict(before=duel_before, after=duel_after), lifecycle=lifecycle), indent=2))
    print('Checks', checks, flush=True)
    for index, page in enumerate(pages):
        emit('final' + str(index), state(page))
        page.screenshot(path=str(root / ('player' + str(index + 1) + '-final.png')))
        command(page, 'disconnect')
        page.wait_for_timeout(500)
    context.close()

if not all(checks.values()):
    sys.exit(1)
