/**
 * In-browser Emscripten WebAssembly runtime for the TempoTec Variations V1 emulator.
 *
 * Supports:
 *   - Loading self-contained `.sonix-web.zip` firmware bundles from:
 *       1) GitHub Pages catalog (`builds/manifest.json`)
 *       2) Drag-and-drop / local file picker (`.sonix-web.zip`)
 *       3) Direct HTTPS URL (e.g., GitHub Release or CDN bundle URL)
 *   - Pure-JS ZIP extraction via browser DecompressionStream('deflate-raw')
 *   - IndexedDB persistence for custom imported builds, virtual microSD files,
 *     and device_config.ini across firmware swaps and page refreshes
 *   - Full MEMFS Virtual microSD CRUD, live Battery/Charging files, AirPods
 *     fake telemetry, on-device PNG screenshots, and real-time PCM / RGB565 hooks.
 */

import { unpackFirmware } from "./firmware-bundle.js";

const IDB_NAME = "sonix_v1_wasm_emu_v1";
const IDB_STORE_BUILDS = "builds";
const IDB_STORE_SD = "sd_files";
const IDB_STORE_META = "meta";

// In-memory fallback used automatically when IndexedDB is unavailable or restricted
// (e.g., iOS Safari Private Browsing or embedded mobile WebViews).
const memFallback = {
	[IDB_STORE_BUILDS]: new Map(),
	[IDB_STORE_SD]: new Map(),
	[IDB_STORE_META]: new Map(),
};
const STORE_KEY_FIELD = {
	[IDB_STORE_BUILDS]: "id",
	[IDB_STORE_SD]: "path",
	[IDB_STORE_META]: "key",
};
let dbPromise;

function openDb() {
	return dbPromise ??= new Promise((resolve) => {
		try {
			// Even reading window.indexedDB can throw in a restricted iframe.
			const req = indexedDB.open(IDB_NAME, 1);
			req.onupgradeneeded = () => {
				const db = req.result;
				if (!db.objectStoreNames.contains(IDB_STORE_BUILDS)) db.createObjectStore(IDB_STORE_BUILDS, { keyPath: "id" });
				if (!db.objectStoreNames.contains(IDB_STORE_SD)) db.createObjectStore(IDB_STORE_SD, { keyPath: "path" });
				if (!db.objectStoreNames.contains(IDB_STORE_META)) db.createObjectStore(IDB_STORE_META, { keyPath: "key" });
			};
			req.onsuccess = () => {
				const db = req.result;
				db.onversionchange = () => { db.close(); dbPromise = null; };
				db.onclose = () => { dbPromise = null; };
				resolve(db);
			};
			req.onerror = () => resolve(null);
		} catch {
			resolve(null);
		}
	});
}

async function idbGetAll(storeName) {
	const db = await openDb();
	const memory = memFallback[storeName];
	if (!db) return [...memory.values()];
	try {
		const stored = await new Promise((resolve, reject) => {
			const tx = db.transaction(storeName, "readonly");
			const req = tx.objectStore(storeName).getAll();
			req.onsuccess = () => resolve(req.result || []);
			req.onerror = () => reject(tx.error || req.error);
		});
		return [...stored.filter((row) => !memory.has(row[STORE_KEY_FIELD[storeName]])), ...memory.values()];
	} catch {
		return [...memory.values()];
	}
}

async function idbGet(storeName, key) {
	if (memFallback[storeName].has(key)) return memFallback[storeName].get(key);
	const db = await openDb();
	if (!db) return null;
	try {
		return await new Promise((resolve, reject) => {
			const tx = db.transaction(storeName, "readonly");
			const req = tx.objectStore(storeName).get(key);
			req.onsuccess = () => resolve(req.result || null);
			req.onerror = () => reject(tx.error || req.error);
		});
	} catch {
		return memFallback[storeName]?.get(key) || null;
	}
}

async function idbPut(storeName, value) {
	const key = value[STORE_KEY_FIELD[storeName]];
	const memory = memFallback[storeName];
	memory.set(key, value);
	const db = await openDb();
	if (!db) return;
	try {
		await new Promise((resolve, reject) => {
			const tx = db.transaction(storeName, "readwrite");
			tx.objectStore(storeName).put(value);
			tx.oncomplete = () => resolve();
			tx.onerror = tx.onabort = () => reject(tx.error);
		});
		if (memory.get(key) === value) memory.delete(key);
	} catch {
		// This write (possibly a rebuild of an existing ID) is newer than disk.
	}
}

