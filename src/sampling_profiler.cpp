// Sampling profiler for TrackMania's render thread (standalone headset builds).
//
// On a Quest the render thread kept one core busy under Box64 while the GPU
// idled, so the frame rate depends on where that thread spends its time. A
// helper thread suspends it every 2 ms, reads its instruction pointer and the
// top of its stack, and resumes it at once; everything that allocates or
// logs happens only while the render thread runs, so it cannot deadlock on
// a lock the suspended thread holds.

#include "sampling_profiler.h"

#include "log.h"
#include "runtime_paths.h"

#include <Windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
constexpr DWORD kSampleIntervalMilliseconds = 2;
constexpr ULONGLONG kReportMilliseconds = 15000;
constexpr size_t kStackWordsScanned = 512;

struct ModuleRange {
    uintptr_t begin;
    uintptr_t end;
    std::string name;
    std::vector<std::pair<uint32_t, std::string>> exports;  // sorted by RVA
};

// Exported functions of a loaded module, so samples in system code can be
// named by the nearest export below them.
std::vector<std::pair<uint32_t, std::string>> ReadExports(uintptr_t base) {
    std::vector<std::pair<uint32_t, std::string>> exports;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return exports;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return exports;
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return exports;
    const auto* table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + table->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + table->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + table->AddressOfFunctions);
    for (DWORD index = 0; index < table->NumberOfNames; ++index) {
        const DWORD rva = functions[ordinals[index]];
        // Forwarded exports point into the export directory, not at code.
        if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size) continue;
        exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[index]));
    }
    std::sort(exports.begin(), exports.end());
    return exports;
}

bool IsSystemModule(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "ntdll.dll" || lower == "win32u.dll" || lower == "kernel32.dll" || lower == "kernelbase.dll" ||
           lower == "msvcrt.dll" || lower == "ucrtbase.dll" || lower == "user32.dll" || lower == "ws2_32.dll";
}

std::string ExportName(const ModuleRange& module, uintptr_t address) {
    const auto rva = static_cast<uint32_t>(address - module.begin);
    auto it = std::upper_bound(module.exports.begin(), module.exports.end(), rva,
                               [](uint32_t value, const auto& entry) { return value < entry.first; });
    if (it == module.exports.begin()) return module.name;
    return module.name + "!" + std::prev(it)->second;
}

std::vector<ModuleRange> SnapshotModules() {
    std::vector<ModuleRange> modules;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return modules;
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32First(snapshot, &entry); more; more = Module32Next(snapshot, &entry)) {
        const auto begin = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        modules.push_back({begin, begin + entry.modBaseSize, entry.szModule, {}});
        if (IsSystemModule(entry.szModule)) modules.back().exports = ReadExports(begin);
    }
    CloseHandle(snapshot);
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
    return modules;
}

const ModuleRange* FindModule(const std::vector<ModuleRange>& modules, uintptr_t address) {
    auto it = std::upper_bound(modules.begin(), modules.end(), address,
                               [](uintptr_t value, const ModuleRange& module) { return value < module.begin; });
    if (it == modules.begin()) return nullptr;
    --it;
    return address < it->end ? &*it : nullptr;
}

bool HasName(const ModuleRange* module, const char* lowerName) {
    if (!module) return false;
    std::string name = module->name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == lowerName;
}

bool IsCallerModule(const ModuleRange* module) {
    return HasName(module, "tmforever.exe") || HasName(module, "tmfoxr.dll");
}

std::string Hex(uintptr_t value) {
    char text[20];
    sprintf_s(text, "0x%lx", static_cast<unsigned long>(value));
    return text;
}

std::string Ranked(const std::map<std::string, uint32_t>& counts, uint32_t samples, size_t limit) {
    std::vector<std::pair<uint32_t, std::string>> ranked;
    for (const auto& [name, count] : counts) ranked.emplace_back(count, name);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string report;
    for (size_t index = 0; index < ranked.size() && index < limit; ++index) {
        char text[32];
        sprintf_s(text, " %.1f%%", 100.0 * ranked[index].first / std::max<uint32_t>(samples, 1));
        report += (report.empty() ? "" : ", ") + ranked[index].second + text;
    }
    return report;
}

