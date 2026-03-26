let currentStatus = null;
let configInitialized = false;
let latestWifiSurvey = null;
const pendingReceiverSelections = new Map();
const pendingReceiverBrightness = new Map();
let statusEvents = null;
let surveyProgressTimer = null;
let pollTimer = null;
let pendingWirelessTransport = null;
let wirelessTransportUpdateInFlight = false;
let pendingEspNowPhyMode = null;
let espNowPhyModeUpdateInFlight = false;
let pendingRadioChannel = null;
let radioChannelUpdateInFlight = false;

function wirelessTransportLabel(transport) {
  switch (transport) {
    case "auto":
      return "Auto";
    case "beacon_probe":
      return "Beacon / Probe";
    case "espnow":
      return "ESP-NOW";
    case "hostapd":
      return "hostapd";
    case "rawinject":
      return "Raw Inject";
    case "stub":
      return "Stub";
    default:
      return transport || "Unknown";
  }
}

function espNowPhyModeLabel(mode) {
  switch (mode) {
    case "lr_250k":
      return "LR 250 kbit";
    case "lr_500k":
      return "LR 500 kbit";
    case "11b_1m":
      return "11b 1M";
    case "11b_2m":
      return "11b 2M";
    case "11b_11m":
      return "11b 11M";
    case "11g_6m":
      return "11g 6M";
    default:
      return mode || "Unknown";
  }
}

function syncWirelessTransportControl(status) {
  const select = document.getElementById("wirelessTransport");
  if (!select) {
    return;
  }

  const wireless = status?.wireless || {};
  const supported = Array.isArray(wireless.supported_transports) && wireless.supported_transports.length
    ? wireless.supported_transports
    : [wireless.transport || status?.network?.wireless_transport || "auto"];
  const current = wireless.transport || status?.network?.wireless_transport || supported[0];
  const selected = pendingWirelessTransport || select.value || current;

  select.innerHTML = supported.map((transport) =>
    `<option value="${escapeHtml(transport)}">${escapeHtml(wirelessTransportLabel(transport))}</option>`
  ).join("");

  const nextValue = supported.includes(selected) ? selected : current;
  select.value = nextValue;
  select.disabled = supported.length <= 1 || wirelessTransportUpdateInFlight;
}

async function updateWirelessTransport(transport) {
  const select = document.getElementById("wirelessTransport");
  if (!select) {
    return;
  }

  const current = currentStatus?.wireless?.transport
    || currentStatus?.route?.wireless_transport
    || currentStatus?.network?.wireless_transport
    || "auto";
  if (transport === current) {
    pendingWirelessTransport = null;
    syncWirelessTransportControl(currentStatus);
    return;
  }

  pendingWirelessTransport = transport;
  wirelessTransportUpdateInFlight = true;
  syncWirelessTransportControl(currentStatus);

  try {
    const result = await api("/api/v1/wireless/transport", {
      method: "POST",
      body: JSON.stringify({ transport })
    });
    setMessage(result.message || "Wireless transport updated.", "success");
    await refreshStatus();
  } catch (error) {
    pendingWirelessTransport = current;
    setMessage(`Wireless transport update failed: ${error.message}`, "danger");
    syncWirelessTransportControl(currentStatus);
  } finally {
    pendingWirelessTransport = null;
    wirelessTransportUpdateInFlight = false;
    syncWirelessTransportControl(currentStatus);
  }
}

function syncEspNowPhyModeControl(status) {
  const field = document.getElementById("espnowPhyModeField");
  const select = document.getElementById("espnowPhyMode");
  if (!field || !select) {
    return;
  }

  const wireless = status?.wireless || {};
  const advertised = Array.isArray(wireless.supported_espnow_phy_modes)
    ? wireless.supported_espnow_phy_modes.filter(Boolean)
    : [];
  const current = wireless.espnow_phy_pending_mode || wireless.espnow_phy_mode || status?.config?.espnow_phy_mode || "";
  if (!advertised.length && !current) {
    field.hidden = true;
    select.innerHTML = "";
    select.disabled = true;
    return;
  }

  field.hidden = false;
  const supported = advertised.length ? advertised : [current || "11b_1m"];
  const selected = pendingEspNowPhyMode || select.value || current || supported[0];
  select.innerHTML = supported.map((mode) =>
    `<option value="${escapeHtml(mode)}">${escapeHtml(espNowPhyModeLabel(mode))}</option>`
  ).join("");

  const nextValue = supported.includes(selected) ? selected : (current || supported[0]);
  select.value = nextValue;
  select.disabled = supported.length <= 1 || espNowPhyModeUpdateInFlight || Boolean(wireless.espnow_phy_sync_pending);
}

function syncRadioChannelControl(status) {
  const input = document.getElementById("radioChannel");
  const current = Number(status?.network?.radio_channel || 1);
  const selected = pendingRadioChannel ?? (input.value ? Number(input.value) : current);
  if (!Number.isFinite(selected) || selected < 1 || selected > 13) {
    input.value = String(current);
  } else {
    input.value = String(selected);
  }
  input.disabled = radioChannelUpdateInFlight;
  if (pendingRadioChannel === current) {
    pendingRadioChannel = null;
  }
}