async function idbDelete(storeName, key) {
	memFallback[storeName]?.delete(key);
	const db = await openDb();
	if (!db) return;
	try {
		await new Promise((resolve, reject) => {
			const tx = db.transaction(storeName, "readwrite");
			tx.objectStore(storeName).delete(key);
			tx.oncomplete = () => resolve();
			tx.onerror = tx.onabort = () => reject(tx.error);
		});
	} catch {}
}

/* -------------------------------------------------------------------------
 * Built-in Demo WAV Synthesizer (44.1 kHz & 96 kHz Hi-Res)
 * ------------------------------------------------------------------------- */
function makeWavBytes({ sampleRate, durationSec, title, artist, album, notes }) {
	const channels = 2;
	const numFrames = Math.floor(sampleRate * durationSec);
	const dataBytes = numFrames * channels * 2;
	const enc = new TextEncoder();

	function infoSubchunk(id, text) {
		const raw = enc.encode(text + "\0");
		const pad = raw.length % 2;
		const buf = new Uint8Array(8 + raw.length + pad);
		for (let i = 0; i < 4; i++) buf[i] = id.charCodeAt(i);
		new DataView(buf.buffer).setUint32(4, raw.length, true);
		buf.set(raw, 8);
		return buf;
	}

	const infoParts = [
		infoSubchunk("INAM", title),
		infoSubchunk("IART", artist),
		infoSubchunk("IPRD", album),
		infoSubchunk("IGNR", "Electronic"),
		infoSubchunk("ICRD", "2026"),
	];
	let infoLen = 4;
	for (const p of infoParts) infoLen += p.length;
	const listChunk = new Uint8Array(8 + infoLen);
	listChunk.set(enc.encode("LIST"), 0);
	new DataView(listChunk.buffer).setUint32(4, infoLen, true);
	listChunk.set(enc.encode("INFO"), 8);
	let lo = 12;
	for (const p of infoParts) {
		listChunk.set(p, lo);
		lo += p.length;
	}

	const totalSize = 44 + dataBytes + listChunk.length;
	const out = new Uint8Array(totalSize);
	const dv = new DataView(out.buffer);
	out.set(enc.encode("RIFF"), 0);
	dv.setUint32(4, totalSize - 8, true);
	out.set(enc.encode("WAVE"), 8);
	out.set(enc.encode("fmt "), 12);
	dv.setUint32(16, 16, true);
	dv.setUint16(20, 1, true);
	dv.setUint16(22, channels, true);
	dv.setUint32(24, sampleRate, true);
	dv.setUint32(28, sampleRate * channels * 2, true);
	dv.setUint16(32, channels * 2, true);
	dv.setUint16(34, 16, true);
	out.set(enc.encode("data"), 36);
	dv.setUint32(40, dataBytes, true);

	const stepDur = durationSec / notes.length;
	let p = 44;
	for (let i = 0; i < numFrames; i++) {
		const t = i / sampleRate;
		const noteIdx = Math.min(notes.length - 1, Math.floor(t / stepDur));
		const localT = t - noteIdx * stepDur;
		const env = Math.min(1, localT / 0.025) * Math.exp(-localT * 1.45);
		const fade = Math.min(1, (durationSec - t) / 0.35);
		const chord = notes[noteIdx];
		let l = 0;
		let r = 0;
		for (let c = 0; c < chord.length; c++) {
			const f = chord[c];
			const phase = 2 * Math.PI * f * t;
			const tone = Math.sin(phase) * 0.55 + Math.sin(phase * 2) * 0.22 + Math.sin(phase * 3) * 0.08;
			const pan = chord.length > 1 ? c / (chord.length - 1) : 0.5;
			l += tone * (0.65 + 0.35 * (1 - pan));
			r += tone * (0.65 + 0.35 * pan);
		}
		const gain = 0.22 * env * fade;
		const sL = Math.max(-32767, Math.min(32767, Math.round(l * gain * 32767)));
		const sR = Math.max(-32767, Math.min(32767, Math.round(r * gain * 32767)));
		dv.setInt16(p, sL, true);
		dv.setInt16(p + 2, sR, true);
		p += 4;
	}
	out.set(listChunk, 44 + dataBytes);
	return out;
}

