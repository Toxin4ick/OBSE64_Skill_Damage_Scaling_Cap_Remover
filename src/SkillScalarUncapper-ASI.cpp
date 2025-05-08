// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <windows.h>
#include <cstdint>       // For uintptr_t etc.
#include <MinHook.h>     // MinHook header
#include <Psapi.h>       // For GetModuleInformation
#include <string>        // For std::string (used by AOB scanning)
#include <vector>        // For std::vector (used by AOB scanning)
#include <array>         // For std::array (for patch bytes)
#include <cstdio>        // For sprintf_s

#include "PatternScanner.h" // Include your pattern scanner header

#pragma comment(lib, "Psapi.lib") // Link against Psapi.lib for GetModuleInformation

// --- Global Variables ---
HMODULE g_GameHandle = nullptr;                      // Handle to game module (process's exe)
uintptr_t g_GameBaseAddr = 0;                        // Base address of game module
uintptr_t g_FoundMagickaCostFormulaAddr = 0;         // Store address of MagickaCostFormula found by AOB

// --- Function Pointer Types ---
// For the function we are hooking (MagickaCostFormula)
typedef float(__fastcall* MagickaCostFormula_t)(float Base_Cost, int Skill, int Luck);
// For the original function trampoline
MagickaCostFormula_t g_OriginalMagickaCostFormula = nullptr;

// For the luck_calculator function we need to call (this will point to the patched original)
typedef float(__fastcall* LuckCalculatorFunc_t)(int skill, int luck);
// Global variable to hold the runtime address of luck_calculator
LuckCalculatorFunc_t g_luck_calculator = nullptr;

// Global pointers to the dynamically found float game settings
float* g_pfMagicCasterSkillCostMult = nullptr;
float* g_pfMagicCasterSkillCostBase = nullptr;


// --- Helper function to resolve RIP-relative addresses from instructions ---
uintptr_t ResolveRipRelativeAddress(uintptr_t instructionAddress, size_t instructionLength, ptrdiff_t offsetToDisplacement) {
    if (instructionAddress == 0) {
        OutputDebugStringA("ResolveRipRelativeAddress: ERROR - instructionAddress is null.\n");
        return 0;
    }

    uintptr_t displacementAddress = instructionAddress + offsetToDisplacement;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(displacementAddress), &mbi, sizeof(mbi)) == 0 || mbi.Protect == PAGE_NOACCESS || mbi.Protect == 0) {
        char errBuf[256];
        sprintf_s(errBuf, sizeof(errBuf), "ResolveRipRelativeAddress: ERROR - Cannot read displacement at 0x%p (from instruction 0x%p)\n",
            (void*)displacementAddress, (void*)instructionAddress);
        OutputDebugStringA(errBuf);
        return 0;
    }

    int32_t relativeOffset = *reinterpret_cast<int32_t*>(displacementAddress);
    uintptr_t nextInstructionAddress = instructionAddress + instructionLength;
    return nextInstructionAddress + relativeOffset;
}


// --- Your Hook Function ---
extern "C" float __fastcall hkMagickaCostFormula(float Base_Cost, int Skill, int Luck)
{
    if (!g_luck_calculator) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - g_luck_calculator is null!\n");
        return Base_Cost;
    }
    if (!g_pfMagicCasterSkillCostMult || !g_pfMagicCasterSkillCostBase) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - Global cost factor pointers are null!\n");
        return Base_Cost;
    }

    float fMagicCasterSkillCostMult = 0.0f;
    float fMagicCasterSkillCostBase = 0.0f;

    MEMORY_BASIC_INFORMATION mbi; // Re-declare for local scope
    if (VirtualQuery(g_pfMagicCasterSkillCostMult, &mbi, sizeof(mbi)) == 0 || mbi.Protect == PAGE_NOACCESS || mbi.Protect == 0) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - g_pfMagicCasterSkillCostMult points to unreadable memory!\n");
        return Base_Cost;
    }
    fMagicCasterSkillCostMult = *g_pfMagicCasterSkillCostMult;

    if (VirtualQuery(g_pfMagicCasterSkillCostBase, &mbi, sizeof(mbi)) == 0 || mbi.Protect == PAGE_NOACCESS || mbi.Protect == 0) {
        OutputDebugStringA("hkMagickaCostFormula: ERROR - g_pfMagicCasterSkillCostBase points to unreadable memory!\n");
        return Base_Cost;
    }
    fMagicCasterSkillCostBase = *g_pfMagicCasterSkillCostBase;

    // char debugBuffer[256];
    // sprintf_s(debugBuffer, sizeof(debugBuffer), "hkMagickaCostFormula: Values Read: Mult=%f (from 0x%p), Base=%f (from 0x%p)\n",
    //    fMagicCasterSkillCostMult, (void*)g_pfMagicCasterSkillCostMult,
    //    fMagicCasterSkillCostBase, (void*)g_pfMagicCasterSkillCostBase);
    // OutputDebugStringA(debugBuffer);

    float ModifiedSkill = g_luck_calculator(Skill, Luck); // Calls the PATCHED original luck_skill_modifier

    float calculatedCost;
    if (ModifiedSkill > 100.0f) {
        calculatedCost = (Base_Cost * fMagicCasterSkillCostBase) * (100.0f / ModifiedSkill);
    }
    else {
        calculatedCost = ((1.0f - ModifiedSkill / 100.0f) * fMagicCasterSkillCostMult + fMagicCasterSkillCostBase) * Base_Cost;
    }

    return calculatedCost;
}


