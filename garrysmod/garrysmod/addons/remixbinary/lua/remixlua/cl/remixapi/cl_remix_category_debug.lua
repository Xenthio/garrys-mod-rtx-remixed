--[[
    RTX Remix Category Manager - Debug Utilities
    
    Provides debugging and visualization tools for the category management system
]]--

-- Wait for RemixCategoryManager to be available
if not RemixCategoryManager then
    MsgC(Color(255, 200, 100), "[RemixCategoryDebug] Waiting for RemixCategoryManager to load...\n")
    
    -- Defer loading until manager is available
    hook.Add("Think", "RemixCategoryDebug_WaitForManager", function()
        if RemixCategoryManager then
            hook.Remove("Think", "RemixCategoryDebug_WaitForManager")
            include("remixlua/cl/remixapi/cl_remix_category_debug.lua")
        end
    end)
    
    return
end

RemixCategoryDebug = RemixCategoryDebug or {}

-- Color helper
local function ColorPrint(color, ...)
    MsgC(color, ...)
end

--[[
    Print all category flags in a readable format
]]--
function RemixCategoryDebug.PrintCategoryFlags(flags)
    if not flags or flags == 0 then
        ColorPrint(Color(200, 200, 200), "No flags set (0x0)\n")
        return
    end
    
    ColorPrint(Color(100, 200, 255), string.format("Category Flags: 0x%X (%d)\n", flags, flags))
    ColorPrint(Color(200, 200, 200), "Active flags:\n")
    
    for name, value in pairs(RemixCategoryManager.CATEGORY) do
        if bit.band(flags, value) ~= 0 then
            ColorPrint(Color(100, 255, 100), string.format("  - %s (0x%X)\n", name, value))
        end
    end
end

