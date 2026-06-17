-- ESP-Duel (onion-os Lua) — graphical fighter. vs CPU (best-of-3, difficulty)
-- AND vs another badge over the air. Energy/SPECIAL, persistent stats, move
-- icons + clash screen.
--
-- Controls: LEFT=SLASH  UP=SHIELD  RIGHT=BLAST  SELECT=SPECIAL (when charged).
--
-- WIRELESS (vs Badge): two badges pair over ESP-NOW and duel to 0 HP with a
-- SHA-256 commit-reveal protocol — neither can change its move after seeing the
-- other's. The exchange is arrival-order-independent with retransmit so a lost
-- frame can't desync the match. See play_net() / net_round() below.

onion.log("ESP-Duel start")

-- ── Upstream-firmware compatibility shim ──────────────────────────────────────
-- This game was written against the old gfx_* Lua API. Upstream onion-os exposes
-- the same drawing on a canvas via display_*. These shims re-create gfx_* on top
-- of display_*, so all the game logic below is unchanged:
--   * gfx_clear()  -> display_begin() + wipe the canvas to white (deferred frame)
--   * gfx_show()   -> display_commit() (paints the batched frame in one refresh)
--   * gfx_text size 2 -> "large" font (FreeMonoBold18pt); else "small" (FreeMono9pt)
-- Drawing is batched between gfx_clear and gfx_show into a single e-ink refresh.
-- If you ever run this on firmware with native gfx_*, delete this block.
if not onion.display_begin then
  error("ESP-Duel needs the onion-os display_* API (upstream firmware)")
end
do
  function onion.gfx_clear()
    onion.display_begin()                            -- defer refresh until gfx_show()
    onion.display_text("", 0, 0, { clear = true })   -- wipe canvas to white
  end
  function onion.gfx_text(x, y, str, size, white)
    onion.display_text(str, x, y, {
      clear = false,
      font  = (size and size >= 2) and "large" or "small",
      color = white and "white" or "black",
    })
  end
  function onion.gfx_rect(x, y, w, h, fill)
    onion.display_rect(x, y, w, h, { fill = fill and true or false })
  end
  function onion.gfx_line(x0, y0, x1, y1)
    onion.display_line(x0, y0, x1, y1)
  end
  function onion.gfx_circle(cx, cy, r, fill)
    onion.display_circle(cx, cy, r, { fill = fill and true or false })
  end
  function onion.gfx_triangle(x0, y0, x1, y1, x2, y2, fill)
    onion.display_triangle(x0, y0, x1, y1, x2, y2, { fill = fill and true or false })
  end
  function onion.gfx_show()
    onion.display_commit()                           -- render the batched frame
  end
end

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

