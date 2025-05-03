// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <windows.h>
#include <cstdint>       // For uintptr_t etc.
#include <MinHook.h>     // MinHook header
#include <obse64/PluginAPI.h> // Include the main OBSE header (REQUIRED - get from OBSE source/CommonLibOBSE)

// --- Global Variables ---
PluginHandle g_pluginHandle = kPluginHandle_Invalid; // Handle OBSE assigns
HMODULE g_GameHandle = nullptr;                      // Handle to game module (process)
uintptr_t g_GameBaseAddr = 0;                        // Base address of game module

// --- Function Pointer Types ---
// For the function we are hooking (MagickaCostFormula)
typedef float(__fastcall* MagickaCostFormula_t)(float Base_Cost, int Skill, int Luck);
// For the original function trampoline
MagickaCostFormula_t g_OriginalMagickaCostFormula = nullptr;

// For the luck_calculator function we need to call
typedef float(__fastcall* LuckCalculatorFunc_t)(int skill, int luck);
// Global variable to hold the runtime address of luck_calculator
LuckCalculatorFunc_t g_luck_calculator = nullptr;


// --- Your Hook Function ---
// Use extern "C" to prevent C++ name mangling if needed by hook setup/lookup (MinHook usually handles it okay)
extern "C" float __fastcall hkMagickaCostFormula(float Base_Cost, int Skill, int Luck)
{
    // Safety Check
    if (!g_luck_calculator) {
        //OutputDebugStringA("hkMagickaCostFormula: ERROR - g_luck_calculator is null!");
        return Base_Cost; // Fallback
    }
    if (g_GameBaseAddr == 0) { // Ensure game base address is valid
        //OutputDebugStringA("hkMagickaCostFormula: ERROR - g_GameBaseAddr is null!");
        return Base_Cost;
    }

    //OutputDebugStringA("hkMagickaCostFormula: Entered function."); // Log entry

    // Define offsets (USE CORRECTED baseOffset!)
    const uintptr_t multOffset = 0x8FE82D8;
    const uintptr_t baseOffset = 0x8FE82C8;
    uintptr_t multAddr = g_GameBaseAddr + multOffset;
    uintptr_t baseAddr = g_GameBaseAddr + baseOffset;

    float fMagicCasterSkillCostMult = 0.0f; // Initialize to default
    float fMagicCasterSkillCostBase = 0.0f; // Initialize to default

    // Log before first read
    //OutputDebugStringA("hkMagickaCostFormula: Attempting to read multAddr...");
    try { // Use try-catch for safety, though direct crash is more likely
        fMagicCasterSkillCostMult = *reinterpret_cast<float*>(multAddr);
        //OutputDebugStringA("hkMagickaCostFormula: Read multAddr OK.");
    }
    catch (...) {
        //OutputDebugStringA("hkMagickaCostFormula: EXCEPTION reading multAddr!");
        return Base_Cost; // Abort on error
    }


    // Log before second read
    //OutputDebugStringA("hkMagickaCostFormula: Attempting to read baseAddr...");
    try {
        fMagicCasterSkillCostBase = *reinterpret_cast<float*>(baseAddr);
        //OutputDebugStringA("hkMagickaCostFormula: Read baseAddr OK.");
    }
    catch (...) {
        //OutputDebugStringA("hkMagickaCostFormula: EXCEPTION reading baseAddr!");
        return Base_Cost; // Abort on error
    }

    // Log the values read
    //char buffer[256];
    //sprintf_s(buffer, sizeof(buffer), "hkMagickaCostFormula: Values Read: Mult=%f, Base=%f",
    //    fMagicCasterSkillCostMult, fMagicCasterSkillCostBase);
    //OutputDebugStringA(buffer);


    // Call Required Game Function
    //OutputDebugStringA("hkMagickaCostFormula: Calling luck_calculator...");
    float ModifiedSkill = g_luck_calculator(Skill, Luck);
    //sprintf_s(buffer, sizeof(buffer), "hkMagickaCostFormula: ModifiedSkill = %f", ModifiedSkill);
    //OutputDebugStringA(buffer);


    // Implement Your Custom Logic...
    float calculatedCost;

    if (ModifiedSkill > 100.0f) {
        calculatedCost = (Base_Cost * fMagicCasterSkillCostBase) * (100.0f / ModifiedSkill);
    }
    else {
        calculatedCost = ((1.0f - ModifiedSkill / 100.0f) * fMagicCasterSkillCostMult + fMagicCasterSkillCostBase) * Base_Cost;
    }

    // Return Result (Do NOT call original)
    return calculatedCost;
}


