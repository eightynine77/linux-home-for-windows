// ==WindhawkMod==
// @id tilde-home-paths
// @name Tilde Home Paths
// @description Expand ~/ and ~\ to the current user's Windows profile in Explorer address bar and common filesystem path operations.
// @version 1.0.0
// @author ChatGPT
// @include explorer.exe
// @include cmd.exe
// @include powershell.exe
// @include pwsh.exe
// @architecture x86-64
// @compilerOptions -lcomctl32 -lshell32 -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Tilde Home Paths

this mod adds input handling for the classic Windows File Explorer address bar. It expands a leading `~`, `~/`, or `~\\` in the edit control before Explorer decides whether the text is a URL or a filesystem location. When typing `~`, the edit text is rewritten to the absolute profile path immediately; typing a slash and subfolder afterward continues as a normal Windows path. This happens before Enter, so Explorer does not get the chance to misclassify a bare tilde as a web address.

The mod hooks common filesystem APIs in Explorer, CMD, Windows PowerShell, and PowerShell 7+ as a fallback for paths passed to those APIs.

Examples:
- Explorer address bar: `~`, `~/Downloads`, `~\\Downloads`
- CMD path-taking commands: `cd ~\\Downloads`, `dir ~/Downloads`

Limitations:
- Explorer address-bar input and the filesystem API hooks are separate mechanisms. Autocomplete suggestions depend on the exact Windows 10 build and Explorer controls; this mod cannot guarantee they will look identical to `%USERPROFILE%` suggestions.
- CMD has no native tilde expansion. The API hooks cover built-in commands only when they pass the path through a hooked Windows API; they do not rewrite every command-line argument or every third-party program.
- PowerShell already has its own path expansion rules. Other terminals rely on the shell running inside them (for example, Bash has its own `~` expansion); this mod doesn't provide universal expansion across arbitrary shells.
- This source has not been compiled or runtime-tested on the user's PC. Disable the mod from Windhawk if it misbehaves, and share the compiler log if it fails to build.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <cstddef>

