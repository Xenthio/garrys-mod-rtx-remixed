#ifdef _WIN64

#include "patch_manager.h"
#include "e_utils.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

constexpr size_t kMaxPatchBytes = 64;
constexpr size_t kMaxExpectedBytes = 128;
constexpr size_t kMaxSignatureBytes = 512;
constexpr int kMaxOffsetSearchRadius = 64;
constexpr int kMaxPatchOffsetMagnitude = 4096;

struct SignatureByte {
	uint8_t value;
	bool wildcard;
};

struct MemoryRange {
	uint8_t* begin;
	size_t length;
};

bool IsRangeInModule(const DynLibInfo& module, const void* address, size_t length) {
	const uintptr_t moduleBegin = reinterpret_cast<uintptr_t>(module.baseAddress);
	const uintptr_t rangeBegin = reinterpret_cast<uintptr_t>(address);

	if (!module.baseAddress
		|| module.memorySize == 0
		|| moduleBegin > UINTPTR_MAX - module.memorySize)
	{
		return false;
	}

	const uintptr_t moduleEnd = moduleBegin + module.memorySize;
	return rangeBegin >= moduleBegin
		&& rangeBegin < moduleEnd
		&& length <= moduleEnd - rangeBegin;
}

bool BytesMatch(const void* address, const std::vector<uint8_t>& expectedBytes) {
	return address
		&& !expectedBytes.empty()
		&& memcmp(address, expectedBytes.data(), expectedBytes.size()) == 0;
}

bool ParseSignature(
	const std::string& signature,
	std::vector<SignatureByte>& parsed)
{
	parsed.clear();
	std::istringstream stream(signature);
	std::string token;

	while (stream >> token) {
		if (token == "?" || token == "??") {
			parsed.push_back({0, true});
			continue;
		}

		if (token.size() != 2
			|| !std::isxdigit(static_cast<unsigned char>(token[0]))
			|| !std::isxdigit(static_cast<unsigned char>(token[1])))
		{
			return false;
		}

		const unsigned long value = std::strtoul(token.c_str(), nullptr, 16);
		if (value > 0xFF) {
			return false;
		}
		parsed.push_back({static_cast<uint8_t>(value), false});
	}

	return !parsed.empty() && parsed.size() <= kMaxSignatureBytes;
}

bool GetExecutableRanges(
	const DynLibInfo& module,
	std::vector<MemoryRange>& ranges)
{
	ranges.clear();
	if (!IsRangeInModule(module, module.baseAddress, sizeof(IMAGE_DOS_HEADER))) {
		return false;
	}

	auto* const moduleBase = static_cast<uint8_t*>(module.baseAddress);
	auto* const dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew < 0) {
		return false;
	}

	const uintptr_t ntAddress =
		reinterpret_cast<uintptr_t>(moduleBase)
		+ static_cast<uintptr_t>(dosHeader->e_lfanew);
	auto* const ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(ntAddress);
	if (!IsRangeInModule(module, ntHeaders, sizeof(*ntHeaders))
		|| ntHeaders->Signature != IMAGE_NT_SIGNATURE
		|| ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
		|| ntHeaders->FileHeader.NumberOfSections == 0
		|| ntHeaders->FileHeader.NumberOfSections > 96)
	{
		return false;
	}

	auto* const firstSection = IMAGE_FIRST_SECTION(ntHeaders);
	const size_t sectionTableSize =
		static_cast<size_t>(ntHeaders->FileHeader.NumberOfSections)
		* sizeof(IMAGE_SECTION_HEADER);
	if (!IsRangeInModule(module, firstSection, sectionTableSize)) {
		return false;
	}

	for (WORD index = 0;
		index < ntHeaders->FileHeader.NumberOfSections;
		++index)
	{
		const IMAGE_SECTION_HEADER& section = firstSection[index];
		if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
			continue;
		}

		size_t sectionSize = (std::max)(
			static_cast<size_t>(section.Misc.VirtualSize),
			static_cast<size_t>(section.SizeOfRawData));
		if (section.VirtualAddress >= module.memorySize) {
			continue;
		}

		sectionSize = (std::min)(
			sectionSize,
			module.memorySize - static_cast<size_t>(section.VirtualAddress));
		if (sectionSize == 0) {
			continue;
		}

		ranges.push_back({
			moduleBase + section.VirtualAddress,
			sectionSize
		});
	}

	return !ranges.empty();
}

