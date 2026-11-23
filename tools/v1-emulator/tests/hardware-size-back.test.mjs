/**
 * Front-end checks for the two hardware-fidelity features:
 *
 *   1. "Real size" — the 2.0" 240x320 panel is drawn at its true 30.48 x 40.64
 *      mm inside the true 41.8 x 87.2 x 13.75 mm body. A phone or tablet is
 *      identified from its portrait CSS viewport plus devicePixelRatio, so the
 *      density is the panel's real dpi; everywhere else a 1in CSS probe is the
 *      fallback and the readout says the size is an approximation.
 *   2. The rear shell — the real Variations V1 back: dark navy aluminium plate
 *      with the smooth cap whose edge steps UP to the right, the copper chamfer
 *      insert in that riser, the exposed Torx screw, the 45deg milled rib field
 *      and the TempoTec piano-key print up the lower right.
 *
 * These only exercise the static UI, so they run without a firmware bundle: the
 * WASM backend never boots and the screen overlay stays up.
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import test from "node:test";
import { chromium } from "playwright-core";

const PUBLIC = new URL("../public/", import.meta.url);

async function open(t, viewport = { width: 1440, height: 900 }) {
	const server = http.createServer((req, res) => {
		const file = (req.url.split("?")[0] || "/").replace(/^\//, "") || "index.html";
		const full = path.join(PUBLIC.pathname, file);
		if (!full.startsWith(PUBLIC.pathname) || !fs.existsSync(full) || fs.statSync(full).isDirectory()) {
			res.writeHead(404).end();
			return;
		}
		res.setHeader("Content-Type", file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html");
		fs.createReadStream(full).pipe(res);
	});
	await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
	t.after(() => server.close());
	const executablePath = process.env.CHROME_BIN;
	const browser = await chromium.launch(executablePath ? { executablePath } : { channel: "chrome" });
	t.after(() => browser.close());
	const page = await browser.newPage({ viewport });
	page.on("pageerror", (err) => {
		throw err;
	});
	await page.goto(`http://127.0.0.1:${server.address().port}/`);
	await page.waitForSelector("#v1-assembly");
	return page;
}

/**
 * Pin the display conversion to a known value: a 1in probe laid out 400 CSS px
 * wide means one CSS px is 0.0635 mm, so the panel's 0.127 mm device pixel
 * needs 2 CSS px. The probe is defined in inches, so its measured px width is
 * what the browser believes one inch is.
 */
async function calibrate(page, probePx = 400) {
	await page.addStyleTag({ content: `.real-size-probe{width:${probePx}px!important}` });
}

const mmPerCssPx = (probePx) => 25.4 / probePx;

test("Real size draws the 2.0in panel at its true 30.48 x 40.64 mm", async (t) => {
	const page = await open(t);
	await calibrate(page, 400); // 1 CSS px = 0.0635 mm
	await page.click('[data-scale="real"]');
	await page.evaluate(() => window.dispatchEvent(new Event("resize")));
	await page.waitForTimeout(150);

	const out = await page.evaluate(() => {
		const box = (sel) => {
			const r = document.querySelector(sel).getBoundingClientRect();
			return { w: r.width, h: r.height };
		};
		return {
			mode: document.documentElement.dataset.scale,
			scale: Number(getComputedStyle(document.documentElement).getPropertyValue("--screen-scale")),
			screen: box("#v1-screen-stage"),
			body: box("#v1-chassis"),
			badgeVisible: !document.getElementById("v1-real-badge").classList.contains("hidden"),
			badgeText: document.getElementById("v1-real-badge").textContent,
			noteVisible: !document.getElementById("real-size-note").classList.contains("hidden"),
			noteText: document.getElementById("real-size-note-detail").textContent,
		};
	});

	const mm = mmPerCssPx(400);
	assert.equal(out.mode, "real");
	// 0.127 mm per device pixel / 0.0635 mm per CSS px = 2
	assert.ok(Math.abs(out.scale - 0.127 / mm) < 0.002, `scale should be ${(0.127 / mm).toFixed(3)}, got ${out.scale}`);
	// The panel is 240x320 device px at that scale
	assert.ok(Math.abs(out.screen.w * mm - 30.48) < 0.2, `panel should be 30.48 mm wide, got ${(out.screen.w * mm).toFixed(2)}`);
	assert.ok(Math.abs(out.screen.h * mm - 40.64) < 0.3, `panel should be 40.64 mm tall, got ${(out.screen.h * mm).toFixed(2)}`);
	// The body is the real 41.8 x 87.2 mm (5px frame each side included)
	assert.ok(Math.abs(out.body.w * mm - 41.8) < 0.5, `body should be 41.8 mm wide, got ${(out.body.w * mm).toFixed(2)}`);
	assert.ok(Math.abs(out.body.h * mm - 87.2) < 1.0, `body should be 87.2 mm tall, got ${(out.body.h * mm).toFixed(2)}`);
	// The readout reports those measurements rather than claiming a bare "1:1"
	assert.equal(out.badgeVisible, true);
	assert.equal(out.noteVisible, true);
	assert.match(out.badgeText, /30\.5 × 40\.6 mm/);
	assert.match(out.noteText, /41\.8 × 87\.2 × 13\.75 mm/);
});

