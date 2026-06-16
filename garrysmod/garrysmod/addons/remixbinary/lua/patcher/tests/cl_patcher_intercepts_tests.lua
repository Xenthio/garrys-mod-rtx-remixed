if not CLIENT then return end

RTXPatcher = nil
include("patcher/sh_core.lua")
include("patcher/cl_intercepts.lua")

local tests = {}

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

local function test(name, fn)
    tests[#tests + 1] = { name = name, fn = fn }
end

test("around intercept can replace return values", function()
    local target = {
        value = 3,
        Add = function(self, amount)
            return self.value + amount
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "Add", {
        patchId = "test",
        id = "around_add",
        around = function(original, self, amount)
            return original(self, amount) * 2
        end
    })

    assertTruthy(ok, err)
    assertEquals(target:Add(4), 14, "around result")
end)

test("around intercept preserves multiple returns with middle nil", function()
    local target = {
        Values = function(self, value)
            return value, nil, "tail"
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "Values", {
        patchId = "test",
        id = "around_multi_return",
        around = function(original, self, value)
            local first, second, third = original(self, value)
            return first .. "-patched", second, third
        end
    })

    assertTruthy(ok, err)

    local first, second, third = target:Values("source")
    assertEquals(first, "source-patched", "multiple return first")
    assertEquals(second, nil, "multiple return middle nil")
    assertEquals(third, "tail", "multiple return third")
end)

test("around intercept preserves zero return values", function()
    local target = {
        NoReturn = function()
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "NoReturn", {
        patchId = "test",
        id = "zero_returns",
        around = function(original, self)
            return original(self)
        end
    })

    assertTruthy(ok, err)
    assertEquals(select("#", target:NoReturn()), 0, "zero return count")
end)

test("dot-call intercept preserves explicit arguments", function()
    local target = {
        Join = function(left, right)
            return left .. ":" .. right
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "Join", {
        patchId = "test",
        id = "dot_join",
        around = function(original, left, right)
            assertEquals(left, "left", "dot-call first arg")
            assertEquals(right, "right", "dot-call second arg")
            return original(left, right) .. "-patched"
        end
    })

    assertTruthy(ok, err)
    assertEquals(target.Join("left", "right"), "left:right-patched", "dot-call result")
end)

test("same intercept id does not wrap twice", function()
    local target = {
        Count = function()
            return 2
        end
    }

    local firstOk, firstErr = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "idempotent_count",
        around = function(original)
            return original() + 1
        end
    })

    local secondOk, secondErr = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "idempotent_count",
        around = function(original)
            return original() + 1
        end
    })

    assertTruthy(firstOk, firstErr)
    assertTruthy(secondOk, secondErr)
    assertEquals(target.Count(), 3, "idempotent result")
end)

test("duplicate intercept returns restorable state key", function()
    local target = {
        Count = function()
            return 2
        end
    }

    local firstOk, firstKey = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "restorable_duplicate",
        around = function(original)
            return original() + 1
        end
    })

    local secondOk, secondKey = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "restorable_duplicate",
        around = function(original)
            return original() + 1
        end
    })

    assertTruthy(firstOk, firstKey)
    assertTruthy(secondOk, secondKey)
    assertEquals(secondKey, firstKey, "duplicate state key")
    assertEquals(target.Count(), 3, "patched before restore")

    local restoreOk, restoreErr = RTXPatcher.RestoreFunctionIntercept(secondKey)
    assertTruthy(restoreOk, restoreErr)
    assertEquals(target.Count(), 2, "original after duplicate restore")
    assertEquals(RTXPatcher.State.functionIntercepts[firstKey], nil, "state removed after restore")
end)

