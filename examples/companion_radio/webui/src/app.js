import Connection from "@liamcottle/meshcore.js/src/connection/connection.js";
import Constants from "@liamcottle/meshcore.js/src/constants.js";

const MAX_FRAME_BYTES = 176;
const MAX_MESSAGE_BYTES = 140;
const MAX_HISTORY_MESSAGES = 120;
const POLL_DELAY_MS = 90;
const COMMAND_TIMEOUT_MS = 30000;
const MAX_RECOVERY_ATTEMPTS = 2;
const RADIO_LEASE_KEY = "rcc6-radio-owner";
const RADIO_LEASE_MS = 7000;
const RADIO_HEARTBEAT_MS = 2000;
const $ = (id) => document.getElementById(id);
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const encoder = new TextEncoder();

function createSessionId() {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return [...bytes].map((value) => value.toString(16).padStart(2, "0")).join("");
}

const TAB_OWNER_ID = createSessionId();

function loadSessionId() {
  try {
    const saved = localStorage.getItem("rcc6-session");
    if (/^[0-9a-f]{32}$/.test(saved || "")) return saved;
    const created = createSessionId();
    localStorage.setItem("rcc6-session", created);
    return created;
  } catch {
    return createSessionId();
  }
}

class HttpPollingConnection extends Connection {
  constructor() {
    super();
    this.running = false;
    this.failures = 0;
    this.interrupted = false;
    this.sessionId = loadSessionId();
    this.lastDispatchedSequence = 0;
    this.pendingAck = 0;
    this.ready = false;
    this.connecting = false;
    this.drainTask = null;
    this.leaseTimer = null;
    this.storageListening = false;
    this.onStorage = (event) => {
      if (event.key === RADIO_LEASE_KEY && this.running && !this.ownsLease()) this.handleLeaseLoss();
    };
  }

  async connect() {
    if (this.running || this.connecting) return;
    this.connecting = true;
    try {
      if (!this.storageListening) {
        window.addEventListener("storage", this.onStorage);
        this.storageListening = true;
      }
      if (!await this.claimLease()) {
        this.emit("transport-busy");
        return;
      }
      this.running = true;
      this.ready = false;
      this.abortController = new AbortController();
      this.drainTask = this.drainRetainedFrames();
      const drained = await this.drainTask;
      if (!drained || !this.running || !this.ownsLease()) return;
      this.ready = true;
      this.pollTask = this.poll();
      this.emit("connected");
    } finally {
      this.connecting = false;
    }
  }

  async close() {
    const wasReady = this.ready;
    this.running = false;
    this.ready = false;
    this.abortController?.abort();
    try { await this.drainTask; } catch { /* aborted */ }
    try { await this.pollTask; } catch { /* aborted */ }
    if (wasReady) this.onDisconnected();
    this.releaseLease();
    if (this.storageListening) window.removeEventListener("storage", this.onStorage);
    this.storageListening = false;
  }

  readLease() {
    try {
      const lease = JSON.parse(localStorage.getItem(RADIO_LEASE_KEY) || "null");
      return lease && typeof lease.owner === "string" && Number.isFinite(lease.expires) ? lease : null;
    } catch { return null; }
  }

  ownsLease() {
    const lease = this.readLease();
    return lease?.owner === TAB_OWNER_ID && lease.expires > Date.now();
  }

  writeLease() {
    try {
      localStorage.setItem(RADIO_LEASE_KEY, JSON.stringify({ owner: TAB_OWNER_ID, expires: Date.now() + RADIO_LEASE_MS }));
      return this.ownsLease();
    } catch { return false; }
  }

  async claimLease() {
    const current = this.readLease();
    if (current && current.owner !== TAB_OWNER_ID && current.expires > Date.now()) return false;
    if (!this.writeLease()) return false;
    await delay(80);
    if (!this.ownsLease()) return false;
    this.stopHeartbeat();
    this.leaseTimer = setInterval(() => {
      if (!this.ownsLease() || !this.writeLease()) this.handleLeaseLoss();
    }, RADIO_HEARTBEAT_MS);
    return true;
  }

  stopHeartbeat() {
    clearInterval(this.leaseTimer);
    this.leaseTimer = null;
  }

  handleLeaseLoss() {
    this.stopHeartbeat();
    if (!this.running) return;
    this.running = false;
    this.ready = false;
    this.abortController?.abort();
    this.emit("transport-busy");
  }

  releaseLease() {
    this.stopHeartbeat();
    try {
      if (this.readLease()?.owner === TAB_OWNER_ID) localStorage.removeItem(RADIO_LEASE_KEY);
    } catch { /* storage unavailable */ }
  }

  leavePage() {
    this.running = false;
    this.ready = false;
    this.abortController?.abort();
    this.releaseLease();
  }

  async sendToRadioFrame(data) {
    const frame = data instanceof Uint8Array ? data : new Uint8Array(data);
    if (!frame.length || frame.length > MAX_FRAME_BYTES) throw new Error("Invalid companion frame size");
    if (!this.ready) throw new Error("Companion link is still draining");
    if (!this.ownsLease()) {
      this.handleLeaseLoss();
      throw new Error("Another tab owns the companion link");
    }
    try {
      const response = await fetch("/api/frame", {
        method: "POST",
        headers: {
          "Content-Type": "application/octet-stream",
          "X-RCC6-Session": this.sessionId,
        },
        body: frame,
        cache: "no-store",
        credentials: "same-origin",
        signal: this.abortController.signal,
      });
      if (!response.ok) throw new Error(`Frame POST failed (${response.status})`);
      this.markHealthy();
    } catch (error) {
      if (error.name !== "AbortError") {
        error.rcc6TransportFailure = true;
        this.markFailure(error);
      }
      throw error;
    }
  }

  async fetchFrame() {
    const headers = { Accept: "application/octet-stream", "X-RCC6-Session": this.sessionId };
    if (this.pendingAck) headers["X-RCC6-Ack"] = String(this.pendingAck);
    const response = await fetch("/api/frame", { headers, cache: "no-store", credentials: "same-origin", signal: this.abortController.signal });
    if (response.status === 204) {
      this.pendingAck = 0;
      this.markHealthy();
      return false;
    }
    if (!response.ok) throw new Error(`Frame poll failed (${response.status})`);
    const sequence = Number(response.headers.get("X-RCC6-Seq"));
    if (!Number.isInteger(sequence) || sequence < 1 || sequence > 0xffffffff) throw new Error("Invalid frame sequence");
    const frame = new Uint8Array(await response.arrayBuffer());
    if (!frame.length || frame.length > MAX_FRAME_BYTES) throw new Error("Invalid radio frame");
    if (sequence !== this.lastDispatchedSequence) {
      this.onFrameReceived(frame);
      this.lastDispatchedSequence = sequence;
      // MeshCore.js defers `once` handlers twice; let message persistence finish before ACK.
      await delay(0);
      await delay(0);
    }
    this.pendingAck = sequence;
    this.markHealthy();
    return true;
  }

  async drainRetainedFrames() {
    while (this.running) {
      if (!this.ownsLease()) { this.handleLeaseLoss(); return false; }
      try {
        if (!await this.fetchFrame()) return true;
      } catch (error) {
        if (!this.running || error.name === "AbortError") return false;
        this.markFailure(error);
        await delay(Math.min(250 * 2 ** Math.min(this.failures, 3), 2000));
      }
    }
    return false;
  }

  async poll() {
    while (this.running) {
      if (!this.ownsLease()) { this.handleLeaseLoss(); break; }
      try {
        if (!await this.fetchFrame()) await delay(POLL_DELAY_MS);
      } catch (error) {
        if (!this.running || error.name === "AbortError") break;
        this.markFailure(error);
        await delay(Math.min(250 * 2 ** Math.min(this.failures, 3), 2000));
      }
    }
  }

  markFailure(error) {
    this.failures += 1;
    if (this.failures >= 3 && !this.interrupted) {
      this.interrupted = true;
      this.emit("transport-state", "reconnecting", error);
    }
  }

  markHealthy() {
    const restored = this.interrupted;
    this.failures = 0;
    this.interrupted = false;
    if (restored && this.ready) this.emit("transport-restored");
  }
}

const state = {
  connected: false,
  linkStatus: "reconnecting",
  startedAt: Date.now(),
  view: "home",
  self: null,
  device: null,
  battery: 0,
  contacts: [],
  channels: [],
  messages: [],
  nearby: [],
  selected: "",
  radio: null,
  packets: null,
  rfSamples: [],
  network: null,
  ultimate: null,
  ultimateHistory: [],
  ultimateSettings: null,
  historyNode: "",
  mapMode: "places",
};

let connection = new HttpPollingConnection();
let commandTail = Promise.resolve();
let syncing = false;
let recovering = false;
let recoveryAttempts = 0;
let unannouncedMessages = 0;

