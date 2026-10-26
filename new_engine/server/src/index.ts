import http from "http";
import express from "express";
import cors from "cors";
import { Server } from "colyseus";
import { GameRoom } from "./GameRoom";

const port = Number(process.env.PORT || 2567);
const app = express();

app.use(cors());
app.use(express.json());

const server = http.createServer(app);
const gameServer = new Server({
    server: server,
});

gameServer.define('game_room', GameRoom);

server.listen(port, () => {
    console.log(`Server is running on port ${port}`);
});
