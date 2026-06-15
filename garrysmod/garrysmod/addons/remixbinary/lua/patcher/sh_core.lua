RTXPatcher = RTXPatcher or {}

local Patcher = RTXPatcher
Patcher.Version = "1"
Patcher.Patches = Patcher.Patches or {}
Patcher.State = Patcher.State or {}
Patcher.State.pending = Patcher.State.pending or {}
Patcher.State.applied = Patcher.State.applied or {}
Patcher.State.failed = Patcher.State.failed or {}
Patcher.State.logs = Patcher.State.logs or {}
Patcher.State.functionIntercepts = Patcher.State.functionIntercepts or {}
Patcher.State.hookIntercepts = Patcher.State.hookIntercepts or {}

local function makeFallbackConVar(default)
    return {
        GetBool = function() return tostring(default) ~= "0" end,
        GetString = function() return tostring(default) end,
        GetFloat = function() return tonumber(default) or 0 end,
        GetInt = function() return tonumber(default) or 0 end
    }
end

local function getExistingConVar(name)
    if GetConVar then
        local convar = GetConVar(name)
        if convar then
            return convar
        end
    end
end

local function createConVar(name, default, help)
    if CLIENT and CreateClientConVar then
        local convar = CreateClientConVar(name, default, true, false, help)
        if convar then
            return convar
        end

        convar = getExistingConVar(name)
        if convar then
            return convar
        end
    end

    if CreateConVar then
        local convar = CreateConVar(name, default, FCVAR_ARCHIVE, help)
        if convar then
            return convar
        end

        convar = getExistingConVar(name)
        if convar then
            return convar
        end
    end

    local convar = getExistingConVar(name)
    if convar then
        return convar
    end

    return makeFallbackConVar(default)
end

Patcher.ConVars = Patcher.ConVars or {}
Patcher.ConVars.enabled = Patcher.ConVars.enabled or createConVar("rtx_patcher_enabled", "1", "Enable runtime addon compatibility patches")
Patcher.ConVars.debug = Patcher.ConVars.debug or createConVar("rtx_patcher_debug", "0", "Enable runtime addon patcher debug logging")

local function currentSide()
    if CLIENT then return "client" end
    if SERVER then return "server" end
    return "shared"
end

local function sideMatches(side)
    if not side or side == "shared" then return true end
    return side == currentSide()
end