function exclusive(task) {
  const result = commandTail.then(() => {
    let timer;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => {
        const error = new Error("Companion command timed out");
        error.rcc6CommandTimeout = true;
        reject(error);
      }, COMMAND_TIMEOUT_MS);
    });
    return Promise.race([Promise.resolve().then(task), timeout]).finally(() => clearTimeout(timer));
  });
  commandTail = result.catch(() => {});
  result.catch((error) => {
    if (error?.rcc6CommandTimeout || error?.rcc6TransportFailure) scheduleConnectionRecovery();
  });
  return result;
}

function scheduleConnectionRecovery() {
  if (recovering || recoveryAttempts >= MAX_RECOVERY_ATTEMPTS) {
    if (recoveryAttempts >= MAX_RECOVERY_ATTEMPTS) toast("Radio sync is paused. Tap the link status to retry.", "error");
    return;
  }
  recovering = true;
  recoveryAttempts += 1;
  setLink("reconnecting");
  toast(`Recovering companion link (${recoveryAttempts}/${MAX_RECOVERY_ATTEMPTS})`, "warn");
  setTimeout(async () => {
    const previous = connection;
    try {
      await previous.close();
      syncing = false;
      $("refresh-button").disabled = false;
      connection = new HttpPollingConnection();
      bindConnectionEvents(connection);
      await connection.connect();
    } catch {
      setLink("offline");
      toast("Companion recovery failed", "error");
    } finally {
      recovering = false;
    }
  }, 0);
}

function text(id, value) { $(id).textContent = value ?? "—"; }
function hex(bytes, length = bytes?.length ?? 0) { return bytes ? [...bytes.slice(0, length)].map((value) => value.toString(16).padStart(2, "0")).join("") : ""; }
function hashText(value) {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) hash = Math.imul(hash ^ value.charCodeAt(index), 16777619);
  return (hash >>> 0).toString(16);
}
function historyKey() { return state.self?.publicKey ? `rcc6-history-${hex(state.self.publicKey)}` : ""; }
function pendingHistoryKey() { return `rcc6-history-pending-${connection.sessionId}`; }
function messageId(kind, key, timestamp, value) { return `${kind}:${key}:${timestamp}:${hashText(value)}`; }
function persistHistory() {
  const key = historyKey() || pendingHistoryKey();
  try { localStorage.setItem(key, JSON.stringify(state.messages.slice(-MAX_HISTORY_MESSAGES))); } catch { /* storage is optional */ }
}
function readHistory(key) {
  try {
    const saved = JSON.parse(localStorage.getItem(key) || "[]");
    if (!Array.isArray(saved)) return [];
    return saved.filter((item) => item && typeof item.id === "string" && typeof item.key === "string" && typeof item.text === "string")
      .slice(-MAX_HISTORY_MESSAGES)
      .map((item) => ({
        id: item.id.slice(0, 180), key: item.key.slice(0, 100), text: item.text.slice(0, MAX_MESSAGE_BYTES * 2),
        timestamp: Number.isFinite(item.timestamp) ? item.timestamp : 0,
        direction: item.direction === "out" ? "out" : "in",
        status: typeof item.status === "string" ? item.status.slice(0, 16) : "received",
        unread: Boolean(item.unread),
        snr: Number.isFinite(item.snr) ? item.snr : undefined,
        expectedAckCrc: Number.isInteger(item.expectedAckCrc) ? item.expectedAckCrc : undefined,
      }));
  } catch { return []; }
}
function loadHistoryForNode() {
  const key = historyKey();
  if (!key || state.historyNode === key) return;
  state.historyNode = key;
  const merged = [...readHistory(key), ...readHistory(pendingHistoryKey()), ...state.messages];
  const seen = new Set();
  state.messages = merged.filter((item) => !seen.has(item.id) && seen.add(item.id)).slice(-MAX_HISTORY_MESSAGES);
  try { localStorage.removeItem(pendingHistoryKey()); } catch { /* storage is optional */ }
  persistHistory();
}
function channelKey(channel) { return `ch:${channel.channelIdx}`; }
function contactKey(contact) { return `dm:${hex(contact.publicKey)}`; }
function initials(name = "?") { return name.trim().split(/\s+/).slice(0, 2).map((part) => part[0]).join("").toUpperCase() || "?"; }
function formatNumber(value) { return Number.isFinite(value) ? value.toLocaleString() : "—"; }
function formatClock(epochSecs) { return epochSecs ? new Date(epochSecs * 1000).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" }) : "now"; }
function formatAge(epochSecs) {
  if (!epochSecs) return "just now";
  const seconds = Math.max(0, Math.floor(Date.now() / 1000) - epochSecs);
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
  return `${Math.floor(seconds / 86400)}d ago`;
}
function formatSession() {
  const minutes = Math.floor((Date.now() - state.startedAt) / 60000);
  return minutes < 1 ? "Now" : minutes < 60 ? `${minutes}m` : `${Math.floor(minutes / 60)}h ${minutes % 60}m`;
}
function radioFrequency(value) { return Number.isFinite(value) ? (value / 1e6).toFixed(3) : "—"; }
function radioBandwidth(value) { return Number.isFinite(value) ? (value / 1000).toFixed(value % 1000 ? 1 : 0) : "—"; }
function signalQuality(rssi) { return rssi >= -80 ? "Excellent" : rssi >= -95 ? "Good" : rssi >= -110 ? "Fair" : "Weak"; }

function advertisedLocation(node) {
  let lat = Number(node?.advLat);
  let lon = Number(node?.advLon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
  if (Math.abs(lat) > 90 || Math.abs(lon) > 180) { lat /= 1000000; lon /= 1000000; }
  if ((lat === 0 && lon === 0) || Math.abs(lat) > 90 || Math.abs(lon) > 180) return null;
  return { lat, lon };
}

function canvasContext(canvas, minimumHeight = 120) {
  if (!canvas) return null;
  const width = Math.max(280, canvas.clientWidth || 0);
  const height = Math.max(minimumHeight, canvas.clientHeight || 0);
  const ratio = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.scale(ratio, ratio);
  context.clearRect(0, 0, width, height);
  return { context, width, height };
}

function drawActivityBars(canvas, rows, limit = 24) {
  const surface = canvasContext(canvas, 110);
  if (!surface) return 0;
  const { context, width, height } = surface;
  const ordered = [...(rows || [])].reverse().slice(-limit);
  context.strokeStyle = "rgba(141,152,167,.11)";
  context.lineWidth = 1;
  for (let row = 1; row < 4; row += 1) {
    const y = Math.round(row * (height - 24) / 4) + .5;
    context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
  }
  if (!ordered.length) return 0;
  const values = ordered.map((row) => ({ rx: Number(row[1]) || 0, tx: Number(row[2]) || 0, fail: Number(row[3]) || 0 }));
  const maximum = Math.max(1, ...values.flatMap((row) => [row.rx, row.tx, row.fail]));
  const group = width / values.length;
  const barWidth = Math.max(1.5, Math.min(8, group * .22));
  values.forEach((row, index) => {
    const x = index * group + (group - barWidth * 3) / 2;
    [[row.rx, "#44f08a"], [row.tx, "#42a5ff"], [row.fail, "#ff5468"]].forEach(([value, color], series) => {
      const barHeight = Math.max(value ? 2 : 0, value / maximum * (height - 26));
      context.fillStyle = color;
      context.globalAlpha = .84;
      context.fillRect(x + series * barWidth, height - 12 - barHeight, Math.max(1, barWidth - 1), barHeight);
    });
  });
  context.globalAlpha = 1;
  return values.reduce((total, row) => total + row.rx + row.tx, 0);
}

function element(tag, className, content) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (content != null) node.textContent = content;
  return node;
}

function toast(message, kind = "ok") {
  const item = element("div", `toast ${kind}`);
  item.append(element("i"), element("span", "", message));
  $("toasts").append(item);
  setTimeout(() => item.classList.add("out"), 3200);
  setTimeout(() => item.remove(), 3500);
}

function setLink(status) {
  const online = status === "connected";
  state.connected = online;
  state.linkStatus = status;
  const label = online ? "Connected" : status === "reconnecting" ? "Reconnecting" : status === "busy" ? "Another tab active" : "Offline";
  text("link-label", label);
  text("side-status", label);
  text("hero-link", label);
  $("connect-button").className = `link-chip ${status}`;
  $("side-dot").className = `status-dot ${status}`;
}

function targetInfo(key) {
  if (key.startsWith("ch:")) {
    const channel = state.channels.find((item) => channelKey(item) === key);
    return channel ? { key, name: `#${channel.name || `Channel ${channel.channelIdx}`}`, subtitle: "Mesh channel", avatar: "#", channel } : null;
  }
  const contact = state.contacts.find((item) => contactKey(item) === key);
  return contact ? { key, name: contact.advName || "Unnamed contact", subtitle: `Direct · ${hex(contact.publicKey, 4)}`, avatar: initials(contact.advName), contact } : null;
}

function targetForContactPrefix(prefix) {
  const contact = state.contacts.find((item) => prefix.every((byte, index) => item.publicKey[index] === byte));
  return contact ? contactKey(contact) : `dm-prefix:${hex(prefix)}`;
}

function normalizeTargets() {
  if (state.selected && targetInfo(state.selected)) return;
  state.selected = state.channels[0] ? channelKey(state.channels[0]) : state.contacts[0] ? contactKey(state.contacts[0]) : "";
}

function addMessage(message) {
  const item = { id: `${Date.now()}-${Math.random()}`, ...message };
  if (state.messages.some((existing) => existing.id === item.id)) return null;
  state.messages.push(item);
  if (state.messages.length > MAX_HISTORY_MESSAGES) state.messages.splice(0, state.messages.length - MAX_HISTORY_MESSAGES);
  persistHistory();
  renderMessages();
  renderHome();
  return item;
}

function ingestWaiting(item) {
  if (item.contactMessage) {
    const message = item.contactMessage;
    const key = targetForContactPrefix(message.pubKeyPrefix);
    return addMessage({ id: messageId("dm", hex(message.pubKeyPrefix), message.senderTimestamp, message.text), key, text: message.text, timestamp: message.senderTimestamp, direction: "in", status: "received", unread: document.hidden || state.view !== "messages" || state.selected !== key, snr: message.snr }) ? 1 : 0;
  }
  if (item.channelMessage) {
    const message = item.channelMessage;
    const key = `ch:${message.channelIdx}`;
    return addMessage({ id: messageId("channel", key, message.senderTimestamp, message.text), key, text: message.text, timestamp: message.senderTimestamp, direction: "in", status: "received", unread: document.hidden || state.view !== "messages" || state.selected !== key, snr: message.snr }) ? 1 : 0;
  }
  return 0;
}

function refreshNearbyFromContacts() {
  state.nearby = [...state.contacts].filter((item) => item.lastAdvert).sort((a, b) => b.lastAdvert - a.lastAdvert).slice(0, 24);
}

async function refreshContacts() {
  const contacts = await connection.getContacts();
  state.contacts = contacts.sort((a, b) => (b.lastAdvert || 0) - (a.lastAdvert || 0));
  let historyChanged = false;
  for (const message of state.messages) {
    if (!message.key.startsWith("dm-prefix:")) continue;
    const prefix = message.key.slice("dm-prefix:".length);
    const contact = state.contacts.find((item) => hex(item.publicKey).startsWith(prefix));
    if (contact) { message.key = contactKey(contact); historyChanged = true; }
  }
  if (historyChanged) persistHistory();
  refreshNearbyFromContacts();
  normalizeTargets();
  renderAll();
}

async function syncMessages() {
  let added = 0;
  while (true) {
    const waiting = await connection.syncNextMessage();
    if (!waiting) break;
    added += ingestWaiting(waiting);
  }
  added += unannouncedMessages;
  unannouncedMessages = 0;
  if (added) toast(`${added} new message${added === 1 ? "" : "s"}`, "warn");
}

async function refreshStats() {
  if (!state.connected) return;
  try {
    const battery = await connection.getBatteryVoltage();
    state.battery = battery.batteryMilliVolts;
  } catch { /* optional */ }
  try {
    const radio = await connection.getStatsRadio();
    state.radio = radio.data;
    if (Number.isFinite(state.radio.lastRssi)) state.rfSamples.push(state.radio.lastRssi);
    state.rfSamples = state.rfSamples.slice(-36);
  } catch { /* optional */ }
  try { state.packets = (await connection.getStatsPackets()).data; } catch { /* optional */ }
  renderAll();
}

async function refreshNetworkStatus() {
  try {
    const response = await fetch("/api/network", {
      headers: { Accept: "application/json", "X-RCC6-Session": connection.sessionId },
      cache: "no-store",
      credentials: "same-origin",
    });
    if (!response.ok) throw new Error(`Network status failed (${response.status})`);
    const data = await response.json();
    if (data.mode !== "ap" && data.mode !== "station") throw new Error("Invalid network mode");
    state.network = {
      mode: data.mode,
      ssid: typeof data.ssid === "string" ? data.ssid : "",
      ip: typeof data.ip === "string" ? data.ip : "",
      fallback: Boolean(data.fallback),
    };
  } catch { /* network configuration endpoint is optional on older builds */ }
  renderNetwork();
}

async function ultimateFetch(path, options = {}) {
  const response = await fetch(path, {
    cache: "no-store",
    credentials: "same-origin",
    ...options,
    headers: { Accept: "application/json", ...(options.headers || {}) },
  });
  if (!response.ok) throw new Error(`Ultimate API failed (${response.status})`);
  return response;
}

async function refreshUltimate() {
  try {
    const [status, history, settings] = await Promise.all([
      ultimateFetch("/api/ultimate/status").then((response) => response.json()),
      ultimateFetch("/api/ultimate/history?limit=20").then((response) => response.json()),
      ultimateFetch("/api/ultimate/settings").then((response) => response.json()),
    ]);
    state.ultimate = status;
    state.ultimateHistory = Array.isArray(history.records) ? history.records : [];
    state.ultimateSettings = settings;
  } catch { /* non-Ultimate firmware remains compatible with the standard WebUI */ }
  renderUltimate();
}

async function setNetwork(fields) {
  if (!connection.ready || !connection.ownsLease()) throw new Error("RCC6 companion link is not ready");
  const response = await fetch("/api/network", {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded",
      "X-RCC6-Session": connection.sessionId,
    },
    body: new URLSearchParams(fields),
    cache: "no-store",
    credentials: "same-origin",
  });
  if (!response.ok) throw new Error(`Network update failed (${response.status})`);
}

