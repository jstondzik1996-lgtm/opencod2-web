import { Schema, type, MapSchema } from "@colyseus/schema";

export class Player extends Schema {
    @type("string") id: string = "";
    @type("string") name: string = "";
    @type("number") x: number = 0;
    @type("number") y: number = 0;
    @type("number") z: number = 0;
    @type("number") pitch: number = 0;
    @type("number") yaw: number = 0;
    @type("number") health: number = 100;
    @type("boolean") isBot: boolean = false;
    @type("boolean") isDead: boolean = false;
}

export class GameState extends Schema {
    @type({ map: Player }) players = new MapSchema<Player>();
}
