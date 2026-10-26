import { Room, Client } from "colyseus";
import { GameState, Player } from "./GameState";
import RAPIER from "@dimforge/rapier3d-compat";

export class GameRoom extends Room<GameState, any> {
    maxClients = 16;
    world!: RAPIER.World;
    bots: any[] = [];
    tickRate = 60;

    async onCreate(options: any) {
        this.setState(new GameState());

        await RAPIER.init();
        const gravity = { x: 0.0, y: -9.81, z: 0.0 };
        this.world = new RAPIER.World(gravity);

        // Simple ground plane
        const groundColliderDesc = RAPIER.ColliderDesc.cuboid(50, 0.5, 50);
        this.world.createCollider(groundColliderDesc);

        this.setSimulationInterval((deltaTime) => this.update(deltaTime), 1000 / this.tickRate);

        // Spawn bots based on options or default to 15
        const numBots = options.bots || 15;
        for (let i = 0; i < numBots; i++) {
            this.spawnBot(`bot_${i}`);
        }

        this.onMessage("move", (client, message) => {
            const player = this.state.players.get(client.sessionId);
            if (!player || player.isDead) return;

            const body = client.userData?.body as RAPIER.RigidBody;
            if (body) {
                const speed = 5;
                body.setLinvel({
                    x: message.dx * speed,
                    y: body.linvel().y,
                    z: message.dz * speed
                }, true);
            }

            player.pitch = message.pitch || player.pitch;
            player.yaw = message.yaw || player.yaw;
        });

        this.onMessage("shoot", (client, message) => {
            const player = this.state.players.get(client.sessionId);
            if (!player || player.isDead) return;

            // Simple raycast for shooting
            const pos = client.userData.body.translation();
            // Start slightly above origin for camera height
            const start = new RAPIER.Vector3(pos.x, pos.y + 1.6, pos.z);

            // Calculate direction from pitch and yaw
            const pitch = message.pitch * (Math.PI / 180);
            const yaw = message.yaw * (Math.PI / 180);

            const dirX = Math.cos(pitch) * -Math.sin(yaw);
            const dirY = Math.sin(pitch);
            const dirZ = Math.cos(pitch) * -Math.cos(yaw);

            const dir = new RAPIER.Vector3(dirX, dirY, dirZ);

            const maxToi = 100.0;
            const solid = true;

            const ray = new RAPIER.Ray(start, dir);
            const hit = this.world.castRay(ray, maxToi, solid, RAPIER.QueryFilterFlags.EXCLUDE_DYNAMIC, undefined, undefined, client.userData.body);

            // For players/bots, we can do a simple distance/radius check manually to avoid complex filter flag matching for now
            let hitPlayerId: string | null = null;
            let closestDist = maxToi;

            this.state.players.forEach((p, id) => {
                if (id === client.sessionId || p.isDead) return;

                let targetBody;
                if (p.isBot) {
                    targetBody = this.bots.find(b => b.id === id)?.body;
                } else {
                    targetBody = this.clients.find(c => c.sessionId === id)?.userData?.body;
                }

                if (targetBody) {
                    const tPos = targetBody.translation();
                    const dx = tPos.x - start.x;
                    const dy = tPos.y - start.y;
                    const dz = tPos.z - start.z;

                    // Dot product to project vector onto ray
                    const t = dx*dir.x + dy*dir.y + dz*dir.z;

                    if (t > 0 && t < closestDist) {
                        // Closest point on ray
                        const cx = start.x + dir.x * t;
                        const cy = start.y + dir.y * t;
                        const cz = start.z + dir.z * t;

                        // Distance to cylinder (radius 0.5, height 2 centered)
                        const dist2 = (cx-tPos.x)**2 + (cz-tPos.z)**2;
                        if (dist2 <= 0.25 && cy >= tPos.y - 1 && cy <= tPos.y + 1) {
                            closestDist = t;
                            hitPlayerId = id;
                        }
                    }
                }
            });

            // If we hit a wall earlier than a player, ignore player hit
            // hit is of type RayColliderHit | RayColliderToi
            // In rapier3d-compat, castRay returns a hit object with .toi
            const hitDistance = hit ? (hit as any).toi : Number.MAX_VALUE;

            if (hit != null && hitDistance < closestDist) {
                hitPlayerId = null;
            }

            if (hitPlayerId) {
                const p = this.state.players.get(hitPlayerId);
                if (p) {
                    p.health -= 25;
                    if (p.health <= 0 && !p.isDead) {
                        p.isDead = true;
                        setTimeout(() => this.respawn(hitPlayerId!), 3000);
                    }
                }
            }
        });
    }