// --- Core Initialization Function ---
bool InitializeMod()
{
    OutputDebugStringA("InitializeMod called!\n");
    char buffer[512];

    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("InitializeMod: MinHook init failed!\n");
        return false;
    }
    OutputDebugStringA("InitializeMod: MinHook initialized successfully.\n");

    g_GameHandle = GetModuleHandleA(NULL);
    if (!g_GameHandle) {
        OutputDebugStringA("InitializeMod: Failed to get game handle!\n");
        MH_Uninitialize(); return false;
    }
    g_GameBaseAddr = reinterpret_cast<uintptr_t>(g_GameHandle);

    MODULEINFO modInfo = { 0 };
    if (!GetModuleInformation(GetCurrentProcess(), g_GameHandle, &modInfo, sizeof(MODULEINFO))) {
        OutputDebugStringA("InitializeMod: Failed to get module information!\n");
        MH_Uninitialize(); return false;
    }
    size_t gameModuleSize = modInfo.SizeOfImage;
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Game Module Base: 0x%p, Size: 0x%IX\n", (void*)g_GameBaseAddr, gameModuleSize);
    OutputDebugStringA(buffer);

    uintptr_t magickaCostFormulaAddr_local = 0;
    uintptr_t luckCalcAddr_local = 0;

    // 1. Find MagickaCostFormula
    const std::string magickaCostFormulaAOB = "48 83 EC 38 8B CA 0F 29 74 24 20 41 8B D0 0F 28 F0 E8 ?? ?? ?? ??";
    std::vector<uint8_t> magickaCostFormulaBytes;
    std::vector<bool> magickaCostFormulaMask;

    OutputDebugStringA(("InitializeMod: Parsing MagickaCostFormula AOB: " + magickaCostFormulaAOB + "\n").c_str());
    if (!PatternScan::ParseAOBString(magickaCostFormulaAOB, magickaCostFormulaBytes, magickaCostFormulaMask)) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to parse MagickaCostFormula AOB string!\n");
        MH_Uninitialize(); return false;
    }

    magickaCostFormulaAddr_local = PatternScan::FindPattern(g_GameBaseAddr, gameModuleSize, magickaCostFormulaBytes, magickaCostFormulaMask);
    if (magickaCostFormulaAddr_local == 0) {
        OutputDebugStringA("InitializeMod: ERROR - MagickaCostFormula pattern NOT FOUND using AOB!\n");
        MH_Uninitialize(); return false;
    }
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Found MagickaCostFormula via AOB at: 0x%p\n", (void*)magickaCostFormulaAddr_local);
    OutputDebugStringA(buffer);

    // 2. Derive luck_skill_modifier address from the call within MagickaCostFormula
    const ptrdiff_t callOpcodeOffsetInAOB = 17;
    uintptr_t callInstructionAddress = magickaCostFormulaAddr_local + callOpcodeOffsetInAOB;
    if (*reinterpret_cast<uint8_t*>(callInstructionAddress) == 0xE8) {
        luckCalcAddr_local = ResolveRipRelativeAddress(callInstructionAddress, 5, 1);
        if (luckCalcAddr_local == 0) {
            OutputDebugStringA("InitializeMod: ERROR - Failed to resolve luck_skill_modifier address from call.\n");
            MH_Uninitialize(); return false;
        }
        sprintf_s(buffer, sizeof(buffer), "InitializeMod: Derived luck_skill_modifier address: 0x%p\n", (void*)luckCalcAddr_local);
        OutputDebugStringA(buffer);
    }
    else {
        sprintf_s(buffer, sizeof(buffer), "InitializeMod: ERROR - Expected E8 for luck_skill_modifier call at 0x%p, found 0x%02X!\n", (void*)callInstructionAddress, *reinterpret_cast<uint8_t*>(callInstructionAddress));
        OutputDebugStringA(buffer); MH_Uninitialize(); return false;
    }
    g_luck_calculator = reinterpret_cast<LuckCalculatorFunc_t>(luckCalcAddr_local); // Store for hook function

    // 3. Patch luck_skill_modifier (remove the cap)
    // The instruction to patch is "cmovbe rax, rcx" (48 0F 46 C1)
    // We will replace it with "mov rax, rcx; nop" (48 89 C8 90)
    const std::string cmovbeAOB = "48 0F 46 C1";
    std::vector<uint8_t> cmovbeBytes;
    std::vector<bool> cmovbeMask;
    constexpr std::array<std::uint8_t, 4> luckPatchBytes = { 0x48, 0x89, 0xC8, 0x90 };

    OutputDebugStringA(("InitializeMod: Parsing cmovbe AOB for luck_skill_modifier: " + cmovbeAOB + "\n").c_str());
    if (!PatternScan::ParseAOBString(cmovbeAOB, cmovbeBytes, cmovbeMask)) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to parse cmovbe AOB string!\n");
        MH_Uninitialize(); return false;
    }
    // Search for cmovbe within the first ~100 bytes of luck_skill_modifier
    uintptr_t cmovbeAddress = PatternScan::FindPattern(luckCalcAddr_local, 100, cmovbeBytes, cmovbeMask);
    if (cmovbeAddress == 0) {
        sprintf_s(buffer, sizeof(buffer), "InitializeMod: ERROR - cmovbe instruction (48 0F 46 C1) NOT FOUND within luck_skill_modifier (starting at 0x%p)!\n", (void*)luckCalcAddr_local);
        OutputDebugStringA(buffer);
        MH_Uninitialize(); return false;
    }
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Found cmovbe instruction to patch at: 0x%p\n", (void*)cmovbeAddress);
    OutputDebugStringA(buffer);

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(cmovbeAddress), luckPatchBytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        sprintf_s(buffer, sizeof(buffer), "InitializeMod: ERROR - VirtualProtect PAGE_EXECUTE_READWRITE failed for luck patch at 0x%p! Error: %lu\n", (void*)cmovbeAddress, GetLastError());
        OutputDebugStringA(buffer);
        MH_Uninitialize(); return false;
    }
    memcpy(reinterpret_cast<void*>(cmovbeAddress), luckPatchBytes.data(), luckPatchBytes.size());
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Patched luck_skill_modifier at 0x%p.\n", (void*)cmovbeAddress);
    OutputDebugStringA(buffer);
    DWORD dummy = 0; // Needs a variable for the last parameter
    if (!VirtualProtect(reinterpret_cast<void*>(cmovbeAddress), luckPatchBytes.size(), oldProtect, &dummy)) {
        sprintf_s(buffer, sizeof(buffer), "InitializeMod: WARNING - VirtualProtect restore failed for luck patch at 0x%p! Error: %lu\n", (void*)cmovbeAddress, GetLastError());
        OutputDebugStringA(buffer); // Non-fatal, but not ideal
    }


    // 4. Find fMagicCasterSkillCostMult within MagickaCostFormula
    const std::string multAOB = "F3 0F 59 0D ?? ?? ?? ??";
    std::vector<uint8_t> multBytes;
    std::vector<bool> multMask;
    if (!PatternScan::ParseAOBString(multAOB, multBytes, multMask)) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to parse fMagicCasterSkillCostMult AOB!\n");
        MH_Uninitialize(); return false;
    }
    uintptr_t multInstructionAddr = PatternScan::FindPattern(magickaCostFormulaAddr_local, 100, multBytes, multMask);
    if (multInstructionAddr == 0) {
        OutputDebugStringA("InitializeMod: ERROR - fMagicCasterSkillCostMult instruction (F3 0F 59 0D) NOT FOUND within MagickaCostFormula!\n");
        MH_Uninitialize(); return false;
    }
    g_pfMagicCasterSkillCostMult = reinterpret_cast<float*>(ResolveRipRelativeAddress(multInstructionAddr, 8, 4));
    if (g_pfMagicCasterSkillCostMult == nullptr) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to resolve fMagicCasterSkillCostMult address.\n");
        MH_Uninitialize(); return false;
    }
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Found fMagicCasterSkillCostMult instruction at 0x%p, value address: 0x%p\n",
        (void*)multInstructionAddr, (void*)g_pfMagicCasterSkillCostMult);
    OutputDebugStringA(buffer);

    // 5. Find fMagicCasterSkillCostBase within MagickaCostFormula
    const std::string baseAOB = "F3 0F 58 0D ?? ?? ?? ??";
    std::vector<uint8_t> baseBytes;
    std::vector<bool> baseMask;
    if (!PatternScan::ParseAOBString(baseAOB, baseBytes, baseMask)) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to parse fMagicCasterSkillCostBase AOB!\n");
        MH_Uninitialize(); return false;
    }
    uintptr_t searchStartForBase = multInstructionAddr + 8;
    uintptr_t baseInstructionAddr = PatternScan::FindPattern(searchStartForBase, 100 - (searchStartForBase - magickaCostFormulaAddr_local), baseBytes, baseMask);
    if (baseInstructionAddr == 0) {
        baseInstructionAddr = PatternScan::FindPattern(magickaCostFormulaAddr_local, 100, baseBytes, baseMask);
    }
    if (baseInstructionAddr == 0) {
        OutputDebugStringA("InitializeMod: ERROR - fMagicCasterSkillCostBase instruction (F3 0F 58 0D) NOT FOUND within MagickaCostFormula!\n");
        MH_Uninitialize(); return false;
    }
    g_pfMagicCasterSkillCostBase = reinterpret_cast<float*>(ResolveRipRelativeAddress(baseInstructionAddr, 8, 4));
    if (g_pfMagicCasterSkillCostBase == nullptr) {
        OutputDebugStringA("InitializeMod: ERROR - Failed to resolve fMagicCasterSkillCostBase address.\n");
        MH_Uninitialize(); return false;
    }
    sprintf_s(buffer, sizeof(buffer), "InitializeMod: Found fMagicCasterSkillCostBase instruction at 0x%p, value address: 0x%p\n",
        (void*)baseInstructionAddr, (void*)g_pfMagicCasterSkillCostBase);
    OutputDebugStringA(buffer);

    // Final check for all dynamically found elements
    if (luckCalcAddr_local == 0 || g_pfMagicCasterSkillCostMult == nullptr || g_pfMagicCasterSkillCostBase == nullptr) {
        OutputDebugStringA("InitializeMod: ERROR - One or more required addresses/pointers are null after all scanning and patching!\n");
        MH_Uninitialize(); return false;
    }

    // 6. Create and enable the hook for MagickaCostFormula
    if (MH_CreateHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr_local),
        reinterpret_cast<LPVOID>(&hkMagickaCostFormula),
        reinterpret_cast<LPVOID*>(&g_OriginalMagickaCostFormula)) != MH_OK)
    {
        OutputDebugStringA("InitializeMod: Failed to create hook for MagickaCostFormula!\n");
        MH_Uninitialize(); return false;
    }
    OutputDebugStringA("InitializeMod: MagickaCostFormula hook created.\n");

    if (MH_EnableHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr_local)) != MH_OK) {
        OutputDebugStringA("InitializeMod: Failed to enable MagickaCostFormula hook!\n");
        MH_RemoveHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr_local));
        MH_Uninitialize(); return false;
    }
    OutputDebugStringA("InitializeMod: MagickaCostFormula hook enabled successfully.\n");

    g_FoundMagickaCostFormulaAddr = magickaCostFormulaAddr_local;
    OutputDebugStringA("InitializeMod: Initialization completed successfully.\n");
    return true;
}