--[[
    List all materials with their hashes and categories
]]--
function RemixCategoryDebug.ListMaterialCategories()
    local materials = RemixMaterial.GetCachedMaterials()
    
    ColorPrint(Color(100, 200, 255), string.format("=== Material Categories (%d materials) ===\n", #materials))
    
    local categorized = 0
    for i, materialName in ipairs(materials) do
        local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
        
        if hashStr then
            local category = RemixMaterial.GetHashCategory(hashStr)
            
            if category then
                categorized = categorized + 1
                ColorPrint(Color(200, 200, 200), string.format("%3d. ", i))
                ColorPrint(Color(255, 255, 100), materialName)
                ColorPrint(Color(150, 150, 150), string.format(" [%s]", hashStr))
                ColorPrint(Color(100, 255, 100), string.format(" -> 0x%X\n", category))
            end
        end
    end
    
    ColorPrint(Color(100, 200, 255), string.format("Total: %d categorized out of %d materials\n", categorized, #materials))
end

--[[
    Show statistics about categorized materials
]]--
function RemixCategoryDebug.ShowStatistics()
    local materials = RemixMaterial.GetCachedMaterials()
    
    local stats = {
        total = #materials,
        withHash = 0,
        withCategory = 0,
        byCategory = {}
    }
    
    -- Initialize category counters
    for name, value in pairs(RemixCategoryManager.CATEGORY) do
        stats.byCategory[name] = 0
    end
    
    -- Count materials
    for _, materialName in ipairs(materials) do
        local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
        
        if hashStr then
            stats.withHash = stats.withHash + 1
            
            local category = RemixMaterial.GetHashCategory(hashStr)
            if category then
                stats.withCategory = stats.withCategory + 1
                
                -- Count individual flags
                for name, value in pairs(RemixCategoryManager.CATEGORY) do
                    if bit.band(category, value) ~= 0 then
                        stats.byCategory[name] = stats.byCategory[name] + 1
                    end
                end
            end
        end
    end
    
    -- Print statistics
    ColorPrint(Color(100, 200, 255), "=== Category Statistics ===\n")
    ColorPrint(Color(200, 200, 200), string.format("Total materials: %d\n", stats.total))
    ColorPrint(Color(200, 200, 200), string.format("Materials with hash: %d (%.1f%%)\n", 
        stats.withHash, (stats.withHash / stats.total) * 100))
    ColorPrint(Color(200, 200, 200), string.format("Materials with category: %d (%.1f%%)\n", 
        stats.withCategory, (stats.total > 0 and (stats.withCategory / stats.total) * 100 or 0)))
    
    ColorPrint(Color(100, 200, 255), "\nCategory usage:\n")
    
    -- Sort by usage
    local sorted = {}
    for name, count in pairs(stats.byCategory) do
        if count > 0 then
            table.insert(sorted, {name = name, count = count})
        end
    end
    table.sort(sorted, function(a, b) return a.count > b.count end)
    
    for _, entry in ipairs(sorted) do
        ColorPrint(Color(100, 255, 100), string.format("  %s: ", entry.name))
        ColorPrint(Color(255, 255, 100), string.format("%d materials\n", entry.count))
    end
end

--[[
    Search for materials by pattern
]]--
function RemixCategoryDebug.SearchMaterials(pattern)
    local materials = RemixMaterial.GetCachedMaterials()
    local matches = {}
    
    pattern = string.lower(pattern)
    
    for _, materialName in ipairs(materials) do
        if string.find(string.lower(materialName), pattern, 1, true) then
            table.insert(matches, materialName)
        end
    end
    
    ColorPrint(Color(100, 200, 255), string.format("=== Search Results for '%s' (%d matches) ===\n", pattern, #matches))
    
    for i, materialName in ipairs(matches) do
        local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
        
        ColorPrint(Color(200, 200, 200), string.format("%3d. ", i))
        ColorPrint(Color(255, 255, 100), materialName)
        
        if hashStr then
            ColorPrint(Color(150, 150, 150), string.format(" [%s]", hashStr))
            
            local category = RemixMaterial.GetHashCategory(hashStr)
            if category then
                ColorPrint(Color(100, 255, 100), string.format(" -> 0x%X", category))
            end
        end
        
        ColorPrint(Color(255, 255, 255), "\n")
    end
    
    return matches
end

--[[
    Test if a specific material has correct category
]]--
function RemixCategoryDebug.TestMaterial(materialName)
    ColorPrint(Color(100, 200, 255), string.format("=== Testing Material: %s ===\n", materialName))
    
    -- Check if material exists
    local mat = Material(materialName)
    if mat:IsError() then
        ColorPrint(Color(255, 100, 100), "ERROR: Material not found or is error material!\n")
        return false
    end
    
    ColorPrint(Color(100, 255, 100), "Material exists: OK\n")
    
    -- Track it
    ColorPrint(Color(200, 200, 200), "Tracking material...\n")
    RemixMaterial.TrackMaterial(materialName)
    
    -- Get hash
    timer.Simple(0.5, function()
        local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
        
        if not hashStr then
            ColorPrint(Color(255, 100, 100), "ERROR: Could not get texture hash!\n")
            ColorPrint(Color(255, 200, 100), "Tip: Make sure the material is visible on screen\n")
            return
        end
        
        ColorPrint(Color(100, 255, 100), string.format("Hash: %s (%.0f)\n", hashStr, hashNum))
        
        -- Check category
        local category = RemixMaterial.GetHashCategory(hashStr)
        if category then
            ColorPrint(Color(100, 255, 100), "Category is set:\n")
            RemixCategoryDebug.PrintCategoryFlags(category)
        else
            ColorPrint(Color(255, 200, 100), "No category set for this material\n")
        end
    end)
    
    return true
end

--[[
    Export category mappings to a file
]]--
function RemixCategoryDebug.ExportMappings(filename)
    filename = filename or "remix_categories_export.txt"
    
    local materials = RemixMaterial.GetCachedMaterials()
    local lines = {}
    
    table.insert(lines, "# RTX Remix Category Mappings")
    table.insert(lines, "# Generated: " .. os.date("%Y-%m-%d %H:%M:%S"))
    table.insert(lines, "# Map: " .. game.GetMap())
    table.insert(lines, "")
    
    local count = 0
    for _, materialName in ipairs(materials) do
        local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
        
        if hashStr then
            local category = RemixMaterial.GetHashCategory(hashStr)
            
            if category then
                count = count + 1
                table.insert(lines, string.format("%s|%s|0x%X", materialName, hashStr, category))
            end
        end
    end
    
    table.insert(lines, "")
    table.insert(lines, string.format("# Total: %d categorized materials", count))
    
    local content = table.concat(lines, "\n")
    file.Write(filename, content)
    
    ColorPrint(Color(100, 255, 100), string.format("Exported %d category mappings to: %s\n", count, filename))
    return filename
end

--[[
    Import category mappings from a file
]]--
function RemixCategoryDebug.ImportMappings(filename)
    filename = filename or "remix_categories_export.txt"
    
    local content = file.Read(filename, "DATA")
    if not content then
        ColorPrint(Color(255, 100, 100), string.format("ERROR: Could not read file: %s\n", filename))
        return false
    end
    
    local lines = string.Explode("\n", content)
    local count = 0
    
    for _, line in ipairs(lines) do
        -- Skip comments and empty lines
        if not string.StartsWith(line, "#") and line ~= "" then
            local parts = string.Explode("|", line)
            
            if #parts == 3 then
                local materialName = parts[1]
                local hashStr = parts[2]
                local categoryStr = parts[3]
                
                local category = tonumber(categoryStr)
                if category then
                    RemixMaterial.SetHashCategory(hashStr, category)
                    count = count + 1
                end
            end
        end
    end
    
    ColorPrint(Color(100, 255, 100), string.format("Imported %d category mappings from: %s\n", count, filename))
    return true
end

-- Console commands for debugging

concommand.Add("remix_debug_list_categories", function()
    RemixCategoryDebug.ListMaterialCategories()
end, nil, "List all materials with their categories")

concommand.Add("remix_debug_stats", function()
    RemixCategoryDebug.ShowStatistics()
end, nil, "Show category usage statistics")

concommand.Add("remix_debug_search", function(ply, cmd, args)
    if not args[1] then
        ColorPrint(Color(255, 200, 100), "Usage: remix_debug_search <pattern>\n")
        ColorPrint(Color(255, 200, 100), "Example: remix_debug_search concrete\n")
        return
    end
    
    RemixCategoryDebug.SearchMaterials(args[1])
end, nil, "Search for materials by name pattern")

concommand.Add("remix_debug_test_material", function(ply, cmd, args)
    if not args[1] then
        ColorPrint(Color(255, 200, 100), "Usage: remix_debug_test_material <material_name>\n")
        ColorPrint(Color(255, 200, 100), "Example: remix_debug_test_material materials/concrete/concrete.vmt\n")
        return
    end
    
    RemixCategoryDebug.TestMaterial(args[1])
end, nil, "Test a specific material's hash and category")

concommand.Add("remix_debug_export", function(ply, cmd, args)
    local filename = args[1]
    RemixCategoryDebug.ExportMappings(filename)
end, nil, "Export category mappings to a file")

concommand.Add("remix_debug_import", function(ply, cmd, args)
    if not args[1] then
        ColorPrint(Color(255, 200, 100), "Usage: remix_debug_import <filename>\n")
        return
    end
    
    RemixCategoryDebug.ImportMappings(args[1])
end, nil, "Import category mappings from a file")

concommand.Add("remix_debug_print_flags", function(ply, cmd, args)
    if not args[1] then
        ColorPrint(Color(255, 200, 100), "Usage: remix_debug_print_flags <flags_hex>\n")
        ColorPrint(Color(255, 200, 100), "Example: remix_debug_print_flags 0x800\n")
        return
    end
    
    local flags = tonumber(args[1])
    if not flags then
        ColorPrint(Color(255, 100, 100), "Invalid flags: " .. args[1] .. "\n")
        return
    end
    
    RemixCategoryDebug.PrintCategoryFlags(flags)
end, nil, "Print category flags in readable format")

ColorPrint(Color(100, 255, 100), "[RemixCategoryDebug] Loaded!\n")
ColorPrint(Color(200, 200, 200), "[RemixCategoryDebug] Commands: remix_debug_stats, remix_debug_list_categories, remix_debug_search, etc.\n")

return RemixCategoryDebug