async function syncAll() {
  if (syncing) return;
  syncing = true;
  $("refresh-button").disabled = true;
  try {
    await refreshNetworkStatus();
    try { state.device = await connection.deviceQuery(Constants.SupportedCompanionProtocolVersion); } catch { /* older firmware */ }
    state.self = await connection.getSelfInfo(6000);
    loadHistoryForNode();
    await refreshContacts();
    try { state.channels = (await connection.getChannels()).filter((item) => item.name); } catch { state.channels = []; }
    normalizeTargets();
    await syncMessages();
    await refreshStats();
    await refreshUltimate();
    renderAll();
  } finally {
    syncing = false;
    $("refresh-button").disabled = false;
  }
}

function renderHome() {
  const self = state.self;
  text("node-name", self?.name || "RADIO");
  text("node-id", self?.publicKey ? `ID ${hex(self.publicKey, 8)}…` : "Waiting for companion identity…");
  text("hero-contacts", state.contacts.length || "—");
  text("hero-uptime", formatSession());
  const unread = state.messages.filter((item) => item.unread);
  text("home-unread", unread.length);
  text("nav-unread", unread.length);
  $("nav-unread").hidden = !unread.length;
  const latest = unread.at(-1);
  const preview = $("message-preview");
  preview.replaceChildren();
  if (latest) {
    const target = targetInfo(latest.key);
    preview.className = "preview";
    preview.append(element("strong", "", target?.name || "New message"), element("span", "", latest.text));
  } else {
    preview.className = "preview empty";
    preview.textContent = "No unread messages";
  }
  const nearby = state.nearby[0];
  const nearbyPreview = $("nearby-preview");
  nearbyPreview.replaceChildren();
  if (nearby) {
    nearbyPreview.className = "preview";
    nearbyPreview.append(element("strong", "", nearby.advName || "Unnamed node"), element("span", "", `${formatAge(nearby.lastAdvert)} · ${hex(nearby.publicKey, 4)}`));
  } else {
    nearbyPreview.className = "preview empty";
    nearbyPreview.textContent = "Waiting for adverts";
  }
  const rssi = state.radio?.lastRssi;
  text("home-rssi", Number.isFinite(rssi) ? `${rssi} dBm` : "— dBm");
  text("home-snr", Number.isFinite(state.radio?.lastSnr) ? `SNR ${state.radio.lastSnr.toFixed(1)}` : "SNR —");
  text("home-frequency", self?.radioFreq ? `${radioFrequency(self.radioFreq)} MHz` : "— MHz");
  text("home-packets", state.packets ? `${formatNumber(state.packets.recv)} packets` : "— packets");
  $("radio-meter-fill").style.width = Number.isFinite(rssi) ? `${Math.max(4, Math.min(100, (rssi + 125) * 2))}%` : "0";
  renderDashboardAnalytics();
}

