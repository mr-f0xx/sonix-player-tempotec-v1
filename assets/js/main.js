/* Sonix Player showcase — progressive enhancement only.
   The page works without JS; this adds live GitHub Releases data. */
(() => {
	"use strict";

	const REPO = "https://github.com/mr-f0xx/sonix-player-tempotec-v1";
	const API = "https://api.github.com/repos/mr-f0xx/sonix-player-tempotec-v1/releases/latest";
	const FALLBACK = `${REPO}/releases/latest`;

	const nodes = {
		downloads: document.querySelectorAll("[data-download]"),
		versions: document.querySelectorAll("[data-release-version]"),
		dates: document.querySelectorAll("[data-release-date]"),
		sizes: document.querySelectorAll("[data-release-size]"),
		assets: document.querySelectorAll("[data-release-asset]"),
		status: document.querySelectorAll("[data-release-status]"),
	};

	const setText = (list, value) => list.forEach((node) => { node.textContent = value; });
	const setStatus = (message, isError = false) => {
		nodes.status.forEach((node) => {
			node.textContent = message;
			node.classList.toggle("error", isError);
		});
	};

	const formatDate = (iso) => {
		const date = new Date(iso);
		if (Number.isNaN(date.getTime())) return "—";
		return new Intl.DateTimeFormat("en-GB", { day: "2-digit", month: "short", year: "numeric", timeZone: "UTC" }).format(date);
	};

	const formatSize = (bytes) => {
		if (!Number.isFinite(bytes) || bytes <= 0) return "—";
		return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
	};

	async function loadLatestRelease() {
		const controller = new AbortController();
		const timeout = setTimeout(() => controller.abort(), 9000);
		try {
			const response = await fetch(API, {
				headers: { Accept: "application/vnd.github+json" },
				cache: "no-store",
				signal: controller.signal,
			});
			if (!response.ok) throw new Error(`HTTP ${response.status}`);
			const release = await response.json();
			const upt = release.assets?.find((asset) => asset.name === "v1.upt")
				|| release.assets?.find((asset) => asset.name.toLowerCase().endsWith(".upt"));

			if (!upt) throw new Error("no .upt asset in the latest release");
			nodes.downloads.forEach((link) => {
				link.href = upt.browser_download_url;
				link.title = `${release.tag_name || "Latest"} · ${upt.name}`;
			});
			setText(nodes.versions, release.tag_name || "latest");
			setText(nodes.dates, formatDate(release.published_at || release.created_at));
			setText(nodes.sizes, formatSize(upt.size));
			setText(nodes.assets, upt.name);
			setStatus(`Latest release ${release.tag_name || ""} · ${formatSize(upt.size)} · link resolved from GitHub Releases`.replace(/\s+/g, " "));
		} catch (error) {
			// Keep the buttons usable: /releases/latest always serves the newest file.
			nodes.downloads.forEach((link) => { link.href = FALLBACK; });
			setStatus("Release information is temporarily unavailable — open All releases to download manually.", true);
			console.warn("[sonix] release lookup failed:", error);
		} finally {
			clearTimeout(timeout);
		}
	}

	loadLatestRelease();
})();
