if not CLIENT then return end

local function FixupBlankMaterial(mat, filepath)
	local blankmat = Material("debug/particleerror")
	mat:SetTexture( "$basetexture", blankmat:GetTexture("$basetexture") )
end

local function FixupGUIMaterial(mat, filepath)
	local blankmat = Material("rtx/guiwhite")
	mat:SetTexture( "$basetexture", blankmat:GetTexture("$basetexture") )
end

local function FixupParticleMaterial(mat, filepath)
	mat:SetInt( "$additive", 1 )
end

-- Trying to fix these crash the game 
local bannedmaterials = {
	"materials/particle/warp3_warp_noz.vmt",
	"materials/particle/warp4_warp.vmt",
	"materials/particle/warp4_warp_noz.vmt",
	"materials/particle/warp5_warp.vmt",
	"materials/particle/warp5_explosion.vmt",
	"materials/particle/warp_ripple.vmt"
}
local function FixupMaterial(filepath)
	
	for k, v in pairs(bannedmaterials) do
		if (v == filepath) then 
			--print("[RTX Fixes] - Skipping material " .. filepath)
			return 
		end
	end

	--print("[RTX Fixes] - Fixing material " .. filepath)
	local mattrim = (filepath:sub(0, #"materials/") == "materials/") and filepath:sub(#"materials/"+1) or filepath
	local matname = mattrim:gsub(".vmt".."$", "");
	local mat = Material(matname)
	--print("[RTX Fixes] - (Shader: " .. mat:GetShader() .. ")")
	--print("[RTX Fixes] - (Texture: " .. mat:GetString("$basetexture") .. ")")

	--coroutine.wait( 0.01 )
	if (mat:IsError()) then
		print("[RTX Fixes] - This texture loaded as an error? Trying to fix anyways but this shouldn't happen.")
	end

	-- TODO: How to fetch water in a level?
	-- if (mat:GetString("$basetexture") == "dev/water" || mat:GetShader() == "Water_DX60" ) then -- this is water, make it water
	-- 	FixupWaterMaterial(mat, filepath)
	-- end
	if (mat:GetString("$addself") != nil) then
		FixupParticleMaterial(mat, filepath)
	end
	if (mat:GetString("$basetexture") == nil) then
		FixupBlankMaterial(mat, filepath)
	end
end

local function MaterialFixupInSubDir(dir)
	--print("[RTX Fixes] - Fixing materials in " .. dir)

	local allfiles, _ = file.Find( dir .. "*.vmt", "GAME" )
	for k, v in pairs(allfiles) do
		FixupMaterial(dir .. v)
	end
end

local function MaterialFixupInDir(dir) 
	
	print("[RTX Fixes] - Starting root material fixup in " .. dir)
	local _, allfolders = file.Find( dir .. "*" , "GAME" )
	MaterialFixupInSubDir(dir)
	for k, v in pairs(allfolders) do
		MaterialFixupInSubDir(dir .. v .. "/")
	end
end

local function MaterialFixups()
	MaterialFixupInDir("materials/particle/")
	MaterialFixupInDir("materials/effects/")

	-- giving things a real texture makes remix less confused
	FixupGUIMaterial(Material("vgui/white"), "materials/vgui/white.vmt")
	FixupGUIMaterial(Material("vgui/white_additive"), "materials/vgui/white_additive.vmt") 
	FixupGUIMaterial(Material("vgui/black"), "materials/vgui/black.vmt")
	FixupGUIMaterial(Material("white"), "white")
	FixupGUIMaterial(Material("VGUI_White"), "VGUI_White")
	FixupGUIMaterial(Material("!VGUI_White"), "VGUI_White") -- Dynamically created in vgui2.dll I think.
	FixupGUIMaterial(Material("!white"), "white")
	--WantsMaterialFixup = false
end

CreateClientConVar(	"rtx_fixmaterials", 1,  true, false) 
local function RTXLoadMaterialFixer()
    concommand.Add( "rtx_fixmaterials_fixnow", MaterialFixups)
    -- start fixing up materials, can freeze the game :(
    if (GetConVar( "rtx_fixmaterials" ):GetBool()) then MaterialFixups() end
end

hook.Add( "InitPostEntity", "RTXReady_MaterialFixer", RTXLoadMaterialFixer)  