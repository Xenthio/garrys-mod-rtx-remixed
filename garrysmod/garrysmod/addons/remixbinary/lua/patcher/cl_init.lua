if not CLIENT then return end

include("patcher/sh_core.lua")
include("patcher/cl_intercepts.lua")

local files = {}
if file and file.Find then
    files = file.Find("patcher/addons/*.lua", "LUA") or {}
end

table.sort(files)

for _, fileName in ipairs(files) do
    local path = "patcher/addons/" .. fileName
    local ok, err = pcall(include, path)
    if not ok and RTXPatcher and RTXPatcher.Debug then
        RTXPatcher.Debug("failed to include", path, err)
    end
end

if RTXPatcher then
    RTXPatcher.TryApplyAll()
end