function createDemoTracks() {
	return [
		{
			path: "Music/TempoTec Demo/01 - Velvet Rhodes (44.1kHz).wav",
			data: makeWavBytes({
				sampleRate: 44100,
				durationSec: 12,
				title: "Velvet Rhodes (44.1kHz)",
				artist: "TempoTec V1 Lab",
				album: "Emulator Demo Suite",
				notes: [
					[220.0, 261.63, 329.63, 392.0],
					[174.61, 220.0, 261.63, 329.63],
					[196.0, 246.94, 293.66, 392.0],
					[164.81, 246.94, 329.63, 392.0],
					[220.0, 261.63, 329.63, 440.0],
					[196.0, 293.66, 349.23, 440.0],
				],
			}),
		},
		{
			path: "Music/TempoTec Demo/02 - Crystal Arpeggio (96kHz).wav",
			data: makeWavBytes({
				sampleRate: 96000,
				durationSec: 8,
				title: "Crystal Arpeggio (96kHz Hi-Res)",
				artist: "TempoTec V1 Lab",
				album: "Emulator Demo Suite",
				notes: [
					[261.63, 329.63, 392.0, 523.25],
					[293.66, 369.99, 440.0, 587.33],
					[329.63, 415.3, 493.88, 659.25],
					[261.63, 392.0, 523.25, 659.25],
				],
			}),
		},
	];
}

const DEFAULT_DEVICE_CONFIG = [
	"# sonix_player settings. Rewritten by the player; hand edits are read back.",
	"",
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
].join("\n");

function mkdirpFS(FS, dir) {
	const parts = dir.split("/").filter(Boolean);
	let cur = "";
	for (const p of parts) {
		cur += "/" + p;
		try {
			FS.mkdir(cur);
		} catch {}
	}
}

function loadScriptFromBlobUrl(url) {
	return new Promise((resolve, reject) => {
		const s = document.createElement("script");
		s.src = url;
		s.async = true;
		s.onload = () => {
			s.remove();
			resolve();
		};
		s.onerror = (err) => {
			s.remove();
			reject(new Error("Failed to load Emscripten JS module from bundle"));
		};
		document.head.appendChild(s);
	});
}

/* -------------------------------------------------------------------------
 * SonixWasmBackend — In-browser TempoTec V1 Engine
 * ------------------------------------------------------------------------- */
export class SonixWasmBackend {
	constructor(callbacks) {
		this.cb = callbacks;
		this.module = null;
		this.activeBundle = null; // { meta, jsBytes, wasmBytes, workerBytes?, usrFiles: Map }
		this.jsBlobUrl = null;
		this.wasmBlobUrl = null;
		this.workerBlobUrl = null;
		this.bootSeq = 0;

		this.state = {
			lifecycle: "stopped",
			runtime: "wasm",
			pid: null,
			board: "TempoTec V1",
			width: 240,
			height: 320,
			rotated: false,
			screenOn: true,
			chargeScreen: false,
			backlight: { value: 99, max: 100, percent: 99 },
			volume: 40,
			battery: { percent: 82, status: "Discharging" },
			airpods: { enabled: false, model: "A3047", left: 85, right: 80, caseBat: 65, noise: 2 },
			led: { mode: "off", r: 0, g: 0, b: 0, r_step: 0, g_step: 0, b_step: 0, rise_ms: 0, fall_ms: 0, period_ms: 0 },
			audioRate: 0,
			audioChannels: 0,
			audioActive: false,
			fps: 0,
			build: null,
		};

		this._frameCounter = 0;
		this._fpsTimer = null;
		this._audioIdleTimer = null;
		this._lastConfigText = DEFAULT_DEVICE_CONFIG;
		this._sdSyncTimer = null;
	}

	async initStorage() {
		const savedCfg = await idbGet(IDB_STORE_META, "device_config.ini");
		if (savedCfg && typeof savedCfg.value === "string") {
			this._lastConfigText = savedCfg.value;
		}
		const existingSd = await idbGetAll(IDB_STORE_SD);
		if (existingSd.length === 0) {
			const demos = createDemoTracks();
			for (const track of demos) {
				await idbPut(IDB_STORE_SD, { path: track.path, data: track.data, mtime: Date.now() });
			}
		}
	}

	async listSavedCustomBuilds() {
		const all = await idbGetAll(IDB_STORE_BUILDS);
		return all
			.map((b) => ({
				id: b.id,
				name: b.name,
				ref: b.ref || "local",
				shortCommit: b.shortCommit || "custom",
				builtAt: b.builtAt,
				size: b.size || b.zipBuffer?.byteLength || 0,
				source: "custom",
			}))
			.sort((a, b) => String(b.builtAt || "").localeCompare(String(a.builtAt || "")));
	}

	async getSavedCustomBuildBuffer(id) {
		const rec = await idbGet(IDB_STORE_BUILDS, id);
		// Older records store ArrayBuffers. A Blob lets catalog reads clone
		// handles rather than every previously imported firmware's bytes.
		return rec?.zipBuffer instanceof Blob ? rec.zipBuffer.arrayBuffer() : rec?.zipBuffer;
	}

