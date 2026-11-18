/**
 * TempoTec Variations V1 — Interactive Browser Emulator Frontend
 *
 * Dual-runtime architecture:
 *   1) In-Browser WebAssembly Mode (GitHub Pages / static hosting, or `?mode=wasm`):
 *      Runs compiled `sonix_player_wasm.wasm` directly in browser pthreads via
 *      `./wasm-runtime.js`, with a universal `.sonix-web.zip` Firmware Build Loader
 *      (catalog switcher, drag-and-drop, file picker, URL fetch, IndexedDB persistence).
 *   2) Local Native Bridge Mode (when served by `tools/v1-emulator/server.mjs`):
 *      Streams RGB565 frames, ALSA PCM, and telemetry over `/ws` from native `sonix_player_web`.
 */

import { publishedBuilds } from "./build-catalog.js";
import { WEB_BUILD_HINT } from "./firmware-bundle.js";
import { SonixWasmBackend } from "./wasm-runtime.js";

/* --------------------------------------------------------------------------
 * Fast RGB565 lookup tables
 * -------------------------------------------------------------------------- */
const LUT5 = new Uint8Array(32);
const LUT6 = new Uint8Array(64);
for (let i = 0; i < 32; i++) LUT5[i] = Math.round((i * 255) / 31);
for (let i = 0; i < 64; i++) LUT6[i] = Math.round((i * 255) / 63);

/* --------------------------------------------------------------------------
 * DOM references
 * -------------------------------------------------------------------------- */
const el = {
	brandSub: document.getElementById("brand-sub"),
	runtimePill: document.getElementById("runtime-pill"),
	connPill: document.getElementById("conn-pill"),
	lifecyclePill: document.getElementById("lifecycle-pill"),
	fpsPill: document.getElementById("fps-pill"),
	audioPill: document.getElementById("audio-pill"),
	audioToggleBtn: document.getElementById("btn-audio-toggle"),
	audioDot: document.getElementById("audio-dot"),
	audioBtnLabel: document.getElementById("audio-btn-label"),
	screenshotBtn: document.getElementById("btn-screenshot"),
	deviceShotBtn: document.getElementById("btn-device-shot"),
	rebuildBtn: document.getElementById("btn-rebuild"),

	buildRuntimeBadge: document.getElementById("build-runtime-badge"),
	buildSelect: document.getElementById("build-select"),
	buildLatestBtn: document.getElementById("btn-build-latest"),
	buildResult: document.getElementById("build-result"),
	buildStandalone: document.getElementById("build-standalone"),
	buildLoadBtn: document.getElementById("btn-build-load"),
	buildDelBtn: document.getElementById("btn-build-del"),
	buildMeta: document.getElementById("build-meta"),
	buildProgress: document.getElementById("build-progress"),
	buildProgressText: document.getElementById("build-progress-text"),
	buildProgressFill: document.getElementById("build-progress-fill"),
	buildDropzone: document.getElementById("build-dropzone"),
	buildFileInput: document.getElementById("build-file-input"),
	buildUrlInput: document.getElementById("build-url-input"),
	buildUrlForm: document.getElementById("build-url-form"),

	canvas: document.getElementById("v1-canvas"),
	stageCanvasWrap: document.getElementById("stage-canvas-wrap"),
	screenStage: document.getElementById("v1-screen-stage"),
	backlightDim: document.getElementById("v1-backlight-dim"),
	shutterFlash: document.getElementById("v1-shutter-flash"),
	touchRipple: document.getElementById("v1-touch-ripple"),
	screenOverlay: document.getElementById("v1-screen-overlay"),
	overlayTitle: document.getElementById("overlay-title"),
	overlaySub: document.getElementById("overlay-sub"),
	overlayBootBtn: document.getElementById("overlay-boot-btn"),
	rotBadge: document.getElementById("v1-rot-badge"),
	usbcPort: document.getElementById("v1-usbc"),
	topJacks: document.getElementById("v1-top-jacks"),
	sdSlotBtn: document.getElementById("v1-sd-slot"),
	v1Assembly: document.getElementById("v1-assembly"),
	v1ChassisBack: document.getElementById("v1-chassis-back"),
	flipDeviceBtn: document.getElementById("btn-flip-device"),

	ledGlow: document.getElementById("v1-led-glow"),
	ledCore: document.getElementById("v1-led-core"),
	ledInspectorOrb: document.getElementById("led-inspector-orb"),
	ledModeBadge: document.getElementById("led-mode-badge"),
	ledHex: document.getElementById("led-hex"),
	ledSteps: document.getElementById("led-steps"),
	ledTiming: document.getElementById("led-timing"),

	teleLedDot: document.getElementById("tele-led-dot"),
	teleLedText: document.getElementById("tele-led-text"),
	teleBacklight: document.getElementById("tele-backlight"),
	teleVolume: document.getElementById("tele-volume"),
	teleBattery: document.getElementById("tele-battery"),
	teleScreen: document.getElementById("tele-screen"),

	batSlider: document.getElementById("bat-slider"),
	batReadout: document.getElementById("bat-readout"),

	holdPowerBtn: document.getElementById("btn-hold-power"),
	comboShotBtn: document.getElementById("btn-combo-shot"),
	chargeScreenBtn: document.getElementById("btn-charge-screen"),
	rebootBtn: document.getElementById("btn-reboot"),
	powerToggleBtn: document.getElementById("btn-power-toggle"),
	resetConfigBtn: document.getElementById("btn-reset-config"),

	airpodsEnable: document.getElementById("airpods-enable"),
	airpodsControls: document.getElementById("airpods-controls"),
	airpodsModel: document.getElementById("airpods-model"),
	apL: document.getElementById("ap-l"),
	apR: document.getElementById("ap-r"),
	apC: document.getElementById("ap-c"),
	apLVal: document.getElementById("ap-l-val"),
	apRVal: document.getElementById("ap-r-val"),
	apCVal: document.getElementById("ap-c-val"),

	sdCrumbs: document.getElementById("sd-crumbs"),
	sdList: document.getElementById("sd-list"),
	sdDropzone: document.getElementById("sd-dropzone"),
	sdUploadInput: document.getElementById("sd-upload-input"),
	sdRefreshBtn: document.getElementById("btn-sd-refresh"),

	logBody: document.getElementById("log-body"),
	logCount: document.getElementById("log-count"),
	logFilter: document.getElementById("log-filter"),
	logAutoscroll: document.getElementById("log-autoscroll"),
	logCopyBtn: document.getElementById("btn-log-copy"),
	logClearBtn: document.getElementById("btn-log-clear"),

	shotModal: document.getElementById("shot-modal"),
	shotModalBackdrop: document.getElementById("shot-modal-backdrop"),
	shotModalClose: document.getElementById("shot-modal-close"),
	shotModalTitle: document.getElementById("shot-modal-title"),
	shotModalImg: document.getElementById("shot-modal-img"),
	shotModalDownload: document.getElementById("shot-modal-download"),

	toastStack: document.getElementById("toast-stack"),
};

const ctx = el.canvas.getContext("2d", { alpha: false });
let imgData = ctx.createImageData(240, 320);

/* --------------------------------------------------------------------------
 * Client state & dual-runtime controller
 * -------------------------------------------------------------------------- */
let runtimeMode = "detecting"; // "wasm" | "native"
let ws = null;
let wsClosedManually = false;
let reconnectDelay = 500;
let wasmBackend = null;
let catalogBuilds = []; // { id, name, ref, shortCommit, builtAt, size, url, source: 'catalog'|'custom' }
let buildBusyText = null;
let buildError = null;
let localServerAvailable = false;
let catalogRequest = 0;
let loadRequest = null;
let loadQueue = Promise.resolve();
let followedBuild = null; // null when pinned to an explicit build; "" while awaiting the first publish.
let checkingLatest = false;
let activeSdBlobUrls = [];
let deviceState = {
	lifecycle: "starting",
	mode: "normal",
	width: 240,
	height: 320,
	rotated: false,
	screenOn: true,
	backlight: { value: 200, max: 255, percent: 78 },
	led: {
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
	},
	volume: 50,
	battery: { percent: 82, status: "Discharging" },
	airpods: { enabled: false, model: "A3047", left: 85, right: 80, caseBat: 65, noise: 2 },
	fps: 0,
	audioRate: 0,
	audioActive: false,
};

let currentSdDir = "Music";
let activeStreamFilter = "all";
const allLogs = [];
const MAX_CLIENT_LOGS = 1500;

/* --------------------------------------------------------------------------
 * Toasts
 * -------------------------------------------------------------------------- */
function showToast(message) {
	const t = document.createElement("div");
	t.className = "toast";
	t.textContent = message;
	el.toastStack.appendChild(t);
	setTimeout(() => {
		t.style.opacity = "0";
		t.style.transition = "opacity 0.25s ease";
		setTimeout(() => t.remove(), 260);
	}, 2600);
}

/* --------------------------------------------------------------------------
 * Web Audio streaming player (virtual ALSA DAC -> browser speakers)
 * -------------------------------------------------------------------------- */
let audioEnabled = false;
let audioCtx = null;
let audioNextTime = 0;
let audioUnlocked = false;
const isCoarsePointer = () => window.matchMedia?.("(pointer: coarse)")?.matches ?? false;

function ensureAudioContext() {
	if (!audioCtx) {
		const AudioCtor = window.AudioContext || window.webkitAudioContext;
		if (!AudioCtor) {
			showToast("Web Audio API is unavailable in this browser.");
			return null;
		}
		audioCtx = new AudioCtor();
		audioNextTime = 0;
	}
	if (audioCtx.state === "suspended") {
		audioCtx.resume().catch(() => {});
	}
	// iOS Safari requires playing a silent buffer inside a direct user gesture to unlock audio output.
	if (!audioUnlocked && audioCtx) {
		try {
			const silent = audioCtx.createBuffer(1, 1, 22050);
			const src = audioCtx.createBufferSource();
			src.buffer = silent;
			src.connect(audioCtx.destination);
			src.start(0);
			audioUnlocked = true;
		} catch {}
	}
	return audioCtx;
}

