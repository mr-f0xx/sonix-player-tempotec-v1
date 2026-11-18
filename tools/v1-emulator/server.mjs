#!/usr/bin/env node
/**
 * TempoTec Variations V1 — Interactive Browser Emulator Server
 *
 * Zero external npm dependencies (pure Node.js built-ins).
 * Wraps the native `sonix-player/sonix_player_web` binary over a Unix domain
 * socket and serves an interactive browser UI with:
 *   - Real-time 240x320 RGB565 LVGL display stream
 *   - Touch / drag / scroll forwarding
 *   - Full TempoTec V1 physical flank buttons (short/long/hold + combos)
 *   - Live SGM31324 RGB LED telemetry (steady & breathing modes)
 *   - Live Web Audio PCM playback from the virtual ALSA driver
 *   - Dynamic battery percentage, charger status, and AirPods telemetry
 *   - Virtual microSD browser / file upload / on-device screenshot viewer
 *   - Live color-coded stdout/stderr log console & one-click rebuild
 */

import { localCatalog, newestFile } from "./local-builds.mjs";
import http from "node:http";
import net from "node:net";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const REPO_ROOT = path.resolve(__dirname, "..", "..");
const PLAYER_DIR = path.join(REPO_ROOT, "sonix-player");
const PLAYER_BIN = path.join(PLAYER_DIR, "sonix_player_web");
const ASSETS_DIR = path.join(REPO_ROOT, "sonix-packer", "assets", "V1");
const PUBLIC_DIR = path.join(__dirname, "public");
const DIST_DIR = path.join(__dirname, "dist");
const RUNTIME_DIR = path.join(__dirname, ".runtime");
const SD_DIR = path.join(RUNTIME_DIR, "sd");
const CONFIG_DIR = path.join(RUNTIME_DIR, "config");
const SYSFS_DIR = path.join(RUNTIME_DIR, "sysfs");
const BATTERY_CAP_FILE = path.join(SYSFS_DIR, "capacity");
const BATTERY_STATUS_FILE = path.join(SYSFS_DIR, "status");
const CONFIG_INI_FILE = path.join(CONFIG_DIR, "device_config.ini");
const EBOOK_INI_FILE = path.join(CONFIG_DIR, "ebook_config.ini");

const HOST = process.env.HOST || "0.0.0.0";
const PORT = Number(process.env.PORT || 8080);

const SOCK_PATH = path.join(
	os.tmpdir(),
	`sonix-v1-webemu-${process.pid}-${crypto.randomBytes(4).toString("hex")}.sock`
);

const MAX_LOG_LINES = 1500;
const MAX_UPLOAD_BYTES = 128 * 1024 * 1024; // 128 MB

/* --------------------------------------------------------------------------
 * Global emulator state
 * -------------------------------------------------------------------------- */
const state = {
	lifecycle: "starting", // starting | running | powered_off | building | crashed
	mode: "normal", // normal | charge_screen
	pid: null,
	exitCode: null,
	width: 240,
	height: 320,
	rotated: false,
	screenOn: true,
	backlight: { value: 200, max: 255, percent: 78 },
	led: {
		mode: "off", // off | steady | breathing
		r: 0,
		g: 0,
		b: 0,
		r_step: 0,
		g_step: 0,
		b_step: 0,
		rise_ms: 0,
		fall_ms: 0,
		period_ms: 0,
	},
	volume: 50,
	battery: {
		percent: 82,
		status: "Discharging", // Discharging | Charging | Full | Not charging
	},
	airpods: {
		enabled: false,
		model: "A3047", // AirPods Pro 2 (USB-C)
		left: 85,
		right: 80,
		caseBat: 65,
		noise: 2, // 1=Off, 2=ANC, 3=Transparency, 4=Adaptive
	},
	fps: 0,
	audioRate: 0,
	audioActive: false,
	binaryExists: fs.existsSync(PLAYER_BIN),
	assetsExist: fs.existsSync(path.join(ASSETS_DIR, "usr", "resource", "sonix")),
};

let logSeq = 0;
const logBuffer = [];
let lastFramePacket = null; // Buffer: [0x01][w_be16][h_be16][rgb565...]
let frameCountWindow = 0;
let lastAudioTime = 0;

let unixServer = null;
let playerConn = null;
let playerProc = null;
let buildProc = null;
let powerIntent = null; // null | "poweroff" | "reboot" | "restart"
let powerHoldBootTimer = null;

/** @type {Set<WsClient>} */
const wsClients = new Set();

/* --------------------------------------------------------------------------
 * Utility helpers
 * -------------------------------------------------------------------------- */
function atomicWriteFile(filePath, content) {
	fs.mkdirSync(path.dirname(filePath), { recursive: true });
	const tmp = `${filePath}.tmp.${process.pid}`;
	fs.writeFileSync(tmp, content, "utf8");
	fs.renameSync(tmp, filePath);
}

function pushLog(stream, text) {
	const clean = String(text).replace(/\r+$/, "");
	if (!clean) return;
	const entry = {
		seq: ++logSeq,
		ts: Date.now(),
		stream,
		line: clean,
	};
	logBuffer.push(entry);
	if (logBuffer.length > MAX_LOG_LINES) {
		logBuffer.splice(0, logBuffer.length - MAX_LOG_LINES);
	}
	broadcastJson({ type: "log", entry });
}

function writeBatteryFiles() {
	const pct = Math.max(0, Math.min(100, Math.round(Number(state.battery.percent) || 0)));
	state.battery.percent = pct;
	const validStatuses = new Set(["Discharging", "Charging", "Full", "Not charging"]);
	if (!validStatuses.has(state.battery.status)) {
		state.battery.status = "Discharging";
	}
	atomicWriteFile(BATTERY_CAP_FILE, `${pct}\n`);
	atomicWriteFile(BATTERY_STATUS_FILE, `${state.battery.status}\n`);
}

function buildAirpodsFakeSpec() {
	if (!state.airpods.enabled) return null;
	const model = String(state.airpods.model || "A3047").replace(/[^A-Za-z0-9]/g, "");
	const l = Math.max(-1, Math.min(100, Math.round(Number(state.airpods.left))));
	const r = Math.max(-1, Math.min(100, Math.round(Number(state.airpods.right))));
	const c = Math.max(-1, Math.min(100, Math.round(Number(state.airpods.caseBat))));
	const noise = Math.max(1, Math.min(4, Math.round(Number(state.airpods.noise) || 2)));
	return `${model},${l},${r},${c},${noise}`;
}

