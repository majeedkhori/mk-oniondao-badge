-- Browse downloaded Onion OS image assets from SPIFFS.
--
-- Controls:
--   UP/LEFT: previous image
--   DOWN/RIGHT: next image
--   SELECT: redraw current image
--   CANCEL: return to Onion OS

local images = onion.images()

if #images == 0 then
    onion.log("No downloaded images")
    onion.release_display()
    return
end

table.sort(images)

local index = 1

local function show_current()
    local name = images[index]
    local ok, err = onion.display_bitmap(name, -1, -1, true)
    if ok then
        onion.log(index .. "/" .. #images .. " " .. name)
    else
        onion.log(err or ("Could not display " .. name))
    end
end

local function any_button_pressed()
    local buttons = onion.buttons()
    return buttons.left or buttons.down or buttons.up or buttons.right or
        buttons.select or buttons.cancel
end

while any_button_pressed() do
    onion.sleep(50)
end

show_current()

local last = onion.buttons()

while true do
    local buttons = onion.buttons()

    if buttons.cancel and not last.cancel then
        onion.release_display()
        return
    end

    if (buttons.down and not last.down) or (buttons.right and not last.right) then
        index = index + 1
        if index > #images then index = 1 end
        show_current()
    elseif (buttons.up and not last.up) or (buttons.left and not last.left) then
        index = index - 1
        if index < 1 then index = #images end
        show_current()
    elseif buttons.select and not last.select then
        show_current()
    end

    last = buttons
    onion.sleep(80)
end