	async deleteSavedCustomBuild(id) {
		await idbDelete(IDB_STORE_BUILDS, id);
	}

	_emitState() {
		this.cb.onState?.({ ...this.state });
	}

	_emitLog(stream, line) {
		this.cb.onLog?.({ ts: Date.now(), stream, line: String(line) });
	}

	async loadBundleFromUrl(url, options = {}) {
		options.progress?.(`Downloading ${options.name || url}...`, 0.05);
		let res;
		try {
			res = await fetch(url, { cache: "no-store", signal: options.signal });
		} catch (err) {
			if (options.signal?.aborted) throw err;
			throw new Error(`Cannot fetch ${url}. Use a direct bundle URL that allows CORS, or download the ZIP and open it here.`);
		}
		if (!res.ok) {
			throw new Error(`HTTP ${res.status} fetching firmware bundle from ${url}`);
		}
		if (res.headers.get("content-type")?.includes("text/html")) {
			throw new Error("This URL returned a web page. Use a direct bundle URL or download the Actions artifact and browse to its ZIP.");
		}
		const contentLength = Number(res.headers.get("content-length") || 0);
		let arrayBuffer;
		if (res.body && contentLength > 0) {
			const reader = res.body.getReader();
			const chunks = [];
			let received = 0;
			while (true) {
				const { done, value } = await reader.read();
				if (done) break;
				chunks.push(value);
				received += value.byteLength;
				const pct = Math.min(0.65, (received / contentLength) * 0.65);
				options.progress?.(
					`Downloading ${options.name || "bundle"} (${(received / 1048576).toFixed(1)} / ${(contentLength / 1048576).toFixed(1)} MB)...`,
					pct
				);
			}
			const merged = new Uint8Array(received);
			let off = 0;
			for (const c of chunks) {
				merged.set(c, off);
				off += c.byteLength;
			}
			arrayBuffer = merged.buffer;
		} else {
			arrayBuffer = await res.arrayBuffer();
		}
		return this.loadBundleFromBuffer(arrayBuffer, options);
	}

	async loadBundleFromBuffer(arrayBuffer, options = {}) {
		options.progress?.("Unpacking firmware bundle...", 0.7);
		const { manifest, ...bundle } = await unpackFirmware(arrayBuffer, (done, total) => {
			options.signal?.throwIfAborted();
			options.progress?.(`Unpacking bundle (${done}/${total} assets)...`, 0.7 + (done / total) * 0.2);
		});

		const meta = {
			id: manifest?.id || options.id || `custom-${Date.now()}`,
			name: manifest?.name || options.name || "Custom Sonix Build",
			ref: manifest?.ref || options.ref || "local",
			commit: manifest?.commit || options.commit || "",
			shortCommit: manifest?.shortCommit || options.shortCommit || "local",
			subject: manifest?.subject || options.subject || "",
			builtAt: manifest?.builtAt || options.builtAt || new Date().toISOString(),
			size: arrayBuffer.byteLength,
			source: options.saveCustom ? "custom" : options.source || "url",
		};

		await (this.storageReady ??= this.initStorage());
		options.signal?.throwIfAborted();
		// Do not disconnect a working native bridge (or stop the previous WASM
		// build) until we actually have a web bundle to replace it with.
		this.cb.onActivate?.();
		this.activeBundle = { meta, ...bundle };
		options.progress?.(`Starting ${meta.name} — waiting for the display...`, 0.95);
		await this.boot({ signal: options.signal });
		if (options.saveCustom) await idbPut(IDB_STORE_BUILDS, { ...meta, zipBuffer: new Blob([arrayBuffer]) });
		return meta;
	}

	async stop() {
		++this.bootSeq; // Ignore late frames, exits and power events from the old module.
		this.cancelBoot?.();
		this.cancelBoot = null;
		clearTimeout(this._audioIdleTimer);
		this._persistMemfsState();
		if (this._fpsTimer) {
			clearInterval(this._fpsTimer);
			this._fpsTimer = null;
		}
		if (this._sdSyncTimer) {
			clearInterval(this._sdSyncTimer);
			this._sdSyncTimer = null;
		}
		const mod = this.module;
		this.module = null;
		const urls = [this.jsBlobUrl, this.wasmBlobUrl, this.workerBlobUrl];
		this.jsBlobUrl = this.wasmBlobUrl = this.workerBlobUrl = null;
		this.state.lifecycle = "stopped";
		this.state.audioActive = false;
		this.state.fps = 0;
		this.state.led = { mode: "off", r: 0, g: 0, b: 0, r_step: 0, g_step: 0, b_step: 0, rise_ms: 0, fall_ms: 0, period_ms: 0 };
		this._emitState();
		if (mod) {
			try { mod._webemu_wasm_quit?.(); } catch {}
			await new Promise((r) => setTimeout(r, 80));
			try { mod.PThread?.terminateAllThreads?.(); } catch {}
		}
		for (const url of urls) if (url) URL.revokeObjectURL(url);
	}