function resumeAudioIfEnabled() {
	if (audioEnabled && audioCtx && audioCtx.state === "suspended") {
		audioCtx.resume().catch(() => {});
	}
}

function setAudioEnabled(on) {
	audioEnabled = on;
	el.audioDot.classList.toggle("on", on);
	el.audioBtnLabel.textContent = on ? "Audio: On" : "Audio: Off";
	el.topJacks?.classList.toggle("audio-active", on);
	if (on) {
		ensureAudioContext();
	}
	sendWsJson({ type: "audio_pref", enabled: on });
}

function schedulePcmBytes(pcmBytesView, rate, channels) {
	if (!audioEnabled || !audioCtx || audioCtx.state !== "running") return;
	if (rate < 8000 || rate > 384000 || channels < 1 || channels > 2) return;

	const totalSamples = Math.floor(pcmBytesView.byteLength / 2);
	const frames = Math.floor(totalSamples / channels);
	if (frames <= 0) return;

	let targetRate = rate;
	let outFrames = frames;
	let ratio = 1;
	if (rate > 96000) {
		targetRate = 48000;
		ratio = rate / targetRate;
		outFrames = Math.max(1, Math.floor(frames / ratio));
	}

	let buf;
	try {
		buf = audioCtx.createBuffer(channels, outFrames, targetRate);
	} catch {
		return;
	}

	const chL = buf.getChannelData(0);
	const chR = channels > 1 ? buf.getChannelData(1) : null;
	// Ensure 2-byte alignment for Int16Array
	const aligned =
		pcmBytesView.byteOffset % 2 === 0
			? new Int16Array(pcmBytesView.buffer, pcmBytesView.byteOffset, totalSamples)
			: new Int16Array(pcmBytesView.slice().buffer, 0, totalSamples);

	if (ratio === 1) {
		if (channels === 2) {
			for (let i = 0; i < outFrames; i++) {
				chL[i] = aligned[i * 2] / 32768;
				chR[i] = aligned[i * 2 + 1] / 32768;
			}
		} else {
			for (let i = 0; i < outFrames; i++) {
				chL[i] = aligned[i] / 32768;
			}
		}
	} else {
		for (let i = 0; i < outFrames; i++) {
			const srcIdx = Math.min(frames - 1, Math.floor(i * ratio));
			if (channels === 2) {
				chL[i] = aligned[srcIdx * 2] / 32768;
				chR[i] = aligned[srcIdx * 2 + 1] / 32768;
			} else {
				chL[i] = aligned[srcIdx] / 32768;
			}
		}
	}

	const src = audioCtx.createBufferSource();
	src.buffer = buf;
	src.connect(audioCtx.destination);

	const now = audioCtx.currentTime;
	const lead = isCoarsePointer() ? 0.065 : 0.04;
	if (audioNextTime < now + 0.015 || audioNextTime > now + 0.4) {
		audioNextTime = now + lead;
	}
	src.start(audioNextTime);
	audioNextTime += buf.duration;
}

function handleAudioPacket(view) {
	if (view.byteLength < 7) return;
	const rate = view.getUint32(1, false);
	const channels = view.getUint16(5, false);
	const pcmView = new Uint8Array(view.buffer, view.byteOffset + 7, view.byteLength - 7);
	schedulePcmBytes(pcmView, rate, channels);
}

/* --------------------------------------------------------------------------
 * Framebuffer RGB565 renderer
 * -------------------------------------------------------------------------- */
function renderRgb565Bytes(bytes, w, h) {
	const pxCount = w * h;
	if (w <= 0 || h <= 0 || bytes.byteLength < pxCount * 2) return;

	if (el.canvas.width !== w || el.canvas.height !== h) {
		el.canvas.width = w;
		el.canvas.height = h;
		imgData = ctx.createImageData(w, h);
	}

	const dst = imgData.data;
	for (let i = 0, s = 0, d = 0; i < pxCount; i++, s += 2, d += 4) {
		const px = bytes[s] | (bytes[s + 1] << 8); // RGB565 little-endian
		dst[d] = LUT5[(px >> 11) & 0x1f];
		dst[d + 1] = LUT6[(px >> 5) & 0x3f];
		dst[d + 2] = LUT5[px & 0x1f];
		dst[d + 3] = 255;
	}
	ctx.putImageData(imgData, 0, 0);
}

function handleFramePacket(view) {
	if (view.byteLength < 5) return;
	const w = view.getUint16(1, false);
	const h = view.getUint16(3, false);
	const expectedBytes = 5 + w * h * 2;
	if (w <= 0 || h <= 0 || view.byteLength < expectedBytes) return;
	const bytes = new Uint8Array(view.buffer, view.byteOffset + 5, w * h * 2);
	renderRgb565Bytes(bytes, w, h);
}

/* --------------------------------------------------------------------------
 * Live SGM31324 RGB LED animation loop (steady + hardware breathing curve)
 * -------------------------------------------------------------------------- */
let ledAnimStart = performance.now();

function updateLedMetadata() {
	const led = deviceState.led;
	const hex = `#${[led.r, led.g, led.b]
		.map((v) => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, "0"))
		.join("")
		.toUpperCase()}`;

	el.ledModeBadge.textContent = (led.mode || "off").toUpperCase();
	el.ledHex.textContent = hex;
	el.ledSteps.textContent = `R:${led.r_step} G:${led.g_step} B:${led.b_step}`;

	if (led.mode === "breathing") {
		el.ledTiming.textContent = `rise ${led.rise_ms}ms / fall ${led.fall_ms}ms / ${led.period_ms}ms`;
		el.teleLedText.textContent = `BREATH ${hex}`;
	} else if (led.mode === "steady") {
		el.ledTiming.textContent = "Steady";
		el.teleLedText.textContent = `ON ${hex}`;
	} else {
		el.ledTiming.textContent = "Off";
		el.teleLedText.textContent = "OFF";
	}
}

function renderLedFrame(now) {
	const led = deviceState.led;
	let intensity = 0;

	if (led.mode === "steady") {
		intensity = 1;
	} else if (led.mode === "breathing") {
		const rise = Math.max(150, led.rise_ms || 1200);
		const fall = Math.max(150, led.fall_ms || 1200);
		const period = Math.max(rise + fall + 100, led.period_ms || 3000);
		const phase = (now - ledAnimStart) % period;
		if (phase < rise) {
			const u = phase / rise;
			intensity = 0.5 - 0.5 * Math.cos(Math.PI * u);
		} else if (phase < rise + fall) {
			const u = (phase - rise) / fall;
			intensity = 0.5 + 0.5 * Math.cos(Math.PI * u);
		} else {
			intensity = 0;
		}
		intensity = 0.08 + 0.92 * intensity;
	}

	if (intensity <= 0.01 || (led.r === 0 && led.g === 0 && led.b === 0)) {
		el.ledCore.style.background = "rgba(20, 24, 32, 0.55)";
		el.ledCore.style.boxShadow = "inset 0 0 1px rgba(255,255,255,0.08)";
		el.ledGlow.style.opacity = "0";
		el.ledInspectorOrb.style.background = "#141923";
		el.ledInspectorOrb.style.boxShadow = "none";
		el.teleLedDot.style.background = "#222936";
		el.teleLedDot.style.boxShadow = "none";
	} else {
		const r = Math.min(255, Math.round(led.r * intensity));
		const g = Math.min(255, Math.round(led.g * intensity));
		const b = Math.min(255, Math.round(led.b * intensity));
		const coreR = Math.min(255, Math.round(r + (255 - r) * 0.42 * intensity));
		const coreG = Math.min(255, Math.round(g + (255 - g) * 0.42 * intensity));
		const coreB = Math.min(255, Math.round(b + (255 - b) * 0.42 * intensity));
		const rgb = `rgb(${r}, ${g}, ${b})`;
		const coreRgb = `rgb(${coreR}, ${coreG}, ${coreB})`;

		el.ledCore.style.background = `linear-gradient(180deg, ${rgb} 0%, ${coreRgb} 50%, ${rgb} 100%)`;
		el.ledCore.style.boxShadow = `0 0 ${Math.round(6 * intensity)}px ${coreRgb}, 0 0 ${Math.round(14 * intensity)}px ${rgb}`;
		el.ledGlow.style.background = `radial-gradient(ellipse at center, ${rgb} 0%, transparent 72%)`;
		el.ledGlow.style.opacity = String(Math.min(1, 0.92 * intensity));

		el.ledInspectorOrb.style.background = coreRgb;
		el.ledInspectorOrb.style.boxShadow = `0 0 ${Math.round(22 * intensity)}px ${rgb}`;

		el.teleLedDot.style.background = coreRgb;
		el.teleLedDot.style.boxShadow = `0 0 8px ${rgb}`;
	}

	requestAnimationFrame(renderLedFrame);
}
requestAnimationFrame(renderLedFrame);

/* --------------------------------------------------------------------------
 * UI State Synchronization
 * -------------------------------------------------------------------------- */
