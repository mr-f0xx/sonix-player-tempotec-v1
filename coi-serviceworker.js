/*!
 * coi-serviceworker.js — Cross-Origin Isolation Service Worker for GitHub Pages.
 * Adds Cross-Origin-Opener-Policy: same-origin,
 * Cross-Origin-Embedder-Policy: require-corp, and
 * Cross-Origin-Resource-Policy: cross-origin so Emscripten WebAssembly pthreads
 * (SharedArrayBuffer) work on static HTTPS hosts across desktop AND mobile
 * browsers (including iOS Safari / WebKit, which does not support credentialless).
 *
 * version: 2-require-corp
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
					headers.set("Cross-Origin-Embedder-Policy", "require-corp");
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
		const RELOAD_KEY = "sonix_coi_reloaded_v2";
		if (window.crossOriginIsolated) {
			try { sessionStorage.removeItem(RELOAD_KEY); } catch {}
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

		const hasReloaded = () => {
			try { return sessionStorage.getItem(RELOAD_KEY) === "1"; } catch { return false; }
		};
		const triggerReloadOnce = () => {
			if (window.crossOriginIsolated || hasReloaded()) return;
			try { sessionStorage.setItem(RELOAD_KEY, "1"); } catch {}
			window.location.reload();
		};

		navigator.serviceWorker.addEventListener("controllerchange", () => {
			triggerReloadOnce();
		});

		navigator.serviceWorker
			.register(new URL("coi-serviceworker.js", window.location.href).href)
			.then((reg) => {
				if (reg.active && !navigator.serviceWorker.controller) {
					triggerReloadOnce();
					return;
				}
				// If an older service worker (e.g. credentialless) was previously registered on iOS Safari,
				// request an update check right away so the new require-corp worker activates promptly.
				try { reg.update(); } catch {}
				reg.addEventListener("updatefound", () => {
					const sw = reg.installing;
					if (!sw) return;
					sw.addEventListener("statechange", () => {
						if (sw.state === "activated") {
							triggerReloadOnce();
						}
					});
				});
			})
			.catch((err) => {
				console.warn("[coi-sw] registration skipped:", err);
			});
	})();
}