	_persistMemfsState() {
		if (!this.module || !this.module.FS) return;
		const FS = this.module.FS;
		try {
			const cfgBytes = FS.readFile("/config/device_config.ini");
			const text = new TextDecoder().decode(cfgBytes);
			if (text && text.length > 10) {
				this._lastConfigText = text;
				idbPut(IDB_STORE_META, { key: "device_config.ini", value: text }).catch(() => {});
			}
		} catch {}
	}

	async boot({ chargeScreen = false, signal } = {}) {
		if (!this.activeBundle) {
			throw new Error("No firmware bundle loaded yet");
		}
		const stopping = this.stop();
		const seq = this.bootSeq;
		await stopping;
		const superseded = () => new DOMException("Firmware boot superseded", "AbortError");
		if (seq !== this.bootSeq || signal?.aborted) throw superseded();
		this.state.build = this.activeBundle.meta;
		this.state.error = null;

		this.state.lifecycle = "booting";
		this.state.chargeScreen = !!chargeScreen;
		this.state.screenOn = true;
		this.state.rotated = false;
		this._emitState();
		this._emitLog("emu", `Booting WebAssembly build: ${this.activeBundle.meta.name}`);

		// With PROXY_TO_PTHREAD the factory can resolve before C main has
		// even started. Only call this a successful boot after the first frame.
		let frameReady, fail;
		const firstFrame = new Promise((resolve) => { frameReady = resolve; });
		const failed = new Promise((_, reject) => { fail = reject; });
		const cancel = () => fail(superseded());
		this.cancelBoot = cancel;
		signal?.addEventListener("abort", cancel, { once: true });
		const timer = setTimeout(() => fail(new Error("No display frame from this firmware after 45 seconds. See Live Logs.")), 45000);
		try {
			const ready = Promise.all([this._createModule(seq, chargeScreen, frameReady, fail), firstFrame]);
			const [mod] = await Promise.race([ready, failed]);
			if (seq !== this.bootSeq || signal?.aborted) throw superseded();
			this.module = mod;
		} catch (err) {
			if (seq === this.bootSeq) {
				const stopping = this.stop();
				if (err.name !== "AbortError") {
					this.state.lifecycle = "crashed";
					this.state.error = err.message;
					this._emitState();
				}
				await stopping;
			}
			throw err;
		} finally {
			clearTimeout(timer);
			signal?.removeEventListener("abort", cancel);
			if (this.cancelBoot === cancel) this.cancelBoot = null;
		}
		this.state.lifecycle = "running";
		this._emitState();

		this._fpsTimer = setInterval(() => {
			this.state.fps = this._frameCounter;
			this._frameCounter = 0;
			this.cb.onEvent?.({ type: "metrics", fps: this.state.fps });
		}, 1000);

		this._sdSyncTimer = setInterval(() => {
			this._persistMemfsState();
		}, 5000);

		this.cb.onSdChanged?.();
	}

