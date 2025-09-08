// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h" // Precompiled header, ensure it's configured or remove if not used
#include <windows.h>
#include <cstdint>      // For uintptr_t, int32_t etc.
#include <vector>       // For std::vector
#include <string>       // For std::string
#include <array>        // For std::array
#include <cstdio>       // For sprintf_s

#include <Psapi.h>      // For GetModuleInformation
#include <MinHook.h>    // MinHook header
#include <obse64/PluginAPI.h> // OBSE header

#include "PatternScanner.h" // Your pattern scanner

#pragma comment(lib, "Psapi.lib") // Link against Psapi.lib

// --- Global Variables ---
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
HMODULE      g_GameHandle = nullptr;
uintptr_t    g_GameBaseAddr = 0;
size_t       g_GameModuleSize = 0;
uintptr_t    g_FoundMagickaCostFormulaAddr = 0; // Store address of hooked function for cleanup

// Pointers to game's global float settings, found dynamically
float* g_pfMagicCasterSkillCostMult = nullptr;
float* g_pfMagicCasterSkillCostBase = nullptr;

// --- Function Pointer Types ---
typedef float(__fastcall* MagickaCostFormula_t)(float Base_Cost, int Skill, int Luck);
MagickaCostFormula_t g_OriginalMagickaCostFormula = nullptr; // Trampoline for original

typedef float(__fastcall* LuckCalculatorFunc_t)(int skill, int luck);
LuckCalculatorFunc_t g_luck_calculator = nullptr; // Points to game's luck_skill_modifier (patched)

float getCalculateCost(int ModifiedSkill, float fMagicCasterSkillCostMult, float fMagicCasterSkillCostBase, float Base_Cost) {
    if (ModifiedSkill > 100) {
        float baseMultiplier = 0.3f;
        float a = 0.44f;
        float b = 2.0f;
        float divider = 1.0f + a * powf(static_cast<float>(ModifiedSkill - 100), b);

        float rawCost = ((1.0f - static_cast<float>(ModifiedSkill) / 100.0f) * fMagicCasterSkillCostMult + baseMultiplier) * Base_Cost;
        return rawCost / divider;
    }
    
    return ((1.0f - static_cast<float>(ModifiedSkill) / 100.0f) * fMagicCasterSkillCostMult + fMagicCasterSkillCostBase) * Base_Cost;
}

float getModifiedSkill(int Skill, int Luck) {
    float ModifiedSkill = g_luck_calculator(Skill, Luck);

    if (ModifiedSkill > 100 && Skill < 100) {
        ModifiedSkill = 100;
    }

    if (Skill >= 100) {
        ModifiedSkill = Skill;
    }

    return ModifiedSkill;
}

// --- Helper: Resolve RIP-Relative Address ---
uintptr_t ResolveRipRelativeAddress(uintptr_t instructionAddress, size_t instructionLength, ptrdiff_t offsetToDisplacement) {
    if (instructionAddress == 0) {
        OutputDebugStringA("ResolveRipRelativeAddress: ERROR - instructionAddress is null.\n");
        return 0;
    }
    uintptr_t displacementValueAddress = instructionAddress + offsetToDisplacement;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(displacementValueAddress), &mbi, sizeof(mbi)) == 0 ||
        !(mbi.Protect & PAGE_EXECUTE_READ || mbi.Protect & PAGE_READONLY || mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_EXECUTE_READWRITE) ||
        mbi.State != MEM_COMMIT) {
        char errBuf[256];
        sprintf_s(errBuf, sizeof(errBuf), "ResolveRipRelativeAddress: ERROR - Cannot query/read displacement at 0x%p (from instruction 0x%p). Protect: 0x%lX, State: 0x%lX\n",
            (void*)displacementValueAddress, (void*)instructionAddress, mbi.Protect, mbi.State);
        OutputDebugStringA(errBuf);
        return 0;
    }
    int32_t relativeOffset = *reinterpret_cast<int32_t*>(displacementValueAddress);
    uintptr_t nextInstructionAddress = instructionAddress + instructionLength;
    return nextInstructionAddress + relativeOffset;
}