local function sanitizeId(id)
    local value = tostring(id)
    local bytes = {}

    for i = 1, #value do
        bytes[#bytes + 1] = string.format("%02X", string.byte(value, i))
    end

    return table.concat(bytes, "")
end

function Patcher.Debug(...)
    local parts = {}
    for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring(select(i, ...))
    end

    local line = table.concat(parts, " ")
    Patcher.State.logs[#Patcher.State.logs + 1] = line

    if Patcher.ConVars.debug:GetBool() then
        print("[RTXPatcher] " .. line)
    end
end

function Patcher.IsEnabled()
    return Patcher.ConVars.enabled:GetBool()
end

function Patcher.MarkFailed(id, reason)
    local spec = Patcher.Patches[id]
    if spec then
        spec._retryScheduled = nil
    end

    Patcher.State.pending[id] = nil
    Patcher.State.applied[id] = nil
    Patcher.State.failed[id] = {
        state = "failed",
        reason = tostring(reason),
        time = RealTime and RealTime() or 0
    }
    Patcher.Debug(id, "failed:", reason)
    return false, reason
end

local function markPending(id, reason)
    Patcher.State.applied[id] = nil
    Patcher.State.failed[id] = nil
    Patcher.State.pending[id] = {
        state = "pending",
        reason = tostring(reason or "dependencies not ready"),
        time = RealTime and RealTime() or 0
    }
end

local function markApplied(id)
    Patcher.State.pending[id] = nil
    Patcher.State.failed[id] = nil
    Patcher.State.applied[id] = {
        state = "applied",
        time = RealTime and RealTime() or 0
    }
end

function Patcher.ScheduleRetry(id, reason)
    local spec = Patcher.Patches[id]
    if not spec then return false, "unknown patch" end

    markPending(id, reason)

    if spec._retryScheduled then
        return false, reason
    end

    spec._attempts = (spec._attempts or 0) + 1

    local maxAttempts = spec.maxAttempts
    if maxAttempts ~= nil and maxAttempts >= 0 and spec._attempts > maxAttempts then
        return Patcher.MarkFailed(id, reason or "maximum dependency retries reached")
    end

    if timer and timer.Create then
        local timerName = "RTXPatcher_Retry_" .. sanitizeId(id)
        spec._retryScheduled = true
        timer.Create(timerName, spec.retryInterval or 0.5, 1, function()
            spec._retryScheduled = nil
            Patcher.TryApply(id)
        end)
    end

    return false, reason
end

function Patcher.TryApply(id)
    local spec = Patcher.Patches[id]
    if not spec then return false, "unknown patch" end

    local failed = Patcher.State.failed[id]
    if failed then
        return false, failed.reason
    end

    if Patcher.State.applied[id] then
        return true
    end

    if not Patcher.IsEnabled() then
        markPending(id, "rtx_patcher_enabled is disabled")
        return false, "disabled"
    end

    if not sideMatches(spec.side) then
        markPending(id, "wrong realm")
        return false, "wrong realm"
    end

    if spec.enabled then
        local enabledOk, enabled = pcall(spec.enabled)
        if not enabledOk then
            return Patcher.MarkFailed(id, enabled)
        end
        if not enabled then
            markPending(id, "patch convar disabled")
            return false, "patch disabled"
        end
    end

    if spec.depends then
        local dependsOk, ready, reasonOrContext = pcall(spec.depends)
        if not dependsOk then
            return Patcher.MarkFailed(id, ready)
        end
        if not ready then
            return Patcher.ScheduleRetry(id, reasonOrContext or "dependencies not ready")
        end
        spec._context = reasonOrContext
    end

    local applyOk, err = pcall(spec.apply, Patcher, spec, spec._context)
    if not applyOk then
        return Patcher.MarkFailed(id, err)
    end

    spec._retryScheduled = nil
    markApplied(id)
    Patcher.Debug(id, "applied")
    return true
end

function Patcher.TryApplyAll()
    for id in pairs(Patcher.Patches) do
        Patcher.TryApply(id)
    end
end

function Patcher.RegisterPatch(spec)
    if type(spec) ~= "table" then
        error("RTXPatcher.RegisterPatch expected table", 2)
    end
    if type(spec.id) ~= "string" or spec.id == "" then
        error("RTXPatcher.RegisterPatch requires non-empty id", 2)
    end
    if type(spec.apply) ~= "function" then
        error("RTXPatcher.RegisterPatch requires apply function", 2)
    end

    if Patcher.State.applied[spec.id] then
        spec._attempts = spec._attempts or 0
        Patcher.Patches[spec.id] = spec
        Patcher.Debug(spec.id, "already applied")
        return true
    end

    spec._attempts = spec._attempts or 0
    Patcher.Patches[spec.id] = spec
    markPending(spec.id, "registered")
    Patcher.Debug(spec.id, "registered")
    return Patcher.TryApply(spec.id)
end

function Patcher.GetStatus()
    local status = {
        enabled = Patcher.IsEnabled(),
        patches = {}
    }

    for id, spec in pairs(Patcher.Patches) do
        local state = Patcher.State.applied[id] or Patcher.State.failed[id] or Patcher.State.pending[id]
        status.patches[id] = {
            id = id,
            label = spec.label or id,
            state = state and state.state or "registered",
            reason = state and state.reason or nil
        }
    end

    return status
end

function Patcher.PrintStatus()
    local status = Patcher.GetStatus()
    print("[RTXPatcher] enabled=" .. tostring(status.enabled))
    for id, patch in pairs(status.patches) do
        local suffix = patch.reason and (" (" .. patch.reason .. ")") or ""
        print("[RTXPatcher] " .. id .. ": " .. patch.state .. suffix)
    end
end

if concommand and concommand.Add and not (_G and _G.RTXPatcher_StatusCommandAdded) then
    concommand.Add("rtx_patcher_status", function()
        if RTXPatcher and RTXPatcher.PrintStatus then
            RTXPatcher.PrintStatus()
        end
    end)
    if _G then
        _G.RTXPatcher_StatusCommandAdded = true
    end
end

return Patcher