	async _createModule(seq, chargeScreen, frameReady, fail) {
		const sdFiles = await idbGetAll(IDB_STORE_SD);
		if (seq !== this.bootSeq) throw new DOMException("Boot cancelled", "AbortError");
		const bundle = this.activeBundle;
		const jsBlob = new Blob([bundle.jsBytes], { type: "application/javascript" });
		this.jsBlobUrl = URL.createObjectURL(jsBlob);
		this.wasmBlobUrl = null;

		window.createSonixPlayer = undefined;
		await loadScriptFromBlobUrl(this.jsBlobUrl);
		if (seq !== this.bootSeq) throw new DOMException("Boot cancelled", "AbortError");
		if (typeof window.createSonixPlayer !== "function") {
			throw new Error("createSonixPlayer factory not exported by bundle JS");
		}

		const factory = window.createSonixPlayer;
		const self = this;
		const options = {
			mainScriptUrlOrBlob: this.jsBlobUrl,
			wasmBinary: bundle.wasmBytes,
			locateFile: (p) => {
				if (p.endsWith(".worker.js")) {
					if (!bundle.workerBytes) throw new Error(`Bundle is missing the pthread helper ${p}. Rebuild it with web-wasm-bundle.`);
					return self.workerBlobUrl ??= URL.createObjectURL(new Blob([bundle.workerBytes], { type: "application/javascript" }));
				}
				if (p.endsWith(".wasm")) {
					if (!self.wasmBlobUrl) {
						self.wasmBlobUrl = URL.createObjectURL(
							new Blob([bundle.wasmBytes], { type: "application/wasm" })
						);
					}
					return self.wasmBlobUrl;
				}
				return p;
			},
			print: (line) => self._emitLog("stdout", line),
			printErr: (line) => self._emitLog("stderr", line),
			onAbort: (what) => {
				if (seq !== self.bootSeq) return;
				const err = new Error(`WASM abort: ${what}`);
				self._emitLog("stderr", err.message);
				self.state.error = err.message;
				self.state.lifecycle = "crashed";
				self._emitState();
				fail(err);
			},
			onExit: (code) => {
				if (seq !== self.bootSeq) return;
				self._emitLog("emu", `sonix_player_wasm exited (code=${code})`);
				if (self.state.lifecycle === "booting") {
					fail(new Error(`Firmware exited during boot (code ${code}). See Live Logs.`));
				} else if (code === 43) {
					self._schedulePowerAction("boot", 250);
				} else {
					self.state.lifecycle = "stopped";
					self.state.audioActive = false;
					self._emitState();
				}
			},
			preRun: [
				(M) => {
					if (seq !== self.bootSeq) throw new DOMException("Boot cancelled", "AbortError");
					M.ENV.SONIX_PANEL = "v1";
					M.ENV.SONIX_SD_ROOT = "/sd";
					M.ENV.SONIX_CONFIG = "/config/device_config.ini";
					M.ENV.SONIX_EBOOK_CONFIG = "/config/ebook_config.ini";
					M.ENV.SONIX_BATTERY_CAPACITY = "/tmp/sonix-emu/battery_capacity";
					M.ENV.SONIX_BATTERY_STATUS = "/tmp/sonix-emu/battery_status";
					M.ENV.SONIX_NO_MOUNT = "1";
					M.ENV.SONIX_NO_SYSSERVER = "1";
					if (chargeScreen) {
						M.ENV.SONIX_CHARGE_SCREEN = "1";
					}
					if (self.state.airpods.enabled) {
						const ap = self.state.airpods;
						M.ENV.SONIX_AIRPODS_FAKE = `${ap.model},${ap.left},${ap.right},${ap.caseBat},${ap.noise}`;
					}

					// Populate TempoTec V1 /usr assets from the loaded bundle
					for (const [relPath, bytes] of bundle.usrFiles.entries()) {
						const fullPath = "/" + relPath;
						const dir = fullPath.slice(0, fullPath.lastIndexOf("/"));
						mkdirpFS(M.FS, dir);
						M.FS.writeFile(fullPath, bytes);
					}

					// Populate virtual microSD + config + battery sysfs files
					mkdirpFS(M.FS, "/sd/Music");
					mkdirpFS(M.FS, "/sd/Books");
					mkdirpFS(M.FS, "/sd/Themes");
					mkdirpFS(M.FS, "/sd/Fonts");
					mkdirpFS(M.FS, "/sd/Screenshots");
					mkdirpFS(M.FS, "/config");
					mkdirpFS(M.FS, "/tmp/sonix-emu");

					for (const rec of sdFiles) {
						const fullPath = "/sd/" + String(rec.path).replace(/^\/+/, "");
						const dir = fullPath.slice(0, fullPath.lastIndexOf("/"));
						mkdirpFS(M.FS, dir);
						M.FS.writeFile(fullPath, rec.data instanceof Uint8Array ? rec.data : new Uint8Array(rec.data));
					}

					M.FS.writeFile("/config/device_config.ini", self._lastConfigText);
					M.FS.writeFile("/tmp/sonix-emu/battery_capacity", `${self.state.battery.percent}\n`);
					M.FS.writeFile("/tmp/sonix-emu/battery_status", `${self.state.battery.status}\n`);
					M.FS.chdir("/");

					M._sonixBridge = {
						onFrame(view, w, h) {
							if (seq !== self.bootSeq || !(w > 0 && h > 0 && view?.byteLength >= w * h * 2)) return;
							self._frameCounter++;
							self.state.width = w;
							self.state.height = h;
							self.cb.onFrame?.(view, w, h);
							frameReady();
						},
						onEvent(jsonStr) {
							if (seq !== self.bootSeq) return;
							try {
								const ev = JSON.parse(jsonStr);
								self._handleFirmwareEvent(ev);
							} catch {}
						},
						onPcm(view, rate, channels) {
							if (seq !== self.bootSeq) return;
							if (!self.state.audioActive || self.state.audioRate !== rate || self.state.audioChannels !== channels) {
								self.state.audioActive = true;
								self.state.audioRate = rate;
								self.state.audioChannels = channels;
								self.cb.onEvent?.({ type: "audio_info", active: true, rate, channels });
							}
							if (self._audioIdleTimer) clearTimeout(self._audioIdleTimer);
							self._audioIdleTimer = setTimeout(() => {
								self.state.audioActive = false;
								self.cb.onEvent?.({ type: "audio_info", active: false, rate: self.state.audioRate, channels: self.state.audioChannels });
							}, 450);
							self.cb.onPcm?.(view, rate, channels);
						},
					};
				},
			],
		};
		// Emscripten populates the options object during initialization. Keep
		// it so stop() can terminate a partial pthread pool on boot failure.
		this.module = options;
		const mod = await factory(options);
		if (seq !== this.bootSeq) {
			try { mod.PThread?.terminateAllThreads?.(); } catch {}
			throw new DOMException("Boot cancelled", "AbortError");
		}
		return mod;
	}