/* --------------------------------------------------------------------------
 * Synthetic musical WAV generator for first-run demo microSD
 * -------------------------------------------------------------------------- */
function makeInfoSubchunk(fourcc, value) {
	const raw = Buffer.from(`${value}\0`, "utf8");
	const padLen = raw.length % 2 === 1 ? raw.length + 1 : raw.length;
	const out = Buffer.alloc(8 + padLen);
	out.write(fourcc, 0, 4, "ascii");
	out.writeUInt32LE(raw.length, 4);
	raw.copy(out, 8);
	return out;
}

function buildWavWithInfo({ sampleRate, durationSec, generator, meta }) {
	const numChannels = 2;
	const bitsPerSample = 16;
	const numFrames = Math.floor(sampleRate * durationSec);
	const dataSize = numFrames * numChannels * (bitsPerSample / 8);
	const pcm = Buffer.alloc(dataSize);

	for (let i = 0; i < numFrames; i++) {
		const t = i / sampleRate;
		const [l, r] = generator(t, durationSec);
		const cl = Math.max(-1, Math.min(1, l));
		const cr = Math.max(-1, Math.min(1, r));
		const off = i * 4;
		pcm.writeInt16LE(Math.round(cl * 30000), off);
		pcm.writeInt16LE(Math.round(cr * 30000), off + 2);
	}

	const infoParts = [];
	if (meta.title) infoParts.push(makeInfoSubchunk("INAM", meta.title));
	if (meta.artist) infoParts.push(makeInfoSubchunk("IART", meta.artist));
	if (meta.album) infoParts.push(makeInfoSubchunk("IPRD", meta.album));
	if (meta.genre) infoParts.push(makeInfoSubchunk("IGNR", meta.genre));
	if (meta.track) infoParts.push(makeInfoSubchunk("ITRK", String(meta.track)));
	if (meta.year) infoParts.push(makeInfoSubchunk("ICRD", String(meta.year)));

	const infoBody = Buffer.concat([Buffer.from("INFO", "ascii"), ...infoParts]);
	const listChunk = Buffer.alloc(8 + infoBody.length);
	listChunk.write("LIST", 0, 4, "ascii");
	listChunk.writeUInt32LE(infoBody.length, 4);
	infoBody.copy(listChunk, 8);

	const fmtChunk = Buffer.alloc(24);
	fmtChunk.write("fmt ", 0, 4, "ascii");
	fmtChunk.writeUInt32LE(16, 4);
	fmtChunk.writeUInt16LE(1, 8); // PCM
	fmtChunk.writeUInt16LE(numChannels, 10);
	fmtChunk.writeUInt32LE(sampleRate, 12);
	fmtChunk.writeUInt32LE(sampleRate * numChannels * (bitsPerSample / 8), 16);
	fmtChunk.writeUInt16LE(numChannels * (bitsPerSample / 8), 20);
	fmtChunk.writeUInt16LE(bitsPerSample, 22);

	const dataHeader = Buffer.alloc(8);
	dataHeader.write("data", 0, 4, "ascii");
	dataHeader.writeUInt32LE(dataSize, 4);

	const totalPayload = 4 + fmtChunk.length + listChunk.length + dataHeader.length + pcm.length;
	const riffHeader = Buffer.alloc(12);
	riffHeader.write("RIFF", 0, 4, "ascii");
	riffHeader.writeUInt32LE(totalPayload, 4);
	riffHeader.write("WAVE", 8, 4, "ascii");

	return Buffer.concat([riffHeader, fmtChunk, listChunk, dataHeader, pcm]);
}

