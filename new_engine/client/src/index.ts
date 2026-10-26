import * as pc from 'playcanvas';
import { Client, Room } from 'colyseus.js';

const canvas = document.getElementById('application-canvas') as HTMLCanvasElement;
const hpElem = document.getElementById('hud') as HTMLDivElement;

const app = new pc.Application(canvas, {
    elementInput: new pc.ElementInput(canvas),
    mouse: new pc.Mouse(canvas),
    keyboard: new pc.Keyboard(window),
});

app.setCanvasFillMode(pc.FILLMODE_FILL_WINDOW);
app.setCanvasResolution(pc.RESOLUTION_AUTO);

window.addEventListener('resize', () => app.resizeCanvas());

// Setup scene
app.scene.ambientLight = new pc.Color(0.5, 0.5, 0.5);

const light = new pc.Entity('light');
light.addComponent('light', {
    type: 'directional',
    color: new pc.Color(1, 1, 1),
    castShadows: false, // Turned off for performance in headless testing
    intensity: 1,
});
light.setLocalEulerAngles(45, 30, 0);
app.root.addChild(light);

// Ground
const groundMaterial = new pc.StandardMaterial();
groundMaterial.diffuse = new pc.Color(0.2, 0.6, 0.2);
groundMaterial.update();

const ground = new pc.Entity('ground');
ground.addComponent('render', {
    type: 'box',
    material: groundMaterial,
});
ground.setLocalScale(50, 1, 50);
ground.setLocalPosition(0, -0.5, 0);
app.root.addChild(ground);

// Camera
const camera = new pc.Entity('camera');
camera.addComponent('camera', {
    clearColor: new pc.Color(0.5, 0.7, 0.9),
});
app.root.addChild(camera);

let pitch = 0;
let yaw = 0;
let isPointerLocked = false;

// Mouse look
app.mouse.on(pc.EVENT_MOUSEDOWN, (e: pc.MouseEvent) => {
    if (!pc.Mouse.isPointerLocked()) {
        app.mouse.enablePointerLock();
    } else {
        if (e.button === pc.MOUSEBUTTON_LEFT && room) {
            room.send("shoot", { pitch, yaw });
        }
    }
});

app.mouse.on(pc.EVENT_MOUSEMOVE, (e: pc.MouseEvent) => {
    if (pc.Mouse.isPointerLocked()) {
        yaw -= e.dx * 0.2;
        pitch -= e.dy * 0.2;
        pitch = pc.math.clamp(pitch, -85, 85);
    }
});

app.start();

// Networking
const client = new Client('ws://localhost:2567');
let room: Room<any> | undefined;

const players: Map<string, pc.Entity> = new Map();

const playerMaterial = new pc.StandardMaterial();
playerMaterial.diffuse = new pc.Color(0.8, 0.2, 0.2);
playerMaterial.update();

const botMaterial = new pc.StandardMaterial();
botMaterial.diffuse = new pc.Color(0.2, 0.2, 0.8);
botMaterial.update();

client.joinOrCreate("game_room", { name: "Player1", bots: 15 }).then(r => {
    room = r;
    console.log("Joined successfully!");

    room.state.players.onAdd((playerState: any, id: string) => {
        if (id === room?.sessionId) {
            // Local player
            playerState.onChange(() => {
                // Update camera position to follow physics body smoothly
                camera.setPosition(playerState.x, playerState.y + 1.6, playerState.z);
                camera.setEulerAngles(pitch, yaw, 0);

                if (hpElem) hpElem.innerText = `HP: ${playerState.health}`;
                if (playerState.isDead && hpElem) {
                    hpElem.innerText = `DEAD`;
                }
            });
        } else {
            // Remote player / bot
            const p = new pc.Entity(`player_${id}`);
            p.addComponent('render', {
                type: 'cylinder',
                material: playerState.isBot ? botMaterial : playerMaterial,
            });
            p.setLocalScale(1, 2, 1);
            app.root.addChild(p);
            players.set(id, p);

            playerState.onChange(() => {
                if (playerState.isDead) {
                    p.enabled = false;
                } else {
                    p.enabled = true;
                    p.setPosition(playerState.x, playerState.y, playerState.z);
                    const rot = new pc.Quat().setFromEulerAngles(0, playerState.yaw * pc.math.RAD_TO_DEG, 0);
                    p.setRotation(rot);
                }
            });
        }
    });

    room.state.players.onRemove((playerState: any, id: string) => {
        const p = players.get(id);
        if (p) {
            p.destroy();
            players.delete(id);
        }
    });
}).catch(e => {
    console.error("JOIN ERROR", e);
});

// Update loop for movement
app.on('update', (dt: number) => {
    if (!room) return;

    let dx = 0;
    let dz = 0;

    if (app.keyboard.isPressed(pc.KEY_W)) dz -= 1;
    if (app.keyboard.isPressed(pc.KEY_S)) dz += 1;
    if (app.keyboard.isPressed(pc.KEY_A)) dx -= 1;
    if (app.keyboard.isPressed(pc.KEY_D)) dx += 1;

    // Transform input to local space based on yaw
    if (dx !== 0 || dz !== 0) {
        const rad = yaw * pc.math.DEG_TO_RAD;
        const s = Math.sin(rad);
        const c = Math.cos(rad);

        const rdx = dx * c - dz * s;
        const rdz = dx * s + dz * c;

        // Normalize
        const len = Math.sqrt(rdx*rdx + rdz*rdz);
        if (len > 0) {
            room.send("move", { dx: rdx/len, dz: rdz/len, pitch, yaw });
        }
    } else {
        room.send("move", { dx: 0, dz: 0, pitch, yaw });
    }
});
