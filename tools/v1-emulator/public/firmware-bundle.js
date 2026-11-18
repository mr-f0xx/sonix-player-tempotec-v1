/* -------------------------------------------------------------------------
 * Zero-dependency ZIP reader (Store + Deflate via DecompressionStream)
 * ------------------------------------------------------------------------- */
export const WEB_BUILD_HINT = "Use the sonix-v1-web-wasm artifact for the same commit, or run make -C sonix-player web-wasm-bundle. Device firmware (v1.upt) cannot run in a browser.";

// Inspect names before inflating anything: an Actions download is a ZIP around
// one web bundle; the much larger device artifact contains a MIPS image instead.
function zipDirectory(bytes) {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const decoder = new TextDecoder("utf-8");

	// Locate End of Central Directory (EOCD: 0x06054b50)
	let eocdOffset = -1;
	const minOffset = Math.max(0, bytes.length - 65557);
	for (let i = bytes.length - 22; i >= minOffset; i--) {
		if (view.getUint32(i, true) === 0x06054b50) {
			eocdOffset = i;
			break;
		}
	}
	if (eocdOffset < 0) {
		throw new Error(`Not a firmware ZIP. ${WEB_BUILD_HINT}`);
	}

	const totalEntries = view.getUint16(eocdOffset + 10, true);
	const cdOffset = view.getUint32(eocdOffset + 16, true);
	const files = new Map();

	let ptr = cdOffset;
	for (let idx = 0; idx < totalEntries; idx++) {
		if (view.getUint32(ptr, true) !== 0x02014b50) {
			throw new Error("Corrupt ZIP central directory entry");
		}
		const method = view.getUint16(ptr + 10, true);
		const compSize = view.getUint32(ptr + 20, true);
		const nameLen = view.getUint16(ptr + 28, true);
		const extraLen = view.getUint16(ptr + 30, true);
		const commentLen = view.getUint16(ptr + 32, true);
		const localHeaderOffset = view.getUint32(ptr + 42, true);
		const name = decoder.decode(bytes.subarray(ptr + 46, ptr + 46 + nameLen));
		ptr += 46 + nameLen + extraLen + commentLen;

		if (name.endsWith("/")) continue;

		if (view.getUint32(localHeaderOffset, true) !== 0x04034b50) {
			throw new Error(`Corrupt local header for ${name}`);
		}
		const lNameLen = view.getUint16(localHeaderOffset + 26, true);
		const lExtraLen = view.getUint16(localHeaderOffset + 28, true);
		const dataStart = localHeaderOffset + 30 + lNameLen + lExtraLen;
		const compData = bytes.subarray(dataStart, dataStart + compSize);

		if (dataStart + compSize > cdOffset) throw new Error(`Truncated ZIP entry: ${name}`);
		files.set(name.replace(/^\/+/, ""), async () => {
			if (method === 0) return compData;
			if (method !== 8) throw new Error(`Unsupported ZIP compression method ${method} for ${name}`);
			if (typeof DecompressionStream === "undefined") throw new Error("This browser cannot decompress firmware ZIPs. Try a current browser.");
			const stream = new Blob([compData]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
			return new Uint8Array(await new Response(stream).arrayBuffer());
		});
	}
	return files;
}

export async function unpackFirmware(arrayBuffer, onProgress) {
	let bytes = new Uint8Array(arrayBuffer);
	let files = zipDirectory(bytes);
	if (!files.has("manifest.json") && !files.has("sonix_player_wasm.js")) {
		const nested = [...files.keys()].filter((name) => name.toLowerCase().endsWith(".sonix-web.zip"));
		if (nested.length === 0) {
			// The device workflow includes the exact source revision in its report.
			const report = files.get("BUILD-REPORT.txt");
			const sha = report && /^Commit: ([a-f0-9]{40})$/im.exec(new TextDecoder().decode(await report()))?.[1];
			if (sha) throw new Error(`This is a device build. Download sonix-v1-web-wasm-${sha.slice(0, 8)} for the matching browser firmware.`);
			throw new Error(`Not a Sonix web build. ${WEB_BUILD_HINT}`);
		}
		if (nested.length !== 1) throw new Error("This archive contains multiple builds. Extract and choose one .sonix-web.zip.");
		bytes = await files.get(nested[0])();
		files = zipDirectory(bytes); // Exactly one wrapper, not recursive archive extraction.
	}

	const manifest = files.has("manifest.json")
		? JSON.parse(new TextDecoder().decode(await files.get("manifest.json")())) : {};
	if (manifest.format && manifest.format !== "sonix-web-v1") throw new Error(`Unsupported firmware bundle format: ${manifest.format}`);
	const jsName = manifest.entryJs || "sonix_player_wasm.js";
	const wasmName = manifest.wasm || "sonix_player_wasm.wasm";
	for (const name of [jsName, wasmName]) {
		if (!files.has(name)) throw new Error(`Bundle is missing ${name}. ${WEB_BUILD_HINT}`);
	}
	const assets = [...files.keys()].filter((name) => name.startsWith("usr/"));
	if (!assets.length) throw new Error("Bundle is missing TempoTec V1 runtime assets (usr/)");
	const jsBytes = await files.get(jsName)();
	const wasmBytes = await files.get(wasmName)();
	const usrFiles = new Map();
	for (const name of assets) {
		if (name.split("/").includes("..")) throw new Error(`Invalid asset path: ${name}`);
		usrFiles.set(name, await files.get(name)());
		if (usrFiles.size % 12 === 0 || usrFiles.size === assets.length) onProgress?.(usrFiles.size, assets.length);
	}
	const workerBytes = await files.get("sonix_player_wasm.worker.js")?.();
	return { manifest, jsBytes, wasmBytes, workerBytes, usrFiles };
}
