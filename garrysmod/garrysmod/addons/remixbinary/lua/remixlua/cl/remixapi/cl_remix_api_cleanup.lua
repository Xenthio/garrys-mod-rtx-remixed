if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

local RTX_CLEANUP_HOOKS = {
    "InitPostEntity",
    "OnReloaded",
    "OnMapStart",
    "ShutDown"
}

-- Add ConVar for debugging
local cv_debug_cleanup = CreateClientConVar("rtx_debug_cleanup", "0", true, false, "Show debug messages for RTX cleanup")

local function DebugMsg(msg)
    if cv_debug_cleanup:GetBool() then
        print("[gmRTX - Remix Cleanup] " .. msg)
    end
end

-- Guard to prevent concurrent cleanup operations
local cleanupInProgress = false

-- Function to handle cleanup
local function CleanupRTX()
    -- Prevent concurrent cleanup
    if cleanupInProgress then
        DebugMsg("Cleanup already in progress, skipping...")
        return
    end
    
    cleanupInProgress = true
    
    -- Use pcall to ensure lock is always released
    local success, err = pcall(function()
        DebugMsg("Starting RTX cleanup...")
        
        -- Clear existing meshes first
        if mapMeshes then
        for renderType, chunks in pairs(mapMeshes) do
            for chunkKey, materials in pairs(chunks) do
                for matName, group in pairs(materials) do
                    if group.meshes then
                        for _, mesh in ipairs(group.meshes) do
                            if mesh and mesh.Destroy then
                                mesh:Destroy()
                            end
                        end
                    end
                end
            end
        end
    end
    
    -- Reset mesh tables
    mapMeshes = {
        opaque = {},
        translucent = {},
    }
    
    -- Clear material cache
    materialCache = {}
    
        -- Force GC to clean up Lua resources
        collectgarbage("collect")
        
        -- Call native cleanup using new RemixResource API
        local nativeSuccess = false
        if RemixResource and RemixResource.ClearResources then
            nativeSuccess = RemixResource.ClearResources()
        else
            DebugMsg("RemixResource API not available")
        end
        
        if nativeSuccess then
            DebugMsg("RTX cleanup completed successfully")
        else
            DebugMsg("RTX cleanup failed!")
        end
        
        -- Wait a frame before rebuilding
        timer.Simple(0, function()
            if BuildMapMeshes then
                BuildMapMeshes()
            end
        end)
    end)
    
    -- Release the lock even if cleanup failed
    cleanupInProgress = false
    
    if not success then
        ErrorNoHalt("[gmRTX - Remix Cleanup] Error during cleanup: " .. tostring(err) .. "\n")
    end
end

-- Add cleanup hooks
for _, hookName in ipairs(RTX_CLEANUP_HOOKS) do
    hook.Add(hookName, "RTXCleanup", function()
        DebugMsg("Hook: " .. hookName)
        CleanupRTX()
    end)
end

-- Add console command for manual cleanup
concommand.Add("rtx_cleanup", function()
    CleanupRTX()
end)

-- Add to menu
hook.Add("PreCleanupMap", "RTXCleanupMapChange", function()
    DebugMsg("Pre-cleanup map detected")
    CleanupRTX()
end)

hook.Add("PostCleanupMap", "RTXCleanupMapChange", function()
    DebugMsg("Post-cleanup map detected")
    CleanupRTX()
end)

hook.Add("OnMapLoadFinished", "RTXCleanupMapChange", function()
    DebugMsg("Map load finished")
    CleanupRTX()
end)

-- Optional: Add cleanup on significant memory usage
local lastCleanupTime = 0
local CLEANUP_INTERVAL = 3600 -- 1 hour minimum between auto-cleanups

hook.Add("Think", "RTXMemoryMonitor", function()
    -- Don't check too often
    if CurTime() - lastCleanupTime < CLEANUP_INTERVAL then return end
    
    -- Get memory info from Lua
    local mem = collectgarbage("count")
    
    -- If memory usage is high, trigger cleanup
    if mem > 256000 then -- 250MB in KB
        DebugMsg("High memory usage detected: " .. math.floor(mem/1024) .. "MB")
        CleanupRTX()
        lastCleanupTime = CurTime()
    end
end)