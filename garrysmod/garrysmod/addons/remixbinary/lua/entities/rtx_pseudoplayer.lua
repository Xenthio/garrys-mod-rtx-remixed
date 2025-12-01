-- Shitty solution to have a shadow for the player
CreateConVar( "rtx_debug_pseudoplayer", 0,  FCVAR_ARCHIVE ) 
CreateClientConVar(	"rtx_pseudoplayer_unique_hashes", 1,  true, false)
CreateClientConVar(	"rtx_pseudoplayer_offset_localangles", 0,  true, false)
CreateClientConVar(	"rtx_pseudoplayer_offset_x", 0,  true, false)
CreateClientConVar(	"rtx_pseudoplayer_offset_z", 0,  true, false)
AddCSLuaFile()

ENT.Type 			= "anim"
ENT.PrintName		= "Pseudoplayer"
ENT.Author			= "Xenthio"
ENT.Information		= "For firstperson self shadows and body reflections with RTX"
ENT.Category		= "RTX"

ENT.Spawnable		= false
ENT.AdminSpawnable	= false

local pseudoweapon
local pseudoplayer
local materialsset
local materialSuffix = ""  -- Unique suffix for material names

function ENT:Initialize()
	if pseudoplayer then
		pseudoplayer:Remove()
	end
	materialsset = false
	RunConsoleCommand("r_flashlightnear", "50")

	print("[gmRTX] - Pseudoplayer Initialised.")
	self:SetModel(LocalPlayer():GetModel())
	self:SetParent(LocalPlayer())
	self:AddEffects( EF_BONEMERGE )
	self:SetPos(LocalPlayer():GetPos()) 

	pseudoplayer = ClientsideModel(LocalPlayer():GetModel())
	pseudoplayer:SetMoveType(MOVETYPE_NONE)
	pseudoplayer:SetParent(self)
	pseudoplayer:AddEffects( EF_BONEMERGE )
	pseudoplayer.RenderOverride = PseudoplayerRender

	pseudoweapon = ents.CreateClientside( "rtx_pseudoweapon" )
	pseudoweapon:Spawn() 
	pseudoweapon:SetParent(pseudoplayer)
	
end

local materialtable = {}
local materialsset = false


function OffsetStuff()
	if (GetConVar("rtx_pseudoplayer_offset_localangles"):GetBool()) then
		LocalPlayer():SetAngles(LocalPlayer():GetRenderAngles())
		local angles = LocalPlayer():GetRenderAngles()
		local localangle = LocalPlayer():EyeAngles()
		localangle.pitch = 0
		
		--localangle:Normalize()
		local posoffset = localangle:Forward() * GetConVar("rtx_pseudoplayer_offset_x"):GetFloat()
		
		posoffset = posoffset + localangle:Up() * GetConVar("rtx_pseudoplayer_offset_z"):GetFloat()
		local worldposoffset = LocalPlayer():GetPos() + posoffset
		local localposoffset = LocalPlayer():WorldToLocal( worldposoffset )
		LocalPlayer():ManipulateBonePosition(0,localposoffset)

		local localangleoffset = Angle(angles.yaw - localangle.yaw, 0, 0)
		LocalPlayer():ManipulateBoneAngles(0,-1 *localangleoffset)
	else
		LocalPlayer():ManipulateBonePosition(0,Vector(GetConVar("rtx_pseudoplayer_offset_x"):GetFloat(),0,0))
	end
end

-- We use this to override materials in dx7 mode.
function PseudoplayerRender(self) 

	-- guy wanted offsets so he could use the addon with gmod legs 3, we do them here.
	OffsetStuff()

	if (!materialtable) then return end
	
	if (!GetConVar( "rtx_pseudoplayer_unique_hashes" ):GetBool()) then 
		render.ModelMaterialOverride(nil,nil)
		render.SuppressEngineLighting( true )
		self:DrawModel()
		render.SuppressEngineLighting( false )
		return
	else
		for k, v in pairs(materialtable) do

		-- SetMaterial and SetSubMaterial don't work in dx8 or dx7 mode, luckily render.MaterialOverride does.
		-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/5826
			render.MaterialOverrideByIndex( k-1, v ) 
			
		end
		render.SuppressEngineLighting( true )
		self:DrawModel()
		render.SuppressEngineLighting( false )
		render.ModelMaterialOverride(nil,nil)
	end 
	