function seedRuntimeEnvironment() {
	fs.mkdirSync(SD_DIR, { recursive: true });
	fs.mkdirSync(CONFIG_DIR, { recursive: true });
	fs.mkdirSync(SYSFS_DIR, { recursive: true });

	for (const sub of ["Music", "Books", "Notes", "Themes", "Fonts", "Screenshots"]) {
		fs.mkdirSync(path.join(SD_DIR, sub), { recursive: true });
	}

	const demoAlbumDir = path.join(SD_DIR, "Music", "TempoTec Demo");
	if (!fs.existsSync(demoAlbumDir)) {
		fs.mkdirSync(demoAlbumDir, { recursive: true });

		// Track 1: Warm electric-piano chords at 44.1 kHz (triggers standard-res LED)
		const chords = [
			[174.61, 220.0, 261.63, 329.63], // Fmaj7
			[164.81, 196.0, 246.94, 293.66], // Em7
			[146.83, 174.61, 220.0, 261.63], // Dm7
			[130.81, 196.0, 246.94, 293.66], // Cmaj9
		];
		const wav44 = buildWavWithInfo({
			sampleRate: 44100,
			durationSec: 12,
			meta: {
				title: "Velvet Rhodes (44.1 kHz)",
				artist: "TempoTec V1 Lab",
				album: "Variations Demo",
				genre: "Ambient",
				track: 1,
				year: 2026,
			},
			generator: (t, dur) => {
				const bar = Math.min(chords.length - 1, Math.floor(t / 3.0));
				const localT = t - bar * 3.0;
				const env = Math.min(1, localT / 0.04) * Math.exp(-localT * 0.48);
				const fade = Math.min(1, t / 0.2) * Math.min(1, (dur - t) / 0.4);
				const chord = chords[bar];
				let l = 0;
				let r = 0;
				for (let n = 0; n < chord.length; n++) {
					const f = chord[n];
					const vibrato = 1 + 0.002 * Math.sin(2 * Math.PI * 4.5 * t + n);
					const tone =
						Math.sin(2 * Math.PI * f * vibrato * t) +
						0.25 * Math.sin(2 * Math.PI * f * 2 * t) * Math.exp(-localT * 1.5);
					const pan = n / (chord.length - 1);
					l += tone * (1 - pan * 0.45);
					r += tone * (0.55 + pan * 0.45);
				}
				const gain = 0.18 * env * fade;
				return [l * gain, r * gain];
			},
		});
		fs.writeFileSync(path.join(demoAlbumDir, "01 - Velvet Rhodes (44.1kHz).wav"), wav44);

		// Track 2: Sparkling Hi-Res 96 kHz arpeggio (triggers Hi-Res badge & >48 kHz LED)
		const notes = [261.63, 329.63, 392.0, 493.88, 523.25, 493.88, 392.0, 329.63];
		const wav96 = buildWavWithInfo({
			sampleRate: 96000,
			durationSec: 8,
			meta: {
				title: "Crystal Arpeggio (96 kHz Hi-Res)",
				artist: "TempoTec V1 Lab",
				album: "Variations Demo",
				genre: "Electronic",
				track: 2,
				year: 2026,
			},
			generator: (t, dur) => {
				const stepLen = 0.25;
				const idx = Math.floor(t / stepLen) % notes.length;
				const localT = t % stepLen;
				const f = notes[idx];
				const env = Math.min(1, localT / 0.008) * Math.exp(-localT * 6.0);
				const bass = Math.sin(2 * Math.PI * 130.81 * t) * 0.16;
				const bell =
					Math.sin(2 * Math.PI * f * t) +
					0.35 * Math.sin(2 * Math.PI * f * 2 * t) +
					0.12 * Math.sin(2 * Math.PI * f * 3 * t);
				const stereo = 0.5 + 0.4 * Math.sin(2 * Math.PI * 0.5 * t);
				const fade = Math.min(1, t / 0.15) * Math.min(1, (dur - t) / 0.35);
				return [
					(bass + bell * env * 0.32 * (1 - stereo * 0.3)) * fade,
					(bass + bell * env * 0.32 * (0.7 + stereo * 0.3)) * fade,
				];
			},
		});
		fs.writeFileSync(path.join(demoAlbumDir, "02 - Crystal Arpeggio (96kHz).wav"), wav96);
	}

	const welcomeNote = path.join(SD_DIR, "Notes", "Welcome to TempoTec V1.txt");
	if (!fs.existsSync(welcomeNote)) {
		fs.writeFileSync(
			welcomeNote,
			[
				"Welcome to the TempoTec Variations V1 Browser Emulator!",
				"",
				"- Drag or click on the 240x320 screen just like touch input.",
				"- Click or hold the right-side physical buttons (Power, Vol+, Vol-, Prev, Play, Next).",
				"- Hold Power for ~2s to open the real Power Menu (Sleep / Reboot / Power Off).",
				"- Press Vol+ and Power together (or click Device Shot) to take an on-device PNG screenshot.",
				"- Upload your own MP3, FLAC, WAV, OGG, M4A, DSD, EPUB, or GB/GBC files via the microSD tab.",
				"",
			].join("\n"),
			"utf8"
		);
	}

	const cfgFile = path.join(CONFIG_DIR, "device_config.ini");
	if (!fs.existsSync(cfgFile)) {
		fs.writeFileSync(
			cfgFile,
			[
				"[ui]",
				"language = English",
				"",
				"[clock]",
				"configured = true",
				"automatic = false",
				"utc_offset_minutes = 0",
				"",
				"[system]",
				"screenshots = true",
				"",
			].join("\n"),
			"utf8"
		);
	}

	writeBatteryFiles();
}

/* --------------------------------------------------------------------------
 * Minimal RFC 6455 WebSocket implementation (zero dependencies)
 * -------------------------------------------------------------------------- */
class WsClient {
	constructor(socket) {
		this.socket = socket;
		this.closed = false;
		this.buf = Buffer.alloc(0);
		this.audioEnabled = true;

		socket.on("data", (chunk) => this._onData(chunk));
		socket.on("close", () => this._cleanup());
		socket.on("error", () => this._cleanup());
	}

	_cleanup() {
		if (this.closed) return;
		this.closed = true;
		wsClients.delete(this);
		try {
			this.socket.destroy();
		} catch {
			/* ignore */
		}
	}

	sendFrame(opcode, payloadBuf) {
		if (this.closed || this.socket.destroyed) return;
		const len = payloadBuf.length;
		let header;
		if (len < 126) {
			header = Buffer.alloc(2);
			header[0] = 0x80 | opcode;
			header[1] = len;
		} else if (len < 65536) {
			header = Buffer.alloc(4);
			header[0] = 0x80 | opcode;
			header[1] = 126;
			header.writeUInt16BE(len, 2);
		} else {
			header = Buffer.alloc(10);
			header[0] = 0x80 | opcode;
			header[1] = 127;
			header.writeBigUInt64BE(BigInt(len), 2);
		}
		try {
			this.socket.write(header);
			if (len > 0) this.socket.write(payloadBuf);
		} catch {
			this._cleanup();
		}
	}

	sendText(str) {
		this.sendFrame(0x01, Buffer.from(str, "utf8"));
	}

	sendJson(obj) {
		this.sendText(JSON.stringify(obj));
	}

	sendBinary(buf) {
		if (this.socket.writableLength > 2 * 1024 * 1024) {
			return; // drop frame/audio chunk if browser tab is severely congested
		}
		this.sendFrame(0x02, buf);
	}

	_onData(chunk) {
		this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
		while (this.buf.length >= 2) {
			const b0 = this.buf[0];
			const b1 = this.buf[1];
			const opcode = b0 & 0x0f;
			const masked = (b1 & 0x80) !== 0;
			let payloadLen = b1 & 0x7f;
			let offset = 2;

			if (payloadLen === 126) {
				if (this.buf.length < 4) return;
				payloadLen = this.buf.readUInt16BE(2);
				offset = 4;
			} else if (payloadLen === 127) {
				if (this.buf.length < 10) return;
				const big = this.buf.readBigUInt64BE(2);
				if (big > BigInt(4 * 1024 * 1024)) {
					this._cleanup();
					return;
				}
				payloadLen = Number(big);
				offset = 10;
			}

			const maskLen = masked ? 4 : 0;
			if (this.buf.length < offset + maskLen + payloadLen) return;

			const payload = Buffer.alloc(payloadLen);
			if (masked) {
				const mask = this.buf.subarray(offset, offset + 4);
				const raw = this.buf.subarray(offset + 4, offset + 4 + payloadLen);
				for (let i = 0; i < payloadLen; i++) {
					payload[i] = raw[i] ^ mask[i & 3];
				}
			} else {
				this.buf.copy(payload, 0, offset, offset + payloadLen);
			}

			this.buf = this.buf.subarray(offset + maskLen + payloadLen);

			if (opcode === 0x08) {
				this.sendFrame(0x08, Buffer.alloc(0));
				this._cleanup();
				return;
			}
			if (opcode === 0x09) {
				this.sendFrame(0x0a, payload);
				continue;
			}
			if (opcode === 0x01) {
				try {
					const msg = JSON.parse(payload.toString("utf8"));
					handleClientWsMessage(this, msg);
				} catch {
					/* ignore malformed client JSON */
				}
			}
		}
	}
}