bool IsRangeInExecutableSection(
	const std::vector<MemoryRange>& executableRanges,
	const void* address,
	size_t length)
{
	const uintptr_t rangeBegin = reinterpret_cast<uintptr_t>(address);
	for (const MemoryRange& range : executableRanges) {
		const uintptr_t sectionBegin =
			reinterpret_cast<uintptr_t>(range.begin);
		if (sectionBegin > UINTPTR_MAX - range.length) {
			continue;
		}
		const uintptr_t sectionEnd = sectionBegin + range.length;
		if (rangeBegin >= sectionBegin
			&& rangeBegin < sectionEnd
			&& length <= sectionEnd - rangeBegin)
		{
			return true;
		}
	}
	return false;
}

std::vector<void*> FindSignatureMatches(
	const std::vector<MemoryRange>& executableRanges,
	const std::vector<SignatureByte>& signature)
{
	std::vector<void*> matches;

	for (const MemoryRange& range : executableRanges) {
		if (range.length < signature.size()) {
			continue;
		}

		const size_t lastOffset = range.length - signature.size();
		for (size_t offset = 0; offset <= lastOffset; ++offset) {
			bool matched = true;
			for (size_t index = 0; index < signature.size(); ++index) {
				if (!signature[index].wildcard
					&& range.begin[offset + index] != signature[index].value)
				{
					matched = false;
					break;
				}
			}

			if (matched) {
				matches.push_back(range.begin + offset);
			}
		}
	}

	return matches;
}

bool AddSignedOffset(
	uintptr_t address,
	int offset,
	uintptr_t& result)
{
	if (offset < 0) {
		const uintptr_t magnitude =
			static_cast<uintptr_t>(-static_cast<int64_t>(offset));
		if (address < magnitude) {
			return false;
		}
		result = address - magnitude;
		return true;
	}

	const uintptr_t magnitude = static_cast<uintptr_t>(offset);
	if (address > UINTPTR_MAX - magnitude) {
		return false;
	}
	result = address + magnitude;
	return true;
}

std::vector<void*> FindValidatedPatchAddresses(
	const DynLibInfo& module,
	const std::vector<MemoryRange>& executableRanges,
	const std::vector<void*>& signatureAddresses,
	const PatchLocator& locator,
	size_t patchLength)
{
	std::vector<void*> validAddresses;
	const int radius = locator.offsetSearchRadius;
	const size_t validationLength =
		(std::max)(patchLength, locator.expectedBytes.size());

	for (void* signatureAddress : signatureAddresses) {
		for (int candidateOffset = locator.offset - radius;
			candidateOffset <= locator.offset + radius;
			++candidateOffset)
		{
			uintptr_t candidateValue = 0;
			if (!AddSignedOffset(
				reinterpret_cast<uintptr_t>(signatureAddress),
				candidateOffset,
				candidateValue))
			{
				continue;
			}

			void* const candidateAddress =
				reinterpret_cast<void*>(candidateValue);

			if (!IsRangeInModule(module, candidateAddress, validationLength)
				|| !IsRangeInExecutableSection(
					executableRanges,
					candidateAddress,
					validationLength)
				|| !BytesMatch(candidateAddress, locator.expectedBytes))
			{
				continue;
			}

			if (std::find(
				validAddresses.begin(),
				validAddresses.end(),
				candidateAddress) == validAddresses.end())
			{
				validAddresses.push_back(candidateAddress);
			}
		}
	}

	return validAddresses;
}

bool RangesOverlap(
	const void* firstAddress,
	size_t firstLength,
	const void* secondAddress,
	size_t secondLength)
{
	const uintptr_t firstBegin =
		reinterpret_cast<uintptr_t>(firstAddress);
	const uintptr_t secondBegin =
		reinterpret_cast<uintptr_t>(secondAddress);

	if (firstBegin > UINTPTR_MAX - firstLength
		|| secondBegin > UINTPTR_MAX - secondLength)
	{
		return true;
	}

	return firstBegin < secondBegin + secondLength
		&& secondBegin < firstBegin + firstLength;
}

} // namespace

PatchManager& PatchManager::Instance() {
	static PatchManager instance;
	return instance;
}