using SHParseDisplayName_t = HRESULT(WINAPI*)(PCWSTR, IBindCtx*, PIDLIST_ABSOLUTE*, SFGAOF, SFGAOF*);
using SHCreateItemFromParsingName_t = HRESULT(WINAPI*)(PCWSTR, IBindCtx*, REFIID, void**);
using ILCreateFromPathW_t = PIDLIST_ABSOLUTE(WINAPI*)(PCWSTR);
using SetCurrentDirectoryW_t = decltype(&SetCurrentDirectoryW);
using GetFullPathNameW_t = decltype(&GetFullPathNameW);
using GetFileAttributesW_t = decltype(&GetFileAttributesW);
using GetFileAttributesExW_t = decltype(&GetFileAttributesExW);
using FindFirstFileW_t = decltype(&FindFirstFileW);
using FindFirstFileExW_t = decltype(&FindFirstFileExW);
using CreateFileW_t = decltype(&CreateFileW);
using CreateFile2_t = HANDLE(WINAPI*)(PCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using CreateDirectoryW_t = decltype(&CreateDirectoryW);
using RemoveDirectoryW_t = decltype(&RemoveDirectoryW);
using DeleteFileW_t = decltype(&DeleteFileW);
using CopyFileW_t = decltype(&CopyFileW);
using MoveFileW_t = decltype(&MoveFileW);
using MoveFileExW_t = decltype(&MoveFileExW);
using ShellExecuteW_t = decltype(&ShellExecuteW);
using ShellExecuteExW_t = decltype(&ShellExecuteExW);
using CreateWindowExW_t = decltype(&CreateWindowExW);

static SHParseDisplayName_t g_SHParseDisplayName_Original;
static SHCreateItemFromParsingName_t g_SHCreateItemFromParsingName_Original;
static ILCreateFromPathW_t g_ILCreateFromPathW_Original;
static SetCurrentDirectoryW_t g_SetCurrentDirectoryW_Original;
static GetFullPathNameW_t g_GetFullPathNameW_Original;
static GetFileAttributesW_t g_GetFileAttributesW_Original;
static GetFileAttributesExW_t g_GetFileAttributesExW_Original;
static FindFirstFileW_t g_FindFirstFileW_Original;
static FindFirstFileExW_t g_FindFirstFileExW_Original;
static CreateFileW_t g_CreateFileW_Original;
static CreateFile2_t g_CreateFile2_Original;
static CreateDirectoryW_t g_CreateDirectoryW_Original;
static RemoveDirectoryW_t g_RemoveDirectoryW_Original;
static DeleteFileW_t g_DeleteFileW_Original;
static CopyFileW_t g_CopyFileW_Original;
static MoveFileW_t g_MoveFileW_Original;
static MoveFileExW_t g_MoveFileExW_Original;
static ShellExecuteW_t g_ShellExecuteW_Original;
static ShellExecuteExW_t g_ShellExecuteExW_Original;
static CreateWindowExW_t g_CreateWindowExW_Original;

static constexpr UINT_PTR kTildeEditSubclassId = 0x54494C44;
static UINT g_subclassRegisteredMsg = RegisterWindowMessageW(L"Windhawk_SetWindowSubclassFromAnyThread_tilde-home-paths");
static thread_local bool g_rewritingEditText = false;
static volatile LONG g_shuttingDown = 0;

struct SET_WINDOW_SUBCLASS_FROM_ANY_THREAD_PARAM {
    SUBCLASSPROC pfnSubclass;
    UINT_PTR uIdSubclass;
    DWORD_PTR dwRefData;
    BOOL result;
};

static LRESULT CALLBACK CallWndProcForWindowSubclass(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message == g_subclassRegisteredMsg && message->lParam) {
            auto* param = reinterpret_cast<SET_WINDOW_SUBCLASS_FROM_ANY_THREAD_PARAM*>(message->lParam);
            if (message->wParam) {
                param->result = SetWindowSubclass(message->hwnd, param->pfnSubclass, param->uIdSubclass, param->dwRefData);
            } else {
                param->result = RemoveWindowSubclass(message->hwnd, param->pfnSubclass, param->uIdSubclass);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static BOOL SetWindowSubclassFromAnyThread(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data) {
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    if (!threadId) {
        return FALSE;
    }
    if (threadId == GetCurrentThreadId()) {
        return SetWindowSubclass(hwnd, proc, id, data);
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcForWindowSubclass, nullptr, threadId);
    if (!hook) {
        return FALSE;
    }
    SET_WINDOW_SUBCLASS_FROM_ANY_THREAD_PARAM param{proc, id, data, FALSE};
    SendMessageW(hwnd, g_subclassRegisteredMsg, TRUE, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return param.result;
}

static BOOL RemoveWindowSubclassFromAnyThread(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id) {
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    if (!threadId) {
        return FALSE;
    }
    if (threadId == GetCurrentThreadId()) {
        return RemoveWindowSubclass(hwnd, proc, id);
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcForWindowSubclass, nullptr, threadId);
    if (!hook) {
        return FALSE;
    }
    SET_WINDOW_SUBCLASS_FROM_ANY_THREAD_PARAM param{proc, id, 0, FALSE};
    SendMessageW(hwnd, g_subclassRegisteredMsg, FALSE, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return param.result;
}

static bool IsExplorerProcess() {
    wchar_t imagePath[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, imagePath, ARRAYSIZE(imagePath));
    if (length == 0 || length >= ARRAYSIZE(imagePath)) {
        return false;
    }
    const wchar_t* baseName = imagePath;
    for (const wchar_t* p = imagePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            baseName = p + 1;
        }
    }
    return _wcsicmp(baseName, L"explorer.exe") == 0;
}

static bool TryExpandHomePath(PCWSTR input, std::wstring& expanded) {
    if (!input || input[0] != L'~' ||
        (input[1] != L'\0' && input[1] != L'/' && input[1] != L'\\')) {
        return false;
    }

    DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (!required) {
        return false;
    }
    std::wstring profile(required, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), required);
    if (!copied || copied >= required) {
        return false;
    }
    profile.resize(copied);

    std::wstring result = profile;
    if (input[1] == L'/' || input[1] == L'\\') {
        result.push_back(L'\\');
        for (PCWSTR p = input + 2; *p; ++p) {
            result.push_back(*p == L'/' ? L'\\' : *p);
        }
    }
    expanded.swap(result);
    return true;
}

static PCWSTR ExpandPathIfNeeded(PCWSTR input, std::wstring& storage) {
    return TryExpandHomePath(input, storage) ? storage.c_str() : input;
}

static bool AddExportHook(HMODULE module, LPCSTR exportName, void* hook, void** original) {
    if (!module) {
        Wh_Log(L"Tilde Home Paths: module unavailable for %S", exportName);
        return false;
    }
    FARPROC target = GetProcAddress(module, exportName);
    if (!target) {
        Wh_Log(L"Tilde Home Paths: export unavailable: %S", exportName);
        return false;
    }
    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(target), hook, original)) {
        Wh_Log(L"Tilde Home Paths: hook failed: %S", exportName);
        return false;
    }
    return true;
}

static bool IsExplorerTopLevel(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }
    wchar_t className[128]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) {
        return false;
    }
    return _wcsicmp(className, L"CabinetWClass") == 0 || _wcsicmp(className, L"ExploreWClass") == 0;
}