function renderDashboardAnalytics() {
  const ultimate = state.ultimate;
  const received = ultimate?.rx ?? state.packets?.recv;
  const sent = ultimate?.tx ?? state.packets?.sent;
  text("dashboard-rx", formatNumber(received));
  text("dashboard-tx", formatNumber(sent));
  const delivery = ultimate?.delivery || {};
  text("dashboard-delivery", String(delivery.state || "idle").toUpperCase());
  text("dashboard-delivery-note", delivery.target || "No local send");
  text("dashboard-history", formatNumber(ultimate?.historyCount));
  text("dashboard-history-note", ultimate ? `of ${formatNumber(ultimate.historyCapacity || 0)} records` : "stored messages");
  const total = drawActivityBars($("dashboard-activity-chart"), ultimate?.hours, 24);
  text("dashboard-activity-total", total ? `${formatNumber(total)} packets shown` : "No samples yet");
  drawSignalHistogram($("dashboard-signal-chart"), state.rfSamples);
  renderNearbyBars();
}

function drawSignalHistogram(canvas, samples = []) {
  const surface = canvasContext(canvas, 138);
  if (!surface) return;
  const { context, width, height } = surface;
  const values = samples.filter(Number.isFinite).slice(-60);
  text("dashboard-signal-note", values.length ? `${values.length} recent sample${values.length === 1 ? "" : "s"}` : "Waiting for packets");
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "rgba(141,152,167,.14)";
  [0.25, 0.5, 0.75].forEach((part) => { context.beginPath(); context.moveTo(0, height * part); context.lineTo(width, height * part); context.stroke(); });
  const edges = [-130, -115, -105, -95, -85, -70];
  const counts = edges.slice(0, -1).map((low, index) => values.filter((value) => value >= low && value < edges[index + 1]).length);
  const max = Math.max(1, ...counts); const gap = 8; const barWidth = (width - gap * (counts.length - 1)) / counts.length;
  counts.forEach((count, index) => {
    const barHeight = Math.max(count ? 5 : 1, count / max * (height - 28));
    const gradient = context.createLinearGradient(0, height - barHeight, 0, height);
    gradient.addColorStop(0, index >= 3 ? "#44f08a" : index >= 2 ? "#39e7ff" : "#ff8a3d"); gradient.addColorStop(1, "rgba(36,87,255,.28)");
    context.fillStyle = gradient; context.fillRect(index * (barWidth + gap), height - barHeight - 16, barWidth, barHeight);
    context.fillStyle = "rgba(245,248,251,.78)"; context.font = "9px system-ui, sans-serif"; context.textAlign = "center";
    context.fillText(String(count), index * (barWidth + gap) + barWidth / 2, height - 3);
  });
  context.textAlign = "left";
}

function routeSummary(contact) {
  const raw = Number(contact?.outPathLen); const packed = Number.isFinite(raw) ? raw & 0xff : 0xff;
  const hops = packed === 0xff ? null : packed & 0x3f;
  return { hops, label: hops === null ? "route unknown" : hops === 0 ? "direct" : `${hops} hop${hops === 1 ? "" : "s"}` };
}

function renderNearbyBars() {
  const host = $("dashboard-nearby-bars"); host.replaceChildren();
  const now = Math.floor(Date.now() / 1000); const nodes = state.nearby.slice(0, 6);
  if (!nodes.length) { host.append(element("p", "empty-state", "Waiting for node adverts.")); return; }
  nodes.forEach((contact) => {
    const age = Math.max(0, now - Number(contact.lastAdvert || now));
    const freshness = Math.max(5, 100 - Math.min(100, age / 216));
    const row = element("div", "nearby-bar"); const track = element("span", "bar-track"); const fill = document.createElement("i");
    fill.style.width = `${freshness}%`; track.append(fill);
    row.append(element("strong", "", contact.advName || "Unnamed node"), track, element("span", "", `${routeSummary(contact).label} · ${formatAge(contact.lastAdvert)}`));
    host.append(row);
  });
}

function renderMessages() {
  normalizeTargets();
  const targets = [
    ...state.channels.map((channel) => ({ key: channelKey(channel), name: `#${channel.name}`, subtitle: "Channel", avatar: "#" })),
    ...state.contacts.map((contact) => ({ key: contactKey(contact), name: contact.advName || "Unnamed contact", subtitle: "Direct", avatar: initials(contact.advName) })),
  ];
  text("thread-count", targets.length);
  const threadList = $("thread-list");
  threadList.replaceChildren();
  if (!targets.length) threadList.append(element("p", "empty-state", "Contacts and channels appear after sync."));
  for (const target of targets) {
    const messages = state.messages.filter((item) => item.key === target.key);
    const latest = messages.at(-1);
    const unread = messages.filter((item) => item.unread).length;
    const button = element("button", `thread${state.selected === target.key ? " active" : ""}`);
    button.type = "button";
    const avatar = element("span", "avatar", target.avatar);
    const copy = element("span", "thread-copy");
    copy.append(element("strong", "", target.name), element("small", "", latest?.text || target.subtitle));
    const meta = unread ? element("span", "thread-badge", unread) : element("time", "thread-time", latest ? formatClock(latest.timestamp) : "");
    button.append(avatar, copy, meta);
    button.addEventListener("click", () => selectTarget(target.key));
    threadList.append(button);
  }

  const select = $("target-select");
  select.replaceChildren(new Option("Choose recipient", ""));
  if (state.channels.length) {
    const group = document.createElement("optgroup"); group.label = "Channels";
    for (const channel of state.channels) group.append(new Option(`#${channel.name}`, channelKey(channel)));
    select.append(group);
  }
  if (state.contacts.length) {
    const group = document.createElement("optgroup"); group.label = "Direct contacts";
    for (const contact of state.contacts) group.append(new Option(contact.advName || "Unnamed contact", contactKey(contact)));
    select.append(group);
  }
  select.value = state.selected;
  const info = targetInfo(state.selected);
  text("conversation-title", info?.name || "Choose a conversation");
  text("conversation-subtitle", info?.subtitle || "Channel or direct contact");
  text("conversation-avatar", info?.avatar || "#");
  const list = $("message-list");
  list.replaceChildren();
  const messages = state.messages.filter((item) => item.key === state.selected);
  if (!messages.length) list.append(element("div", "welcome-bubble", info ? `No messages with ${info.name} yet.` : "Select a channel or contact to start talking across the mesh."));
  for (const message of messages) {
    const bubble = element("article", `bubble ${message.direction === "out" ? "out" : "in"} ${message.status === "failed" ? "failed" : ""}`);
    const status = message.direction === "out" ? ` · ${message.status}` : Number.isFinite(message.snr) ? ` · SNR ${message.snr.toFixed(1)}` : "";
    bubble.append(element("p", "", message.text), element("small", "", `${formatClock(message.timestamp)}${status}`));
    list.append(bubble);
  }
  requestAnimationFrame(() => { list.scrollTop = list.scrollHeight; });
  $("message-input").disabled = !info || !state.connected;
  $("composer").querySelector("button").disabled = !info || !state.connected;
}

function selectTarget(key) {
  state.selected = key;
  for (const message of state.messages) if (message.key === key) message.unread = false;
  persistHistory();
  renderMessages();
  renderHome();
}

function renderNearby() {
  const list = $("nearby-list");
  list.replaceChildren();
  requestAnimationFrame(drawMeshMap);
  if (!state.nearby.length) {
    list.append(element("article", "card empty-state", "Nearby nodes appear as adverts arrive."));
    return;
  }
  for (const contact of state.nearby) {
    const card = element("article", "card nearby-card");
    const avatar = element("span", "avatar", initials(contact.advName));
    const body = element("div");
    const route = routeSummary(contact).label;
    const location = advertisedLocation(contact);
    body.append(element("p", "eyebrow", contact.type === 2 ? "REPEATER" : contact.type === 3 ? "ROOM" : "CONTACT"), element("h3", "", contact.advName || "Unnamed node"), element("p", "", `ID ${hex(contact.publicKey, 5)} · ${route}${location ? ` · ${location.lat.toFixed(3)}, ${location.lon.toFixed(3)}` : ""}`));
    const time = element("time", "", formatAge(contact.lastAdvert));
    const action = element("button", "contact-action", "Message →");
    action.type = "button";
    action.addEventListener("click", () => { state.selected = contactKey(contact); showView("messages"); });
    card.append(avatar, body, time, action);
    list.append(card);
  }
}

