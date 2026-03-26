#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>

using namespace GarrysMod::Lua;

namespace RemixAPI {

static bool SetAtmosphereVar(const std::string& key, const std::string& value) {
    auto& cfg = RemixAPI::Instance().GetConfigManager();
    return cfg.SetConfigVariable(key, value);
}

static std::string FloatToString(double v) {
    std::ostringstream ss;
    ss << static_cast<float>(v);
    return ss.str();
}

static std::string Vec3ToString(double r, double g, double b) {
    std::ostringstream ss;
    ss << static_cast<float>(r) << ", "
       << static_cast<float>(g) << ", "
       << static_cast<float>(b);
    return ss.str();
}

// ============================================================================
// Sky mode
// ============================================================================

LUA_FUNCTION(RemixAtmosphere_SetSkyMode) {
    LUA->CheckType(1, Type::Number);
    int mode = static_cast<int>(LUA->GetNumber(1));
    LUA->PushBool(SetAtmosphereVar("rtx.skyMode", std::to_string(mode)));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_GetSkyMode) {
    auto& cfg = RemixAPI::Instance().GetConfigManager();
    std::string val = cfg.GetConfigVariable("rtx.skyMode");
    if (val.empty()) val = "0";
    LUA->PushNumber(std::stod(val));
    return 1;
}

// ============================================================================
// Sun parameters
// ============================================================================

LUA_FUNCTION(RemixAtmosphere_SetSunElevation) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunElevation", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetSunRotation) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunRotation", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetSunSize) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunSize", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetSunIntensity) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunIntensity", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetSunDisc) {
    LUA->CheckType(1, Type::Bool);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunDisc", LUA->GetBool(1) ? "True" : "False"));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetSunIlluminance) {
    LUA->CheckType(1, Type::Number);
    LUA->CheckType(2, Type::Number);
    LUA->CheckType(3, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.sunIlluminance",
        Vec3ToString(LUA->GetNumber(1), LUA->GetNumber(2), LUA->GetNumber(3))));
    return 1;
}

// ============================================================================
// Density multipliers
// ============================================================================

LUA_FUNCTION(RemixAtmosphere_SetAirDensity) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.airDensity", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetDustDensity) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.aerosolDensity", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetOzoneDensity) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.ozoneDensity", FloatToString(LUA->GetNumber(1))));
    return 1;
}

// ============================================================================
// Planet / observer
// ============================================================================

LUA_FUNCTION(RemixAtmosphere_SetAltitude) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.altitude", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetPlanetRadius) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.planetRadius", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetAtmosphereThickness) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", FloatToString(LUA->GetNumber(1))));
    return 1;
}

// ============================================================================
// Advanced scattering parameters
// ============================================================================

LUA_FUNCTION(RemixAtmosphere_SetMieAnisotropy) {
    LUA->CheckType(1, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", FloatToString(LUA->GetNumber(1))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetRayleighScattering) {
    LUA->CheckType(1, Type::Number);
    LUA->CheckType(2, Type::Number);
    LUA->CheckType(3, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.rayleighScattering",
        Vec3ToString(LUA->GetNumber(1), LUA->GetNumber(2), LUA->GetNumber(3))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetMieScattering) {
    LUA->CheckType(1, Type::Number);
    LUA->CheckType(2, Type::Number);
    LUA->CheckType(3, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.mieScattering",
        Vec3ToString(LUA->GetNumber(1), LUA->GetNumber(2), LUA->GetNumber(3))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetOzoneAbsorption) {
    LUA->CheckType(1, Type::Number);
    LUA->CheckType(2, Type::Number);
    LUA->CheckType(3, Type::Number);
    LUA->PushBool(SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption",
        Vec3ToString(LUA->GetNumber(1), LUA->GetNumber(2), LUA->GetNumber(3))));
    return 1;
}

LUA_FUNCTION(RemixAtmosphere_SetOzoneLayer) {
    LUA->CheckType(1, Type::Number);
    LUA->CheckType(2, Type::Number);
    bool ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", FloatToString(LUA->GetNumber(1)));
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", FloatToString(LUA->GetNumber(2))) && ok;
    LUA->PushBool(ok);
    return 1;
}

// ============================================================================
// Presets (mirrors the ImGui preset buttons in dxvk-remix-gmod)
// ============================================================================

static bool ApplyPresetEarth() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "20, 20, 20") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "6371") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "100") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.0058, 0.0135, 0.0331") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.003996, 0.003996, 0.003996") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.97") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0.00204, 0.00497, 0.000214") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "25") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "15") && ok;
    return ok;
}

static bool ApplyPresetMars() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "15, 12, 10") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "3389.5") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "50") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.008, 0.01, 0.012") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.008, 0.008, 0.008") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.7") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0, 0, 0") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "0") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "1") && ok;
    return ok;
}

static bool ApplyPresetClearSky() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "25, 25, 25") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "6371") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "80") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.004, 0.009, 0.022") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.001, 0.001, 0.001") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.9") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0.00204, 0.00497, 0.000214") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "25") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "15") && ok;
    return ok;
}

static bool ApplyPresetHazy() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "18, 18, 18") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "6371") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "100") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.0058, 0.0135, 0.0331") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.012, 0.012, 0.012") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.65") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0.00204, 0.00497, 0.000214") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "25") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "15") && ok;
    return ok;
}

