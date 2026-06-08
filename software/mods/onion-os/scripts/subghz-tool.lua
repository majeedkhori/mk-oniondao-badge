-- OnionGHz — a Flipper-style sub-GHz playground for the CC1101 side module.
-- Interactive menu: Receive monitor, Transmit beacon, RSSI meter, Spectrum,
-- Find Signal, plus live Frequency / Modulation control and a Radio-info screen.
--
-- WHAT THIS RADIO CAN AND CAN'T DO
-- The firmware drives the CC1101 in variable-length PACKET mode with a fixed
-- sync word (0xD391) + CRC at ~2.4 kbps. So packet RX/TX is badge<->badge only:
--   * Receive monitor / Transmit beacon decode/send onion packets — run TX on
--     one badge and RX on another (same freq + modulation) for range tests.
--   * They will NOT decode car fobs / weather stations / Flipper captures.
-- BUT the RSSI features measure raw carrier ENERGY regardless of modulation, so
-- they DO sense real transmitters:
--   * RSSI meter  — live signal strength on the current frequency.
--   * Spectrum    — RSSI bars across the common bands.
--   * Find Signal — sweeps bands and reports the strongest carrier. Hold a fob/
--     remote near the badge and it tells you roughly which band it's on.
--   (Requires the onion.subghz_rssi binding in firmware.)
--
-- Controls: UP/DOWN move, LEFT/RIGHT adjust, SELECT choose, CANCEL back/stop.

if not onion.subghz_begin then
  error("OnionGHz needs the onion-os subghz_* API (upstream firmware)")
end
local HAS_RSSI = onion.subghz_rssi ~= nil

-- Radio is on the LEFT port = variant L1 (verified by subghz-test).
local PINS = { mosi = 48, sck = 47, cs = 19, miso = 42, gdo0 = 41 }

