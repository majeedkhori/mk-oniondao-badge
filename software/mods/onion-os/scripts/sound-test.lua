-- Sound module test (NS4168 amp + SPH0641 PDM mic).
-- Runs ONE sweep of the three port variants, shows results on the e-paper,
-- then returns. No script-side button loop, so it cannot get stuck:
-- after it finishes, press any button to dismiss back to the home screen.
--
-- Your module is on the RIGHT port (J10) = variant R, so watch the "R" line:
-- a working module shows a LOW rms (and you hear the tone); dead pins rail
-- near 30000.

local variants = {
    { name = "L1", mic = 48, bclk = 47, ws = 19, sdo = 42, ctrl = 41 },
    { name = "L2", mic = 40, bclk = 41, ws = 42, sdo = 19, ctrl = 47 },
    { name = "R",  mic = 38, bclk = 39, ws = 16, sdo = 15, ctrl = 7  },
}

-- immediate proof-of-life: screen clears to this within a second of SELECT
onion.show("Sound test\n\nrunning sweep...\nlisten for a beep")
onion.log("Sound test: starting sweep")

local results = {}
for _, v in ipairs(variants) do
    onion.log(v.name .. ": tone (listen)")
    onion.tone(1000, 1500, v.bclk, v.ws, v.sdo, v.ctrl)
    onion.sleep(150)

    local rms, peak = onion.mic_rms(1500, v.bclk, v.ws, v.mic)
    local verdict, value
    if rms == false then
        verdict, value = "err", 0
    else
        value = math.floor(rms)
        verdict = (rms < 8000) and "LIVE!" or "dead"
    end

    local line = string.format("%-2s rms %5d %s", v.name, value, verdict)
    results[#results + 1] = line
    onion.log(line)
end

-- show results and return; firmware keeps the image up until any key is
-- pressed, then dismisses to home. Nothing here can hang.
onion.show("SOUND TEST DONE\n\n" .. table.concat(results, "\n")
    .. "\n\n(R = your port)\npress any key")
onion.log("Sweep done")
