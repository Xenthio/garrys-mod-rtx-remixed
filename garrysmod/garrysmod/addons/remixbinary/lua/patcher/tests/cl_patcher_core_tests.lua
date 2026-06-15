if not CLIENT then return end

local function assertEquals(actual, expected, message)
    if actual ~= expected then
        error((message or "assertEquals failed") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual), 2)
    end
end

local function assertTruthy(value, message)
    if not value then
        error(message or "assertTruthy failed", 2)
    end
end

local function freshCore()
    RTXPatcher = nil
    include("patcher/sh_core.lua")
    return RTXPatcher
end

local tests = {}

local function test(name, fn)
    tests[#tests + 1] = {
        name = name,
        fn = fn,
    }
end

test("core fills missing state subtables", function()
    local logs = {}

    RTXPatcher = {
        State = {
            logs = logs,
        },
    }

    include("patcher/sh_core.lua")

    assertEquals(type(RTXPatcher.State.pending), "table", "pending table should exist")
    assertEquals(type(RTXPatcher.State.applied), "table", "applied table should exist")
    assertEquals(type(RTXPatcher.State.failed), "table", "failed table should exist")
    assertEquals(RTXPatcher.State.logs, logs, "existing logs table should be preserved")
    assertEquals(type(RTXPatcher.State.functionIntercepts), "table", "functionIntercepts table should exist")
    assertEquals(type(RTXPatcher.State.hookIntercepts), "table", "hookIntercepts table should exist")
end)

test("core reuses existing client convars on hard reload", function()
    local oldCreateClientConVar = CreateClientConVar
    local oldGetConVar = GetConVar
    local stored = {}

    CreateClientConVar = function(name, default)
        if stored[name] then
            return nil
        end

        local convar = {
            GetBool = function() return tostring(default) ~= "0" end,
            GetString = function() return tostring(default) end,
            GetFloat = function() return tonumber(default) or 0 end,
            GetInt = function() return tonumber(default) or 0 end,
        }

        stored[name] = convar
        return convar
    end

    GetConVar = function(name)
        return stored[name]
    end

    local ok, err = pcall(function()
        RTXPatcher = nil
        include("patcher/sh_core.lua")

        local enabled = RTXPatcher.ConVars.enabled
        assertTruthy(enabled, "first include should create enabled convar")

        RTXPatcher = nil
        include("patcher/sh_core.lua")

        assertEquals(RTXPatcher.ConVars.enabled, enabled, "second include should reuse existing enabled convar")
        assertTruthy(RTXPatcher.ConVars.enabled.GetBool, "reused enabled convar should be usable")
        assertEquals(RTXPatcher.IsEnabled(), true, "reused enabled convar should report enabled")
    end)

    CreateClientConVar = oldCreateClientConVar
    GetConVar = oldGetConVar

    if not ok then
        error(err, 0)
    end
end)