function broadcastJson(obj) {
	if (wsClients.size === 0) return;
	const text = JSON.stringify(obj);
	for (const client of wsClients) {
		client.sendText(text);
	}
}

function broadcastBinary(buf, kind = "frame") {
	if (wsClients.size === 0) return;
	for (const client of wsClients) {
		if (kind === "audio" && !client.audioEnabled) continue;
		client.sendBinary(buf);
	}
}

function broadcastState() {
	broadcastJson({ type: "state", state });
}

/* --------------------------------------------------------------------------
 * Unix domain socket bridge with sonix_player_web
 * -------------------------------------------------------------------------- */
function sendPlayerCommand(line) {
	if (!playerConn || playerConn.destroyed) return false;
	try {
		playerConn.write(`${line}\n`);
		return true;
	} catch {
		return false;
	}
}

function handlePlayerTelemetry(msg) {
	if (!msg || typeof msg !== "object") return;
	switch (msg.type) {
		case "ready":
			if (msg.width) state.width = msg.width;
			if (msg.height) state.height = msg.height;
			state.lifecycle = "running";
			broadcastState();
			break;
		case "led":
			state.led = {
				mode: msg.mode || "off",
				r: Number(msg.r) || 0,
				g: Number(msg.g) || 0,
				b: Number(msg.b) || 0,
				r_step: Number(msg.r_step) || 0,
				g_step: Number(msg.g_step) || 0,
				b_step: Number(msg.b_step) || 0,
				rise_ms: Number(msg.rise_ms) || 0,
				fall_ms: Number(msg.fall_ms) || 0,
				period_ms: Number(msg.period_ms) || 0,
			};
			broadcastJson({ type: "led", led: state.led });
			break;
		case "backlight": {
			const value = Number(msg.value) || 0;
			const max = Number(msg.max) || 255;
			const percent =
				msg.percent !== undefined
					? Math.max(0, Math.min(100, Math.round(Number(msg.percent))))
					: Math.max(0, Math.min(100, Math.round((value * 100) / max)));
			state.backlight = { value, max, percent };
			broadcastJson({ type: "backlight", backlight: state.backlight });
			break;
		}
		case "screen":
			state.screenOn = Boolean(msg.on);
			broadcastJson({ type: "screen", on: state.screenOn });
			break;
		case "rotated":
			state.rotated = Boolean(msg.rotated);
			broadcastJson({ type: "rotated", rotated: state.rotated });
			break;
		case "volume":
			state.volume = Math.max(0, Math.min(100, Number(msg.percent) || 0));
			broadcastJson({ type: "volume", percent: state.volume });
			break;
		case "power":
			if (msg.action === "poweroff") {
				powerIntent = "poweroff";
				pushLog("emu", "Device requested power off (virtual hardware shutdown).");
			} else if (msg.action === "reboot") {
				powerIntent = "reboot";
				pushLog("emu", "Device requested reboot — restarting emulator...");
			}
			broadcastJson({ type: "power", action: msg.action });
			break;
		default:
			broadcastJson(msg);
			break;
	}
}

function setupUnixBridge() {
	try {
		fs.unlinkSync(SOCK_PATH);
	} catch {
		/* ignore */
	}

	unixServer = net.createServer((conn) => {
		if (playerConn && !playerConn.destroyed) {
			try {
				playerConn.destroy();
			} catch {
				/* ignore */
			}
		}
		playerConn = conn;
		let rxBuf = Buffer.alloc(0);

		conn.on("data", (chunk) => {
			rxBuf = rxBuf.length ? Buffer.concat([rxBuf, chunk]) : chunk;
			while (rxBuf.length >= 5) {
				const pktType = rxBuf[0];
				const pktLen = rxBuf.readUInt32BE(1);
				if (pktLen > 2 * 1024 * 1024) {
					conn.destroy();
					return;
				}
				if (rxBuf.length < 5 + pktLen) break;

				const payload = rxBuf.subarray(5, 5 + pktLen);
				rxBuf = rxBuf.subarray(5 + pktLen);

				if (pktType === 1) {
					// Frame packet: [u16 BE w][u16 BE h][RGB565 LE pixels]
					const out = Buffer.allocUnsafe(1 + payload.length);
					out[0] = 1;
					payload.copy(out, 1);
					lastFramePacket = out;
					if (payload.length >= 4) {
						state.width = payload.readUInt16BE(0);
						state.height = payload.readUInt16BE(2);
					}
					frameCountWindow++;
					broadcastBinary(out, "frame");
				} else if (pktType === 2) {
					try {
						const msg = JSON.parse(payload.toString("utf8"));
						handlePlayerTelemetry(msg);
					} catch {
						/* ignore */
					}
				} else if (pktType === 3) {
					// Audio PCM packet: [u32 BE rate][u16 BE ch][S16LE PCM]
					if (payload.length >= 6) {
						const rate = payload.readUInt32BE(0);
						if (rate !== state.audioRate) {
							state.audioRate = rate;
							broadcastJson({ type: "audio_info", rate, active: true });
						}
						lastAudioTime = Date.now();
						if (!state.audioActive) {
							state.audioActive = true;
							broadcastJson({ type: "audio_info", rate: state.audioRate, active: true });
						}
					}
					const out = Buffer.allocUnsafe(1 + payload.length);
					out[0] = 3;
					payload.copy(out, 1);
					broadcastBinary(out, "audio");
				}
			}
		});

		conn.on("close", () => {
			if (playerConn === conn) {
				playerConn = null;
			}
		});
		conn.on("error", () => {
			if (playerConn === conn) {
				playerConn = null;
			}
		});
	});

	unixServer.listen(SOCK_PATH);
}