test("Real size survives a change of display resolution", async (t) => {
	const page = await open(t);
	await calibrate(page, 400);
	await page.click('[data-scale="real"]');
	await page.waitForTimeout(120);
	// A denser display: 1in now measures 600 CSS px (0.0423 mm per px)
	await calibrate(page, 600);
	await page.evaluate(() => window.dispatchEvent(new Event("resize")));
	await page.waitForTimeout(150);
	const out = await page.evaluate(() => {
		const r = document.getElementById("v1-screen-stage").getBoundingClientRect();
		return { w: r.width, scale: Number(getComputedStyle(document.documentElement).getPropertyValue("--screen-scale")) };
	});
	assert.ok(Math.abs(out.scale - 0.127 / mmPerCssPx(600)) < 0.002, `scale should follow the new probe, got ${out.scale}`);
	assert.ok(Math.abs(out.w * mmPerCssPx(600) - 30.48) < 0.2, `panel should still be 30.48 mm, got ${(out.w * mmPerCssPx(600)).toFixed(2)}`);
});

test("leaving Real size restores the fitted view and hides the readout", async (t) => {
	const page = await open(t);
	await page.click('[data-scale="real"]');
	assert.equal(await page.locator("#real-size-note").isVisible(), true);
	await page.click('[data-scale="1.25"]');
	assert.equal(await page.locator("#real-size-note").isVisible(), false);
	assert.equal(await page.locator("#v1-real-badge").isVisible(), false);
	const w = await page.evaluate(() => document.getElementById("v1-screen-stage").getBoundingClientRect().width);
	// The chassis is modelled in the panel's 240 device px, so the multiplier is
	// relative to native: 1.25x = 0.91 CSS px per device px = 218 px across.
	assert.ok(Math.abs(w - 240 * 0.91) < 2, `1.25x should give a 218 px wide panel, got ${w.toFixed(1)}`);
});

test("Fit keeps the whole device, jacks and USB-C included, inside the stage", async (t) => {
	const page = await open(t, { width: 1440, height: 900 });
	await page.click('[data-scale="fit"]');
	await page.waitForTimeout(200);
	const out = await page.evaluate(() => {
		const r = (sel) => document.querySelector(sel).getBoundingClientRect();
		const wrap = r(".stage-canvas-wrap");
		const asm = r("#v1-assembly");
		const usbc = r("#v1-usbc");
		return { wrap: { top: wrap.top, bottom: wrap.bottom }, asmTop: asm.top, usbcBottom: usbc.bottom };
	});
	assert.ok(out.asmTop >= out.wrap.top - 12, "the jack strip stays inside the stage");
	assert.ok(out.usbcBottom <= out.wrap.bottom + 12, "the USB-C port stays inside the stage");
});

test("the fixed multipliers draw a usable device, not an oversized one", async (t) => {
	const page = await open(t, { width: 1440, height: 900 });
	// Expected panel widths in CSS px. These match what the previous release drew
	// (240/300/360/480) now that the chassis is modelled in real device pixels,
	// so the numbers on the buttons mean what they used to.
	const expected = { "1": 175, "1.25": 218, "1.5": 262, "2": 350 };
	for (const [mode, want] of Object.entries(expected)) {
		await page.click(`[data-scale="${mode}"]`);
		await page.waitForTimeout(120);
		const out = await page.evaluate(() => {
			const r = document.getElementById("v1-screen-stage").getBoundingClientRect();
			const pane = document.querySelector(".stage-pane");
			return { w: r.width, paneScroll: pane.scrollHeight > pane.clientHeight + 2 };
		});
		assert.ok(Math.abs(out.w - want) < 3, `${mode}x should draw a ${want} px panel, got ${out.w.toFixed(1)}`);
		// The default view on a normal desktop window must not need scrolling
		if (mode === "1.25" || mode === "1") {
			assert.equal(out.paneScroll, false, `${mode}x should fit a 1440x900 stage without scrolling`);
		}
	}
	// 2x is the largest preset and stays under the stage width
	const widest = await page.evaluate(() => document.getElementById("v1-chassis").getBoundingClientRect().width);
	assert.ok(widest < 700, `the 2x device is ${widest.toFixed(0)} px wide, well inside the stage`);
});

