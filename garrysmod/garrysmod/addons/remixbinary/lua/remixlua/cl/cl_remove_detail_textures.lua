-- Detail Texture Remover using NikNaks
-- This addon replaces detail textures with dev textures after map load

-- Wait for NikNaks to load if it hasn't already
if not NikNaks then 
    require("niknaks") 
end

local enable_addon = CreateConVar("rtx_rdt_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the Detail Texture Remover addon")
local apply_delay = CreateConVar("rtx_rdt_delay", "0", FCVAR_ARCHIVE, "Delay before initial removal (seconds)")
local debug_mode = CreateConVar("rtx_rdt_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- The replacement texture - using the error texture
local replacementTexture = "rtx/ignore"

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local materialsProcessed = 0
local detailTexturesRemoved = 0

-- Timer identifier for continuous checking
local TIMER_NAME = "DetailTextureRemoverContinuous"

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        print("", ...)
    end
end

-- Flag to track whether we're running via command (show all output) or automatically (silent)
local runningFromCommand = false

-- Function to process a single material
local function ProcessMaterial(matName)
    if not matName or matName == "" then 
        return false 
    end
    
    -- Skip already processed materials
    if modifiedMaterials[matName] ~= nil then
        return modifiedMaterials[matName] -- Return true if this had a detail texture, false otherwise
    end
    
    local mat = Material(matName)
    if not mat or mat:IsError() then 
        modifiedMaterials[matName] = false
        return false 
    end
    
    local modified = false
    
    -- Check if this material has detail textures
    local detailTexture = mat:GetString("$detail")
    if detailTexture and detailTexture ~= "" then
        if runningFromCommand or debug_mode:GetBool() then
        end
        
        -- Replace the detail texture with our error texture
        mat:SetTexture("$detail", replacementTexture)
        modified = true
    end
    
    -- Check for $envmapmask
    local envmapmask = mat:GetString("$envmapmask")
    if envmapmask and envmapmask ~= "" then
        if runningFromCommand or debug_mode:GetBool() then
        end
        
        -- Replace the envmapmask with our error texture
        mat:SetTexture("$envmapmask", replacementTexture)
        modified = true
    end
    
    -- Check for $envmap
    local envmap = mat:GetString("$envmap")
    if envmap and envmap ~= "" then
        if runningFromCommand or debug_mode:GetBool() then
        end
        
        -- Replace the envmap with our error texture
        mat:SetTexture("$envmap", replacementTexture)
        modified = true
    end
    
    -- Check for $detailscale
    local detailscale = mat:GetVector("$detailscale")
    if detailscale then
        if runningFromCommand or debug_mode:GetBool() then
        end
        
        -- Set detailscale to [0 0 0]
        mat:SetVector("$detailscale", Vector(0, 0, 0))
        modified = true
    end
    
    -- Update counters
    materialsProcessed = materialsProcessed + 1
    if modified then
        detailTexturesRemoved = detailTexturesRemoved + 1
        modifiedMaterials[matName] = true
        return true
    else
        modifiedMaterials[matName] = false
    end
    
    return false
end

-- Function to process materials from BSP faces - basic version for continuous checking
local function ProcessBSPMaterialsBasic()
    -- Skip if NikNaks is not available or current map isn't loaded
    if not NikNaks or not NikNaks.CurrentMap then
        if runningFromCommand then
            error("NikNaks or CurrentMap not available - cannot process BSP materials")
        end
        return
    end
    
    -- Get the current map
    local bsp = NikNaks.CurrentMap
    
    -- Process textures from map
    for _, texture in ipairs(bsp:GetTextures()) do
        if texture then
            ProcessMaterial(texture)
        end
    end
    
    -- Process faces to get additional materials
    for _, face in pairs(bsp:GetFaces()) do
        local texture = face:GetTexture()
        if texture then
            ProcessMaterial(texture)
        end
    end
    
    -- Simple processing for static props
    for _, prop in pairs(bsp:GetStaticProps()) do
        local modelPath = prop:GetModel()
        if modelPath then
            ProcessMaterial(modelPath)
        end
    end
end

-- Function to process materials from BSP faces - thorough version for initial scan
local function ProcessBSPMaterialsThorough()
    -- Skip if NikNaks is not available or current map isn't loaded
    if not NikNaks or not NikNaks.CurrentMap then
        if runningFromCommand then
            error("NikNaks or CurrentMap not available - cannot process BSP materials")
        end
        return
    end
    
    -- Get the current map
    local bsp = NikNaks.CurrentMap
    
    -- Process textures from map
    for _, texture in ipairs(bsp:GetTextures()) do
        if texture then
            ProcessMaterial(texture)
        end
    end
    
    -- Process faces to get additional materials
    for _, face in pairs(bsp:GetFaces()) do
        local texture = face:GetTexture()
        if texture then
            ProcessMaterial(texture)
        end
    end
    
    -- Thorough processing for static props
    for _, prop in pairs(bsp:GetStaticProps()) do
        local modelPath = prop:GetModel()
        if modelPath then
            -- Get the model path
            local fullModelPath = prop.PropType
            
            -- Process the model itself
            ProcessMaterial(fullModelPath)
            
            -- Now get and process all materials associated with this model using NikNaks
            if NikNaks.ModelMaterials then
                local materials = NikNaks.ModelMaterials(fullModelPath)
                if materials then
                    for _, mat in ipairs(materials) do
                        if mat and not mat:IsError() then
                            local matName = mat:GetName()
                            if matName and matName ~= "" then
                                ProcessMaterial(matName)
                            end
                        end
                    end
                end
            end
        end
    end
end

-- Function to process all currently loaded entities
local function ProcessLoadedEntities()
    -- Process entities in the world
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            -- Process entity's materials
            if ent:GetMaterials() then
                for _, matName in ipairs(ent:GetMaterials()) do
                    ProcessMaterial(matName)
                end
            end

            -- Process model
            local modelName = ent:GetModel()
            if modelName then
                ProcessMaterial(modelName)
            end
        end
    end
end

-- Main function to remove detail textures - can be called repeatedly
local function RemoveDetailTextures(showOutput, useThorough)
    if not enable_addon:GetBool() then return 0 end
    
    -- Save previous counts to calculate new replacements
    local previousProcessed = materialsProcessed
    local previousRemoved = detailTexturesRemoved
    
    -- Set flag for debug messages
    runningFromCommand = showOutput or false
    
    local startTime = SysTime()
    if runningFromCommand or debug_mode:GetBool() then
        DebugPrint("Starting detail texture removal...")
    end
    
    -- Process BSP materials - using appropriate method
    if NikNaks and NikNaks.CurrentMap then
        if useThorough then
            ProcessBSPMaterialsThorough()
        else
            ProcessBSPMaterialsBasic()
        end
    else
        if runningFromCommand then
            error("NikNaks or CurrentMap not available - cannot process detail textures")
        end
        return 0
    end
    
    -- Process all loaded entities
    ProcessLoadedEntities()
    
    -- Reset flag
    runningFromCommand = false
    
    local endTime = SysTime()
    local processingTime = math.Round((endTime - startTime) * 1000)
    
    -- Calculate new textures found
    local newProcessed = materialsProcessed - previousProcessed
    local newRemoved = detailTexturesRemoved - previousRemoved
    
    -- Only report when new textures are found or when explicitly requested
    if newRemoved > 0 then
        if showOutput or debug_mode:GetBool() then
            DebugPrint("Removed " .. newRemoved .. " detail textures in " .. processingTime .. "ms")
        end
    elseif showOutput then
        DebugPrint("No new detail textures found. Checked " .. newProcessed .. " materials in " .. processingTime .. "ms")
    end
    
    return newRemoved
end

-- Function to force reapply to already processed materials
local function ForceReapply()
    local count = 0
    -- Go through all materials we've already identified
    for matName, hadDetail in pairs(modifiedMaterials) do
        if hadDetail then
            local mat = Material(matName)
            if mat and not mat:IsError() then
                -- Reapply our replacements
                mat:SetTexture("$detail", replacementTexture)
                
                -- Check for $envmapmask
                local envmapmask = mat:GetString("$envmapmask")
                if envmapmask and envmapmask ~= "" then
                    mat:SetTexture("$envmapmask", replacementTexture)
                end
                
                -- Check for $envmap
                local envmap = mat:GetString("$envmap")
                if envmap and envmap ~= "" then
                    mat:SetTexture("$envmap", replacementTexture)
                end
                
                -- Check for $detailscale
                local detailscale = mat:GetVector("$detailscale")
                if detailscale then
                    mat:SetVector("$detailscale", Vector(0, 0, 0))
                end
                
                count = count + 1
            end
        end
    end
    
    return count
end

-- Add command to force reapply texture replacements
concommand.Add("force_reapply_dtexture", function()
    local count = ForceReapply()
    print("Force reapplied texture replacements to " .. count .. " materials")
    notification.AddLegacy("Reapplied replacements to " .. count .. " materials", NOTIFY_GENERIC, 3)
end)

-- Modify the continuous checking timer to use the basic method
local function StartContinuousChecking()
    if timer.Exists(TIMER_NAME) then 
        timer.Remove(TIMER_NAME)
    end
    
    -- Create a timer that runs every 2 seconds using the basic method
    timer.Create(TIMER_NAME, 2, 0, function() 
        RemoveDetailTextures(false, false) -- Use basic method
    end)
end

-- Stop the continuous checking
local function StopContinuousChecking()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
end

-- Function to handle when the enable cvar changes
cvars.AddChangeCallback("rtx_rdt_enabled", function(_, _, new)
    if new == "1" then
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay after NikNaks is fully loaded
hook.Add("InitPostEntity", "RemoveDetailTexturesOnMapLoad", function()
    if enable_addon:GetBool() then
        -- First run after map loads
        timer.Simple(apply_delay:GetFloat(), function()
            -- Ensure NikNaks and Map are ready
            if not NikNaks or not NikNaks.CurrentMap then
                print("[G] Waiting for NikNaks to fully load...")
                timer.Simple(2, function()
                    if not NikNaks or not NikNaks.CurrentMap then
                        error("[gmRTX - RDT] NikNaks or CurrentMap not available after waiting")
                        return
                    end
                    local found = RemoveDetailTextures(true, true) -- Use thorough method for initial scan
                    StartContinuousChecking()
                end)
                return
            end
            
            local found = RemoveDetailTextures(true, true) -- Use thorough method for initial scan
            
            -- Start continuous checking after initial scan
            StartContinuousChecking()
        end)
    end
end)

-- Reset variables on map change
hook.Add("ShutDown", "CleanupDetailTextureRemover", function()
    StopContinuousChecking()
    modifiedMaterials = {}
    materialsProcessed = 0
    detailTexturesRemoved = 0
end)

-- Add command to manually trigger detail texture removal
concommand.Add("rtx_rdt_remove", function()
    local oldCount = detailTexturesRemoved
    RemoveDetailTextures(true, true)
    local newCount = detailTexturesRemoved - oldCount
    
    notification.AddLegacy("Replaced " .. newCount .. " new detail textures", NOTIFY_GENERIC, 3)
end)

print("[gmRTX - RDT] Addon loaded. Will replace detail textures with " .. replacementTexture)