import { gunzipSync } from "node:zlib";
import { readFile } from "node:fs/promises";

const header = await readFile(new URL("./Rcc6WebUiAssets.h", import.meta.url), "utf8");
const appSource = await readFile(new URL("./src/app.js", import.meta.url), "utf8");
const body = header.match(/RCC6_WEB_UI_INDEX_GZ\[\] PROGMEM = \{([\s\S]*?)\n\};/);
if (!body) throw new Error("Generated gzip array is missing");
const bytes = Uint8Array.from([...body[1].matchAll(/0x([0-9a-f]{2})/g)], (match) => Number.parseInt(match[1], 16));
const html = gunzipSync(bytes).toString("utf8");

for (const required of [
  "Overview", "Messages", "What this radio can hear", "Radio", "This radio", "/api/frame", "/api/network",
  "X-RCC6-Session", "application/x-www-form-urlencoded", "Set up local Wi-Fi", "LAN WEB LOGIN",
  "meshcore", "localStorage", "rcc6-radio-owner", "rcc6-history-pending-", "syncNextMessage", "MeshCore.js",
  "NeonPocketMC", "brand-pocket",
  "NeonPocket Ultimate Companion", "LAST 24 HOURS", "Advertised places", "MEMORY HEADROOM",
  "SIGNAL SAMPLES", "Freshest nearby nodes", "Hop view",
  "/api/ultimate/status", "/api/ultimate/history", "/api/ultimate/export",
  "/api/ultimate/settings", "/api/ultimate/location", "/api/ultimate/ota",
  "SIGNED UPDATE", "NeonPocket settings", "BATTERY TREND", "ultimate-location-share",
  "LATEST DELIVERY", "Power profile", "Battery capacity", "Battery calibration offset",
]) {
  if (!html.includes(required)) throw new Error(`Built UI is missing ${required}`);
}
if (!header.includes("extern const size_t RCC6_WEB_UI_INDEX_GZ_LEN;")) throw new Error("Length declaration is missing");
if (/<script\s+[^>]*src=|<link\s+[^>]*rel=[\"']stylesheet/i.test(html)) throw new Error("External runtime asset detected");
if (html.includes("location.reload")) throw new Error("Reload-based transport recovery detected");
if (html.includes("navigator.locks")) throw new Error("Secure-context Web Locks dependency detected");
if (appSource.includes(".getWaitingMessages(")) throw new Error("Destructive batch message drain detected");
for (const required of ["this.drainTask = this.drainRetainedFrames()", "if (!this.ready) throw"]) {
  if (!appSource.includes(required)) throw new Error(`Takeover drain guard is missing ${required}`);
}
for (const required of ["loginUser", "loginKey", "device key, not your home Wi-Fi password"]) {
  if (!appSource.includes(required)) throw new Error(`LAN login handoff is missing ${required}`);
}
for (const required of [
  "async function refreshUltimate()", "function drawUltimateChart(",
  "function drawActivityBars(", "function drawMeshMap(", "function advertisedLocation(",
  'ultimateFetch("/api/ultimate/settings"', 'fetch("/api/ultimate/ota"',
  "navigator.geolocation.getCurrentPosition", "batteryTrendMvPerHour", "advertise }),",
  "batteryRuntimeMinutes", "batteryCapacityMah", "usbHostConnected", "ultimate.delivery",
  'split("\\0", 1)',
]) {
  if (!appSource.includes(required)) throw new Error(`Ultimate WebUI behavior is missing ${required}`);
}
for (const required of ["raw & 0xff", "packed === 0xff", "packed & 0x3f", "route unknown"]) {
  if (!appSource.includes(required)) throw new Error(`Mesh route decoding is missing ${required}`);
}
const decodeHopCount = (rawPath) => ((Number(rawPath) & 0xff) === 0xff ? null : Number(rawPath) & 0x3f);
for (const [rawPath, expected] of [[-1, null], [255, null], [0x80, 0], [0x81, 1], [0xc2, 2]]) {
  if (decodeHopCount(rawPath) !== expected) throw new Error(`Mesh route decode failed for ${rawPath}`);
}
console.log(`Verified ${bytes.length}-byte NeonPocket Ultimate WebUI gzip asset`);