/* --------------------------------------------------------------------------
 * Player process lifecycle management
 * -------------------------------------------------------------------------- */
function attachLineReader(stream, tag) {
	let rem = "";
	stream.on("data", (chunk) => {
		rem += chunk.toString("utf8");
		const lines = rem.split("\n");
		rem = lines.pop() ?? "";
		for (const line of lines) {
			pushLog(tag, line);
			if (line.includes("screenshot") && line.includes(".png")) {
				broadcastJson({ type: "sd_changed" });
			}
		}
	});
	stream.on("end", () => {
		if (rem.trim()) pushLog(tag, rem);
	});
}

function stopPlayer(intent = "poweroff") {
	return new Promise((resolve) => {
		powerIntent = intent;
		if (!playerProc) {
			resolve();
			return;
		}
		const proc = playerProc;
		let done = false;
		const finish = () => {
			if (done) return;
			done = true;
			clearTimeout(killTimer);
			resolve();
		};
		proc.once("exit", finish);
		sendPlayerCommand("Q");
		setTimeout(() => {
			if (!done) {
				try {
					proc.kill("SIGTERM");
				} catch {
					/* ignore */
				}
			}
		}, 450);
		const killTimer = setTimeout(() => {
			if (!done) {
				try {
					proc.kill("SIGKILL");
				} catch {
					/* ignore */
				}
				finish();
			}
		}, 1800);
	});
}

function startPlayer(mode = "normal") {
	if (buildProc) {
		pushLog("emu", "Cannot start while a rebuild is in progress.");
		return;
	}
	if (!fs.existsSync(PLAYER_BIN)) {
		state.binaryExists = false;
		pushLog("emu", "sonix_player_web binary missing — starting automatic build...");
		rebuildAndStart(mode);
		return;
	}

	state.binaryExists = true;
	state.mode = mode;
	state.lifecycle = "starting";
	state.exitCode = null;
	state.screenOn = true;
	state.rotated = false;
	state.led = {
		mode: "off",
		r: 0,
		g: 0,
		b: 0,
		r_step: 0,
		g_step: 0,
		b_step: 0,
		rise_ms: 0,
		fall_ms: 0,
		period_ms: 0,
	};
	state.audioActive = false;
	powerIntent = null;
	writeBatteryFiles();
	broadcastState();

	const env = {
		...process.env,
		SONIX_WEB_EMU_SOCK: SOCK_PATH,
		SONIX_PANEL: "v1",
		SONIX_SD_ROOT: SD_DIR,
		SONIX_CONFIG: CONFIG_INI_FILE,
		SONIX_EBOOK_CONFIG: EBOOK_INI_FILE,
		SONIX_BATTERY_CAPACITY: BATTERY_CAP_FILE,
		SONIX_BATTERY_STATUS: BATTERY_STATUS_FILE,
		TZ: process.env.TZ || "UTC",
	};
	const apSpec = buildAirpodsFakeSpec();
	if (apSpec) {
		env.SONIX_AIRPODS_FAKE = apSpec;
	} else {
		delete env.SONIX_AIRPODS_FAKE;
	}
	if (mode === "charge_screen") {
		env.SONIX_CHARGE_SCREEN = "1";
	} else {
		delete env.SONIX_CHARGE_SCREEN;
	}

	pushLog(
		"emu",
		`Booting TempoTec V1 (${mode === "charge_screen" ? "offline charge screen" : "normal OS"})...`
	);

	const proc = spawn(PLAYER_BIN, [], {
		cwd: ASSETS_DIR,
		env,
		stdio: ["ignore", "pipe", "pipe"],
	});
	playerProc = proc;
	state.pid = proc.pid;
	broadcastState();

	attachLineReader(proc.stdout, "stdout");
	attachLineReader(proc.stderr, "stderr");

	proc.on("exit", (code, signal) => {
		if (playerProc === proc) {
			playerProc = null;
		}
		state.pid = null;
		state.exitCode = code;
		state.audioActive = false;

		const intent = powerIntent;
		powerIntent = null;

		if (intent === "reboot" || code === 43) {
			state.lifecycle = "starting";
			state.screenOn = false;
			state.led.mode = "off";
			state.led.r = state.led.g = state.led.b = 0;
			broadcastState();
			pushLog("emu", "Rebooting TempoTec V1...");
			setTimeout(() => startPlayer("normal"), 450);
			return;
		}

		if (intent === "restart") {
			return;
		}

		if (intent === "poweroff" || code === 42 || code === 0) {
			state.lifecycle = "powered_off";
			state.screenOn = false;
			state.led = {
				mode: "off",
				r: 0,
				g: 0,
				b: 0,
				r_step: 0,
				g_step: 0,
				b_step: 0,
				rise_ms: 0,
				fall_ms: 0,
				period_ms: 0,
			};
			pushLog(
				"emu",
				"TempoTec V1 is powered off. Hold the Power button (1s) or click Boot to turn it back on."
			);
			broadcastState();
			return;
		}

		state.lifecycle = "crashed";
		state.screenOn = false;
		pushLog(
			"stderr",
			`sonix_player_web exited unexpectedly (code=${code}, signal=${signal || "none"})`
		);
		broadcastState();
	});
}

async function rebuildAndStart(modeAfter = "normal") {
	if (buildProc || state.lifecycle === "building") return;
	state.lifecycle = "building";
	state.screenOn = false;
	broadcastState();

	await stopPlayer("restart");
	const jobs = os.availableParallelism?.() || Math.max(1, os.cpus().length);
	pushLog("build", `Running make -C sonix-player -j${jobs} web-emu ...`);

	const proc = spawn("make", ["-C", PLAYER_DIR, `-j${jobs}`, "web-emu"], {
		cwd: REPO_ROOT,
		stdio: ["ignore", "pipe", "pipe"],
	});
	buildProc = proc;

	attachLineReader(proc.stdout, "build");
	attachLineReader(proc.stderr, "build");
	proc.once("error", (err) => pushLog("stderr", `Cannot run make: ${err.message}`));

	proc.once("close", (code) => {
		buildProc = null;
		state.binaryExists = fs.existsSync(PLAYER_BIN);
		if (code === 0) {
			pushLog("build", "Build succeeded! Booting TempoTec V1...");
			startPlayer(modeAfter);
		} else {
			state.lifecycle = "crashed";
			pushLog("stderr", `Build failed with exit code ${code}.`);
			broadcastState();
		}
	});
}

