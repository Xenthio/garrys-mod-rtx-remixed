-- Shitty solution to have a shadow for the player, weapon edition wow
CreateConVar( "rtx_debug_pseudoplayer", 0, FCVAR_ARCHIVE )
CreateClientConVar(	"rtx_pseudoweapon_unique_hashes", 1,  true, false)
CreateClientConVar(	"rtx_pseudoweapon_unique_hashes_downscale", 1,  true, false)
AddCSLuaFile()

ENT.Type 			= "anim"
ENT.PrintName		= "Pseudoweapon"
ENT.Author			= "Xenthio"
ENT.Information		= "For firstperson self shadows and reflections with RTX"
ENT.Category		= "RTX"

ENT.Spawnable		= false
ENT.AdminSpawnable	= false

local pseudoweapon
local materialSuffix = ""  -- Unique suffix for material names
local prevclassname = ""

function ENT:Initialize()
	if pseudoweapon then
		pseudoweapon:Remove()
	end

	weaponmodel = "models/weapons/w_pistol.mdl"
	if LocalPlayer():GetActiveWeapon():IsValid() and (LocalPlayer():GetActiveWeapon():GetWeaponWorldModel() != "") then
		weaponmodel = LocalPlayer():GetActiveWeapon():GetModel()
	end

	print("[gmRTX] - Pseudoweapon Initialised.")
	self:SetModel(weaponmodel)
	self:SetParent(LocalPlayer():GetActiveWeapon())
	self:AddEffects( EF_BONEMERGE ) 
	self:SetMoveType( MOVETYPE_NONE )

	pseudoweapon = ClientsideModel(weaponmodel)
	pseudoweapon:SetParent(self)
	pseudoweapon:AddEffects( EF_BONEMERGE ) 
	pseudoweapon.RenderOverride = PseudoweaponRender
end

local materialtable = {}

-- We use this to override materials in dx7 mode.
function PseudoweaponRender(self) 
	

	if (!materialtable) then return end
	if (!GetConVar( "rtx_pseudoweapon_unique_hashes" ):GetBool()) then 
		render.ModelMaterialOverride(nil,nil)
		render.SuppressEngineLighting( true )
		self:DrawModel()
		render.SuppressEngineLighting( false )
		return
	else
		--render.MaterialOverride(nil)
		for k, v in pairs(materialtable) do
	
			--print(k)
			--self:SetSubMaterial(k, "!pseudoplayermaterial" .. k)
			render.MaterialOverrideByIndex( k-1, v ) 
			--render.ModelMaterialOverride( Material("!pseudoplayermaterial" .. k))
		end
		render.SuppressEngineLighting( true )
		self:DrawModel()
		render.SuppressEngineLighting( false )
		render.ModelMaterialOverride(nil,nil)
	end 
	
end

-- This fucking piece of shit allows us to have a slighly modified texture for the local players own weapons.
-- This is so we can have it be invisible but still cast shadows and draw in reflections, while not making other weapons invisible.
local function MaterialSet()
	if (!pseudoweapon) then return end 
	if (!GetConVar( "rtx_pseudoweapon_unique_hashes" ):GetBool()) then return end

	
	-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/5826
	pseudoweapon.RenderOverride = PseudoweaponRender 
	for k, v in pairs(pseudoweapon:GetMaterials()) do
		local mat = Material(v)
		local tex = mat:GetTexture( "$basetexture" )   

	    -- create a copy so we can have the texture be drawn unlit.
		local matblank = CreateMaterial( "pseudoweaponmaterialtemp" .. k .. materialSuffix, "UnlitGeneric", {
			["$basetexture"] = "color/white",
			["$model"] = 1,
			["$translucent"] = 0,
			["$vertexalpha"] = 0,
			["$vertexcolor"] = 0,  
		} )
	    -- we need to create a second material so we can write the alpha, since gmod doesnt do it properly for some reason.
		local matblankalpha = CreateMaterial( "pseudoweaponmaterialtempalpha" .. k .. materialSuffix, "UnlitGeneric", {
			["$basetexture"] = "color/white",
			["$model"] = 1,
			["$translucent"] = 1,
			["$vertexalpha"] = 0,
			["$vertexcolor"] = 0,  
		} )
		matblank:SetTexture( "$basetexture", tex )
		matblankalpha:SetTexture( "$basetexture", tex )

		tex:Download()

		local width = tex:Width() / GetConVar( "rtx_pseudoweapon_unique_hashes_downscale" ):GetInt()
		local height = tex:Height() / GetConVar( "rtx_pseudoweapon_unique_hashes_downscale" ):GetInt()
		local texname = "pseudoweapontexture" .. k .. width .. "x" .. height .. materialSuffix -- we need to create one unique for different widths and heights.
		local newtex = GetRenderTargetEx( texname, width, height, RT_SIZE_LITERAL, MATERIAL_RT_DEPTH_NONE, 0, 0, IMAGE_FORMAT_RGBA8888 ) 
		render.PushRenderTarget( newtex )
			cam.Start2D()
				render.OverrideAlphaWriteEnable( true, true )

				-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/2571
				render.SetWriteDepthToDestAlpha( false )
				--render.SuppressEngineLighting( true ) 
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
					h 	= 1,
				} 
				draw.TexturedQuad( texturedQuadStructure )

				--render.SuppressEngineLighting( false )
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
		local matlua = CreateMaterial( "pseudoweaponmaterial" .. k .. materialSuffix, mat:GetShader(), kv )
		matlua:SetTexture( "$basetexture", newertex)

		-- add them to the table, we apply the new material every frame in the RenderOverride, since Entity:SetMaterial() is broken in dx7 mode.
		-- Workaround for issue: https://github.com/Facepunch/garrysmod-issues/issues/5826
		materialtable[k] = matlua
	end