bool PatchManager::RegisterPatchWithFallbacks(
	const std::string& name,
	const std::string& moduleName,
	const std::vector<PatchLocator>& locators,
	const std::vector<uint8_t>& patchBytes)
{
	if (name.empty()
		|| moduleName.empty()
		|| locators.empty()
		|| patchBytes.empty()
		|| patchBytes.size() > kMaxPatchBytes)
	{
		Warning(
			"[PatchManager] Rejected invalid patch registration: '%s'\n",
			name.c_str());
		return false;
	}

	if (FindPatch(name)) {
		Warning(
			"[PatchManager] Rejected duplicate patch registration: '%s'\n",
			name.c_str());
		return false;
	}

	for (const PatchLocator& locator : locators) {
		std::vector<SignatureByte> parsedSignature;
		const bool patchBytesUnchanged =
			locator.expectedBytes.size() >= patchBytes.size()
			&& std::equal(
				patchBytes.begin(),
				patchBytes.end(),
				locator.expectedBytes.begin());

		if (locator.description.empty()
			|| !ParseSignature(locator.signature, parsedSignature)
			|| locator.expectedBytes.size() < patchBytes.size()
			|| locator.expectedBytes.size() > kMaxExpectedBytes
			|| locator.offsetSearchRadius < 0
			|| locator.offsetSearchRadius > kMaxOffsetSearchRadius
			|| locator.offset < -kMaxPatchOffsetMagnitude
			|| locator.offset > kMaxPatchOffsetMagnitude
			|| patchBytesUnchanged)
		{
			Warning(
				"[PatchManager] Rejected invalid locator '%s' for patch '%s'\n",
				locator.description.c_str(),
				name.c_str());
			return false;
		}
	}

	PatchEntry entry;
	entry.name = name;
	entry.moduleName = moduleName;
	entry.locators = locators;
	entry.patchBytes = patchBytes;
	entry.patchAddress = nullptr;
	entry.resolvedModuleBase = nullptr;
	entry.resolvedLocator = static_cast<size_t>(-1);
	entry.applied = false;
	m_patches.push_back(std::move(entry));
	return true;
}