test("status command uses current global patcher after hard reload", function()
    local oldConcommand = concommand
    local oldPrint = print
    local oldStatusCommandAdded = _G and _G.RTXPatcher_StatusCommandAdded or nil
    local callbacks = {}
    local output = {}

    if _G then
        _G.RTXPatcher_StatusCommandAdded = nil
    end

    concommand = {
        Add = function(name, fn)
            if not callbacks[name] then
                callbacks[name] = fn
            end
        end,
    }

    print = function(line)
        output[#output + 1] = tostring(line)
    end

    local ok, err = pcall(function()
        RTXPatcher = nil
        include("patcher/sh_core.lua")
        RTXPatcher.RegisterPatch({
            id = "test.first_command",
            label = "First Command Test",
            side = "client",
            enabled = function()
                return true
            end,
            depends = function()
                return true
            end,
            apply = function() end,
            maxAttempts = 0,
        })

        RTXPatcher = nil
        include("patcher/sh_core.lua")
        RTXPatcher.RegisterPatch({
            id = "test.second_command",
            label = "Second Command Test",
            side = "client",
            enabled = function()
                return true
            end,
            depends = function()
                return true
            end,
            apply = function() end,
            maxAttempts = 0,
        })

        assertTruthy(callbacks.rtx_patcher_status, "status command callback should be stored")
        output = {}
        callbacks.rtx_patcher_status()

        local text = table.concat(output, "\n")
        assertTruthy(string.find(text, "test.second_command", 1, true), "status command should print newest patcher state")
    end)

    concommand = oldConcommand
    print = oldPrint
    if _G then
        _G.RTXPatcher_StatusCommandAdded = oldStatusCommandAdded
    end

    if not ok then
        error(err, 0)
    end
end)

test("RegisterPatch applies ready patches once", function()
    local patcher = freshCore()
    local applyCount = 0

    patcher.RegisterPatch({
        id = "test.ready",
        label = "Ready Test",
        side = "client",
        enabled = function()
            return true
        end,
        depends = function()
            return true
        end,
        apply = function()
            applyCount = applyCount + 1
        end,
        maxAttempts = 0,
    })

    patcher.TryApply("test.ready")

    assertEquals(applyCount, 1, "ready patch should apply exactly once")
    assertTruthy(patcher.State.applied["test.ready"], "ready patch should be marked applied")
end)

test("RegisterPatch does not reapply already applied patches", function()
    local patcher = freshCore()
    local applied = 0

    local function spec()
        return {
            id = "test.reload",
            label = "Reload Test",
            side = "client",
            enabled = function()
                return true
            end,
            depends = function()
                return true
            end,
            apply = function()
                applied = applied + 1
            end,
            maxAttempts = 0,
        }
    end

    patcher.RegisterPatch(spec())
    patcher.RegisterPatch(spec())

    assertEquals(applied, 1, "already applied patch should not apply again")
    assertTruthy(patcher.State.applied["test.reload"], "reloaded patch should remain applied")
end)

test("RegisterPatch records pending patches when dependencies are missing", function()
    local patcher = freshCore()

    patcher.RegisterPatch({
        id = "test.pending",
        label = "Pending Test",
        side = "client",
        enabled = function()
            return true
        end,
        depends = function()
            return false, "waiting for fake dependency"
        end,
        apply = function()
            error("apply should not run when dependencies are missing")
        end,
        retryInterval = 999,
        maxAttempts = 2,
    })

    local pending = patcher.State.pending["test.pending"]

    assertTruthy(pending, "pending patch should be recorded")
    assertEquals(pending.reason, "waiting for fake dependency", "pending reason should be recorded")
end)

test("pending retry is only scheduled once while waiting", function()
    local patcher = freshCore()
    local oldTimer = timer
    local created = {}

    timer = {
        Create = function(name, delay, reps, fn)
            created[#created + 1] = {
                name = name,
                delay = delay,
                reps = reps,
                fn = fn,
            }
        end,
    }

    local ok, err = pcall(function()
        patcher.RegisterPatch({
            id = "test.retry-once",
            label = "Retry Once Test",
            side = "client",
            enabled = function()
                return true
            end,
            depends = function()
                return false, "still waiting"
            end,
            apply = function()
                error("apply should not run when dependencies are missing")
            end,
            retryInterval = 999,
            maxAttempts = 1,
        })

        patcher.TryApply("test.retry-once")
        patcher.TryApply("test.retry-once")
        patcher.TryApplyAll()

        assertEquals(#created, 1, "pending retry should only create one timer")
        assertEquals(patcher.State.failed["test.retry-once"], nil, "waiting patch should not fail before retry fires")
    end)

    timer = oldTimer

    if not ok then
        error(err, 0)
    end
end)

test("retry timer names are unique for distinct patch ids", function()
    local patcher = freshCore()
    local oldTimer = timer
    local created = {}

    timer = {
        Create = function(name, delay, reps, fn)
            created[#created + 1] = {
                name = name,
                delay = delay,
                reps = reps,
                fn = fn,
            }
        end,
    }

    local ok, err = pcall(function()
        local function pendingSpec(id)
            return {
                id = id,
                label = id,
                side = "client",
                enabled = function()
                    return true
                end,
                depends = function()
                    return false, "still waiting"
                end,
                apply = function()
                    error("apply should not run when dependencies are missing")
                end,
                retryInterval = 999,
                maxAttempts = 1,
            }
        end

        patcher.RegisterPatch(pendingSpec("test.foo-bar"))
        patcher.RegisterPatch(pendingSpec("test.foo_bar"))

        assertEquals(#created, 2, "each pending patch should create a retry timer")
        assertTruthy(created[1].name ~= created[2].name, "retry timer names should not collide")
    end)

    timer = oldTimer

    if not ok then
        error(err, 0)
    end
end)

test("stale retry callback does not reapply failed patches", function()
    local patcher = freshCore()
    local oldTimer = timer
    local retryCallback
    local ready = false
    local applyCount = 0

    timer = {
        Create = function(name, delay, reps, fn)
            retryCallback = fn
        end,
    }

    local ok, err = pcall(function()
        local id = "test.stale-retry"

        patcher.RegisterPatch({
            id = id,
            label = "Stale Retry Test",
            side = "client",
            enabled = function()
                return true
            end,
            depends = function()
                return ready, "still waiting"
            end,
            apply = function()
                applyCount = applyCount + 1
                error("expected stale retry failure")
            end,
            retryInterval = 999,
            maxAttempts = 2,
        })

        assertTruthy(retryCallback, "retry callback should be captured")

        ready = true
        patcher.TryApply(id)
        retryCallback()

        assertEquals(applyCount, 1, "stale retry should not reapply failed patch")
        assertTruthy(patcher.State.failed[id], "failed patch should remain failed")
    end)

    timer = oldTimer

    if not ok then
        error(err, 0)
    end
end)

test("failed apply is captured in failed state", function()
    local patcher = freshCore()

    patcher.RegisterPatch({
        id = "test.fail",
        label = "Fail Test",
        side = "client",
        enabled = function()
            return true
        end,
        depends = function()
            return true
        end,
        apply = function()
            error("expected apply failure")
        end,
        maxAttempts = 0,
    })

    local failed = patcher.State.failed["test.fail"]

    assertTruthy(failed, "failed patch should be recorded")
    assertTruthy(string.find(tostring(failed.reason), "expected apply failure", 1, true), "failed reason should contain apply error")
end)

test("GetStatus reports registered ids", function()
    local patcher = freshCore()

    patcher.RegisterPatch({
        id = "test.status",
        label = "Status Test",
        side = "client",
        enabled = function()
            return true
        end,
        depends = function()
            return true
        end,
        apply = function() end,
        maxAttempts = 0,
    })

    local status = patcher.GetStatus()
    local patchStatus = status.patches["test.status"]

    assertTruthy(patchStatus, "registered patch should be reported")
    assertEquals(patchStatus.state, "applied", "registered patch state should be applied")
end)

local failures = 0

for _, case in ipairs(tests) do
    local ok, err = pcall(case.fn)

    if ok then
        print("[RTXPatcherTests] ok - " .. case.name)
    else
        failures = failures + 1
        print("[RTXPatcherTests] FAIL - " .. case.name .. ": " .. tostring(err))
    end
end

if failures > 0 then
    error("[RTXPatcherTests] " .. failures .. " failure(s)")
end

print("[RTXPatcherTests] PASS")