test("a phone is identified and Real size is exact there, not the 96 dpi guess", async (t) => {
	// 390 CSS px at devicePixelRatio 3 is an iPhone 12-16 class 460 ppi panel.
	// The CSS millimetre would read 0.48 px/device-px here; the panel density
	// gives the true 0.7667.
	const page = await open(t, { width: 390, height: 664 });
	await page.evaluate(() => Object.defineProperty(window, "devicePixelRatio", { value: 3, configurable: true }));
	await page.click('[data-scale="real"]');
	await page.evaluate(() => window.dispatchEvent(new Event("resize")));
	await page.waitForTimeout(200);
	const out = await page.evaluate(() => {
		const r = document.getElementById("v1-screen-stage").getBoundingClientRect();
		return {
			scale: Number(getComputedStyle(document.documentElement).getPropertyValue("--screen-scale")),
			w: r.width,
			badge: document.getElementById("v1-real-badge").textContent,
			source: document.getElementById("real-size-note-source").textContent,
			approx: document.getElementById("v1-real-badge").classList.contains("v1-real-badge-approx"),
		};
	});
	// 3 x 25.4 / 460 mm per CSS px, so 0.127 / that = 0.7667
	assert.ok(Math.abs(out.scale - 0.7667) < 0.01, `iPhone panel scale should be ~0.767, got ${out.scale}`);
	assert.match(out.source, /460 ppi/, "the readout names the panel it measured against");
	assert.match(out.badge, /^Screen 1:1/, "a known panel gets an exact 1:1 claim, not an approximation");
	assert.equal(out.approx, false);
	assert.match(out.badge, /30\.5 × 40\.6 mm/);
	assert.ok(Math.abs(out.w - 240 * out.scale) < 2, "the panel is drawn 240 device px across");
});

test("an unidentifiable display says the size is approximate instead of claiming 1:1", async (t) => {
	const page = await open(t, { width: 1440, height: 900 });
	await page.click('[data-scale="real"]');
	await page.waitForTimeout(150);
	const out = await page.evaluate(() => ({
		badge: document.getElementById("v1-real-badge").textContent,
		approx: document.getElementById("v1-real-badge").classList.contains("v1-real-badge-approx"),
		source: document.getElementById("real-size-note-source").textContent,
	}));
	// A 1440 px desktop cannot be identified, and headless Chromium reports 96 dpi
	assert.equal(out.approx, true);
	assert.match(out.badge, /≈1:1/);
	assert.match(out.source, /96 dpi/);
});

