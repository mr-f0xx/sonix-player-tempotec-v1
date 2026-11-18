import fs from "node:fs";
import path from "node:path";

// A staged Pages snapshot and a newer local web-wasm-bundle can have the
// *same filename* (uncommitted edits). Resolve the catalog and downloads alike.
export function newestFile(dirs, name) {
	let newest;
	for (const dir of dirs) {
		const file = path.join(dir, name);
		const stat = fs.statSync(file, { throwIfNoEntry: false });
		if (stat?.isFile() && (!newest || stat.mtimeMs > newest.stat.mtimeMs)) newest = { file, stat };
	}
	return newest;
}

export function localCatalog(distDir) {
	const dirs = [distDir, path.join(distDir, "pages", "builds")];
	const manifest = newestFile(dirs, "manifest.json");
	const catalog = manifest ? JSON.parse(fs.readFileSync(manifest.file, "utf8")) : { builds: [] };
	const builds = [...(catalog.builds || [])];
	const publishedTime = manifest?.stat.mtimeMs ?? -Infinity;
	let latest = null;
	for (const file of fs.existsSync(distDir) ? fs.readdirSync(distDir) : []) {
		if (!file.endsWith(".sonix-web.zip")) continue;
		const winner = newestFile(dirs, file);
		if (!winner) continue;
		const { stat } = winner;
		const url = `builds/${encodeURIComponent(file)}`;
		const known = builds.find((b) => b.file === file || b.url?.replace(/^\.\//, "") === url);
		if (known && stat.mtimeMs <= publishedTime) continue;
		const slug = file.slice(0, -".sonix-web.zip".length);
		const entry = known || { id: slug, name: slug, ref: "local", shortCommit: slug.slice(-8), file, url };
		entry.builtAt = stat.mtime.toISOString();
		entry.size = stat.size;
		if (!known) builds.push(entry);
		if (stat.mtimeMs > publishedTime && (!latest || stat.mtimeMs > latest.time)) {
			latest = { time: stat.mtimeMs, id: entry.id };
		}
	}
	builds.sort((a, b) => (Date.parse(b.builtAt) || 0) - (Date.parse(a.builtAt) || 0));
	return { format: "sonix-web-catalog-v1", ...catalog, defaultBuildId: latest?.id || catalog.defaultBuildId || builds[0]?.id, builds };
}
