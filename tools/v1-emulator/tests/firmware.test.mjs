import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { buildZip } from "../pack-wasm-bundle.mjs";
import { localCatalog, newestFile } from "../local-builds.mjs";
import { publishedBuilds } from "../public/build-catalog.js";
import { unpackFirmware } from "../public/firmware-bundle.js";

const entries = [
	{ name: "manifest.json", data: JSON.stringify({ format: "sonix-web-v1", entryJs: "player.js" }) },
	{ name: "player.js", data: "// Test build\n".repeat(64) },
	{ name: "sonix_player_wasm.wasm", data: Buffer.from([0, 97, 115, 109]) },
	{ name: "usr/test", data: "asset" },
];

test("extract the selected web firmware, directly or from one Actions wrapper", async () => {
	const direct = buildZip(entries);
	const nested = { name: "output/my.sonix-web.zip", data: direct };
	for (const archive of [direct, buildZip([nested])]) {
		const b = await unpackFirmware(archive);
		assert.equal(Buffer.from(b.jsBytes).toString(), entries[1].data);
		assert.deepEqual(Buffer.from(b.wasmBytes), entries[2].data);
		assert.equal(Buffer.from(b.usrFiles.get("usr/test")).toString(), "asset");
	}
	await assert.rejects(unpackFirmware(buildZip([nested, { ...nested, name: "other.sonix-web.zip" }])), /multiple builds/);
	await assert.rejects(unpackFirmware(buildZip(entries.slice(0, -1))), /assets/);
	await assert.rejects(unpackFirmware(buildZip(entries.filter((e) => e.name !== "player.js"))), /missing player.js/);
	await assert.rejects(unpackFirmware(buildZip([{ name: "v1.upt", data: "device image" }])), /sonix-v1-web-wasm.*same commit/);
	const deviceReport = { name: "BUILD-REPORT.txt", data: `Commit: ${"abcdef12".repeat(5)}\n` };
	await assert.rejects(unpackFirmware(buildZip([deviceReport])), /sonix-v1-web-wasm-abcdef12/);
	await assert.rejects(unpackFirmware(Buffer.from("not a zip")), /Not a firmware ZIP/);
});

test("published default wins, then newest build date, never lexical order", () => {
	const builds = [
		{ id: "old", file: "z.zip", builtAt: "2025-02-01" },
		{ id: "default", url: "builds/m.zip", builtAt: "2025-03-01" },
		{ id: "new", file: "a.zip", builtAt: "2025-04-01" },
		{ id: "missing", builtAt: "2026-01-01" },
	];
	assert.equal(publishedBuilds({ builds, defaultBuildId: "default" })[0].id, "cat:default");
	assert.equal(publishedBuilds({ builds, defaultBuildId: "gone" })[0].url, "./builds/a.zip");
	assert.deepEqual(publishedBuilds({ builds: [] }), []);
});

test("local rebuilds outrank stale staged bytes, even at the very same URL", (t) => {
	const dist = fs.mkdtempSync(path.join(os.tmpdir(), "sonix-builds-"));
	t.after(() => fs.rmSync(dist, { recursive: true, force: true }));
	const stage = path.join(dist, "pages/builds");
	fs.mkdirSync(stage, { recursive: true });
	const file = "a.sonix-web.zip";
	function write(dir, name, data, time) {
		fs.writeFileSync(path.join(dir, name), data);
		fs.utimesSync(path.join(dir, name), time, time);
	}
	write(stage, file, "staged", 100);
	write(stage, "manifest.json", JSON.stringify({ defaultBuildId: "a", builds: [{ id: "a", url: `builds/${file}` }] }), 110);
	write(dist, "z.sonix-web.zip", "older", 105);
	assert.equal(localCatalog(dist).defaultBuildId, "a");
	write(dist, file, "just built", 200);
	const catalog = localCatalog(dist);
	assert.equal(catalog.defaultBuildId, "a");
	assert.equal(catalog.builds[0].size, 10);
	assert.equal(catalog.builds.filter((b) => b.id === "a").length, 1);
	assert.equal(newestFile([dist, stage], file).file, path.join(dist, file));
	fs.unlinkSync(path.join(stage, "manifest.json"));
	assert.equal(localCatalog(dist).defaultBuildId, "a"); // z sorts first lexically in reverse order
});