void PatchManager::ResolveAll() {
	for (auto& patch : m_patches) {
		if (patch.applied) {
			Warning(
				"[PatchManager] Refusing to re-resolve applied patch: %s\n",
				patch.name.c_str());
			continue;
		}

		patch.patchAddress = nullptr;
		patch.resolvedModuleBase = nullptr;
		patch.resolvedLocator = static_cast<size_t>(-1);
		patch.originalBytes.clear();

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

		std::vector<MemoryRange> executableRanges;
		if (!GetExecutableRanges(moduleInfo, executableRanges)) {
			Warning(
				"[PatchManager] Failed to enumerate executable sections in %s (patch: %s)\n",
				patch.moduleName.c_str(),
				patch.name.c_str());
			continue;
		}

		for (size_t locatorIndex = 0;
			locatorIndex < patch.locators.size();
			++locatorIndex)
		{
			const PatchLocator& locator = patch.locators[locatorIndex];
			std::vector<SignatureByte> parsedSignature;
			if (!ParseSignature(locator.signature, parsedSignature)) {
				Warning(
					"[PatchManager] Locator '%s' became invalid for patch '%s'\n",
					locator.description.c_str(),
					patch.name.c_str());
				continue;
			}

			const std::vector<void*> signatureMatches =
				FindSignatureMatches(executableRanges, parsedSignature);
			if (signatureMatches.empty()) {
				continue;
			}

			const std::vector<void*> validAddresses =
				FindValidatedPatchAddresses(
				moduleInfo,
				executableRanges,
				signatureMatches,
				locator,
				patch.patchBytes.size());

			if (validAddresses.empty()) {
				Warning(
					"[PatchManager] Locator '%s' matched %zu time(s), but no target passed validation for patch '%s'\n",
					locator.description.c_str(),
					signatureMatches.size(),
					patch.name.c_str());
				continue;
			}
			if (validAddresses.size() != 1) {
				Warning(
					"[PatchManager] Locator '%s' is ambiguous for patch '%s': %zu validated targets\n",
					locator.description.c_str(),
					patch.name.c_str(),
					validAddresses.size());
				continue;
			}

			void* const patchAddress = validAddresses.front();
			bool overlapsExistingPatch = false;
			for (const PatchEntry& other : m_patches) {
				if (&other == &patch
					|| !other.patchAddress
					|| other.resolvedModuleBase != moduleInfo.baseAddress)
				{
					continue;
				}

				if (RangesOverlap(
					patchAddress,
					patch.patchBytes.size(),
					other.patchAddress,
					other.patchBytes.size()))
				{
					Warning(
						"[PatchManager] Patch '%s' overlaps resolved patch '%s'; refusing target\n",
						patch.name.c_str(),
						other.name.c_str());
					overlapsExistingPatch = true;
					break;
				}
			}
			if (overlapsExistingPatch) {
				continue;
			}

			patch.patchAddress = patchAddress;
			patch.resolvedModuleBase = moduleInfo.baseAddress;
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

static bool ValidateResolvedPatchTarget(
	const PatchEntry& patch,
	size_t requiredLength)
{
	if (!patch.patchAddress || !patch.resolvedModuleBase) {
		return false;
	}

	HMODULE currentModule = GetModuleHandleA(patch.moduleName.c_str());
	if (!currentModule) {
		return false;
	}

	DynLibInfo currentModuleInfo{};
	if (!GetLibraryInfo(currentModule, currentModuleInfo)
		|| currentModuleInfo.baseAddress != patch.resolvedModuleBase
		|| !IsRangeInModule(
			currentModuleInfo,
			patch.patchAddress,
			requiredLength))
	{
		return false;
	}

	std::vector<MemoryRange> executableRanges;
	return GetExecutableRanges(currentModuleInfo, executableRanges)
		&& IsRangeInExecutableSection(
			executableRanges,
			patch.patchAddress,
			requiredLength);
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
	const size_t validationLength = hasResolvedLocator
		? (std::max)(
			patch->patchBytes.size(),
			patch->locators[patch->resolvedLocator].expectedBytes.size())
		: patch->patchBytes.size();
	const bool expectedTargetChanged =
		hasResolvedLocator
		&& !BytesMatch(
			patch->patchAddress,
			patch->locators[patch->resolvedLocator].expectedBytes);
	if (!hasResolvedLocator
		|| !ValidateResolvedPatchTarget(*patch, validationLength)
		|| patch->originalBytes.size() != patch->patchBytes.size()
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
	if (!ValidateResolvedPatchTarget(*patch, patch->patchBytes.size())
		|| !BytesMatch(patch->patchAddress, patch->patchBytes))
	{
		Warning(
			"[PatchManager] Failed to restore patch: '%s' module or patched bytes changed externally\n",
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
	if (!address || !bytes || len == 0 || len > kMaxPatchBytes) {
		Warning("[PatchManager] WriteBytes rejected invalid arguments\n");
		return false;
	}

	MEMORY_BASIC_INFORMATION memoryInfo{};
	if (!VirtualQuery(address, &memoryInfo, sizeof(memoryInfo))
		|| memoryInfo.State != MEM_COMMIT)
	{
		Warning("[PatchManager] WriteBytes target is not committed memory: %p\n", address);
		return false;
	}

	const uintptr_t regionBegin =
		reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
	const uintptr_t regionEnd = regionBegin + memoryInfo.RegionSize;
	const uintptr_t writeBegin = reinterpret_cast<uintptr_t>(address);
	if (regionBegin > UINTPTR_MAX - memoryInfo.RegionSize
		|| writeBegin < regionBegin
		|| writeBegin >= regionEnd
		|| len > regionEnd - writeBegin)
	{
		Warning("[PatchManager] WriteBytes range is invalid: %p (%zu bytes)\n", address, len);
		return false;
	}

	DWORD oldProtect;
	if (!VirtualProtect(address, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		Warning("[PatchManager] VirtualProtect failed at %p (error: %lu)\n", address, GetLastError());
		return false;
	}

	memcpy(address, bytes, len);
	const bool bytesWritten = memcmp(address, bytes, len) == 0;
	if (!FlushInstructionCache(GetCurrentProcess(), address, len)) {
		Warning(
			"[PatchManager] FlushInstructionCache failed at %p (error: %lu)\n",
			address,
			GetLastError());
	}

	DWORD dummy;
	if (!VirtualProtect(address, len, oldProtect, &dummy)) {
		Warning(
			"[PatchManager] Failed to restore page protection at %p (error: %lu)\n",
			address,
			GetLastError());
	}

	if (!bytesWritten) {
		Warning("[PatchManager] Write verification failed at %p\n", address);
	}
	return bytesWritten;
}

#endif // _WIN64
