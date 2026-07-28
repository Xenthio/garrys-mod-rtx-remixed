#ifdef _WIN64

#include "patch_manager.h"
#include "e_utils.h"
#include <tier0/dbg.h>

namespace {

bool IsRangeInModule(const DynLibInfo& module, const void* address, size_t length) {
	const uintptr_t moduleBegin = reinterpret_cast<uintptr_t>(module.baseAddress);
	const uintptr_t moduleEnd = moduleBegin + module.memorySize;
	const uintptr_t rangeBegin = reinterpret_cast<uintptr_t>(address);

	return rangeBegin >= moduleBegin
		&& rangeBegin <= moduleEnd
		&& length <= moduleEnd - rangeBegin;
}

bool BytesMatch(const void* address, const std::vector<uint8_t>& expectedBytes) {
	return expectedBytes.empty()
		|| memcmp(address, expectedBytes.data(), expectedBytes.size()) == 0;
}

void* FindValidatedPatchAddress(
	const DynLibInfo& module,
	void* signatureAddress,
	const PatchLocator& locator,
	size_t patchLength)
{
	const int radius = locator.offsetSearchRadius > 0 ? locator.offsetSearchRadius : 0;
	std::vector<void*> validAddresses;

	for (int candidateOffset = locator.offset - radius;
		candidateOffset <= locator.offset + radius;
		++candidateOffset)
	{
		const intptr_t candidateValue =
			reinterpret_cast<intptr_t>(signatureAddress) + candidateOffset;
		void* candidateAddress = reinterpret_cast<void*>(candidateValue);

		if (!IsRangeInModule(module, candidateAddress, patchLength)
			|| !IsRangeInModule(module, candidateAddress, locator.expectedBytes.size())
			|| !BytesMatch(candidateAddress, locator.expectedBytes))
		{
			continue;
		}

		validAddresses.push_back(candidateAddress);
	}

	if (validAddresses.size() != 1) {
		return nullptr;
	}

	return validAddresses.front();
}

} // namespace

PatchManager& PatchManager::Instance() {
	static PatchManager instance;
	return instance;
}

void PatchManager::RegisterPatch(
	const std::string& name,
	const std::string& moduleName,
	const char* signature,
	size_t sigLen,
	int offset,
	const std::vector<uint8_t>& patchBytes)
{
	PatchLocator locator;
	locator.description = "primary";
	locator.signature = std::string(signature, sigLen);
	locator.offset = offset;
	locator.offsetSearchRadius = 0;

	PatchEntry entry;
	entry.name = name;
	entry.moduleName = moduleName;
	entry.locators.push_back(std::move(locator));
	entry.patchBytes = patchBytes;
	entry.patchAddress = nullptr;
	entry.resolvedLocator = static_cast<size_t>(-1);
	entry.requireUniqueSignature = false;
	entry.applied = false;
	m_patches.push_back(std::move(entry));
}

void PatchManager::RegisterPatchWithFallbacks(
	const std::string& name,
	const std::string& moduleName,
	const std::vector<PatchLocator>& locators,
	const std::vector<uint8_t>& patchBytes)
{
	PatchEntry entry;
	entry.name = name;
	entry.moduleName = moduleName;
	entry.locators = locators;
	entry.patchBytes = patchBytes;
	entry.patchAddress = nullptr;
	entry.resolvedLocator = static_cast<size_t>(-1);
	entry.requireUniqueSignature = true;
	entry.applied = false;
	m_patches.push_back(std::move(entry));
}

void PatchManager::ResolveAll() {
	for (auto& patch : m_patches) {
		HMODULE hModule = GetModuleHandleA(patch.moduleName.c_str());
		if (!hModule) {
			Warning("[PatchManager] Module not loaded: %s (patch: %s)\n",
				patch.moduleName.c_str(), patch.name.c_str());
			continue;
		}

		DynLibInfo moduleInfo{};
		if (!GetLibraryInfo(hModule, moduleInfo)) {
			Warning("[PatchManager] Failed to inspect module: %s (patch: %s)\n",
				patch.moduleName.c_str(), patch.name.c_str());
			continue;
		}

		for (size_t locatorIndex = 0;
			locatorIndex < patch.locators.size();
			++locatorIndex)
		{
			const PatchLocator& locator = patch.locators[locatorIndex];
			void* found = ScanSign(
				hModule,
				locator.signature.c_str(),
				locator.signature.size());
			if (!found) {
				continue;
			}

			if (patch.requireUniqueSignature) {
				void* duplicate = ScanSign(
					hModule,
					locator.signature.c_str(),
					locator.signature.size(),
					static_cast<uint8_t*>(found) + 1);
				if (duplicate) {
					Warning(
						"[PatchManager] Locator '%s' is ambiguous for patch '%s' in %s\n",
						locator.description.c_str(),
						patch.name.c_str(),
						patch.moduleName.c_str());
					continue;
				}
			}

			void* patchAddress = FindValidatedPatchAddress(
				moduleInfo,
				found,
				locator,
				patch.patchBytes.size());
			if (!patchAddress) {
				Warning(
					"[PatchManager] Locator '%s' matched but target validation failed for patch '%s'\n",
					locator.description.c_str(),
					patch.name.c_str());
				continue;
			}

			patch.patchAddress = patchAddress;
			patch.resolvedLocator = locatorIndex;

			// Save original bytes only after the address has passed validation.
			patch.originalBytes.resize(patch.patchBytes.size());
			memcpy(
				patch.originalBytes.data(),
				patch.patchAddress,
				patch.patchBytes.size());

			Msg(
				"[PatchManager] Resolved patch '%s' at %p in %s using locator '%s'\n",
				patch.name.c_str(),
				patch.patchAddress,
				patch.moduleName.c_str(),
				locator.description.c_str());
			break;
		}

		if (!patch.patchAddress) {
			Warning(
				"[PatchManager] No compatible signature found for patch: %s in %s\n",
				patch.name.c_str(),
				patch.moduleName.c_str());
		}
	}
}