function syncUiFromState() {
	// Rebuild always targets this server's working tree, even after a WASM import.
	el.rebuildBtn.classList.toggle("hidden", !localServerAvailable);
	el.rebuildBtn.disabled = Boolean(buildBusyText) || (runtimeMode === "native" && deviceState.lifecycle === "building");
	// Runtime & build header pills
	if (runtimeMode === "wasm") {
		el.runtimePill.className = "pill pill-info";
		el.runtimePill.textContent = "WASM · Browser";
		el.connPill.className = deviceState.lifecycle === "running" ? "pill pill-ok" : "pill pill-muted";
		el.connPill.textContent = deviceState.build?.shortCommit ? `Build ${deviceState.build.shortCommit}` : "In-Browser";
		el.buildRuntimeBadge.textContent = "WASM";
		el.brandSub.textContent = deviceState.build?.name
			? `${deviceState.build.name} · LVGL 9.6 · 240×320`
			: "WebAssembly sonix-player · LVGL 9.6 · 240×320";
	} else if (runtimeMode === "native") {
		el.runtimePill.className = "pill pill-muted";
		el.runtimePill.textContent = "Native Bridge";
		el.buildRuntimeBadge.textContent = "NATIVE";
		el.brandSub.textContent = "Native sonix-player · LVGL 9.6 · 240×320";
	}

	// Lifecycle badge
	const lc = deviceState.lifecycle;
	el.lifecyclePill.className = "pill";
	if (lc === "running") {
		el.lifecyclePill.classList.add("pill-ok");
		el.lifecyclePill.textContent =
			deviceState.mode === "charge_screen" || deviceState.chargeScreen ? "Charge Screen" : "Running";
	} else if (lc === "building") {
		el.lifecyclePill.classList.add("pill-warn");
		el.lifecyclePill.textContent = "Building...";
	} else if (lc === "powered_off" || lc === "stopped") {
		el.lifecyclePill.classList.add("pill-muted");
		el.lifecyclePill.textContent = "Powered Off";
	} else if (lc === "crashed") {
		el.lifecyclePill.classList.add("pill-err");
		el.lifecyclePill.textContent = "Stopped";
	} else {
		el.lifecyclePill.classList.add("pill-info");
		el.lifecyclePill.textContent = "Booting...";
	}

	// Power Off / Boot toggle button
	el.powerToggleBtn.textContent = lc === "powered_off" || lc === "stopped" ? "Boot V1" : "Power Off";

	// FPS & audio pills
	el.fpsPill.textContent = `${deviceState.fps || 0} FPS`;
	if (deviceState.audioActive && deviceState.audioRate) {
		const khz = (deviceState.audioRate / 1000).toFixed(
			deviceState.audioRate % 1000 === 0 ? 0 : 1
		);
		el.audioPill.textContent = `DAC ${khz} kHz`;
		el.audioPill.className = "pill pill-mono pill-ok";
	} else {
		el.audioPill.textContent = "DAC Idle";
		el.audioPill.className = "pill pill-mono";
	}

	// Screen overlay & backlight dimming
	const blPct = Math.max(0, Math.min(100, deviceState.backlight?.percent ?? 78));
	const dimOpacity = deviceState.screenOn ? Math.max(0, (100 - blPct) * 0.0072) : 0.96;
	el.backlightDim.style.opacity = String(dimOpacity);
	el.teleBacklight.textContent = `${blPct}%`;
	el.teleVolume.textContent = `${deviceState.volume ?? 50}%`;
	el.rotBadge.classList.toggle("hidden", !deviceState.rotated);

	if (buildBusyText) {
		el.screenOverlay.classList.remove("hidden");
		el.overlayTitle.textContent = "Loading Firmware...";
		el.overlaySub.textContent = buildBusyText;
		el.overlayBootBtn.classList.add("hidden");
		el.teleScreen.textContent = "Loading";
	} else if (lc === "building") {
		el.screenOverlay.classList.remove("hidden");
		el.overlayTitle.textContent = "Rebuilding Firmware...";
		el.overlaySub.textContent = "Compiling sonix_player_web via make web-emu";
		el.overlayBootBtn.classList.add("hidden");
		el.teleScreen.textContent = "Building";
	} else if (lc === "powered_off" || lc === "stopped") {
		el.screenOverlay.classList.remove("hidden");
		const issue = runtimeMode === "wasm" && wasmSupportIssue();
		const noBundle = runtimeMode === "wasm" && !wasmBackend?.activeBundle;
		if (issue) {
			el.overlayTitle.textContent = "Web build needs isolation";
			el.overlaySub.textContent = issue;
			el.overlayBootBtn.textContent = window.top !== window.self ? "Open in new tab" : "Reload Page";
			el.overlayBootBtn.classList.remove("hidden");
		} else if (noBundle) {
			el.overlayTitle.textContent = buildError ? "Firmware not loaded" : "Select a Sonix Build";
			el.overlaySub.textContent = buildError || "Choose a published build or load a web firmware ZIP in Builds.";
			el.overlayBootBtn.textContent = "Open Builds";
			el.overlayBootBtn.classList.remove("hidden");
		} else {
			el.overlayTitle.textContent = "TempoTec V1 Off";
			el.overlaySub.textContent = "Hold side PWR button (1s) or tap below to boot";
			el.overlayBootBtn.textContent = "Power On V1";
			el.overlayBootBtn.classList.remove("hidden");
		}
		el.teleScreen.textContent = "Powered Off";
	} else if (lc === "crashed") {
		el.screenOverlay.classList.remove("hidden");
		el.overlayTitle.textContent = "Process Stopped";
		el.overlaySub.textContent = deviceState.error || "Inspect live logs or reboot the emulator";
		el.overlayBootBtn.textContent = "Retry boot";
		el.overlayBootBtn.classList.remove("hidden");
		el.teleScreen.textContent = "Stopped";
	} else if (!deviceState.screenOn) {
		el.screenOverlay.classList.remove("hidden");
		el.overlayTitle.textContent = "Display Asleep";
		el.overlaySub.textContent = "Press Power (P) or click below to wake";
		el.overlayBootBtn.textContent = "Wake Screen";
		el.overlayBootBtn.classList.remove("hidden");
		el.teleScreen.textContent = "Asleep";
	} else {
		el.screenOverlay.classList.add("hidden");
		el.overlayBootBtn.textContent = "Power On V1";
		el.teleScreen.textContent = deviceState.rotated ? "Active (180°)" : "Active";
	}

	// Battery & USB-C visual
	const bat = deviceState.battery || { percent: 82, status: "Discharging" };
	el.batSlider.value = String(bat.percent);
	el.batReadout.textContent = `${bat.percent}%`;
	el.teleBattery.textContent = `${bat.percent}% · ${bat.status}`;
	const plugged = bat.status !== "Discharging";
	el.usbcPort.classList.toggle("plugged", plugged);
	document.querySelectorAll("[data-bat-status]").forEach((btn) => {
		btn.classList.toggle("active", btn.dataset.batStatus === bat.status);
	});

	// AirPods UI
	const ap = deviceState.airpods;
	if (ap) {
		el.airpodsEnable.checked = Boolean(ap.enabled);
		el.airpodsControls.classList.toggle("enabled", Boolean(ap.enabled));
		if (ap.model) el.airpodsModel.value = String(ap.model);
		el.apL.value = String(ap.left ?? 85);
		el.apR.value = String(ap.right ?? 80);
		el.apC.value = String(ap.caseBat ?? 65);
		el.apLVal.textContent = `${ap.left ?? 85}%`;
		el.apRVal.textContent = `${ap.right ?? 80}%`;
		el.apCVal.textContent = `${ap.caseBat ?? 65}%`;
	}

	updateLedMetadata();
}

/* --------------------------------------------------------------------------
 * Transport dispatch (WebAssembly in-browser OR local WebSocket server)
 * -------------------------------------------------------------------------- */
function sendWsJson(obj) {
	if (runtimeMode === "wasm") {
		if (!wasmBackend) return;
		if (obj.type === "pointer") wasmBackend.sendPointer(obj.x, obj.y, obj.down);
		else if (obj.type === "key") wasmBackend.sendKey(obj.button, obj.down);
		return;
	}
	if (ws && ws.readyState === WebSocket.OPEN) {
		ws.send(JSON.stringify(obj));
	}
}

function disconnectWs() {
	wsClosedManually = true;
	if (ws) {
		try { ws.close(); } catch {}
		ws = null;
	}
}

function connectWs() {
	if (runtimeMode !== "native" || ws) return;
	wsClosedManually = false;
	const proto = location.protocol === "https:" ? "wss:" : "ws:";
	const url = `${proto}//${location.host}/ws`;
	el.connPill.className = "pill pill-warn";
	el.connPill.textContent = "Connecting";

	const sock = new WebSocket(url);
	sock.binaryType = "arraybuffer";
	ws = sock;

	sock.onopen = () => {
		if (runtimeMode !== "native") return;
		reconnectDelay = 500;
		el.connPill.className = "pill pill-ok";
		el.connPill.textContent = "Connected";
		sendWsJson({ type: "audio_pref", enabled: audioEnabled });
	};

	sock.onmessage = (ev) => {
		if (runtimeMode !== "native") return;
		if (ev.data instanceof ArrayBuffer) {
			const view = new DataView(ev.data);
			if (view.byteLength < 1) return;
			const kind = view.getUint8(0);
			if (kind === 1) {
				handleFramePacket(view);
			} else if (kind === 3) {
				handleAudioPacket(view);
			}
			return;
		}
		try {
			const msg = JSON.parse(ev.data);
			handleServerJson(msg);
		} catch {
			/* ignore */
		}
	};

	sock.onclose = () => {
		if (ws !== sock) return;
		ws = null;
		if (wsClosedManually || runtimeMode !== "native") return;
		el.connPill.className = "pill pill-err";
		el.connPill.textContent = "Reconnecting";
		setTimeout(connectWs, reconnectDelay);
		reconnectDelay = Math.min(3000, Math.round(reconnectDelay * 1.4));
	};

	sock.onerror = () => {
		try {
			sock.close();
		} catch {
			/* ignore */
		}
	};
}

