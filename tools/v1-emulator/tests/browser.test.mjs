import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import test from "node:test";
import { chromium } from "playwright-core";
import { buildZip } from "../pack-wasm-bundle.mjs";

// Small, distinguishable builds for loader tests. The asset pixel proves which
// bundle supplied the screen, even for an uncommitted rebuild with the same ID.
function fixture(name, pixel, fail = false) {
	return buildZip([
		{ name: "manifest.json", data: JSON.stringify({ id: name, name, shortCommit: name }) },
		{ name: "sonix_player_wasm.wasm", data: Buffer.from([0, 97, 115, 109]) },
		{ name: "usr/pixel", data: Buffer.from([pixel & 255, pixel >> 8]) },
		{ name: "sonix_player_wasm.js", data: `var createSonixPlayer = async (m) => {
			const files = new Map(); m.ENV = {};
			m.FS = { mkdir() {}, chdir() {}, readdir: () => [], writeFile: (p, v) => files.set(p,v), readFile: p => files.get(p) };
			m.preRun[0](m);
			setTimeout(() => ${fail ? 'm.onExit(1)' : 'm._sonixBridge.onFrame(files.get("/usr/pixel"), 1, 1)'}, 30);
			return m;
		};` },
	]);
}
const wrapped = (bytes) => buildZip([{ name: "run/build.sonix-web.zip", data: bytes, compress: false }]);