// --- Core Initialization Function ---
bool InitializeMod()
{
    //OutputDebugStringA("InitializeMod called!");

    // 1. Initialize MinHook (can be done here or in OBSEPlugin_Load, once is enough)
    // It's generally safe to call MH_Initialize multiple times if needed.
    if (MH_Initialize() != MH_OK) {
        //OutputDebugStringA("InitializeMod: MinHook init failed!");
        return false;
    }

    // 2. Get Game Module Handle & Base Address
    g_GameHandle = GetModuleHandleA(NULL);
    if (!g_GameHandle) {
        //OutputDebugStringA("InitializeMod: Failed to get game handle!");
        MH_Uninitialize(); // Clean up MinHook if init fails here
        return false;
    }
    g_GameBaseAddr = reinterpret_cast<uintptr_t>(g_GameHandle);
    OutputDebugStringA("InitializeMod: Got game handle and base address.");

    // 3. Calculate Runtime Addresses (Using Offsets)
    // MAKE SURE THESE OFFSETS ARE CORRECT FOR YOUR GAME VERSION
    const uintptr_t magickaCostFormulaOffset = 0x68d1040;
    const uintptr_t luckCalcOffset = 0x68C9FE0;

    uintptr_t magickaCostFormulaAddr = g_GameBaseAddr + magickaCostFormulaOffset;
    uintptr_t luckCalcAddr = g_GameBaseAddr + luckCalcOffset;
    OutputDebugStringA("InitializeMod: Calculated addresses.");

    // --- (Alternative: Use AOB Scan here) ---

    // 4. Store Address of luck_calculator
    if (luckCalcAddr) { // Basic check
        g_luck_calculator = reinterpret_cast<LuckCalculatorFunc_t>(luckCalcAddr);
    //    OutputDebugStringA("InitializeMod: Stored luck_calculator address.");
    }
    else {
    //    OutputDebugStringA("InitializeMod: Failed to find luck_calculator address!");
        MH_Uninitialize();
        return false; // Fail initialization
    }

    // 5. Create the Hook for MagickaCostFormula
    if (MH_CreateHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr),
        reinterpret_cast<LPVOID>(&hkMagickaCostFormula),
        reinterpret_cast<LPVOID*>(&g_OriginalMagickaCostFormula)) != MH_OK)
    {
    //    OutputDebugStringA("InitializeMod: Failed to create hook!");
        MH_Uninitialize();
        return false;
    }
    //OutputDebugStringA("InitializeMod: Hook created.");

    // 6. Enable the Hook
    if (MH_EnableHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr)) != MH_OK) {
    //    OutputDebugStringA("InitializeMod: Failed to enable hook!");
        // Attempt to remove the hook we just created before failing
        MH_RemoveHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr));
        MH_Uninitialize();
        return false;
    }

    //OutputDebugStringA("InitializeMod: Hook enabled successfully!");
    return true;
}


// --- Core Cleanup Function ---
void CleanupMod()
{
    //OutputDebugStringA("CleanupMod called!");
    // Check if base address is known before trying to disable hooks
    if (g_GameBaseAddr != 0) {
        // Recalculate target address to disable/remove hook
        const uintptr_t magickaCostFormulaOffset = 0x68d1040; // Use same offset
        uintptr_t magickaCostFormulaAddr = g_GameBaseAddr + magickaCostFormulaOffset;

        // Disable and remove the hook
        MH_DisableHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr));
        MH_RemoveHook(reinterpret_cast<LPVOID>(magickaCostFormulaAddr));
    //    OutputDebugStringA("CleanupMod: Hooks disabled and removed.");
    }
    //else {
    //    OutputDebugStringA("CleanupMod: Game base address unknown, cannot disable hooks safely.");
    //}

    // Uninitialize MinHook
    MH_Uninitialize();
    //OutputDebugStringA("CleanupMod: MinHook uninitialized.");
}


// --- OBSE Plugin Exports ---
extern "C" {

    // Version Data
    __declspec(dllexport) OBSEPluginVersionData OBSEPlugin_Version = {
        OBSEPluginVersionData::kVersion, // Structure version
        2,                               
        "Magicka Cost Skill Cap Fix",  // Your plugin's name
        "jab",                     
        OBSEPluginVersionData::kAddressIndependence_Signatures, // Indicate if using AOB/sig scanning (Set accurately)
        OBSEPluginVersionData::kStructureIndependence_NoStructs, // Indicate if using game structs directly (Set accurately)
        { 0 },                           // Compatible runtime version (set if needed)
        0,                               // OBSE version requirement (0 for any)
        0, 0, { 0 }                      // Reserved fields
    };

    // Load Function (Called by OBSE)
    __declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse) {
        g_pluginHandle = obse->GetPluginHandle();

        //OutputDebugStringA("OBSEPlugin_Load called!");

        // Call your main initialization function
        if (!InitializeMod()) {
            //OutputDebugStringA("OBSEPlugin_Load: InitializeMod failed!");
            return false; // Tell OBSE loading failed
        }

        // If using delayed init via messaging, set that up here instead of calling InitializeMod directly

        //OutputDebugStringA("OBSEPlugin_Load finished successfully.");
        return true; // Tell OBSE loading succeeded
    }

}; // End extern "C"


// --- Standard DllMain ---
// Keep this minimal
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // DO NOT CALL InitializeMod() or MH_Initialize() HERE
        break;
    case DLL_PROCESS_DETACH:
        // Cleanup might be called here, or might rely on OS cleanup
        // If you initialized MinHook in OBSEPlugin_Load, you MUST call
        // CleanupMod here (or ensure MH_Uninitialize is called somehow)
        // Check if lpReserved is NULL to see if it's a dynamic unload vs process termination
        if (lpReserved == nullptr) {
            CleanupMod();
        }
        break;
    }
    return TRUE;
}