static bool IsAddressBarEdit(HWND hwnd) {
    wchar_t className[128]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) || _wcsicmp(className, L"Edit") != 0) {
        return false;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || !IsExplorerTopLevel(root)) {
        return false;
    }

    bool hasAddressBand = false;
    bool hasComboAncestor = false;
    for (HWND parent = GetParent(hwnd); parent && parent != root; parent = GetParent(parent)) {
        wchar_t parentClass[128]{};
        if (!GetClassNameW(parent, parentClass, ARRAYSIZE(parentClass))) {
            continue;
        }
        if (_wcsicmp(parentClass, L"Address Band Root") == 0 ||
            _wcsicmp(parentClass, L"Breadcrumb Parent") == 0 ||
            wcsstr(parentClass, L"Address") != nullptr) {
            hasAddressBand = true;
        }
        if (_wcsicmp(parentClass, L"ComboBoxEx32") == 0 || _wcsicmp(parentClass, L"ComboBox") == 0) {
            hasComboAncestor = true;
        }
        if (wcsstr(parentClass, L"Search") != nullptr) {
            return false;
        }
    }

    // The address bar is hosted in the address band on classic Windows 10 Explorer.
    // The combo-box fallback covers builds which use a different address-band child class.
    return hasAddressBand || hasComboAncestor;
}

static bool IsCurrentEditUnderProfile(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0 || length > 32760) {
        return false;
    }
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, current.data(), length + 1);
    if (copied <= 0) {
        return false;
    }
    current.resize(copied);

    std::wstring profile;
    if (!TryExpandHomePath(L"~", profile) || current.size() < profile.size()) {
        return false;
    }
    if (CompareStringOrdinal(current.c_str(), static_cast<int>(profile.size()),
                             profile.c_str(), static_cast<int>(profile.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return current.size() == profile.size() || current[profile.size()] == L'\\' || current[profile.size()] == L'/';
}

static bool ExpandEditText(HWND hwnd, bool includeBareTilde) {
    if (g_rewritingEditText || !IsAddressBarEdit(hwnd)) {
        return false;
    }

    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0 || length > 32760) {
        return false;
    }
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, current.data(), length + 1);
    if (copied <= 0) {
        return false;
    }
    current.resize(copied);
    if (current == L"~" && !includeBareTilde) {
        return false;
    }

    std::wstring expanded;
    if (!TryExpandHomePath(current.c_str(), expanded) || expanded == current) {
        return false;
    }

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(expanded.size()) - static_cast<std::ptrdiff_t>(current.size());

    g_rewritingEditText = true;
    BOOL set = SetWindowTextW(hwnd, expanded.c_str());
    g_rewritingEditText = false;
    if (!set) {
        return false;
    }

    auto adjust = [&](DWORD position) -> DWORD {
        std::ptrdiff_t adjusted = static_cast<std::ptrdiff_t>(position) + delta;
        if (adjusted < 0) adjusted = 0;
        if (adjusted > static_cast<std::ptrdiff_t>(expanded.size())) adjusted = static_cast<std::ptrdiff_t>(expanded.size());
        return static_cast<DWORD>(adjusted);
    };
    SendMessageW(hwnd, EM_SETSEL, adjust(selectionStart), adjust(selectionEnd));
    Wh_Log(L"Tilde Home Paths: expanded Explorer address-bar input");
    return true;
}