-- ---------- two-badge wireless (ESP-NOW commit-reveal) ----------
-- A pure-Lua port of the standalone ESP-Duel protocol. No firmware change is
-- needed: onion-os already exposes espnow_* + sha256 + secure_random. Fairness
-- comes from a commit-reveal exchange (you can't change your move after seeing
-- the foe's), and the round protocol is arrival-order-independent with
-- retransmit + a reveal-burst so a dropped frame can't desync the two badges.
-- Both badges resolve each round locally from the two moves; outcome() is
-- deterministic and dmg[] is symmetric, so they always agree on the result.
local NET_CH      = 6     -- fixed ESP-NOW channel: both badges pin here so that
                          -- different Wi-Fi APs can't strand them on split channels
local RETX        = 4     -- (re)send every 4 ticks (~600ms at 150ms/recv)
local FORFEIT     = 170   -- peer silent this many ticks (~25s) => win by forfeit
local BAR_TIMEOUT = 80    -- ready-barrier give-up (~10s)

-- flip NET_DEBUG to true to trace the handshake over serial (onion.log → UART,
-- 115200). Each badge prints its own view: channel, pairing, barrier progress.
local NET_DEBUG = false
local function ndbg(s) if NET_DEBUG and onion.log then onion.log("DUEL " .. s) end end

local function release_buttons()
  local b = onion.buttons()
  while b.left or b.up or b.right or b.down or b.select or b.cancel do
    onion.sleep(30); b = onion.buttons()
  end
end

local function hexenc(s)
  local o = {}
  for i = 1, #s do o[i] = string.format("%02x", string.byte(s, i)) end
  return table.concat(o)
end

-- ascii nonce: ATECC random if available, else fold local entropy + radio counters
local function gen_nonce()
  local r = onion.secure_random and onion.secure_random(8)
  if r and #r >= 8 then return hexenc(r) end
  local info = onion.espnow_info and onion.espnow_info() or {}
  g_entropy = (g_entropy * 1103515245 + (info.received or 0) + (info.sent or 0) + 12345) % 2147483647
  return string.format("%08x%08x", g_entropy, (g_entropy * 48271 + 11) % 2147483647)
end

-- commit binds (round, move, nonce) as plain ascii so both sides hash identically
local function commit_hash(round, move, nonce)
  return hexenc(onion.sha256(round .. "|" .. move .. "|" .. nonce))
end

local function psplit(s)
  local t = {}
  for part in string.gmatch(s, "([^|]+)") do t[#t + 1] = part end
  return t
end

local function nsend(peer, s)      onion.espnow_send(s, peer) end
local function nbroadcast(s)       onion.espnow_send(s)       end

-- ---------- wireless draw helpers (reuse the vs-CPU widgets) ----------
local function draw_search(mymac, dots)
  onion.gfx_clear()
  onion.gfx_rect(0, 0, W, H, false)
  onion.gfx_text(28, 38, "vs BADGE", 2)
  onion.gfx_line(0, 50, W, 50)
  onion.gfx_text(10, 76,  "Looking for a", 1)
  onion.gfx_text(10, 96,  "nearby badge" .. string.rep(".", dots), 1)
  onion.gfx_text(10, 128, "Me: " .. mymac, 1)
  onion.gfx_text(4, 168, "CANCEL = back", 1)
  onion.gfx_show()
end

local function draw_found(peer, p1)
  onion.gfx_clear(); onion.gfx_rect(0, 0, W, H, false)
  onion.gfx_text(centered("FOE FOUND!", 0, W), 50, "FOE FOUND!", 2)
  onion.gfx_text(centered(peer, 0, W), 86, peer, 1)
  local role = p1 and "You are PLAYER 1" or "You are PLAYER 2"
  onion.gfx_text(centered(role, 0, W), 112, role, 1)
  onion.gfx_text(centered("syncing...", 0, W), 150, "syncing...", 1)
  onion.gfx_show()
end

local function draw_pick_net(round, you, opp, en_me)
  onion.gfx_clear()
  titlebar("vs BADGE  R" .. round)
  hpbar("YOU", 28, you)
  hpbar("FOE", 50, opp)
  energy_pips(70, en_me)
  local boxes = {{2,"L","SLASH"}, {92,"U","SHIELD"}, {182,"R","BLAST"}}
  for _, b in ipairs(boxes) do
    local x = b[1]
    onion.gfx_rect(x, 86, 80, 60, false)
    onion.gfx_text(x + 35, 99, b[2], 1)
    draw_icon(b[3], x + 40, 116, 11)
    onion.gfx_text(centered(b[3], x, 80), 143, b[3], 1)
  end
  if en_me >= 3 then onion.gfx_text(4, 162, "SELECT=SPECIAL  CXL=quit", 1)
  else onion.gfx_text(4, 162, "CANCEL = forfeit", 1) end
  onion.gfx_show()
end

local function draw_wait_net(round, you, opp)
  onion.gfx_clear()
  titlebar("vs BADGE  R" .. round)
  hpbar("YOU", 28, you); hpbar("FOE", 50, opp)
  onion.gfx_text(centered("MOVE LOCKED", 0, W), 104, "MOVE LOCKED", 1)
  onion.gfx_text(centered("waiting for foe...", 0, W), 128, "waiting for foe...", 1)
  onion.gfx_show()
end

local function draw_clash_net(round, mine, theirs, you_take, op_take, you, opp)
  onion.gfx_clear()
  titlebar("ROUND " .. round .. "  -  CLASH!")
  draw_icon(mine, 40, 54, 15)
  onion.gfx_text(centered(mine, 4, 72), 84, mine, 1)
  onion.gfx_text(centered("-" .. you_take, 4, 72), 102, "-" .. you_take, 1)
  onion.gfx_text(116, 64, "VS", 2)
  draw_icon(theirs, 224, 54, 15)
  onion.gfx_text(centered(theirs, 188, 72), 84, theirs, 1)
  onion.gfx_text(centered("-" .. op_take, 188, 72), 102, "-" .. op_take, 1)
  hpbar("YOU", 112, you); hpbar("FOE", 134, opp)
  onion.gfx_show()
end

local function draw_net_result(result, nw, nl)
  onion.gfx_clear(); onion.gfx_rect(0, 0, W, H, false)
  local v = (result == "win"  and "YOU WIN!")
         or (result == "lose" and "YOU LOSE")
         or (result == "draw" and "DRAW")
         or (result == "cheat" and "BAD DATA")
         or "FOE LEFT"
  onion.gfx_text(centered(v, 0, W), 52, v, 2)
  local rec = "W " .. nw .. "   L " .. nl
  onion.gfx_text(centered(rec, 0, W), 92, rec, 1)
  onion.gfx_text(4, 150, "SELECT = rematch", 1)
  onion.gfx_text(4, 170, "CANCEL = menu", 1)
  onion.gfx_show()
end

-- ---------- pairing + sync ----------
-- Broadcast our presence and listen. Returns the peer MAC, or nil on CANCEL.
-- We accept H (hello) OR Y (ready) from a peer: if the other badge latched onto
-- us first and already moved to the ready-barrier, its Y still pairs us to it.
local function net_pair(mymac)
  local tick = 0
  draw_search(mymac, 0)
  while true do
    if tick % 3 == 0 then nbroadcast("H|" .. mymac) end
    local m = onion.espnow_receive(150)
    if m and m.mac ~= mymac then
      local p = psplit(m.payload or "")
      if p[1] == "H" or p[1] == "Y" then
        ndbg("paired peer=" .. tostring(m.mac) .. " via=" .. tostring(p[1]))
        return m.mac
      end
    end
    local b = onion.buttons()
    if b.cancel then release_buttons(); return nil end
    tick = tick + 1
    if tick % 4 == 0 then draw_search(mymac, (tick // 4) % 4) end
  end
end

-- Exchange `tag` until we have both sent and received it. Returns true when both
-- badges are synced, false if the peer quit (F) or we time out.
local function net_barrier(peer, tag)
  local got, tick, silent, heard = false, 0, 0, 0
  while not got do
    if tick % 3 == 0 then nsend(peer, tag) end
    local m = onion.espnow_receive(120)
    if m and m.mac == peer then
      heard = heard + 1
      local p = psplit(m.payload or "")
      if p[1] == "F" then ndbg("barrier " .. tag .. " got F (foe quit)"); return false end
      if p[1] == tag then got = true end
    else
      silent = silent + 1
      if silent > BAR_TIMEOUT then
        ndbg("barrier " .. tag .. " TIMEOUT silent=" .. silent .. " heard=" .. heard)
        return false
      end
    end
    tick = tick + 1
  end
  ndbg("barrier " .. tag .. " OK heard=" .. heard)
  for _ = 1, 5 do nsend(peer, tag); onion.sleep(70) end   -- make sure peer exits too
  return true
end

-- last reveal we sent, so a peer still finishing the previous round can be
-- re-fed it on demand (defends against a lost final reveal desyncing the match)
local g_last_reveal = nil

-- Play one networked round. Returns:
--   "ok", mine, theirs   resolved cleanly
--   "forfeit"            we quit (peer told)
--   "forfeit_win"        peer quit or went silent
--   "cheat"              peer's reveal didn't match its commit
local function net_round(peer, round, you, opp, en_me)
  local op_commit, op_move, op_nonce = nil, nil, nil
  local mine, cmsg, rmsg

  -- one place that interprets every peer frame, whatever phase we're in
  local function handle(m)
    if not (m and m.mac == peer) then return nil end
    local p = psplit(m.payload or "")
    local tag = p[1]
    if tag == "C" and tonumber(p[2]) == round then
      op_commit = op_commit or p[3]
    elseif tag == "R" and tonumber(p[2]) == round then
      if not op_move then op_move, op_nonce = p[3], p[4] end
      op_commit = op_commit or commit_hash(round, p[3], p[4])   -- recover a lost commit
    elseif tag == "R" and g_last_reveal and tonumber(p[2]) == g_last_reveal.round then
      nsend(peer, g_last_reveal.msg)                            -- peer is a round behind
    elseif tag == "F" then
      return "F"
    end
    return nil
  end

  -- 1) pick a move. While waiting on the human we keep the link warm (K) and
  --    drain incoming frames so the foe doesn't forfeit us for "thinking".
  draw_pick_net(round, you, opp, en_me)
  release_buttons()
  local pickt = 0
  while not mine do
    local b = onion.buttons()
    if b.left  then mine = "SLASH"
    elseif b.up    then mine = "SHIELD"
    elseif b.right then mine = "BLAST"
    elseif b.select and en_me >= 3 then mine = "SPECIAL"
    elseif b.cancel then
      for _ = 1, 5 do nsend(peer, "F"); onion.sleep(50) end
      return "forfeit"
    end
    if not mine then
      if pickt % 5 == 0 then nsend(peer, "K|" .. round) end
      if handle(onion.espnow_receive(90)) == "F" then return "forfeit_win" end
      pickt = pickt + 1
    end
  end

  local nonce = gen_nonce()
  cmsg = "C|" .. round .. "|" .. commit_hash(round, mine, nonce)
  rmsg = "R|" .. round .. "|" .. mine .. "|" .. nonce
  draw_wait_net(round, you, opp)

  -- 2) exchange COMMITs (never reveal before we hold the foe's commit => fair)
  ndbg("R" .. round .. " wait commit")
  local tick, silent = 0, 0
  while op_commit == nil do
    if tick % RETX == 0 then nsend(peer, cmsg) end
    local m = onion.espnow_receive(150)
    if m and m.mac == peer then silent = 0 else silent = silent + 1 end
    if handle(m) == "F" then return "forfeit_win" end
    if silent > FORFEIT then ndbg("R" .. round .. " commit FORFEIT silent=" .. silent); return "forfeit_win" end
    tick = tick + 1
  end
  ndbg("R" .. round .. " got commit")

  -- 3) exchange REVEALs
  g_last_reveal = { round = round, msg = rmsg }
  tick, silent = 0, 0
  while op_move == nil do
    if tick % RETX == 0 then nsend(peer, rmsg) end
    local m = onion.espnow_receive(150)
    if m and m.mac == peer then silent = 0 else silent = silent + 1 end
    if handle(m) == "F" then return "forfeit_win" end
    if silent > FORFEIT then return "forfeit_win" end
    tick = tick + 1
  end

  -- 4) verify the foe's move matches its commit
  if commit_hash(round, op_move, op_nonce) ~= op_commit then return "cheat" end

  -- 5) burst our reveal so the foe definitely advances out of this round
  for _ = 1, 3 do nsend(peer, rmsg); onion.sleep(60) end
  return "ok", mine, op_move
