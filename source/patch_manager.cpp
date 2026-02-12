#ifdef _WIN64

#include "patch_manager.h"
#include "e_utils.h"
#include <tier0/dbg.h>

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
	PatchEntry entry;
	entry.name = name;
	entry.moduleName = moduleName;
	entry.signature = std::string(signature, sigLen);
	entry.sigLen = sigLen;
	entry.offset = offset;
	entry.patchBytes = patchBytes;
	entry.patchAddress = nullptr;
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

		void* found = ScanSign(hModule, patch.signature.c_str(), patch.sigLen);
		if (!found) {
			Warning("[PatchManager] Signature not found for patch: %s in %s\n",
				patch.name.c_str(), patch.moduleName.c_str());
			continue;
		}

		patch.patchAddress = static_cast<uint8_t*>(found) + patch.offset;

		// Save original bytes
		patch.originalBytes.resize(patch.patchBytes.size());
		memcpy(patch.originalBytes.data(), patch.patchAddress, patch.patchBytes.size());

		Msg("[PatchManager] Resolved patch '%s' at %p (module: %s)\n",
			patch.name.c_str(), patch.patchAddress, patch.moduleName.c_str());
	}
}

bool PatchManager::ApplyPatch(const std::string& name) {
	PatchEntry* patch = FindPatch(name);
	if (!patch || !patch->patchAddress || patch->applied)
		return false;

	if (!WriteBytes(patch->patchAddress, patch->patchBytes.data(), patch->patchBytes.size()))
		return false;

	patch->applied = true;
	Msg("[PatchManager] Applied patch: %s\n", name.c_str());
	return true;
}

bool PatchManager::RestorePatch(const std::string& name) {
	PatchEntry* patch = FindPatch(name);
	if (!patch || !patch->patchAddress || !patch->applied)
		return false;

	if (patch->originalBytes.empty())
		return false;

	if (!WriteBytes(patch->patchAddress, patch->originalBytes.data(), patch->originalBytes.size()))
		return false;

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