async function updateRadioChannel(channelValue) {
  const input = document.getElementById("radioChannel");
  const parsed = Number(channelValue);
  if (!Number.isFinite(parsed) || parsed < 1 || parsed > 13) {
    setMessage("Radio channel must be between 1 and 13.", "danger");
    syncRadioChannelControl(currentStatus || {});
    return;
  }

  const current = Number(currentStatus?.network?.radio_channel || 0);
  if (parsed === current) {
    pendingRadioChannel = null;
    syncRadioChannelControl(currentStatus || {});
    return;
  }

  pendingRadioChannel = parsed;
  radioChannelUpdateInFlight = true;
  syncRadioChannelControl(currentStatus || {});
  try {
    const result = await api("/api/v1/config", {
      method: "PUT",
      body: JSON.stringify({ radio_channel: parsed })
    });
    setMessage(result.message || "Radio channel updated.", "success");
    await refreshStatus();
  } catch (error) {
    pendingRadioChannel = current || null;
    setMessage(`Radio channel update failed: ${error.message}`, "danger");
  } finally {
    radioChannelUpdateInFlight = false;
    syncRadioChannelControl(currentStatus || {});
  }
}

async function updateEspNowPhyMode(mode) {
  const select = document.getElementById("espnowPhyMode");
  if (!select) {
    return;
  }

  const current = currentStatus?.wireless?.espnow_phy_pending_mode
    || currentStatus?.wireless?.espnow_phy_mode
    || "11b_1m";
  if (mode === current) {
    pendingEspNowPhyMode = null;
    syncEspNowPhyModeControl(currentStatus);
    return;
  }

  pendingEspNowPhyMode = mode;
  espNowPhyModeUpdateInFlight = true;
  syncEspNowPhyModeControl(currentStatus);

  try {
    const result = await api("/api/v1/wireless/espnow/phy", {
      method: "POST",
      body: JSON.stringify({ mode })
    });
    setMessage(result.message || "ESP-NOW PHY mode updated.", "success");
    await refreshStatus();
  } catch (error) {
    pendingEspNowPhyMode = current;
    setMessage(`ESP-NOW PHY mode update failed: ${error.message}`, "danger");
    syncEspNowPhyModeControl(currentStatus);
  } finally {
    pendingEspNowPhyMode = null;
    espNowPhyModeUpdateInFlight = false;
    syncEspNowPhyModeControl(currentStatus);
  }
}

function getSelectedSwitcherSource() {
  return document.querySelector('input[name="switcherSource"]:checked')?.value || "none";
}

function deriveSelectedSwitcherSource(status) {
  if (status.switchers.atem.enabled) {
    return "atem";
  }
  if (status.switchers.vmix.enabled) {
    return "vmix";
  }
  if (status.switchers.tsl.enabled) {
    return "tsl";
  }
  if (status.switchers.obs.enabled) {
    return "obs";
  }
  return "none";
}

function setSelectedSwitcherSource(source) {
  const selected = document.querySelector(`input[name="switcherSource"][value="${source}"]`)
    || document.querySelector('input[name="switcherSource"][value="none"]');
  if (selected) {
    selected.checked = true;
  }
  applySwitcherSourceState();
}

function applySwitcherSourceState() {
  const source = getSelectedSwitcherSource();
  const fields = [
    ["atemField", "atemIp", source !== "atem"],
    ["vmixField", "vmixIp", source !== "vmix"],
    ["tslField", "tslListenAddr", source !== "tsl"],
    ["obsUrlField", "obsUrl", source !== "obs"],
    ["obsPasswordField", "obsPassword", source !== "obs"]
  ];

  for (const [fieldId, inputId, disabled] of fields) {
    const field = document.getElementById(fieldId);
    const input = document.getElementById(inputId);
    if (field) {
      field.classList.toggle("disabled", disabled);
    }
    if (input) {
      input.disabled = disabled;
    }
  }
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "\"": "&quot;",
    "'": "&#39;"
  })[char]);
}

async function api(path, options = {}) {
  const headers = Object.assign({ Accept: "application/json" }, options.headers || {});
  if (options.body && !headers["Content-Type"]) {
    headers["Content-Type"] = "application/json";
  }

  const response = await fetch(path, Object.assign({}, options, { headers }));
  const text = await response.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch (_) {}

  if (!response.ok) {
    const message = data && data.message ? data.message : text || response.statusText;
    throw new Error(message);
  }

  return data;
}

function setMessage(text, tone = "info") {
  const line = document.getElementById("commandLine");
  line.textContent = text;
  line.className = `command-banner${tone === "danger" ? " danger" : tone === "success" ? " success" : ""}`;
}

function updateWifiSurveyMeta(text) {
  document.getElementById("wifiSurveyMeta").textContent = text;
}

function surveyMetaPrefix(survey) {
  const mode = survey.scan_mode ? `Mode: ${survey.scan_mode}. ` : "";
  const warnings = survey.warnings && survey.warnings.length
    ? `Warnings: ${survey.warnings.join(" | ")}. `
    : "";
  return `${mode}${warnings}`;
}

function setWifiSurveyEnabled(enabled) {
  document.getElementById("wifiSurveyButton").disabled = !enabled;
}

function setSurveyProgressState(visible, percent = 0, label = "") {
  const wrapper = document.getElementById("wifiSurveyProgress");
  const fill = document.getElementById("wifiSurveyProgressFill");
  const text = document.getElementById("wifiSurveyProgressLabel");
  if (!wrapper || !fill || !text) {
    return;
  }
  wrapper.hidden = !visible;
  fill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  text.textContent = label;
}

function clearSurveyProgressTimer() {
  if (surveyProgressTimer) {
    window.clearInterval(surveyProgressTimer);
    surveyProgressTimer = null;
  }
}

