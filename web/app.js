"use strict";
const $ = (id) => document.getElementById(id);
let csrf = "", signedIn = false, updating = false, cancelled = false, logPaused = false;
let lastStatus = null, release = null, pollRunning = false;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const bytes = (n) => `${(n / 1024).toFixed(1)} KiB`;
const text = (id, value) => { $(id).textContent = value; };
function notice(message, error = false) {
  text("notice", message); $("notice").hidden = false; $("notice").classList.toggle("error", error);
}
async function api(path, { method = "GET", body, ticket, extra = {}, timeout = 20000 } = {}) {
  const headers = { ...extra };
  if (csrf) headers["X-CSRF-Token"] = csrf;
  if (ticket) headers["X-Update-Ticket"] = ticket;
  if (body && !(body instanceof ArrayBuffer) && !ArrayBuffer.isView(body)) {
    headers["Content-Type"] = "application/json"; body = JSON.stringify(body);
  }
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  try {
    const response = await fetch(`/api/v1${path}`, { method, body, headers, signal: controller.signal, cache: "no-store" });
    const value = await response.json();
    if (!response.ok) {
      const error = new Error(value.message || `Request failed (${response.status})`);
      error.status = response.status; throw error;
    }
    return value;
  } catch (error) {
    if (error.name === "AbortError") throw new Error(`The device did not respond within ${timeout / 1000} seconds. Check its Wi-Fi connection and retry.`);
    throw error;
  } finally { clearTimeout(timer); }
}
function loginView() {
  signedIn = false; csrf = "";
  $("login-panel").hidden = false; $("console").hidden = true; $("logout").hidden = true;
  text("connection", "Not signed in");
}
function consoleView() {
  signedIn = true; $("login-panel").hidden = true; $("console").hidden = false; $("logout").hidden = false;
}
function controls() {
  const busy = updating || Boolean(lastStatus?.ota?.busy);
  $("upload").disabled = busy || !lastStatus?.ota?.healthy || !$("image").files.length;
  $("rollback").disabled = busy || !lastStatus?.rollback_available;
  $("check-release").disabled = busy || !lastStatus?.network?.online || Boolean(release?.checking);
  $("install-release").disabled = busy || !lastStatus?.network?.online || !release?.verified || !release?.newer;
  $("image").disabled = busy;
  $("network-form").querySelectorAll("button,input").forEach((element) => { element.disabled = busy; });
}
function render(status) {
  lastStatus = status;
  text("connection", "Local connection active");
  text("device-name", status.device_id); text("board", status.board);
  const seconds = Math.floor(status.uptime_ms / 1000);
  text("uptime", `${Math.floor(seconds / 3600)}h ${Math.floor(seconds / 60) % 60}m uptime`);
  text("version", status.version); text("slot", status.slot); text("idf", status.idf);
  text("rollback-state", status.rollback_available ? "Available" : "No confirmed previous image");
  $("slot-details").replaceChildren(...(status.slots || []).map((slot) => {
    const row = document.createElement("tr");
    for (const value of [slot.slot + (slot.running ? " •" : ""), slot.version, slot.state]) {
      const cell = document.createElement("td"); cell.textContent = value; row.append(cell);
    }
    return row;
  }));
  text("ota-phase", status.ota.phase);
  text("health-label", status.ota.trial ? "Trial boot — not confirmed" : !status.settings_healthy ? "Storage needs recovery" : status.ota.healthy ? "Services operational" : "Checking startup health");
  text("reset-reason", `Reset reason ${status.reset_reason}`);
  text("heap", bytes(status.free_heap)); text("minimum-heap", bytes(status.minimum_heap)); text("largest-block", bytes(status.largest_block));
  text("ap-ssid", status.network.ap_ssid); text("station-ip", status.network.station_ip);
  text("station-detail", status.network.station_detail || (status.network.online ? "Connected" : "Optional uplink is offline"));
  text("network-badge", status.network.maintenance ? "Maintenance" : status.network.online ? "Hotspot + uplink" : "Hotspot only");
  if (document.activeElement !== $("ssid") && !$("ssid").value) $("ssid").value = status.network.station_ssid || "";
  controls();
}
async function poll() {
  if (!signedIn || updating || pollRunning) return;
  pollRunning = true;
  try {
    render(await api("/status"));
    release = await api("/releases");
    text("release-status", release.version ? `${release.version} · ${release.message}` : release.message || "Release service starting");
    if (!logPaused) {
      const logs = await api("/logs");
      const nearBottom = $("logs").scrollTop + $("logs").clientHeight >= $("logs").scrollHeight - 30;
      text("logs", logs.map((line) => `${(line.ms / 1000).toFixed(3).padStart(10)}  ${line.message}`).join("\n") || "No events yet.");
      if (nearBottom) $("logs").scrollTop = $("logs").scrollHeight;
    }
    controls();
  } catch (error) {
    text("connection", "Connection interrupted");
    if (error.status === 401) { loginView(); notice("Session ended. Sign in again to continue."); }
    else notice(`Cannot read the device. Stay on its hotspot and retry. ${error.message}`, true);
  } finally { pollRunning = false; }
}
$("login-form").addEventListener("submit", async (event) => {
  event.preventDefault(); const button = event.submitter; const label = button.textContent;
  button.disabled = true; button.textContent = "Checking credentials…";
  try {
    const session = await api("/session", { method: "POST", body: { password: $("password").value }, timeout: 45000 });
    csrf = session.csrf; $("password").value = ""; $("notice").hidden = true; consoleView(); await poll();
  } catch (error) { notice(error.message, true); } finally { button.disabled = false; button.textContent = label; }
});
$("logout").addEventListener("click", async () => {
  try { await api("/session", { method: "DELETE" }); } catch (error) { notice(error.message, true); }
  loginView();
});
$("theme").addEventListener("click", () => {
  const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = theme; localStorage.setItem("203-theme", theme);
  text("theme", theme === "dark" ? "Light theme" : "Dark theme");
});
const theme = localStorage.getItem("203-theme");
if (theme === "light") { document.documentElement.dataset.theme = "light"; text("theme", "Dark theme"); }
$("image").addEventListener("change", () => { text("file-label", $("image").files[0]?.name || "Choose firmware .bin"); controls(); });
function progress(stage, detail, percent = null) {
  $("progress-panel").hidden = false; text("progress-stage", stage); text("progress-detail", detail);
  if (percent === null) $("progress").removeAttribute("value"); else $("progress").value = percent;
}
async function confirmUpdate(ticket, expected, timeout = 160000) {
  sessionStorage.setItem("203-update", JSON.stringify({ ticket, expected, expires: Date.now() + timeout }));
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    await sleep(2000);
    let state;
    try { state = await api("/ota/status", { ticket, timeout: 5000 }); }
    catch { progress("Reconnecting", "Stay on the device hotspot. The previous image returns automatically if this trial cannot confirm."); continue; }
    if (state.phase === "failed" || state.phase === "rolled_back") throw new Error(state.detail);
    if (state.trial) {
      progress("Checking new firmware", "Management reconnected. Waiting for the 30-second health window.");
      try { await api("/ota/confirm", { method: "POST", ticket, timeout: 5000 }); }
      catch (error) { if (error.status) throw error; continue; }
    }
    if (state.phase === "confirmed" && state.running_version === expected) {
      sessionStorage.removeItem("203-update");
      progress("Confirmed healthy", `Version ${expected} is running and confirmed.`, 100);
      notice(`Installed ${expected}. Sign in again after the reboot.`); return;
    }
    if (!state.trial) progress(state.phase, state.detail);
  }
  throw new Error("The new firmware did not confirm within the allowed time. Reconnect and inspect the active version; rollback may have restored it.");
}
$("upload").addEventListener("click", async () => {
  const file = $("image").files[0]; if (!file || updating) return;
  updating = true; cancelled = false; controls(); $("cancel").hidden = false;
  try {
    if (file.size > 0x1E0000 || file.size < 4096 || file.size % 4096) throw new Error("Select a signed application image that fits the 1.875 MiB slot.");
    progress("Preparing", "Calculating image SHA-256 before transfer.");
    const data = await file.arrayBuffer();
    const view = new Uint8Array(data);
    if (view[0] !== 0xE9) throw new Error("This file is not an ESP application image.");
    const expected = new TextDecoder("ascii").decode(view.slice(48,80)).split("\0")[0];
    const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-256", data))].map((n) => n.toString(16).padStart(2,"0")).join("");
    const job = await api("/ota/start", { method: "POST", body: { size: file.size, sha256: hash } });
    for (let offset = 0; offset < file.size; offset += job.chunk_size) {
      if (cancelled) throw new Error("Transfer cancelled.");
      const end = Math.min(offset + job.chunk_size, file.size);
      await api("/ota/chunk", { method: "PUT", ticket: job.ticket, body: data.slice(offset,end), extra: { "Content-Type":"application/octet-stream", "X-Upload-Offset":String(offset) } });
      progress("Transferring", "Writing the inactive firmware slot. The running image remains available.", end / file.size * 100);
      text("progress-bytes", `${bytes(end)} / ${bytes(file.size)}`);
    }
    $("cancel").hidden = true;
    progress("Verifying", "The device is checking compatibility, the complete image hash, and its signing key.");
    sessionStorage.setItem("203-update", JSON.stringify({ ticket: job.ticket, expected, expires: Date.now()+160000 }));
    try { await api("/ota/finish", { method: "POST", ticket: job.ticket }); }
    catch (error) { if (error.status) throw error; }
    await confirmUpdate(job.ticket, expected);
    loginView();
  } catch (error) {
    try { await api("/ota/abort", { method: "POST", timeout: 3000 }); } catch {}
    notice(error.message, true); progress("Not confirmed", error.message, 0);
  } finally { updating = false; $("cancel").hidden = true; controls(); }
});
$("cancel").addEventListener("click", () => { cancelled = true; });
$("network-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await api("/network", { method: "PUT", body: { ssid: $("ssid").value, password: $("wifi-password").value, enabled: true } });
    $("wifi-password").value = ""; notice("Uplink saved. The hotspot remains available while the board connects."); await poll();
  } catch (error) { notice(error.message, true); }
});
$("disconnect").addEventListener("click", async () => {
  try { await api("/network", { method: "PUT", body: { ssid: "", password: "", enabled: false } }); $("ssid").value = ""; notice("Uplink disabled. Local hotspot management remains available."); await poll(); }
  catch (error) { notice(error.message, true); }
});
$("check-release").addEventListener("click", async () => {
  try { await api("/releases/check", { method: "POST" }); notice("Checking the signed GitHub release manifest…"); await sleep(1200); await poll(); }
  catch (error) { notice(error.message, true); }
});
$("install-release").addEventListener("click", async () => {
  if (!release?.verified || updating) return;
  updating = true; controls();
  try {
    const expected = release.version;
    const job = await api("/releases/install", { method: "POST" });
    sessionStorage.setItem("203-update", JSON.stringify({ ticket: job.ticket, expected, expires: Date.now()+480000 }));
    await confirmUpdate(job.ticket, expected, 480000); loginView();
  } catch (error) { notice(error.message, true); progress("Not confirmed", error.message); }
  finally { updating = false; controls(); }
});
$("rollback").addEventListener("click", async () => {
  if (!window.confirm("Restore the previous confirmed firmware and reboot?")) return;
  try { await api("/ota/rollback", { method: "POST" }); notice("Rollback requested. Reconnect to the hotspot and sign in after the reboot."); loginView(); }
  catch (error) { notice(error.message, true); }
});
$("pause-log").addEventListener("click", () => { logPaused = !logPaused; text("pause-log", logPaused ? "Resume" : "Pause"); $("pause-log").setAttribute("aria-pressed",String(logPaused)); });
$("export").addEventListener("click", async () => {
  try { const report = await api("/diagnostics"); const url = URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:"application/json"})); const link = document.createElement("a"); link.href=url; link.download="entrance-diagnostics.json"; link.click(); setTimeout(() => URL.revokeObjectURL(url),1000); }
  catch (error) { notice(error.message, true); }
});
setInterval(poll, 5000);
try {
  const saved = JSON.parse(sessionStorage.getItem("203-update") || "null");
  if (saved && saved.expires > Date.now()) {
    updating = true; consoleView(); controls();
    confirmUpdate(saved.ticket,saved.expected,Math.max(1000,saved.expires-Date.now()))
      .catch((error) => notice(error.message,true)).finally(() => { updating=false; loginView(); });
  }
} catch { sessionStorage.removeItem("203-update"); }