	_schedulePowerAction(action, delay) {
		const seq = this.bootSeq;
		setTimeout(() => {
			if (seq === this.bootSeq) this.powerAction(action).catch((err) => this._emitLog("stderr", err.message));
		}, delay);
	}

	_handleFirmwareEvent(ev) {
		switch (ev.type) {
			case "led":
				this.state.led = {
					mode: ev.mode || "off",
					r: ev.r || 0,
					g: ev.g || 0,
					b: ev.b || 0,
					r_step: ev.r_step || 0,
					g_step: ev.g_step || 0,
					b_step: ev.b_step || 0,
					rise_ms: ev.rise_ms || 0,
					fall_ms: ev.fall_ms || 0,
					period_ms: ev.period_ms || 0,
				};
				break;
			case "backlight":
				this.state.backlight = { value: ev.value, max: ev.max, percent: ev.percent };
				break;
			case "screen":
				this.state.screenOn = !!ev.on;
				break;
			case "rotated":
				this.state.rotated = !!ev.rotated;
				break;
			case "volume":
				this.state.volume = ev.percent;
				break;
			case "power":
				if (ev.action === "reboot") {
					this._schedulePowerAction("boot", 250);
				} else if (ev.action === "poweroff") {
					this._schedulePowerAction("poweroff", 150);
				}
				break;
		}
		this.cb.onEvent?.(ev);
	}

	sendPointer(x, y, down) {
		if (!this.module || !this.module._webemu_wasm_pointer) return;
		this.module._webemu_wasm_pointer(x | 0, y | 0, down ? 1 : 0);
	}

	sendKey(button, down) {
		if (!this.module || !this.module.ccall) return;
		this.module.ccall("webemu_wasm_key", null, ["string", "number"], [String(button), down ? 1 : 0]);
	}

	setBattery(percent, status) {
		const pct = Math.max(0, Math.min(100, Math.round(Number(percent) || 0)));
		const st = ["Charging", "Discharging", "Full", "Not charging"].includes(status) ? status : "Discharging";
		this.state.battery = { percent: pct, status: st };
		if (this.module && this.module.FS) {
			try {
				this.module.FS.writeFile("/tmp/sonix-emu/battery_capacity", `${pct}\n`);
				this.module.FS.writeFile("/tmp/sonix-emu/battery_status", `${st}\n`);
			} catch {}
		}
		this.cb.onEvent?.({ type: "battery", battery: this.state.battery });
	}

	async setAirpods(cfg) {
		this.state.airpods = {
			enabled: !!cfg.enabled,
			model: String(cfg.model || "A3047"),
			left: Math.max(0, Math.min(100, Number(cfg.left ?? 85))),
			right: Math.max(0, Math.min(100, Number(cfg.right ?? 80))),
			caseBat: Math.max(0, Math.min(100, Number(cfg.caseBat ?? 65))),
			noise: Math.max(0, Math.min(3, Number(cfg.noise ?? 2))),
		};
		this.cb.onEvent?.({ type: "airpods", airpods: this.state.airpods });
		if (this.activeBundle) {
			await this.boot({ chargeScreen: this.state.chargeScreen });
		}
	}