static LRESULT CALLBACK AddressBarEditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                                   UINT_PTR subclassId, DWORD_PTR refData) {
    // Once a leading tilde has expanded, normalize subsequently typed forward slashes
    // to Windows separators so `~/Downloads/child` remains a valid path while typing.
    if (message == WM_CHAR && wParam == L'/' && IsCurrentEditUnderProfile(hwnd)) {
        return DefSubclassProc(hwnd, message, L'\\', lParam);
    }

    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        ExpandEditText(hwnd, true);
    }

    LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_CHAR && wParam == L'~') {
        ExpandEditText(hwnd, true);
    } else if (message == WM_CHAR && (wParam == L'/' || wParam == L'\\')) {
        ExpandEditText(hwnd, false);
    } else if (message == WM_PASTE || message == WM_SETTEXT) {
        ExpandEditText(hwnd, true);
    }

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, AddressBarEditSubclassProc, subclassId);
    }
    return result;
}

static void MaybeSubclassAddressBarEdit(HWND hwnd) {
    if (InterlockedCompareExchange(&g_shuttingDown, 0, 0) != 0) {
        return;
    }
    if (IsAddressBarEdit(hwnd)) {
        if (SetWindowSubclassFromAnyThread(hwnd, AddressBarEditSubclassProc, kTildeEditSubclassId, 0)) {
            Wh_Log(L"Tilde Home Paths: subclassed Explorer address-bar edit control (%p)", hwnd);
        }
    }
}

static BOOL CALLBACK EnumExplorerEditWindowsProc(HWND hwnd, LPARAM) {
    MaybeSubclassAddressBarEdit(hwnd);
    return TRUE;
}

static BOOL CALLBACK EnumExplorerWindowsProc(HWND hwnd, LPARAM) {
    if (IsExplorerTopLevel(hwnd)) {
        EnumChildWindows(hwnd, EnumExplorerEditWindowsProc, 0);
    }
    return TRUE;
}

static BOOL CALLBACK EnumExplorerEditWindowsUnsubclassProc(HWND hwnd, LPARAM) {
    if (IsAddressBarEdit(hwnd)) {
        RemoveWindowSubclassFromAnyThread(hwnd, AddressBarEditSubclassProc, kTildeEditSubclassId);
    }
    return TRUE;
}

static BOOL CALLBACK EnumExplorerWindowsUnsubclassProc(HWND hwnd, LPARAM) {
    if (IsExplorerTopLevel(hwnd)) {
        EnumChildWindows(hwnd, EnumExplorerEditWindowsUnsubclassProc, 0);
    }
    return TRUE;
}

static HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    HWND hwnd = g_CreateWindowExW_Original(exStyle, className, windowName, style, x, y, width, height, parent, menu, instance, param);
    if (hwnd && IsExplorerProcess()) {
        MaybeSubclassAddressBarEdit(hwnd);
    }
    return hwnd;
}

static HRESULT WINAPI SHParseDisplayName_Hook(PCWSTR name, IBindCtx* bindContext, PIDLIST_ABSOLUTE* itemIdList,
                                               SFGAOF attributesIn, SFGAOF* attributesOut) {
    std::wstring expanded;
    return g_SHParseDisplayName_Original(ExpandPathIfNeeded(name, expanded), bindContext, itemIdList, attributesIn, attributesOut);
}

static HRESULT WINAPI SHCreateItemFromParsingName_Hook(PCWSTR name, IBindCtx* bindContext, REFIID interfaceId, void** result) {
    std::wstring expanded;
    return g_SHCreateItemFromParsingName_Original(ExpandPathIfNeeded(name, expanded), bindContext, interfaceId, result);
}

static PIDLIST_ABSOLUTE WINAPI ILCreateFromPathW_Hook(PCWSTR path) {
    std::wstring expanded;
    return g_ILCreateFromPathW_Original(ExpandPathIfNeeded(path, expanded));
}

static BOOL WINAPI SetCurrentDirectoryW_Hook(PCWSTR path) {
    std::wstring expanded;
    return g_SetCurrentDirectoryW_Original(ExpandPathIfNeeded(path, expanded));
}

static DWORD WINAPI GetFullPathNameW_Hook(PCWSTR path, DWORD bufferLength, PWSTR buffer, PWSTR* filePart) {
    std::wstring expanded;
    return g_GetFullPathNameW_Original(ExpandPathIfNeeded(path, expanded), bufferLength, buffer, filePart);
}

static DWORD WINAPI GetFileAttributesW_Hook(PCWSTR path) {
    std::wstring expanded;
    return g_GetFileAttributesW_Original(ExpandPathIfNeeded(path, expanded));
}

static BOOL WINAPI GetFileAttributesExW_Hook(PCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information) {
    std::wstring expanded;
    return g_GetFileAttributesExW_Original(ExpandPathIfNeeded(path, expanded), level, information);
}