    respawn(id: string) {
        const p = this.state.players.get(id);
        if (!p) return;

        p.health = 100;
        p.isDead = false;

        let body;
        if (p.isBot) {
            body = this.bots.find(b => b.id === id)?.body;
        } else {
            body = this.clients.find(c => c.sessionId === id)?.userData?.body;
        }

        if (body) {
            body.setTranslation({
                x: Math.random() * 20 - 10,
                y: 5,
                z: Math.random() * 20 - 10
            }, true);
            body.setLinvel({x: 0, y: 0, z: 0}, true);
        }
    }

    spawnBot(id: string) {
        const bot = new Player();
        bot.id = id;
        bot.name = `Bot ${id}`;
        bot.isBot = true;
        bot.x = Math.random() * 20 - 10;
        bot.y = 5;
        bot.z = Math.random() * 20 - 10;
        this.state.players.set(id, bot);

        // Physics body for bot
        const rigidBodyDesc = RAPIER.RigidBodyDesc.dynamic()
            .setTranslation(bot.x, bot.y, bot.z)
            .lockRotations();
        const rigidBody = this.world.createRigidBody(rigidBodyDesc);
        const colliderDesc = RAPIER.ColliderDesc.cylinder(1, 0.5); // height 2, radius 0.5
        this.world.createCollider(colliderDesc, rigidBody);

        this.bots.push({ id, body: rigidBody, targetTime: 0, moveDir: {x:0, z:0} });
    }

    onJoin(client: Client, options: any) {
        console.log(client.sessionId, "joined!");

        const player = new Player();
        player.id = client.sessionId;
        player.name = options.name || "Player";
        player.x = 0;
        player.y = 5;
        player.z = 0;
        this.state.players.set(client.sessionId, player);

        // Physics body for player
        const rigidBodyDesc = RAPIER.RigidBodyDesc.dynamic()
            .setTranslation(player.x, player.y, player.z)
            .lockRotations();
        const rigidBody = this.world.createRigidBody(rigidBodyDesc);
        const colliderDesc = RAPIER.ColliderDesc.cylinder(1, 0.5);
        this.world.createCollider(colliderDesc, rigidBody);

        client.userData = { body: rigidBody };
    }

    update(deltaTime: number) {
        this.world.step();

        const now = Date.now();

        // Bot logic
        for (const botData of this.bots) {
            const botState = this.state.players.get(botData.id);
            if (!botState || botState.isDead) continue;

            // Random movement for now
            if (now > botData.targetTime) {
                botData.moveDir = {
                    x: Math.random() * 2 - 1,
                    z: Math.random() * 2 - 1
                };
                // Normalize
                const len = Math.sqrt(botData.moveDir.x**2 + botData.moveDir.z**2);
                if (len > 0) {
                    botData.moveDir.x /= len;
                    botData.moveDir.z /= len;
                }
                botData.targetTime = now + 1000 + Math.random() * 2000;
            }

            const speed = 3;
            botData.body.setLinvel({
                x: botData.moveDir.x * speed,
                y: botData.body.linvel().y,
                z: botData.moveDir.z * speed
            }, true);

            // Look in movement direction
            if (botData.moveDir.x !== 0 || botData.moveDir.z !== 0) {
                botState.yaw = Math.atan2(botData.moveDir.x, botData.moveDir.z);
            }
        }

        // Sync physics state to colyseus state
        this.state.players.forEach((player: Player, id: string) => {
            let body;
            if (player.isBot) {
                const botData = this.bots.find(b => b.id === id);
                if (botData) body = botData.body;
            } else {
                const client = this.clients.find(c => c.sessionId === id);
                if (client) body = client.userData?.body;
            }

            if (body) {
                const pos = body.translation();
                player.x = pos.x;
                player.y = pos.y;
                player.z = pos.z;
            }
        });
    }

    onLeave(client: Client, consented?: boolean) {
        console.log(client.sessionId, "left!");
        const body = client.userData?.body;
        if (body) {
            this.world.removeRigidBody(body);
        }
        this.state.players.delete(client.sessionId);
    }
}