-- Common ISM presets (all within the CC1101's 300-348 / 387-464 / 779-928 bands).
local FREQS = { 300.00, 303.875, 310.00, 315.00, 318.00, 390.00,
                418.00, 433.42, 433.92, 434.42, 868.35, 915.00 }
-- A trimmed set for the spectrum view so the bars fit on one screen.
local SPECTRUM = { 300.00, 315.00, 318.00, 390.00, 433.92, 868.35, 915.00 }
local MODS  = { "ook", "gfsk", "2fsk", "msk" }   -- ook/ask is most common on 433

local fi  = 9       -- index into FREQS (433.92)
local mi  = 1       -- index into MODS  (ook)
local function freq() return FREQS[fi] end
local function mod()  return MODS[mi]  end

local sz = onion.display_size()
local W, H = sz.width, sz.height

-- ── input ────────────────────────────────────────────────────────────────────
local KEYS = { "up", "down", "left", "right", "select", "cancel" }
local function poll()                 -- first held button, or nil (non-blocking)
  local b = onion.buttons()
  for _, k in ipairs(KEYS) do if b[k] then return k end end
  return nil
end
local function drain() while poll() do onion.sleep(10) end end  -- wait for release
local function read_key()             -- blocking, debounced single press
  drain()
  local k
  repeat k = poll(); onion.sleep(15) until k
  drain()
  return k
end

-- ── display ──────────────────────────────────────────────────────────────────
local function draw(title, lines, footer)
  onion.display_begin()
  onion.display_text(title, 6, 16, { clear = true, font = "bold" })
  onion.display_line(0, 22, W, 22)
  local y = 38
  for _, ln in ipairs(lines) do
    onion.display_text(ln, 6, y, { clear = false })
    y = y + 15
  end
  if footer then onion.display_text(footer, 6, H - 6, { clear = false }) end
  onion.display_commit()
end

local function tohex(s, maxbytes)
  local t, n = {}, math.min(#s, maxbytes or #s)
  for i = 1, n do t[i] = string.format("%02X", s:byte(i)) end
  return table.concat(t, " ") .. (#s > n and " .." or "")
end

-- Map an RSSI dBm value (~ -110 quiet .. -30 strong) to a text bar.
local function bar(dbm, width)
  local frac = (dbm + 110) / 80
  if frac < 0 then frac = 0 elseif frac > 1 then frac = 1 end
  local n = math.floor(frac * width + 0.5)
  return string.rep("#", n) .. string.rep(".", width - n)
end

-- ── radio ────────────────────────────────────────────────────────────────────
local function radio_start()
  onion.subghz_end()
  onion.sleep(15)
  local ok, err = onion.subghz_begin({
    freq = freq(), modulation = mod(),
    mosi = PINS.mosi, sck = PINS.sck, cs = PINS.cs, miso = PINS.miso, gdo0 = PINS.gdo0,
  })
  return ok, err
end

-- ── packet modes ─────────────────────────────────────────────────────────────
local function mode_receive()
  onion.subghz_set_frequency(freq())
  local log, count = {}, 0
  local function render()
    local lines = {
      string.format("%.3f MHz  %s", freq(), mod():upper()),
      string.format("RX: %d packet(s)", count),
      "",
    }
    for i = #log, math.max(1, #log - 5), -1 do lines[#lines + 1] = log[i] end
    if count == 0 then lines[#lines + 1] = "listening..." end
    draw("RECEIVE", lines, "CANCEL = back")
  end
  render()
  while true do
    local pkt = onion.subghz_receive(400)
    if pkt then
      count = count + 1
      log[#log + 1] = string.format("%4ddBm %2dB %s", pkt.rssi_dbm, pkt.len, tohex(pkt.payload, 6))
      render()
    end
    if poll() == "cancel" then drain(); return end
  end
end

local function mode_transmit()
  onion.subghz_set_frequency(freq())
  local n, errs = 0, 0
  local function render(last)
    draw("TX BEACON", {
      string.format("%.3f MHz  %s", freq(), mod():upper()),
      string.format("sent: %d   errs: %d", n, errs),
      "last: " .. (last or "-"),
      "",
      "transmitting...",
    }, "CANCEL = stop")
  end
  render()
  while true do
    n = n + 1
    local payload = "ONION-" .. n
    local ok, err = onion.subghz_transmit(payload)
    if not ok then errs = errs + 1; payload = "ERR:" .. tostring(err) end
    if n % 5 == 0 or not ok then render(payload) end   -- limit e-ink redraws
    for _ = 1, 8 do                                     -- ~400ms, cancel-aware
      if poll() == "cancel" then drain(); return end
      onion.sleep(50)
    end
  end
end

-- ── RSSI / spectrum modes (need subghz_rssi) ─────────────────────────────────
local function mode_rssi()
  onion.subghz_set_frequency(freq())
  while true do
    local dbm = onion.subghz_rssi()
    draw("RSSI METER", {
      string.format("%.3f MHz  %s", freq(), mod():upper()),
      "",
      string.format("  %4d dBm", dbm),
      "  [" .. bar(dbm, 16) .. "]",
    }, "CANCEL = back")
    if poll() == "cancel" then drain(); return end
  end
end

local function mode_spectrum()
  while true do
    local lines = {}
    for _, f in ipairs(SPECTRUM) do
      onion.subghz_set_frequency(f)
      local dbm = onion.subghz_rssi(2)
      lines[#lines + 1] = string.format("%6.2f %4d %s", f, dbm, bar(dbm, 8))
    end
    draw("SPECTRUM", lines, "CANCEL = back")
    onion.subghz_set_frequency(freq())   -- leave current freq selected
    if poll() == "cancel" then drain(); return end
  end
end

local function mode_find()
  local best_f, best_d = nil, -999
  for pass = 1, 4 do
    for _, f in ipairs(FREQS) do
      onion.subghz_set_frequency(f)
      local dbm = onion.subghz_rssi(2)
      if dbm > best_d then best_d, best_f = dbm, f end
    end
    draw("FIND SIGNAL", {
      "Hold a transmitter",
      "near the badge...",
      "",
      string.format("pass %d/4", pass),
      best_f and string.format("peak %.3f  %ddBm", best_f, best_d) or "",
    }, "CANCEL = back")
    if poll() == "cancel" then drain(); break end
  end
  onion.subghz_set_frequency(freq())
  draw("FIND SIGNAL", {
    "Strongest carrier:",
    "",
    best_f and string.format("   %.3f MHz", best_f) or "   none found",
    best_f and string.format("   %d dBm", best_d) or "",
  }, "any key: back")
  read_key()
end

-- ── settings / info ──────────────────────────────────────────────────────────
local function mode_set_freq()
  while true do
    draw("FREQUENCY", {
      "",
      string.format("   %.3f MHz", freq()),
      "",
      string.format("   (%d of %d presets)", fi, #FREQS),
    }, "LEFT/RIGHT change  SEL ok")
    local k = read_key()
    if k == "left"  then fi = (fi - 2) % #FREQS + 1
    elseif k == "right" then fi = fi % #FREQS + 1
    elseif k == "select" then onion.subghz_set_frequency(freq()); return
    elseif k == "cancel" then return end
  end
end

local function mode_set_mod()
  while true do
    local lines = { "" }
    for i, m in ipairs(MODS) do
      lines[#lines + 1] = (i == mi and "> " or "  ") .. m:upper()
    end
    draw("MODULATION", lines, "UP/DN pick  SEL apply")
    local k = read_key()
    if k == "up" then mi = (mi - 2) % #MODS + 1
    elseif k == "down" then mi = mi % #MODS + 1
    elseif k == "select" then
      local ok, err = radio_start()
      if not ok then draw("MODULATION", { "begin failed:", tostring(err) }, "any key"); read_key() end
      return
    elseif k == "cancel" then return end
  end
end

local function mode_info()
  local i = onion.subghz_info() or {}
  draw("RADIO INFO", {
    "variant : " .. tostring(i.variant),
    "active  : " .. tostring(i.active),
    string.format("freq    : %.3f MHz", i.frequency or freq()),
    string.format("version : 0x%02X", i.version or 0),
    string.format("partnum : 0x%02X", i.partnum or 0),
    "rssi api: " .. (HAS_RSSI and "yes" or "NO"),
  }, "any key: back")
  read_key()
end

-- ── main menu ────────────────────────────────────────────────────────────────
local ITEMS = {
  { "Receive monitor", mode_receive },
  { "Transmit beacon", mode_transmit },
  { "RSSI meter",      HAS_RSSI and mode_rssi },
  { "Spectrum",        HAS_RSSI and mode_spectrum },
  { "Find Signal",     HAS_RSSI and mode_find },
  { "Frequency",       mode_set_freq },
  { "Modulation",      mode_set_mod },
  { "Radio info",      mode_info },
  { "Exit",            nil },
}

local ok, err = radio_start()
if not ok then
  draw("OnionGHz", { "Radio init failed:", tostring(err), "", "Check the CC1101 module" }, "any key: exit")
  read_key()
  onion.release_display()
  return
end

local sel = 1
while true do
  local lines = {}
  for i, it in ipairs(ITEMS) do
    local tag = (i == sel and "> " or "  ")
    if it[2] == false then tag = "  " end   -- disabled (no rssi binding)
    lines[i] = tag .. it[1] .. (it[2] == false and " (n/a)" or "")
  end
  draw(string.format("OnionGHz %.2f %s", freq(), mod():upper()), lines, "UP/DN SEL  CXL=exit")
  local k = read_key()
  if k == "up" then sel = (sel - 2) % #ITEMS + 1
  elseif k == "down" then sel = sel % #ITEMS + 1
  elseif k == "cancel" then break
  elseif k == "select" then
    local fn = ITEMS[sel][2]
    if fn == nil then break end          -- Exit
    if fn ~= false then fn() end         -- skip disabled entries
  end
end

onion.subghz_end()
onion.release_display()
onion.log("OnionGHz exit")