function startSurveyProgressTimer(survey) {
  clearSurveyProgressTimer();
  const startedAt = Number(survey?.scan_started_at_ms || Date.now());
  const estimate = Math.max(1000, Number(survey?.estimated_duration_ms || 26000));

  const render = () => {
    const elapsed = Math.max(0, Date.now() - startedAt);
    const percent = Math.min(96, 8 + ((elapsed / estimate) * 88));
    setSurveyProgressState(true, percent, `Scanning... ${(elapsed / 1000).toFixed(1)}s elapsed`);
  };

  render();
  surveyProgressTimer = window.setInterval(render, 200);
}

function tallyStateInfo(state) {
  switch (Number(state)) {
    case 1:
      return { className: "state-preview" };
    case 2:
      return { className: "state-program" };
    case 3:
      return { className: "state-error" };
    case 0:
    default:
      return { className: "state-off" };
  }
}

function renderTallies(tallies) {
  const grid = document.getElementById("tallyGrid");
  if (!tallies || !tallies.length) {
    grid.innerHTML = "<div class=\"survey-empty\">No tally state reported yet.</div>";
    return;
  }

  grid.innerHTML = tallies.map((state, index) => {
    const stateInfo = tallyStateInfo(state);
    return `
      <div class="tally-tile">
        <strong>Tally ${index + 1}</strong>
        <span class="tally-state ${stateInfo.className}"></span>
      </div>
    `;
  }).join("");
}

function renderChannelSuggestion(suggestion) {
  if (!suggestion) {
    return "";
  }

  const configuredSsidLine = suggestion.configured_ssid && suggestion.configured_ssid_channel
    ? `<div class="survey-meta">Configured SSID "${escapeHtml(suggestion.configured_ssid)}" was observed on channel ${suggestion.configured_ssid_channel}.</div>`
    : "";

  const topChoices = (suggestion.top_non_overlapping || [])
    .map((entry) => `CH ${entry.channel} (${entry.score})`)
    .join(" • ");

  return `
    <div class="survey-item">
      <div>
        <div class="survey-ssid">Suggested 2.4 GHz channel: ${suggestion.recommended_channel}</div>
        <div class="survey-meta">Lower score means less nearby overlap. Best non-overlapping picks: ${topChoices || "n/a"}.</div>
        ${configuredSsidLine}
      </div>
    </div>
  `;
}

function renderSpectralView(survey) {
  if ((!survey.spectral_rows || !survey.spectral_rows.length) && (!survey.spectral_channels || !survey.spectral_channels.length)) {
    return "";
  }

  const recommendedChannel = survey.channel_suggestion ? survey.channel_suggestion.recommended_channel : null;
  const summaries = buildSpectralSummaries(survey);
  const quietest = [...summaries]
    .filter((entry) => entry.sampleCount > 0 || entry.score > 0 || entry.avgMagnitude > 0 || entry.maxMagnitude > 0)
    .sort((left, right) => left.score - right.score || left.channel - right.channel)
    .slice(0, 4)
    .map((entry) => `CH ${entry.channel}`)
    .join(" • ");
  const detailNote = survey.spectral_rows && survey.spectral_rows.length
    ? "The graph uses one contiguous 2.4 GHz frequency axis and overlays individual scan sweeps like speccy."
    : "Detailed FFT rows are unavailable in this capture, so the graph is synthesized from per-channel spectral summaries.";
  const summaryRows = summaries.map((channel) => {
    const recommended = channel.channel === recommendedChannel ? " recommended" : "";
    const energyPct = channel.energyPercent.toFixed(1);
    const scorePct = channel.scorePercent.toFixed(1);
    return `
      <div class="spectral-summary-row${recommended}">
        <div class="spectral-summary-channel">CH ${channel.channel}</div>
        <div class="spectral-summary-bars">
          <div class="spectral-bar-track" title="Average spectral energy">
            <span class="spectral-bar-fill energy" style="width:${energyPct}%"></span>
          </div>
          <div class="spectral-bar-track" title="Combined recommendation score">
            <span class="spectral-bar-fill score" style="width:${scorePct}%"></span>
          </div>
        </div>
        <div class="spectral-summary-metrics">
          <strong>${channel.score}</strong>
          <span>avg ${channel.avgMagnitude}</span>
          <span>peak ${channel.maxMagnitude}</span>
        </div>
      </div>
    `;
  }).join("");

  return `
    <div class="spectral-panel">
      <div class="survey-ssid">Spectral Sweep</div>
      <div class="survey-meta">${detailNote}</div>
      <div class="spectral-legend">
        <span>Recommended: ${recommendedChannel ? `CH ${recommendedChannel}` : "n/a"}</span>
        <span>Quietest by current score: ${quietest || "n/a"}</span>
      </div>
      <div class="spectral-layout">
        <div class="spectral-summary">
          <div class="spectral-summary-head">
            <span>Channel Summary</span>
            <span class="spectral-summary-hint">Top bar = energy, bottom bar = score</span>
          </div>
          <div class="spectral-summary-table">${summaryRows}</div>
        </div>
        <div class="spectral-canvas-wrap">
          <div class="spectral-plot-title">Sweep Overlay</div>
          <canvas id="spectralOverlayCanvas" class="spectral-canvas spectral-canvas-overlay"></canvas>
        </div>
      </div>
    </div>
  `;
}