HANDLE g_renderThread = nullptr;

DWORD WINAPI ProfilerThread(void*) {
    std::vector<ModuleRange> modules = SnapshotModules();
    std::map<std::string, uint32_t> counts;
    // Where time is spent, including the call sites in TrackMania and the mod
    // (as RVAs: the mod's resolve with llvm-addr2line against its image base).
    std::map<std::string, uint32_t> functions;
    std::map<std::string, uint32_t> sites;
    uint32_t samples = 0;
    uint32_t failures = 0;
    std::array<uint32_t, kStackWordsScanned> stack{};
    ULONGLONG lastReport = GetTickCount64();
    for (;;) {
        Sleep(kSampleIntervalMilliseconds);
        if (SuspendThread(g_renderThread) == static_cast<DWORD>(-1)) break;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        const bool haveContext = GetThreadContext(g_renderThread, &context) != FALSE;
        SIZE_T stackBytes = 0;
        if (haveContext) {
            // A read that runs past the end of the stack fails as a whole, so
            // fall back to a smaller window.
            for (SIZE_T size = sizeof(stack); size >= 64 && !stackBytes; size /= 4) {
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Esp), stack.data(),
                                       size, &stackBytes)) stackBytes = 0;
            }
        }
        ResumeThread(g_renderThread);
        if (!haveContext) {
            ++failures;
            continue;
        }
        ++samples;
        const ModuleRange* at = FindModule(modules, context.Eip);
        std::string key = at ? at->name : "outside any module";
        std::string site;
        if (IsCallerModule(at)) {
            site = at->name + "+" + Hex(context.Eip - at->begin);
        } else {
            // The nearest return address into TrackMania or the mod tells who
            // called Direct3D or system code.
            std::string caller = "unknown caller";
            for (size_t index = 0; index < stackBytes / sizeof(uint32_t); ++index) {
                const ModuleRange* candidate = FindModule(modules, stack[index]);
                if (IsCallerModule(candidate)) {
                    caller = candidate->name;
                    site = candidate->name + "+" + Hex(stack[index] - candidate->begin);
                    break;
                }
            }
            key += " via " + caller;
            if (at && !at->exports.empty()) ++functions[ExportName(*at, context.Eip) + " via " + caller];
        }
        ++counts[key];
        if (!site.empty()) ++sites[site];

        const ULONGLONG now = GetTickCount64();
        if (now - lastReport < kReportMilliseconds) continue;
        log::Info("Render thread profile (" + std::to_string(samples) + " samples in " +
            std::to_string((now - lastReport) / 1000) + " s, " + std::to_string(failures) +
            " failed): " + Ranked(counts, samples, 12) + ".");
        log::Info("Render thread profile, system functions: " + Ranked(functions, samples, 15) + ".");
        log::Info("Render thread profile, call sites: " + Ranked(sites, samples, 25) + ".");
        counts.clear();
        functions.clear();
        sites.clear();
        samples = 0;
        failures = 0;
        lastReport = now;
        modules = SnapshotModules();
    }
    log::Warn("Render thread profiler stopped: SuspendThread failed.");
    return 0;
}
} // namespace

void StartRenderThreadProfilerIfRequested() {
    if (g_renderThread) return;
    if (GetFileAttributesW(ModuleFilePath(L"TMFOXR-profile.txt").c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_renderThread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        log::Warn("Render thread profiler unavailable: could not open the render thread.");
        g_renderThread = nullptr;
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &ProfilerThread, nullptr, 0, nullptr);
    if (!thread) {
        log::Warn("Render thread profiler unavailable: could not start its thread.");
        return;
    }
    CloseHandle(thread);
    log::Info("Render thread profiler started (TMFOXR-profile.txt); it reports every 15 s.");
}
} // namespace tmoxr