function drawHeardView(context, width, height) {
  const nodes = state.nearby.slice(0, 64); const now = Math.floor(Date.now() / 1000);
  const direct = nodes.filter((node) => routeSummary(node).hops === 0).length;
  text("map-located", `${nodes.length} heard`); text("map-unlocated", `${direct} direct · ${nodes.length - direct} routed/unknown`);
  $("map-empty").hidden = Boolean(nodes.length);
  const gradient = context.createRadialGradient(width / 2, height / 2, 12, width / 2, height / 2, Math.max(width, height) * .62);
  gradient.addColorStop(0, "#10202a"); gradient.addColorStop(1, "#070b10"); context.fillStyle = gradient; context.fillRect(0, 0, width, height);
  const cx = width / 2; const cy = height / 2; const maxRadius = Math.max(62, Math.min(width, height) * .42);
  const rings = [maxRadius * .28, maxRadius * .56, maxRadius * .82];
  context.strokeStyle = "rgba(57,231,255,.14)"; context.lineWidth = 1;
  rings.forEach((radius, index) => { context.beginPath(); context.arc(cx, cy, radius, 0, Math.PI * 2); context.stroke(); context.fillStyle = "rgba(141,152,167,.62)"; context.font = "9px system-ui, sans-serif"; context.fillText(index === 0 ? "DIRECT" : index === 1 ? "1 HOP" : "2+ / UNKNOWN", cx + 8, cy - radius + 13); });
  nodes.forEach((contact, index) => {
    const route = routeSummary(contact); const ring = route.hops === 0 ? 0 : route.hops === 1 ? 1 : 2;
    const seed = [...(contact.publicKey || [])].slice(0, 6).reduce((sum, value, offset) => sum + value * (offset + 3), 0) || index * 73;
    const angle = (seed % 360) * Math.PI / 180; const radius = rings[ring] + ((seed % 17) - 8) * .8;
    const x = cx + Math.cos(angle) * radius; const y = cy + Math.sin(angle) * radius;
    const age = Math.max(0, now - Number(contact.lastAdvert || now)); const alpha = Math.max(.3, 1 - Math.min(1, age / 21600));
    const color = contact.type === 2 ? "#ff8a3d" : contact.type === 3 ? "#ffd447" : "#42a5ff";
    context.globalAlpha = alpha; context.fillStyle = color; context.shadowColor = color; context.shadowBlur = 10; context.beginPath(); context.arc(x, y, 4, 0, Math.PI * 2); context.fill(); context.shadowBlur = 0;
    if (index < 18) { context.fillStyle = "#f5f8fb"; context.font = "10px system-ui, sans-serif"; context.fillText((contact.advName || "Unnamed").slice(0, 18), x + 8, y - 6); }
    context.globalAlpha = 1;
  });
  context.fillStyle = "#44f08a"; context.shadowColor = "#44f08a"; context.shadowBlur = 15; context.fillRect(cx - 5, cy - 5, 10, 10); context.shadowBlur = 0;
  context.fillStyle = "#f5f8fb"; context.font = "700 10px system-ui, sans-serif"; context.fillText("THIS RADIO", cx + 11, cy + 4);
  context.fillStyle = "rgba(141,152,167,.7)"; context.font = "9px system-ui, sans-serif"; context.fillText("Ring = stored route · brightness = advert freshness", 14, height - 12);
}

function drawMeshMap() {
  const surface = canvasContext($("mesh-map"), 360);
  if (!surface) return;
  const { context, width, height } = surface;
  if (state.mapMode === "heard") { drawHeardView(context, width, height); return; }
  const selfLocation = advertisedLocation(state.self);
  const located = state.nearby.map((contact) => ({ contact, location: advertisedLocation(contact) })).filter((entry) => entry.location);
  text("map-located", `${located.length} located`);
  text("map-unlocated", `${Math.max(0, state.nearby.length - located.length)} without location`);
  $("map-empty").hidden = Boolean(located.length || selfLocation);

  const gradient = context.createLinearGradient(0, 0, 0, height);
  gradient.addColorStop(0, "#0d1820"); gradient.addColorStop(1, "#070b10");
  context.fillStyle = gradient; context.fillRect(0, 0, width, height);
  context.strokeStyle = "rgba(57,231,255,.09)"; context.lineWidth = 1;
  for (let x = 0; x <= width; x += Math.max(34, width / 12)) { context.beginPath(); context.moveTo(x, 0); context.lineTo(x, height); context.stroke(); }
  for (let y = 0; y <= height; y += Math.max(34, height / 8)) { context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke(); }
  context.fillStyle = "rgba(141,152,167,.55)"; context.font = "10px ui-monospace, monospace";
  context.fillText("N", 17, 23); context.beginPath(); context.moveTo(20, 30); context.lineTo(20, 52); context.strokeStyle = "#44f08a"; context.stroke();

  const points = [...located.map((entry) => entry.location), ...(selfLocation ? [selfLocation] : [])];
  if (!points.length) return;
  let minLat = Math.min(...points.map((point) => point.lat)); let maxLat = Math.max(...points.map((point) => point.lat));
  let minLon = Math.min(...points.map((point) => point.lon)); let maxLon = Math.max(...points.map((point) => point.lon));
  const latPad = Math.max(.002, (maxLat - minLat) * .16); const lonPad = Math.max(.002, (maxLon - minLon) * .16);
  minLat -= latPad; maxLat += latPad; minLon -= lonPad; maxLon += lonPad;
  const project = ({ lat, lon }) => ({
    x: 36 + (lon - minLon) / (maxLon - minLon) * (width - 72),
    y: 30 + (maxLat - lat) / (maxLat - minLat) * (height - 60),
  });
  const selfPoint = selfLocation ? project(selfLocation) : null;
  if (selfPoint) {
    context.setLineDash([4, 7]); context.strokeStyle = "rgba(57,231,255,.18)";
    located.forEach(({ location }) => { const point = project(location); context.beginPath(); context.moveTo(selfPoint.x, selfPoint.y); context.lineTo(point.x, point.y); context.stroke(); });
    context.setLineDash([]);
  }
  located.slice(0, 40).forEach(({ contact, location }, index) => {
    const point = project(location); const color = contact.type === 2 ? "#ff8a3d" : contact.type === 3 ? "#ffd447" : "#42a5ff";
    context.beginPath(); context.arc(point.x, point.y, 10, 0, Math.PI * 2); context.fillStyle = `${color}22`; context.fill();
    context.beginPath(); context.arc(point.x, point.y, 4, 0, Math.PI * 2); context.fillStyle = color; context.shadowColor = color; context.shadowBlur = 12; context.fill(); context.shadowBlur = 0;
    if (index < 16) { context.fillStyle = "#f5f8fb"; context.font = "11px system-ui, sans-serif"; context.fillText((contact.advName || "Unnamed").slice(0, 20), point.x + 9, point.y - 7); }
  });
  if (selfPoint) {
    context.fillStyle = "#44f08a"; context.fillRect(selfPoint.x - 5, selfPoint.y - 5, 10, 10);
    context.shadowColor = "#44f08a"; context.shadowBlur = 16; context.strokeStyle = "#dfffea"; context.strokeRect(selfPoint.x - 7, selfPoint.y - 7, 14, 14); context.shadowBlur = 0;
    context.fillStyle = "#f5f8fb"; context.font = "700 11px system-ui, sans-serif"; context.fillText("THIS DEVICE", selfPoint.x + 11, selfPoint.y + 4);
  }
  context.fillStyle = "rgba(141,152,167,.7)"; context.font = "9px ui-monospace, monospace";
  context.fillText(`${minLat.toFixed(3)}° to ${maxLat.toFixed(3)}° N`, 14, height - 12);
  context.textAlign = "right"; context.fillText(`${minLon.toFixed(3)}° to ${maxLon.toFixed(3)}° E`, width - 14, height - 12); context.textAlign = "left";
}

function renderRadio() {
  const self = state.self;
  const radio = state.radio;
  const packets = state.packets;
  text("radio-rssi", Number.isFinite(radio?.lastRssi) ? radio.lastRssi : "—");
  text("radio-quality", Number.isFinite(radio?.lastRssi) ? signalQuality(radio.lastRssi) : "Waiting for sample");
  text("radio-snr", Number.isFinite(radio?.lastSnr) ? `SNR ${radio.lastSnr.toFixed(1)} dB` : "SNR —");
  text("radio-noise", Number.isFinite(radio?.noiseFloor) ? `Noise ${radio.noiseFloor} dBm` : "Noise —");
  text("radio-frequency", radioFrequency(self?.radioFreq));
  text("radio-bandwidth", radioBandwidth(self?.radioBw));
  text("radio-spreading", self?.radioSf ? `SF${self.radioSf}` : "—");
  text("radio-coding", self?.radioCr ? `CR 4/${self.radioCr}` : "CR —");
  text("radio-power", Number.isFinite(self?.txPower) ? self.txPower : "—");
  text("packet-received", formatNumber(packets?.recv));
  text("packet-sent", formatNumber(packets?.sent));
  text("packet-direct", formatNumber(packets?.nRecvDirect));
  text("packet-flood", formatNumber(packets?.nRecvFlood));
  const errors = packets?.nRecvErrors ?? 0;
  text("packet-errors", `${formatNumber(errors)} error${errors === 1 ? "" : "s"}`);
  $("packet-errors").className = errors ? "error" : "quiet";
  if (state.view === "radio") requestAnimationFrame(() => { drawSignalChart(); drawRadioActivity(); });
}

function drawRadioActivity() {
  const total = drawActivityBars($("radio-activity-chart"), state.ultimate?.hours, 72);
  text("radio-activity-total", total ? `${formatNumber(total)} packets shown` : "No samples yet");
}

