if not CLIENT then return end

-- Wait for RemixRenderCore to be available
if not RemixRenderCore then
    timer.Simple(0.1, function()
        if file.Exists("lua/remixlua/cl/customrender/cl_build_progress.lua", "GAME") then
            include("remixlua/cl/customrender/cl_build_progress.lua")
        end
    end)
    return
end

-- Build Progress Indicator
-- Shows a large loading screen while map geometry is being built
-- Author: CR

local PANEL = {}

function PANEL:Init()
    self:SetSize(600, 450)
    self:Center()
    self:SetTitle("Building Map Geometry")
    self:SetDraggable(false)
    self:ShowCloseButton(false)
    self:SetDeleteOnClose(false)
    self:MakePopup()
    self:SetKeyboardInputEnabled(false)
    
    -- Stuck state detection
    self.lastProgressUpdate = SysTime()
    self.stuckThreshold = 30 -- seconds without progress = stuck
    self.isStuck = false
    
    -- Progress bars for each renderer
    self.progressBars = {}
    
    local yPos = 40
    
    -- World Renderer
    self.progressBars.world = vgui.Create("DPanel", self)
    self.progressBars.world:SetPos(20, yPos)
    self.progressBars.world:SetSize(560, 80)
    self.progressBars.world.label = "Map Faces"
    self.progressBars.world.progress = 0
    self.progressBars.world.total = 0
    self.progressBars.world.active = false
    
    yPos = yPos + 90
    
    -- Displacement Renderer
    self.progressBars.displacement = vgui.Create("DPanel", self)
    self.progressBars.displacement:SetPos(20, yPos)
    self.progressBars.displacement:SetSize(560, 80)
    self.progressBars.displacement.label = "Map Displacements"
    self.progressBars.displacement.progress = 0
    self.progressBars.displacement.total = 0
    self.progressBars.displacement.active = false
    
    yPos = yPos + 90
    
    -- Static Props Renderer
    self.progressBars.staticprops = vgui.Create("DPanel", self)
    self.progressBars.staticprops:SetPos(20, yPos)
    self.progressBars.staticprops:SetSize(560, 80)
    self.progressBars.staticprops.label = "Static Props"
    self.progressBars.staticprops.progress = 0
    self.progressBars.staticprops.total = 0
    self.progressBars.staticprops.active = false
    
    -- Status text
    yPos = yPos + 90
    self.statusLabel = vgui.Create("DLabel", self)
    self.statusLabel:SetPos(20, yPos)
    self.statusLabel:SetSize(560, 20)
    self.statusLabel:SetText("Please wait...")
    self.statusLabel:SetTextColor(Color(255, 255, 255))
    self.statusLabel:SetFont("DermaDefault")
    
    -- Force close button (hidden initially)
    yPos = yPos + 30
    self.forceCloseButton = vgui.Create("DButton", self)
    self.forceCloseButton:SetPos(20, yPos)
    self.forceCloseButton:SetSize(560, 30)
    self.forceCloseButton:SetText("Force Close (Build appears stuck)")
    self.forceCloseButton:SetVisible(false)
    self.forceCloseButton.DoClick = function()
        self:Close()
        -- Reset stuck build states
        if RemixRenderCore then
            if RemixRenderCore._worldBuildState then
                RemixRenderCore._worldBuildState.active = false
                RemixRenderCore._worldBuildState.processed = 0
                RemixRenderCore._worldBuildState.total = 0
            end
            if RemixRenderCore._dispBuildState then
                RemixRenderCore._dispBuildState.active = false
                RemixRenderCore._dispBuildState.processed = 0
                RemixRenderCore._dispBuildState.total = 0
            end
            if RemixRenderCore._sprBuildState then
                RemixRenderCore._sprBuildState.active = false
                RemixRenderCore._sprBuildState.built = 0
            end
        end
        print("[Remix Build Progress] Forcibly closed and reset build states")
    end
    
    -- Start time for elapsed time display
    self.startTime = SysTime()
    
    -- Paint function for progress bars
    for _, bar in pairs(self.progressBars) do
        bar.Paint = function(pnl, w, h)
            -- Background
            surface.SetDrawColor(40, 40, 40, 240)
            surface.DrawRect(0, 0, w, h)
            
            -- Label
            draw.SimpleText(pnl.label, "DermaDefaultBold", 10, 10, Color(255, 255, 255))
            
            if pnl.active then
                -- Progress bar background
                local barX, barY = 10, 35
                local barW, barH = w - 20, 25
                surface.SetDrawColor(20, 20, 20, 200)
                surface.DrawRect(barX, barY, barW, barH)
                
                -- Check if we have a valid total (not -1 which means count-only mode)
                if pnl.total > 0 then
                    -- Progress bar fill
                    local percent = (pnl.progress / pnl.total)
                    local fillW = math.floor(barW * percent)
                    if fillW > 0 then
                        surface.SetDrawColor(131, 211, 48, 200)
                        surface.DrawRect(barX, barY, fillW, barH)
                    end
                    
                    -- Progress text with percentage
                    local progressText = string.format("%d / %d (%.1f%%)", 
                        pnl.progress or 0, 
                        pnl.total or 0, 
                        percent * 100)
                    draw.SimpleText(progressText, "DermaDefault", barX + barW / 2, barY + barH / 2, 
                        Color(255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
                elseif pnl.total == -1 then
                    -- Count-only mode (for static props)
                    -- Show animated progress bar
                    local time = SysTime()
                    local animPercent = (math.sin(time * 3) * 0.5 + 0.5)
                    local fillW = math.floor(barW * animPercent)
                    if fillW > 0 then
                        surface.SetDrawColor(131, 211, 48, 200)
                        surface.DrawRect(barX, barY, fillW, barH)
                    end
                    
                    -- Just show count
                    local progressText = string.format("Processing... %d items", pnl.progress or 0)
                    draw.SimpleText(progressText, "DermaDefault", barX + barW / 2, barY + barH / 2, 
                        Color(255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
                else
                    -- No data yet, show loading animation
                    local time = SysTime()
                    local animPercent = (math.sin(time * 3) * 0.5 + 0.5)
                    local fillW = math.floor(barW * animPercent)
                    if fillW > 0 then
                        surface.SetDrawColor(131, 211, 48, 200)
                        surface.DrawRect(barX, barY, fillW, barH)
                    end
                    
                    draw.SimpleText("Initializing...", "DermaDefault", barX + barW / 2, barY + barH / 2, 
                        Color(255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
                end
            else
                -- Not active indicator
                draw.SimpleText("Waiting...", "DermaDefault", 10, 35, Color(150, 150, 150))
            end
        end
    end
end

function PANEL:Paint(w, h)
    -- Semi-transparent dark background
    surface.SetDrawColor(20, 20, 20, 250)
    surface.DrawRect(0, 0, w, h)
    
    -- Border
    surface.SetDrawColor(131, 211, 48, 255)
    surface.DrawOutlinedRect(0, 0, w, h, 2)
    
    -- Title bar
    surface.SetDrawColor(30, 30, 30, 255)
    surface.DrawRect(0, 0, w, 24)
    
    draw.SimpleText("Building Map Geometry", "DermaDefaultBold", 
        w / 2, 12, Color(255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
end

function PANEL:UpdateProgress()
    local anyActive = false
    local currentProgress = 0
    
    -- Update World Renderer
    if RemixRenderCore and RemixRenderCore._worldBuildState then
        local state = RemixRenderCore._worldBuildState
        self.progressBars.world.active = state.active or false
        self.progressBars.world.progress = state.processed or 0
        self.progressBars.world.total = state.total or 0
        if state.active then
            anyActive = true
            currentProgress = currentProgress + (state.processed or 0)
        end
    end
    
    -- Update Displacement Renderer
    if RemixRenderCore and RemixRenderCore._dispBuildState then
        local state = RemixRenderCore._dispBuildState
        self.progressBars.displacement.active = state.active or false
        self.progressBars.displacement.progress = state.processed or 0
        self.progressBars.displacement.total = state.total or 0
        if state.active then
            anyActive = true
            currentProgress = currentProgress + (state.processed or 0)
        end
    end
    
    -- Update Static Props Renderer (it just shows count, no progress bar)
    if RemixRenderCore and RemixRenderCore._sprBuildState then
        local state = RemixRenderCore._sprBuildState
        self.progressBars.staticprops.active = state.active or false
        self.progressBars.staticprops.progress = state.built or 0
        self.progressBars.staticprops.total = -1 -- Indicates we should show count instead of percentage
        if state.active then
            anyActive = true
            currentProgress = currentProgress + (state.built or 0)
        end
    end
    
    -- Track progress for stuck detection
    if self.lastProgress and currentProgress > self.lastProgress then
        self.lastProgressUpdate = SysTime()
        self.isStuck = false
    end
    self.lastProgress = currentProgress
    
    -- Check for stuck state
    local timeSinceProgress = SysTime() - self.lastProgressUpdate
    if anyActive and timeSinceProgress > self.stuckThreshold then
        self.isStuck = true
    end
    
    -- Update status text with elapsed time
    local elapsed = SysTime() - self.startTime
    local statusText = string.format("Elapsed time: %.1f seconds", elapsed)
    if not anyActive then
        statusText = statusText .. " - Complete!"
    elseif self.isStuck then
        statusText = statusText .. " - Build appears stuck! (no progress for " .. math.floor(timeSinceProgress) .. "s)"
        self.statusLabel:SetTextColor(Color(255, 100, 100))
    else
        self.statusLabel:SetTextColor(Color(255, 255, 255))
    end
    self.statusLabel:SetText(statusText)
    
    -- Show force close button if stuck
    if self.isStuck then
        self.forceCloseButton:SetVisible(true)
    else
        self.forceCloseButton:SetVisible(false)
    end
    
    -- Close panel if nothing is building
    if not anyActive and elapsed > 1 then
        timer.Simple(1.5, function()
            if IsValid(self) then
                self:Close()
            end
        end)
    end
end

function PANEL:Think()
    self:UpdateProgress()
end

vgui.Register("RemixBuildProgress", PANEL, "DFrame")

-- Global state tracking
local progressPanel = nil
local lastCheckTime = 0
local isMonitoring = false

-- Check if any renderer is building and show/hide panel accordingly
local function CheckBuildState()
    local now = SysTime()
    if now - lastCheckTime < 0.1 then return end -- Check at most 10 times per second
    lastCheckTime = now
    
    local anyBuilding = false
    
    -- Check if any renderer is building
    if RemixRenderCore then
        if RemixRenderCore._worldBuildState and RemixRenderCore._worldBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._dispBuildState and RemixRenderCore._dispBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._sprBuildState and RemixRenderCore._sprBuildState.active then
            anyBuilding = true
        end
    end
    
    -- Show panel if building and not already visible
    if anyBuilding and not IsValid(progressPanel) then
        progressPanel = vgui.Create("RemixBuildProgress")
        progressPanel.startTime = SysTime()
    end
    
    -- Close panel if not building (panel closes itself after a delay)
    if not anyBuilding and IsValid(progressPanel) then
        -- Let the panel close itself to show the "Complete!" message
    end
    
    -- Stop monitoring if nothing is building
    if not anyBuilding and isMonitoring then
        hook.Remove("Think", "RemixBuildProgress_Check")
        isMonitoring = false
    end
end

-- Start monitoring for build state changes
local function StartMonitoring()
    if isMonitoring then return end
    isMonitoring = true
    hook.Add("Think", "RemixBuildProgress_Check", CheckBuildState)
end

-- Monitor build states on a timer (only check every second when idle)
timer.Create("RemixBuildProgress_IdleCheck", 1, 0, function()
    if isMonitoring then return end -- Already monitoring via Think hook
    
    local anyBuilding = false
    if RemixRenderCore then
        if RemixRenderCore._worldBuildState and RemixRenderCore._worldBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._dispBuildState and RemixRenderCore._dispBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._sprBuildState and RemixRenderCore._sprBuildState.active then
            anyBuilding = true
        end
    end
    
    if anyBuilding then
        StartMonitoring()
    end
end)

-- Clean up on disconnect
hook.Add("ShutDown", "RemixBuildProgress_Cleanup", function()
    if IsValid(progressPanel) then
        progressPanel:Close()
        progressPanel = nil
    end
end)

-- Console command to manually show progress panel (for testing)
concommand.Add("rtx_show_build_progress", function()
    if IsValid(progressPanel) then
        progressPanel:Close()
        progressPanel = nil
    end
    progressPanel = vgui.Create("RemixBuildProgress")
    progressPanel.startTime = SysTime()
    print("[Remix Build Progress] Manually showing progress panel")
end)

print("[Remix Build Progress] Loaded.")

