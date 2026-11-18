#!/usr/bin/env node
/**
 * Packages a compiled Emscripten sonix_player_wasm build + TempoTec V1 /usr
 * assets into a self-contained `.sonix-web.zip` bundle, and optionally stages
 * a complete static site directory ready for GitHub Pages deployment.
 *
 * Zero external npm dependencies (uses Node.js built-in zlib for ZIP deflate).
 */

import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { execSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(__dirname, "../..");
const PLAYER_DIR = path.join(REPO_ROOT, "sonix-player");
const ASSETS_USR_DIR = path.join(REPO_ROOT, "sonix-packer/assets/V1/usr");
const PUBLIC_DIR = path.join(__dirname, "public");
const DEFAULT_DIST_DIR = path.join(__dirname, "dist");

function parseArgs(argv) {
	const opts = {
		out: null,
		stagePages: null,
		label: null,
		ref: null,
		commit: null,
		existingManifest: null,
		maxHistory: 20,
	};
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === "--out" && argv[i + 1]) opts.out = path.resolve(argv[++i]);
		else if (a === "--stage-pages" && argv[i + 1]) opts.stagePages = path.resolve(argv[++i]);
		else if (a === "--label" && argv[i + 1]) opts.label = argv[++i];
		else if (a === "--ref" && argv[i + 1]) opts.ref = argv[++i];
		else if (a === "--commit" && argv[i + 1]) opts.commit = argv[++i];
		else if (a === "--merge-manifest" && argv[i + 1]) opts.existingManifest = path.resolve(argv[++i]);
		else if (a === "--max-history" && argv[i + 1]) opts.maxHistory = Math.max(1, Number(argv[++i]) || 20);
	}
	return opts;
}

function gitOr(cmd, fallback) {
	try {
		return execSync(cmd, { cwd: REPO_ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim() || fallback;
	} catch {
		return fallback;
	}
}

/* -------------------------------------------------------------------------
 * Minimal standard ZIP writer (Method 0 Store & Method 8 Deflate)
 * ------------------------------------------------------------------------- */
const CRC_TABLE = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
	let c = i;
	for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
	CRC_TABLE[i] = c >>> 0;
}

function crc32(buf) {
	let c = 0xffffffff;
	for (let i = 0; i < buf.length; i++) {
		c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
	}
	return (c ^ 0xffffffff) >>> 0;
}

export function buildZip(entries) {
	const chunks = [];
	const central = [];
	let offset = 0;
	const now = new Date();
	const dosTime = ((now.getHours() & 0x1f) << 11) | ((now.getMinutes() & 0x3f) << 5) | ((now.getSeconds() >> 1) & 0x1f);
	const dosDate = (((Math.max(1980, now.getFullYear()) - 1980) & 0x7f) << 9) | (((now.getMonth() + 1) & 0xf) << 5) | (now.getDate() & 0x1f);

	for (const entry of entries) {
		const nameBuf = Buffer.from(entry.name.replace(/\\/g, "/"), "utf8");
		const raw = Buffer.isBuffer(entry.data) ? entry.data : Buffer.from(entry.data);
		const crc = crc32(raw);

		let method = 0;
		let comp = raw;
		if (entry.compress !== false && raw.length > 64) {
			const deflated = zlib.deflateRawSync(raw, { level: 6 });
			if (deflated.length + 16 < raw.length) {
				method = 8;
				comp = deflated;
			}
		}

		// Local file header (30 bytes + name)
		const lfh = Buffer.alloc(30);
		lfh.writeUInt32LE(0x04034b50, 0);
		lfh.writeUInt16LE(20, 4); // version needed 2.0
		lfh.writeUInt16LE(0x0800, 6); // UTF-8 filename flag
		lfh.writeUInt16LE(method, 8);
		lfh.writeUInt16LE(dosTime, 10);
		lfh.writeUInt16LE(dosDate, 12);
		lfh.writeUInt32LE(crc, 14);
		lfh.writeUInt32LE(comp.length, 18);
		lfh.writeUInt32LE(raw.length, 22);
		lfh.writeUInt16LE(nameBuf.length, 26);
		lfh.writeUInt16LE(0, 28);

		chunks.push(lfh, nameBuf, comp);

		// Central directory header (46 bytes + name)
		const cdh = Buffer.alloc(46);
		cdh.writeUInt32LE(0x02014b50, 0);
		cdh.writeUInt16LE(20, 4);
		cdh.writeUInt16LE(20, 6);
		cdh.writeUInt16LE(0x0800, 8);
		cdh.writeUInt16LE(method, 10);
		cdh.writeUInt16LE(dosTime, 12);
		cdh.writeUInt16LE(dosDate, 14);
		cdh.writeUInt32LE(crc, 16);
		cdh.writeUInt32LE(comp.length, 20);
		cdh.writeUInt32LE(raw.length, 24);
		cdh.writeUInt16LE(nameBuf.length, 28);
		cdh.writeUInt16LE(0, 30);
		cdh.writeUInt16LE(0, 32);
		cdh.writeUInt16LE(0, 34);
		cdh.writeUInt16LE(0, 36);
		cdh.writeUInt32LE(0, 38);
		cdh.writeUInt32LE(offset, 42);

		central.push(cdh, nameBuf);
		offset += lfh.length + nameBuf.length + comp.length;
	}

	const cdBuf = Buffer.concat(central);
	const eocd = Buffer.alloc(22);
	eocd.writeUInt32LE(0x06054b50, 0);
	eocd.writeUInt16LE(0, 4);
	eocd.writeUInt16LE(0, 6);
	eocd.writeUInt16LE(entries.length, 8);
	eocd.writeUInt16LE(entries.length, 10);
	eocd.writeUInt32LE(cdBuf.length, 12);
	eocd.writeUInt32LE(offset, 16);
	eocd.writeUInt16LE(0, 20);

	return Buffer.concat([...chunks, cdBuf, eocd]);
}

function collectTree(rootDir, zipPrefix, outEntries) {
	const items = fs.readdirSync(rootDir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name));
	for (const ent of items) {
		const full = path.join(rootDir, ent.name);
		const rel = `${zipPrefix}/${ent.name}`;
		if (ent.isDirectory()) {
			collectTree(full, rel, outEntries);
		} else if (ent.isFile()) {
			const ext = path.extname(ent.name).toLowerCase();
			// Skip re-compressing already-compressed formats (png, jpg, woff2, gz)
			const compress = ![".png", ".jpg", ".jpeg", ".webp", ".gz", ".zip"].includes(ext);
			outEntries.push({ name: rel, data: fs.readFileSync(full), compress });
		}
	}
}