// --- Hook Function: MagickaCostFormula ---
extern "C" float __fastcall hkMagickaCostFormula(float Base_Cost, int Skill, int Luck) {
    if (!g_luck_calculator || !g_pfMagicCasterSkillCostMult || !g_pfMagicCasterSkillCostBase) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - One or more required global pointers are null.\n");
        // Attempt to call original if available, otherwise return base cost
        return (g_OriginalMagickaCostFormula) ? g_OriginalMagickaCostFormula(Base_Cost, Skill, Luck) : Base_Cost;
    }

    float fMagicCasterSkillCostMult = 0.0f;
    float fMagicCasterSkillCostBase = 0.0f;

    MEMORY_BASIC_INFORMATION mbi; // Re-declare for local scope, check readability of game settings
    if (VirtualQuery(g_pfMagicCasterSkillCostMult, &mbi, sizeof(mbi)) == 0 || !(mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_READONLY || mbi.Protect & PAGE_WRITECOPY) || mbi.State != MEM_COMMIT) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - Cannot read g_pfMagicCasterSkillCostMult.\n");
        return (g_OriginalMagickaCostFormula) ? g_OriginalMagickaCostFormula(Base_Cost, Skill, Luck) : Base_Cost;
    }
    fMagicCasterSkillCostMult = *g_pfMagicCasterSkillCostMult;

    if (VirtualQuery(g_pfMagicCasterSkillCostBase, &mbi, sizeof(mbi)) == 0 || !(mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_READONLY || mbi.Protect & PAGE_WRITECOPY) || mbi.State != MEM_COMMIT) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - Cannot read g_pfMagicCasterSkillCostBase.\n");
        return (g_OriginalMagickaCostFormula) ? g_OriginalMagickaCostFormula(Base_Cost, Skill, Luck) : Base_Cost;
    }
    fMagicCasterSkillCostBase = *g_pfMagicCasterSkillCostBase;

    return getCalculateCost(getModifiedSkill(Skill, Luck), fMagicCasterSkillCostMult, fMagicCasterSkillCostBase, Base_Cost);
}

