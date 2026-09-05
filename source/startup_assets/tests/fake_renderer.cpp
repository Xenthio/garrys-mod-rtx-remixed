#include <cstdint>
#include <atomic>
static std::atomic<unsigned int> calls{0};
extern "C" void TestNoop() {}
extern "C" void* __stdcall Direct3DCreate9(unsigned int sdk) {
    ++calls;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000000u + sdk));
}
extern "C" long __stdcall Direct3DCreate9Ex(unsigned int sdk, void** result) {
    ++calls;
    if (!result) return -1;
    *result = reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000000u + sdk));
    return 0;
}
extern "C" __declspec(dllexport) unsigned int TestCallCount() { return calls.load(); }
