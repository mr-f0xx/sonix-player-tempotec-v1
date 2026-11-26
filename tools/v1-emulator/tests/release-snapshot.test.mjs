/**
 * The showcase page must never ship a version it does not know about.
 *
 * 1.7.7 was published, but the page kept announcing 1.7.6 and kept offering
 * 1.7.6's files: the tag's Pages deploy was rejected by the github-pages
 * environment policy, the last successful deploy from main predated the
 * release, and `index.html` pinned the themes ZIP to a literal tag. The live
 * API refresh normally covers for a stale snapshot, but it is rate-limited per
 * visitor, so the fallback is what some people actually get.
 *
 * These checks are static on purpose: no browser, no network, no firmware.
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";

const ROOT = new URL("../../../", import.meta.url).pathname;
const read = (file) => fs.readFileSync(path.join(ROOT, file), "utf8");

const snapshot = JSON.parse(read("assets/release.json"));
const page = read("index.html");

test("the committed release snapshot points at a tag, not an empty one", () => {
	assert.match(snapshot.tag_name, /^\d+\.\d+\.\d+$/, "release.json needs a real tag_name");
	assert.ok(snapshot.published_at, "release.json needs a published_at");
	assert.ok(
		!Number.isNaN(new Date(snapshot.published_at).valueOf()),
		"release.json needs a ISO published_at",
	);
});

test("every download URL in the snapshot matches the tag it announces", () => {
	const urls = (snapshot.assets || []).map((asset) => asset.browser_download_url);
	assert.ok(urls.length > 0, "release.json needs assets");
	for (const url of urls) {
		assert.ok(
			url.includes(`/releases/download/${snapshot.tag_name}/`),
			`${url} does not point at tag ${snapshot.tag_name}`,
		);
	}
});

test("the snapshot carries the firmware asset the page downloads", () => {
	const names = (snapshot.assets || []).map((asset) => (asset.name || "").toLowerCase());
	assert.ok(names.includes("v1.upt"), "release.json needs a v1.upt asset");
});

test("the page links to /releases/latest, never to a pinned tag", () => {
	const tags = page.match(/releases\/download\/\d+\.\d+\.\d+\//g) || [];
	assert.deepEqual(
		tags,
		[],
		"index.html must not pin a release tag; use releases/latest/download/<asset> so the static HTML can never go stale",
	);
	assert.match(
		page,
		/releases\/latest\/download\/v1\.upt/,
		"the firmware button needs a working no-JS fallback",
	);
	assert.match(
		page,
		/releases\/latest\/download\/Icon\.sets\.Themes\.zip/,
		"the themes button needs a working no-JS fallback",
	);
});

test("the page still lets app.js refine the links from the live release", () => {
	assert.match(page, /id="download-button"/);
	assert.match(page, /id="theme-button"/);
	assert.match(page, /id="release-version"/);
	const app = read("assets/app.js");
	assert.match(app, /releases\/latest/, "app.js keeps a last-resort fallback");
});
