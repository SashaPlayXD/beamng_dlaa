-- bng_dlss: camera jitter for DLSS + 'DLAA (NVIDIA)' entry in the game anti-aliasing menu (milestone 6, fix 2)
-- Loaded automatically by scripts/bng_dlss/modScript.lua.
-- Every frame shifts the camera projection by a sub-pixel amount (Halton 2,3, 8 phases).
-- The addon finds the phase by itself and tells us via settings/bng_dlss_state.txt
-- whether jitter is wanted ("1") or not ("0", DLAA is off -> no jitter, no shaking).
-- Console:  bngDlssJitter.setEnabled(false) forces jitter off regardless of the addon.
local M = {}

local PHASES = 8
local userEnabled = true      -- manual master switch
local addonWants = true       -- from the state file; default ON if the file can't be read
local counter = 0
local pollIn = 0
local loggedRead = false

local STATE_PATHS = { "/settings/bng_dlss_state.txt", "settings/bng_dlss_state.txt" }
local REQUEST_PATH = "/settings/bng_dlss_request.txt"
local lastRequest = nil
local menuCheckIn = 0
local framesSinceLoad = 0
local NOTIFY_AT = { [120] = true, [600] = true, [1800] = true }  -- re-push the option list a few times after start

local function notifyUI(reason)
  local ok = pcall(function() settings.notifyUI() end)
  log("D", "bngDlssJitter", "settings.notifyUI (" .. reason .. "): " .. tostring(ok))
end

-- ---------- game menu integration ----------
local function writeRequest(on)
  local v = on and "1" or "0"
  if v == lastRequest then return end
  local ok, err = pcall(writeFile, REQUEST_PATH, v)
  if not ok or err == false then
    ok, err = pcall(function()
      local f = io.open(REQUEST_PATH, "w"); f:write(v); f:close()
    end)
  end
  lastRequest = v
  log("I", "bngDlssJitter", "menu request DLAA=" .. v .. " (write ok=" .. tostring(ok) .. ")")
end

local function setAAPostEffects(smaaOn, fxaaOn)
  local smaa = scenetree.findObject("SMAA_PostEffect")
  local fxaa = scenetree.findObject("FXAA_PostEffect")
  if smaa then if smaaOn then smaa:enable() else smaa:disable() end end
  if fxaa then if fxaaOn then fxaa:enable() else fxaa:disable() end end
end

local function dlaaSelected()
  local aaOn = tonumber(settings.getValue("GraphicAntialias")) or 0
  return aaOn ~= 0 and settings.getValue("GraphicAntialiasType") == "dlaa"
end