function handleServerJson(msg) {
	switch (msg.type) {
		case "init":
			deviceState = { ...deviceState, ...msg.state };
			allLogs.length = 0;
			if (Array.isArray(msg.logs)) {
				for (const entry of msg.logs) allLogs.push(entry);
			}
			rebuildLogDom();
			syncUiFromState();
			refreshSdList();
			break;
		case "state":
			deviceState = { ...deviceState, ...msg.state };
			syncUiFromState();
			break;
		case "led":
			deviceState.led = msg.led || {
				mode: msg.mode || "off",
				r: msg.r || 0,
				g: msg.g || 0,
				b: msg.b || 0,
				r_step: msg.r_step || 0,
				g_step: msg.g_step || 0,
				b_step: msg.b_step || 0,
				rise_ms: msg.rise_ms || 0,
				fall_ms: msg.fall_ms || 0,
				period_ms: msg.period_ms || 0,
			};
			ledAnimStart = performance.now();
			updateLedMetadata();
			break;
		case "backlight":
			deviceState.backlight = msg.backlight || {
				value: msg.value,
				max: msg.max,
				percent: msg.percent,
			};
			syncUiFromState();
			break;
		case "screen":
			deviceState.screenOn = Boolean(msg.on);
			syncUiFromState();
			break;
		case "rotated":
			deviceState.rotated = Boolean(msg.rotated);
			syncUiFromState();
			break;
		case "volume":
			deviceState.volume = msg.percent;
			syncUiFromState();
			break;
		case "battery":
			deviceState.battery = msg.battery;
			syncUiFromState();
			break;
		case "airpods":
			deviceState.airpods = msg.airpods;
			syncUiFromState();
			break;
		case "audio_info":
			deviceState.audioRate = msg.rate;
			deviceState.audioActive = Boolean(msg.active);
			syncUiFromState();
			break;
		case "metrics":
			deviceState.fps = msg.fps;
			deviceState.audioActive = Boolean(msg.audioActive);
			syncUiFromState();
			break;
		case "log":
			appendLogEntry(msg.entry);
			break;
		case "logs_cleared":
			allLogs.length = 0;
			rebuildLogDom();
			break;
		case "sd_changed":
			refreshSdList();
			break;
		case "power":
			if (msg.action === "poweroff") showToast("TempoTec V1 powered off");
			if (msg.action === "reboot") showToast("TempoTec V1 rebooting...");
			break;
		default:
			break;
	}
}

/* --------------------------------------------------------------------------
 * Touch / Pointer / Scroll forwarding on 240x320 screen
 * -------------------------------------------------------------------------- */
let activePointerId = null;

function canvasCoordsFromEvent(ev) {
	const rect = el.canvas.getBoundingClientRect();
	const x = Math.round(((ev.clientX - rect.left) / rect.width) * el.canvas.width);
	const y = Math.round(((ev.clientY - rect.top) / rect.height) * el.canvas.height);
	return {
		x: Math.max(0, Math.min(el.canvas.width - 1, x)),
		y: Math.max(0, Math.min(el.canvas.height - 1, y)),
		rx: ev.clientX - rect.left,
		ry: ev.clientY - rect.top,
	};
}

function showRipple(rx, ry, active) {
	el.touchRipple.style.left = `${rx}px`;
	el.touchRipple.style.top = `${ry}px`;
	el.touchRipple.classList.toggle("active", active);
}

el.canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
el.screenStage.addEventListener("contextmenu", (ev) => ev.preventDefault());

el.canvas.addEventListener("pointerdown", (ev) => {
	if (ev.button !== 0 && ev.pointerType === "mouse") return;
	ev.preventDefault();
	resumeAudioIfEnabled();
	activePointerId = ev.pointerId;
	try { el.canvas.setPointerCapture(ev.pointerId); } catch {}
	const pt = canvasCoordsFromEvent(ev);
	showRipple(pt.rx, pt.ry, true);
	sendWsJson({ type: "pointer", x: pt.x, y: pt.y, down: true });
});

el.canvas.addEventListener("pointermove", (ev) => {
	if (activePointerId !== ev.pointerId) return;
	ev.preventDefault();
	const pt = canvasCoordsFromEvent(ev);
	showRipple(pt.rx, pt.ry, true);
	sendWsJson({ type: "pointer", x: pt.x, y: pt.y, down: true });
});

function releasePointer(ev) {
	if (ev && activePointerId !== ev.pointerId) return;
	if (activePointerId === null) return;
	activePointerId = null;
	const pt = ev ? canvasCoordsFromEvent(ev) : { x: 0, y: 0, rx: 0, ry: 0 };
	showRipple(pt.rx, pt.ry, false);
	sendWsJson({ type: "pointer", x: pt.x, y: pt.y, down: false });
}

el.canvas.addEventListener("pointerup", releasePointer);
el.canvas.addEventListener("pointercancel", releasePointer);
el.canvas.addEventListener("lostpointercapture", releasePointer);

// Mouse wheel scrolling -> smooth synthesized vertical swipe on the touch panel
let wheelBusy = false;
el.canvas.addEventListener(
	"wheel",
	(ev) => {
		ev.preventDefault();
		if (wheelBusy || activePointerId !== null) return;
		wheelBusy = true;
		const pt = canvasCoordsFromEvent(ev);
		const dir = ev.deltaY > 0 ? -1 : 1; // wheel down -> swipe finger upward
		const startY = Math.max(40, Math.min(el.canvas.height - 40, pt.y));
		const steps = 4;
		const totalDelta = dir * 68;

		sendWsJson({ type: "pointer", x: pt.x, y: startY, down: true });
		for (let i = 1; i <= steps; i++) {
			setTimeout(() => {
				const ny = Math.max(
					4,
					Math.min(el.canvas.height - 5, Math.round(startY + (totalDelta * i) / steps))
				);
				sendWsJson({ type: "pointer", x: pt.x, y: ny, down: i < steps });
				if (i === steps) {
					wheelBusy = false;
				}
			}, i * 24);
		}
	},
	{ passive: false }
);

/* --------------------------------------------------------------------------
 * Physical Side Buttons (mouse/multi-touch hold + haptics + keyboard)
 * -------------------------------------------------------------------------- */
const pressedButtons = new Set();

function triggerHaptic(ms = 10) {
	try {
		if (typeof navigator !== "undefined" && typeof navigator.vibrate === "function") {
			navigator.vibrate(ms);
		}
	} catch {}
}

function setHwButton(btnName, down) {
	const domBtn = document.querySelector(`[data-hw-btn="${btnName}"]`);
	if (down) {
		if (pressedButtons.has(btnName)) return;
		pressedButtons.add(btnName);
		domBtn?.classList.add("pressed");
		sendWsJson({ type: "key", button: btnName, down: true });
	} else {
		if (!pressedButtons.has(btnName)) return;
		pressedButtons.delete(btnName);
		domBtn?.classList.remove("pressed");
		sendWsJson({ type: "key", button: btnName, down: false });
	}
}

function releaseAllInputs() {
	releasePointer(null);
	for (const btn of Array.from(pressedButtons)) {
		setHwButton(btn, false);
	}
}

document.querySelectorAll("[data-hw-btn]").forEach((btn) => {
	const name = btn.dataset.hwBtn;
	let heldPointerId = null;

	btn.addEventListener("contextmenu", (ev) => ev.preventDefault());

	btn.addEventListener("pointerdown", (ev) => {
		if (ev.button !== 0 && ev.pointerType === "mouse") return;
		ev.preventDefault();
		resumeAudioIfEnabled();
		heldPointerId = ev.pointerId;
		try { btn.setPointerCapture(ev.pointerId); } catch {}
		if (ev.pointerType === "touch") triggerHaptic(10);
		setHwButton(name, true);
	});
	const up = (ev) => {
		if (heldPointerId !== null && ev.pointerId !== heldPointerId) return;
		ev.preventDefault();
		heldPointerId = null;
		setHwButton(name, false);
	};
	btn.addEventListener("pointerup", up);
	btn.addEventListener("pointercancel", up);
	btn.addEventListener("lostpointercapture", () => {
		if (heldPointerId !== null) {
			heldPointerId = null;
			setHwButton(name, false);
		}
	});
});

// Keyboard shortcuts
const KEY_MAP = {
	KeyP: "power",
	ArrowUp: "vol_up",
	Equal: "vol_up",
	NumpadAdd: "vol_up",
	ArrowDown: "vol_down",
	Minus: "vol_down",
	NumpadSubtract: "vol_down",
	ArrowLeft: "prev",
	BracketLeft: "prev",
	Space: "play",
	Enter: "play",
	ArrowRight: "next",
	BracketRight: "next",
};