static HANDLE WINAPI FindFirstFileW_Hook(PCWSTR path, LPWIN32_FIND_DATAW findData) {
    std::wstring expanded;
    return g_FindFirstFileW_Original(ExpandPathIfNeeded(path, expanded), findData);
}

static HANDLE WINAPI FindFirstFileExW_Hook(PCWSTR path, FINDEX_INFO_LEVELS infoLevel, LPVOID findData,
                                           FINDEX_SEARCH_OPS searchOp, LPVOID searchFilter, DWORD flags) {
    std::wstring expanded;
    return g_FindFirstFileExW_Original(ExpandPathIfNeeded(path, expanded), infoLevel, findData, searchOp, searchFilter, flags);
}

static HANDLE WINAPI CreateFileW_Hook(PCWSTR path, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile) {
    std::wstring expanded;
    return g_CreateFileW_Original(ExpandPathIfNeeded(path, expanded), desiredAccess, shareMode, securityAttributes,
                                  creationDisposition, flagsAndAttributes, templateFile);
}

static HANDLE WINAPI CreateFile2_Hook(PCWSTR path, DWORD desiredAccess, DWORD shareMode, DWORD creationDisposition,
                                      LPCREATEFILE2_EXTENDED_PARAMETERS parameters) {
    std::wstring expanded;
    return g_CreateFile2_Original(ExpandPathIfNeeded(path, expanded), desiredAccess, shareMode, creationDisposition, parameters);
}

static BOOL WINAPI CreateDirectoryW_Hook(PCWSTR path, LPSECURITY_ATTRIBUTES securityAttributes) {
    std::wstring expanded;
    return g_CreateDirectoryW_Original(ExpandPathIfNeeded(path, expanded), securityAttributes);
}

static BOOL WINAPI RemoveDirectoryW_Hook(PCWSTR path) {
    std::wstring expanded;
    return g_RemoveDirectoryW_Original(ExpandPathIfNeeded(path, expanded));
}

static BOOL WINAPI DeleteFileW_Hook(PCWSTR path) {
    std::wstring expanded;
    return g_DeleteFileW_Original(ExpandPathIfNeeded(path, expanded));
}

static BOOL WINAPI CopyFileW_Hook(PCWSTR source, PCWSTR destination, BOOL failIfExists) {
    std::wstring expandedSource;
    std::wstring expandedDestination;
    return g_CopyFileW_Original(ExpandPathIfNeeded(source, expandedSource), ExpandPathIfNeeded(destination, expandedDestination), failIfExists);
}

static BOOL WINAPI MoveFileW_Hook(PCWSTR existingName, PCWSTR newName) {
    std::wstring expandedExisting;
    std::wstring expandedNew;
    return g_MoveFileW_Original(ExpandPathIfNeeded(existingName, expandedExisting), ExpandPathIfNeeded(newName, expandedNew));
}

static BOOL WINAPI MoveFileExW_Hook(PCWSTR existingName, PCWSTR newName, DWORD flags) {
    std::wstring expandedExisting;
    std::wstring expandedNew;
    return g_MoveFileExW_Original(ExpandPathIfNeeded(existingName, expandedExisting), ExpandPathIfNeeded(newName, expandedNew), flags);
}

static HINSTANCE WINAPI ShellExecuteW_Hook(HWND hwnd, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters,
                                           LPCWSTR directory, INT showCommand) {
    std::wstring expanded;
    return g_ShellExecuteW_Original(hwnd, operation, ExpandPathIfNeeded(file, expanded), parameters, directory, showCommand);
}

static BOOL WINAPI ShellExecuteExW_Hook(SHELLEXECUTEINFOW* executeInfo) {
    if (!executeInfo) {
        return g_ShellExecuteExW_Original(executeInfo);
    }
    std::wstring expanded;
    PCWSTR originalFile = executeInfo->lpFile;
    PCWSTR newFile = ExpandPathIfNeeded(originalFile, expanded);
    executeInfo->lpFile = newFile;
    BOOL result = g_ShellExecuteExW_Original(executeInfo);
    executeInfo->lpFile = originalFile;
    return result;
}

