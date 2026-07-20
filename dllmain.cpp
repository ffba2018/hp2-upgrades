#include "pch.h"
#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <regex>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#pragma comment(lib, "winmm.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


std::string g_soundUpgradePath;
std::string g_soundClickPath;
std::string g_soundErrorPath;

void PlayUpgradeSound() {
    if (!g_soundUpgradePath.empty()) {
        PlaySoundA(g_soundUpgradePath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

void PlayClickSound() {
    if (!g_soundClickPath.empty()) {
        PlaySoundA(g_soundClickPath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

void PlayErrorSound() {
    if (!g_soundErrorPath.empty()) {
        PlaySoundA(g_soundErrorPath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

bool SafeReadInt(uintptr_t addr, int& outVal) {
    __try {
        outVal = *(int*)addr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteInt(uintptr_t addr, int val) {
    __try {
        *(int*)addr = val;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// money shit
struct Chain {
    uintptr_t moduleOffset;
    std::vector<uintptr_t> offsets;
};

static std::vector<Chain> candidates = {
    { 0x00457F1C, {0x2600, 0xE8, 0x7C, 0x1AC, 0x44, 0x138} },
    { 0x00457F1C, {0x2600, 0xE8, 0x7C, 0x1AC, 0x14, 0x178} },
    { 0x00457F1C, {0x2600, 0xE8, 0x7C, 0x14, 0x1A0, 0x20} },
    { 0x00457F1C, {0x2600, 0xE8, 0xC, 0x124, 0x1AC, 0x24} },
    { 0x00457F1C, {0x2600, 0xC8, 0x1AC, 0x44, 0x7C, 0x17C} },
    { 0x00455DE4, {0x2610, 0xAC, 0xB8, 0x14, 0x8, 0x1A0} },
    { 0x00455DE4, {0x2600, 0xE8, 0x18, 0x7C, 0x1BC, 0x1A0} },
    { 0x004519CC, {0x2600, 0xE8, 0x28, 0xC, 0xB0, 0xC} },
    { 0x004519C8, {0x2610, 0x18C, 0x8, 0xCC, 0x8, 0x8} },
    { 0x004519C8, {0x2610, 0x17C, 0xB8, 0xC, 0x8, 0x8} },
};

uintptr_t ResolveMoneyAddress(const Chain& c) {
    __try {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        if (!base) return 0;
        uintptr_t addr = *(uintptr_t*)(base + c.moduleOffset);
        if (!addr) return 0;
        size_t n = c.offsets.size();
        for (size_t idx = n - 1; idx >= 1; idx--) {
            addr = *(uintptr_t*)(addr + c.offsets[idx]);
            if (!addr) return 0;
        }
        return addr + c.offsets[0];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::atomic<int>  g_currentMoney{ 0 };
std::atomic<bool> g_addressFound{ false };

void MoneyLoop() {
    while (true) {
        bool found = false;
        for (auto& c : candidates) {
            uintptr_t moneyAddr = ResolveMoneyAddress(c);
            if (moneyAddr) {
                int val = 0;
                if (SafeReadInt(moneyAddr, val)) {
                    g_currentMoney.store(val);
                    found = true;
                    break;
                }
            }
        }
        g_addressFound.store(found);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool AddMoney(int amountToAdd) {
    if (amountToAdd <= 0) return true;

    int currentMoney = 0;
    uintptr_t validAddr = 0;
    for (auto& c : candidates) {
        uintptr_t addr = ResolveMoneyAddress(c);
        if (addr && SafeReadInt(addr, currentMoney)) {
            validAddr = addr;
            break;
        }
    }

    if (!validAddr) return false;

    int targetMoney = currentMoney + amountToAdd;
    auto startTime = std::chrono::steady_clock::now();
    bool moneyAdded = false;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() < 2000) {
        if (SafeWriteInt(validAddr, targetMoney)) {
            int checkVal = 0;
            if (SafeReadInt(validAddr, checkVal) && checkVal >= targetMoney) {
                moneyAdded = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return moneyAdded;
}

// upgrades
std::string g_carsDir;
std::string g_modIniPath;
std::string g_bannerImagePath;
std::vector<std::string> g_carList;
std::string g_selectedCar;
std::string g_statusMsg;

LPDIRECT3DTEXTURE9 g_pBannerTexture = nullptr;
int g_bannerWidth = 0;
int g_bannerHeight = 0;

const int MAX_UPGRADES = 5;
const int UPGRADE_COSTS[MAX_UPGRADES] = { 10000, 12000, 15000, 16000, 20000 };

const int TUNING_COSTS[4] = { 0, 1000, 4000, 10000 };

std::string ReadAllText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteAllText(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

int GetModInt(const std::string& car, const std::string& key, int defaultVal = 0) {
    return GetPrivateProfileIntA(car.c_str(), key.c_str(), defaultVal, g_modIniPath.c_str());
}

double GetModDouble(const std::string& car, const std::string& key, double defaultVal = 0.0) {
    char buf[64];
    GetPrivateProfileStringA(car.c_str(), key.c_str(), std::to_string(defaultVal).c_str(), buf, 64, g_modIniPath.c_str());
    try {
        return std::stod(buf);
    }
    catch (...) {
        return defaultVal;
    }
}

void SetModInt(const std::string& car, const std::string& key, int val) {
    std::string s = std::to_string(val);
    WritePrivateProfileStringA(car.c_str(), key.c_str(), s.c_str(), g_modIniPath.c_str());
}

void SetModDouble(const std::string& car, const std::string& key, double val) {
    char buf[64];
    sprintf_s(buf, "%.6f", val);
    WritePrivateProfileStringA(car.c_str(), key.c_str(), buf, g_modIniPath.c_str());
}

bool GetIniValue(const std::string& content, const std::string& keyName, double& outValue) {
    std::regex re(R"((?:^|\n)\s*)" + keyName + R"(\s*[:=]?\s*(-?[0-9]*\.?[0-9]+))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(content, m, re)) return false;
    outValue = std::stod(m[1].str());
    return true;
}

bool SetIniValue(std::string& content, const std::string& keyName, double newVal) {
    std::regex re(R"((?:^|\n)\s*)" + keyName + R"(\s*[:=]?\s*(-?[0-9]*\.?[0-9]+))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(content, m, re)) return false;

    char buf[64];
    sprintf_s(buf, "%.6f", newVal);

    size_t startPos = m.position(1);
    size_t len = m.length(1);
    content.replace(startPos, len, buf);

    return true;
}

void InitPathsAndCars() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir = exeDir.substr(0, slash);

    g_carsDir = exeDir + "\\Cars\\";

    std::string dataFolder = exeDir + "\\scripts\\upgrades_data\\";

    CreateDirectoryA((exeDir + "\\scripts").c_str(), NULL);
    CreateDirectoryA(dataFolder.c_str(), NULL);

    g_modIniPath = dataFolder + "ModUpgrades.ini";
    g_bannerImagePath = dataFolder + "banner.png";
    g_soundUpgradePath = dataFolder + "upgrade.wav";
    g_soundClickPath = dataFolder + "click.wav";
    g_soundErrorPath = dataFolder + "error.wav";

    std::string searchPath = g_carsDir + "*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        std::string name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            name != "." && name != "..") {
            g_carList.push_back(name);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

void LoadBannerTexture(LPDIRECT3DDEVICE9 pDevice) {
    if (g_pBannerTexture != nullptr) return;

    D3DXIMAGE_INFO info;
    if (SUCCEEDED(D3DXCreateTextureFromFileExA(
        pDevice,
        g_bannerImagePath.c_str(),
        D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2,
        1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
        D3DX_DEFAULT, D3DX_DEFAULT, 0,
        &info, NULL, &g_pBannerTexture)))
    {
        g_bannerWidth = info.Width;
        g_bannerHeight = info.Height;
    }
}

std::string GetCarIniPath(const std::string& carName) {
    return g_carsDir + carName + "\\car.ini";
}

void ResetAllUpgrades() {
    PlayClickSound();

    int totalRefund = 0;

    // calculate refund
    for (const auto& car : g_carList) {
        int dfTier = GetModInt(car, "DownforceTier", 0);
        int wtTier = GetModInt(car, "WeightTier", 0);
        int unlockedGrip = GetModInt(car, "UnlockedGripLevel", 0);
        int unlockedDrift = GetModInt(car, "UnlockedDriftLevel", 0);

        for (int i = 0; i < dfTier && i < MAX_UPGRADES; i++) {
            totalRefund += UPGRADE_COSTS[i];
        }

        for (int i = 0; i < wtTier && i < MAX_UPGRADES; i++) {
            totalRefund += UPGRADE_COSTS[i];
        }

        for (int i = 1; i <= unlockedGrip && i <= 3; i++) {
            totalRefund += TUNING_COSTS[i];
        }

        for (int i = 1; i <= unlockedDrift && i <= 3; i++) {
            totalRefund += TUNING_COSTS[i];
        }

        // restore original values
        int tuneLvl = GetModInt(car, "TuningLevel", 0);
        if (dfTier > 0 || wtTier > 0 || tuneLvl != 0) {
            std::string path = GetCarIniPath(car);
            std::string content = ReadAllText(path);

            if (!content.empty()) {
                if (dfTier > 0) {
                    double origFront = GetModDouble(car, "OriginalFront", 0.0);
                    double origRear = GetModDouble(car, "OriginalRear", 0.0);
                    SetIniValue(content, "AeroFrontDownforceCoefficient", origFront);
                    SetIniValue(content, "AeroRearDownforceCoefficient", origRear);
                }
                if (wtTier > 0) {
                    double origMass = GetModDouble(car, "OriginalMass", 0.0);
                    SetIniValue(content, "Mass", origMass);
                }
                if (tuneLvl != 0) {
                    double origBias = GetModDouble(car, "OriginalFrontGripBias", 0.0);
                    SetIniValue(content, "FrontGripBias", origBias);
                }
                WriteAllText(path, content);
            }
        }
    }

    if (totalRefund > 0) {
        if (!AddMoney(totalRefund)) {
            PlayErrorSound();
            g_statusMsg = "Reset succeeded, but failed to refund $" + std::to_string(totalRefund) + " to memory.";
            WriteAllText(g_modIniPath, "");
            return;
        }
    }

    WriteAllText(g_modIniPath, "");

    if (totalRefund > 0) {
        g_statusMsg = "All upgrades refunded! $" + std::to_string(totalRefund) + " added back to balance.";
    }
    else {
        g_statusMsg = "All cars reset to vanilla stock values.";
    }
}

bool DeductMoney(int cost) {
    if (!g_addressFound.load()) {
        PlayErrorSound();
        g_statusMsg = "Cannot buy upgrade: Money memory address not found.";
        return false;
    }

    int currentMoney = 0;
    uintptr_t validAddr = 0;
    for (auto& c : candidates) {
        uintptr_t addr = ResolveMoneyAddress(c);
        if (addr && SafeReadInt(addr, currentMoney)) {
            validAddr = addr;
            break;
        }
    }

    if (!validAddr) {
        PlayErrorSound();
        g_statusMsg = "Cannot buy upgrade: Failed to read money from address.";
        return false;
    }

    if (currentMoney < cost) {
        PlayErrorSound();
        g_statusMsg = "Not enough money! Required: $" + std::to_string(cost);
        return false;
    }

    int targetMoney = currentMoney - cost;

    auto startTime = std::chrono::steady_clock::now();
    bool moneyLowered = false;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() < 2000) {
        if (SafeWriteInt(validAddr, targetMoney)) {
            int checkVal = 0;
            if (SafeReadInt(validAddr, checkVal) && checkVal <= targetMoney) {
                moneyLowered = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!moneyLowered) {
        PlayErrorSound();
        g_statusMsg = "Failed to deduct money from game memory.";
        return false;
    }

    return true;
}

void BuyDownforceUpgrade() {
    if (g_selectedCar.empty()) return;

    int tier = GetModInt(g_selectedCar, "DownforceTier", 0);
    if (tier >= MAX_UPGRADES) {
        PlayErrorSound();
        g_statusMsg = "Max Downforce upgrades (5) already purchased.";
        return;
    }

    int cost = UPGRADE_COSTS[tier];
    if (!DeductMoney(cost)) return;

    std::string path = GetCarIniPath(g_selectedCar);
    std::string content = ReadAllText(path);
    if (content.empty()) {
        PlayErrorSound();
        g_statusMsg = "Could not read car.ini for this car.";
        return;
    }

    double origFront = 0, origRear = 0;

    if (tier == 0) {
        if (!GetIniValue(content, "AeroFrontDownforceCoefficient", origFront) ||
            !GetIniValue(content, "AeroRearDownforceCoefficient", origRear)) {
            PlayErrorSound();
            g_statusMsg = "Could not find Aero coefficients in car.ini.";
            return;
        }
        SetModDouble(g_selectedCar, "OriginalFront", origFront);
        SetModDouble(g_selectedCar, "OriginalRear", origRear);
    }
    else {
        origFront = GetModDouble(g_selectedCar, "OriginalFront", 0.0);
        origRear = GetModDouble(g_selectedCar, "OriginalRear", 0.0);
    }

    tier++;

    double newFront = origFront + (tier * 1.0);
    double newRear = origRear + (tier * 1.0);

    SetIniValue(content, "AeroFrontDownforceCoefficient", newFront);
    SetIniValue(content, "AeroRearDownforceCoefficient", newRear);

    if (!WriteAllText(path, content)) {
        PlayErrorSound();
        g_statusMsg = "Could not write car.ini (check file permissions).";
        return;
    }

    SetModInt(g_selectedCar, "DownforceTier", tier);
    SetModDouble(g_selectedCar, "CurrentFront", newFront);
    SetModDouble(g_selectedCar, "CurrentRear", newRear);

    PlayUpgradeSound();
    g_statusMsg = "Downforce Tier " + std::to_string(tier) + " purchased for $" + std::to_string(cost) + "!";
}

void BuyWeightUpgrade() {
    if (g_selectedCar.empty()) return;

    int tier = GetModInt(g_selectedCar, "WeightTier", 0);
    if (tier >= MAX_UPGRADES) {
        PlayErrorSound();
        g_statusMsg = "Max Weight Reduction upgrades (5) already purchased.";
        return;
    }

    int cost = UPGRADE_COSTS[tier];
    if (!DeductMoney(cost)) return;

    std::string path = GetCarIniPath(g_selectedCar);
    std::string content = ReadAllText(path);
    if (content.empty()) {
        PlayErrorSound();
        g_statusMsg = "Could not read car.ini for this car.";
        return;
    }

    double origMass = 0;

    if (tier == 0) {
        if (!GetIniValue(content, "Mass", origMass)) {
            PlayErrorSound();
            g_statusMsg = "Could not find Mass in car.ini.";
            return;
        }
        SetModDouble(g_selectedCar, "OriginalMass", origMass);
    }
    else {
        origMass = GetModDouble(g_selectedCar, "OriginalMass", 1500.0);
    }

    tier++;

    double newMass = origMass - (tier * 50.0);

    if (!SetIniValue(content, "Mass", newMass)) {
        PlayErrorSound();
        g_statusMsg = "Could not find Mass entry to modify in car.ini.";
        return;
    }

    if (!WriteAllText(path, content)) {
        PlayErrorSound();
        g_statusMsg = "Could not write car.ini (check file permissions).";
        return;
    }

    SetModInt(g_selectedCar, "WeightTier", tier);
    SetModDouble(g_selectedCar, "CurrentMass", newMass);

    PlayUpgradeSound();
    g_statusMsg = "Weight Reduction Tier " + std::to_string(tier) + " purchased (-" + std::to_string(tier * 50) + "kg) for $" + std::to_string(cost) + "!";
}

void SetCarTuningLevel(int targetLevel) {
    if (g_selectedCar.empty()) return;

    int currentLvl = GetModInt(g_selectedCar, "TuningLevel", 0);
    int unlockedGrip = GetModInt(g_selectedCar, "UnlockedGripLevel", 0);   // 0 to 3
    int unlockedDrift = GetModInt(g_selectedCar, "UnlockedDriftLevel", 0); // 0 to 3

    if (targetLevel == currentLvl) return;

    int requiredCost = 0;

    if (targetLevel < 0) { 
        int neededGrip = abs(targetLevel);
        if (neededGrip > unlockedGrip) {
            requiredCost = TUNING_COSTS[neededGrip];
        }
    }
    else if (targetLevel > 0) { 
        int neededDrift = targetLevel;
        if (neededDrift > unlockedDrift) {
            requiredCost = TUNING_COSTS[neededDrift];
        }
    }

    if (requiredCost > 0) {
        if (!DeductMoney(requiredCost)) return;

        if (targetLevel < 0) {
            SetModInt(g_selectedCar, "UnlockedGripLevel", abs(targetLevel));
        }
        else {
            SetModInt(g_selectedCar, "UnlockedDriftLevel", targetLevel);
        }
    }

    std::string path = GetCarIniPath(g_selectedCar);
    std::string content = ReadAllText(path);
    if (content.empty()) {
        PlayErrorSound();
        g_statusMsg = "Could not read car.ini to apply tuning.";
        return;
    }

    double origBias = GetModDouble(g_selectedCar, "OriginalFrontGripBias", -999.0);
    if (origBias == -999.0) {
        if (!GetIniValue(content, "FrontGripBias", origBias)) {
            origBias = 0.0;
        }
        SetModDouble(g_selectedCar, "OriginalFrontGripBias", origBias);
    }

    double biasOffset = -(targetLevel * 0.05);
    double newBias = origBias + biasOffset;

    if (!SetIniValue(content, "FrontGripBias", newBias)) {
        content += "\nFrontGripBias=" + std::to_string(newBias) + "\n";
    }

    if (WriteAllText(path, content)) {
        SetModInt(g_selectedCar, "TuningLevel", targetLevel);
        PlayUpgradeSound();
        if (requiredCost > 0) {
            g_statusMsg = "Tuning level purchased for $" + std::to_string(requiredCost) + "!";
        }
        else {
            g_statusMsg = "Tuning adjusted to level " + std::to_string(targetLevel);
        }
    }
    else {
        PlayErrorSound();
        g_statusMsg = "Failed to write car.ini during tuning update.";
    }
}

// D3D9 bullshit
typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9);
typedef HRESULT(APIENTRY* Reset_t)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);

static EndScene_t oEndScene = nullptr;
static Reset_t    oReset = nullptr;
static WNDPROC    oWndProc = nullptr;
static HWND       g_gameHwnd = nullptr;
static bool       g_imguiInitialized = false;
static bool       g_showMenu = true;

HRESULT APIENTRY hkEndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY hkReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pParams);
LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void HookVTableEntry(void* pInterface, int index, void* hook, void** original) {
    void** vtable = *reinterpret_cast<void***>(pInterface);
    DWORD oldProtect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect);
    *original = vtable[index];
    vtable[index] = hook;
    DWORD tmp;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &tmp);
}

bool InstallD3D9Hooks() {
    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) return false;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "DummyD3D9HookWnd";
    RegisterClassExA(&wc);
    HWND dummyHwnd = CreateWindowExA(0, "DummyD3D9HookWnd", "dummy", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dummyHwnd;

    IDirect3DDevice9* pDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyHwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDevice);

    if (FAILED(hr)) {
        pD3D->Release();
        DestroyWindow(dummyHwnd);
        UnregisterClassA("DummyD3D9HookWnd", wc.hInstance);
        return false;
    }

    HookVTableEntry(pDevice, 42, &hkEndScene, (void**)&oEndScene);
    HookVTableEntry(pDevice, 16, &hkReset, (void**)&oReset);

    pDevice->Release();
    pD3D->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassA("DummyD3D9HookWnd", wc.hInstance);
    return true;
}

void RenderOverlayUI(LPDIRECT3DDEVICE9 pDevice) {
    LoadBannerTexture(pDevice);

    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Car Upgrades", &g_showMenu);

    // current money read
    if (g_addressFound.load())
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "Current Cash: $%d", g_currentMoney.load());
    else
        ImGui::TextDisabled("Current Cash: (address not found)");

    ImGui::Separator();
    ImGui::TextUnformatted("---- Select Car ----");

    if (g_carList.empty()) {
        ImGui::TextDisabled("No cars found in Cars\\ directory.");
    }
    else {
        if (ImGui::BeginCombo("##selectcar", g_selectedCar.c_str())) {
            for (auto& car : g_carList) {
                bool isSelected = (car == g_selectedCar);
                if (ImGui::Selectable(car.c_str(), isSelected)) {
                    PlayClickSound();
                    g_selectedCar = car;
                    g_statusMsg.clear();
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // downforce upgrade
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ Downforce Upgrades ]");
        int dfTier = GetModInt(g_selectedCar, "DownforceTier", 0);
        ImGui::Text("Tier: %d/%d", dfTier, MAX_UPGRADES);

        if (dfTier < MAX_UPGRADES) {
            ImGui::Text("Next Tier Cost: $%d", UPGRADE_COSTS[dfTier]);
        }
        else {
            ImGui::TextDisabled("Next Tier Cost: MAXED OUT");
        }

        if (dfTier == 0) {
            std::string content = ReadAllText(GetCarIniPath(g_selectedCar));
            double front = 0, rear = 0;
            bool okF = GetIniValue(content, "AeroFrontDownforceCoefficient", front);
            bool okR = GetIniValue(content, "AeroRearDownforceCoefficient", rear);
            if (okF && okR)
                ImGui::Text("Orig F/R: %.2f/%.2f | No aero mods applied.", front, rear);
            else
                ImGui::TextUnformatted("Aero Front/Rear: not found in car.ini");
        }
        else {
            double origF = GetModDouble(g_selectedCar, "OriginalFront");
            double origR = GetModDouble(g_selectedCar, "OriginalRear");
            double curF = GetModDouble(g_selectedCar, "CurrentFront");
            double curR = GetModDouble(g_selectedCar, "CurrentRear");
            ImGui::Text("Orig: %.2f/%.2f  ->  Cur: %.2f/%.2f", origF, origR, curF, curR);
        }

        if (ImGui::Button("Buy Downforce Upgrade")) {
            BuyDownforceUpgrade();
        }

        // weight reduction
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ Weight Reduction Upgrades ]");
        int wtTier = GetModInt(g_selectedCar, "WeightTier", 0);
        ImGui::Text("Tier: %d/%d", wtTier, MAX_UPGRADES);

        if (wtTier < MAX_UPGRADES) {
            ImGui::Text("Next Tier Cost: $%d", UPGRADE_COSTS[wtTier]);
        }
        else {
            ImGui::TextDisabled("Next Tier Cost: MAXED OUT");
        }

        if (wtTier == 0) {
            std::string content = ReadAllText(GetCarIniPath(g_selectedCar));
            double mass = 0;
            if (GetIniValue(content, "Mass", mass))
                ImGui::Text("Orig Mass: %.1f kg | No weight mods applied.", mass);
            else
                ImGui::TextUnformatted("Mass: not found in car.ini");
        }
        else {
            double origMass = GetModDouble(g_selectedCar, "OriginalMass");
            double curMass = GetModDouble(g_selectedCar, "CurrentMass");
            ImGui::Text("Orig Mass: %.1f  ->  Cur Mass: %.1f (-%d kg)", origMass, curMass, wtTier * 50);
        }

        if (ImGui::Button("Buy Weight Upgrade")) {
            BuyWeightUpgrade();
        }

        // tuning
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[ Handling Tuning ]");

        int currentTune = GetModInt(g_selectedCar, "TuningLevel", 0);
        int unlockedGrip = GetModInt(g_selectedCar, "UnlockedGripLevel", 0);
        int unlockedDrift = GetModInt(g_selectedCar, "UnlockedDriftLevel", 0);

        // visual shit
        float visualProgress = (currentTune + 3) / 6.0f; // Maps -3..+3 to 0.0..1.0
        char overlayBuf[64];
        if (currentTune < 0) sprintf_s(overlayBuf, "GRIP LEVEL %d (+%.2f Bias)", abs(currentTune), -(currentTune * 0.05));
        else if (currentTune > 0) sprintf_s(overlayBuf, "DRIFT LEVEL %d (%.2f Bias)", currentTune, -(currentTune * 0.05));
        else sprintf_s(overlayBuf, "STOCK (0.00 Bias)");

        ImGui::ProgressBar(visualProgress, ImVec2(-1.0f, 0.0f), overlayBuf);

        // buttons
        bool canGripMore = (currentTune > -3);
        bool canDriftMore = (currentTune < 3);

        if (!canGripMore) ImGui::BeginDisabled();
        int nextGripTarget = currentTune - 1;
        int nextGripCost = (nextGripTarget < 0 && abs(nextGripTarget) > unlockedGrip) ? TUNING_COSTS[abs(nextGripTarget)] : 0;

        char gripBtnLabel[64];
        if (nextGripCost > 0) sprintf_s(gripBtnLabel, "< Grip Lvl %d ($%d)", abs(nextGripTarget), nextGripCost);
        else sprintf_s(gripBtnLabel, "< Grip Lvl %d", abs(nextGripTarget));

        if (ImGui::Button(gripBtnLabel, ImVec2(180, 0))) {
            SetCarTuningLevel(nextGripTarget);
        }
        if (!canGripMore) ImGui::EndDisabled();

        ImGui::SameLine();

        if (!canDriftMore) ImGui::BeginDisabled();
        int nextDriftTarget = currentTune + 1;
        int nextDriftCost = (nextDriftTarget > 0 && nextDriftTarget > unlockedDrift) ? TUNING_COSTS[nextDriftTarget] : 0;

        char driftBtnLabel[64];
        if (nextDriftCost > 0) sprintf_s(driftBtnLabel, "Drift Lvl %d ($%d) >", nextDriftTarget, nextDriftCost);
        else sprintf_s(driftBtnLabel, "Drift Lvl %d >", nextDriftTarget);

        if (ImGui::Button(driftBtnLabel, ImVec2(180, 0))) {
            SetCarTuningLevel(nextDriftTarget);
        }
        if (!canDriftMore) ImGui::EndDisabled();

        ImGui::TextDisabled("Unlocked Grip: Lvl %d/3 | Unlocked Drift: Lvl %d/3", unlockedGrip, unlockedDrift);

        // refund!!!!!
        ImGui::Separator();
        if (ImGui::Button("Refund all upgrades")) {
            ResetAllUpgrades();
        }

        if (!g_statusMsg.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", g_statusMsg.c_str());
        }
    }

    // banner (looks like shit)
    if (g_pBannerTexture != nullptr) {
        ImGui::Separator();
        float scaleFactor = 0.8f; // size
        float imageWidth = ImGui::GetContentRegionAvail().x * scaleFactor;
        float aspect = (float)g_bannerHeight / (float)g_bannerWidth;
        float imageHeight = imageWidth * aspect;

        float windowWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (windowWidth - imageWidth) * 0.5f);

        ImGui::Image((void*)g_pBannerTexture, ImVec2(imageWidth, imageHeight));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Press INSERT to toggle this menu");
    ImGui::End();
}

HRESULT APIENTRY hkEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!g_imguiInitialized) {
        D3DDEVICE_CREATION_PARAMETERS params;
        pDevice->GetCreationParameters(&params);
        g_gameHwnd = params.hFocusWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(g_gameHwnd);
        ImGui_ImplDX9_Init(pDevice);

        oWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

        InitPathsAndCars();

        g_imguiInitialized = true;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_showMenu) {
        RenderOverlayUI(pDevice);
    }

    ImGui::EndFrame();
    ImGui::Render();

    if (g_showMenu) {
        IDirect3DStateBlock9* stateBlock = nullptr;
        if (SUCCEEDED(pDevice->CreateStateBlock(D3DSBT_ALL, &stateBlock))) {
            stateBlock->Capture();
        }

        IDirect3DBaseTexture9* savedTextures[8] = { nullptr };
        for (DWORD i = 0; i < 8; ++i) {
            pDevice->GetTexture(i, &savedTextures[i]);
        }

        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

        pDevice->SetTexture(0, nullptr);
        pDevice->SetPixelShader(nullptr);
        pDevice->SetVertexShader(nullptr);

        if (stateBlock) {
            stateBlock->Apply();
            stateBlock->Release();
        }

        for (DWORD i = 0; i < 8; ++i) {
            pDevice->SetTexture(i, savedTextures[i]);
            if (savedTextures[i]) {
                savedTextures[i]->Release();
            }
        }
    }

    return oEndScene(pDevice);
}

HRESULT APIENTRY hkReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pParams) {
    if (g_pBannerTexture) {
        g_pBannerTexture->Release();
        g_pBannerTexture = nullptr;
    }
    if (g_imguiInitialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    HRESULT hr = oReset(pDevice, pParams);
    if (g_imguiInitialized && SUCCEEDED(hr)) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return hr;
}

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_imguiInitialized) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        if (msg == WM_KEYUP && wParam == VK_INSERT) {
            g_showMenu = !g_showMenu;
        }

        ImGuiIO& io = ImGui::GetIO();
        bool blockMouse = g_showMenu && io.WantCaptureMouse;
        bool blockKeyboard = g_showMenu && io.WantCaptureKeyboard;

        switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            if (blockMouse) return TRUE;
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (blockKeyboard) return TRUE;
            break;
        }
    }
    return CallWindowProcA(oWndProc, hWnd, msg, wParam, lParam);
}

void HookThread() {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    int attempts = 0;
    while (!InstallD3D9Hooks() && attempts < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        attempts++;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        std::thread(MoneyLoop).detach();
        std::thread(HookThread).detach();
    }
    return TRUE;
}