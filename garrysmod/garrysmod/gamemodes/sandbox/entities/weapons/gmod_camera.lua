
AddCSLuaFile()

SWEP.ViewModel = Model( "models/weapons/c_arms_animations.mdl" )
SWEP.WorldModel = Model( "models/MaxOfS2D/camera.mdl" )

SWEP.Primary.ClipSize		= -1
SWEP.Primary.DefaultClip	= -1
SWEP.Primary.Automatic		= false
SWEP.Primary.Ammo			= "none"

SWEP.Secondary.ClipSize		= -1
SWEP.Secondary.DefaultClip	= -1
SWEP.Secondary.Automatic	= true
SWEP.Secondary.Ammo			= "none"

SWEP.PrintName	= "#gmod_camera"
SWEP.Author	= "Facepunch"

SWEP.Slot		= 5
SWEP.SlotPos	= 1

SWEP.DrawAmmo		= false
SWEP.DrawCrosshair	= false
SWEP.Spawnable		= true

SWEP.ShootSound = Sound( "NPC_CScanner.TakePhoto" )

SWEP.AutoSwitchTo	= false
SWEP.AutoSwitchFrom	= false

if ( SERVER ) then

	--
	-- A concommand to quickly switch to the camera
	--
	concommand.Add( "gmod_camera", function( ply, cmd, args )

		ply:SelectWeapon( "gmod_camera" )

	end )

end

--
-- Network/Data Tables
--
function SWEP:SetupDataTables()

	self:NetworkVar( "Float", 0, "Zoom" )
	self:NetworkVar( "Float", 1, "Roll" )

	if ( SERVER ) then
		self:SetZoom( 70 )
		self:SetRoll( 0 )
	end

end

--
-- Initialize Stuff
--
function SWEP:Initialize()

	self:SetHoldType( "camera" )

end

--
-- Reload resets the FOV and Roll
--
function SWEP:Reload()

	local owner = self:GetOwner()

	if ( !owner:KeyDown( IN_ATTACK2 ) ) then self:SetZoom( owner:IsBot() && 75 || owner:GetInfoNum( "fov_desired", 75 ) ) end
	self:SetRoll( 0 )

end

--
-- PrimaryAttack - make a screenshot
--
function SWEP:PrimaryAttack()

	self:DoShootEffect()

	-- If we're multiplayer this can be done totally clientside
	if ( !game.SinglePlayer() && SERVER ) then return end
	if ( CLIENT && !IsFirstTimePredicted() ) then return end

	self:GetOwner():ConCommand( "jpeg" )

end

--
-- SecondaryAttack - Nothing. See Tick for zooming.
--
function SWEP:SecondaryAttack()
end

--
-- Think - Handle HUD and net_graph hiding
--
function SWEP:Think()

	local owner = self:GetOwner()

	if ( CLIENT && owner != LocalPlayer() ) then return end -- If someone is spectating a player holding this weapon, bail

	-- Hide HUD when camera is active
	if ( CLIENT && !self.HUDHidden ) then
		RunConsoleCommand( "cl_drawhud", "0" )
		self.HUDHidden = true
	end

	-- Hide net_graph when camera is active
	if ( CLIENT && !self.NetGraphHidden ) then
		local netGraphValue = GetConVar( "net_graph" ):GetInt()
		if ( netGraphValue >= 1 ) then
			self.OriginalNetGraph = netGraphValue -- Save original value
			RunConsoleCommand( "net_graph", "0" )
		end
		self.NetGraphHidden = true
	end

end

--
-- Mouse 2 action
--
function SWEP:Tick()

	local owner = self:GetOwner()

	if ( CLIENT && owner != LocalPlayer() ) then return end -- If someone is spectating a player holding this weapon, bail

	local cmd = owner:GetCurrentCommand()

	if ( !cmd:KeyDown( IN_ATTACK2 ) ) then return end -- Not holding Mouse 2, bail

	self:SetZoom( math.Clamp( self:GetZoom() + cmd:GetMouseY() * FrameTime() * 6.6, 0.1, 175 ) ) -- Handles zooming
	self:SetRoll( self:GetRoll() + cmd:GetMouseX() * FrameTime() * 1.65 ) -- Handles rotation