function isTypingInField() {
	const tag = document.activeElement?.tagName;
	return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

window.addEventListener("keydown", (ev) => {
	// Keep keyboard activation of workbench controls (including Browse ZIP)
	// separate from the device's Space/Enter play button.
	if ((ev.code === "Enter" || ev.code === "Space") && ev.target.closest?.("button:not([data-hw-btn]), a")) return;
	if (isTypingInField() || ev.ctrlKey || ev.metaKey || ev.altKey) return;
	if (ev.code === "KeyS") {
		ev.preventDefault();
		captureBrowserScreenshot();
		return;
	}
	const btn = KEY_MAP[ev.code];
	if (btn) {
		ev.preventDefault();
		if (!ev.repeat) setHwButton(btn, true);
	}
});

window.addEventListener("keyup", (ev) => {
	const btn = KEY_MAP[ev.code];
	if (btn) {
		setHwButton(btn, false);
	}
});

window.addEventListener("blur", releaseAllInputs);
window.addEventListener("pagehide", () => {
	releaseAllInputs();
	if (runtimeMode === "wasm") wasmBackend?._persistMemfsState?.();
});
document.addEventListener("visibilitychange", () => {
	if (document.hidden) {
		releaseAllInputs();
		if (runtimeMode === "wasm") wasmBackend?._persistMemfsState?.();
	}
});

/* --------------------------------------------------------------------------
 * Screenshots (Instant PNG download + Firmware Device Shot)
 * -------------------------------------------------------------------------- */
function triggerShutterFlash() {
	el.shutterFlash.classList.add("flash");
	requestAnimationFrame(() => {
		requestAnimationFrame(() => {
			el.shutterFlash.classList.remove("flash");
		});
	});
}

function captureBrowserScreenshot() {
	triggerShutterFlash();
	const ts = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
	const filename = `tempotec-v1-${ts}.png`;
	const dataUrl = el.canvas.toDataURL("image/png");
	const a = document.createElement("a");
	a.href = dataUrl;
	a.download = filename;
	document.body.appendChild(a);
	a.click();
	a.remove();
	showToast(`Saved ${filename}`);
}

el.screenshotBtn.addEventListener("click", captureBrowserScreenshot);

el.deviceShotBtn.addEventListener("click", async () => {
	triggerShutterFlash();
	if (runtimeMode === "wasm") {
		wasmBackend?.requestDeviceScreenshot();
		showToast("Requested firmware screenshot -> /Screenshots/");
		return;
	}
	await fetch("/api/screenshot/device", { method: "POST" });
	showToast("Requested firmware screenshot -> /Screenshots/");
});

/* --------------------------------------------------------------------------
 * Responsive Scale Switcher (Fit / 1x / 1.25x / 1.5x / 2x), Mobile Views,
 * Audio Toggle, Flip View & Hardware Port Shortcuts
 * -------------------------------------------------------------------------- */
let activeScaleMode = window.innerWidth <= 980 ? "fit" : "1.25";

function isMobileViewport() {
	return window.innerWidth <= 768;
}

function computeFitScale() {
	const wrap = el.stageCanvasWrap;
	const mobile = isMobileViewport();
	// Available space inside the stage wrapper (fallback to viewport if hidden in another mobile tab)
	const rect = wrap ? wrap.getBoundingClientRect() : null;
	const hasMeasured = rect && rect.width > 120 && rect.height > 120;
	const availW = hasMeasured ? rect.width - (mobile ? 8 : 28) : window.innerWidth - (mobile ? 16 : 440);
	const availH = hasMeasured ? rect.height - (mobile ? 28 : 44) : window.innerHeight - (mobile ? 210 : 160);

	// Account for left + right flanks (compact icon badges on mobile vs full key badges on desktop)
	const flankW = mobile ? 96 : 196;
	const portH = 44; // top gold jacks + bottom USB-C + vertical breathing room
	// Chassis base dimensions at scale=1: width = 266*s + 10, height = 515*s + 10
	const scaleByW = (availW - flankW - 10) / 266;
	const scaleByH = (availH - portH - 10) / 515;

	let target = Math.min(scaleByW, scaleByH);
	if (!Number.isFinite(target) || target <= 0) target = 1;
	// Clamp cleanly so tiny 320px phones stay usable and large desktops stay balanced
	target = Math.max(0.58, Math.min(mobile ? 1.35 : 1.75, target));
	return Math.round(target * 1000) / 1000;
}

function applyScreenScale(mode) {
	activeScaleMode = String(mode);
	const numericScale = activeScaleMode === "fit" ? computeFitScale() : Number(activeScaleMode) || 1.25;
	document.documentElement.style.setProperty("--screen-scale", String(numericScale));
	document.documentElement.dataset.scale = activeScaleMode;
	const isIntegerScale = Math.abs(numericScale - Math.round(numericScale)) < 0.01;
	document.documentElement.dataset.scaleSmooth = isIntegerScale ? "0" : "1";
	document.querySelectorAll("[data-scale]").forEach((b) => {
		b.classList.toggle("active", b.dataset.scale === activeScaleMode);
	});
}

function setMobileView(view) {
	document.body.dataset.mobileView = view;
	document.querySelectorAll("[data-mobile-nav]").forEach((b) => {
		b.classList.toggle("active", b.dataset.mobileNav === view);
	});
	if (view === "builds" || view === "hardware" || view === "sd") {
		activateTab(view, false);
	}
	if (view === "device" && activeScaleMode === "fit") {
		requestAnimationFrame(() => applyScreenScale("fit"));
	}
}

document.querySelectorAll("[data-mobile-nav]").forEach((btn) => {
	btn.addEventListener("click", () => {
		setMobileView(btn.dataset.mobileNav);
	});
});

applyScreenScale(activeScaleMode);

if (typeof ResizeObserver !== "undefined" && el.stageCanvasWrap) {
	const ro = new ResizeObserver(() => {
		if (activeScaleMode === "fit") {
			applyScreenScale("fit");
		}
	});
	ro.observe(el.stageCanvasWrap);
}

window.addEventListener("resize", () => {
	if (activeScaleMode === "fit") {
		applyScreenScale("fit");
	}
});

window.addEventListener("orientationchange", () => {
	setTimeout(() => {
		if (activeScaleMode === "fit") {
			applyScreenScale("fit");
		}
	}, 120);
});

document.querySelectorAll("[data-scale]").forEach((btn) => {
	btn.addEventListener("click", () => {
		applyScreenScale(btn.dataset.scale);
	});
});

el.audioToggleBtn.addEventListener("click", () => {
	setAudioEnabled(!audioEnabled);
	showToast(audioEnabled ? "Live DAC audio playback enabled" : "Browser audio muted");
});

el.topJacks?.addEventListener("click", () => {
	setAudioEnabled(!audioEnabled);
	showToast(audioEnabled ? "Headphones connected · Live DAC audio enabled" : "Headphones unplugged · Browser audio muted");
});

function setDeviceFlipped(flipped) {
	el.v1Assembly?.classList.toggle("flipped", flipped);
	if (el.flipDeviceBtn) {
		el.flipDeviceBtn.textContent = flipped ? "Front View" : "Back View";
		el.flipDeviceBtn.classList.toggle("active", flipped);
	}
}

el.flipDeviceBtn?.addEventListener("click", () => {
	if (isMobileViewport() && document.body.dataset.mobileView !== "device") {
		setMobileView("device");
	}
	const next = !el.v1Assembly?.classList.contains("flipped");
	setDeviceFlipped(next);
});

el.v1ChassisBack?.addEventListener("click", () => {
	setDeviceFlipped(false);
});

el.sdSlotBtn?.addEventListener("click", () => {
	activateTab("sd");
});

el.usbcPort?.addEventListener("click", () => {
	activateTab("hardware");
});

el.rebuildBtn.addEventListener("click", async () => {
	if (!localServerAvailable || buildBusyText) return;
	const intent = loadRequest;
	followedBuild = null;
	try {
		const res = await fetch("/api/power", {
			method: "POST", headers: { "Content-Type": "application/json" },
			body: JSON.stringify({ action: "rebuild" }),
		});
		if (!res.ok) throw new Error(`Local rebuild request failed (HTTP ${res.status})`);
		if (loadRequest !== intent) return;
		if (runtimeMode !== "native") {
			runtimeMode = "native";
			await wasmBackend?.stop();
			deviceState.build = deviceState.error = null;
			deviceState.chargeScreen = false;
			connectWs();
		}
		setBuildResult("Testing the local working tree. See Live Logs for build output.");
	} catch (err) { reportBuildError(err); }
});

/* --------------------------------------------------------------------------
 * Battery & Charger controls
 * -------------------------------------------------------------------------- */
let batDebounce = null;

function pushBatteryUpdate(percent, status, immediate = false) {
	if (percent !== undefined) {
		deviceState.battery.percent = Number(percent);
		el.batSlider.value = String(percent);
		el.batReadout.textContent = `${percent}%`;
	}
	if (status !== undefined) {
		deviceState.battery.status = status;
	}
	syncUiFromState();

	const send = () => {
		if (runtimeMode === "wasm") {
			wasmBackend?.setBattery(deviceState.battery.percent, deviceState.battery.status);
			return;
		}
		fetch("/api/battery", {
			method: "POST",
			headers: { "Content-Type": "application/json" },
			body: JSON.stringify(deviceState.battery),
		}).catch(() => {});
	};
	clearTimeout(batDebounce);
	if (immediate) {
		send();
	} else {
		batDebounce = setTimeout(send, 80);
	}
}

el.batSlider.addEventListener("input", () => {
	pushBatteryUpdate(Number(el.batSlider.value), undefined, false);
});

el.batSlider.addEventListener("change", () => {
	pushBatteryUpdate(Number(el.batSlider.value), undefined, true);
});

document.querySelectorAll("[data-bat-pct]").forEach((chip) => {
	chip.addEventListener("click", () => {
		pushBatteryUpdate(Number(chip.dataset.batPct), undefined, true);
	});
});

document.querySelectorAll("[data-bat-status]").forEach((btn) => {
	btn.addEventListener("click", () => {
		pushBatteryUpdate(undefined, btn.dataset.batStatus, true);
	});
});

/* --------------------------------------------------------------------------
 * Device Actions & Hardware Combos
 * -------------------------------------------------------------------------- */
el.holdPowerBtn.addEventListener("click", () => {
	showToast("Holding Power button for 2.2s...");
	setHwButton("power", true);
	setTimeout(() => setHwButton("power", false), 2200);
});

el.comboShotBtn.addEventListener("click", () => {
	showToast("Pressing Vol+ & Power combo...");
	setHwButton("vol_up", true);
	setHwButton("power", true);
	setTimeout(() => {
		setHwButton("power", false);
		setHwButton("vol_up", false);
		setTimeout(refreshSdList, 700);
	}, 260);
});

async function postPowerAction(action) {
	if (runtimeMode === "wasm") {
		if (buildBusyText) return;
		try { await wasmBackend?.powerAction(action); } catch (err) { reportBuildError(err); }
		return;
	}
	await fetch("/api/power", {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ action }),
	});
}