	async powerAction(action) {
		if (action === "boot" || action === "reboot") {
			await this.boot({ chargeScreen: false });
		} else if (action === "charge_screen") {
			this.setBattery(this.state.battery.percent, "Charging");
			await this.boot({ chargeScreen: true });
		} else if (action === "poweroff") {
			await this.stop();
		} else if (action === "reset_config") {
			this._lastConfigText = DEFAULT_DEVICE_CONFIG;
			await idbPut(IDB_STORE_META, { key: "device_config.ini", value: DEFAULT_DEVICE_CONFIG });
			await this.boot({ chargeScreen: false });
		}
	}

	requestDeviceScreenshot() {
		if (!this.module || !this.module._webemu_wasm_screenshot) return;
		const before = new Set(this._listScreenshots());
		this.module._webemu_wasm_screenshot();
		setTimeout(() => {
			const after = this._listScreenshots();
			const added = after.filter((name) => !before.has(name));
			for (const name of added) {
				try {
					const data = this.module.FS.readFile(`/sd/Screenshots/${name}`);
					idbPut(IDB_STORE_SD, { path: `Screenshots/${name}`, data: new Uint8Array(data), mtime: Date.now() }).catch(() => {});
				} catch {}
			}
			this.cb.onSdChanged?.();
		}, 900);
	}

	_listScreenshots() {
		if (!this.module || !this.module.FS) return [];
		try {
			return this.module.FS.readdir("/sd/Screenshots").filter((n) => n.endsWith(".png"));
		} catch {
			return [];
		}
	}

	listSd(relDir = "") {
		const clean = String(relDir || "").replace(/^\/+|\/+$/g, "");
		if (!this.module || !this.module.FS) {
			return { dir: clean, items: [] };
		}
		const target = clean ? `/sd/${clean}` : "/sd";
		const FS = this.module.FS;
		try {
			const names = FS.readdir(target).filter((n) => n !== "." && n !== ".." && !n.startsWith("."));
			const items = [];
			for (const name of names) {
				const full = `${target}/${name}`;
				try {
					const st = FS.stat(full);
					const isDir = FS.isDir(st.mode);
					items.push({
						name,
						path: clean ? `${clean}/${name}` : name,
						isDir,
						size: isDir ? 0 : st.size,
						mtime: st.mtime ? st.mtime.getTime() : Date.now(),
					});
				} catch {}
			}
			items.sort((a, b) => (a.isDir !== b.isDir ? (a.isDir ? -1 : 1) : a.name.localeCompare(b.name)));
			return { dir: clean, items };
		} catch {
			return { dir: clean, items: [] };
		}
	}

	async uploadSdFile(dir, name, arrayBuffer) {
		const cleanDir = String(dir || "").replace(/^\/+|\/+$/g, "");
		const safeName = String(name || "upload.bin").replace(/[\\/]/g, "_");
		const relPath = cleanDir ? `${cleanDir}/${safeName}` : safeName;
		const bytes = new Uint8Array(arrayBuffer);
		if (this.module && this.module.FS) {
			const full = `/sd/${relPath}`;
			mkdirpFS(this.module.FS, full.slice(0, full.lastIndexOf("/")));
			this.module.FS.writeFile(full, bytes);
		}
		await idbPut(IDB_STORE_SD, { path: relPath, data: bytes, mtime: Date.now() });
		this.cb.onSdChanged?.();
		return { ok: true, path: relPath, size: bytes.length };
	}

	readSdFile(relPath) {
		if (!this.module || !this.module.FS) return null;
		const clean = String(relPath || "").replace(/^\/+/, "");
		try {
			return new Uint8Array(this.module.FS.readFile(`/sd/${clean}`));
		} catch {
			return null;
		}
	}

	async deleteSdFile(relPath) {
		const clean = String(relPath || "").replace(/^\/+/, "");
		if (this.module && this.module.FS) {
			const full = `/sd/${clean}`;
			try {
				const st = this.module.FS.stat(full);
				if (this.module.FS.isDir(st.mode)) {
					this._rmrfFS(full);
				} else {
					this.module.FS.unlink(full);
				}
			} catch {}
		}
		await idbDelete(IDB_STORE_SD, clean);
		this.cb.onSdChanged?.();
	}

	_rmrfFS(target) {
		const FS = this.module.FS;
		for (const name of FS.readdir(target)) {
			if (name === "." || name === "..") continue;
			const child = `${target}/${name}`;
			const st = FS.stat(child);
			if (FS.isDir(st.mode)) this._rmrfFS(child);
			else FS.unlink(child);
		}
		FS.rmdir(target);
	}
}