test("the front chin spans the glass, with the LED and V1 mark placed as on the banner", async (t) => {
	const page = await open(t, { width: 1600, height: 1100 });
	await page.click('[data-scale="2"]');
	await page.waitForTimeout(250);
	const out = await page.evaluate(() => {
		const chin = document.querySelector(".v1-chin").getBoundingClientRect();
		const glass = document.querySelector(".v1-front-glass").getBoundingClientRect();
		const svg = document.querySelector(".v1-mark-svg").getBoundingClientRect();
		const core = document.querySelector("#v1-led-core").getBoundingClientRect();
		// Ink extents inside the 66x30 viewBox, so padding does not confuse the check
		const inkR = svg.left + (svg.width * 61.5) / 66;
		const inkL = svg.left + (svg.width * 2) / 66;
		const inkB = svg.top + (svg.height * 27.5) / 30;
		const inkT = svg.top + (svg.height * 3) / 30;
		return {
			chinW: chin.width, chinH: chin.height, glassW: glass.width,
			inkW: inkR - inkL, inkH: inkB - inkT,
			inkRightGap: chin.right - inkR, inkBottomGap: chin.bottom - inkB,
			ledLeft: core.left - chin.left,
			ledW: core.width, ledH: core.height,
			ledCentreUp: chin.bottom - (core.top + core.height / 2),
		};
	});
	// The chin is the glass sheet edge to edge. It used to resolve `width: 100%`
	// against the 240 px screen inside the padded glass and came out ~73% of it.
	assert.ok(
		Math.abs(out.chinW - out.glassW) < 1.5,
		`the chin spans the full glass: ${out.chinW.toFixed(1)} vs ${out.glassW.toFixed(1)} px`,
	);
	// Placement taken from the Sonix Player 1.7.1.1 banner, as fractions of the
	// glass panel: the V1 ink is 17.8% of the width, ends 11.7% in from the right
	// and sits 13.7% up; the LED starts 10.1% in from the left and its lower end
	// is 13.3% above the bottom. Both share one centre line.
	const pct = (v, ref) => (v / ref) * 100;
	assert.ok(Math.abs(pct(out.inkW, out.chinW) - 17.8) < 2.5, `V1 mark is ~17.8% of the chin width, got ${pct(out.inkW, out.chinW).toFixed(1)}%`);
	assert.ok(Math.abs(pct(out.inkRightGap, out.chinW) - 11.7) < 2.5, `V1 sits ~11.7% in from the right, got ${pct(out.inkRightGap, out.chinW).toFixed(1)}%`);
	assert.ok(Math.abs(pct(out.inkBottomGap, out.chinH) - 13.7) < 2.5, `V1 sits ~13.7% up from the bottom, got ${pct(out.inkBottomGap, out.chinH).toFixed(1)}%`);
	assert.ok(Math.abs(pct(out.ledLeft, out.chinW) - 10.1) < 2.5, `LED is ~10.1% in from the left, got ${pct(out.ledLeft, out.chinW).toFixed(1)}%`);
	// The banner's pill is a tall capsule: 2.3% of the width by 11.5% of the chin
	assert.ok(Math.abs(pct(out.ledW, out.chinW) - 2.3) < 1.2, `LED is ~2.3% of the chin wide, got ${pct(out.ledW, out.chinW).toFixed(1)}%`);
	assert.ok(Math.abs(pct(out.ledH, out.chinH) - 11.5) < 2.5, `LED is ~11.5% of the chin tall, got ${pct(out.ledH, out.chinH).toFixed(1)}%`);
	// Both sit on the same side of the chin only if the corners are respected:
	// the mark must stay well right of centre and the LED well left of it.
	assert.ok(out.ledLeft < out.chinW * 0.2, "the LED stays in the left corner");
	assert.ok(out.inkRightGap < out.chinW * 0.25, "the mark stays in the right corner");
});