function drawSignalChart() {
  const canvas = $("signal-chart");
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (width < 20 || height < 20) return;
  const ratio = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d"); context.scale(ratio, ratio);
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "rgba(141,152,167,.12)"; context.lineWidth = 1;
  for (let row = 1; row < 4; row += 1) { context.beginPath(); context.moveTo(0, row * height / 4); context.lineTo(width, row * height / 4); context.stroke(); }
  const samples = state.rfSamples.length > 1 ? state.rfSamples : [-120, -120];
  context.beginPath();
  samples.forEach((sample, index) => {
    const x = index * width / Math.max(1, samples.length - 1);
    const y = height - Math.max(0, Math.min(1, (sample + 130) / 60)) * height;
    index ? context.lineTo(x, y) : context.moveTo(x, y);
  });
  context.strokeStyle = "#44f08a"; context.lineWidth = 2; context.shadowColor = "#44f08a"; context.shadowBlur = 8; context.stroke();
}

function renderNetwork() {
  const network = state.network;
  const busy = state.linkStatus === "busy";
  if (!network) {
    text("network-title", "Set up local Wi-Fi");
    text("network-summary", "Use your 2.4 GHz home network instead of staying on the setup hotspot.");
    text("network-current", "Network status unavailable");
    text("more-network-title", "Radio network");
    text("ap-address", "Current mode and IP are unavailable");
    $("network-setup-button").textContent = "Set up local Wi-Fi";
  } else if (network.mode === "station") {
    text("network-title", `Connected to ${network.ssid || "local Wi-Fi"}`);
    text("network-summary", `${network.ip || "DHCP address pending"}${network.fallback ? " · fallback active" : " · ready on your local network"}`);
    text("network-current", `${network.ssid || "Local Wi-Fi"} · ${network.ip || "DHCP pending"}`);
    text("more-network-title", "Local Wi-Fi");
    text("ap-address", `${network.ssid || "Station mode"} · ${network.ip || "DHCP pending"}`);
    $("network-setup-button").textContent = "Change local Wi-Fi";
  } else {
    text("network-title", network.fallback ? "Local Wi-Fi needs attention" : "Set up local Wi-Fi");
    text("network-summary", `${network.ssid || "setup AP"} · ${network.ip || "192.168.4.1"}${network.fallback ? " · station fallback" : ""}`);
    text("network-current", `${network.ssid || "setup AP"} · ${network.ip || "192.168.4.1"}`);
    text("more-network-title", network.fallback ? "Fallback setup AP" : "Setup AP");
    text("ap-address", `${network.ssid || "Setup hotspot"} · ${network.ip || "192.168.4.1"}`);
    $("network-setup-button").textContent = "Set up local Wi-Fi";
  }
  $("network-setup-button").disabled = busy;
  $("return-ap-button").hidden = network?.mode !== "station";
  $("return-ap-button").disabled = busy;
}

function openNetworkDialog() {
  renderNetwork();
  $("network-form").hidden = false;
  $("network-result").hidden = true;
  $("network-ssid").value = state.network?.mode === "station" ? state.network.ssid : "";
  $("network-password").value = "";
  $("network-password").type = "password";
  $("network-reveal").textContent = "Show";
  $("network-reveal").setAttribute("aria-pressed", "false");
  const dialog = $("network-dialog");
  if (dialog.showModal) dialog.showModal(); else dialog.setAttribute("open", "");
  $("network-ssid").focus();
}

function closeNetworkDialog() {
  const dialog = $("network-dialog");
  if (dialog.close) dialog.close(); else dialog.removeAttribute("open");
}

function showNetworkResult(mode) {
  $("network-form").hidden = true;
  $("network-result").hidden = false;
  const steps = $("network-result-steps");
  steps.replaceChildren();
  const values = mode === "station"
    ? ["Wait for the radio to restart.", "Reconnect this phone or computer to your home Wi-Fi.", "Read the new DHCP IP on the screen and open it in your browser."]
    : ["Wait for the radio to restart in setup AP mode.", "Reconnect this phone or computer to the Wi-Fi shown on the screen.", "Open 192.168.4.1 in your browser."];
  for (const value of values) steps.append(element("li", "", value));
  text("network-result-title", mode === "station" ? "Reconnect to your home Wi-Fi" : "Reconnect to the setup AP");
  const note = $("network-result-note");
  note.replaceChildren();
  if (mode === "station") {
    note.append("In station mode, the browser may request HTTP Basic authentication. Use username ", element("strong", "", "meshcore"), " and the 8-letter key shown on the TFT as the password.");
  } else {
    note.textContent = "The TFT shows the setup Wi-Fi name and password after restart.";
  }
}

function renderMore() {
  const self = state.self;
  const device = state.device;
  const model = String(device?.manufacturerModel || "").split("\0", 1)[0]
    .replace(/[^\x20-\x7e]/g, "").replace(/_/g, " ").trim();
  text("more-name", self?.name || "RADIO");
  text("more-model", model || "MeshCore Companion");
  text("more-firmware", device?.firmwareVer != null ? `Protocol ${device.firmwareVer}` : "—");
  text("more-build", device?.firmware_build_date || "—");
  text("more-key", self?.publicKey ? `${hex(self.publicKey, 8)}…` : "—");
  text("app-version", __APP_VERSION__);
  text("library-version", __MESHCORE_JS_VERSION__);
  $("advert-button").disabled = !state.connected;
  $("clear-history-button").disabled = !state.messages.length;
}

function drawUltimateChart(canvas, rows, keys, colors) {
  if (!canvas) return;
  const ratio = devicePixelRatio || 1;
  const width = Math.max(canvas.clientWidth, 280);
  const height = Math.max(canvas.clientHeight, 160);
  canvas.width = width * ratio; canvas.height = height * ratio;
  const ctx = canvas.getContext("2d");
  ctx.scale(ratio, ratio); ctx.clearRect(0, 0, width, height);
  ctx.strokeStyle = "rgba(255,255,255,.055)"; ctx.lineWidth = 1;
  for (let y = 18; y < height; y += 35) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke(); }
  const ordered = [...(rows || [])].reverse();
  if (ordered.length < 2) return;
  const trafficKeys = keys.filter((key) => key !== "battery");
  const trafficMaximum = Math.max(1,
    ...ordered.flatMap((row) => trafficKeys.map((key) => Number(row[key]) || 0)));
  keys.forEach((key, series) => {
    ctx.strokeStyle = colors[series]; ctx.lineWidth = series ? 1.6 : 2.4;
    ctx.shadowColor = colors[series]; ctx.shadowBlur = series ? 4 : 9; ctx.beginPath();
    let drawing = false;
    ordered.forEach((row, index) => {
      const x = index * width / (ordered.length - 1);
      const raw = Number(row[key]);
      if (!Number.isFinite(raw) || (key === "battery" && raw <= 0)) {
        drawing = false;
        return;
      }
      const scaled = key === "battery" ? Math.max(0, Math.min(1, (raw - 3000) / 1200))
                                        : raw / trafficMaximum;
      const y = height - 10 - scaled * (height - 26);
      drawing ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      drawing = true;
    });
    ctx.stroke(); ctx.shadowBlur = 0;
  });
}

