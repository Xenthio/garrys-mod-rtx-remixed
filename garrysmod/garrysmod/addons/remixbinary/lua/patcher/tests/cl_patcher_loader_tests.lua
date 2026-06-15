if not CLIENT then return end

RTXPatcher = nil
include("patcher/cl_init.lua")

if not RTXPatcher then
    error("RTXPatcher was not created")
end

if type(RTXPatcher.RegisterPatch) ~= "function" then
    error("RTXPatcher.RegisterPatch missing")
end

if type(RTXPatcher.InterceptFunction) ~= "function" then
    error("RTXPatcher.InterceptFunction missing")
end

print("[RTXPatcherTests] PASS")