test("the rear shell shows the real V1 back", async (t) => {
	const page = await open(t);
	await page.click("#btn-flip-device");
	await page.waitForTimeout(700); // let the flip settle

	for (const sel of [
		"#v1-chassis-back .v1-back-top",
		"#v1-chassis-back .v1-back-plate",
		"#v1-chassis-back .v1-back-plate-cap",
		"#v1-chassis-back .v1-back-plate-ribs",
		"#v1-chassis-back .v1-back-step .v1-back-accent",
		"#v1-chassis-back .v1-back-screw svg",
		"#v1-chassis-back .v1-back-brand svg",
	]) {
		assert.equal(await page.locator(sel).count(), 1, `${sel} exists on the rear shell`);
	}

	// The rear chassis must fill the same box as the front: it used to collapse
	// to the height of its own padding, which blanked the back view.
	const box = await page.evaluate(() => {
		const r = (sel) => {
			const b = document.querySelector(sel).getBoundingClientRect();
			return { w: b.width, h: b.height };
		};
		return { front: r("#v1-chassis"), back: r("#v1-chassis-back"), shell: r("#v1-chassis-back .v1-back-shell") };
	});
	assert.ok(Math.abs(box.back.w - box.front.w) < 1, "rear chassis matches the front width");
	assert.ok(Math.abs(box.back.h - box.front.h) < 1, "rear chassis matches the front height");
	assert.ok(box.shell.h > box.back.h * 0.95, "the rear plate fills the shell");

	// Feature placement, as fractions of the rear shell
	const geo = await page.evaluate(() => {
		const body = document.querySelector("#v1-chassis-back .v1-back-shell").getBoundingClientRect();
		const frac = (sel) => {
			const b = document.querySelector(`#v1-chassis-back ${sel}`).getBoundingClientRect();
			return {
				x: (b.left - body.left) / body.width,
				y: (b.top - body.top) / body.height,
				w: b.width / body.width,
				h: b.height / body.height,
			};
		};
		return {
			top: frac(".v1-back-top"),
			screw: frac(".v1-back-screw"),
			brand: frac(".v1-back-brand"),
			accentPoints: [...document.querySelectorAll("#v1-chassis-back .v1-back-accent")].map((p) => p.getAttribute("points")),
			seam: document.querySelector("#v1-chassis-back .v1-back-step-line").getAttribute("d"),
		};
	});
	// Antenna cap on the top ~21% of the rear
	assert.ok(geo.top.h > 0.19 && geo.top.h < 0.24, `antenna cap is ~0.214 H tall, got ${geo.top.h.toFixed(3)}`);
	// Screw just right of the copper insert, centred around 0.886 W / 0.19 H
	assert.ok(Math.abs(geo.screw.x + geo.screw.w / 2 - 0.886) < 0.02, `screw centre x ~0.886 W, got ${(geo.screw.x + geo.screw.w / 2).toFixed(3)}`);
	assert.ok(Math.abs(geo.screw.y + geo.screw.h / 2 - 0.192) < 0.03, `screw centre y ~0.192 H, got ${(geo.screw.y + geo.screw.h / 2).toFixed(3)}`);
	assert.ok(geo.screw.w > 0.035 && geo.screw.w < 0.07, `screw is ~5% of the width, got ${geo.screw.w.toFixed(3)}`);
	// TempoTec print up the lower right, reading bottom-to-top
	assert.ok(geo.brand.x > 0.68 && geo.brand.x < 0.84, `print hugs the right side, got ${geo.brand.x.toFixed(3)}`);
	assert.ok(geo.brand.y > 0.5 && geo.brand.y < 0.75, `print sits in the lower half, got ${geo.brand.y.toFixed(3)}`);
	// The step rises to the right: flat at 23.55 H, a 45deg riser, then flat lower
	assert.equal(geo.seam, "M0 23.55 L69 23.55 L85 16.75 L100 16.75");
	assert.deepEqual(geo.accentPoints, ["69.6,23.05 82.2,16.2 82.2,18.05 69.6,24.9"]);

	// The invented details of the old mock-up must be gone
	assert.equal(await page.locator(".v1-back-stripes").count(), 0);
	assert.equal(await page.locator(".v1-back-upper").count(), 0);
	assert.equal(await page.locator(".v1-back-orange-accent").count(), 0);
	assert.equal(await page.locator(".v1-back-torx").count(), 0);

	assert.equal((await page.locator("#btn-flip-device").textContent()).trim(), "Front View");
});

test("the TempoTec print is actually painted, upright, on the rear plate", async (t) => {
	const page = await open(t);
	await page.click("#btn-flip-device");
	await page.waitForTimeout(700);
	// Screenshot just the print and check its pale pixels are on the plate, not
	// clipped away by the shell's overflow - the bug that hid it before.
	const stats = await page.evaluate(() => {
		const svg = document.querySelector("#v1-chassis-back .v1-tempotec-svg");
		const shell = document.querySelector("#v1-chassis-back .v1-back-shell").getBoundingClientRect();
		const b = svg.getBoundingClientRect();
		const cs = getComputedStyle(svg);
		return {
			insideShell: b.left >= shell.left - 1 && b.right <= shell.right + 1 && b.top >= shell.top - 1 && b.bottom <= shell.bottom + 1,
			transform: cs.transform,
			width: b.width,
			height: b.height,
			visible: cs.visibility !== "hidden" && cs.opacity !== "0",
		};
	});
	assert.equal(stats.visible, true);
	assert.equal(stats.insideShell, true, "the print stays inside the rear shell");
	// rotate(90deg) about the top-left corner: matrix(0, 1, -1, 0, w, 0)
	assert.match(stats.transform, /^matrix\(0,\s*1,\s*-1,\s*0,/);
	assert.ok(stats.width > 20 && stats.width < 90, `print is ~0.07 W thick, got ${stats.width.toFixed(1)} px`);
});

test("flipping back returns to the screen", async (t) => {
	const page = await open(t);
	await page.click("#btn-flip-device");
	await page.click("#v1-chassis-back");
	await page.waitForTimeout(700);
	assert.equal((await page.locator("#btn-flip-device").textContent()).trim(), "Back View");
});