function renderUltimate() {
  const ultimate = state.ultimate;
  if (!ultimate) return;
  text("ultimate-gate", ultimate.memoryGate ? "PASS" : "PENDING");
  $("ultimate-gate-dot").className = ultimate.memoryGate ? "pass" : "";
  text("ultimate-history-count", formatNumber(ultimate.historyCount || 0));
  text("ultimate-history-capacity", `${formatNumber(ultimate.historyCapacity || 0)} record capacity`);
  text("ultimate-free-heap", `${Math.round((ultimate.freeHeap || 0) / 1024)} KB`);
  text("ultimate-largest", `largest ${Math.round((ultimate.largestAllocation || 0) / 1024)} KB`);
  text("ultimate-flush", `${((ultimate.displayFlushUs || 0) / 1000).toFixed(1)} ms`);
  text("ultimate-tiles", `${ultimate.displayTiles || 0} / 176 tiles`);
  text("ultimate-drops", ultimate.eventDrops || 0);
  text("ultimate-queue", `outbound ${ultimate.queueDepth || 0} · air ${Math.round((ultimate.airtimeMs || 0) / 60000)}m`);
  const profileNames = ["BALANCED", "FIELD", "BATTERY"];
  const trend = Number(ultimate.batteryTrendMvPerHour || 0);
  text("ultimate-battery", ultimate.batteryMv > 0
    ? `${(ultimate.batteryMv / 1000).toFixed(2)} V · ${trend > 0 ? "+" : ""}${trend} mV/h`
    : "—");
  text("ultimate-runtime", ultimate.usbHostConnected
    ? "USB host connected · unplug to start a clean discharge window"
    : ultimate.batteryRuntimeMinutes > 0
    ? `about ${Math.floor(ultimate.batteryRuntimeMinutes / 60)}h ${ultimate.batteryRuntimeMinutes % 60}m to 3.45 V`
    : `${profileNames[ultimate.powerProfile] || "BALANCED"} · ${ultimate.animationFrameMs || 66} ms frames`);
  const delivery = ultimate.delivery || {};
  text("ultimate-delivery", String(delivery.state || "idle").toUpperCase());
  text("ultimate-delivery-target", delivery.target
    ? `${delivery.target}${delivery.roundTripMs ? ` · ${delivery.roundTripMs} ms` : ""}`
    : "No on-device send");

  const minuteRows = (ultimate.minutes || [])
    .map((row) => ({ rx: row[1], tx: row[2], battery: row[7] }));
  const hourRows = (ultimate.hours || [])
    .map((row) => ({ rx: row[1], tx: row[2], failures: row[3] }));
  drawUltimateChart($("ultimate-minute-chart"), minuteRows,
    ["rx", "tx", "battery"], ["#44f08a", "#39e7ff", "#ffb84d"]);
  drawUltimateChart($("ultimate-week-chart"), hourRows, ["rx", "tx", "failures"], ["#44f08a", "#39e7ff", "#ff5468"]);

  const nodes = ultimate.nodes || [];
  text("ultimate-node-count", `${nodes.length} radio${nodes.length === 1 ? "" : "s"}`);
  const nodeList = $("ultimate-node-list"); nodeList.replaceChildren();
  if (!nodes.length) nodeList.append(element("p", "empty-state", "Listening on the configured preset."));
  nodes.slice(0, 16).forEach((node) => {
    const item = element("div", "ultimate-node");
    item.append(element("strong", "", node.name || "Unnamed radio"),
                element("b", "", Number.isFinite(node.rssi) ? `${node.rssi} dBm` : `${node.path} hops`),
                element("span", "", `Role ${node.role} · ${formatAge(node.seen)}`),
                element("span", "", `${node.packets} packet${node.packets === 1 ? "" : "s"}`));
    nodeList.append(item);
  });

  const historyList = $("ultimate-history-list"); historyList.replaceChildren();
  if (!state.ultimateHistory.length) historyList.append(element("p", "empty-state", "No stored messages."));
  state.ultimateHistory.forEach((record) => {
    const item = element("div", "ultimate-record");
    item.append(element("strong", "", record.sender || (record.kind === 2 ? "#channel" : "Direct")),
                element("span", "", `${record.incoming ? "Received" : "Sent"} · ${formatAge(record.timestamp)}`),
                element("p", "", record.text || ""));
    historyList.append(item);
  });

  const settings = state.ultimateSettings;
  if (settings) {
    $("ultimate-history-cap").value = String(settings.historyCapacity);
    $("ultimate-cadence").value = String(settings.scanCadenceMs);
    $("ultimate-power-profile").value = String(settings.powerProfile || 0);
    $("ultimate-battery-capacity").value = String(settings.batteryCapacityMah || 0);
    $("ultimate-battery-calibration").value = String(settings.batteryCalibrationMv || 0);
    $("ultimate-private").checked = Boolean(settings.privateNotifications);
    const phrases = $("ultimate-phrases");
    if (!phrases.children.length) (settings.quickPhrases || []).forEach((phrase, index) => {
      const label = element("label", "", `Quick phrase ${index + 1}`);
      const input = element("input"); input.type = "text"; input.maxLength = 47;
      input.dataset.phrase = index; input.value = phrase; label.append(input); phrases.append(label);
    });
  }
  renderDashboardAnalytics();
  if (state.view === "radio") requestAnimationFrame(drawRadioActivity);
}

function renderAll() {
  if (state.battery) {
    const volts = `${(state.battery / 1000).toFixed(2)} V`;
    text("battery-chip", volts);
  } else {
    text("battery-chip", "—");
  }
  renderHome(); renderMessages(); renderNearby(); renderRadio(); renderNetwork(); renderUltimate(); renderMore();
}

function showView(view) {
  state.view = view;
  document.querySelectorAll(".screen").forEach((screen) => screen.classList.toggle("active", screen.dataset.screen === view));
  document.querySelectorAll(".nav button").forEach((button) => button.classList.toggle("active", button.dataset.view === view));
  const labels = {
    home: ["Overview", "NEONPOCKETMC"], messages: ["Messages", "CHANNELS & DIRECT"],
    nearby: ["Nearby", "NODES & LOCATIONS"], radio: ["Radio", "LIVE RF"], ultimate: ["This radio", "STORAGE, POWER & DISPLAY"],
  };
  text("screen-title", labels[view]?.[0] || "Ultimate");
  text("screen-kicker", labels[view]?.[1] || "NEONPOCKETMC");
  if (view === "messages" && state.selected) selectTarget(state.selected);
  if (view === "radio") renderRadio();
  if (view === "nearby") requestAnimationFrame(drawMeshMap);
  if (view === "ultimate") refreshUltimate();
  window.scrollTo({ top: 0, behavior: "smooth" });
}

async function sendMessage(event) {
  event.preventDefault();
  const input = $("message-input");
  const messageText = input.value.trim();
  const info = targetInfo(state.selected);
  if (!messageText || !info) return;
  if (encoder.encode(messageText).length > MAX_MESSAGE_BYTES) return toast("Message is over 140 bytes", "error");
  const outgoing = addMessage({ key: state.selected, text: messageText, timestamp: Math.floor(Date.now() / 1000), direction: "out", status: "sending", unread: false });
  input.value = ""; updateComposeCount();
  try {
    const response = await exclusive(() => info.channel ? connection.sendChannelTextMessage(info.channel.channelIdx, messageText) : connection.sendTextMessage(info.contact.publicKey, messageText));
    outgoing.status = "queued";
    if (!info.channel && Number.isInteger(response?.expectedAckCrc)) outgoing.expectedAckCrc = response.expectedAckCrc;
    persistHistory();
    toast("Message queued");
  } catch {
    outgoing.status = "failed";
    persistHistory();
    toast("Message failed", "error");
  }
  renderMessages();
}

function updateComposeCount() {
  const count = encoder.encode($("message-input").value).length;
  text("compose-count", `${count} / ${MAX_MESSAGE_BYTES} bytes`);
  $("compose-count").className = count > MAX_MESSAGE_BYTES ? "error" : "";
}

function bindConnectionEvents(radio) {
  radio.on(Constants.ResponseCodes.ContactMsgRecv, (message) => { unannouncedMessages += ingestWaiting({ contactMessage: message }); });
  radio.on(Constants.ResponseCodes.ChannelMsgRecv, (message) => { unannouncedMessages += ingestWaiting({ channelMessage: message }); });
  radio.on("connected", () => {
    if (!radio.ready || !radio.ownsLease()) return;
    setLink("connected");
    toast("Radio connected");
    setTimeout(() => $("boot").classList.add("hidden"), 220);
    exclusive(syncAll).then(() => { recoveryAttempts = 0; }).catch(() => toast("Initial sync failed", "error"));
  });
  radio.on("disconnected", () => setLink("offline"));
  radio.on("transport-busy", () => {
    setLink("busy");
    $("boot").classList.add("hidden");
    toast("Another tab controls this radio. Close it, then tap the link status to retry.", "warn");
  });
  radio.on("transport-state", () => { setLink("reconnecting"); toast("Link interrupted — reconnecting", "warn"); });
  radio.on("transport-restored", () => {
    setLink("connected");
    toast("Link restored — resyncing");
    exclusive(syncAll).then(() => { recoveryAttempts = 0; }).catch(() => toast("Resync failed", "error"));
  });
  radio.on(Constants.PushCodes.MsgWaiting, () => { if (radio.ready) exclusive(syncMessages).catch(() => toast("Message sync failed", "error")); });
  radio.on(Constants.PushCodes.NewAdvert, (advert) => {
    const existing = state.contacts.findIndex((item) => hex(item.publicKey) === hex(advert.publicKey));
    if (existing >= 0) state.contacts[existing] = advert; else state.contacts.unshift(advert);
    refreshNearbyFromContacts(); renderAll(); toast(`${advert.advName || "A node"} is nearby`, "warn");
  });
  radio.on(Constants.PushCodes.Advert, () => { if (radio.ready) exclusive(refreshContacts).catch(() => {}); });
  radio.on(Constants.PushCodes.SendConfirmed, (confirmation) => {
    const pending = [...state.messages].reverse().find((item) => item.direction === "out" && item.key.startsWith("dm:") && item.status === "queued" && item.expectedAckCrc === confirmation.ackCode);
    if (pending) { pending.status = "delivered"; persistHistory(); renderMessages(); toast("Direct message delivered"); }
  });
  radio.on(Constants.PushCodes.LogRxData, (sample) => {
    state.radio = { ...(state.radio || {}), lastRssi: sample.lastRssi, lastSnr: sample.lastSnr };
    state.rfSamples.push(sample.lastRssi); state.rfSamples = state.rfSamples.slice(-36); renderHome(); renderRadio();
  });
}