end

function ENT:Think()
	if not pseudoweapon or not pseudoweapon:IsValid() then
		weaponmodel = "models/weapons/w_pistol.mdl"
		if LocalPlayer():GetActiveWeapon():IsValid() and (LocalPlayer():GetActiveWeapon():GetWeaponWorldModel() != "") then
			weaponmodel = LocalPlayer():GetActiveWeapon():GetModel()
		end
		pseudoweapon = ClientsideModel(weaponmodel)
		pseudoweapon:SetParent(self)
		pseudoweapon:AddEffects( EF_BONEMERGE )
		pseudoweapon:SetRenderMode(2)
		pseudoweapon:SetColor(Color(255,255,255,0))
	end
	if GetConVar( "rtx_pseudoweapon" ):GetBool() == false then
		if pseudoweapon then
			pseudoweapon:Remove()
		end
		self:Remove()
	end

	if LocalPlayer():GetActiveWeapon():IsValid() and pseudoweapon != nil and LocalPlayer():Alive() then
		pcall(function() LocalPlayer():GetActiveWeapon():DrawWorldModel() end)
		if prevclassname != LocalPlayer():GetActiveWeapon():GetClass() then--or LocalPlayer():GetActiveWeapon():GetModel() != self:GetModel() then
			prevclassname = LocalPlayer():GetActiveWeapon():GetClass()
			self:RemoveEffects( EF_BONEMERGE )
			self:SetModel(LocalPlayer():GetActiveWeapon():GetModel())
			self:SetParent(LocalPlayer():GetActiveWeapon():GetParent(), LocalPlayer():GetActiveWeapon():GetParentAttachment())

			--self:SetPos(LocalPlayer():GetActiveWeapon():GetPos())
			--self:SetAngles(LocalPlayer():GetActiveWeapon():GetAngles())
			--self:SetupBones()

			self:AddEffects( EF_BONEMERGE )

			pseudoweapon:RemoveEffects( EF_BONEMERGE )
			pseudoweapon:SetModel(LocalPlayer():GetActiveWeapon():GetModel())
			pseudoweapon:SetParent(self)
			pseudoweapon:AddEffects( EF_BONEMERGE )
			
			-- Generate deterministic suffix based on weapon class so the same weapon produces the same hash
			local weaponClass = LocalPlayer():GetActiveWeapon():GetClass() or "unknown"
			materialSuffix = "_" .. util.CRC(weaponClass)
			print("[gmRTX] - Weapon changed, material suffix: " .. materialSuffix .. " (" .. weaponClass .. ")")
			
			-- Clear material table and recreate materials
			materialtable = {}
			MaterialSet()
			 
		end
		self:SetRenderOrigin(LocalPlayer():GetActiveWeapon():GetRenderOrigin())
		self:SetRenderAngles(LocalPlayer():GetActiveWeapon():GetRenderAngles())
		self:SetModelScale(LocalPlayer():GetActiveWeapon():GetModelScale())

		pcall(function() -- Customisable Weaponry Fix.
			self:SetRenderOrigin(LocalPlayer():GetActiveWeapon().WMEnt:GetRenderOrigin())
			self:SetRenderAngles(LocalPlayer():GetActiveWeapon().WMEnt:GetRenderAngles())
		end)


		pseudoweapon:SetModelScale(LocalPlayer():GetActiveWeapon():GetModelScale())
		pseudoweapon:SetNoDraw( false )
	else
		pseudoweapon:SetNoDraw( true )
	end

	if (LocalPlayer():GetActiveWeapon():IsValid() and LocalPlayer():GetActiveWeapon():GetWeaponWorldModel() == "") then
		pseudoweapon:SetNoDraw( true )
	end
	if LocalPlayer():GetObserverMode() != OBS_MODE_NONE or (LocalPlayer():GetViewEntity() != LocalPlayer()) or LocalPlayer():ShouldDrawLocalPlayer() then
		pseudoweapon:SetNoDraw( true )
	end
   -- LocalPlayer():GetActiveWeapon():AddEFlags(EFL_FORCE_CHECK_TRANSMIT)
	--LocalPlayer():GetActiveWeapon():AddEFlags(EFL_IN_SKYBOX)
	--LocalPlayer():GetActiveWeapon():RemoveEFlags(EF_NODRAW)
	--debugoverlay.Text( self:GetPos(), "hello!", 0.001)
	--debugoverlay.Text( LocalPlayer():GetActiveWeapon():GetPos(), "PlyrWepon", 0.001)
	--pseudoweapon:AddEFlags(EFL_FORCE_CHECK_TRANSMIT)
	--pseudoweapon:AddEFlags(EFL_IN_SKYBOX)
	--self:AddEFlags(EFL_FORCE_CHECK_TRANSMIT)
	--self:AddEFlags(EFL_IN_SKYBOX)


end

function ENT:OnRemove()
	if pseudoweapon then
		pseudoweapon:Remove()
	end
end
-- remove on auto refresh
hook.Add("OnReloaded", "RTXOnAutoReloadPseudoweapon", function()
	ENT:Remove()
end)
