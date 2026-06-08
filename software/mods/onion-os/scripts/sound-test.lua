-- Sound module test (NS4168 amp + SPH0641 PDM mic) — upstream onion-os API.
-- Runs ONE sweep of the three port variants, shows results on the e-paper,
-- then returns. No script-side button loop, so it cannot get stuck:
-- after it finishes, press any button to dismiss back to the home screen.
--
-- The Sound module is on the RIGHT port (J10) = variant R, so watch the "R"
-- line: a working module shows a LOW rms (and you hear the tone); dead or
-- unwired pins rail near 30000.
--
-- Ported to upstream firmware: the old local onion.tone/onion.mic_rms/onion.show
-- bindings no longer exist. This uses the upstream module API:
--   speaker: sound_speaker_begin{bclk,ws,dout,ctrl} / sound_play_tone / sound_speaker_end
--   mic:     sound_mic_begin{clk,din,ws,ctrl} / sound_mic_level(ms)->{rms,..} / sound_mic_end
--   text:    display_begin/display_text/display_commit (no onion.show upstream)
-- Only one module may be active at a time, so each variant ends speaker before
-- starting the mic, and ends the mic before the next variant.

if not onion.sound_speaker_begin then
  error("sound-test needs the onion-os sound_* API (upstream firmware)")
end

-- Firmware variant pin lines are {MicData/DIN, CLK/BCLK, WS, AudioOut/DOUT, CTRL}.
local variants = {
  { name = "L1", din = 48, bclk = 47, ws = 19, dout = 42, ctrl = 41 },
  { name = "L2", din = 40, bclk = 41, ws = 42, dout = 19, ctrl = 47 },
  { name = "R",  din = 38, bclk = 39, ws = 16, dout = 15, ctrl = 7  },
}

-- Multi-line text on the e-paper in one batched refresh (replaces onion.show).
local function show(text)
  onion.display_begin()
  onion.display_text("", 0, 0, { clear = true })   -- wipe canvas to white
  local y = 20
  for line in (text .. "\n"):gmatch("(.-)\n") do
    onion.display_text(line, 6, y, { clear = false })
    y = y + 16
  end
  onion.display_commit()
end

-- Release any module left active by a previous run (no-ops if nothing is on).
onion.sound_speaker_end()
onion.sound_mic_end()

show("Sound test\n\nrunning sweep...\nlisten for a beep")
onion.log("Sound test: starting sweep")

local results = {}
for _, v in ipairs(variants) do
  -- Speaker: a 1 kHz tone you should hear if the amp + I2S path works.
  onion.log(v.name .. ": tone (listen)")
  local ok, err = onion.sound_speaker_begin({ bclk = v.bclk, ws = v.ws, dout = v.dout, ctrl = v.ctrl })
  if ok then
    onion.sound_play_tone(1000, 1200, 0.6)
    onion.sound_speaker_end()
  else
    onion.log(v.name .. ": speaker err: " .. tostring(err))
  end
  onion.sleep(120)

  -- Mic: measure RMS. Dead/unwired PDM data rails high (~30000); live is low.
  local value, verdict = 0, "err"
  local mok, merr = onion.sound_mic_begin({ clk = v.bclk, din = v.din, ws = v.ws, ctrl = v.ctrl })
  if mok then
    local lvl = onion.sound_mic_level(1000)
    onion.sound_mic_end()
    if lvl and lvl.rms then
      value = math.floor(lvl.rms)
      verdict = (lvl.rms < 8000) and "LIVE!" or "dead"
    end
  else
    onion.log(v.name .. ": mic err: " .. tostring(merr))
  end

  local line = string.format("%-2s rms %5d %s", v.name, value, verdict)
  results[#results + 1] = line
  onion.log(line)
end

-- Show results and return; firmware keeps the image up until any key is pressed,
-- then dismisses to home. Nothing here can hang.
show("SOUND TEST DONE\n\n" .. table.concat(results, "\n")
  .. "\n\n(R = your port)\npress any key")
onion.log("Sweep done")
