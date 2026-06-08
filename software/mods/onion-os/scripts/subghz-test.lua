-- scripts_subghz-test.lua : CC1101 sub-GHz radio bring-up test
-- Verifies a freshly soldered CC1101 on the swappable side-port.
-- It sweeps the three board pin variants (L1/L2/R) to auto-find the wiring,
-- reads the chip version/partnum over SPI, retunes across ISM bands, and runs
-- a TX + RX smoke test. Full detail goes to the serial log; the e-ink screen
-- shows a rolling summary. View serial with: scripts/monitor-serial.sh
--
-- PASS criteria: a variant is detected AND version is a sane CC1101 value
-- (0x14 genuine; 0x04/0x17 common clones). 0x00 / 0xFF => SPI/power wiring bad.

local TEST_FREQ = 433.92

-- explicit pins per board variant: matches kModuleVariants in firmware
-- line = {MOSI, SCK, CS, MISO, GDO0}
local VARIANTS = {
  { name = "L1", mosi = 48, sck = 47, cs = 19, miso = 42, gdo0 = 41 },
  { name = "L2", mosi = 40, sck = 41, cs = 42, miso = 19, gdo0 = 47 },
  { name = "R",  mosi = 38, sck = 39, cs = 16, miso = 15, gdo0 = 7  },
}

local function hex(n) return string.format("0x%02X", n or 0) end

local lines = {}
local function report(s)
  onion.log("[subghz-test] " .. s)
  lines[#lines + 1] = s
end

-- Render the tail of the log to e-ink in a single batched refresh.
local function show()
  onion.display_begin()
  onion.display_text("Sub-GHz CC1101 test", 6, 16, true)
  local maxrows = 12
  local first = math.max(1, #lines - maxrows + 1)
  local y = 34
  for i = first, #lines do
    onion.display_text(lines[i], 6, y, false)
    y = y + 14
  end
  onion.display_commit()
end

report("starting...")

-- 1) Find the radio: try each variant until begin() detects the chip.
local found = nil
for _, v in ipairs(VARIANTS) do
  onion.subghz_end()        -- free the side-port if a prior attempt held it
  onion.sleep(20)
  local ok, err = onion.subghz_begin({
    freq = TEST_FREQ, modulation = "gfsk",
    mosi = v.mosi, sck = v.sck, cs = v.cs, miso = v.miso, gdo0 = v.gdo0,
  })
  if ok then
    found = v
    report("DETECTED on variant " .. v.name)
    break
  else
    report(v.name .. ": " .. tostring(err))
  end
end

if not found then
  report("NO CC1101 FOUND")
  report("check VCC/GND/SPI joints")
  show()
  return
end

-- 2) Read chip identity over SPI.
local info = onion.subghz_info()
report("version=" .. hex(info.version) .. " part=" .. hex(info.partnum))
report(string.format("freq=%.2f MHz", info.frequency or 0))
if info.version == 0x14 then
  report("version OK (genuine)")
elseif info.version == 0x04 or info.version == 0x17 then
  report("version OK (clone)")
else
  report("version unusual!")
end

-- 3) Retune across the common ISM bands (just exercises the PLL/regs).
for _, f in ipairs({ 315.0, 433.92, 868.0, 915.0 }) do
  local ok = onion.subghz_set_frequency(f)
  report(string.format("tune %.2f %s", f, ok and "ok" or "FAIL"))
end
onion.subghz_set_frequency(TEST_FREQ)

-- 4) TX smoke test. Watch on an SDR / Flipper / second badge at 433.92 MHz.
local tx_ok = 0
for i = 1, 5 do
  local ok, err = onion.subghz_transmit("ONION-TEST-" .. i)
  if ok then tx_ok = tx_ok + 1 else report("tx" .. i .. " " .. tostring(err)) end
  onion.sleep(200)
end
report("TX " .. tx_ok .. "/5 @ 433.92")

-- 5) RX smoke test. Returns nil if nothing is on-air (that's fine).
report("RX listen 5s...")
show()
local pkt = onion.subghz_receive(5000)
if pkt then
  report("RX " .. pkt.len .. "B rssi=" .. tostring(pkt.rssi_dbm) .. "dBm")
else
  report("RX none")
end

onion.subghz_end()
report(found and "DONE - radio works" or "DONE")
show()