/* --------------------------------------------------------------------------
 * Client WebSocket message handling
 * -------------------------------------------------------------------------- */
const ALLOWED_BUTTONS = new Set(["power", "vol_up", "vol_down", "prev", "play", "next"]);

function handleClientWsMessage(client, msg) {
	if (!msg || typeof msg !== "object") return;
	switch (msg.type) {
		case "pointer": {
			const x = Math.max(0, Math.min(state.width - 1, Math.round(Number(msg.x) || 0)));
			const y = Math.max(0, Math.min(state.height - 1, Math.round(Number(msg.y) || 0)));
			const down = msg.down ? 1 : 0;
			sendPlayerCommand(`P ${x} ${y} ${down}`);
			break;
		}
		case "key": {
			const btn = String(msg.button || "");
			if (!ALLOWED_BUTTONS.has(btn)) return;
			const down = msg.down ? 1 : 0;

			// Virtual hardware power-on: holding Power while powered off boots the V1!
			if (btn === "power" && state.lifecycle === "powered_off") {
				if (down) {
					clearTimeout(powerHoldBootTimer);
					powerHoldBootTimer = setTimeout(() => {
						powerHoldBootTimer = null;
						if (state.lifecycle === "powered_off") {
							pushLog("emu", "Power button held while off — booting TempoTec V1...");
							startPlayer("normal");
						}
					}, 900);
				} else {
					clearTimeout(powerHoldBootTimer);
					powerHoldBootTimer = null;
				}
				return;
			}
			sendPlayerCommand(`K ${btn} ${down}`);
			break;
		}
		case "screenshot_device":
			sendPlayerCommand("S");
			setTimeout(() => broadcastJson({ type: "sd_changed" }), 650);
			break;
		case "audio_pref":
			client.audioEnabled = Boolean(msg.enabled);
			break;
		default:
			break;
	}
}

/* --------------------------------------------------------------------------
 * Safe virtual microSD path resolution
 * -------------------------------------------------------------------------- */
function resolveSdPath(relPath = "") {
	const cleaned = String(relPath || "").replace(/^\/+/, "");
	const full = path.resolve(SD_DIR, cleaned);
	const rel = path.relative(SD_DIR, full);
	if (rel.startsWith("..") || path.isAbsolute(rel)) {
		throw new Error("Invalid SD path");
	}
	return full;
}

function listSdDirectory(relDir = "") {
	const dirPath = resolveSdPath(relDir);
	if (!fs.existsSync(dirPath) || !fs.statSync(dirPath).isDirectory()) {
		return [];
	}
	const entries = fs.readdirSync(dirPath, { withFileTypes: true });
	const out = [];
	for (const ent of entries) {
		if (ent.name.startsWith(".")) continue;
		const full = path.join(dirPath, ent.name);
		let st;
		try {
			st = fs.statSync(full);
		} catch {
			continue;
		}
		const rel = path.relative(SD_DIR, full).split(path.sep).join("/");
		out.push({
			name: ent.name,
			path: rel,
			isDir: ent.isDirectory(),
			size: ent.isDirectory() ? 0 : st.size,
			mtime: st.mtimeMs,
		});
	}
	out.sort((a, b) => {
		if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
		if (!a.isDir && relDir.toLowerCase().includes("screenshot")) {
			return b.mtime - a.mtime; // newest screenshots first
		}
		return a.name.localeCompare(b.name);
	});
	return out;
}

/* --------------------------------------------------------------------------
 * HTTP server & REST API
 * -------------------------------------------------------------------------- */
const MIME_TYPES = {
	".html": "text/html; charset=utf-8",
	".css": "text/css; charset=utf-8",
	".js": "application/javascript; charset=utf-8",
	".mjs": "application/javascript; charset=utf-8",
	".json": "application/json; charset=utf-8",
	".svg": "image/svg+xml",
	".png": "image/png",
	".jpg": "image/jpeg",
	".jpeg": "image/jpeg",
	".webp": "image/webp",
	".wav": "audio/wav",
	".mp3": "audio/mpeg",
	".flac": "audio/flac",
	".ogg": "audio/ogg",
	".txt": "text/plain; charset=utf-8",
	".ini": "text/plain; charset=utf-8",
};

function readJsonBody(req) {
	return new Promise((resolve, reject) => {
		const chunks = [];
		let total = 0;
		req.on("data", (c) => {
			total += c.length;
			if (total > 1024 * 1024) {
				reject(new Error("Body too large"));
				req.destroy();
				return;
			}
			chunks.push(c);
		});
		req.on("end", () => {
			if (!chunks.length) return resolve({});
			try {
				resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
			} catch {
				reject(new Error("Invalid JSON"));
			}
		});
		req.on("error", reject);
	});
}

function sendJson(res, status, obj) {
	const body = JSON.stringify(obj);
	res.writeHead(status, {
		"Content-Type": "application/json; charset=utf-8",
		"Cache-Control": "no-store",
	});
	res.end(body);
}