function sanitizeSlug(str) {
	return String(str || "build")
		.replace(/^refs\/(heads|tags)\//, "")
		.replace(/[^a-zA-Z0-9._-]+/g, "-")
		.replace(/^-+|-+$/g, "")
		.slice(0, 48) || "build";
}

function main() {
	const opts = parseArgs(process.argv.slice(2));
	const wasmJsPath = path.join(PLAYER_DIR, "sonix_player_wasm.js");
	const wasmBinPath = path.join(PLAYER_DIR, "sonix_player_wasm.wasm");

	if (!fs.existsSync(wasmJsPath) || !fs.existsSync(wasmBinPath)) {
		console.error("Error: sonix_player_wasm.js / .wasm not found. Run 'make -C sonix-player web-wasm' first.");
		process.exit(1);
	}
	if (!fs.existsSync(ASSETS_USR_DIR)) {
		console.error(`Error: V1 assets directory missing: ${ASSETS_USR_DIR}`);
		process.exit(1);
	}

	const commit = opts.commit || gitOr("git rev-parse HEAD", "unknown");
	const shortCommit = commit.slice(0, 8);
	const ref = opts.ref || process.env.GITHUB_REF_NAME || gitOr("git rev-parse --abbrev-ref HEAD", "local");
	const subject = gitOr("git log -1 --pretty=%s", "Sonix TempoTec V1 build");
	const builtAt = new Date().toISOString();
	const slug = sanitizeSlug(ref);
	const bundleFilename = `sonix-v1-${slug}-${shortCommit}.sonix-web.zip`;
	const label = opts.label || `${ref} (${shortCommit})`;

	const wasmJs = fs.readFileSync(wasmJsPath);
	const wasmBin = fs.readFileSync(wasmBinPath);

	const manifest = {
		format: "sonix-web-v1",
		id: `${slug}-${shortCommit}`,
		name: label,
		ref,
		commit,
		shortCommit,
		subject,
		builtAt,
		panel: "v1",
		entryJs: "sonix_player_wasm.js",
		wasm: "sonix_player_wasm.wasm",
		usrPrefix: "usr/",
		wasmBytes: wasmBin.length,
	};

	const entries = [
		{ name: "manifest.json", data: Buffer.from(JSON.stringify(manifest, null, 2) + "\n", "utf8"), compress: false },
		{ name: "sonix_player_wasm.js", data: wasmJs, compress: true },
		{ name: "sonix_player_wasm.wasm", data: wasmBin, compress: true },
	];
	// Some Emscripten releases emit a separate pthread bootstrap; newer ones
	// start workers from the main JS. Keep the bundle self-contained in both cases.
	const worker = "sonix_player_wasm.worker.js";
	if (fs.existsSync(path.join(PLAYER_DIR, worker))) {
		entries.push({ name: worker, data: fs.readFileSync(path.join(PLAYER_DIR, worker)) });
	}
	collectTree(ASSETS_USR_DIR, "usr", entries);

	const zipBuf = buildZip(entries);
	const outPath = opts.out || path.join(DEFAULT_DIST_DIR, bundleFilename);
	fs.mkdirSync(path.dirname(outPath), { recursive: true });
	fs.writeFileSync(`${outPath}.tmp`, zipBuf);
	fs.renameSync(`${outPath}.tmp`, outPath);

	const mb = (zipBuf.length / (1024 * 1024)).toFixed(2);
	console.log(`Packed ${outPath} (${mb} MB, ${entries.length} files, ref=${ref} @ ${shortCommit})`);

	if (opts.stagePages) {
		const siteDir = opts.stagePages;
		const buildsDir = path.join(siteDir, "builds");
		fs.mkdirSync(buildsDir, { recursive: true });

		// Copy static frontend assets
		for (const file of fs.readdirSync(PUBLIC_DIR)) {
			const src = path.join(PUBLIC_DIR, file);
			if (fs.statSync(src).isFile()) {
				fs.copyFileSync(src, path.join(siteDir, file));
			}
		}
		fs.writeFileSync(path.join(siteDir, ".nojekyll"), "");

		// Copy bundle into builds/
		const stagedBundleName = path.basename(outPath);
		fs.copyFileSync(outPath, path.join(buildsDir, stagedBundleName));

		// Build or merge builds/manifest.json
		let previousBuilds = [];
		const manifestSrc = opts.existingManifest && fs.existsSync(opts.existingManifest)
			? opts.existingManifest
			: path.join(buildsDir, "manifest.json");
		if (fs.existsSync(manifestSrc)) {
			try {
				const parsed = JSON.parse(fs.readFileSync(manifestSrc, "utf8"));
				if (Array.isArray(parsed.builds)) previousBuilds = parsed.builds;
			} catch {}
		}

		const currentEntry = {
			id: manifest.id,
			name: manifest.name,
			ref: manifest.ref,
			commit: manifest.commit,
			shortCommit: manifest.shortCommit,
			subject: manifest.subject,
			builtAt: manifest.builtAt,
			panel: manifest.panel,
			size: zipBuf.length,
			url: `builds/${stagedBundleName}`,
		};

		// Keep latest entry for this ref/id first, preserve older builds that still exist
		const deduped = [currentEntry];
		for (const b of previousBuilds) {
			if (!b || !b.url) continue;
			if (b.id === currentEntry.id || b.url === currentEntry.url) continue;
			// Only keep relative builds/* entries whose bundle actually exists in siteDir
			if (!/^https?:\/\//i.test(b.url)) {
				const candidate = path.resolve(siteDir, b.url);
				if (!fs.existsSync(candidate)) continue;
			}
			// If same branch ref was rebuilt, replace older commit for that branch
			if (b.ref && b.ref === currentEntry.ref && !/^v?\d/.test(b.ref)) continue;
			deduped.push(b);
			if (deduped.length >= opts.maxHistory) break;
		}

		const catalog = {
			format: "sonix-web-catalog-v1",
			updatedAt: builtAt,
			defaultBuildId: currentEntry.id,
			builds: deduped,
		};
		fs.writeFileSync(path.join(buildsDir, "manifest.json"), JSON.stringify(catalog, null, 2) + "\n");
		console.log(`Staged static GitHub Pages site in ${siteDir} (${deduped.length} build(s) in builds/manifest.json)`);
	}
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
