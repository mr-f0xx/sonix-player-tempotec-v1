/*!
 * coi-serviceworker.js — Cross-Origin Isolation Service Worker for GitHub Pages.
 * Adds Cross-Origin-Opener-Policy: same-origin and
 * Cross-Origin-Embedder-Policy: credentialless so Emscripten WebAssembly pthreads
 * (SharedArrayBuffer) work on static hosts that cannot configure custom headers.
 */
if (typeof window === "undefined") {
	self.addEventListener("install", () => self.skipWaiting());
	self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

	self.addEventListener("fetch", (event) => {
		const req = event.request;
		if (req.cache === "only-if-cached" && req.mode !== "same-origin") {
			return;
		}
		event.respondWith(
			fetch(req)
				.then((res) => {
					if (!res || res.status === 0) return res;
					// Only rewrite same-origin responses so opaque/CORS responses pass through cleanly.
					const url = new URL(req.url);
					if (url.origin !== self.location.origin) return res;
					const headers = new Headers(res.headers);
					headers.set("Cross-Origin-Embedder-Policy", "credentialless");
					headers.set("Cross-Origin-Opener-Policy", "same-origin");
					headers.set("Cross-Origin-Resource-Policy", "cross-origin");
					return new Response(res.body, {
						status: res.status,
						statusText: res.statusText,
						headers,
					});
				})
				.catch((err) => {
					console.error("[coi-sw] fetch failed:", err);
					throw err;
				})
		);
	});
} else {
	(() => {
		if (window.crossOriginIsolated) {
			try { sessionStorage.removeItem("sonix_coi_reloaded"); } catch {}
			return;
		}
		if (!window.isSecureContext || !("serviceWorker" in navigator)) {
			return;
		}
		// Only auto-reload top-level tabs (e.g. GitHub Pages); avoid reload loops inside cross-origin preview iframes.
		let isTopLevel = true;
		try {
			isTopLevel = window.top === window.self;
		} catch {
			isTopLevel = false;
		}
		if (!isTopLevel) return;
		const alreadyReloaded = (() => {
			try { return sessionStorage.getItem("sonix_coi_reloaded") === "1"; } catch { return false; }
		})();

		navigator.serviceWorker
			.register(new URL("coi-serviceworker.js", window.location.href).href)
			.then((reg) => {
				if (reg.active && !navigator.serviceWorker.controller && !alreadyReloaded) {
					try { sessionStorage.setItem("sonix_coi_reloaded", "1"); } catch {}
					window.location.reload();
					return;
				}
				reg.addEventListener("updatefound", () => {
					const sw = reg.installing;
					if (!sw) return;
					sw.addEventListener("statechange", () => {
						if (sw.state === "activated" && !window.crossOriginIsolated && !alreadyReloaded) {
							try { sessionStorage.setItem("sonix_coi_reloaded", "1"); } catch {}
							window.location.reload();
						}
					});
				});
			})
			.catch((err) => {
				console.warn("[coi-sw] registration skipped:", err);
			});
	})();
}