async function handleApiRequest(req, res, parsedUrl) {
	const pathname = parsedUrl.pathname;

	if (req.method === "GET" && pathname === "/api/state") {
		return sendJson(res, 200, { state, sdDir: SD_DIR, configDir: CONFIG_DIR });
	}

	if (req.method === "POST" && pathname === "/api/battery") {
		const body = await readJsonBody(req);
		if (body.percent !== undefined) {
			state.battery.percent = Math.max(0, Math.min(100, Math.round(Number(body.percent))));
		}
		if (body.status !== undefined) {
			state.battery.status = String(body.status);
		}
		writeBatteryFiles();
		pushLog("emu", `Battery updated: ${state.battery.percent}% (${state.battery.status})`);
		broadcastJson({ type: "battery", battery: state.battery });
		return sendJson(res, 200, { ok: true, battery: state.battery });
	}

	if (req.method === "POST" && pathname === "/api/airpods") {
		const body = await readJsonBody(req);
		state.airpods = {
			enabled: Boolean(body.enabled),
			model: String(body.model ?? state.airpods.model),
			left: Number(body.left ?? state.airpods.left),
			right: Number(body.right ?? state.airpods.right),
			caseBat: Number(body.caseBat ?? state.airpods.caseBat),
			noise: Number(body.noise ?? state.airpods.noise),
		};
		pushLog(
			"emu",
			state.airpods.enabled
				? `AirPods telemetry set (${buildAirpodsFakeSpec()}) — rebooting to apply...`
				: "AirPods telemetry disabled — rebooting to apply..."
		);
		broadcastJson({ type: "airpods", airpods: state.airpods });
		if (body.reboot !== false && state.lifecycle !== "powered_off") {
			await stopPlayer("restart");
			startPlayer("normal");
		}
		return sendJson(res, 200, { ok: true, airpods: state.airpods });
	}

	if (req.method === "POST" && pathname === "/api/power") {
		const body = await readJsonBody(req);
		const action = String(body.action || "");
		if (action === "boot") {
			await stopPlayer("restart");
			startPlayer("normal");
			return sendJson(res, 200, { ok: true });
		}
		if (action === "reboot") {
			await stopPlayer("restart");
			startPlayer("normal");
			return sendJson(res, 200, { ok: true });
		}
		if (action === "poweroff") {
			await stopPlayer("poweroff");
			state.lifecycle = "powered_off";
			state.screenOn = false;
			state.led.mode = "off";
			state.led.r = state.led.g = state.led.b = 0;
			broadcastState();
			return sendJson(res, 200, { ok: true });
		}
		if (action === "charge_screen") {
			if (state.battery.status === "Discharging") {
				state.battery.status = "Charging";
				writeBatteryFiles();
				broadcastJson({ type: "battery", battery: state.battery });
			}
			await stopPlayer("restart");
			startPlayer("charge_screen");
			return sendJson(res, 200, { ok: true });
		}
		if (action === "reset_config") {
			await stopPlayer("restart");
			try {
				fs.rmSync(CONFIG_DIR, { recursive: true, force: true });
			} catch {
				/* ignore */
			}
			fs.mkdirSync(CONFIG_DIR, { recursive: true });
			pushLog("emu", "Configuration wiped — booting into First-Boot Setup Wizard...");
			startPlayer("normal");
			return sendJson(res, 200, { ok: true });
		}
		if (action === "rebuild") {
			rebuildAndStart("normal");
			return sendJson(res, 200, { ok: true });
		}
		return sendJson(res, 400, { error: `Unknown power action: ${action}` });
	}

	if (req.method === "POST" && pathname === "/api/screenshot/device") {
		const sent = sendPlayerCommand("S");
		setTimeout(() => broadcastJson({ type: "sd_changed" }), 650);
		return sendJson(res, 200, { ok: sent });
	}

	if (req.method === "POST" && pathname === "/api/logs/clear") {
		logBuffer.length = 0;
		broadcastJson({ type: "logs_cleared" });
		return sendJson(res, 200, { ok: true });
	}

	if (req.method === "GET" && pathname === "/api/sd/list") {
		const dir = parsedUrl.searchParams.get("dir") || "";
		const items = listSdDirectory(dir);
		return sendJson(res, 200, { dir, items });
	}

	if (req.method === "GET" && pathname === "/api/sd/file") {
		const rel = parsedUrl.searchParams.get("path") || "";
		const full = resolveSdPath(rel);
		if (!fs.existsSync(full) || !fs.statSync(full).isFile()) {
			return sendJson(res, 404, { error: "File not found" });
		}
		const ext = path.extname(full).toLowerCase();
		res.writeHead(200, {
			"Content-Type": MIME_TYPES[ext] || "application/octet-stream",
			"Cache-Control": "no-store",
		});
		fs.createReadStream(full).pipe(res);
		return;
	}

	if (req.method === "POST" && pathname === "/api/sd/upload") {
		const dir = parsedUrl.searchParams.get("dir") || "Music";
		const rawName = parsedUrl.searchParams.get("name") || `upload-${Date.now()}.bin`;
		const safeName = path.basename(rawName).replace(/[\x00-\x1f]/g, "_");
		if (!safeName || safeName === "." || safeName === "..") {
			return sendJson(res, 400, { error: "Invalid filename" });
		}
		const targetDir = resolveSdPath(dir);
		fs.mkdirSync(targetDir, { recursive: true });
		const targetFile = path.join(targetDir, safeName);

		let total = 0;
		const out = fs.createWriteStream(targetFile);
		let aborted = false;

		await new Promise((resolve, reject) => {
			req.on("data", (chunk) => {
				total += chunk.length;
				if (total > MAX_UPLOAD_BYTES) {
					aborted = true;
					out.destroy();
					try {
						fs.unlinkSync(targetFile);
					} catch {
						/* ignore */
					}
					reject(new Error("Upload exceeds 128 MB limit"));
					req.destroy();
					return;
				}
				out.write(chunk);
			});
			req.on("end", () => {
				if (!aborted) {
					out.end(() => resolve());
				}
			});
			req.on("error", (err) => {
				out.destroy();
				reject(err);
			});
			out.on("error", reject);
		});

		const rel = path.relative(SD_DIR, targetFile).split(path.sep).join("/");
		pushLog("emu", `Uploaded to virtual microSD: /${rel} (${(total / 1024).toFixed(1)} KB)`);
		broadcastJson({ type: "sd_changed" });
		return sendJson(res, 200, { ok: true, path: rel, size: total });
	}

	if (req.method === "DELETE" && pathname === "/api/sd/file") {
		const rel = parsedUrl.searchParams.get("path") || "";
		const full = resolveSdPath(rel);
		if (full === SD_DIR) {
			return sendJson(res, 400, { error: "Cannot delete SD root" });
		}
		if (!fs.existsSync(full)) {
			return sendJson(res, 404, { error: "Not found" });
		}
		fs.rmSync(full, { recursive: true, force: true });
		pushLog("emu", `Deleted from virtual microSD: /${rel}`);
		broadcastJson({ type: "sd_changed" });
		return sendJson(res, 200, { ok: true });
	}

	return sendJson(res, 404, { error: "Unknown API endpoint" });
}