end


-- This fucking piece of shit allows us to have a slighly modified texture for the local player.
-- This is so we can have it be invisible but still cast shadows and draw in reflections, while not making other players invisible.
local function MaterialSet()
	if (!pseudoplayer) then return end
	if (materialsset) then return end
	
	-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/5826
	pseudoplayer.RenderOverride = PseudoplayerRender

	-- this is set to false on hotload.
	materialsset = true
	
	for k, v in pairs(pseudoplayer:GetMaterials()) do
		local mat = Material(v)
		local tex = mat:GetTexture( "$basetexture" )   

		-- create a copy so we can have the texture be drawn unlit.
		local matblank = CreateMaterial( "pseudoplayermaterialtemp" .. k .. materialSuffix, "UnlitGeneric", {
			["$basetexture"] = "color/white",
			["$model"] = 1,
			["$translucent"] = 0,
		} )
		-- we need to create a second material so we can write the alpha, since gmod doesnt do it properly for some reason.
		local matblankalpha = CreateMaterial( "pseudoplayermaterialtempalpha" .. k .. materialSuffix, "UnlitGeneric", {
			["$basetexture"] = "color/white",
			["$model"] = 1,
			["$translucent"] = 1,
		} )
		matblank:SetTexture( "$basetexture", tex )
		matblankalpha:SetTexture( "$basetexture", tex )

		local texname = "pseudoplayertexture" .. k .. tex:Width() .. "x" .. tex:Height() .. materialSuffix -- we need to create one unique for different widths and heights.
		local newtex = GetRenderTargetEx( texname, tex:Width(), tex:Height(), RT_SIZE_LITERAL, MATERIAL_RT_DEPTH_NONE, 0, 0, IMAGE_FORMAT_RGBA8888 ) 
		
		render.PushRenderTarget( newtex )
			cam.Start2D()
				render.OverrideAlphaWriteEnable( true, true )

				-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/2571
				render.SetWriteDepthToDestAlpha( false )

				render.ClearDepth()
				render.Clear( 0, 0, 0, 0 )

				-- Draw the base texture (alpha on all pixels is 0)
				render.SetMaterial( matblank )
				render.DrawScreenQuad() 
				-- overlay the properly transparent texture (gives it alpha where its supposed to have it)
				render.SetMaterial( matblankalpha )
				render.DrawScreenQuad() 

				-- Change the texture the tiniest amount.
				local texturedQuadStructure = {
					texture = surface.GetTextureID( "vgui/gradv" ),
					color   = Color( 255, 255, 255, 50 ),
					x 	= 0,
					y 	= 0,
					w 	= 1,
					h 	= 1
				}

				draw.TexturedQuad( texturedQuadStructure )

				render.OverrideAlphaWriteEnable( false )
			cam.End2D()

			-- We cant take our rendertarget and convert it to a non rendertarget texture, so we need to be evil and write to disk :(
			local data = render.Capture({
				format = "png",
				x = 0, 
				y = 0, 
				h = newtex:Height(), 
				w = newtex:Width(),
				alpha = true
			})
			local pictureFile = file.Open( texname .. ".png", "wb", "DATA" )	
			pictureFile:Write( data )
			pictureFile:Close() 
		render.PopRenderTarget()

		-- load our written texture as a material so its an actual texture.
		local matimg = Material( "data/" .. texname .. ".png", "smooth")
		local newertex = matimg:GetTexture( "$basetexture" )

		-- Create our final material, since we need custom keyvalues and we cant do that when we make a material from a png. so instead we copy the texture from above.
		local kv = mat:GetKeyValues() 
		local matlua = CreateMaterial( "pseudoplayermaterial" .. k .. materialSuffix, mat:GetShader(), kv )
		matlua:SetTexture( "$basetexture", newertex)

		-- this is the only keyvalue not copied for some reason??
		matlua:SetVector("$color2", kv["$color2"]) 

		-- add them to the table, we apply the new material every frame in the RenderOverride, since Entity:SetMaterial() is broken in dx7 mode.
		-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/5826
		materialtable[k] = matlua
	end
end

function ENT:Think() 
	MaterialSet()
	if not pseudoplayer or not pseudoplayer:IsValid() or pseudoplayer == nil then
		pseudoplayer = ClientsideModel(LocalPlayer():GetModel())
		pseudoplayer:SetMoveType(MOVETYPE_NONE)
		pseudoplayer:SetParent(self)
		pseudoplayer:AddEffects( EF_BONEMERGE )

		pseudoplayer:SetRenderMode(2)
		pseudoplayer:SetColor(Color(255,255,255,0))
	end
	if GetConVar( "rtx_pseudoplayer" ):GetBool() == false then
		if pseudoplayer then
			pseudoplayer:Remove()
		end
		self:Remove()
	end
	if GetConVar( "rtx_pseudoweapon" ):GetBool() == false then
		if (pseudoweapon and pseudoweapon:IsValid()) then
			pseudoweapon:Remove()
		end

	else 
		if (!pseudoweapon or !pseudoweapon:IsValid()) then
			pseudoweapon = ents.CreateClientside( "rtx_pseudoweapon" )
			pseudoweapon:Spawn()
		end
	end
	if LocalPlayer():Alive() then
		pseudoplayer:SetNoDraw( false )
	else
		pseudoplayer:SetNoDraw( true )
	end
	if LocalPlayer():GetObserverMode() != OBS_MODE_NONE or (LocalPlayer():GetViewEntity() != LocalPlayer()) or LocalPlayer():ShouldDrawLocalPlayer() then
		pseudoplayer:SetNoDraw( true )
	end
	if pseudoplayer:GetModel() != LocalPlayer():GetModel() then
		print("[gmRTX] - Pseudoplayer model changed.")
		self:RemoveEffects( EF_BONEMERGE )
		self:SetModel(LocalPlayer():GetModel())
		self:SetParent(LocalPlayer())
		self:AddEffects( EF_BONEMERGE )

		pseudoplayer:RemoveEffects( EF_BONEMERGE )
		pseudoplayer:SetModel(LocalPlayer():GetModel())
		pseudoplayer:SetParent(self)
		pseudoplayer:AddEffects( EF_BONEMERGE )
		
		-- Generate deterministic suffix based on player model so the same model produces the same hash
		local playerModel = LocalPlayer():GetModel() or "unknown"
		materialSuffix = "_" .. util.CRC(playerModel)
		print("[gmRTX] - Player model changed, material suffix: " .. materialSuffix .. " (" .. playerModel .. ")")
		
		-- Clear material table and force recreation
		materialtable = {}
		materialsset = false 
	end

	
	
	
	 


	for k = 1, LocalPlayer():GetNumBodyGroups() do
		pseudoplayer:SetBodygroup(k, LocalPlayer():GetBodygroup(k))
	end
end
-- function CalcAbsolutePosition(self, pos, ang )
--	 print(pos)
--	 local offset = pseudoplayer:GetAngles():Forward() * GetConVar("rtx_pseudoplayer_offset_x"):GetFloat()
--	 return pos - offset, ang
-- end
function ENT:OnRemove()
	RunConsoleCommand("r_flashlightnear", "4")
	pseudoplayer:Remove()
	if pseudoweapon and pseudoweapon:IsValid() then
		pseudoweapon:Remove()
	end
end
-- remove on auto refresh
hook.Add("OnReloaded", "RTXOnAutoReloadPseudoplayer", function()
	ENT:Remove()
end)