// Serve at a project-site subpath, with a deliberately cacheable catalog and
// bundle, as on static hosting. The real COI service worker supplies isolation.
async function workbench(t, latest) {
	const state = { latest, builtAt: "2026-01-01", downloads: 0 };
	const server = http.createServer((req, res) => {
		const file = req.url.split("?")[0].replace(/^\/project\//, "") || "index.html";
		res.setHeader("Cache-Control", "public, max-age=3600");
		if (file === "builds/manifest.json") {
			res.setHeader("Content-Type", "application/json");
			return res.end(JSON.stringify({ defaultBuildId: "main", builds: [
				{ id: "past", name: "Past", url: "builds/past.zip" },
				{ id: "main", name: "Main", url: "builds/latest.zip", builtAt: state.builtAt },
			] }));
		}
		if (file === "builds/latest.zip") {
			state.downloads++;
			if (state.holdDownload) return state.holdDownload(res);
			return res.end(state.latest);
		}
		if (file === "builds/past.zip") return res.end(fixture("Past", 31));
		if (/^[\w-]+\.(js|css|html)$/.test(file)) {
			const asset = new URL(`../public/${file}`, import.meta.url);
			if (fs.existsSync(asset)) {
				res.setHeader("Content-Type", file.endsWith("js") ? "text/javascript" : file.endsWith("html") ? "text/html" : "text/css");
				return fs.createReadStream(asset).pipe(res);
			}
		}
		res.writeHead(404).end();
	});
	await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
	t.after(() => server.close());
	const executablePath = process.env.CHROME_BIN;
	const browser = await chromium.launch(executablePath ? { executablePath } : { channel: "chrome" });
	t.after(() => browser.close());
	const page = await browser.newPage();
	const url = `http://localhost:${server.address().port}/project/`;
	return { state, page, url };
}

function waitForBoot(page, name, rgb, status = "Booted") {
	return page.waitForFunction(([name, rgb, status]) => {
		const text = (id) => document.getElementById(id)?.textContent;
		const canvas = document.getElementById("v1-canvas");
		const pixel = canvas?.getContext("2d").getImageData(0, 0, 1, 1).data;
		return text("build-result")?.startsWith(`${status}: ${name}`) && text("lifecycle-pill") === "Running"
			&& rgb.every((v, i) => pixel[i] === v);
	}, [name, rgb, status]);
}

test("fresh default, follow-latest, Actions import, same-file edits and pinned selection", async (t) => {
	const { page, url, state } = await workbench(t, fixture("First", 0xf800));
	await page.goto(url);
	await waitForBoot(page, "First", [255, 0, 0], "Following latest");
	state.latest = fixture("Second", 31);
	state.builtAt = "2026-01-02";
	await page.evaluate(() => window.dispatchEvent(new Event("focus")));
	await waitForBoot(page, "Second", [0, 0, 255], "Following latest");

	const upload = (bytes) => page.locator("#build-file-input").setInputFiles({ name: "my-build.zip", mimeType: "application/zip", buffer: bytes });
	const [chooser] = await Promise.all([page.waitForEvent("filechooser"), page.getByRole("button", { name: "Browse ZIP" }).press("Enter")]);
	await chooser.setFiles({ name: "my-build.zip", mimeType: "application/zip", buffer: wrapped(fixture("Mine", 0x07e0)) });
	await waitForBoot(page, "Mine", [0, 255, 0]);
	await upload(fixture("Mine", 0xffff)); // same filename, manifest id and commit; new firmware/asset bytes
	await waitForBoot(page, "Mine", [255, 255, 255]);
	assert.equal(await page.locator("#build-select").inputValue(), "Mine");
	await upload(buildZip([{ name: "v1.upt", data: "device only" }]));
	await page.waitForSelector("#build-result.error");
	assert.match(await page.locator("#build-result").textContent(), /sonix-v1-web-wasm/);
	assert.match(await page.locator("#brand-sub").textContent(), /Mine/);
	assert.equal(await page.locator("#lifecycle-pill").textContent(), "Running");

	// Reload defaults to the publication, not the saved import. Boot the saved
	// entry explicitly and verify it contains the *second* set of bytes.
	await page.reload();
	await waitForBoot(page, "Second", [0, 0, 255], "Following latest");
	await page.selectOption("#build-select", "Mine");
	await page.click("#btn-build-load");
	await waitForBoot(page, "Mine", [255, 255, 255]);
	state.latest = fixture("Third", 0xf800);
	state.builtAt = "2026-01-03";
	const downloadsBeforeFocus = state.downloads;
	await page.evaluate(() => window.dispatchEvent(new Event("focus")));
	await page.fill("#build-url-input", url + "builds/past.zip");
	await page.locator("#build-url-input").press("Enter");
	await waitForBoot(page, "Past", [0, 0, 255]);
	assert.equal(state.downloads, downloadsBeforeFocus, "explicit selections stay pinned after focus");
	await page.click("#btn-build-latest");
	await waitForBoot(page, "Third", [255, 0, 0], "Following latest");

	const before = state.downloads;
	await page.goto(url + "?build=builds/past.zip");
	await waitForBoot(page, "Past", [0, 0, 255]);
	await page.goto(url + "?build=missing.zip");
	await page.waitForSelector("#build-result.error");
	assert.match(await page.locator("#build-result").textContent(), /HTTP 404/);
	assert.equal(state.downloads, before, "a failed explicit URL must not silently start latest");
	await upload(fixture("Broken", 0, true));
	await page.waitForFunction(() => document.querySelector("#build-result.error")?.textContent.includes("exited during boot"));
	assert.equal(await page.locator("#lifecycle-pill").textContent(), "Stopped");
});

test("dropping an artifact supersedes an unfinished automatic download", { timeout: 45000 }, async (t) => {
	const { page, url, state } = await workbench(t, fixture("Slow", 31));
	let response;
	const downloading = new Promise((resolve) => {
		state.holdDownload = (res) => { response = res; resolve(); };
	});
	await page.goto(url);
	await downloading;
	await page.evaluate((bytes) => {
		const transfer = new DataTransfer();
		transfer.items.add(new File([new Uint8Array(bytes)], "custom.zip"));
		document.getElementById("build-dropzone").dispatchEvent(new DragEvent("drop", { bubbles: true, dataTransfer: transfer }));
	}, [...wrapped(fixture("Dropped", 0x07e0))]);
	await waitForBoot(page, "Dropped", [0, 255, 0]);
	response.end(state.latest);
	assert.equal(await page.locator("#build-select").inputValue(), "Dropped");
});

const realBundle = process.env.SONIX_TEST_BUNDLE;
test("compiled firmware draws a real screen, then hot-boots from an Actions ZIP", { skip: !realBundle }, async (t) => {
	const bytes = fs.readFileSync(realBundle);
	const { page, url } = await workbench(t, bytes);
	await page.goto(url);
	for (const prefix of ["Following latest:", "Booted:"]) {
		if (prefix === "Booted:") await page.locator("#build-file-input").setInputFiles({ name: "artifact.zip", mimeType: "application/zip", buffer: wrapped(bytes) });
		await page.waitForFunction((prefix) => {
			const canvas = document.getElementById("v1-canvas");
			if (!document.getElementById("build-result")?.textContent.startsWith(prefix) || canvas.width !== 240 || canvas.height !== 320) return false;
			const data = canvas.getContext("2d").getImageData(0, 0, 240, 320).data;
			return new Set(new Uint32Array(data.buffer)).size > 8;
		}, prefix, { timeout: 60000 });
		assert.equal(await page.locator("#lifecycle-pill").textContent(), "Running");
	}
});