function serveWasmBuilds(req, res, parsedUrl) {
	const sub = decodeURIComponent(parsedUrl.pathname.slice("/builds/".length));
	const headersBase = {
		"Cross-Origin-Opener-Policy": "same-origin",
		"Cross-Origin-Embedder-Policy": "require-corp",
		"Cross-Origin-Resource-Policy": "cross-origin",
		"Cache-Control": "no-cache",
	};

	if (sub === "manifest.json") {
		const catalog = localCatalog(DIST_DIR);
		res.writeHead(200, { ...headersBase, "Content-Type": "application/json; charset=utf-8" });
		res.end(JSON.stringify(catalog));
		return true;
	}

	const safeName = path.basename(sub);
	if (!safeName || safeName !== sub || !safeName.endsWith(".sonix-web.zip")) {
		res.writeHead(404);
		res.end("Not found");
		return true;
	}
	const found = newestFile([DIST_DIR, path.join(DIST_DIR, "pages", "builds")], safeName);
	if (found) {
		res.writeHead(200, {
			...headersBase, "Content-Type": "application/zip", "Content-Length": String(found.stat.size),
		});
		fs.createReadStream(found.file).pipe(res);
		return true;
	}
	res.writeHead(404);
	res.end("Not found");
	return true;
}

function serveStaticFile(req, res, parsedUrl) {
	let rel = parsedUrl.pathname === "/" ? "/index.html" : parsedUrl.pathname;
	try {
		rel = decodeURIComponent(rel);
	} catch {
		res.writeHead(400);
		res.end("Bad request");
		return;
	}
	const full = path.resolve(PUBLIC_DIR, `.${rel}`);
	const relCheck = path.relative(PUBLIC_DIR, full);
	if (relCheck.startsWith("..") || path.isAbsolute(relCheck)) {
		res.writeHead(403);
		res.end("Forbidden");
		return;
	}
	if (!fs.existsSync(full) || !fs.statSync(full).isFile()) {
		res.writeHead(404);
		res.end("Not found");
		return;
	}
	const ext = path.extname(full).toLowerCase();
	res.writeHead(200, {
		"Content-Type": MIME_TYPES[ext] || "application/octet-stream",
		"Cache-Control": "no-cache",
		"Cross-Origin-Opener-Policy": "same-origin",
		"Cross-Origin-Embedder-Policy": "require-corp",
		"Cross-Origin-Resource-Policy": "cross-origin",
	});
	fs.createReadStream(full).pipe(res);
}

const server = http.createServer(async (req, res) => {
	try {
		const parsedUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
		if (parsedUrl.pathname.startsWith("/api/")) {
			await handleApiRequest(req, res, parsedUrl);
			return;
		}
		if (parsedUrl.pathname.startsWith("/builds/")) {
			serveWasmBuilds(req, res, parsedUrl);
			return;
		}
		serveStaticFile(req, res, parsedUrl);
	} catch (err) {
		if (!res.headersSent) {
			sendJson(res, 500, { error: err?.message || "Internal server error" });
		}
	}
});

server.on("upgrade", (req, socket) => {
	try {
		const parsedUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
		if (parsedUrl.pathname !== "/ws") {
			socket.write("HTTP/1.1 404 Not Found\r\n\r\n");
			socket.destroy();
			return;
		}
		const key = req.headers["sec-websocket-key"];
		if (!key) {
			socket.write("HTTP/1.1 400 Bad Request\r\n\r\n");
			socket.destroy();
			return;
		}
		const accept = crypto
			.createHash("sha1")
			.update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
			.digest("base64");
		socket.write(
			[
				"HTTP/1.1 101 Switching Protocols",
				"Upgrade: websocket",
				"Connection: Upgrade",
				`Sec-WebSocket-Accept: ${accept}`,
				"\r\n",
			].join("\r\n")
		);

		const client = new WsClient(socket);
		wsClients.add(client);

		// Send initial state snapshot, log history, and latest framebuffer immediately
		client.sendJson({
			type: "init",
			state,
			logs: logBuffer.slice(-400),
		});
		if (lastFramePacket && state.lifecycle === "running") {
			client.sendBinary(lastFramePacket);
		}
	} catch {
		socket.destroy();
	}
});

/* Periodic FPS & audio-idle telemetry ticker */
setInterval(() => {
	state.fps = frameCountWindow;
	frameCountWindow = 0;
	if (state.audioActive && Date.now() - lastAudioTime > 800) {
		state.audioActive = false;
		broadcastJson({ type: "audio_info", rate: state.audioRate, active: false });
	}
	broadcastJson({ type: "metrics", fps: state.fps, audioActive: state.audioActive });
}, 1000);

/* --------------------------------------------------------------------------
 * Startup & graceful shutdown
 * -------------------------------------------------------------------------- */
function shutdown() {
	console.log("\nShutting down TempoTec V1 emulator...");
	stopPlayer("poweroff").finally(() => {
		try {
			unixServer?.close();
		} catch {
			/* ignore */
		}
		try {
			fs.unlinkSync(SOCK_PATH);
		} catch {
			/* ignore */
		}
		process.exit(0);
	});
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

seedRuntimeEnvironment();
setupUnixBridge();

server.listen(PORT, HOST, () => {
	console.log(`TempoTec Variations V1 Web Emulator listening on http://${HOST}:${PORT}`);
	if (HOST === "0.0.0.0" || HOST === "::") {
		const ifaces = os.networkInterfaces();
		for (const entries of Object.values(ifaces)) {
			for (const info of entries || []) {
				if (info.family === "IPv4" && !info.internal) {
					console.log(`Mobile / LAN URL: http://${info.address}:${PORT}`);
				}
			}
		}
	}
	console.log(`Virtual microSD: ${SD_DIR}`);
	console.log(`Virtual config:  ${CONFIG_DIR}`);
	// Incremental make also checks an existing binary against working-tree edits.
	rebuildAndStart("normal");
});