local function patchMenu()
  if not core_settings_graphic or not core_settings_graphic.getOptions then return end
  local opts = core_settings_graphic.getOptions()
  if not opts or not opts.GraphicAntialiasType or not opts.GraphicAntialias then return end
  local aaType, aaOn = opts.GraphicAntialiasType, opts.GraphicAntialias
  if aaType.__bngDlss then return end

  local origModes, origTypeSet, origOnSet = aaType.getModes, aaType.set, aaOn.set
  aaType.getModes = function()
    local m = origModes()
    local keys, values = {}, {}
    for i, k in ipairs(m.keys) do keys[i] = k; values[i] = m.values[i] end
    keys[#keys + 1] = "dlaa"; values[#values + 1] = "DLAA (NVIDIA)"
    return { keys = keys, values = values }
  end
  aaType.set = function(value)
    if value == "dlaa" then
      settings.setValue("GraphicAntialiasType", "dlaa")
      local on = (tonumber(settings.getValue("GraphicAntialias")) or 0) ~= 0
      if on then setAAPostEffects(false, false) end   -- DLAA replaces SMAA/FXAA
      writeRequest(on)
    else
      writeRequest(false)
      origTypeSet(value)
    end
  end
  aaOn.set = function(value)
    if settings.getValue("GraphicAntialiasType") == "dlaa" then
      settings.setValue("GraphicAntialias", value)
      setAAPostEffects(false, false)
      writeRequest(tonumber(value) ~= 0)
    else
      origOnSet(value)
    end
  end
  aaType.__bngDlss = true
  log("I", "bngDlssJitter", "anti-aliasing menu patched: 'DLAA (NVIDIA)' added")
  -- the UI caches the option lists it received at startup: push the updated list to it now
  notifyUI("after patch")
end

-- keeps the menu patched and SMAA/FXAA off while DLAA is selected (the game may re-apply settings)
local function menuTick()
  pcall(patchMenu)
  pcall(function()
    local sel = dlaaSelected()
    if sel then
      local smaa = scenetree.findObject("SMAA_PostEffect")
      local fxaa = scenetree.findObject("FXAA_PostEffect")
      if (smaa and smaa:isEnabled()) or (fxaa and fxaa:isEnabled()) then setAAPostEffects(false, false) end
    end
    writeRequest(sel)
  end)
end

local function halton(i, b)
  local f, r = 1, 0
  while i > 0 do
    f = f / b
    r = r + f * (i % b)
    i = math.floor(i / b)
  end
  return r
end

local function screenHeight()
  local ok, ext = pcall(function() return scenetree.OnlyGui:getExtent() end)
  if ok and ext and ext.y and ext.y > 0 then return ext.y end
  return 1440
end

local function pollState()
  for _, p in ipairs(STATE_PATHS) do
    local ok, s = pcall(readFile, p)
    if ok and type(s) == "string" and #s > 0 then
      local wants = s:sub(1, 1) ~= "0"
      if not loggedRead or wants ~= addonWants then
        log("I", "bngDlssJitter", "state file " .. p .. " -> jitter " .. tostring(wants))
        loggedRead = true
      end
      addonWants = wants
      return
    end
  end
end

local function resetOffset()
  pcall(function() scenetree.OnlyGui:setFrustumCameraCenterOffset(Point2F(0, 0)) end)
end

local function onPreRender(dtReal, dtSim, dtRaw)
  local i = counter % PHASES + 1      -- 1..8, same as the addon
  counter = counter + 1               -- counts EVERY frame, so the phase never drifts
  framesSinceLoad = framesSinceLoad + 1
  if NOTIFY_AT[framesSinceLoad] then notifyUI("startup " .. framesSinceLoad) end
  menuCheckIn = menuCheckIn - 1
  if menuCheckIn <= 0 then
    menuCheckIn = 60
    menuTick()
  end
  pollIn = pollIn - 1
  if pollIn <= 0 then
    pollIn = 15                       -- ~4-8 times per second
    local was = addonWants
    pollState()
    if was and not addonWants then resetOffset() end
  end
  if not (userEnabled and addonWants) then return end
  local jx = halton(i, 2) - 0.5       -- pixels, [-0.5, 0.5)
  local jy = -halton(i, 3)            -- pixels, (-1, 0]  (positive Y breaks vehicle culling)
  pcall(function()                    -- in menus there may be no camera yet
    local k = math.tan(math.rad(core_camera.getFovDeg()) * 0.5) / (screenHeight() * 0.5)
    scenetree.OnlyGui:setFrustumCameraCenterOffset(Point2F(jx * k, jy * k))
  end)
end

local function setEnabled(v)
  userEnabled = v and true or false
  if not userEnabled then resetOffset() end
  log("I", "bngDlssJitter", "jitter (manual switch) " .. tostring(userEnabled))
end

local function onExtensionLoaded()
  pcall(patchMenu)          -- patch as early as possible, before the options UI is opened
  menuCheckIn = 0           -- and run the full check on the very first frame
  local okf, fov = pcall(function() return core_camera.getFovDeg() end)
  log("I", "bngDlssJitter", "loaded. height=" .. tostring(screenHeight()) .. " fov=" .. tostring(fov))
end

-- the options screen was opened: make sure it shows the list with DLAA
local function onUiChangedState(toState, fromState)
  if type(toState) == "string" and toState:lower():find("option") then
    pcall(patchMenu)
    notifyUI("options opened")
  end
end

M.onPreRender = onPreRender
M.onUiChangedState = onUiChangedState
M.setEnabled = setEnabled
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = resetOffset
return M