end

-- result screen: SELECT rematch (barrier with foe), CANCEL quit (tell foe)
local function net_result_choice(peer)
  release_buttons()
  while true do
    local b = onion.buttons()
    if b.select then return net_barrier(peer, "Y") end
    if b.cancel then
      for _ = 1, 5 do nsend(peer, "F"); onion.sleep(50) end
      return false
    end
    local m = onion.espnow_receive(90)
    if m and m.mac == peer then
      local p = psplit(m.payload or "")
      if p[1] == "F" then return false end
    end
  end
end

local function net_cleanup()
  if onion.espnow_stop then onion.espnow_stop() end
  if onion.wifi_reconnect then onion.wifi_reconnect() end
end

-- full wireless match flow: setup radio -> pair -> sync -> duel to 0 HP -> rematch
local function play_net()
  if not (onion.espnow_start and onion.espnow_send and onion.espnow_receive) then
    onion.gfx_clear(); titlebar("vs BADGE")
    onion.gfx_text(4, 60, "This firmware has no", 1)
    onion.gfx_text(4, 80, "ESP-NOW support.", 1)
    onion.gfx_text(4, 168, "any key: back", 1)
    onion.gfx_show(); wait_button(); return
  end

  -- pin both badges to one ESP-NOW channel, independent of any Wi-Fi AP.
  -- wifi_disconnect() leaves the AP via esp_wifi_disconnect(), which is
  -- ASYNC: the badge stays associated for a few hundred ms. espnow_start()
  -- can't pin a new channel while still associated to an AP on another
  -- channel, so retry across the disassociation window instead of failing
  -- on the first (still-connected) attempt.
  if onion.wifi_disconnect then
    onion.wifi_disconnect()
    onion.gfx_clear(); titlebar("vs BADGE")
    onion.gfx_text(4, 90, "Freeing radio...", 1); onion.gfx_show()
  end
  local ok, err
  for attempt = 1, 12 do              -- ~3s total: covers slow disassociation
    ok, err = onion.espnow_start(NET_CH)
    if ok then break end
    onion.sleep(250)
  end
  if not ok then
    onion.gfx_clear(); titlebar("vs BADGE")
    onion.gfx_text(4, 60, "Radio start failed:", 1)
    onion.gfx_text(4, 80, tostring(err or "?"), 1)
    onion.gfx_text(4, 168, "any key: back", 1)
    onion.gfx_show(); wait_button(); net_cleanup(); return
  end

  local mymac = onion.espnow_mac()
  local info0 = onion.espnow_info and onion.espnow_info() or {}
  ndbg("started ch=" .. tostring(info0.channel) .. " me=" .. tostring(mymac))
  local peer  = net_pair(mymac)
  if not peer then net_cleanup(); return end

  local i_am_p1 = mymac < peer        -- deterministic, display-only role
  draw_found(peer, i_am_p1)
  if not net_barrier(peer, "Y") then
    draw_net_result("left",
      tonumber(onion.kv_get("duel_nw", "0")) or 0,
      tonumber(onion.kv_get("duel_nl", "0")) or 0)
    wait_sel_cxl(); net_cleanup(); return
  end

  while true do                       -- rematch loop
    local you, opp, en_me, en_op, round = 100, 100, 0, 0, 0
    local result = nil
    g_last_reveal = nil

    while you > 0 and opp > 0 do
      round = round + 1
      local st, mine, theirs = net_round(peer, round, you, opp, en_me)
      if st == "forfeit" then net_cleanup(); return
      elseif st == "forfeit_win" then result = "win"; break
      elseif st == "cheat" then result = "cheat"; break end

      -- deterministic, identical on both badges (outcome() + symmetric dmg[])
      local op_take, my_take = outcome(theirs, mine)
      opp = math.max(0, opp - op_take)
      you = math.max(0, you - my_take)
      if mine == "SPECIAL" then en_me = en_me - 3
      else en_me = math.min(5, en_me + 1 + (my_take > 0 and 1 or 0)) end
      if theirs == "SPECIAL" then en_op = en_op - 3
      else en_op = math.min(5, en_op + 1 + (op_take > 0 and 1 or 0)) end

      draw_clash_net(round, mine, theirs, my_take, op_take, you, opp)
      onion.sleep(2200)               -- auto-advance: keep both badges moving in step
    end

    if result == nil then
      result = (you <= 0 and opp <= 0) and "draw"
            or (opp <= 0) and "win" or "lose"
    end

    local nw = tonumber(onion.kv_get("duel_nw", "0")) or 0
    local nl = tonumber(onion.kv_get("duel_nl", "0")) or 0
    if result == "win"  then nw = nw + 1; onion.kv_set("duel_nw", tostring(nw))
    elseif result == "lose" then nl = nl + 1; onion.kv_set("duel_nl", tostring(nl)) end

    draw_net_result(result, nw, nl)
    if not net_result_choice(peer) then break end
  end

  net_cleanup()
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
    onion.gfx_text(10, 118, "LEFT   vs Badge", 1)
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
        play_net()
    end
end