test("restore refuses to clobber newer stacked intercept", function()
    local target = {
        Count = function()
            return 1
        end
    }

    local firstOk, firstKey = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "stack_a",
        around = function(original)
            return original() + 1
        end
    })

    local secondOk, secondKey = RTXPatcher.InterceptFunction(target, "Count", {
        patchId = "test",
        id = "stack_b",
        around = function(original)
            return original() + 10
        end
    })

    assertTruthy(firstOk, firstKey)
    assertTruthy(secondOk, secondKey)
    assertEquals(target.Count(), 12, "stacked behavior before restore")

    local staleOk, staleErr = RTXPatcher.RestoreFunctionIntercept(firstKey)
    assertEquals(staleOk, false, "stale restore ok")
    assertTruthy(string.find(staleErr, "not current", 1, true), "stale restore error")
    assertEquals(target.Count(), 12, "stale restore leaves newer wrapper active")
    assertTruthy(RTXPatcher.State.functionIntercepts[firstKey], "stale restore keeps first state")

    local secondRestoreOk, secondRestoreErr = RTXPatcher.RestoreFunctionIntercept(secondKey)
    assertTruthy(secondRestoreOk, secondRestoreErr)
    assertEquals(target.Count(), 2, "second restore unwinds to first wrapper")

    local firstRestoreOk, firstRestoreErr = RTXPatcher.RestoreFunctionIntercept(firstKey)
    assertTruthy(firstRestoreOk, firstRestoreErr)
    assertEquals(target.Count(), 1, "first restore unwinds to original")
end)

