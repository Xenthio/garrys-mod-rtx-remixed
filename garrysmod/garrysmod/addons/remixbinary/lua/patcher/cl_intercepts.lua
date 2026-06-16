if not CLIENT then return end

RTXPatcher = RTXPatcher or {}

local Patcher = RTXPatcher
local unpackValues = unpack or table.unpack

Patcher.State = Patcher.State or {}
Patcher.State.functionIntercepts = Patcher.State.functionIntercepts or {}
Patcher.State.hookIntercepts = Patcher.State.hookIntercepts or {}

local function packValues(...)
    local results = { ... }
    results.n = select("#", ...)
    return results
end

local function resultCount(results)
    if type(results) ~= "table" then
        return 0
    end

    return results.n or #results
end

local function encodePart(value)
    local text = tostring(value)
    local bytes = {}

    for i = 1, #text do
        bytes[#bytes + 1] = string.format("%02X", string.byte(text, i))
    end

    return table.concat(bytes, "")
end

local function makeFunctionStateKey(patchId, id, target, key)
    return encodePart(patchId) .. ":" .. encodePart(id) .. ":" .. encodePart(target) .. ":" .. encodePart(key)
end

local function makeHookId(patchId, hookId)
    return "RTXPatcher_" .. encodePart(patchId) .. "_" .. encodePart(hookId)
end

local function makeHookStateKey(eventName, patchId, hookId)
    return encodePart(eventName) .. ":" .. encodePart(patchId) .. ":" .. encodePart(hookId)
end

local function debug(...)
    if Patcher.Debug then
        Patcher.Debug(...)
    end
end

function Patcher.InterceptFunction(target, key, options)
    if type(target) ~= "table" then
        return false, "target is not a table"
    end

    options = options or {}

    local patchId = options.patchId or "default"
    local id = options.id or key
    local stateKey = makeFunctionStateKey(patchId, id, target, key)

    if Patcher.State.functionIntercepts[stateKey] then
        return true, stateKey
    end

    local original = target[key]
    if type(original) ~= "function" then
        return false, tostring(key) .. " is not a function"
    end

    local wrapper
    wrapper = function(...)
        if type(options.around) == "function" then
            local results = packValues(options.around(original, ...))
            return unpackValues(results, 1, results.n)
        end

        if type(options.before) == "function" then
            options.before(...)
        end

        local results = packValues(original(...))

        if type(options.after) == "function" then
            local replacement = options.after(results, ...)
            if type(replacement) == "table" then
                results = replacement
            end
        end

        return unpackValues(results, 1, resultCount(results))
    end
    target[key] = wrapper

    Patcher.State.functionIntercepts[stateKey] = {
        target = target,
        key = key,
        original = original,
        wrapper = wrapper,
        patchId = patchId,
        id = id
    }

    debug(patchId, id, "intercepted", key)
    return true, stateKey
end

function Patcher.RestoreFunctionIntercept(stateKey)
    local state = Patcher.State.functionIntercepts[stateKey]
    if not state then
        return false, "unknown function intercept"
    end

    if state.target[state.key] ~= state.wrapper then
        return false, "intercept is not current"
    end

    state.target[state.key] = state.original
    Patcher.State.functionIntercepts[stateKey] = nil

    debug(state.patchId, state.id, "restored", state.key)
    return true
end

function Patcher.InterceptHook(eventName, hookId, fn, options)
    if not hook or type(hook.Add) ~= "function" then
        return false, "hook library unavailable"
    end
    if type(fn) ~= "function" then
        return false, "fn is not a function"
    end

    options = options or {}

    local patchId = options.patchId or "default"
    local generatedHookId = makeHookId(patchId, hookId)
    local stateKey = makeHookStateKey(eventName, patchId, hookId)

    if Patcher.State.hookIntercepts[stateKey] then
        return true, stateKey
    end

    hook.Add(eventName, generatedHookId, fn)

    Patcher.State.hookIntercepts[stateKey] = {
        eventName = eventName,
        hookId = generatedHookId,
        originalHookId = hookId,
        fn = fn,
        patchId = patchId
    }

    debug(patchId, hookId, "hook intercepted", eventName)
    return true, stateKey
end

return Patcher