document.querySelectorAll(".nav button").forEach((button) => button.addEventListener("click", () => showView(button.dataset.view)));
document.querySelectorAll("[data-open]").forEach((button) => button.addEventListener("click", () => showView(button.dataset.open)));
$("target-select").addEventListener("change", (event) => selectTarget(event.target.value));
$("composer").addEventListener("submit", sendMessage);
$("message-input").addEventListener("input", updateComposeCount);
$("nearby-refresh").addEventListener("click", () => exclusive(refreshContacts).then(() => toast("Nearby refreshed")).catch(() => toast("Refresh failed", "error")));
document.querySelectorAll("[data-map-mode]").forEach((button) => button.addEventListener("click", () => {
  state.mapMode = button.dataset.mapMode;
  document.querySelectorAll("[data-map-mode]").forEach((item) => item.classList.toggle("active", item === button));
  requestAnimationFrame(drawMeshMap);
}));
$("refresh-button").addEventListener("click", () => exclusive(syncAll).then(() => toast("Sync complete")).catch(() => toast("Sync failed", "error")));
$("advert-button").addEventListener("click", async () => {
  $("advert-button").disabled = true;
  try { await exclusive(() => connection.sendFloodAdvert()); toast("Advert queued", "warn"); }
  catch { toast("Advert failed", "error"); }
  finally { $("advert-button").disabled = false; }
});
$("connect-button").addEventListener("click", () => {
  recoveryAttempts = 0;
  if (state.connected) return exclusive(syncAll).catch(() => toast("Sync failed", "error"));
  return connection.connect().catch(() => toast("Could not reconnect", "error"));
});
$("network-setup-button").addEventListener("click", openNetworkDialog);
$("network-dialog-close").addEventListener("click", closeNetworkDialog);
$("network-cancel").addEventListener("click", closeNetworkDialog);
$("network-done").addEventListener("click", closeNetworkDialog);
$("network-reveal").addEventListener("click", () => {
  const password = $("network-password");
  const revealed = password.type === "password";
  password.type = revealed ? "text" : "password";
  $("network-reveal").textContent = revealed ? "Hide" : "Show";
  $("network-reveal").setAttribute("aria-pressed", String(revealed));
});
$("network-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const ssid = $("network-ssid").value;
  const password = $("network-password").value;
  const ssidBytes = encoder.encode(ssid).length;
  const passwordBytes = encoder.encode(password).length;
  if (ssidBytes < 1 || ssidBytes > 32) { toast("Wi-Fi name must be 1–32 bytes", "error"); return $("network-ssid").focus(); }
  if (passwordBytes !== 0 && (passwordBytes < 8 || passwordBytes > 64)) { toast("Password must be empty or 8–64 bytes", "error"); return $("network-password").focus(); }
  $("network-save").disabled = true;
  try {
    await setNetwork({ mode: "station", ssid, password });
    showNetworkResult("station");
    toast("Local Wi-Fi saved. Radio is restarting.", "warn");
  } catch { toast("Could not save Wi-Fi settings", "error"); }
  finally { $("network-save").disabled = false; }
});
$("return-ap-button").addEventListener("click", async () => {
  if (!confirm("Return the radio to setup AP mode? The device will restart.")) return;
  $("return-ap-button").disabled = true;
  try {
    await setNetwork({ mode: "ap" });
    openNetworkDialog();
    showNetworkResult("ap");
    toast("Setup AP requested. Radio is restarting.", "warn");
  } catch { toast("Could not return to setup AP", "error"); }
  finally { $("return-ap-button").disabled = false; }
});
$("clear-history-button").addEventListener("click", () => {
  if (!state.messages.length || !confirm("Clear locally stored message history for this node?")) return;
  try { localStorage.removeItem(historyKey()); } catch { /* storage is optional */ }
  state.messages = [];
  renderAll();
  toast("Local message history cleared");
});
$("ultimate-settings-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const quickPhrases = [...document.querySelectorAll("[data-phrase]")].map((input) => input.value);
  try {
    const response = await ultimateFetch("/api/ultimate/settings", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        historyCapacity: Number($("ultimate-history-cap").value),
        scanCadenceMs: Number($("ultimate-cadence").value),
        powerProfile: Number($("ultimate-power-profile").value),
        batteryCapacityMah: Number($("ultimate-battery-capacity").value),
        batteryCalibrationMv: Number($("ultimate-battery-calibration").value),
        privateNotifications: $("ultimate-private").checked,
        quickPhrases,
      }),
    });
    state.ultimateSettings = await response.json();
    renderUltimate(); toast("Device settings saved");
  } catch { toast("Could not save device settings", "error"); }
});
$("ultimate-export").addEventListener("click", () => {
  window.location.assign("/api/ultimate/export");
});
$("ultimate-clear").addEventListener("click", async () => {
  if (!confirm("Permanently erase the /np/ message journal? MeshCore identity and contacts are preserved.")) return;
  try {
    await ultimateFetch("/api/ultimate/history", { method: "DELETE", headers: { "X-NP-Confirm": "clear" } });
    await refreshUltimate(); toast("On-device history cleared", "warn");
  } catch { toast("History clear failed", "error"); }
});

async function saveBrowserLocation(latitude, longitude, accuracy) {
  text("ultimate-location-preview", `${latitude.toFixed(6)}, ${longitude.toFixed(6)} · ±${Math.round(accuracy)} m`);
  const advertise = $("ultimate-location-share").checked;
  const sharing = advertise ? "This location WILL be included in adverts." : "This location will remain private.";
  if (!confirm(`Save this location to MeshCore?\n\n${latitude.toFixed(6)}, ${longitude.toFixed(6)}\nAccuracy ±${Math.round(accuracy)} m\n\n${sharing}`)) return;
  await ultimateFetch("/api/ultimate/location", {
    method: "PUT", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ latitude, longitude, accuracy, advertise }),
  });
  toast(advertise ? "Location saved and enabled for adverts" : "Location saved privately");
}

$("ultimate-location").addEventListener("click", () => {
  const manual = async () => {
    const value = prompt("Browser location is unavailable on this connection. Enter latitude,longitude:", "");
    if (!value) return;
    const [latitude, longitude] = value.split(",").map(Number);
    if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return toast("Enter latitude,longitude", "error");
    try { await saveBrowserLocation(latitude, longitude, 0); } catch { toast("Location save failed", "error"); }
  };
  if (!navigator.geolocation) return manual();
  navigator.geolocation.getCurrentPosition(
    (position) => saveBrowserLocation(position.coords.latitude, position.coords.longitude,
                                      position.coords.accuracy).catch(() => toast("Location save failed", "error")),
    manual, { enableHighAccuracy: true, timeout: 12000, maximumAge: 0 });
});

$("ultimate-ota-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const file = $("ultimate-ota-file").files[0];
  if (!file || !file.name.toLowerCase().endsWith(".npu")) return toast("Choose a NeonPocket .npu package", "error");
  if (!confirm(`Verify and install ${file.name}? The radio will restart only after board, mode, hash, and signature checks pass.`)) return;
  const body = new FormData(); body.append("package", file, file.name);
  const button = $("ultimate-ota-form").querySelector("button"); button.disabled = true;
  try {
    const response = await fetch("/api/ultimate/ota", { method: "POST", body, credentials: "same-origin" });
    const result = await response.json().catch(() => ({}));
    if (!response.ok || result.result !== "success") throw new Error(result.result || "upload-failed");
    toast("Signature verified. Restarting into the new app.", "warn");
  } catch (error) { toast(`Update rejected: ${error.message}`, "error"); button.disabled = false; }
});
window.addEventListener("resize", () => {
  if (state.view === "home") renderDashboardAnalytics();
  if (state.view === "nearby") drawMeshMap();
  if (state.view === "radio") { drawSignalChart(); drawRadioActivity(); }
  if (state.view === "ultimate") renderUltimate();
});
document.addEventListener("visibilitychange", () => {
  if (!document.hidden && state.connected) exclusive(async () => {
    await syncMessages();
    await refreshStats();
    await refreshNetworkStatus();
  }).catch(() => toast("Foreground sync failed", "error"));
});
window.addEventListener("pagehide", () => connection.leavePage());
window.addEventListener("pageshow", (event) => {
  if (event.persisted && !connection.running) {
    setLink("reconnecting");
    connection.connect().catch(() => toast("Could not reconnect", "error"));
  }
});

bindConnectionEvents(connection);
renderAll();
setInterval(() => {
  text("hero-uptime", formatSession());
  if (!document.hidden && state.connected) exclusive(refreshStats).catch(() => {});
  if (!document.hidden) refreshUltimate();
}, 30000);
connection.connect().catch(() => { setLink("offline"); $("boot").classList.add("hidden"); toast("Could not open companion link", "error"); });
