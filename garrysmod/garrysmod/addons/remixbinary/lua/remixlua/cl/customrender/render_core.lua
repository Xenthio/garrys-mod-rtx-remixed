if not CLIENT then return end

RemixRenderCore = RemixRenderCore or {}

do
    local handlers = RemixRenderCore._handlers or {}
    local attached = RemixRenderCore._attached or {}
    local matCache = RemixRenderCore._materials or {}
    local meshRefs = RemixRenderCore._meshRefs or {}
    local statsFns = RemixRenderCore._stats or {}
    local tokens = RemixRenderCore._tokens or {}
    local rebuildSinks = RemixRenderCore._rebuildSinks or {}
    local sortedCache = RemixRenderCore._sortedCache or {}
    local cacheDirty = RemixRenderCore._cacheDirty or {}
    -- Render queues and frame/job state
    local queues = RemixRenderCore._queues or { opaque = { buckets = {}, order = {} }, translucent = { buckets = {}, order = {} } }
    local frameState = RemixRenderCore._frame or { began = false, skybox = false, lastJobFrame = -1, cachedEyePos = nil }
    local jobs = RemixRenderCore._jobs or {}
    local offscreenCount = RemixRenderCore._offscreenCount or 0
    local frameBudgetHistory = RemixRenderCore._frameBudgetHistory or {}
    local FRAME_BUDGET_SAMPLES = 10
    local renderState = RemixRenderCore._renderState or { lastColorR = 1, lastColorG = 1, lastColorB = 1 }
    local translucentItemsCache = RemixRenderCore._translucentItems or {}
    local WHITE_COLOR_NORMALIZED = { r = 1, g = 1, b = 1 }
    
    -- Performance profiler
    local debugEnabled = CreateClientConVar("rtx_debug_profiling", "0", true, false, "Enable performance profiling and allocation tracking (0 = off, 1 = on)")
    local perfHistory = RemixRenderCore._perfHistory or {}
    local perfThreshold = 1.0 / 1000  -- Report operations taking >1ms
    local lastGCCount = collectgarbage("count")
    local gcCycleCount = 0
    local lastGCStepTime = 0
    
    -- Detailed allocation tracking per component
    local allocTracking = RemixRenderCore._allocTracking or {
        enabled = false,
        components = {},
        frameStart = 0,
        lastFrame = -1
    }
    
    -- Object pooling to reduce GC pressure
    local vectorPool = RemixRenderCore._vectorPool or {}
    local vectorPoolIndex = 0
    local MAX_VECTOR_POOL = 10
    
    local function getPooledVector(x, y, z)
        vectorPoolIndex = (vectorPoolIndex % MAX_VECTOR_POOL) + 1
        local v = vectorPool[vectorPoolIndex]
        if not v then
            v = Vector()
            vectorPool[vectorPoolIndex] = v
        end
        v.x = x
        v.y = y
        v.z = z
        return v
    end
    
    -- Color table pool for normalized colors
    local colorPool = RemixRenderCore._colorPool or {}
    local colorPoolIndex = 0
    local MAX_COLOR_POOL = 20
    
    local function getPooledColor(r, g, b)
        colorPoolIndex = (colorPoolIndex % MAX_COLOR_POOL) + 1
        local c = colorPool[colorPoolIndex]
        if not c then
            c = {}
            colorPool[colorPoolIndex] = c
        end
        c.r = r
        c.g = g
        c.b = b
        return c
    end

    local function safeCall(id, fn, ...)
        local ok, a, b, c, d = pcall(fn, ...)
        if not ok then
            ErrorNoHalt("[RemixRenderCore] Handler '" .. tostring(id) .. "' error: " .. tostring(a) .. "\n")
            return nil
        end
        return a, b, c, d
    end

    -- Allocation tracking helpers (defined early so they can be used everywhere)
    local function trackAllocStart(componentName)
        if not debugEnabled:GetBool() or not allocTracking.enabled then return end
        local mem = collectgarbage("count")
        if not allocTracking.components[componentName] then
            allocTracking.components[componentName] = { total = 0, calls = 0, last = 0 }
        end
        allocTracking.components[componentName].last = mem
    end
    
    local function trackAllocEnd(componentName)
        if not debugEnabled:GetBool() or not allocTracking.enabled then return end
        local mem = collectgarbage("count")
        local comp = allocTracking.components[componentName]
        if comp and comp.last then
            local delta = mem - comp.last
            comp.total = comp.total + delta
            comp.calls = comp.calls + 1
            comp.last = nil
        end
    end

    -- ============================
    -- Frame Orchestration + Render Queue
    -- ============================
    function RemixRenderCore.BeginFrame(_, bSkybox)
        trackAllocStart("BeginFrame_Total")
        local startTime = SysTime()
        
        -- Track frame start for allocation tracking
        if allocTracking.enabled then
            local frame = FrameNumber()
            if allocTracking.lastFrame ~= frame then
                allocTracking.lastFrame = frame
                allocTracking.frameStart = collectgarbage("count")
            end
        end
        
        -- Clear queues by emptying bucket arrays instead of destroying tables
        trackAllocStart("Queue_Clearing")
        local oq = queues.opaque
        local tq = queues.translucent
        table.Empty(oq.order)
        table.Empty(tq.order)
        -- Reuse bucket tables instead of destroying them
        for mat, bucket in pairs(oq.buckets) do 
            if bucket then table.Empty(bucket) end
        end
        for mat, bucket in pairs(tq.buckets) do 
            if bucket then table.Empty(bucket) end
        end
        trackAllocEnd("Queue_Clearing")
        
        frameState.began = true
        frameState.skybox = bSkybox or false
        
        -- Cache EyePos for this frame to avoid multiple engine calls
        frameState.cachedEyePos = EyePos()
        
        -- Advance scheduled jobs only once per real frame
        local currentFrame = FrameNumber()
        if frameState.lastJobFrame ~= currentFrame then
            frameState.lastJobFrame = currentFrame
            if debugEnabled:GetBool() then
                local jobStart = SysTime()
                RemixRenderCore.StepJobs(0.0015)
                local jobTime = SysTime() - jobStart
                if jobTime > perfThreshold then
                    RemixRenderCore.RecordPerf("StepJobs", jobTime)
                end
            else
                RemixRenderCore.StepJobs(0.0015)
            end
        end
        
        -- Track GC activity with timing (only if debug enabled)
        if debugEnabled:GetBool() then
            local currentGC = collectgarbage("count")
            local gcDelta = currentGC - lastGCCount
            if math.abs(gcDelta) > 500 then  -- >500KB change
                gcCycleCount = gcCycleCount + 1
            end
            lastGCCount = currentGC
        end
        
        -- Run MORE AGGRESSIVE incremental GC
        local gcStepStart = SysTime()
        collectgarbage("step", 20)  -- Much larger step size
        local gcStepTime = SysTime() - gcStepStart
        lastGCStepTime = gcStepTime
        
        -- Only track if really slow
        -- if gcStepTime > 0.005 then  -- >5ms for GC step
        --     RemixRenderCore.RecordPerf("GC_Step_Slow", gcStepTime)
        -- end
        
        if debugEnabled:GetBool() then
            local elapsed = SysTime() - startTime
            if elapsed > perfThreshold then
                RemixRenderCore.RecordPerf("BeginFrame", elapsed)
            end
        end
        trackAllocEnd("BeginFrame_Total")
    end

    local function normalizeColor(col)
        if not col then return nil end
        if istable(col) and col.r then
            -- Fast path for white (most common)
            if col.r == 255 and col.g == 255 and col.b == 255 then
                return WHITE_COLOR_NORMALIZED
            end
            -- Use pooled table to avoid allocation
            return getPooledColor((col.r or 255) / 255, (col.g or 255) / 255, (col.b or 255) / 255)
        end
        return nil
    end

    function RemixRenderCore.Submit(item)
        trackAllocStart("Submit_QueueAdd")
        -- item = { material=IMaterial, mesh=IMesh, matrix=Matrix|nil, translucent=bool|nil, color=Color|{r,g,b}|nil }
        if not item or not item.material or not item.mesh then 
            trackAllocEnd("Submit_QueueAdd")
            return 
        end
        local q = item.translucent and queues.translucent or queues.opaque
        -- store normalized color for fast modulation
        if item.color then item._ncolor = normalizeColor(item.color) end
        -- bucket by material to avoid per-frame material name sorting
        local buckets = q.buckets
        local order = q.order
        local mat = item.material
        local bucket = buckets[mat]
        if not bucket then
            bucket = {}
            buckets[mat] = bucket
        end
        -- Check if bucket is empty (first item this frame)
        if #bucket == 0 then
            order[#order + 1] = mat
        end
        bucket[#bucket + 1] = item
        trackAllocEnd("Submit_QueueAdd")
    end

    -- Depth sort helper for translucent meshes (caches depth in item)
    local function cacheItemDepth(item, camPos)
        if not item.matrix then 
            item._cachedDepth = 0
            return 0 
        end
        local pos = item.matrix:GetTranslation()
        local depth = pos:DistToSqr(camPos)
        item._cachedDepth = depth
        return depth
    end
    
    -- Pre-defined sort comparator to avoid creating function every frame
    local function depthSortComparator(a, b)
        return (a._cachedDepth or 0) > (b._cachedDepth or 0)
    end

    local function flushQueue(queue, translucent)
        if not queue or not queue.order then return end
        trackAllocStart(translucent and "FlushQueue_Translucent_Inner" or "FlushQueue_Opaque_Inner")
        local lastMat = nil
        local order = queue.order
        local buckets = queue.buckets
        
        -- Early out if empty
        if #order == 0 then 
            trackAllocEnd(translucent and "FlushQueue_Translucent_Inner" or "FlushQueue_Opaque_Inner")
            return 
        end
        
        -- Reset color modulation state at start of flush
        renderState.lastColorR = 1
        renderState.lastColorG = 1
        renderState.lastColorB = 1
        
        -- For translucent, we need depth sorting
        if translucent then
            trackAllocStart("Translucent_Sorting")
            local camPos = frameState.cachedEyePos or EyePos()
            
            -- Reuse array instead of allocating new one
            table.Empty(translucentItemsCache)
            
            -- Collect all items from all buckets and pre-calculate depths
            for i = 1, #order do
                local mat = order[i]
                local bucket = buckets[mat]
                if bucket and #bucket > 0 then
                    for j = 1, #bucket do
                        local item = bucket[j]
                        cacheItemDepth(item, camPos)
                        translucentItemsCache[#translucentItemsCache + 1] = item
                    end
                end
            end
            
            -- Sort back-to-front by cached distance (using pre-defined comparator)
            table.sort(translucentItemsCache, depthSortComparator)
            trackAllocEnd("Translucent_Sorting")
            
            -- Render sorted items with state tracking
            trackAllocStart("Translucent_Drawing")
            for i = 1, #translucentItemsCache do
                local it = translucentItemsCache[i]
                if it.material ~= lastMat then
                    render.SetMaterial(it.material)
                    lastMat = it.material
                end
                
                -- Only set color modulation if changed
                if it._ncolor then
                    local nc = it._ncolor
                    if nc.r ~= renderState.lastColorR or nc.g ~= renderState.lastColorG or nc.b ~= renderState.lastColorB then
                        render.SetColorModulation(nc.r, nc.g, nc.b)
                        renderState.lastColorR = nc.r
                        renderState.lastColorG = nc.g
                        renderState.lastColorB = nc.b
                    end
                elseif renderState.lastColorR ~= 1 or renderState.lastColorG ~= 1 or renderState.lastColorB ~= 1 then
                    render.SetColorModulation(1, 1, 1)
                    renderState.lastColorR = 1
                    renderState.lastColorG = 1
                    renderState.lastColorB = 1
                end
                
                if it.matrix then cam.PushModelMatrix(it.matrix) end
                it.mesh:Draw()
                if it.matrix then cam.PopModelMatrix() end
            end
            trackAllocEnd("Translucent_Drawing")
        else
            -- Opaque: bucket by material (no sorting needed)
            trackAllocStart("Opaque_Drawing")
            for i = 1, #order do
                local mat = order[i]
                local bucket = buckets[mat]
                if bucket and #bucket > 0 then
                    if mat ~= lastMat then
                        render.SetMaterial(mat)
                        lastMat = mat
                    end
                    for j = 1, #bucket do
                        local it = bucket[j]
                        
                        -- Only set color modulation if changed
                        if it._ncolor then
                            local nc = it._ncolor
                            if nc.r ~= renderState.lastColorR or nc.g ~= renderState.lastColorG or nc.b ~= renderState.lastColorB then
                                render.SetColorModulation(nc.r, nc.g, nc.b)
                                renderState.lastColorR = nc.r
                                renderState.lastColorG = nc.g
                                renderState.lastColorB = nc.b
                            end
                        elseif renderState.lastColorR ~= 1 or renderState.lastColorG ~= 1 or renderState.lastColorB ~= 1 then
                            render.SetColorModulation(1, 1, 1)
                            renderState.lastColorR = 1
                            renderState.lastColorG = 1
                            renderState.lastColorB = 1
                        end
                        
                        if it.matrix then cam.PushModelMatrix(it.matrix) end
                        it.mesh:Draw()
                        if it.matrix then cam.PopModelMatrix() end
                    end
                end
            end
            trackAllocEnd("Opaque_Drawing")
        end
        
        -- Reset color modulation state at end of flush
        if renderState.lastColorR ~= 1 or renderState.lastColorG ~= 1 or renderState.lastColorB ~= 1 then
            render.SetColorModulation(1, 1, 1)
            renderState.lastColorR = 1
            renderState.lastColorG = 1
            renderState.lastColorB = 1
        end
        trackAllocEnd(translucent and "FlushQueue_Translucent_Inner" or "FlushQueue_Opaque_Inner")
    end

    function RemixRenderCore.FlushPass(translucent)
        if not frameState.began then return end
        -- Suppress engine lighting to prevent lightmap rendering on custom meshes
        render.SuppressEngineLighting(true)
        if debugEnabled:GetBool() then
            trackAllocStart(translucent and "FlushPass_Translucent" or "FlushPass_Opaque")
            local startTime = SysTime()
            if translucent then
                flushQueue(queues.translucent, true)
            else
                flushQueue(queues.opaque, false)
            end
            local elapsed = SysTime() - startTime
            if elapsed > perfThreshold then
                RemixRenderCore.RecordPerf(translucent and "FlushTranslucent" or "FlushOpaque", elapsed)
            end
            trackAllocEnd(translucent and "FlushPass_Translucent" or "FlushPass_Opaque")
        else
            if translucent then
                flushQueue(queues.translucent, true)
            else
                flushQueue(queues.opaque, false)
            end
        end
        render.SuppressEngineLighting(false)
    end

    -- ============================
    -- Hook Aggregation System
    -- ============================
    local function installAggregator(hookName)
        if attached[hookName] then return end
        attached[hookName] = true

        hook.Add(hookName, "RemixRenderCore-" .. hookName, function(...)
            local list = handlers[hookName]
            if not list then return end

            -- Use cached sorted list if available and not dirty
            local ordered
            if not cacheDirty[hookName] and sortedCache[hookName] then
                ordered = sortedCache[hookName]
            else
                -- Build and cache sorted list
                ordered = {}
                for id, entry in pairs(list) do
                    if isfunction(entry) then
                        ordered[#ordered + 1] = { id = id, fn = entry, prio = 100 }
                    elseif istable(entry) and isfunction(entry.fn) then
                        ordered[#ordered + 1] = { id = id, fn = entry.fn, prio = tonumber(entry.prio) or 100 }
                    end
                end
                table.sort(ordered, function(a, b)
                    if a.prio == b.prio then return tostring(a.id) < tostring(b.id) end
                    return a.prio < b.prio
                end)
                
                sortedCache[hookName] = ordered
                cacheDirty[hookName] = false
            end

            local aggregatedReturn = nil
            for i = 1, #ordered do
                local it = ordered[i]
                local ret = select(1, safeCall(it.id, it.fn, ...))
                if ret ~= nil then
                    aggregatedReturn = aggregatedReturn or ret
                end
            end
            return aggregatedReturn
        end)
    end

    function RemixRenderCore.Register(hookName, id, fn)
        if not hookName or not id or not fn then return end
        handlers[hookName] = handlers[hookName] or {}
        if isfunction(fn) then
            handlers[hookName][id] = fn
        elseif istable(fn) and isfunction(fn.fn) then
            handlers[hookName][id] = { fn = fn.fn, prio = tonumber(fn.prio) or 100 }
        else
            return
        end
        cacheDirty[hookName] = true
        installAggregator(hookName)
    end

    function RemixRenderCore.Unregister(hookName, id)
        local list = handlers[hookName]
        if not list then return end
        list[id] = nil
        cacheDirty[hookName] = true
        -- Optional: remove aggregator if empty
        local hasAny = false
        for _, _ in pairs(list) do hasAny = true break end
        if not hasAny then
            handlers[hookName] = nil
            sortedCache[hookName] = nil
            cacheDirty[hookName] = nil
            if attached[hookName] then
                hook.Remove(hookName, "RemixRenderCore-" .. hookName)
                attached[hookName] = nil
            end
        end
    end

    RemixRenderCore._handlers = handlers
    RemixRenderCore._attached = attached
    RemixRenderCore._materials = matCache
    RemixRenderCore._meshRefs = meshRefs
    RemixRenderCore._stats = statsFns
    RemixRenderCore._tokens = tokens
    RemixRenderCore._rebuildSinks = rebuildSinks
    RemixRenderCore._queues = queues
    RemixRenderCore._frame = frameState
    RemixRenderCore._jobs = jobs
    RemixRenderCore._offscreenCount = offscreenCount
    RemixRenderCore._frameBudgetHistory = frameBudgetHistory
    RemixRenderCore._renderState = renderState
    RemixRenderCore._sortedCache = sortedCache
    RemixRenderCore._cacheDirty = cacheDirty
    RemixRenderCore._translucentItems = translucentItemsCache
    RemixRenderCore._perfHistory = perfHistory
    RemixRenderCore._vectorPool = vectorPool
    RemixRenderCore._colorPool = colorPool
    RemixRenderCore._allocTracking = allocTracking
    
    -- Expose tracking functions for external use
    RemixRenderCore.TrackAllocStart = trackAllocStart
    RemixRenderCore.TrackAllocEnd = trackAllocEnd
    
    -- Performance tracking (optimized to reduce allocations)
    local perfEntryPool = {}
    for i = 1, 100 do
        perfEntryPool[i] = { name = "", time = 0, when = 0, extra = nil }
    end
    
    function RemixRenderCore.RecordPerf(name, time, extra)
        local now = SysTime()
        
        -- Reuse pooled entry instead of allocating
        local idx = (#perfHistory % 100) + 1
        local entry = perfEntryPool[idx]
        entry.name = name
        entry.time = time or 0
        entry.when = now
        entry.extra = extra
        
        perfHistory[#perfHistory + 1] = entry
        
        -- Keep only last 100 entries
        if #perfHistory > 100 then
            table.remove(perfHistory, 1)
        end
        
        -- Log significant events (only print, don't format unless needed)
        -- DISABLED to reduce allocation spam from string.format
        -- if time and time > 0.002 then  -- >2ms
        --     print(string.format("[PERF] %s: %.2fms%s", name, time * 1000, extra and (" (" .. extra .. ")") or ""))
        -- elseif extra then
        --     print(string.format("[PERF] %s: %s", name, extra))
        -- end
    end
    
    function RemixRenderCore.GetPerfHistory()
        return perfHistory
    end
    
    function RemixRenderCore.ClearPerfHistory()
        table.Empty(perfHistory)
    end

    -- ============================
    -- Offscreen RT Tracking
    -- ============================
    function RemixRenderCore.PushOffscreen()
        offscreenCount = (offscreenCount or 0) + 1
        RemixRenderCore._offscreenCount = offscreenCount
    end

    function RemixRenderCore.PopOffscreen()
        if not offscreenCount or offscreenCount == 0 then
            ErrorNoHalt("[RemixRenderCore] PopOffscreen called without matching Push!\n")
            offscreenCount = 0
            RemixRenderCore._offscreenCount = 0
            return
        end
        offscreenCount = offscreenCount - 1
        RemixRenderCore._offscreenCount = offscreenCount
    end

    function RemixRenderCore.IsOffscreen()
        return (offscreenCount or 0) > 0
    end

    -- ============================
    -- Shared Material Filtering & Caching
    -- ============================
    local _matcherCache = {}
    local _matcherCacheOrder = {}
    local MAX_MATCHER_CACHE = 100
    
    -- Material name lowercase cache
    local _lowerCache = setmetatable({}, {
        __index = function(t, str)
            if not str then return "" end
            local lower = string.lower(str)
            t[str] = lower
            return lower
        end
    })
    
    -- Centralized PVS cache
    local _pvsCache = nil
    local _pvsFrame = -1
    local _pvsLastLeaf = nil
    local _pvsLastPos = nil
    local _pvsUnavailable = false
    local PVS_UPDATE_DISTANCE_SQR = 64 * 64  -- Only recalc if moved 64 units
    
    function RemixRenderCore.IsPVSValid(pvs)
        if not pvs then return false end
        -- Fast check: if table has any entries, check first one with next()
        local k, v = next(pvs)
        return k ~= nil
    end
    
    function RemixRenderCore.GetPVS(eyePos)
        if _pvsUnavailable or not NikNaks or not NikNaks.CurrentMap or not eyePos then
            return nil
        end
        
        local frame = FrameNumber()
        if _pvsFrame == frame and RemixRenderCore.IsPVSValid(_pvsCache) then
            return _pvsCache
        end
        
        -- Hysteresis: only recalc if moved significantly (reduces thrashing at leaf boundaries)
        if _pvsLastPos and _pvsCache then
            local distSqr = eyePos:DistToSqr(_pvsLastPos)
            if distSqr < PVS_UPDATE_DISTANCE_SQR then
                _pvsFrame = frame
                return _pvsCache
            end
        end
        
        -- Try cached leaf lookup first
        if NikNaks.CurrentMap.PointInLeafCache then
            local leaf, changed = NikNaks.CurrentMap:PointInLeafCache(0, eyePos, _pvsLastLeaf)
            if not changed and RemixRenderCore.IsPVSValid(_pvsCache) then
                _pvsFrame = frame
                return _pvsCache
            end
            _pvsLastLeaf = leaf
        end
        
        -- Calculate new PVS (this can be expensive) - TRACK ALLOCATION
        local ok, newPVS, pvsTime
        if debugEnabled:GetBool() then
            trackAllocStart("PVS_Calculation")
            local pvsStart = SysTime()
            ok, newPVS = pcall(function() return NikNaks.CurrentMap:PVSForOrigin(eyePos) end)
            pvsTime = SysTime() - pvsStart
            trackAllocEnd("PVS_Calculation")
            if pvsTime > perfThreshold then
                RemixRenderCore.RecordPerf("PVS_Calc", pvsTime)
            end
        else
            ok, newPVS = pcall(function() return NikNaks.CurrentMap:PVSForOrigin(eyePos) end)
        end
        
        if ok and RemixRenderCore.IsPVSValid(newPVS) then
            _pvsCache = newPVS
            _pvsFrame = frame
            -- Use pooled vector instead of allocating new one
            _pvsLastPos = getPooledVector(eyePos.x, eyePos.y, eyePos.z)
            return _pvsCache
        elseif not ok then
            -- PVS is broken for this map, disable permanently
            _pvsUnavailable = true
            _pvsCache = nil
            print("[RemixRenderCore] PVS unavailable for this map (invalid cluster data), disabling PVS culling")
        end
        
        return nil
    end
    
    function RemixRenderCore.ResetPVS()
        _pvsCache = nil
        _pvsFrame = -1
        _pvsLastLeaf = nil
        _pvsLastPos = nil
        _pvsUnavailable = false
    end
    
    -- Expose cached EyePos for this frame to avoid redundant engine calls
    function RemixRenderCore.GetCachedEyePos()
        return frameState.cachedEyePos
    end
    
    function RemixRenderCore.GetLowerCase(str)
        return _lowerCache[str]
    end
    
    function RemixRenderCore.BuildMatcherList(str)
        if not str or str == "" then return {} end
        local cached = _matcherCache[str]
        if cached then return cached end
        local list = {}
        for token in string.gmatch(str, "[^,]+") do
            token = string.Trim(string.lower(token))
            if token ~= "" then list[#list+1] = token end
        end
        
        -- LRU eviction if cache is full
        if #_matcherCacheOrder >= MAX_MATCHER_CACHE then
            local oldest = table.remove(_matcherCacheOrder, 1)
            _matcherCache[oldest] = nil
        end
        
        _matcherCache[str] = list
        _matcherCacheOrder[#_matcherCacheOrder + 1] = str
        return list
    end

    function RemixRenderCore.IsMaterialAllowed(matName, whitelist, blacklist)
        if not matName then return false end
        local lname = _lowerCache[matName]
        
        -- Check blacklist first
        local bl = RemixRenderCore.BuildMatcherList(blacklist)
        for i = 1, #bl do
            if string.find(lname, bl[i], 1, true) then return false end
        end
        
        -- Check whitelist
        local wl = RemixRenderCore.BuildMatcherList(whitelist)
        if #wl == 0 then return true end -- No whitelist means allow all
        for i = 1, #wl do
            if string.find(lname, wl[i], 1, true) then return true end
        end
        return false
    end

    -- Spatial binning utilities removed; no longer used

    -- ============================
    -- Bounds Calculation Utilities
    -- ============================
    function RemixRenderCore.CreateBounds()
        return {
            mins = Vector(math.huge, math.huge, math.huge),
            maxs = Vector(-math.huge, -math.huge, -math.huge)
        }
    end

    function RemixRenderCore.UpdateBounds(bounds, pos)
        if pos.x < bounds.mins.x then bounds.mins.x = pos.x end
        if pos.y < bounds.mins.y then bounds.mins.y = pos.y end
        if pos.z < bounds.mins.z then bounds.mins.z = pos.z end
        if pos.x > bounds.maxs.x then bounds.maxs.x = pos.x end
        if pos.y > bounds.maxs.y then bounds.maxs.y = pos.y end
        if pos.z > bounds.maxs.z then bounds.maxs.z = pos.z end
    end

    function RemixRenderCore.GetBoundsCenter(mins, maxs)
        return (mins + maxs) * 0.5
    end

    -- ============================
    -- Vertex Validation
    -- ============================
    function RemixRenderCore.ValidateVertex(pos)
        -- Check for nil or invalid structure
        if not pos or not pos.x or not pos.y or not pos.z then
            return false
        end
        
        -- Check for NaN (NaN != NaN in Lua)
        if pos.x ~= pos.x or pos.y ~= pos.y or pos.z ~= pos.z then
            return false
        end
        
        -- Check for extreme values
        local maxCoord = 16384
        if math.abs(pos.x) > maxCoord or 
           math.abs(pos.y) > maxCoord or 
           math.abs(pos.z) > maxCoord then
            return false
        end
        
        return true
    end

    -- ============================
    -- Debug Utilities
    -- ============================
    local debugPrefixes = {}
    
    function RemixRenderCore.CreateDebugPrint(prefix, convar)
        debugPrefixes[prefix] = convar
        return function(...)
            if convar and convar:GetBool() then
                print("[" .. prefix .. "]", ...)
            end
        end
    end

    -- ============================
    -- Spatial Utilities
    -- ============================
    -- Integer-based chunk key hashing (avoids string concatenation)
    function RemixRenderCore.HashChunkKey(x, y, z)
        -- Use bit operations for fast hashing
        -- Supports coordinates -2048 to 2047 per axis (12 bits each, 36 bits total)
        x = bit.band(x + 2048, 0xFFF)
        y = bit.band(y + 2048, 0xFFF)
        z = bit.band(z + 2048, 0xFFF)
        return bit.bor(bit.lshift(x, 24), bit.lshift(y, 12), z)
    end
    
    -- Distance culling with epsilon to prevent popping at boundaries
    function RemixRenderCore.ShouldCullByDistance(pos, playerPos, maxDist)
        if maxDist <= 0 then return false end
        local epsilon = maxDist * 0.05 -- 5% hysteresis
        return pos:DistToSqr(playerPos) > ((maxDist + epsilon) * (maxDist + epsilon))
    end

    local matCacheOrder = RemixRenderCore._matCacheOrder or {}
    local matCacheIndex = RemixRenderCore._matCacheIndex or {}
    local MAX_MATERIAL_CACHE = 500
    RemixRenderCore._matCacheOrder = matCacheOrder
    RemixRenderCore._matCacheIndex = matCacheIndex
    
    function RemixRenderCore.GetMaterial(name)
        if not name or name == "" then name = "debug/debugwhite" end
        local mat = matCache[name]
        if mat ~= nil then
            -- Move to end of LRU queue for frequently accessed materials
            local idx = matCacheIndex[name]
            if idx and idx ~= #matCacheOrder then
                table.remove(matCacheOrder, idx)
                matCacheOrder[#matCacheOrder + 1] = name
                -- Update indices for shifted items
                for i = idx, #matCacheOrder do
                    matCacheIndex[matCacheOrder[i]] = i
                end
            end
            return mat
        end
        
        -- LRU eviction if cache is full - evict oldest (index 1)
        if #matCacheOrder >= MAX_MATERIAL_CACHE then
            local oldestName = matCacheOrder[1]
            if oldestName then
                matCache[oldestName] = nil
                matCacheIndex[oldestName] = nil
                table.remove(matCacheOrder, 1)
                -- Update indices for shifted items
                for i = 1, #matCacheOrder do
                    matCacheIndex[matCacheOrder[i]] = i
                end
            end
        end
        
        mat = Material(name)
        matCache[name] = mat
        matCacheOrder[#matCacheOrder + 1] = name
        matCacheIndex[name] = #matCacheOrder
        return mat
    end

    -- ============================
    -- Lightweight Job Scheduler
    -- ============================
    local jobArray = {}
    local jobsDirty = false
    
    function RemixRenderCore.ScheduleJob(id, fn)
        if not id or not isfunction(fn) then return end
        if not jobs[id] then
            jobsDirty = true
        end
        jobs[id] = fn
    end

    function RemixRenderCore.CancelJob(id)
        if jobs[id] then
            jobs[id] = nil
            jobsDirty = true
        end
    end

    -- Smoothed frame budget calculation
    function RemixRenderCore.UpdateFrameBudget(spent, currentBudget)
        currentBudget = currentBudget or 0.003
        
        -- Add to history
        frameBudgetHistory[#frameBudgetHistory + 1] = spent
        if #frameBudgetHistory > FRAME_BUDGET_SAMPLES then
            table.remove(frameBudgetHistory, 1)
        end
        
        -- Calculate exponential moving average
        local avg = 0
        local weight = 1.0
        local totalWeight = 0
        for i = #frameBudgetHistory, 1, -1 do
            avg = avg + frameBudgetHistory[i] * weight
            totalWeight = totalWeight + weight
            weight = weight * 0.8
        end
        avg = avg / totalWeight
        
        -- Adjust budget based on average, not single frame
        local newBudget = currentBudget
        if avg > currentBudget * 1.3 then
            newBudget = math.max(0.001, currentBudget * 0.95)
        elseif avg < currentBudget * 0.7 then
            newBudget = math.min(0.008, currentBudget * 1.05)
        end
        
        return newBudget
    end

    function RemixRenderCore.StepJobs(budgetMs)
        budgetMs = budgetMs or 1.5 / 1000
        
        -- Rebuild job array if dirty
        if jobsDirty then
            jobArray = {}
            for id, fn in pairs(jobs) do
                jobArray[#jobArray + 1] = {id = id, fn = fn}
            end
            jobsDirty = false
        end
        
        -- Process jobs using array iteration (faster than pairs)
        local start = SysTime()
        local i = 1
        while i <= #jobArray do
            local job = jobArray[i]
            local ok, res = pcall(job.fn)
            if not ok then
                ErrorNoHalt("[RemixRenderCore] Job '" .. tostring(job.id) .. "' error: " .. tostring(res) .. "\n")
                jobs[job.id] = nil
                table.remove(jobArray, i)
            elseif res == false then
                jobs[job.id] = nil
                table.remove(jobArray, i)
            else
                i = i + 1
            end
            if SysTime() - start > budgetMs then break end
        end
    end

    function RemixRenderCore.TrackMesh(meshObj)
        if not meshObj then return end
        meshRefs[meshObj] = true
    end

    function RemixRenderCore.DestroyMesh(meshObj)
        if not meshObj then return false end
        if not meshRefs[meshObj] then return false end
        
        local ok, err = pcall(function()
            if meshObj.Destroy then
                meshObj:Destroy()
            end
        end)
        
        if not ok then
            ErrorNoHalt("[RemixRenderCore] Failed to destroy mesh: " .. tostring(err) .. "\n")
        end
        
        meshRefs[meshObj] = nil
        return ok
    end

    function RemixRenderCore.DestroyTrackedMeshes()
        -- Collect meshes to destroy first (don't modify table during iteration)
        local toDestroy = {}
        for m, _ in pairs(meshRefs) do
            toDestroy[#toDestroy + 1] = m
        end
        
        local destroyed = 0
        local failed = 0
        
        -- Now safely destroy them
        for i = 1, #toDestroy do
            local m = toDestroy[i]
            if RemixRenderCore.DestroyMesh(m) then
                destroyed = destroyed + 1
            else
                failed = failed + 1
            end
        end
        
        if failed > 0 then
            ErrorNoHalt("[RemixRenderCore] Failed to destroy " .. failed .. " meshes (" .. destroyed .. " succeeded)\n")
        end
        
        return destroyed, failed
    end

    -- Debounce utility and rebuild dispatch
    local debounceTimers = {}
    function RemixRenderCore.Debounce(id, delay, fn)
        if not id or not isfunction(fn) then return end
        delay = delay or 0.2
        if debounceTimers[id] then
            timer.Remove(debounceTimers[id])
        end
        local tname = "RemixDebounce-" .. id
        debounceTimers[id] = tname
        timer.Create(tname, delay, 1, function()
            debounceTimers[id] = nil
            fn()
        end)
    end

    function RemixRenderCore.RegisterRebuildSink(id, fn)
        if not id or not isfunction(fn) then return end
        rebuildSinks[id] = fn
    end

    function RemixRenderCore.NewToken(name)
        if not name then name = tostring(SysTime()) end
        local tok = { name = name, cancelled = false }
        local prev = tokens[name]
        if prev then prev.cancelled = true end
        tokens[name] = tok
        return tok
    end

    function RemixRenderCore.CancelToken(name)
        local tok = tokens[name]
        if tok then tok.cancelled = true end
    end

    function RemixRenderCore.RequestRebuild(reason)
        RemixRenderCore.Debounce("GlobalRebuild", 0.25, function()
            for id, fn in pairs(rebuildSinks) do
                local token = RemixRenderCore.NewToken(id)
                safeCall(id, fn, token, reason)
            end
        end)
    end

    -- Unified capture mode toggle
    local captureConVar = CreateClientConVar("rtx_capture_mode", "0", true, false, "RTX Remix capture mode")
    cvars.AddChangeCallback("rtx_capture_mode", function(_, _, new)
        local on = (new == "1")
        -- r_drawworld stays at 1 so the engine always collects surfaces for overlays/decals.
        -- Temporarily restore engine world drawing by disabling the Shader_DrawChains patch.
        RunConsoleCommand("rtx_patch_skip_world_draw", on and "0" or "1")
        RunConsoleCommand("r_drawstaticprops", on and "0" or "1")
    end, "RemixRenderCoreCapture")

    -- Shared 3D skybox geometry toggle
    local sky3dConVar = GetConVar("rtx_3dsky") or CreateClientConVar("rtx_3dsky", "1", true, false, "Render 3D skybox geometry (map faces, displacements, static props)")
    function RemixRenderCore.Is3DSkyEnabled()
        return sky3dConVar:GetBool()
    end

    -- Stats registry for unified debug overlay
    function RemixRenderCore.RegisterStats(id, fn)
        if not id or not isfunction(fn) then return end
        statsFns[id] = fn
    end

    function RemixRenderCore.UnregisterStats(id)
        statsFns[id] = nil
    end

    local debugConVar = CreateClientConVar("rtx_render_debug", "0", true, false, "Show Remix render debug overlay")
    hook.Add("HUDPaint", "RemixRenderCoreDebug", function()
        if not debugConVar:GetBool() then return end
        local x, y = 10, 10
        draw.SimpleText("Remix Render Debug", "DermaDefaultBold", x, y, Color(255,255,0))
        y = y + 16
        for id, fn in pairs(statsFns) do
            if isfunction(fn) then
                local ok, line = pcall(fn)
                if ok and line and line ~= "" then
                    draw.SimpleText(line, "DermaDefault", x, y, Color(255,255,255))
                    y = y + 14
                end
            end
        end
    end)

    -- Console helpers
    concommand.Add("rtx_rebuild_all", function()
        RemixRenderCore.RequestRebuild("console")
    end)

    concommand.Add("rtx_clear_caches", function()
        RemixRenderCore.DestroyTrackedMeshes()
        for k in pairs(matCache) do matCache[k] = nil end
        print("[RemixRenderCore] Cleared mesh/material caches.")
    end)
    
    concommand.Add("rtx_perf_history", function()
        local history = RemixRenderCore.GetPerfHistory()
        if #history == 0 then
            print("[PERF] No performance events recorded.")
            return
        end
        print(string.format("[PERF] Last %d performance events:", #history))
        for i = math.max(1, #history - 20), #history do
            local entry = history[i]
            if entry.time and entry.time > 0 then
                print(string.format("  %.2fs ago: %s took %.2fms%s", 
                    SysTime() - entry.when, 
                    entry.name, 
                    entry.time * 1000,
                    entry.extra and (" - " .. entry.extra) or ""))
            else
                print(string.format("  %.2fs ago: %s%s", 
                    SysTime() - entry.when, 
                    entry.name,
                    entry.extra and (" - " .. entry.extra) or ""))
            end
        end
    end)
    
    concommand.Add("rtx_perf_clear", function()
        RemixRenderCore.ClearPerfHistory()
        print("[PERF] Performance history cleared.")
    end)
    
    concommand.Add("rtx_gc_info", function()
        local mem = collectgarbage("count")
        print(string.format("[GC] Current memory: %.2f MB", mem / 1024))
        print(string.format("[GC] GC cycles tracked: %d", gcCycleCount))
        print(string.format("[GC] Last GC step time: %.3fms", lastGCStepTime * 1000))
        print("[GC] Running full collection...")
        local before = collectgarbage("count")
        local gcStart = SysTime()
        collectgarbage("collect")
        local gcTime = SysTime() - gcStart
        local after = collectgarbage("count")
        print(string.format("[GC] Freed %.2f MB (%.2f -> %.2f MB) in %.2fms", (before - after) / 1024, before / 1024, after / 1024, gcTime * 1000))
    end)
    
    -- Add allocation rate tracking
    local allocationTracker = { startMem = 0, startTime = 0, samples = {} }
    
    concommand.Add("rtx_gc_track_start", function()
        allocationTracker.startMem = collectgarbage("count")
        allocationTracker.startTime = SysTime()
        allocationTracker.samples = {}
        print("[GC] Started allocation tracking. Use rtx_gc_track_stop to view results.")
        
        local trackHook
        trackHook = hook.Add("Think", "RemixGCAllocationTracker", function()
            local elapsed = SysTime() - allocationTracker.startTime
            if elapsed >= 10 then
                hook.Remove("Think", "RemixGCAllocationTracker")
                local finalMem = collectgarbage("count")
                local allocated = finalMem - allocationTracker.startMem
                print(string.format("[GC] Auto-stopped after 10s: Allocated %.2f MB (%.2f KB/s)", 
                    allocated / 1024, 
                    allocated / elapsed))
                return
            end
            
            if #allocationTracker.samples < 100 then
                allocationTracker.samples[#allocationTracker.samples + 1] = {
                    time = elapsed,
                    mem = collectgarbage("count")
                }
            end
        end)
    end)
    
    concommand.Add("rtx_gc_track_stop", function()
        hook.Remove("Think", "RemixGCAllocationTracker")
        local finalMem = collectgarbage("count")
        local elapsed = SysTime() - allocationTracker.startTime
        if elapsed == 0 then
            print("[GC] No tracking data. Use rtx_gc_track_start first.")
            return
        end
        
        local allocated = finalMem - allocationTracker.startMem
        print(string.format("[GC] Allocation tracking stopped after %.1fs", elapsed))
        print(string.format("[GC] Total allocated: %.2f MB (%.2f KB/s)", allocated / 1024, allocated / elapsed))
        
        if #allocationTracker.samples >= 2 then
            print("[GC] Allocation rate over time:")
            for i = 2, math.min(10, #allocationTracker.samples) do
                local prev = allocationTracker.samples[i-1]
                local curr = allocationTracker.samples[i]
                local dt = curr.time - prev.time
                local dm = curr.mem - prev.mem
                if dt > 0 then
                    print(string.format("  %.1fs: %.1f KB/s", curr.time, dm / dt))
                end
            end
        end
    end)
    
    -- Component-level allocation tracking
    concommand.Add("rtx_alloc_track_enable", function()
        if not debugEnabled:GetBool() then
            print("[ALLOC] ERROR: Debug profiling is disabled. Set rtx_debug_profiling 1 first.")
            return
        end
        allocTracking.enabled = true
        allocTracking.components = {}
        allocTracking.frameStart = collectgarbage("count")
        allocTracking.lastFrame = FrameNumber()
        print("[ALLOC] Component-level allocation tracking enabled.")
        print("[ALLOC] Use rtx_alloc_track_report to view results.")
    end)
    
    concommand.Add("rtx_alloc_track_disable", function()
        allocTracking.enabled = false
        print("[ALLOC] Component-level allocation tracking disabled.")
    end)
    
    concommand.Add("rtx_alloc_track_report", function()
        if not allocTracking.enabled and table.Count(allocTracking.components) == 0 then
            print("[ALLOC] No tracking data. Use rtx_alloc_track_enable first.")
            return
        end
        
        print("[ALLOC] Component allocation report:")
        print(string.format("[ALLOC] %-30s %12s %12s %12s", "Component", "Total (KB)", "Calls", "Avg (KB)"))
        print(string.rep("-", 75))
        
        -- Sort by total allocation
        local sorted = {}
        for name, data in pairs(allocTracking.components) do
            table.insert(sorted, {name = name, data = data})
        end
        table.sort(sorted, function(a, b) return a.data.total > b.data.total end)
        
        for _, entry in ipairs(sorted) do
            local avg = entry.data.calls > 0 and (entry.data.total / entry.data.calls) or 0
            print(string.format("[ALLOC] %-30s %12.2f %12d %12.2f", 
                entry.name, 
                entry.data.total, 
                entry.data.calls, 
                avg))
        end
        
        print(string.rep("-", 75))
        print("[ALLOC] Use rtx_alloc_track_clear to reset counters.")
    end)
    
    concommand.Add("rtx_alloc_track_clear", function()
        allocTracking.components = {}
        allocTracking.frameStart = collectgarbage("count")
        print("[ALLOC] Allocation tracking counters cleared.")
    end)
    
    -- Shared skybox-pass tracking via PreDrawSkyBox / PostDrawSkyBox
    local inSkyboxPass = false
    RemixRenderCore.Register("PreDrawSkyBox", "RemixFrame-SkyboxBegin", function()
        inSkyboxPass = true
    end)
    RemixRenderCore.Register("PostDrawSkyBox", "RemixFrame-SkyboxEnd", function()
        inSkyboxPass = false
    end)
    function RemixRenderCore.IsInSkyboxPass()
        return inSkyboxPass
    end

    -- Centralized flush hooks: begin frame on PreDrawOpaque, flush on PostDraw* passes
    RemixRenderCore.Register("PreDrawOpaqueRenderables", "RemixFrame-Begin", { fn = function(bDrawingDepth, bDrawingSkybox)
        RemixRenderCore.BeginFrame(bDrawingDepth, bDrawingSkybox)
    end, prio = 0 })

    RemixRenderCore.Register("PostDrawOpaqueRenderables", "RemixFrame-FlushOpaque", { fn = function()
        RemixRenderCore.FlushPass(false)
    end, prio = 1000 })

    RemixRenderCore.Register("PostDrawTranslucentRenderables", "RemixFrame-FlushTrans", { fn = function()
        RemixRenderCore.FlushPass(true)
    end, prio = 1000 })

    -- Global cleanup
    hook.Add("ShutDown", "RemixRenderCoreCleanup", function()
        RemixRenderCore.DestroyTrackedMeshes()
        for k in pairs(matCache) do matCache[k] = nil end
        for k in pairs(statsFns) do statsFns[k] = nil end
        RemixRenderCore.ResetPVS()
    end)
    
    hook.Add("PostCleanupMap", "RemixRenderCoreMapCleanup", function()
        RemixRenderCore.ResetPVS()
    end)
end

return RemixRenderCore


