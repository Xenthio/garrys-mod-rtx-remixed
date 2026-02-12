#pragma once
#ifdef _WIN64

#include <Windows.h>
#include <vector>
#include <string>
#include <functional>

struct PatchEntry {
	std::string name;           // Human-readable name
	std::string moduleName;     // DLL to patch (e.g. "engine.dll")
	std::string signature;      // Signature to find (ScanSign format)
	size_t sigLen;              // Length of signature string
	int offset;                 // Offset from found address to patch
	std::vector<uint8_t> patchBytes;    // Bytes to write
	std::vector<uint8_t> originalBytes; // Saved original bytes
	void* patchAddress;         // Resolved address
	bool applied;               // Whether patch is currently active
};

class PatchManager {
public:
	static PatchManager& Instance();

	// Register a patch definition
	void RegisterPatch(
		const std::string& name,
		const std::string& moduleName,
		const char* signature,
		size_t sigLen,
		int offset,
		const std::vector<uint8_t>& patchBytes
	);

	// Resolve all patch addresses (call after DLLs are loaded)
	void ResolveAll();

	// Apply/restore individual patches by name
	bool ApplyPatch(const std::string& name);
	bool RestorePatch(const std::string& name);

	// Apply/restore all registered patches
	void ApplyAll();
	void RestoreAll();

	// Check if a patch is currently applied
	bool IsApplied(const std::string& name) const;

	// Toggle a patch on/off
	void SetPatchEnabled(const std::string& name, bool enabled);

private:
	PatchManager() = default;
	PatchManager(const PatchManager&) = delete;
	PatchManager& operator=(const PatchManager&) = delete;

	PatchEntry* FindPatch(const std::string& name);
	const PatchEntry* FindPatch(const std::string& name) const;

	bool WriteBytes(void* address, const uint8_t* bytes, size_t len);

	std::vector<PatchEntry> m_patches;
};

#endif // _WIN64