function buildSpectralSummaries(survey) {
  const scoreMap = new Map((survey.channel_suggestion?.channel_scores || []).map((entry) => [Number(entry.channel), Number(entry.score || 0)]));
  const channelsMap = new Map();
  for (let channel = 1; channel <= 13; channel += 1) {
    channelsMap.set(channel, {
      channel,
      sampleCount: 0,
      avgRSSI: 0,
      avgNoise: 0,
      avgMagnitude: 0,
      maxMagnitude: 0,
      score: scoreMap.get(channel) || 0
    });
  }

  for (const entry of (survey.spectral_channels || [])) {
    const channel = Number(entry.channel);
    if (!channelsMap.has(channel)) {
      continue;
    }
    channelsMap.set(channel, {
      channel,
      sampleCount: Number(entry.sample_count || 0),
      avgRSSI: Number(entry.avg_rssi || 0),
      avgNoise: Number(entry.avg_noise || 0),
      avgMagnitude: Number(entry.avg_magnitude || 0),
      maxMagnitude: Number(entry.max_magnitude || 0),
      score: Number(entry.score || scoreMap.get(channel) || 0)
    });
  }

  const summaries = Array.from(channelsMap.values()).sort((left, right) => left.channel - right.channel);
  const maxMagnitude = summaries.reduce((best, entry) => Math.max(best, entry.maxMagnitude, entry.avgMagnitude), 0) || 1;
  const maxScore = summaries.reduce((best, entry) => Math.max(best, entry.score), 0) || 1;

  return summaries.map((entry) => ({
    ...entry,
    energyPercent: (Math.max(entry.avgMagnitude, entry.maxMagnitude * 0.6) / maxMagnitude) * 100,
    scorePercent: maxScore > 0 ? (entry.score / maxScore) * 100 : 0
  }));
}

function synthesizeRowsFromChannels(channels) {
  const binCount = 64;
  return (channels || []).map((channel) => {
    const center = (binCount - 1) / 2;
    const sigma = binCount / 7;
    const floor = Math.max(1, channel.avg_magnitude || 1);
    const peak = Math.max(floor, channel.max_magnitude || floor);
    const bins = Array.from({ length: binCount }, (_, index) => {
      const distance = index - center;
      const weight = Math.exp(-(distance * distance) / (2 * sigma * sigma));
      return floor + ((peak - floor) * weight);
    });

    return {
      channel: channel.channel,
      bins
    };
  });
}

function spectralColor(value) {
  const clamped = Math.max(0, Math.min(1, value));
  const hue = 220 - (220 * clamped);
  const saturation = 75 + (15 * clamped);
  const lightness = 18 + (42 * clamped);
  return `hsl(${hue} ${saturation}% ${lightness}%)`;
}

function channelCenterMHz(channel) {
  return 2412 + ((Number(channel) - 1) * 5);
}

function spectralBandStartMHz() {
  return 2402;
}

function spectralBandEndMHz() {
  return 2482;
}

function spectralBandWidthMHz() {
  return spectralBandEndMHz() - spectralBandStartMHz();
}

function sourceSpectralRows(survey) {
  return survey.spectral_rows && survey.spectral_rows.length
    ? survey.spectral_rows
    : synthesizeRowsFromChannels(survey.spectral_channels);
}

function rowToBandBins(row, resolution) {
  const bins = row.bins || [];
  const out = Array.from({ length: resolution }, () => 0);
  const counts = Array.from({ length: resolution }, () => 0);
  if (!bins.length) {
    return out;
  }

  const centerMHz = channelCenterMHz(row.channel);
  const startMHz = centerMHz - 10;
  const widthMHz = 20;

  bins.forEach((value, index) => {
    const freqMHz = startMHz + (((index + 0.5) / bins.length) * widthMHz);
    const normalized = (freqMHz - spectralBandStartMHz()) / spectralBandWidthMHz();
    const bucket = Math.max(0, Math.min(resolution - 1, Math.round(normalized * (resolution - 1))));
    out[bucket] += value;
    counts[bucket] += 1;
  });

  for (let index = 0; index < resolution; index += 1) {
    if (counts[index] > 0) {
      out[index] /= counts[index];
    }
  }

  return out;
}

function buildBandEnvelope(rows, resolution) {
  const avg = Array.from({ length: resolution }, () => 0);
  const peak = Array.from({ length: resolution }, () => 0);
  const counts = Array.from({ length: resolution }, () => 0);

  rows.forEach((row) => {
    const projected = rowToBandBins(row, resolution);
    projected.forEach((value, index) => {
      if (value <= 0) {
        return;
      }
      avg[index] += value;
      counts[index] += 1;
      if (value > peak[index]) {
        peak[index] = value;
      }
    });
  });

  for (let index = 0; index < resolution; index += 1) {
    if (counts[index] > 0) {
      avg[index] /= counts[index];
    }
  }

  return { avg, peak };
}

function drawBandGuideLines(ctx, width, height, pad, recommendedChannel) {
  const allChannels = Array.from({ length: 13 }, (_, index) => index + 1);
  const primaryChannels = new Set([1, 6, 11, 13]);
  const plotWidth = width - pad.left - pad.right;
  const plotHeight = height - pad.top - pad.bottom;

  ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textBaseline = "middle";

  allChannels.forEach((channel) => {
    const normalized = (channelCenterMHz(channel) - spectralBandStartMHz()) / spectralBandWidthMHz();
    const x = pad.left + (normalized * plotWidth);
    if (channel === Number(recommendedChannel)) {
      ctx.strokeStyle = "rgba(77,212,176,0.35)";
    } else if (primaryChannels.has(channel)) {
      ctx.strokeStyle = "rgba(255,255,255,0.14)";
    } else {
      ctx.strokeStyle = "rgba(255,255,255,0.05)";
    }
    ctx.beginPath();
    ctx.moveTo(x, pad.top);
    ctx.lineTo(x, pad.top + plotHeight);
    ctx.stroke();

    if (primaryChannels.has(channel) || channel === Number(recommendedChannel)) {
      ctx.fillStyle = channel === Number(recommendedChannel) ? "#9be7d4" : "#93a1b5";
      ctx.fillText(`CH${channel}`, x - 12, height - 12);
    }
  });
}

