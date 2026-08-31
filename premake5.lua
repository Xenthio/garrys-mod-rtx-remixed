PROJECT_GENERATOR_VERSION = 3 -- 3 is needed for 64 bit support

newoption({
	trigger = "gmcommon",
	description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
	value = "../garrysmod_common"
})

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon)

CreateWorkspace({name = "RTXFixesBinary", abi_compatible = false, path = ""})
	--CreateProject({serverside = true, source_path = "source", manual_files = false})
	--	IncludeLuaShared()
	--	IncludeScanning()
	--	IncludeDetouring()
	--	IncludeSDKCommon()
	--	IncludeSDKTier0()
	--	IncludeSDKTier1()

	CreateProject({serverside = false, source_path = "source" })
		IncludeLuaShared()
		IncludeScanning()
		IncludeDetouring()
		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		IncludeSDKTier2()
		IncludeSDKTier3()
		IncludeSDKMathlib()
		IncludeHelpersExtended()

		includedirs {
			"public/include",
		} 

		
		files {
			"source/remixapi/*.cpp",
			"source/remixapi/*.h",
			"source/remixapi/rtxlights/*.cpp",
			"source/remixapi/rtxlights/*.h",
			"source/material_pipeline/*.cpp",
			"source/material_pipeline/*.h",
			"source/material_pipeline/shader_fixes/*.cpp",
			"source/material_pipeline/shader_fixes/*.h",
			"source/material_pipeline/hash_collision_fixer/*.cpp",
			"source/material_pipeline/hash_collision_fixer/*.h",
			"source/material_pipeline/auto_categorisation/*.cpp",
			"source/material_pipeline/auto_categorisation/*.h",
			"source/material_pipeline/to_pbr/*.cpp",
			"source/material_pipeline/to_pbr/*.h",
		} 

		filter({"system:windows", "platforms:x86_64"})
			includedirs("source/advmat_rtx_bridge/include")
			files {
				"source/advmat_rtx_bridge/src/bridge.cpp",
				"source/advmat_rtx_bridge/src/filesystem_utils.cpp",
				"source/advmat_rtx_bridge/src/internal.h",
				"source/advmat_rtx_bridge/src/lua_bindings.cpp",
				"source/advmat_rtx_bridge/src/texture_ingest.cpp",
				"source/advmat_rtx_bridge/src/usda_writer.cpp",
				"source/advmat_rtx_bridge/include/advmat_rtx_bridge/*.h",
			}
			links({"windowscodecs", "ole32"})
			exceptionhandling("On")

		filter("system:windows")
			files({"source/win32/*.cpp", "source/win32/*.hpp"})

		filter("system:linux or macosx")
			files({"source/posix/*.cpp", "source/posix/*.hpp"})