el.chargeScreenBtn.addEventListener("click", () => {
	showToast("Booting offline Charge Screen...");
	postPowerAction("charge_screen");
});

el.rebootBtn.addEventListener("click", () => {
	showToast("Rebooting TempoTec V1...");
	postPowerAction("reboot");
});

el.powerToggleBtn.addEventListener("click", () => {
	if (deviceState.lifecycle === "powered_off" || deviceState.lifecycle === "stopped") {
		showToast("Booting TempoTec V1...");
		postPowerAction("boot");
	} else {
		postPowerAction("poweroff");
	}
});

el.overlayBootBtn.addEventListener("click", () => {
	if (runtimeMode === "wasm" && (!window.crossOriginIsolated || typeof SharedArrayBuffer === "undefined")) {
		if (window.top !== window.self) { el.buildStandalone.click(); return; }
		try { sessionStorage.removeItem("sonix_coi_reloaded_v2"); } catch {}
		window.location.reload();
		return;
	}
	if (runtimeMode === "wasm" && !wasmBackend?.activeBundle) {
		setMobileView("builds");
		return;
	}
	if (deviceState.lifecycle === "running" && !deviceState.screenOn) {
		setHwButton("power", true);
		setTimeout(() => setHwButton("power", false), 90);
		return;
	}
	showToast("Booting TempoTec V1...");
	postPowerAction("boot");
});

el.resetConfigBtn.addEventListener("click", () => {
	showToast("Wiping device_config.ini for First-Boot Setup...");
	postPowerAction("reset_config");
});

/* --------------------------------------------------------------------------
 * AirPods Telemetry Simulator
 * -------------------------------------------------------------------------- */
function applyAirpods() {
	const payload = {
		enabled: el.airpodsEnable.checked,
		model: el.airpodsModel.value,
		left: Number(el.apL.value),
		right: Number(el.apR.value),
		caseBat: Number(el.apC.value),
		noise: 2,
		reboot: true,
	};
	el.airpodsControls.classList.toggle("enabled", payload.enabled);
	if (runtimeMode === "wasm") {
		wasmBackend
			?.setAirpods(payload)
			.then(() => showToast(payload.enabled ? "Applied AirPods telemetry (rebooting)" : "Disabled AirPods telemetry"))
			.catch(() => {});
		return;
	}
	fetch("/api/airpods", {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify(payload),
	})
		.then(() => showToast(payload.enabled ? "Applied AirPods telemetry (rebooting)" : "Disabled AirPods telemetry"))
		.catch(() => {});
}

el.airpodsEnable.addEventListener("change", applyAirpods);
el.airpodsModel.addEventListener("change", () => {
	if (el.airpodsEnable.checked) applyAirpods();
});
for (const [slider, label] of [
	[el.apL, el.apLVal],
	[el.apR, el.apRVal],
	[el.apC, el.apCVal],
]) {
	slider.addEventListener("input", () => {
		label.textContent = `${slider.value}%`;
	});
	slider.addEventListener("change", () => {
		if (el.airpodsEnable.checked) applyAirpods();
	});
}

/* --------------------------------------------------------------------------
 * Inspector Tabs
 * -------------------------------------------------------------------------- */
function activateTab(target, syncMobile = true) {
	document.querySelectorAll(".tab-btn").forEach((b) => {
		const isTarget = b.dataset.tab === target;
		b.classList.toggle("active", isTarget);
		b.setAttribute("aria-selected", String(isTarget));
	});
	document.querySelectorAll(".tab-content").forEach((pane) => {
		pane.classList.toggle("active", pane.id === `tab-${target}`);
	});
	if (syncMobile && isMobileViewport() && (target === "builds" || target === "hardware" || target === "sd")) {
		document.body.dataset.mobileView = target;
		document.querySelectorAll("[data-mobile-nav]").forEach((b) => {
			b.classList.toggle("active", b.dataset.mobileNav === target);
		});
	}
	if (target === "sd") {
		refreshSdList();
	}
}

document.querySelectorAll(".tab-btn").forEach((btn) => {
	btn.addEventListener("click", () => {
		activateTab(btn.dataset.tab);
	});
});

/* --------------------------------------------------------------------------
 * Virtual microSD Browser, Drag-and-Drop Upload & Screenshot Preview
 * -------------------------------------------------------------------------- */
