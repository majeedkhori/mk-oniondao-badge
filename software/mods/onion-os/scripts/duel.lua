-- ESP-Duel (onion-os Lua) — vs CPU, graphical. Best-of-3, energy/SPECIAL,
-- difficulty levels, persistent stats, move icons + clash screen.
--
-- Controls: LEFT=SLASH  UP=SHIELD  RIGHT=BLAST  SELECT=SPECIAL (when charged).
--
-- WIRELESS LATER: opponents are pluggable. The CPU pick is one function;
-- a future badge_opponent (ESP-NOW commit-reveal) returns a move the same way
-- and play_duel() stays unchanged.

onion.log("ESP-Duel start")

local W, H = 264, 176

-- dmg[you][opp] = { hp you lose, hp opponent loses }
local dmg = {
    SLASH  = { SLASH = {30,30}, SHIELD = {10,0}, BLAST = {20,30} },
    SHIELD = { SLASH = {0,10},  SHIELD = {0,0},  BLAST = {0,0}  },
    BLAST  = { SLASH = {30,20}, SHIELD = {0,0},  BLAST = {20,20} },
}

local hist = { SLASH = 0, SHIELD = 0, BLAST = 0, SPECIAL = 0 }
local g_entropy = 1

local function wait_button()
    local b = onion.buttons()
    while b.left or b.right or b.up or b.down or b.select or b.cancel do
        g_entropy = g_entropy + 1; onion.sleep(40); b = onion.buttons()
    end
    while true do
        g_entropy = g_entropy + 1; b = onion.buttons()
        if b.left   then return "left"   end
        if b.up     then return "up"     end
        if b.right  then return "right"  end
        if b.down   then return "down"   end
        if b.select then return "select" end
        if b.cancel then return "cancel" end
        onion.sleep(40)
    end
end

local function wait_sel_cxl()
    while true do
        local k = wait_button()
        if k == "select" then return "select" end
        if k == "cancel" then return "cancel" end
    end
end

-- ---------- drawing helpers ----------
local function titlebar(text)
    onion.gfx_rect(0, 0, W, 24, true)
    onion.gfx_text(6, 17, text, 1, true)
end

local function hpbar(label, top, hp)
    onion.gfx_text(4, top + 13, label, 1)
    local bx, bw = 50, 168
    onion.gfx_rect(bx, top, bw, 16, false)
    local innerw = math.floor((bw - 4) * hp / 100)
    if innerw > 0 then onion.gfx_rect(bx + 2, top + 2, innerw, 12, true) end
    onion.gfx_text(bx + bw + 4, top + 13, tostring(hp), 1)
end

local function energy_pips(top, energy)
    onion.gfx_text(4, top + 9, "EN", 1)
    local px = 34
    for i = 1, 5 do
        if i <= energy then onion.gfx_rect(px, top, 10, 10, true)
        else onion.gfx_rect(px, top, 10, 10, false) end
        px = px + 13
    end
    if energy >= 3 then onion.gfx_text(px + 6, top + 9, "SPECIAL!", 1) end
end

-- move icons, centered at (cx,cy), scaled to radius r (all integer math)
local function icon_slash(cx, cy, r)
    onion.gfx_line(cx-r, cy+r, cx+r, cy-r)
    onion.gfx_line(cx-r+2, cy+r, cx+r+2, cy-r+2)
    onion.gfx_line(cx-r-1, cy+r-3, cx-r+5, cy+r+3)   -- hilt
end
local function icon_shield(cx, cy, r)
    local pb = cy + r + 2
    onion.gfx_line(cx-r, cy-r, cx+r, cy-r)           -- top edge
    onion.gfx_line(cx-r, cy-r, cx-r, cy)             -- left side
    onion.gfx_line(cx+r, cy-r, cx+r, cy)             -- right side
    onion.gfx_line(cx-r, cy, cx, pb)                 -- to bottom point
    onion.gfx_line(cx+r, cy, cx, pb)
    onion.gfx_line(cx, cy-r+3, cx, cy+r-2)           -- cross
    onion.gfx_line(cx-r+4, cy-2, cx+r-4, cy-2)
end
local function icon_blast(cx, cy, r)
    local d = math.floor(r * 3 / 4)
    onion.gfx_line(cx, cy-r, cx, cy+r)
    onion.gfx_line(cx-r, cy, cx+r, cy)
    onion.gfx_line(cx-d, cy-d, cx+d, cy+d)
    onion.gfx_line(cx-d, cy+d, cx+d, cy-d)
    onion.gfx_circle(cx, cy, math.max(2, math.floor(r/3)), true)
