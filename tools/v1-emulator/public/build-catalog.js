// Published builds only. Saved imports are manual choices, never candidates for
// "latest". Old/synthesized manifests without a default use the newest build time.
export function publishedBuilds(catalog) {
	const entries = (catalog.builds || []).filter((b) => b && (b.url || b.file)).map((b) => {
		const url = b.url || `./builds/${b.file}`;
		return {
			...b, url, source: "catalog", id: `cat:${b.id || b.file || url}`,
			name: b.name || b.file || url,
			shortCommit: b.shortCommit || b.commit?.slice(0, 8) || "",
		};
	});
	const defaultId = `cat:${catalog.defaultBuildId}`;
	return entries.sort((a, b) => Number(b.id === defaultId) - Number(a.id === defaultId)
		|| (Date.parse(b.builtAt) || 0) - (Date.parse(a.builtAt) || 0));
}