// --- Core Mod Initialization ---
bool InitializeMod() {
    OutputDebugStringA("Skill Scalar Uncapper: Initializing...\n");

    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("Skill Scalar Uncapper: MinHook initialization failed.\n");
        return false;
    }

    g_GameHandle = GetModuleHandleA(NULL);
    if (!g_GameHandle) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to get game module handle.\n");
        MH_Uninitialize();
        return false;
    }
    g_GameBaseAddr = reinterpret_cast<uintptr_t>(g_GameHandle);

    MODULEINFO modInfo = { 0 };
    if (!GetModuleInformation(GetCurrentProcess(), g_GameHandle, &modInfo, sizeof(MODULEINFO))) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to get module information.\n");
        MH_Uninitialize();
        return false;
    }
    g_GameModuleSize = modInfo.SizeOfImage;

    char logBuffer[512]; // General purpose buffer for formatted logs
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Game Base: 0x%p, Size: 0x%IX\n", (void*)g_GameBaseAddr, g_GameModuleSize);
    OutputDebugStringA(logBuffer);

    // 1. Find MagickaCostFormula
    const std::string magickaCostFormulaAOB = "48 83 EC 38 8B CA 0F 29 74 24 20 41 8B D0 0F 28 F0 E8 ?? ?? ?? ??";
    std::vector<uint8_t> magickaCostFormulaBytes;
    std::vector<bool> magickaCostFormulaMask;
    if (!PatternScan::ParseAOBString(magickaCostFormulaAOB, magickaCostFormulaBytes, magickaCostFormulaMask)) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to parse MagickaCostFormula AOB.\n");
        MH_Uninitialize(); return false;
    }
    uintptr_t magickaFormulaAddr = PatternScan::FindPattern(g_GameBaseAddr, g_GameModuleSize, magickaCostFormulaBytes, magickaCostFormulaMask);
    if (magickaFormulaAddr == 0) {
        OutputDebugStringA("Skill Scalar Uncapper: MagickaCostFormula pattern not found.\n");
        MH_Uninitialize(); return false;
    }
    g_FoundMagickaCostFormulaAddr = magickaFormulaAddr; // Store for hook and cleanup
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Found MagickaCostFormula at 0x%p\n", (void*)g_FoundMagickaCostFormulaAddr);
    OutputDebugStringA(logBuffer);

    // 2. Derive luck_skill_modifier (g_luck_calculator) from MagickaCostFormula
    const ptrdiff_t callOffsetInFormula = 17; // Offset of E8 (call) in MagickaCostFormulaAOB
    uintptr_t callToLuckModAddr = g_FoundMagickaCostFormulaAddr + callOffsetInFormula;
    if (reinterpret_cast<unsigned char*>(callToLuckModAddr)[0] != 0xE8) {
        sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Expected E8 for luck_skill_modifier call, found 0x%02X at 0x%p\n",
            reinterpret_cast<unsigned char*>(callToLuckModAddr)[0], (void*)callToLuckModAddr);
        OutputDebugStringA(logBuffer); MH_Uninitialize(); return false;
    }
    uintptr_t luckModAddr = ResolveRipRelativeAddress(callToLuckModAddr, 5, 1);
    if (luckModAddr == 0) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to resolve luck_skill_modifier address.\n");
        MH_Uninitialize(); return false;
    }
    g_luck_calculator = reinterpret_cast<LuckCalculatorFunc_t>(luckModAddr);
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Derived luck_skill_modifier at 0x%p\n", (void*)luckModAddr);
    OutputDebugStringA(logBuffer);

    // 3. Find and resolve fMagicCasterSkillCostMult
    const std::string multAOB = "F3 0F 59 0D ?? ?? ?? ??"; // mulss xmm, [rip+offset]
    std::vector<uint8_t> multBytes; std::vector<bool> multMask;
    if (!PatternScan::ParseAOBString(multAOB, multBytes, multMask)) { /*Error*/ MH_Uninitialize(); return false; }
    uintptr_t multInstrAddr = PatternScan::FindPattern(g_FoundMagickaCostFormulaAddr, 100, multBytes, multMask); // Search within MagickaCostFormula
    if (multInstrAddr == 0) { /*Error*/ OutputDebugStringA("Skill Scalar Uncapper: fMagicCasterSkillCostMult pattern not found.\n"); MH_Uninitialize(); return false; }
    g_pfMagicCasterSkillCostMult = reinterpret_cast<float*>(ResolveRipRelativeAddress(multInstrAddr, 8, 4));
    if (!g_pfMagicCasterSkillCostMult) { /*Error*/ OutputDebugStringA("Skill Scalar Uncapper: Failed to resolve fMagicCasterSkillCostMult address.\n"); MH_Uninitialize(); return false; }
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Found fMagicCasterSkillCostMult (instr 0x%p) at 0x%p\n", (void*)multInstrAddr, (void*)g_pfMagicCasterSkillCostMult);
    OutputDebugStringA(logBuffer);

    // 4. Find and resolve fMagicCasterSkillCostBase
    const std::string baseAOB = "F3 0F 58 0D ?? ?? ?? ??"; // addss xmm, [rip+offset]
    std::vector<uint8_t> baseBytes; std::vector<bool> baseMask;
    if (!PatternScan::ParseAOBString(baseAOB, baseBytes, baseMask)) { /*Error*/ MH_Uninitialize(); return false; }
    uintptr_t searchStartForBase = multInstrAddr + multBytes.size(); // Start after the mult instruction
    size_t remainingSearchLength = 100 - (searchStartForBase - g_FoundMagickaCostFormulaAddr);
    uintptr_t baseInstrAddr = 0;
    if (remainingSearchLength > baseBytes.size()) { // Check if remaining space is sensible
        baseInstrAddr = PatternScan::FindPattern(searchStartForBase, remainingSearchLength, baseBytes, baseMask);
    }
    if (baseInstrAddr == 0) { // Fallback: search entire 100 byte range from start of MagickaCostFormula
        baseInstrAddr = PatternScan::FindPattern(g_FoundMagickaCostFormulaAddr, 100, baseBytes, baseMask);
    }
    if (baseInstrAddr == 0) { /*Error*/ OutputDebugStringA("Skill Scalar Uncapper: fMagicCasterSkillCostBase pattern not found.\n"); MH_Uninitialize(); return false; }
    g_pfMagicCasterSkillCostBase = reinterpret_cast<float*>(ResolveRipRelativeAddress(baseInstrAddr, 8, 4));
    if (!g_pfMagicCasterSkillCostBase) { /*Error*/ OutputDebugStringA("Skill Scalar Uncapper: Failed to resolve fMagicCasterSkillCostBase address.\n"); MH_Uninitialize(); return false; }
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Found fMagicCasterSkillCostBase (instr 0x%p) at 0x%p\n", (void*)baseInstrAddr, (void*)g_pfMagicCasterSkillCostBase);
    OutputDebugStringA(logBuffer);

    // 5. Patch luck_skill_modifier to remove skill cap
    const std::string cmovbeAOB = "48 0F 46 C1"; // cmovbe rax, rcx
    std::vector<uint8_t> cmovbeBytes; std::vector<bool> cmovbeMask;
    const std::array<uint8_t, 4> luckPatchBytes = { 0x48, 0x89, 0xC8, 0x90 }; // mov rax, rcx; nop
    if (!PatternScan::ParseAOBString(cmovbeAOB, cmovbeBytes, cmovbeMask)) { /*Error*/ MH_Uninitialize(); return false; }
    uintptr_t cmovbeAddr = PatternScan::FindPattern(luckModAddr, 100, cmovbeBytes, cmovbeMask); // Search within luck_skill_modifier
    if (cmovbeAddr == 0) {
        sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: cmovbe instruction not found in luck_skill_modifier (0x%p).\n", (void*)luckModAddr);
        OutputDebugStringA(logBuffer); MH_Uninitialize(); return false;
    }
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(cmovbeAddr), luckPatchBytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: VirtualProtect (RWX) failed for luck patch: %lu\n", GetLastError());
        OutputDebugStringA(logBuffer); MH_Uninitialize(); return false;
    }
    memcpy(reinterpret_cast<void*>(cmovbeAddr), luckPatchBytes.data(), luckPatchBytes.size());
    DWORD dummy; // VirtualProtect needs a non-null pointer for oldProtect
    VirtualProtect(reinterpret_cast<void*>(cmovbeAddr), luckPatchBytes.size(), oldProtect, &dummy); // Restore protection (log warning on fail)
    sprintf_s(logBuffer, sizeof(logBuffer), "Skill Scalar Uncapper: Patched luck_skill_modifier at 0x%p\n", (void*)cmovbeAddr);
    OutputDebugStringA(logBuffer);

    // 6. Create and Enable Hook for MagickaCostFormula
    if (MH_CreateHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr), reinterpret_cast<LPVOID>(&hkMagickaCostFormula),
        reinterpret_cast<LPVOID*>(&g_OriginalMagickaCostFormula)) != MH_OK) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to create hook.\n");
        MH_Uninitialize(); return false;
    }
    if (MH_EnableHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr)) != MH_OK) {
        OutputDebugStringA("Skill Scalar Uncapper: Failed to enable hook.\n");
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr)); // Attempt to clean up hook
        MH_Uninitialize(); return false;
    }

    OutputDebugStringA("Skill Scalar Uncapper: Initialization successful.\n");
    return true;
}