test("before and after intercepts preserve original call", function()
    local beforeCalled = false
    local afterCalled = false
    local events = {}
    local target = {
        Echo = function(self, value)
            events[#events + 1] = "original"
            return value
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "Echo", {
        patchId = "test",
        id = "before_after_echo",
        before = function(self, value)
            events[#events + 1] = "before"
            beforeCalled = value == "source"
        end,
        after = function(results)
            events[#events + 1] = "after"
            afterCalled = true
            results[1] = results[1] .. "-patched"
            return results
        end
    })

    assertTruthy(ok, err)
    assertEquals(target:Echo("source"), "source-patched", "after result")
    assertTruthy(beforeCalled, "before called")
    assertTruthy(afterCalled, "after called")
    assertEquals(events[1], "before", "event order 1")
    assertEquals(events[2], "original", "event order 2")
    assertEquals(events[3], "after", "event order 3")
end)

test("after intercept can replace result table with nil holes", function()
    local target = {
        Values = function()
            return "original", "middle"
        end
    }

    local ok, err = RTXPatcher.InterceptFunction(target, "Values", {
        patchId = "test",
        id = "after_replace_values",
        after = function(results)
            assertEquals(results.n, 2, "original result count")
            return {
                [1] = "first",
                [3] = "third",
                n = 3
            }
        end
    })

    assertTruthy(ok, err)

    local first, second, third = target.Values()
    assertEquals(first, "first", "replacement first")
    assertEquals(second, nil, "replacement middle nil")
    assertEquals(third, "third", "replacement third")
end)

test("restore function intercept puts original back", function()
    local target = {
        Value = function()
            return "original"
        end
    }

    local ok, stateKey = RTXPatcher.InterceptFunction(target, "Value", {
        patchId = "test",
        id = "restore_value",
        around = function(original)
            return original() .. "-patched"
        end
    })

    assertTruthy(ok, stateKey)
    assertEquals(target.Value(), "original-patched", "patched before restore")

    local restoreOk, restoreErr = RTXPatcher.RestoreFunctionIntercept(stateKey)
    assertTruthy(restoreOk, restoreErr)
    assertEquals(target.Value(), "original", "original after restore")

    local missingOk, missingErr = RTXPatcher.RestoreFunctionIntercept(stateKey)
    assertEquals(missingOk, false, "missing restore ok")
    assertTruthy(string.find(missingErr, "unknown", 1, true), "missing restore error")
end)

test("non-table target returns failure", function()
    local ok, err = RTXPatcher.InterceptFunction(nil, "Missing", {
        patchId = "test",
        id = "missing_target"
    })

    assertEquals(ok, false, "non-table target ok")
    assertTruthy(string.find(err, "not a table", 1, true), "non-table target error")
end)

test("missing target function returns failure", function()
    local ok, err = RTXPatcher.InterceptFunction({}, "Missing", {
        patchId = "test",
        id = "missing"
    })

    assertEquals(ok, false, "missing function ok")
    assertTruthy(string.find(err, "not a function", 1, true), "missing function error")
end)

test("hook intercept adds generated hook id and stores state", function()
    local oldHook = hook
    local added = {}

    hook = {
        Add = function(eventName, hookId, fn)
            added[#added + 1] = {
                eventName = eventName,
                hookId = hookId,
                fn = fn
            }
        end
    }

    local ok, err = pcall(function()
        local fn = function() return true end
        local interceptOk, stateKey = RTXPatcher.InterceptHook("Think", "paint", fn, {
            patchId = "test"
        })

        assertTruthy(interceptOk, stateKey)
        assertEquals(#added, 1, "hook add count")
        assertEquals(added[1].eventName, "Think", "hook event")
        assertEquals(added[1].hookId, "RTXPatcher_74657374_7061696E74", "generated hook id")
        assertEquals(added[1].fn, fn, "hook function")
        assertTruthy(RTXPatcher.State.hookIntercepts[stateKey], "hook state should be stored")
    end)

    hook = oldHook

    if not ok then
        error(err, 0)
    end
end)

test("duplicate hook returns same state key and does not add twice", function()
    local oldHook = hook
    local added = {}

    hook = {
        Add = function(eventName, hookId, fn)
            added[#added + 1] = {
                eventName = eventName,
                hookId = hookId,
                fn = fn
            }
        end
    }

    local ok, err = pcall(function()
        local fn = function() return true end
        local firstOk, firstKey = RTXPatcher.InterceptHook("Think", "duplicate", fn, {
            patchId = "test"
        })
        local secondOk, secondKey = RTXPatcher.InterceptHook("Think", "duplicate", fn, {
            patchId = "test"
        })

        assertTruthy(firstOk, firstKey)
        assertTruthy(secondOk, secondKey)
        assertEquals(firstKey, secondKey, "duplicate hook state key")
        assertEquals(#added, 1, "duplicate hook add count")
    end)

    hook = oldHook

    if not ok then
        error(err, 0)
    end
end)

test("hook ids do not collide for separator variants", function()
    local oldHook = hook
    local added = {}

    hook = {
        Add = function(eventName, hookId, fn)
            added[#added + 1] = {
                eventName = eventName,
                hookId = hookId,
                fn = fn
            }
        end
    }

    local ok, err = pcall(function()
        local fn = function() return true end
        local firstOk, firstKey = RTXPatcher.InterceptHook("Think", "c", fn, {
            patchId = "a_b"
        })
        local secondOk, secondKey = RTXPatcher.InterceptHook("Think", "b_c", fn, {
            patchId = "a"
        })

        assertTruthy(firstOk, firstKey)
        assertTruthy(secondOk, secondKey)
        assertEquals(#added, 2, "separator variants hook add count")
        assertTruthy(added[1].hookId ~= added[2].hookId, "generated hook ids differ")
        assertTruthy(firstKey ~= secondKey, "hook state keys differ")
    end)

    hook = oldHook

    if not ok then
        error(err, 0)
    end
end)

test("hook intercept reports unavailable hook library", function()
    local oldHook = hook
    hook = nil

    local ok, err = RTXPatcher.InterceptHook("Think", "missing_hook", function() end, {
        patchId = "test"
    })

    hook = oldHook

    assertEquals(ok, false, "missing hook library ok")
    assertTruthy(string.find(err, "unavailable", 1, true), "missing hook library error")
end)

test("hook intercept requires function", function()
    local oldHook = hook

    hook = {
        Add = function()
            error("hook.Add should not run for invalid function")
        end
    }

    local ok, err = RTXPatcher.InterceptHook("Think", "bad_hook", nil, {
        patchId = "test"
    })

    hook = oldHook

    assertEquals(ok, false, "bad hook function ok")
    assertTruthy(string.find(err, "not a function", 1, true), "bad hook function error")
end)

local failures = 0
for _, entry in ipairs(tests) do
    local ok, err = pcall(entry.fn)
    if ok then
        print("[RTXPatcherTests] ok - " .. entry.name)
    else
        failures = failures + 1
        print("[RTXPatcherTests] FAIL - " .. entry.name .. ": " .. tostring(err))
    end
end

if failures > 0 then
    error("[RTXPatcherTests] " .. failures .. " failure(s)")
end

print("[RTXPatcherTests] PASS")
