-- Console commands to quickly verify Remix API lights in-game

local function vec(x, y, z) return { x = x, y = y, z = z } end

local function spawn_sphere_for_ent(ent, radius, r, g, b)
  if not IsValid(ent) or not RemixLight then return nil end
  radius = tonumber(radius) or 40
  r = tonumber(r) or 20
  g = tonumber(g) or 16
  b = tonumber(b) or 12

  local pos = ent:GetPos() + Vector(0, 0, 64)
  local dir = (ent:EyeAngles() or ent:GetAngles()):Forward()

  local base = {
    hash = tonumber(util.CRC(string.format("sphere_light_cmd_%d", ent:EntIndex()))),
    radiance = vec(r, g, b),
  }

  local sphere = {
    position = vec(pos.x, pos.y, pos.z),
    radius = radius,
    shaping = {
      direction = vec(dir.x, dir.y, dir.z),
      coneAngleDegrees = 35.0,
      coneSoftness = 0.2,
      focusExponent = 1.0,
    },
    volumetricRadianceScale = 1.0,
  }

  return RemixLight.CreateSphere(base, sphere, ent:EntIndex())
end

concommand.Add("remix_light_spawn", function(ply, cmd, args)
  local lp = LocalPlayer()
  if not IsValid(lp) then return end
  local id = spawn_sphere_for_ent(lp, args[1], args[2], args[3], args[4])
  if id then
    print(string.format("[RemixLight] Spawned sphere light id=%d", id))
    -- the native present callback will submit queued lights; we also queue explicitly in C++
  else
    print("[RemixLight] Failed to spawn light (RemixLight not ready?)")
  end
end)

concommand.Add("remix_light_clear", function()
  local lp = LocalPlayer()
  if not IsValid(lp) or not RemixLight then return end
  RemixLight.DestroyLightsForEntity(lp:EntIndex())
  print("[RemixLight] Cleared lights for LocalPlayer")
end)