end
local function icon_special(cx, cy, r)
    local h = math.floor(r/3)
    onion.gfx_triangle(cx, cy-r, cx-r, cy+h, cx+r, cy+h, true)
    onion.gfx_triangle(cx, cy+r, cx-r, cy-h, cx+r, cy-h, true)
end
local function draw_icon(move, cx, cy, r)
    if move == "SLASH"   then icon_slash(cx, cy, r)
    elseif move == "SHIELD" then icon_shield(cx, cy, r)
    elseif move == "BLAST"  then icon_blast(cx, cy, r)
    elseif move == "SPECIAL" then icon_special(cx, cy, r) end
end

local function centered(name, boxx, boxw)
    return boxx + math.floor((boxw - #name * 11) / 2)
end

local function draw_pick(round, you, opp, en_you, sy, sc)
    onion.gfx_clear()
    titlebar("DUEL R" .. round .. "        SET " .. sy .. "-" .. sc)
    hpbar("YOU", 28, you)
    hpbar("CPU", 50, opp)
    energy_pips(70, en_you)
    local boxes = {{2,"L","SLASH"}, {92,"U","SHIELD"}, {182,"R","BLAST"}}
    for _, b in ipairs(boxes) do
        local x = b[1]
        onion.gfx_rect(x, 86, 80, 60, false)
        onion.gfx_text(x + 35, 99, b[2], 1)             -- key letter (top)
        draw_icon(b[3], x + 40, 116, 11)                -- icon (box center, smaller)
        onion.gfx_text(centered(b[3], x, 80), 143, b[3], 1)  -- name (bottom)
    end
    if en_you >= 3 then
        onion.gfx_text(4, 162, "SELECT=SPECIAL  CXL=quit", 1)
    else
        onion.gfx_text(4, 162, "CANCEL = forfeit", 1)
    end
    onion.gfx_show()
end

local function draw_clash(round, mine, theirs, you_take, cpu_take, you, opp)
    onion.gfx_clear()
    titlebar("ROUND " .. round .. "  -  CLASH!")
    draw_icon(mine, 40, 54, 15)
    onion.gfx_text(centered(mine, 4, 72), 84, mine, 1)
    onion.gfx_text(centered("-" .. you_take, 4, 72), 102, "-" .. you_take, 1)
    onion.gfx_text(116, 64, "VS", 2)
    draw_icon(theirs, 224, 54, 15)
    onion.gfx_text(centered(theirs, 188, 72), 84, theirs, 1)
    onion.gfx_text(centered("-" .. cpu_take, 188, 72), 102, "-" .. cpu_take, 1)
    hpbar("YOU", 112, you)
    hpbar("CPU", 134, opp)
    onion.gfx_text(4, 170, "SELECT = continue", 1)
    onion.gfx_show()
end

local function draw_match_result(verdict, sy, sc)
    onion.gfx_clear()
    onion.gfx_rect(0, 0, W, H, false)
    onion.gfx_text(centered(verdict, 0, W), 60, verdict, 2)
    onion.gfx_text(centered("SET " .. sy .. " - " .. sc, 0, W), 100, "SET " .. sy .. " - " .. sc, 1)
    onion.gfx_text(centered("SELECT = next match", 0, W), 168, "SELECT = next match", 1)
    onion.gfx_show()
end

local function draw_set_over(won, w, l)
    onion.gfx_clear()
    onion.gfx_rect(0, 0, W, H, false)
    local title = won and "SET WON!" or "SET LOST"
    onion.gfx_text(centered(title, 0, W), 46, title, 2)
    onion.gfx_text(40, 84, "Lifetime record", 1)
    onion.gfx_text(40, 110, "WINS " .. w .. "   LOSSES " .. l, 1)
    onion.gfx_text(4, 150, "SELECT = play again", 1)
    onion.gfx_text(4, 170, "CANCEL = menu", 1)
    onion.gfx_show()
end

-- ---------- AI ----------
local function predict_player()
    local best, n = "SLASH", -1
    for m, c in pairs(hist) do
        if m ~= "SPECIAL" and c > n then best, n = m, c end
    end
    return best
end

-- (cpu_take, ply_take) for a matchup
local function outcome(cpu_move, ply_move)
    if cpu_move == "SPECIAL" and ply_move == "SPECIAL" then return 40, 40 end
    if cpu_move == "SPECIAL" then return 0, 40 end
    if ply_move == "SPECIAL" then return 40, 0 end
    local d = dmg[cpu_move][ply_move]
    return d[1], d[2]
end

local function cpu_choose(diff, you_hp, cpu_hp, cpu_en)
    local can = cpu_en >= 3
    local r = (g_entropy * 48271 + 13) % 100
    if diff == "easy" then
        if can and r < 8 then return "SPECIAL" end
        if r < 45 then return "SLASH" elseif r < 78 then return "BLAST" else return "SHIELD" end
    elseif diff == "normal" then
        if can and r < 30 then return "SPECIAL" end
        if r < 48 then return "SLASH" elseif r < 82 then return "BLAST" else return "SHIELD" end
    else -- hard: predict + best response
        local pred = predict_player()
        if can and (pred == "SLASH" or pred == "BLAST") then return "SPECIAL" end
        if cpu_hp <= 30 and (pred == "SLASH" or pred == "BLAST") then return "SHIELD" end
        local cand = {"SLASH", "SHIELD", "BLAST"}
        if can then cand[#cand + 1] = "SPECIAL" end
        local best, bestscore = "SLASH", -999
        for _, cm in ipairs(cand) do
            local ct, pt = outcome(cm, pred)
            local score = pt - ct
            if ct >= cpu_hp then score = score - 100 end
            if pt >= you_hp then score = score + 50 end
            if score > bestscore then best, bestscore = cm, score end
        end
        return best
    end
end

-- ---------- one duel (to 0 HP) ----------
local function play_duel(diff, sy, sc)
    local you, opp, en_you, en_cpu, round = 100, 100, 0, 0, 0
    while you > 0 and opp > 0 do
        round = round + 1
        draw_pick(round, you, opp, en_you, sy, sc)

        local mine
        while true do
            local k = wait_button()
            if k == "left"  then mine = "SLASH"  break end
            if k == "up"    then mine = "SHIELD" break end
            if k == "right" then mine = "BLAST"  break end
            if k == "select" and en_you >= 3 then mine = "SPECIAL" break end
            if k == "cancel" then return "forfeit" end
        end
        hist[mine] = (hist[mine] or 0) + 1

        local theirs = cpu_choose(diff, you, opp, en_cpu)
        local cpu_take, you_take = outcome(theirs, mine)
        opp = math.max(0, opp - cpu_take)
        you = math.max(0, you - you_take)

        if mine == "SPECIAL" then en_you = en_you - 3
        else en_you = math.min(5, en_you + 1 + (you_take > 0 and 1 or 0)) end
        if theirs == "SPECIAL" then en_cpu = en_cpu - 3
        else en_cpu = math.min(5, en_cpu + 1 + (cpu_take > 0 and 1 or 0)) end

        draw_clash(round, mine, theirs, you_take, cpu_take, you, opp)
        if wait_sel_cxl() == "cancel" then return "forfeit" end
    end
    if you <= 0 and opp <= 0 then return "draw"
    elseif opp <= 0 then return "win"
    else return "lose" end
end

-- ---------- best-of-3 set ----------
local function play_set(diff)
    hist.SLASH, hist.SHIELD, hist.BLAST, hist.SPECIAL = 0, 0, 0, 0
    local sy, sc = 0, 0
    while sy < 2 and sc < 2 do
        local r = play_duel(diff, sy, sc)
        if r == "forfeit" then return end
        if r == "win" then sy = sy + 1
        elseif r == "lose" then sc = sc + 1 end
        if sy < 2 and sc < 2 then
            local v = (r == "win") and "YOU WIN!" or (r == "lose") and "YOU LOSE" or "DRAW"
            draw_match_result(v, sy, sc)
            if wait_sel_cxl() == "cancel" then return end
        end
    end
    local won = sy >= 2
    local w = (tonumber(onion.kv_get("duel_w", "0")) or 0)
    local l = (tonumber(onion.kv_get("duel_l", "0")) or 0)
    if won then w = w + 1; onion.kv_set("duel_w", tostring(w))
    else l = l + 1; onion.kv_set("duel_l", tostring(l)) end
    draw_set_over(won, w, l)
    if wait_sel_cxl() == "select" then return "again" end
end

-- ---------- menus / info ----------
local function choose_difficulty()
    onion.gfx_clear()
    onion.gfx_rect(0, 0, W, H, false)
    onion.gfx_text(28, 40, "DIFFICULTY", 2)
    onion.gfx_line(0, 52, W, 52)
    onion.gfx_text(10, 80,  "LEFT   Easy", 1)
    onion.gfx_text(10, 102, "UP     Normal", 1)
    onion.gfx_text(10, 124, "RIGHT  Hard", 1)
    onion.gfx_text(10, 146, "CANCEL back", 1)
    onion.gfx_show()
    while true do
        local k = wait_button()
        if k == "left"   then return "easy"   end
        if k == "up"     then return "normal" end
        if k == "right"  then return "hard"   end
        if k == "cancel" then return nil      end
    end
end

local function show_stats()
    local w = tonumber(onion.kv_get("duel_w", "0")) or 0
    local l = tonumber(onion.kv_get("duel_l", "0")) or 0
    onion.gfx_clear()
    onion.gfx_rect(0, 0, W, H, false)
    onion.gfx_text(60, 40, "STATS", 2)
    onion.gfx_line(0, 52, W, 52)
    onion.gfx_text(30, 84,  "Sets won:  " .. w, 1)
    onion.gfx_text(30, 110, "Sets lost: " .. l, 1)
    onion.gfx_text(4, 170, "any key: back", 1)
    onion.gfx_show()
    wait_button()
end

local function instructions()
    onion.gfx_clear()
    titlebar("HOW TO PLAY   1/3")
    onion.gfx_text(4, 48,  "You and your opponent", 1)
    onion.gfx_text(4, 68,  "pick a move each", 1)
    onion.gfx_text(4, 88,  "round, at once. Win", 1)
    onion.gfx_text(4, 108, "a duel at 0 HP; win", 1)
    onion.gfx_text(4, 128, "2 duels = the set.", 1)
    onion.gfx_text(4, 168, "any key: next", 1)
    onion.gfx_show(); wait_button()

    onion.gfx_clear()
    titlebar("HOW TO PLAY   2/3")
    onion.gfx_text(4, 48,  "L SLASH: 30 dmg, but", 1)
    onion.gfx_text(4, 68,  " -10 if they SHIELD", 1)
    onion.gfx_text(4, 88,  "R BLAST: 20 dmg,", 1)
    onion.gfx_text(4, 108, " safe vs SHIELD (0)", 1)
    onion.gfx_text(4, 128, "U SHIELD: blocks all,", 1)
    onion.gfx_text(4, 148, " deals no damage", 1)
    onion.gfx_text(4, 168, "any key: next", 1)
    onion.gfx_show(); wait_button()

    onion.gfx_clear()
    titlebar("HOW TO PLAY   3/3")
    onion.gfx_text(4, 48,  "Gain 1 ENERGY each", 1)
    onion.gfx_text(4, 68,  "round (+1 when hit).", 1)
    onion.gfx_text(4, 88,  "At 3 pips press", 1)
    onion.gfx_text(4, 108, "SELECT for SPECIAL:", 1)
    onion.gfx_text(4, 128, "40 dmg, ignores", 1)
    onion.gfx_text(4, 148, "SHIELD. Costs 3.", 1)
    onion.gfx_text(4, 168, "any key: back", 1)
    onion.gfx_show(); wait_button()
end

local function draw_menu()
    onion.gfx_clear()
    onion.gfx_rect(0, 0, W, H, false)
    onion.gfx_text(28, 38, "ESP-DUEL", 2)
    onion.gfx_line(0, 50, W, 50)
    onion.gfx_text(10, 74,  "UP     Play vs CPU", 1)
    onion.gfx_text(10, 96,  "DOWN   Stats", 1)
    onion.gfx_text(10, 118, "LEFT   vs Badge (soon)", 1)
    onion.gfx_text(10, 140, "SELECT How to Play", 1)
    onion.gfx_text(10, 162, "CANCEL Exit", 1)
    onion.gfx_show()
end

-- ---------- main ----------
while true do
    draw_menu()
    local k = wait_button()
    if k == "cancel" then
        onion.release_display()
        return
    elseif k == "up" then
        local diff = choose_difficulty()
        if diff then
            while play_set(diff) == "again" do end
        end
    elseif k == "down" then
        show_stats()
    elseif k == "select" then
        instructions()
    elseif k == "left" then
        onion.gfx_clear()
        titlebar("vs BADGE")
        onion.gfx_text(4, 52,  "Wireless mode is", 1)
        onion.gfx_text(4, 72,  "coming soon.", 1)
        onion.gfx_text(4, 100, "Needs a 2nd badge", 1)
        onion.gfx_text(4, 120, "+ a firmware update.", 1)
        onion.gfx_text(4, 168, "any key: back", 1)
        onion.gfx_show()
        wait_button()
    end
end