bool PatchManager::ApplyPatch(const std::string& name) {
	PatchEntry* patch = FindPatch(name);
	if (!patch) {
		Warning("[PatchManager] Failed to apply patch: '%s' not found\n", name.c_str());
		return false;
	}
	if (!patch->patchAddress) {
		Warning("[PatchManager] Failed to apply patch: '%s' address not resolved\n", name.c_str());
		return false;
	}
	if (patch->applied) {
		Warning("[PatchManager] Failed to apply patch: '%s' already applied\n", name.c_str());
		return false;
	}
	const bool hasResolvedLocator =
		patch->resolvedLocator < patch->locators.size();
	const bool expectedTargetChanged =
		hasResolvedLocator
		&& !BytesMatch(
			patch->patchAddress,
			patch->locators[patch->resolvedLocator].expectedBytes);
	if (patch->originalBytes.size() != patch->patchBytes.size()
		|| !BytesMatch(patch->patchAddress, patch->originalBytes)
		|| expectedTargetChanged)
	{
		Warning(
			"[PatchManager] Failed to apply patch: '%s' target bytes changed after resolution\n",
			name.c_str());
		return false;
	}

	if (!WriteBytes(patch->patchAddress, patch->patchBytes.data(), patch->patchBytes.size())) {
		Warning("[PatchManager] Failed to apply patch: '%s' WriteBytes failed\n", name.c_str());
		return false;
	}

	patch->applied = true;
	Msg("[PatchManager] Applied patch: %s\n", name.c_str());
	return true;
}

bool PatchManager::RestorePatch(const std::string& name) {
	PatchEntry* patch = FindPatch(name);
	if (!patch) {
		Warning("[PatchManager] Failed to restore patch: '%s' not found\n", name.c_str());
		return false;
	}
	if (!patch->patchAddress) {
		Warning("[PatchManager] Failed to restore patch: '%s' address not resolved\n", name.c_str());
		return false;
	}
	if (!patch->applied) {
		Warning("[PatchManager] Failed to restore patch: '%s' not currently applied\n", name.c_str());
		return false;
	}

	if (patch->originalBytes.empty()) {
		Warning("[PatchManager] Failed to restore patch: '%s' no original bytes saved\n", name.c_str());
		return false;
	}
	if (!BytesMatch(patch->patchAddress, patch->patchBytes)) {
		Warning(
			"[PatchManager] Failed to restore patch: '%s' patched bytes were changed externally\n",
			name.c_str());
		return false;
	}

	if (!WriteBytes(patch->patchAddress, patch->originalBytes.data(), patch->originalBytes.size())) {
		Warning("[PatchManager] Failed to restore patch: '%s' WriteBytes failed\n", name.c_str());
		return false;
	}

	patch->applied = false;
	Msg("[PatchManager] Restored patch: %s\n", name.c_str());
	return true;
}

void PatchManager::ApplyAll() {
	for (auto& patch : m_patches) {
		if (patch.patchAddress && !patch.applied) {
			ApplyPatch(patch.name);
		}
	}
}

void PatchManager::RestoreAll() {
	for (auto& patch : m_patches) {
		if (patch.patchAddress && patch.applied) {
			RestorePatch(patch.name);
		}
	}
}

bool PatchManager::IsApplied(const std::string& name) const {
	const PatchEntry* patch = FindPatch(name);
	return patch && patch->applied;
}

void PatchManager::SetPatchEnabled(const std::string& name, bool enabled) {
	if (enabled)
		ApplyPatch(name);
	else
		RestorePatch(name);
}

PatchEntry* PatchManager::FindPatch(const std::string& name) {
	for (auto& patch : m_patches) {
		if (patch.name == name)
			return &patch;
	}
	return nullptr;
}

const PatchEntry* PatchManager::FindPatch(const std::string& name) const {
	for (const auto& patch : m_patches) {
		if (patch.name == name)
			return &patch;
	}
	return nullptr;
}

bool PatchManager::WriteBytes(void* address, const uint8_t* bytes, size_t len) {
	DWORD oldProtect;
	if (!VirtualProtect(address, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		Warning("[PatchManager] VirtualProtect failed at %p (error: %lu)\n", address, GetLastError());
		return false;
	}

	memcpy(address, bytes, len);
	FlushInstructionCache(GetCurrentProcess(), address, len);

	DWORD dummy;
	VirtualProtect(address, len, oldProtect, &dummy);
	return true;
}

#endif // _WIN64