end

--
-- Override players Field Of View
--
function SWEP:TranslateFOV( current_fov )

	return self:GetZoom()

end

--
-- Deploy - Allow lastinv
--
function SWEP:Deploy()

	if ( CLIENT ) then
		self.HUDHidden = false -- Reset flags
		self.NetGraphHidden = false
		self.OriginalNetGraph = nil
	end

	return true

end

--
-- Holster - Restore HUD and net_graph when switching weapons
--
function SWEP:Holster()

	if ( CLIENT ) then
		-- Restore HUD
		RunConsoleCommand( "cl_drawhud", "1" )
		self.HUDHidden = false
		
		-- Restore net_graph if we had saved a value
		if ( self.OriginalNetGraph ) then
			RunConsoleCommand( "net_graph", tostring( self.OriginalNetGraph ) )
			self.OriginalNetGraph = nil
		end
		self.NetGraphHidden = false
	end

	return true

end

--
-- OnRemove - Ensure HUD and net_graph are restored if weapon is removed unexpectedly
--
function SWEP:OnRemove()

	if ( CLIENT ) then
		-- Restore HUD
		RunConsoleCommand( "cl_drawhud", "1" )
		self.HUDHidden = false
		
		-- Restore net_graph if we had saved a value
		if ( self.OriginalNetGraph ) then
			RunConsoleCommand( "net_graph", tostring( self.OriginalNetGraph ) )
			self.OriginalNetGraph = nil
		end
		self.NetGraphHidden = false
	end

end

--
-- Set FOV to players desired FOV
--
function SWEP:Equip()

	local owner = self:GetOwner()

	if ( self:GetZoom() == 70 && owner:IsPlayer() && !owner:IsBot() ) then
		self:SetZoom( owner:GetInfoNum( "fov_desired", 75 ) )
	end

end

function SWEP:ShouldDropOnDie() return false end

--
-- The effect when a weapon is fired successfully
--
function SWEP:DoShootEffect()

	local owner = self:GetOwner()

	self:EmitSound( self.ShootSound )
	self:SendWeaponAnim( ACT_VM_PRIMARYATTACK )
	owner:SetAnimation( PLAYER_ATTACK1 )

	if ( SERVER && !game.SinglePlayer() ) then

		--
		-- Note that the flash effect is only
		-- shown to other players!
		--

		local vPos = owner:GetShootPos()
		local vForward = owner:GetAimVector()

		local trace = {}
		trace.start = vPos
		trace.endpos = vPos + vForward * 256
		trace.filter = owner

		local tr = util.TraceLine( trace )

		local effectdata = EffectData()
		effectdata:SetOrigin( tr.HitPos )
		util.Effect( "camera_flash", effectdata, true )

	end

end

if ( SERVER ) then return end -- Only clientside lua after this line

SWEP.WepSelectIcon = surface.GetTextureID( "vgui/gmod_camera" )

-- HUD is now controlled by cl_drawhud console command in Deploy/Holster

function SWEP:FreezeMovement()

	local owner = self:GetOwner()

	-- Don't aim if we're holding the right mouse button
	if ( owner:KeyDown( IN_ATTACK2 ) || owner:KeyReleased( IN_ATTACK2 ) ) then
		return true
	end

	return false

end

function SWEP:CalcView( ply, origin, angles, fov )

	if ( self:GetRoll() != 0 ) then
		angles.Roll = self:GetRoll()
	end

	return origin, angles, fov

end

function SWEP:AdjustMouseSensitivity()

	if ( self:GetOwner():KeyDown( IN_ATTACK2 ) ) then return 1 end

	return self:GetZoom() / 80

end
