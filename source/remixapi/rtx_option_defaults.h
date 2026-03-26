#pragma once

#include <unordered_map>
#include <string>

namespace RemixAPI {

// Some RTX Option defaults extracted from public/include/remix/rtx_options.h
// This table contains the default values from each RTX_OPTION declaration
inline const std::unordered_map<std::string, std::string> RTX_OPTION_DEFAULTS = {
    // Core RTX settings
    {"rtx.showRaytracingOption", "True"},
    {"rtx.enableRaytracing", "True"},
    {"rtx.timeDeltaBetweenFrames", "0.0"},
    {"rtx.keepTexturesForTagging", "False"},
    {"rtx.skipDrawCallsPostRTXInjection", "False"},
    {"rtx.dlssPreset", "1"}, // On
    {"rtx.nisPreset", "1"}, // Balanced  
    {"rtx.taauPreset", "1"}, // Balanced
    {"rtx.xessProfile", "2"}, // Balanced
    {"rtx.xessNetworkModel", "0"}, // KPSS
    {"rtx.xessAutoExposureMode", "2"}, // UseXeSS
    {"rtx.xessJitterScale", "1.0"},
    {"rtx.xessUseOptimizedJitter", "True"},
    {"rtx.xessAutoExposureJitterDamping", "0.85"},
    {"rtx.xessAutoExposureTemporalOptimization", "True"},
    {"rtx.xessUseJitteredMotionVectors", "False"},
    {"rtx.xessForceInvertedDepth", "False"},
    {"rtx.xessForceLDRInput", "False"},
    {"rtx.xessForceHighResMotionVectors", "False"},
    {"rtx.xessEnableMotionVectorDebug", "False"},
    {"rtx.graphicsPreset", "5"}, // Auto
    {"rtx.raytraceModePreset", "1"}, // Auto
    {"rtx.emissiveIntensity", "1.0"},
    {"rtx.fireflyFilteringLuminanceThreshold", "1000.0"},
    {"rtx.secondarySpecularFireflyFilteringThreshold", "1000.0"},
    {"rtx.vertexColorStrength", "0.6"},
    {"rtx.vertexColorIsBakedLighting", "True"},
    {"rtx.ignoreAllVertexColorBakedLighting", "False"},
    {"rtx.allowFSE", "False"},
    {"rtx.baseGameModRegex", ""},
    {"rtx.baseGameModPathRegex", ""},

    // Shader compilation
    {"rtx.shader.asyncSpirVRecompilation", "True"},
    {"rtx.shader.recompileOnLaunch", "False"},
    {"rtx.shader.useLiveEditMode", "False"},
    {"rtx.shader.prewarmAllVariants", "False"},
    {"rtx.shader.enableAsyncCompilation", "True"},
    {"rtx.shader.enableAsyncCompilationUI", "True"},
    {"rtx.shader.asyncCompilationThrottleMilliseconds", "33"},

    // Raytraced render target
    {"rtx.raytracedRenderTarget.enable", "True"},

    // View model
    {"rtx.viewModel.enable", "False"},
    {"rtx.viewModel.rangeMeters", "1.0"},
    {"rtx.viewModel.scale", "1.0"},
    {"rtx.viewModel.enableVirtualInstances", "True"},
    {"rtx.viewModel.perspectiveCorrection", "True"},
    {"rtx.viewModel.maxZThreshold", "0.0"},

    // Player model
    {"rtx.playerModel.enableVirtualInstances", "True"},
    {"rtx.playerModel.enableInPrimarySpace", "False"},
    {"rtx.playerModel.enablePrimaryShadows", "True"},
    {"rtx.playerModel.backwardOffset", "0.0"},
    {"rtx.playerModel.horizontalDetectionDistance", "34.0"},
    {"rtx.playerModel.verticalDetectionDistance", "64.0"},
    {"rtx.playerModel.eyeHeight", "64.0"},
    {"rtx.playerModel.intersectionCapsuleRadius", "24.0"},
    {"rtx.playerModel.intersectionCapsuleHeight", "68.0"},

    // Displacement
    {"rtx.displacement.mode", "1"}, // QuadtreePOM
    {"rtx.displacement.enableDirectLighting", "True"},
    {"rtx.displacement.enableIndirectLighting", "True"},
    {"rtx.displacement.enableNEECache", "True"},
    {"rtx.displacement.enableReSTIRGI", "True"},
    {"rtx.displacement.enableIndirectHit", "False"},
    {"rtx.displacement.enablePSR", "False"},
    {"rtx.displacement.displacementFactor", "1.0"},
    {"rtx.displacement.maxIterations", "64"},

    // Core rendering
    {"rtx.resolvePreCombinedMatrices", "True"},
    {"rtx.minPrimsInDynamicBLAS", "1000"},
    {"rtx.maxPrimsInMergedBLAS", "50000"},
    {"rtx.forceMergeAllMeshes", "False"},
    {"rtx.minimizeBlasMerging", "False"},
    {"rtx.enableAlwaysCalculateAABB", "False"},

    // Camera & free cam
    {"rtx.shakeCamera", "False"},
    {"rtx.cameraAnimationMode", "3"}, // CameraShake_Pitch
    {"rtx.cameraShakePeriod", "20"},
    {"rtx.cameraAnimationAmplitude", "2.0"},
    {"rtx.skipObjectsWithUnknownCamera", "False"},
    {"rtx.enableNearPlaneOverride", "False"},
    {"rtx.nearPlaneOverride", "0.1"},
    {"rtx.useRayPortalVirtualInstanceMatching", "True"},
    {"rtx.enablePortalFadeInEffect", "False"},

    // Lighting & integration
    {"rtx.useRTXDI", "True"},
    {"rtx.integrateIndirectMode", "2"}, // NeuralRadianceCache
    {"rtx.upscalerType", "1"}, // DLSS
    {"rtx.enableRayReconstruction", "True"},
    {"rtx.lowMemoryGpu", "False"},
    {"rtx.resolutionScale", "0.75"},
    {"rtx.forceCameraJitter", "False"},
    {"rtx.cameraJitterSequenceLength", "64"},
    {"rtx.enableDirectLighting", "True"},
    {"rtx.enableSecondaryBounces", "True"},
    {"rtx.zUp", "False"},
    {"rtx.leftHandedCoordinateSystem", "False"},
    {"rtx.uniqueObjectDistance", "300.0"},

    // UI
    {"rtx.showUI", "0"}, // None
    {"rtx.defaultToAdvancedUI", "False"},
    {"rtx.showRayReconstructionUI", "True"},
    {"rtx.showUICursor", "True"},
    {"rtx.blockInputToGameInUI", "True"},
    {"rtx.qualityDLSS", "2"}, // Auto

    // Render passes
    {"rtx.renderPassGBufferRaytraceMode", "0"}, // RayQuery
    {"rtx.renderPassIntegrateDirectRaytraceMode", "0"}, // RayQuery  
    {"rtx.renderPassIntegrateIndirectRaytraceMode", "1"}, // TraceRay
    {"rtx.captureDebugImage", "False"},

    // Denoising
    {"rtx.useDenoiser", "True"},
    {"rtx.useDenoiserReferenceMode", "False"},
    {"rtx.accumulation.numberOfFramesToAccumulate", "1024"},
    {"rtx.accumulation.blendMode", "0"}, // Average
    {"rtx.accumulation.resetOnCameraTransformChange", "True"},
    {"rtx.denoiseDirectAndIndirectLightingSeparately", "True"},
    {"rtx.replaceDirectSpecularHitTWithIndirectSpecularHitT", "True"},
    {"rtx.adaptiveResolutionDenoising", "True"},
    {"rtx.adaptiveAccumulation", "True"},
    {"rtx.numFramesToKeepInstances", "1"},
    {"rtx.numFramesToKeepBLAS", "1"},
    {"rtx.numFramesToKeepLights", "100"},
    {"rtx.sceneScale", "1.0"},

    // Anti-culling
    {"rtx.antiCulling.object.enable", "False"},
    {"rtx.antiCulling.object.enableHighPrecisionAntiCulling", "True"},
    {"rtx.antiCulling.object.enableInfinityFarFrustum", "False"},
    {"rtx.antiCulling.object.hashInstanceWithBoundingBoxHash", "True"},
    {"rtx.antiCulling.object.numObjectsToKeep", "10000"},
    {"rtx.antiCulling.object.fovScale", "1.0"},
    {"rtx.antiCulling.object.farPlaneScale", "10.0"},
    {"rtx.antiCulling.light.enable", "False"},
    {"rtx.antiCulling.light.numLightsToKeep", "1000"},
    {"rtx.antiCulling.light.numFramesToExtendLightLifetime", "1000"},
    {"rtx.antiCulling.light.fovScale", "1.0"},

    // Resolve options
    {"rtx.primaryRayMaxInteractions", "32"},
    {"rtx.psrRayMaxInteractions", "32"},
    {"rtx.secondaryRayMaxInteractions", "8"},
    {"rtx.enableSeparateUnorderedApproximations", "True"},
    {"rtx.trackParticleObjects", "True"},
    {"rtx.enableDirectTranslucentShadows", "False"},
    {"rtx.enableDirectAlphaBlendShadows", "True"},
    {"rtx.enableIndirectTranslucentShadows", "False"},
    {"rtx.enableIndirectAlphaBlendShadows", "True"},
    {"rtx.resolveTransparencyThreshold", "0.003921569"}, // 1.0f/255.0f
    {"rtx.resolveOpaquenessThreshold", "0.996078431"}, // 254.0f/255.0f

    // PSR options
    {"rtx.enablePSRR", "True"},
    {"rtx.enablePSTR", "True"},
    {"rtx.psrrMaxBounces", "10"},
    {"rtx.pstrMaxBounces", "10"},
    {"rtx.enablePSTROutgoingSplitApproximation", "True"},
    {"rtx.enablePSTRSecondaryIncidentSplitApproximation", "True"},
    {"rtx.psrrNormalDetailThreshold", "0.0"},
    {"rtx.pstrNormalDetailThreshold", "0.0"},

    // Shader execution reordering
    {"rtx.isShaderExecutionReorderingSupported", "True"},
    {"rtx.enableShaderExecutionReorderingInPathtracerGbuffer", "False"},
    {"rtx.enableShaderExecutionReorderingInPathtracerIntegrateIndirect", "True"},

    // Path tracing
    {"rtx.enableRussianRoulette", "True"},
    {"rtx.russianRouletteMode", "0"}, // ThroughputBased
    {"rtx.russianRouletteDiffuseContinueProbability", "0.1"},
    {"rtx.russianRouletteSpecularContinueProbability", "0.98"},
    {"rtx.russianRouletteDistanceFactor", "0.1"},
    {"rtx.russianRouletteMaxContinueProbability", "0.9"},
    {"rtx.russianRoulette1stBounceMinContinueProbability", "0.6"},
    {"rtx.russianRoulette1stBounceMaxContinueProbability", "1.0"},
    {"rtx.pathMinBounces", "1"},
    {"rtx.pathMaxBounces", "4"},
    {"rtx.opaqueDiffuseLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minOpaqueDiffuseLobeSamplingProbability", "0.25"},
    {"rtx.opaqueSpecularLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minOpaqueSpecularLobeSamplingProbability", "0.25"},
    {"rtx.opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minOpaqueOpacityTransmissionLobeSamplingProbability", "0.25"},
    {"rtx.opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minOpaqueDiffuseTransmissionLobeSamplingProbability", "0.25"},
    {"rtx.translucentSpecularLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minTranslucentSpecularLobeSamplingProbability", "0.3"},
    {"rtx.translucentTransmissionLobeSamplingProbabilityZeroThreshold", "0.01"},
    {"rtx.minTranslucentTransmissionLobeSamplingProbability", "0.25"},
    {"rtx.indirectRaySpreadAngleFactor", "0.05"},
    {"rtx.rngSeedWithFrameIndex", "True"},
    {"rtx.enableFirstBounceLobeProbabilityDithering", "True"},
    {"rtx.enableUnorderedResolveInIndirectRays", "True"},
    {"rtx.enableProbabilisticUnorderedResolveInIndirectRays", "True"},
    {"rtx.enableUnorderedEmissiveParticlesInIndirectRays", "False"},
    {"rtx.enableTransmissionApproximationInIndirectRays", "False"},
    {"rtx.enableDecalMaterialBlending", "True"},
    {"rtx.enableBillboardOrientationCorrection", "True"},
    {"rtx.useIntersectionBillboardsOnPrimaryRays", "False"},
    {"rtx.translucentDecalAlbedoFactor", "10.0"},
    {"rtx.worldSpaceUiBackgroundOffset", "-0.01"},

    // Light sampling
    {"rtx.risLightSampleCount", "7"},

    // Subsurface scattering
    {"rtx.subsurface.enableThinOpaque", "True"},
    {"rtx.subsurface.enableTextureMaps", "True"},
    {"rtx.subsurface.surfaceThicknessScale", "1.0"},
    {"rtx.subsurface.enableDiffusionProfile", "True"},
    {"rtx.subsurface.diffusionProfileScale", "1.0"},
    {"rtx.subsurface.enableTransmission", "True"},
    {"rtx.subsurface.enableTransmissionSingleScattering", "True"},
    {"rtx.subsurface.enableTransmissionDiffusionProfileCorrection", "False"},
    {"rtx.subsurface.transmissionBsdfSampleCount", "1"},
    {"rtx.subsurface.transmissionSingleScatteringSampleCount", "1"},
    {"rtx.subsurface.diffusionProfileDebugPixelPosition", "2147483647, 2147483647"}, // INT32_MAX

    // Alpha test/blend
    {"rtx.enableAlphaBlend", "True"},
    {"rtx.enableAlphaTest", "True"},
    {"rtx.enableCulling", "True"},
    {"rtx.enableCullingInSecondaryRays", "False"},
    {"rtx.enableEmissiveBlendModeTranslation", "True"},
    {"rtx.enableEmissiveBlendEmissiveOverride", "True"},
    {"rtx.emissiveBlendOverrideEmissiveIntensity", "0.2"},
    {"rtx.particleSoftnessFactor", "0.05"},
    {"rtx.forceCutoutAlpha", "0.5"},

    // Ray portals
    {"rtx.rayPortalSamplingWeightMinDistance", "10.0"},
    {"rtx.rayPortalSamplingWeightMaxDistance", "1000.0"},
    {"rtx.rayPortalCameraHistoryCorrection", "False"},
    {"rtx.rayPortalCameraInBetweenPortalsCorrection", "False"},
    {"rtx.rayPortalCameraInBetweenPortalsCorrectionThreshold", "0.1"},

    // Materials & textures
    {"rtx.useWhiteMaterialMode", "False"},
    {"rtx.useHighlightLegacyMode", "False"},
    {"rtx.useHighlightUnsafeAnchorMode", "False"},
    {"rtx.useHighlightUnsafeReplacementMode", "False"},
    {"rtx.nativeMipBias", "0.0"},
    {"rtx.upscalingMipBias", "0.0"},
    {"rtx.useAnisotropicFiltering", "True"},
    {"rtx.maxAnisotropySamples", "8.0"},
    {"rtx.enableMultiStageTextureFactorBlending", "True"},

    // Developer options
    {"rtx.enableBreakIntoDebuggerOnPressingB", "False"},
    {"rtx.enableInstanceDebuggingTools", "False"},
    {"rtx.drawCallRange", "0, 2147483647"}, // 0, INT32_MAX
    {"rtx.instanceOverrideWorldOffset", "0.0, 0.0, 0.0"},
    {"rtx.instanceOverrideInstanceIdx", "4294967295"}, // UINT32_MAX
    {"rtx.instanceOverrideInstanceIdxRange", "15"},
    {"rtx.instanceOverrideSelectedInstancePrintMaterialHash", "False"},
    {"rtx.enablePresentThrottle", "False"},
    {"rtx.presentThrottleDelay", "16"},
    {"rtx.validateCPUIndexData", "False"},

    // Aliasing
    {"rtx.aliasing.beginPass", "0"}, // FrameBegin
    {"rtx.aliasing.endPass", "16"}, // FrameEnd  
    {"rtx.aliasing.width", "1280"},
    {"rtx.aliasing.height", "720"},
    {"rtx.aliasing.depth", "1"},
    {"rtx.aliasing.layer", "1"},

    // Opacity micromap
    {"rtx.opacityMicromap.enable", "True"},

    // Reflex
    {"rtx.reflexMode", "1"}, // LowLatency
    {"rtx.isReflexEnabled", "True"},
    {"rtx.enableVsync", "2"}, // WaitingForImplicitSwapchain

    // Replacement assets
    {"rtx.enableReplacementAssets", "True"},
    {"rtx.enableReplacementLights", "True"},
    {"rtx.enableReplacementMeshes", "True"},
    {"rtx.enableReplacementMaterials", "True"},
    {"rtx.enableReplacementInstancerMeshRendering", "True"},
    {"rtx.adaptiveResolutionReservedGPUMemoryGiB", "2"},
    {"rtx.limitedBonesPerVertex", "4"},

    // Texture manager
    {"rtx.texturemanager.budgetPercentageOfAvailableVram", "50"},
    {"rtx.texturemanager.fixedBudgetEnable", "False"},
    {"rtx.texturemanager.fixedBudgetMiB", "2048"},
    {"rtx.texturemanager.samplerFeedbackEnable", "True"},
    {"rtx.texturemanager.neverDowngradeTextures", "False"},
    {"rtx.texturemanager.stagingBufferSizeMiB", "96"},
    {"rtx.reloadTextureWhenResolutionChanged", "False"},
    {"rtx.alwaysWaitForAsyncTextures", "False"},
    {"rtx.initializer.asyncAssetLoading", "True"},
    {"rtx.usePartialDdsLoader", "True"},

    // Tonemapping
    {"rtx.tonemappingMode", "1"}, // Local
    {"rtx.useLegacyACES", "True"},
    {"rtx.showLegacyACESOption", "False"},

    // Capture
    {"rtx.captureShowMenuOnHotkey", "True"},
    {"rtx.captureInstances", "True"},
    {"rtx.captureNoInstance", "False"},
    {"rtx.captureTimestampReplacement", "{timestamp}"},
    {"rtx.captureInstanceStageName", "capture_{timestamp}.usd"},
    {"rtx.captureEnableMultiframe", "False"},
    {"rtx.captureMaxFrames", "1"},
    {"rtx.captureFramesPerSecond", "24"},
    {"rtx.captureMeshPositionDelta", "0.3"},
    {"rtx.captureMeshNormalDelta", "0.3"},
    {"rtx.captureMeshTexcoordDelta", "0.3"},
    {"rtx.captureMeshColorDelta", "0.3"},
    {"rtx.captureMeshBlendWeightDelta", "0.01"},

    // Miscellaneous
    {"rtx.useVirtualShadingNormalsForDenoising", "True"},
    {"rtx.resetDenoiserHistoryOnSettingsChange", "False"},
    {"rtx.fogIgnoreSky", "False"},
    {"rtx.skyBrightness", "1.0"},
    {"rtx.skyForceHDR", "False"},
    {"rtx.skyProbeSide", "1024"},
    {"rtx.skyUiDrawcallCount", "0"},
    {"rtx.skyDrawcallIdThreshold", "0"},
    {"rtx.skyMinZThreshold", "1.0"},
    {"rtx.skyAutoDetect", "0"}, // None
    {"rtx.skyAutoDetectUniqueCameraDistance", "1.0"},
    {"rtx.skyReprojectToMainCameraSpace", "False"},
    {"rtx.skyReprojectScale", "16.0"},
    {"rtx.skyReprojectCameraTimeoutFrames", "60"},
    {"rtx.skyReprojectFallbackToRaster", "True"},
    {"rtx.skyReprojectLogFallbacks", "True"},
    {"rtx.skyReprojectPreventCameraConflicts", "True"},
    {"rtx.skyReprojectCameraSignatureFrames", "10"},
    {"rtx.logLegacyHashReplacementMatches", "False"},
    {"rtx.fusedWorldViewMode", "0"}, // None
    {"rtx.useBuffersDirectly", "True"},
    {"rtx.alwaysCopyDecalGeometries", "True"},
    {"rtx.ignoreLastTextureStage", "False"},
    {"rtx.terrain.terrainAsDecalsEnabledIfNoBaker", "False"},
    {"rtx.terrain.terrainAsDecalsAllowOverModulate", "False"},
    {"rtx.userBrightness", "50"},
    {"rtx.userBrightnessEVRange", "3.0"},

    // Automation
    {"rtx.automation.disableBlockingDialogBoxes", "False"},
    {"rtx.automation.disableDisplayMemoryStatistics", "False"},
    {"rtx.automation.disableUpdateUpscaleFromDlssPreset", "False"},
    {"rtx.automation.suppressAssetLoadingErrors", "False"},

    // === VOLUMETRICS SECTION ===
    // Core volumetrics
    {"rtx.volumetrics.enable", "True"},
    {"rtx.volumetrics.enableAtmosphere", "False"},
    {"rtx.volumetrics.anisotropy", "0.5"},
    {"rtx.volumetrics.depthOffset", "0.5"},

    // Atmosphere
    {"rtx.volumetrics.atmosphereHeightMeters", "30.0"},
    {"rtx.volumetrics.atmosphereInverted", "False"},
    {"rtx.volumetrics.atmospherePlanetRadiusMeters", "10000.0"},

    // Fog remapping  
    {"rtx.volumetrics.enableFogRemap", "False"},
    {"rtx.volumetrics.fogRemapMode", "0"},
    {"rtx.volumetrics.fogRemapMaxDistanceMin", "0.0"},
    {"rtx.volumetrics.fogRemapMaxDistanceMax", "1000.0"},
    {"rtx.volumetrics.fogRemapTransmittanceColorMin", "0.0, 0.0, 0.0"},
    {"rtx.volumetrics.fogRemapTransmittanceColorMax", "1.0, 1.0, 1.0"},
    {"rtx.volumetrics.fogRemapScatteringAlbedoMin", "0.0, 0.0, 0.0"},
    {"rtx.volumetrics.fogRemapScatteringAlbedoMax", "1.0, 1.0, 1.0"},

    // Heterogeneous fog
    {"rtx.volumetrics.enableHeterogeneousFog", "False"},
    {"rtx.volumetrics.heterogeneousFogDensityScale", "1.0"},
    {"rtx.volumetrics.heterogeneousFogNoiseFrequency", "1.0"},
    {"rtx.volumetrics.heterogeneousFogNoiseTimeScale", "1.0"},
    {"rtx.volumetrics.heterogeneousFogNoiseOctaves", "3"},
    {"rtx.volumetrics.heterogeneousFogNoiseLacunarity", "2.0"},
    {"rtx.volumetrics.heterogeneousFogNoiseGain", "0.5"},
    {"rtx.volumetrics.heterogeneousFogNoiseOffset", "0.0, 0.0, 0.0"},
    {"rtx.volumetrics.heterogeneousFogNoiseInvert", "False"},

    // Single scattering
    {"rtx.volumetrics.singleScatteringAlbedo", "1.0, 1.0, 1.0"},
    {"rtx.volumetrics.singleScatteringVolumetricAnisotropy", "0.0"},
    {"rtx.volumetrics.singleScatteringExtinctionCoefficient", "1.0, 1.0, 1.0"},

    // Multi scattering
    {"rtx.volumetrics.multiScatteringAlbedo", "1.0, 1.0, 1.0"},
    {"rtx.volumetrics.multiScatteringVolumetricAnisotropy", "0.0"},
    {"rtx.volumetrics.multiScatteringExtinctionCoefficient", "1.0, 1.0, 1.0"},

    // Isotropic
    {"rtx.volumetrics.isotropicAlbedo", "1.0, 1.0, 1.0"},
    {"rtx.volumetrics.isotropicExtinctionCoefficient", "1.0, 1.0, 1.0"},

    // Transmittance
    {"rtx.volumetrics.transmittanceColor", "1.0, 1.0, 1.0"},
    {"rtx.volumetrics.transmittanceMeasurementDistance", "1000.0"},

    // Froxel grid
    {"rtx.volumetrics.froxelMaxDistanceMeters", "100.0"},
    {"rtx.volumetrics.froxelDepthSlices", "64"},
    {"rtx.volumetrics.froxelDepthSliceDistributionExponent", "2.0"},
    {"rtx.volumetrics.froxelFireflyFilteringLuminanceThreshold", "1000.0"},

    // Chromaticity
    {"rtx.volumetrics.enableChromaticity", "False"},
    {"rtx.volumetrics.chromaticityRedShift", "0.0"},
    {"rtx.volumetrics.chromaticityGreenShift", "0.0"},
    {"rtx.volumetrics.chromaticityBlueShift", "0.0"},

    // Advanced volumetrics
    {"rtx.volumetrics.enableTemporalReuse", "True"},
    {"rtx.volumetrics.enablePortalVolumes", "True"},
    {"rtx.volumetrics.enableVolumetricLighting", "True"},
    {"rtx.volumetrics.volumetricInitialRISCandidateCount", "8"},
    {"rtx.volumetrics.volumetricInitialRayTMax", "1000.0"},

    // Auto exposure
    {"rtx.autoExposure.enabled", "True"},
    {"rtx.autoExposure.evMinValue", "-2.0"},
    {"rtx.autoExposure.evMaxValue", "4.0"},

    // Tonemapping 
    {"rtx.tonemap.exposureBias", "0.0"},
    {"rtx.tonemap.dynamicRange", "15.0"},

    // Post-processing
    {"rtx.bloom.enable", "True"},
    {"rtx.bloom.burnIntensity", "1.0"},
    {"rtx.postfx.enable", "True"},
    {"rtx.enableFog", "False"},

    {"rtx.skyMode", "0"},
    {"rtx.atmosphere.sunDisc", "True"},
    {"rtx.atmosphere.sunSize", "0.545"},
    {"rtx.atmosphere.sunIntensity", "1.0"},
    {"rtx.atmosphere.sunElevation", "15.0"},
    {"rtx.atmosphere.sunRotation", "0.0"},
    {"rtx.atmosphere.altitude", "100.0"},
    {"rtx.atmosphere.airDensity", "1.0"},
    {"rtx.atmosphere.aerosolDensity", "1.0"},
    {"rtx.atmosphere.ozoneDensity", "1.0"},
    {"rtx.atmosphere.planetRadius", "6371.0"},
    {"rtx.atmosphere.atmosphereThickness", "100.0"},
    {"rtx.atmosphere.mieAnisotropy", "0.97"},
    {"rtx.atmosphere.rayleighScattering", "0.0058, 0.0135, 0.0331"},
    {"rtx.atmosphere.mieScattering", "0.003996, 0.003996, 0.003996"},
    {"rtx.atmosphere.ozoneAbsorption", "0.00204, 0.00497, 0.000214"},
    {"rtx.atmosphere.ozoneLayerAltitude", "25.0"},
    {"rtx.atmosphere.ozoneLayerWidth", "15.0"},
    {"rtx.atmosphere.sunIlluminance", "20.0, 20.0, 20.0"}
};

} // namespace RemixAPI 