function formatSize(bytes) {
	if (bytes < 1024) return `${bytes} B`;
	if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
	return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

async function refreshSdList(dir = currentSdDir) {
	currentSdDir = dir;
	el.sdCrumbs.textContent = `/${dir}`;
	try {
		if (runtimeMode === "wasm") {
			if (!wasmBackend) {
				renderSdEntries([]);
				return;
			}
			const data = wasmBackend.listSd(dir);
			renderSdEntries(data.items || []);
			return;
		}
		const res = await fetch(`/api/sd/list?dir=${encodeURIComponent(dir)}`);
		const data = await res.json();
		renderSdEntries(data.items || []);
	} catch {
		el.sdList.innerHTML = `<div class="sd-row">Failed to list microSD directory.</div>`;
	}
}

function revokeSdBlobUrls() {
	for (const url of activeSdBlobUrls) {
		try { URL.revokeObjectURL(url); } catch {}
	}
	activeSdBlobUrls = [];
}

function renderSdEntries(items) {
	revokeSdBlobUrls();
	el.sdList.innerHTML = "";

	if (currentSdDir) {
		const parent = currentSdDir.includes("/")
			? currentSdDir.slice(0, currentSdDir.lastIndexOf("/"))
			: "";
		const upRow = document.createElement("div");
		upRow.className = "sd-row";
		upRow.innerHTML = `<div class="sd-name"><span class="sd-badge">UP</span><span>..</span></div>`;
		upRow.querySelector(".sd-name").addEventListener("click", () => refreshSdList(parent));
		el.sdList.appendChild(upRow);
	}

	if (items.length === 0) {
		const empty = document.createElement("div");
		empty.className = "sd-row";
		empty.style.color = "var(--text-muted)";
		empty.textContent = "Folder is empty. Upload files or drop them above.";
		el.sdList.appendChild(empty);
		return;
	}

	for (const item of items) {
		const row = document.createElement("div");
		row.className = "sd-row";

		const nameWrap = document.createElement("div");
		nameWrap.className = "sd-name";

		const ext = item.isDir
			? "DIR"
			: (item.name.split(".").pop() || "FILE").toUpperCase().slice(0, 5);
		const badge = document.createElement("span");
		badge.className = "sd-badge";
		badge.textContent = ext;

		const label = document.createElement("span");
		label.textContent = item.name;
		label.title = item.path;

		nameWrap.append(badge, label);

		const actions = document.createElement("div");
		actions.className = "sd-row-actions";

		if (item.isDir) {
			nameWrap.addEventListener("click", () => refreshSdList(item.path));
		} else {
			const sizeEl = document.createElement("span");
			sizeEl.className = "sd-size";
			sizeEl.textContent = formatSize(item.size);
			actions.appendChild(sizeEl);

			const isImage = /\.(png|jpe?g|webp|bmp)$/i.test(item.name);

			const openWasmOrNativeFile = async (downloadOnly) => {
				if (runtimeMode === "wasm") {
					const bytes = await wasmBackend.readSdFile(item.path);
					if (!bytes) {
						showToast(`Failed to read ${item.name}`);
						return;
					}
					const mime = item.name.toLowerCase().endsWith(".png")
						? "image/png"
						: "application/octet-stream";
					const blobUrl = URL.createObjectURL(new Blob([bytes], { type: mime }));
					activeSdBlobUrls.push(blobUrl);
					if (downloadOnly) {
						const a = document.createElement("a");
						a.href = blobUrl;
						a.download = item.name;
						document.body.appendChild(a);
						a.click();
						a.remove();
					} else {
						openShotModal(item.name, blobUrl);
					}
					return;
				}
				const fileUrl = `/api/sd/file?path=${encodeURIComponent(item.path)}`;
				if (downloadOnly) {
					const a = document.createElement("a");
					a.href = fileUrl;
					a.download = item.name;
					document.body.appendChild(a);
					a.click();
					a.remove();
				} else {
					openShotModal(item.name, fileUrl);
				}
			};

			if (isImage) {
				const previewBtn = document.createElement("button");
				previewBtn.className = "btn btn-ghost btn-xs";
				previewBtn.textContent = "View";
				previewBtn.addEventListener("click", () => openWasmOrNativeFile(false));
				nameWrap.addEventListener("click", () => openWasmOrNativeFile(false));
				actions.appendChild(previewBtn);
			}

			const dlBtn = document.createElement("button");
			dlBtn.className = "btn btn-ghost btn-xs";
			dlBtn.textContent = "DL";
			dlBtn.addEventListener("click", () => openWasmOrNativeFile(true));
			actions.appendChild(dlBtn);

			const delBtn = document.createElement("button");
			delBtn.className = "btn btn-danger-soft btn-xs";
			delBtn.textContent = "Del";
			delBtn.addEventListener("click", async () => {
				if (runtimeMode === "wasm") {
					await wasmBackend.deleteSdFile(item.path);
				} else {
					await fetch(`/api/sd/file?path=${encodeURIComponent(item.path)}`, {
						method: "DELETE",
					});
				}
				refreshSdList();
			});
			actions.appendChild(delBtn);
		}

		row.append(nameWrap, actions);
		el.sdList.appendChild(row);
	}
}

async function uploadFilesToSd(fileList) {
	const files = Array.from(fileList || []);
	if (!files.length) return;
	const targetDir = currentSdDir || "Music";
	for (const file of files) {
		if (runtimeMode === "wasm") {
			try {
				const buf = await file.arrayBuffer();
				await wasmBackend.uploadSdFile(targetDir, file.name, buf);
				showToast(`Copied ${file.name} -> /${targetDir}/`);
			} catch {
				showToast(`Failed to copy ${file.name}`);
			}
		} else {
			const url = `/api/sd/upload?dir=${encodeURIComponent(targetDir)}&name=${encodeURIComponent(file.name)}`;
			const res = await fetch(url, {
				method: "POST",
				body: file,
			});
			if (res.ok) {
				showToast(`Uploaded ${file.name} -> /${targetDir}/`);
			} else {
				showToast(`Failed to upload ${file.name}`);
			}
		}
	}
	refreshSdList();
}

el.sdUploadInput.addEventListener("change", () => {
	uploadFilesToSd(el.sdUploadInput.files);
	el.sdUploadInput.value = "";
});

el.sdRefreshBtn.addEventListener("click", () => refreshSdList());

document.querySelectorAll("[data-sd-dir]").forEach((chip) => {
	chip.addEventListener("click", () => refreshSdList(chip.dataset.sdDir));
});

["dragenter", "dragover"].forEach((evName) => {
	el.sdDropzone.addEventListener(evName, (ev) => {
		ev.preventDefault();
		el.sdDropzone.classList.add("dragover");
	});
});
["dragleave", "drop"].forEach((evName) => {
	el.sdDropzone.addEventListener(evName, (ev) => {
		ev.preventDefault();
		el.sdDropzone.classList.remove("dragover");
	});
});
el.sdDropzone.addEventListener("drop", (ev) => {
	uploadFilesToSd(ev.dataTransfer?.files);
});

/* Screenshot preview modal */
function openShotModal(title, url) {
	el.shotModalTitle.textContent = title;
	el.shotModalImg.src = url;
	el.shotModalDownload.href = url;
	el.shotModalDownload.download = title;
	el.shotModal.classList.remove("hidden");
}

function closeShotModal() {
	el.shotModal.classList.add("hidden");
}

el.shotModalClose.addEventListener("click", closeShotModal);
el.shotModalBackdrop.addEventListener("click", closeShotModal);

/* --------------------------------------------------------------------------
 * Live Log Console
 * -------------------------------------------------------------------------- */
function formatLogTime(ts) {
	const d = new Date(ts);
	return d.toTimeString().slice(0, 8);
}

function logMatchesFilter(entry) {
	if (activeStreamFilter !== "all" && entry.stream !== activeStreamFilter) {
		return false;
	}
	const q = el.logFilter.value.trim().toLowerCase();
	if (q && !entry.line.toLowerCase().includes(q)) {
		return false;
	}
	return true;
}

function makeLogRow(entry) {
	const row = document.createElement("div");
	row.className = `log-line log-${entry.stream}`;

	const ts = document.createElement("span");
	ts.className = "log-ts";
	ts.textContent = formatLogTime(entry.ts);

	const tag = document.createElement("span");
	tag.className = "log-tag";
	tag.textContent = entry.stream;

	const msg = document.createElement("span");
	msg.className = "log-msg";
	msg.textContent = entry.line;

	row.append(ts, tag, msg);
	return row;
}

function appendLogEntry(entry) {
	if (!entry) return;
	allLogs.push(entry);
	if (allLogs.length > MAX_CLIENT_LOGS) {
		allLogs.shift();
	}
	el.logCount.textContent = String(allLogs.length);
	if (logMatchesFilter(entry)) {
		el.logBody.appendChild(makeLogRow(entry));
		while (el.logBody.childElementCount > MAX_CLIENT_LOGS) {
			el.logBody.firstElementChild?.remove();
		}
		if (el.logAutoscroll.checked) {
			el.logBody.scrollTop = el.logBody.scrollHeight;
		}
	}
}

function rebuildLogDom() {
	el.logBody.innerHTML = "";
	const frag = document.createDocumentFragment();
	for (const entry of allLogs) {
		if (logMatchesFilter(entry)) {
			frag.appendChild(makeLogRow(entry));
		}
	}
	el.logBody.appendChild(frag);
	el.logCount.textContent = String(allLogs.length);
	if (el.logAutoscroll.checked) {
		el.logBody.scrollTop = el.logBody.scrollHeight;
	}
}

el.logFilter.addEventListener("input", rebuildLogDom);

document.querySelectorAll(".stream-chip").forEach((chip) => {
	chip.addEventListener("click", () => {
		activeStreamFilter = chip.dataset.stream;
		document.querySelectorAll(".stream-chip").forEach((c) => c.classList.toggle("active", c === chip));
		rebuildLogDom();
	});
});

el.logCopyBtn.addEventListener("click", async () => {
	const text = allLogs
		.filter(logMatchesFilter)
		.map((e) => `[${formatLogTime(e.ts)}] [${e.stream}] ${e.line}`)
		.join("\n");
	try {
		await navigator.clipboard.writeText(text);
		showToast("Copied logs to clipboard");
	} catch {
		showToast("Clipboard unavailable");
	}
});

el.logClearBtn.addEventListener("click", async () => {
	if (runtimeMode === "wasm") {
		allLogs.length = 0;
		rebuildLogDom();
		return;
	}
	await fetch("/api/logs/clear", { method: "POST" });
});

/* --------------------------------------------------------------------------
 * WebAssembly Runtime & Universal .sonix-web.zip Firmware Build Loader
 * -------------------------------------------------------------------------- */
function ensureWasmBackend() {
	if (wasmBackend) return wasmBackend;
	wasmBackend = new SonixWasmBackend({
		onActivate: switchToWasmMode,
		onFrame: (rgb565Bytes, w, h) => {
			if (runtimeMode !== "wasm") return;
			renderRgb565Bytes(rgb565Bytes, w, h);
		},
		onPcm: (pcmBytes, rate, channels) => {
			if (runtimeMode !== "wasm") return;
			schedulePcmBytes(pcmBytes, rate, channels);
		},
		onState: (state) => {
			if (runtimeMode !== "wasm") return;
			deviceState = { ...deviceState, ...state };
			syncUiFromState();
		},
		onEvent: (ev) => {
			if (runtimeMode !== "wasm") return;
			handleServerJson(ev);
		},
		onLog: (entry) => {
			if (runtimeMode !== "wasm") return;
			appendLogEntry(entry);
		},
		onSdChanged: () => {
			if (runtimeMode !== "wasm") return;
			refreshSdList();
		},
	});
	return wasmBackend;
}

function switchToWasmMode() {
	if (runtimeMode === "wasm") return ensureWasmBackend();
	disconnectWs();
	runtimeMode = "wasm";
	const backend = ensureWasmBackend();
	deviceState = { ...deviceState, ...backend.state };
	syncUiFromState();
	return backend;
}

function setBuildProgress(percent, label) {
	if (label == null) {
		buildBusyText = null;
		el.buildProgress.classList.add("hidden");
		syncUiFromState();
		return;
	}
	buildBusyText = label;
	el.buildProgress.classList.remove("hidden");
	el.buildProgressText.textContent = label;
	el.buildProgressFill.style.width = `${Math.max(4, Math.min(100, percent))}%`;
	syncUiFromState();
}

function setBuildResult(message, error = false) {
	buildError = error ? message : null;
	el.buildResult.textContent = message;
	el.buildResult.classList.toggle("error", error);
}

function reportBuildError(err) {
	const message = err?.message || String(err);
	setBuildResult(message, true);
	showToast(message);
	appendLogEntry({ ts: Date.now(), stream: "stderr", line: `[firmware] ${message}` });
}

function wasmSupportIssue() {
	if (!window.isSecureContext) return "Use HTTPS or localhost for web firmware. Over plain HTTP, use the native bridge.";
	if (!window.crossOriginIsolated || typeof SharedArrayBuffer === "undefined") {
		return window.top !== window.self
			? "Open the emulator in its own tab, then load the build there. This embedded page cannot use SharedArrayBuffer."
			: "Reload once to activate cross-origin isolation. The host must support COOP/COEP for WebAssembly threads.";
	}
	return null;
}

function updateSelectedBuildMeta() {
	const item = catalogBuilds.find((b) => b.id === el.buildSelect.value);
	el.buildLoadBtn.disabled = !item;
	el.buildDelBtn.classList.toggle("hidden", item?.source !== "custom");
	const bits = [];
	if (item?.ref) bits.push(`Ref: ${item.ref}`);
	if (item?.shortCommit) bits.push(`Commit: ${item.shortCommit}`);
	if (item?.size) bits.push(formatSize(item.size));
	if (Date.parse(item?.builtAt)) bits.push(new Date(item.builtAt).toLocaleString());
	if (item?.source === "custom") bits.push("Imported in this browser");
	el.buildMeta.textContent = bits.join(" · ") || "Import a web build below, or check for the latest published firmware.";
}

async function fetchPublished(signal) {
	const res = await fetch("./builds/manifest.json", { cache: "no-store", signal });
	if (!res.ok) throw new Error(`HTTP ${res.status}`);
	return publishedBuilds(await res.json());
}

async function refreshBuildCatalog(signal) {
	const seq = ++catalogRequest;
	let published = [];
	let catalogError;
	try {
		published = await fetchPublished(signal);
	} catch (err) {
		catalogError = err; // A missing catalog must not prevent local imports.
	}
	const custom = await ensureWasmBackend().listSavedCustomBuilds();
	signal?.throwIfAborted();
	const latest = published[0];
	if (seq === catalogRequest) {
		catalogBuilds = [...published, ...custom];
		renderBuildOptions(loadRequest ? el.buildSelect.value : null);
	}
	return { latest, catalogError };
}

function renderBuildOptions(selectId) {
	const latest = catalogBuilds.find((item) => item.source === "catalog");
	el.buildSelect.replaceChildren();
	for (const item of catalogBuilds) {
		const prefix = item === latest ? "[Latest] " : item.source === "custom" ? "[Custom] " : "";
		el.buildSelect.add(new Option(prefix + item.name, item.id));
	}
	if (!catalogBuilds.length) el.buildSelect.add(new Option("No web builds available", ""));
	if (selectId && catalogBuilds.some((b) => b.id === selectId)) el.buildSelect.value = selectId;
	updateSelectedBuildMeta();
}

// A slow auto-boot must never finish over a later file/drop/URL selection.
// Cancel stale downloads; serialize teardown/startup because Emscripten exports
// its factory into the page and owns a pool of live pthread workers.
function requestBuild(label, load) {
	followedBuild = null;
	loadRequest?.abort();
	loadRequest = new AbortController();
	const { signal } = loadRequest;
	const progress = (text, fraction) => {
		if (!signal.aborted) setBuildProgress(Math.round(fraction * 100), text);
	};
	setBuildResult("");
	progress(label, 0.02);
	loadQueue = loadQueue.then(async () => {
		if (signal.aborted) return;
		try {
			const issue = wasmSupportIssue();
			if (issue) throw new Error(issue);
			const meta = await load(ensureWasmBackend(), { signal, progress });
			signal.throwIfAborted();
			const status = followedBuild ? "Following latest" : "Booted";
			setBuildResult(`${status}: ${meta.name} · ${meta.shortCommit} · ${new Date(meta.builtAt).toLocaleString()}`);
			if (isMobileViewport()) setMobileView("device");
		} catch (err) {
			if (!signal.aborted) reportBuildError(err);
		} finally {
			if (!signal.aborted) setBuildProgress(null, null);
		}
	});
	return loadQueue;
}

async function loadEntry(entry, backend, request) {
	if (entry.source === "custom") {
		const buf = await backend.getSavedCustomBuildBuffer(entry.id);
		if (!buf) throw new Error("Saved custom build not found. Browse to the file again.");
		return backend.loadBundleFromBuffer(buf, { ...entry, ...request });
	}
	return backend.loadBundleFromUrl(entry.url, { ...entry, ...request });
}

function loadLocalZipFile(file) {
	if (!file) return;
	return requestBuild(`Reading ${file.name}...`, async (backend, request) => {
		if (/\.upt$/i.test(file.name)) throw new Error(WEB_BUILD_HINT);
		const meta = await backend.loadBundleFromBuffer(await file.arrayBuffer(), {
			...request, name: file.name, saveCustom: true,
		});
		request.signal.throwIfAborted();
		// No network round trip is needed to finish a file import.
		catalogBuilds = [...catalogBuilds.filter((b) => b.id !== meta.id), meta];
		renderBuildOptions(meta.id);
		return meta;
	});
}

function bootUrl(url) {
	return requestBuild(`Fetching ${url}...`, (backend, request) =>
		backend.loadBundleFromUrl(url, { ...request, name: url.split("/").pop() || "URL build" }));
}

el.buildSelect.addEventListener("change", updateSelectedBuildMeta);

el.buildLoadBtn.addEventListener("click", () => {
	const entry = catalogBuilds.find((b) => b.id === el.buildSelect.value);
	if (entry) requestBuild(`Loading ${entry.name}...`, (backend, request) => loadEntry(entry, backend, request));
});

function buildKey(b) {
	return JSON.stringify([b.id, b.url, b.commit, b.builtAt, b.size]);
}

function bootLatest(catalog) {
	return requestBuild("Checking for the latest firmware...", async (backend, request) => {
		const { latest, catalogError } = catalog || await refreshBuildCatalog(request.signal);
		if (!latest) throw new Error(noPublishedMessage(catalogError));
		el.buildSelect.value = latest.id;
		updateSelectedBuildMeta();
		const meta = await loadEntry(latest, backend, request);
		if (!request.signal.aborted) followedBuild = buildKey(latest);
		return meta;
	});
}
el.buildLatestBtn.addEventListener("click", () => bootLatest());

// Poll just the small manifest, not the ZIP or the user's saved builds. Never
// replace a hand-picked firmware, revive a powered-off device, or apply a late
// poll response after the user has started another load.
async function checkLatest() {
	if (followedBuild === null || checkingLatest || document.hidden || (wasmBackend?.activeBundle && deviceState.lifecycle !== "running")) return;
	const previousRequest = loadRequest;
	checkingLatest = true;
	try {
		const [latest] = await fetchPublished();
		if (previousRequest === loadRequest && followedBuild !== null && latest && buildKey(latest) !== followedBuild) {
			await bootLatest();
		}
	} catch { /* Offline: keep the running firmware and try the catalog again later. */ }
	finally { checkingLatest = false; }
}
setInterval(checkLatest, 60000);
window.addEventListener("focus", checkLatest);

el.buildDelBtn.addEventListener("click", async () => {
	const entry = catalogBuilds.find((b) => b.id === el.buildSelect.value);
	if (!entry || entry.source !== "custom") return;
	const backend = ensureWasmBackend();
	await backend.deleteSavedCustomBuild(entry.id);
	showToast(`Deleted ${entry.name}`);
	catalogBuilds = catalogBuilds.filter((b) => b.id !== entry.id);
	renderBuildOptions();
});

el.buildFileInput.addEventListener("change", () => {
	const file = el.buildFileInput.files?.[0];
	el.buildFileInput.value = "";
	if (file) loadLocalZipFile(file);
});

document.getElementById("btn-build-browse").addEventListener("click", () => el.buildFileInput.click());
el.buildDropzone.addEventListener("click", () => el.buildFileInput.click());

["dragenter", "dragover"].forEach((evName) => {
	el.buildDropzone.addEventListener(evName, (ev) => {
		ev.preventDefault();
		el.buildDropzone.classList.add("dragover");
	});
});
["dragleave", "drop"].forEach((evName) => {
	el.buildDropzone.addEventListener(evName, (ev) => {
		ev.preventDefault();
		el.buildDropzone.classList.remove("dragover");
	});
});
el.buildDropzone.addEventListener("drop", (ev) => {
	const file = ev.dataTransfer?.files?.[0];
	if (file) loadLocalZipFile(file);
});

el.buildUrlForm.addEventListener("submit", (ev) => {
	ev.preventDefault();
	const url = el.buildUrlInput.value.trim();
	if (url) bootUrl(url);
	else reportBuildError(new Error("Enter a direct firmware bundle URL first."));
});

// Allow dropping a .sonix-web.zip anywhere on the stage or page
window.addEventListener("dragover", (ev) => {
	if (ev.dataTransfer?.types?.includes("Files")) {
		ev.preventDefault();
	}
});
window.addEventListener("drop", (ev) => {
	if (ev.target === el.sdDropzone || el.sdDropzone.contains(ev.target)) return;
	if (ev.target === el.buildDropzone || el.buildDropzone.contains(ev.target)) return;
	const file = ev.dataTransfer?.files?.[0];
	if (file && /\.(zip|upt)$/i.test(file.name)) {
		ev.preventDefault();
		loadLocalZipFile(file);
	}
});

/* --------------------------------------------------------------------------
 * Auto-detect runtime mode (Local Node Server vs Static GitHub Pages WASM)
 * -------------------------------------------------------------------------- */
async function probeLocalServer() {
	try {
		const ctrl = new AbortController();
		const timer = setTimeout(() => ctrl.abort(), 900);
		const res = await fetch("/api/state", { signal: ctrl.signal, cache: "no-store" });
		clearTimeout(timer);
		if (!res.ok) return false;
		const ct = res.headers.get("content-type") || "";
		if (!ct.includes("application/json")) return false;
		const data = await res.json();
		return Boolean(data && typeof data === "object" && data.state);
	} catch {
		return false;
	}
}

function noPublishedMessage(err) {
	return (err ? `Cannot read the published catalog (${err.message}).` : "No published web firmware yet.")
		+ " Import a web build below, or use Boot latest to check again.";
}

async function bootstrap() {
	const params = new URLSearchParams(location.search);
	const explicitUrl = params.get("build");
	el.buildStandalone.href = location.href;
	el.buildStandalone.classList.toggle("hidden", window.top === window.self || !wasmSupportIssue());
	const [catalog, hasLocalServer] = await Promise.all([refreshBuildCatalog(), probeLocalServer()]);
	localServerAvailable = hasLocalServer;
	if (runtimeMode === "detecting" && hasLocalServer && params.get("mode")?.toLowerCase() !== "wasm" && !explicitUrl) {
		runtimeMode = "native";
		syncUiFromState();
		connectWs();
		if (!loadRequest) setBuildResult("Testing the local working tree. After edits, choose Rebuild & boot local.");
		return;
	}

	switchToWasmMode();
	syncUiFromState();
	if (loadRequest) return; // Don't overwrite a choice made during runtime detection.
	if (explicitUrl) {
		el.buildUrlInput.value = explicitUrl;
		return bootUrl(explicitUrl); // A bad explicit link must not boot some other revision.
	}
	if (catalog.latest) return bootLatest(catalog);
	followedBuild = "";
	setBuildResult(noPublishedMessage(catalog.catalogError), Boolean(catalog.catalogError));
	syncUiFromState();
	refreshSdList();
}

syncUiFromState();
bootstrap().catch(reportBuildError);