function drawSpectralView(survey) {
  const overlayCanvas = document.getElementById("spectralOverlayCanvas");
  if (!overlayCanvas) {
    return;
  }

  const summaries = buildSpectralSummaries(survey);
  const rows = sourceSpectralRows(survey);
  drawSpectralOverlay(overlayCanvas, rows, survey.channel_suggestion?.recommended_channel);
}

function drawSpectralOverlay(canvas, rows, recommendedChannel) {
  if (!rows.length) {
    return;
  }

  const dpr = window.devicePixelRatio || 1;
  const width = canvas.clientWidth || canvas.parentElement.clientWidth || 720;
  const pad = { top: 12, right: 14, bottom: 28, left: 8 };
  const plotWidth = width - pad.left - pad.right;
  const resolution = Math.max(240, Math.floor(plotWidth));
  const height = 220;
  const plotHeight = height - pad.top - pad.bottom;
  const envelope = buildBandEnvelope(rows, resolution);
  const maxValue = envelope.peak.reduce((best, value) => Math.max(best, value), 0) || 1;

  canvas.width = Math.floor(width * dpr);
  canvas.height = Math.floor(height * dpr);
  canvas.style.height = `${height}px`;
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);

  ctx.fillStyle = "#0d131b";
  ctx.fillRect(0, 0, width, height);

  drawBandGuideLines(ctx, width, height, pad, recommendedChannel);

  ctx.fillStyle = "rgba(97,182,255,0.12)";
  ctx.beginPath();
  envelope.avg.forEach((value, index) => {
    const x = pad.left + ((index / (resolution - 1)) * plotWidth);
    const y = pad.top + plotHeight - ((value / maxValue) * plotHeight);
    if (index === 0) {
      ctx.moveTo(x, pad.top + plotHeight);
      ctx.lineTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.lineTo(pad.left + plotWidth, pad.top + plotHeight);
  ctx.closePath();
  ctx.fill();

  const traces = rows.map((row) => {
    const projected = rowToBandBins(row, resolution);
    const peak = projected.reduce((best, value) => Math.max(best, value), 0);
    return { projected, peak };
  }).sort((left, right) => left.peak - right.peak);

  traces.forEach((trace) => {
    if (trace.peak <= 0) {
      return;
    }
    const strength = trace.peak / maxValue;
    const strokeAlpha = 0.10 + (strength * 0.24);
    const blue = Math.round(205 + (40 * strength));
    const green = Math.round(185 + (35 * strength));
    ctx.beginPath();
    trace.projected.forEach((value, index) => {
      if (value <= 0) {
        return;
      }
      const x = pad.left + ((index / resolution) * plotWidth);
      const y = pad.top + plotHeight - ((value / maxValue) * plotHeight);
      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.strokeStyle = `rgba(${blue}, ${green}, 255, ${strokeAlpha.toFixed(3)})`;
    ctx.lineWidth = 0.9 + (0.45 * strength);
    ctx.stroke();
  });

  ctx.strokeStyle = "rgba(124,199,255,0.9)";
  ctx.lineWidth = 1.3;
  ctx.beginPath();
  envelope.avg.forEach((value, index) => {
    const x = pad.left + ((index / (resolution - 1)) * plotWidth);
    const y = pad.top + plotHeight - ((value / maxValue) * plotHeight);
    if (index === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();

  ctx.strokeStyle = "#4dd4b0";
  ctx.lineWidth = 1.8;
  ctx.beginPath();
  envelope.peak.forEach((value, index) => {
    const x = pad.left + ((index / (resolution - 1)) * plotWidth);
    const y = pad.top + plotHeight - ((value / maxValue) * plotHeight);
    if (index === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();
}

function renderWifiSurvey(survey) {
  latestWifiSurvey = survey;
  const list = document.getElementById("wifiSurveyList");
  const spectralHtml = renderSpectralView(survey);

  if (survey.error) {
    list.innerHTML = "<div class=\"survey-empty\">Survey failed. Try again in a few seconds.</div>";
    updateWifiSurveyMeta(`Survey failed: ${survey.error}`);
    return;
  }

  if (!survey.networks || !survey.networks.length) {
    list.innerHTML = `${renderChannelSuggestion(survey.channel_suggestion)}${spectralHtml}<div class="survey-empty">No nearby Wi-Fi networks found.</div>`;
    drawSpectralView(survey);
    if (survey.channel_suggestion) {
      updateWifiSurveyMeta(`${surveyMetaPrefix(survey)}Survey complete. Suggested quiet channel: ${survey.channel_suggestion.recommended_channel}.`);
    } else {
      updateWifiSurveyMeta(`${surveyMetaPrefix(survey)}Survey complete. No networks found.`);
    }
    return;
  }

  const networksHtml = survey.networks.map((network) => {
    const ssid = network.hidden || !network.ssid ? "(hidden SSID)" : escapeHtml(network.ssid);
    return `
      <div class="survey-item">
        <div>
          <div class="survey-ssid">${ssid}</div>
          <div class="survey-meta">RSSI ${network.rssi} dBm | Channel ${network.channel} | ${escapeHtml(network.auth_mode)}</div>
        </div>
      </div>
    `;
  }).join("");

  list.innerHTML = `${renderChannelSuggestion(survey.channel_suggestion)}${spectralHtml}${networksHtml}`;
  drawSpectralView(survey);

  if (survey.channel_suggestion) {
    const currentSsid = survey.channel_suggestion.configured_ssid;
    const configuredChannel = survey.channel_suggestion.configured_ssid_channel;
    const configuredSuffix = currentSsid && configuredChannel
      ? ` Configured SSID "${currentSsid}" is on channel ${configuredChannel}.`
      : "";
    updateWifiSurveyMeta(
      `${surveyMetaPrefix(survey)}Survey complete. Found ${survey.network_count} network${survey.network_count === 1 ? "" : "s"}. Suggested quiet channel: ${survey.channel_suggestion.recommended_channel}.${configuredSuffix}`
    );
  } else {
    updateWifiSurveyMeta(`${surveyMetaPrefix(survey)}Survey complete. Found ${survey.network_count} network${survey.network_count === 1 ? "" : "s"}.`);
  }
}

function renderReceivers(receivers) {
  const body = document.getElementById("receiversBody");
  if (!receivers.length) {
    body.innerHTML = "<div class=\"survey-empty\">No receivers reported yet.</div>";
    return;
  }

  const sortedReceivers = [...receivers].sort((left, right) => {
    const leftTallyId = Number(left.tally_id);
    const rightTallyId = Number(right.tally_id);
    const leftSortTallyId = Number.isFinite(leftTallyId) ? leftTallyId : Number.MAX_SAFE_INTEGER;
    const rightSortTallyId = Number.isFinite(rightTallyId) ? rightTallyId : Number.MAX_SAFE_INTEGER;
    if (leftSortTallyId !== rightSortTallyId) {
      return leftSortTallyId - rightSortTallyId;
    }
    return String(left.mac || "").localeCompare(String(right.mac || ""));
  });

  body.innerHTML = sortedReceivers.map((receiver) => {
    const receiverMac = String(receiver.mac || "");
    const receiverTallyId = Number(receiver.tally_id);
    const receiverBrightness = Number(receiver.brightness_percent ?? 100);
    const pendingValue = pendingReceiverSelections.get(receiverMac);
    const pendingBrightness = pendingReceiverBrightness.get(receiverMac);
    const selectedTallyId = pendingValue === undefined ? receiverTallyId : pendingValue;
    const selectedBrightness = pendingBrightness === undefined ? receiverBrightness : pendingBrightness;
    if (pendingValue === receiverTallyId) {
      pendingReceiverSelections.delete(receiverMac);
    }
    if (pendingBrightness === receiverBrightness) {
      pendingReceiverBrightness.delete(receiverMac);
    }

    const options = Array.from({ length: 16 }, (_, idx) => {
      const tallyId = idx + 1;
      const selected = tallyId === selectedTallyId ? "selected" : "";
      return `<option value="${tallyId}" ${selected}>Tally ${tallyId}</option>`;
    }).join("");

    return `
      <article class="receiver-card">
        <div class="receiver-head">
          <div>
            <div class="receiver-title">Tally ${receiverTallyId}</div>
            <div class="receiver-mac">${escapeHtml(receiverMac)}</div>
          </div>
          <span class="pill ${receiver.online ? "ok" : "bad"}">${receiver.online ? "Online" : "Offline"}</span>
        </div>
        <div class="receiver-stats">
          <div class="mini-stat">
            <span>Battery</span>
            <strong>${receiver.battery_percent}%</strong>
          </div>
          <div class="mini-stat">
            <span>Hub RSSI</span>
            <strong>${receiver.hub_rssi}</strong>
          </div>
          <div class="mini-stat">
            <span>Seen</span>
            <strong>${receiver.last_seen_ms_ago} ms</strong>
          </div>
          <div class="mini-stat">
            <span>LED</span>
            <strong>${receiverBrightness}%</strong>
          </div>
        </div>
        <div class="receiver-actions">
          <select id="receiver-${receiverMac}" onchange="rememberReceiverSelection('${receiverMac}')">${options}</select>
          <button class="secondary" type="button" onclick="assignReceiverId('${receiverMac}')">Set ID</button>
          <button class="ghost" type="button" onclick="identifyReceiver('${receiverMac}')">Identify</button>
        </div>
        <div class="receiver-actions receiver-actions-brightness">
          <input id="brightness-${receiverMac}" type="range" min="0" max="100" value="${selectedBrightness}" oninput="rememberReceiverBrightness('${receiverMac}')">
          <span id="brightness-value-${receiverMac}" class="brightness-value">${selectedBrightness}%</span>
          <button class="secondary" type="button" onclick="setReceiverBrightness('${receiverMac}')">Set LED</button>
        </div>
      </article>
    `;
  }).join("");
}

function rememberReceiverSelection(mac) {
  const select = document.getElementById(`receiver-${mac}`);
  if (!select) {
    return;
  }
  pendingReceiverSelections.set(mac, Number(select.value));
}

function rememberReceiverBrightness(mac) {
  const input = document.getElementById(`brightness-${mac}`);
  const label = document.getElementById(`brightness-value-${mac}`);
  if (!input || !label) {
    return;
  }
  const brightness = Number(input.value);
  pendingReceiverBrightness.set(mac, brightness);
  label.textContent = `${brightness}%`;
}

function renderStatus(status) {
  currentStatus = status;

  const modeText = status.mode.config
    ? "Config"
    : status.simulation.enabled ? "Simulation" : "Live";
  const onlineCount = status.receivers.filter((receiver) => receiver.online).length;

  document.getElementById("modeBadge").textContent = modeText;
  document.getElementById("modeMeta").textContent = status.mode.config
    ? "Configuration endpoints are open and writable."
    : status.simulation.enabled
      ? "The hub is generating synthetic tally states."
      : "The hub is forwarding live tally state.";

  document.getElementById("channelValue").textContent = `CH ${status.network.radio_channel || "-"}`;
  document.getElementById("radioLine").textContent =
    `SSID ${status.network.ap_ssid} · AP ${status.network.ap_mac} · Uplink ${status.network.uplink_ip || "unavailable"}`;

  document.getElementById("receiverValue").textContent = `${onlineCount} / ${status.receivers.length}`;
  document.getElementById("receiverSummary").textContent =
    status.receivers.length
      ? `${onlineCount} online, ${status.receivers.length - onlineCount} offline`
      : "No receivers tracked yet.";

  document.getElementById("telemetryValue").textContent = String(status.telemetry.packet_count || 0);
  if (status.telemetry.last_receiver) {
    const telemetry = status.telemetry.last_receiver;
    document.getElementById("telemetryMeta").textContent =
      `Last ${telemetry.mac} · tally ${telemetry.tally_id} · ${telemetry.age_ms} ms ago`;
    document.getElementById("telemetryLine").textContent =
      `Last telemetry: ${telemetry.mac} · tally ${telemetry.tally_id} · hub RSSI ${telemetry.receiver_reported_hub_rssi} · air RSSI ${telemetry.air_rssi}`;
  } else {
    document.getElementById("telemetryMeta").textContent = "No telemetry packets received yet.";
    document.getElementById("telemetryLine").textContent = "Waiting for receiver telemetry...";
  }

  setMessage(status.command.status && status.command.status !== "None" ? status.command.status : "No command queued.");
  document.getElementById("simulationButton").textContent = status.simulation.enabled
    ? "Stop Simulation"
    : "Start Simulation";
  document.getElementById("statusJson").textContent = JSON.stringify(status, null, 2);

  setWifiSurveyEnabled(true);
  renderTallies(status.tallies);
  renderReceivers(status.receivers);

  if (!latestWifiSurvey) {
    updateWifiSurveyMeta("Available on the Linux hub. The dedicated radio may briefly pause tally traffic during a scan.");
  }

  if (!configInitialized) {
    syncWirelessTransportControl(status);
    syncEspNowPhyModeControl(status);
    syncRadioChannelControl(status);
    document.getElementById("atemIp").value = status.switchers.atem.ip || "";
    document.getElementById("vmixIp").value = status.switchers.vmix.ip || "";
    document.getElementById("tslListenAddr").value = status.switchers.tsl.listen_addr || ":9800";
    document.getElementById("obsUrl").value = status.switchers.obs.url || "";
    document.getElementById("obsPassword").value = "";
    setSelectedSwitcherSource(deriveSelectedSwitcherSource(status));
    configInitialized = true;
  } else {
    syncWirelessTransportControl(status);
    syncEspNowPhyModeControl(status);
    syncRadioChannelControl(status);
  }
}

async function refreshStatus() {
  try {
    const status = await api("/api/v1/status");
    renderStatus(status);
  } catch (error) {
    setMessage(`Status refresh failed: ${error.message}`, "danger");
  }
}

async function refreshSurveyState() {
  try {
    const survey = await api("/api/v1/wifi/survey");
    handleSurveyUpdate(survey);
  } catch (error) {
    updateWifiSurveyMeta(`Survey refresh failed: ${error.message}`);
  }
}

async function refreshAll() {
  await Promise.allSettled([refreshStatus(), refreshSurveyState()]);
}

function startPollingStatus() {
  if (pollTimer) {
    return;
  }
  pollTimer = window.setInterval(() => {
    void refreshAll();
  }, 2000);
}

function stopPollingStatus() {
  if (!pollTimer) {
    return;
  }
  window.clearInterval(pollTimer);
  pollTimer = null;
}

function handleSurveyUpdate(survey) {
  latestWifiSurvey = survey;

  if (survey.scanning) {
    setWifiSurveyEnabled(false);
    updateWifiSurveyMeta("Scanning nearby Wi-Fi networks. The dedicated radio may briefly pause tally traffic.");
    startSurveyProgressTimer(survey);
    return;
  }

  clearSurveyProgressTimer();
  setWifiSurveyEnabled(true);
  if (survey.scan_started_at_ms) {
    const elapsed = Math.max(0, Date.now() - Number(survey.scan_started_at_ms));
    setSurveyProgressState(true, 100, `Scan complete in ${(elapsed / 1000).toFixed(1)}s`);
    window.setTimeout(() => {
      if (latestWifiSurvey === survey && !survey.scanning) {
        setSurveyProgressState(false);
      }
    }, 1200);
  } else {
    setSurveyProgressState(false);
  }
  renderWifiSurvey(survey);
}

function connectStatusEvents() {
  if (!window.EventSource) {
    startPollingStatus();
    return;
  }

  if (statusEvents) {
    statusEvents.close();
  }

  const stream = new EventSource("/api/v1/events");
  statusEvents = stream;

  stream.onopen = () => {
    if (statusEvents === stream) {
      stopPollingStatus();
    }
  };

  stream.addEventListener("status", (event) => {
    try {
      renderStatus(JSON.parse(event.data));
    } catch (error) {
      console.error("Failed to decode status event", error);
    }
  });

  stream.addEventListener("survey", (event) => {
    try {
      handleSurveyUpdate(JSON.parse(event.data));
    } catch (error) {
      console.error("Failed to decode survey event", error);
    }
  });

  stream.onerror = () => {
    if (statusEvents !== stream) {
      return;
    }
    stream.close();
    statusEvents = null;
    startPollingStatus();
    if (!currentStatus) {
      setMessage("Live updates disconnected. Waiting for reconnect...", "danger");
    }
  };
}

async function runWifiSurvey() {
  setWifiSurveyEnabled(false);
  setSurveyProgressState(true, 2, "Preparing scan...");
  updateWifiSurveyMeta("Starting Wi-Fi survey...");
  try {
    const queued = await api("/api/v1/wifi/survey", { method: "POST" });
    if (queued?.survey) {
      handleSurveyUpdate(queued.survey);
    } else {
      updateWifiSurveyMeta("Scanning nearby Wi-Fi networks. The dedicated radio may briefly pause tally traffic.");
    }
  } catch (error) {
    setSurveyProgressState(false);
    setWifiSurveyEnabled(true);
    updateWifiSurveyMeta(`Survey failed: ${error.message}`);
  }
}

async function assignReceiverId(mac) {
  const select = document.getElementById(`receiver-${mac}`);
  pendingReceiverSelections.set(mac, Number(select.value));
  try {
    const result = await api("/api/v1/receivers/assign-id", {
      method: "POST",
      body: JSON.stringify({ mac, tally_id: Number(select.value) })
    });
    setMessage(result.message || "Receiver command queued.", "success");
  } catch (error) {
    setMessage(`Assign ID failed: ${error.message}`, "danger");
  }
}

async function setReceiverBrightness(mac) {
  const input = document.getElementById(`brightness-${mac}`);
  if (!input) {
    return;
  }
  const brightnessPercent = Number(input.value);
  pendingReceiverBrightness.set(mac, brightnessPercent);
  try {
    const result = await api("/api/v1/receivers/set-brightness", {
      method: "POST",
      body: JSON.stringify({ mac, brightness_percent: brightnessPercent })
    });
    setMessage(result.message || "Brightness update queued.", "success");
  } catch (error) {
    setMessage(`Set brightness failed: ${error.message}`, "danger");
  }
}

async function identifyReceiver(mac) {
  try {
    const result = await api("/api/v1/receivers/identify", {
      method: "POST",
      body: JSON.stringify({ mac })
    });
    setMessage(result.message || "Identify command queued.", "success");
  } catch (error) {
    setMessage(`Identify failed: ${error.message}`, "danger");
  }
}

async function toggleSimulation() {
  if (!currentStatus) {
    return;
  }

  try {
    const result = await api("/api/v1/simulation", {
      method: "POST",
      body: JSON.stringify({ enabled: !currentStatus.simulation.enabled })
    });
    setMessage(result.message || "Simulation updated.", "success");
  } catch (error) {
    setMessage(`Simulation update failed: ${error.message}`, "danger");
  }
}

document.getElementById("configForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const switcherSource = getSelectedSwitcherSource();

  const payload = {
    wireless_transport: document.getElementById("wirelessTransport").value,
    radio_channel: Number(document.getElementById("radioChannel").value),
    atem_ip: document.getElementById("atemIp").value,
    vmix_ip: document.getElementById("vmixIp").value,
    tsl_listen_addr: document.getElementById("tslListenAddr").value,
    obs_url: document.getElementById("obsUrl").value,
    atem_enabled: switcherSource === "atem",
    vmix_enabled: switcherSource === "vmix",
    tsl_enabled: switcherSource === "tsl",
    obs_enabled: switcherSource === "obs"
  };
  const espNowPhyField = document.getElementById("espnowPhyModeField");
  const espNowPhySelect = document.getElementById("espnowPhyMode");
  if (espNowPhyField && !espNowPhyField.hidden && espNowPhySelect) {
    payload.espnow_phy_mode = espNowPhySelect.value;
  }
  const obsPassword = document.getElementById("obsPassword").value;
  if (switcherSource === "obs" && obsPassword) {
    payload.obs_password = obsPassword;
  }

  try {
    const result = await api("/api/v1/config", {
      method: "PUT",
      body: JSON.stringify(payload)
    });
    setMessage(result.message || "Configuration saved.", "success");
  } catch (error) {
    setMessage(`Config update failed: ${error.message}`, "danger");
  }
});

document.getElementById("simulationButton").addEventListener("click", toggleSimulation);
document.getElementById("refreshButton").addEventListener("click", async () => {
  await refreshStatus();
  await refreshSurveyState();
});
document.getElementById("wifiSurveyButton").addEventListener("click", runWifiSurvey);
document.getElementById("wirelessTransport").addEventListener("change", async (event) => {
  await updateWirelessTransport(event.target.value);
});
document.getElementById("espnowPhyMode").addEventListener("change", async (event) => {
  await updateEspNowPhyMode(event.target.value);
});
document.getElementById("radioChannel").addEventListener("change", async (event) => {
  await updateRadioChannel(event.target.value);
});
document.querySelectorAll('input[name="switcherSource"]').forEach((radio) => {
  radio.addEventListener("change", applySwitcherSourceState);
});

applySwitcherSourceState();
void refreshAll();
connectStatusEvents();