// --- Core Mod Cleanup ---
void CleanupMod() {
    OutputDebugStringA("Skill Scalar Uncapper: Cleaning up...\n");
    // Note: The patch to luck_skill_modifier is not reverted.
    if (g_FoundMagickaCostFormulaAddr != 0) { // Only try to disable/remove if it was found
        MH_DisableHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr));
    }
    MH_Uninitialize(); // Uninitialize MinHook
    OutputDebugStringA("Skill Scalar Uncapper: Cleanup finished.\n");
}

// --- OBSE Plugin Exports ---
extern "C" {
    __declspec(dllexport) OBSEPluginVersionData OBSEPlugin_Version = {
        OBSEPluginVersionData::kVersion,
        2, // Plugin version
        "Skill Scalar Uncapper",
        "jab", // Author
        OBSEPluginVersionData::kAddressIndependence_Signatures,
        OBSEPluginVersionData::kStructureIndependence_NoStructs,
        {0}, // Compatible Oblivion.exe version (usually { RUNTIME_VERSION_1_2_416, 0 } or left {0} for any)
        0,   // OBSE major version requirement (0 for any)
        0, 0, {0} // Reserved
    };

    __declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse) {
        g_pluginHandle = obse->GetPluginHandle();
        // Optional: Query OBSE interface versions, pass obse to other functions if needed

        if (!InitializeMod()) {
            // Optionally, use OBSE's _MESSAGE or similar for in-game error if load fails.
            return false; // Signal OBSE that plugin loading failed
        }
        return true; // Signal OBSE that plugin loaded successfully
    }
} // End extern "C"

// --- Standard DllMain ---
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule); // Optimization for DLLs not creating threads
        // Do not call InitializeMod or MH_Initialize here; OBSEPlugin_Load handles it.
        break;
    case DLL_PROCESS_DETACH:
        // lpReserved is NULL if FreeLibrary is called or the DLL is unloaded dynamically.
        // lpReserved is non-NULL if the process is terminating.
        // We should call CleanupMod in both cases to ensure MinHook is uninitialized.
        CleanupMod();
        break;
    }
    return TRUE;
}