// --- Core Cleanup Function ---
void CleanupMod()
{
    OutputDebugStringA("CleanupMod called!\n");
    // Note: The patch to luck_skill_modifier is not automatically reverted here.
    // If this DLL is unloaded mid-game, the patch remains. For most game modding, this is acceptable.
    // Reverting it would require storing the original bytes and writing them back,
    // but that adds complexity and might not be necessary if the game is typically restarted.

    if (g_FoundMagickaCostFormulaAddr != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_FoundMagickaCostFormulaAddr));
        OutputDebugStringA("CleanupMod: MagickaCostFormula hook disabled and removed.\n");
        g_FoundMagickaCostFormulaAddr = 0;
    }
    else {
        OutputDebugStringA("CleanupMod: MagickaCostFormula hook address was not set, skipping explicit disable/remove.\n");
    }
    MH_Uninitialize(); // This cleans up all MinHook related resources.
    OutputDebugStringA("CleanupMod: MinHook uninitialized.\n");
}



// --- Standard DllMain ---
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        OutputDebugStringA("DllMain: DLL_PROCESS_ATTACH\n");
        InitializeMod();
        break;
    case DLL_PROCESS_DETACH:
        OutputDebugStringA("DllMain: DLL_PROCESS_DETACH\n");
        if (lpReserved == nullptr) {
            OutputDebugStringA("DllMain: DLL_PROCESS_DETACH (Dynamic Unload) - Calling CleanupMod.\n");
            CleanupMod();
        }
        else {
            OutputDebugStringA("DllMain: DLL_PROCESS_DETACH (Process Terminating) - Calling CleanupMod.\n");
            CleanupMod(); // Call anyway for MinHook uninitialization
        }
        break;
    }
    return TRUE;
}