BOOL Wh_ModInit() {
    InterlockedExchange(&g_shuttingDown, 0);
    HMODULE kernelBase = GetModuleHandleW(L"KernelBase.dll");
    if (!kernelBase) {
        Wh_Log(L"Tilde Home Paths: KernelBase.dll was not loaded");
        return FALSE;
    }

    AddExportHook(kernelBase, "SetCurrentDirectoryW", reinterpret_cast<void*>(SetCurrentDirectoryW_Hook), reinterpret_cast<void**>(&g_SetCurrentDirectoryW_Original));
    AddExportHook(kernelBase, "GetFullPathNameW", reinterpret_cast<void*>(GetFullPathNameW_Hook), reinterpret_cast<void**>(&g_GetFullPathNameW_Original));
    AddExportHook(kernelBase, "GetFileAttributesW", reinterpret_cast<void*>(GetFileAttributesW_Hook), reinterpret_cast<void**>(&g_GetFileAttributesW_Original));
    AddExportHook(kernelBase, "GetFileAttributesExW", reinterpret_cast<void*>(GetFileAttributesExW_Hook), reinterpret_cast<void**>(&g_GetFileAttributesExW_Original));
    AddExportHook(kernelBase, "FindFirstFileW", reinterpret_cast<void*>(FindFirstFileW_Hook), reinterpret_cast<void**>(&g_FindFirstFileW_Original));
    AddExportHook(kernelBase, "FindFirstFileExW", reinterpret_cast<void*>(FindFirstFileExW_Hook), reinterpret_cast<void**>(&g_FindFirstFileExW_Original));
    AddExportHook(kernelBase, "CreateFileW", reinterpret_cast<void*>(CreateFileW_Hook), reinterpret_cast<void**>(&g_CreateFileW_Original));
    AddExportHook(kernelBase, "CreateFile2", reinterpret_cast<void*>(CreateFile2_Hook), reinterpret_cast<void**>(&g_CreateFile2_Original));
    AddExportHook(kernelBase, "CreateDirectoryW", reinterpret_cast<void*>(CreateDirectoryW_Hook), reinterpret_cast<void**>(&g_CreateDirectoryW_Original));
    AddExportHook(kernelBase, "RemoveDirectoryW", reinterpret_cast<void*>(RemoveDirectoryW_Hook), reinterpret_cast<void**>(&g_RemoveDirectoryW_Original));
    AddExportHook(kernelBase, "DeleteFileW", reinterpret_cast<void*>(DeleteFileW_Hook), reinterpret_cast<void**>(&g_DeleteFileW_Original));
    AddExportHook(kernelBase, "CopyFileW", reinterpret_cast<void*>(CopyFileW_Hook), reinterpret_cast<void**>(&g_CopyFileW_Original));
    AddExportHook(kernelBase, "MoveFileW", reinterpret_cast<void*>(MoveFileW_Hook), reinterpret_cast<void**>(&g_MoveFileW_Original));
    AddExportHook(kernelBase, "MoveFileExW", reinterpret_cast<void*>(MoveFileExW_Hook), reinterpret_cast<void**>(&g_MoveFileExW_Original));

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");
    AddExportHook(shell32, "SHParseDisplayName", reinterpret_cast<void*>(SHParseDisplayName_Hook), reinterpret_cast<void**>(&g_SHParseDisplayName_Original));
    AddExportHook(shell32, "SHCreateItemFromParsingName", reinterpret_cast<void*>(SHCreateItemFromParsingName_Hook), reinterpret_cast<void**>(&g_SHCreateItemFromParsingName_Original));
    AddExportHook(shell32, "ILCreateFromPathW", reinterpret_cast<void*>(ILCreateFromPathW_Hook), reinterpret_cast<void**>(&g_ILCreateFromPathW_Original));
    AddExportHook(shell32, "ShellExecuteW", reinterpret_cast<void*>(ShellExecuteW_Hook), reinterpret_cast<void**>(&g_ShellExecuteW_Original));
    AddExportHook(shell32, "ShellExecuteExW", reinterpret_cast<void*>(ShellExecuteExW_Hook), reinterpret_cast<void**>(&g_ShellExecuteExW_Original));

    if (IsExplorerProcess()) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        AddExportHook(user32, "CreateWindowExW", reinterpret_cast<void*>(CreateWindowExW_Hook), reinterpret_cast<void**>(&g_CreateWindowExW_Original));
        EnumWindows(EnumExplorerWindowsProc, 0);
    }

    return TRUE;
}

void Wh_ModBeforeUninit() {
    if (IsExplorerProcess()) {
        InterlockedExchange(&g_shuttingDown, 1);
        EnumWindows(EnumExplorerWindowsUnsubclassProc, 0);
    }
}