static bool ApplyPresetAlien() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "15, 22, 18") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "5000") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "120") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.004, 0.018, 0.01") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.005, 0.005, 0.005") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.75") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0.001, 0.0005, 0.003") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "30") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "20") && ok;
    return ok;
}

static bool ApplyPresetDesert() {
    bool ok = true;
    ok = SetAtmosphereVar("rtx.atmosphere.sunIlluminance", "28, 24, 18") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.planetRadius", "6000") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.atmosphereThickness", "90") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.rayleighScattering", "0.007, 0.011, 0.018") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieScattering", "0.015, 0.012, 0.008") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.mieAnisotropy", "0.6") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneAbsorption", "0.0005, 0.001, 0.0001") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerAltitude", "20") && ok;
    ok = SetAtmosphereVar("rtx.atmosphere.ozoneLayerWidth", "10") && ok;
    return ok;
}

LUA_FUNCTION(RemixAtmosphere_ApplyPreset) {
    LUA->CheckType(1, Type::String);
    std::string preset = LUA->GetString(1);

    // Normalize to lowercase
    std::transform(preset.begin(), preset.end(), preset.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    bool ok = false;
    if (preset == "earth")          ok = ApplyPresetEarth();
    else if (preset == "mars")      ok = ApplyPresetMars();
    else if (preset == "clearsky")  ok = ApplyPresetClearSky();
    else if (preset == "hazy")      ok = ApplyPresetHazy();
    else if (preset == "alien")     ok = ApplyPresetAlien();
    else if (preset == "desert")    ok = ApplyPresetDesert();
    else {
        Warning("[RemixAtmosphere] Unknown preset '%s'. Valid: earth, mars, clearsky, hazy, alien, desert\n", LUA->GetString(1));
    }

    LUA->PushBool(ok);
    return 1;
}

// ============================================================================
// AtmosphereManager
// ============================================================================

AtmosphereManager::AtmosphereManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

AtmosphereManager::~AtmosphereManager() {
}

void AtmosphereManager::InitializeLuaBindings() {
    if (!m_lua) return;

    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    m_lua->CreateTable();

    // Sky mode
    m_lua->PushCFunction(RemixAtmosphere_SetSkyMode);
    m_lua->SetField(-2, "SetSkyMode");
    m_lua->PushCFunction(RemixAtmosphere_GetSkyMode);
    m_lua->SetField(-2, "GetSkyMode");

    // Sun
    m_lua->PushCFunction(RemixAtmosphere_SetSunElevation);
    m_lua->SetField(-2, "SetSunElevation");
    m_lua->PushCFunction(RemixAtmosphere_SetSunRotation);
    m_lua->SetField(-2, "SetSunRotation");
    m_lua->PushCFunction(RemixAtmosphere_SetSunSize);
    m_lua->SetField(-2, "SetSunSize");
    m_lua->PushCFunction(RemixAtmosphere_SetSunIntensity);
    m_lua->SetField(-2, "SetSunIntensity");
    m_lua->PushCFunction(RemixAtmosphere_SetSunDisc);
    m_lua->SetField(-2, "SetSunDisc");
    m_lua->PushCFunction(RemixAtmosphere_SetSunIlluminance);
    m_lua->SetField(-2, "SetSunIlluminance");

    // Density multipliers
    m_lua->PushCFunction(RemixAtmosphere_SetAirDensity);
    m_lua->SetField(-2, "SetAirDensity");
    m_lua->PushCFunction(RemixAtmosphere_SetDustDensity);
    m_lua->SetField(-2, "SetDustDensity");
    m_lua->PushCFunction(RemixAtmosphere_SetOzoneDensity);
    m_lua->SetField(-2, "SetOzoneDensity");

    // Planet / observer
    m_lua->PushCFunction(RemixAtmosphere_SetAltitude);
    m_lua->SetField(-2, "SetAltitude");
    m_lua->PushCFunction(RemixAtmosphere_SetPlanetRadius);
    m_lua->SetField(-2, "SetPlanetRadius");
    m_lua->PushCFunction(RemixAtmosphere_SetAtmosphereThickness);
    m_lua->SetField(-2, "SetAtmosphereThickness");

    // Advanced scattering
    m_lua->PushCFunction(RemixAtmosphere_SetMieAnisotropy);
    m_lua->SetField(-2, "SetMieAnisotropy");
    m_lua->PushCFunction(RemixAtmosphere_SetRayleighScattering);
    m_lua->SetField(-2, "SetRayleighScattering");
    m_lua->PushCFunction(RemixAtmosphere_SetMieScattering);
    m_lua->SetField(-2, "SetMieScattering");
    m_lua->PushCFunction(RemixAtmosphere_SetOzoneAbsorption);
    m_lua->SetField(-2, "SetOzoneAbsorption");
    m_lua->PushCFunction(RemixAtmosphere_SetOzoneLayer);
    m_lua->SetField(-2, "SetOzoneLayer");

    // Presets
    m_lua->PushCFunction(RemixAtmosphere_ApplyPreset);
    m_lua->SetField(-2, "ApplyPreset");

    // Constants
    m_lua->PushNumber(0);
    m_lua->SetField(-2, "MODE_SKYBOX");
    m_lua->PushNumber(1);
    m_lua->SetField(-2, "MODE_PHYSICAL");

    m_lua->SetField(-2, "RemixAtmosphere");
    m_lua->Pop();

    Msg("[AtmosphereManager] Lua bindings initialized\n");
}

} // namespace RemixAPI

#endif // _WIN64
