// =============================================================================
//  check7z.cpp — 扫描本机自带的 7z 解压程序
//
//  用途：统计“有多少软件自带 7z 解压程序”，以及它们的位置、版本和体积。
//  平台：Windows / C++17。/utf-8 编译（源码为 UTF-8 无 BOM）。
//
//  v2.0.0 相对初版的主要改动：
//    * 全程宽字符路径 + UTF-8 控制台输出，中文/日文路径不再乱码；
//    * 多线程目录遍历，跳过 junction / 符号链接，避免死循环与重复计数；
//    * 注册表读取支持 64/32 位视图、REG_EXPAND_SZ、App Paths；
//    * 按“软件目录”聚合结果，显示版本号与体积，而不是只丢一堆路径；
//    * 命令行参数：--root / --exclude / --json / --csv / --threads 等；
//    * 支持 Ctrl+C 中断，并输出已完成的部分结果；
//    * 不再把 WinRAR / Bandizip 误算成“7z”（它们只是支持 7z 格式）。
// =============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "2.0.0";

// -----------------------------------------------------------------------------
// 基础工具
// -----------------------------------------------------------------------------

std::atomic<bool> g_interrupted{ false };
std::atomic<bool> g_progressVisible{ false };
std::mutex g_consoleMutex;
bool g_progressDirty = false;   // 受 g_consoleMutex 保护：进度行是否还没换行

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}

bool IsConsoleHandle(HANDLE h) {
    DWORD mode = 0;
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::string PathUtf8(const fs::path& p) { return ToUtf8(p.native()); }

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::string GroupDigits(uint64_t value) {
    std::string s = std::to_string(value);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string FormatSize(uint64_t bytes) {
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[64];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

std::string LocalTimeIso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    const std::tm* p = std::localtime(&t);
    if (p) tm = *p;
#endif
    char buf[40] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

// -----------------------------------------------------------------------------
// 7z 可执行文件识别
// -----------------------------------------------------------------------------

bool IsSevenZipExe(const fs::path& p) {
    const std::wstring name = p.filename().native();
    if (name.size() < 5 || name.size() > 8) return false;
    if (name[0] != L'7') return false;

    wchar_t buf[16] = {};
    for (size_t i = 0; i < name.size(); ++i) buf[i] = static_cast<wchar_t>(std::towlower(name[i]));

    return std::wcscmp(buf, L"7z.exe") == 0 ||
           std::wcscmp(buf, L"7zg.exe") == 0 ||
           std::wcscmp(buf, L"7zfm.exe") == 0 ||
           std::wcscmp(buf, L"7za.exe") == 0 ||
           std::wcscmp(buf, L"7zr.exe") == 0;
}

// 规范化路径用于去重：解析 . / .. / 短文件名，并统一小写。
std::wstring CanonicalKey(const fs::path& p) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(p, ec);
    std::wstring s = ec ? p.native() : canonical.native();
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring FileVersion(const fs::path& p) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(p.c_str(), &dummy);
    if (size == 0) return {};

    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(p.c_str(), 0, size, buf.data())) return {};

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || ffi == nullptr) return {};
    if (ffi->dwSignature != 0xFEEF04BDu) return {};

    const unsigned ms = ffi->dwFileVersionMS;
    const unsigned ls = ffi->dwFileVersionLS;
    wchar_t out[64] = {};
    std::swprintf(out, 64, L"%u.%u.%u.%u", ms >> 16, ms & 0xFFFFu, ls >> 16, ls & 0xFFFFu);
    return out;
}

// -----------------------------------------------------------------------------
// 注册表：只认 7-Zip 自己，不再把 WinRAR / Bandizip 算作 7z
// -----------------------------------------------------------------------------

std::wstring RegValue(HKEY root, const wchar_t* sub, const wchar_t* value, REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return {};

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        RegCloseKey(key);
        return {};
    }

    std::wstring buf(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, value, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buf.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return {};

    buf.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
    while (!buf.empty() && (buf.back() == L'\0' || buf.back() == L' ')) buf.pop_back();

    if (type == REG_EXPAND_SZ) {
        const DWORD need = ExpandEnvironmentStringsW(buf.c_str(), nullptr, 0);
        if (need > 1) {
            std::wstring expanded(need, L'\0');
            if (ExpandEnvironmentStringsW(buf.c_str(), expanded.data(), need) != 0) {
                expanded.resize(need - 1);
                buf = std::move(expanded);
            }
        }
    }

    if (buf.size() >= 2 && buf.front() == L'"' && buf.back() == L'"') {
        buf = buf.substr(1, buf.size() - 2);
    }
    return buf;
}

void AddUnique(std::vector<fs::path>& out, const fs::path& path) {
    const std::wstring key = Lower(path.native());
    for (const auto& existing : out) {
        if (Lower(existing.native()) == key) return;
    }
    out.push_back(path);
}

void ProbeRegistry(std::vector<fs::path>& out) {
    for (const REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
        const std::wstring dir = RegValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip", L"Path", view);
        if (!dir.empty()) AddUnique(out, fs::path(dir) / L"7z.exe");
    }

    const std::wstring userDir = RegValue(HKEY_CURRENT_USER, L"SOFTWARE\\7-Zip", L"Path", 0);
    if (!userDir.empty()) AddUnique(out, fs::path(userDir) / L"7z.exe");

    struct AppPath { HKEY root; REGSAM view; };
    const AppPath appPaths[] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  0 },
    };
    for (const auto& ap : appPaths) {
        const std::wstring exe = RegValue(
            ap.root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\7z.exe", L"", ap.view);
        if (!exe.empty()) AddUnique(out, fs::path(exe));
    }
}

// -----------------------------------------------------------------------------
// 目录扫描：共享队列 + pending 计数的工作线程池
// -----------------------------------------------------------------------------

struct Hit {
    fs::path path;
    fs::path owner;   // 所属“软件目录”
    uint64_t size = 0;
};

struct Stats {
    uint64_t directories = 0;
    uint64_t files = 0;
    uint64_t denied = 0;
    double seconds = 0.0;
};

struct ExcludeRules {
    std::vector<std::wstring> names;   // 精确匹配目录名
    std::vector<std::wstring> paths;   // 路径片段（小写）
};

class Scanner {
public:
    Scanner(ExcludeRules rules, int maxDepth, bool verbose)
        : rules_(std::move(rules)), maxDepth_(maxDepth), verbose_(verbose) {}

    void AddRoot(const fs::path& root) {
        queue_.push_back(Task{ root, root, 0 });
    }

    void AddExternal(const fs::path& exe) {
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec)) return;
        AddHit(exe, exe.parent_path(), fs::file_size(exe, ec));
    }

    void Run(unsigned threads) {
        if (queue_.empty()) return;
        pending_ = queue_.size();

        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) pool.emplace_back([this] { Worker(); });
        for (auto& t : pool) t.join();
    }

    std::vector<Hit> TakeHits() {
        std::lock_guard<std::mutex> lock(hitMutex_);
        return std::move(hits_);
    }

    uint64_t Directories() const { return directories_.load(); }
    uint64_t Files() const { return files_.load(); }
    uint64_t Denied() const { return denied_.load(); }

private:
    struct Task {
        fs::path dir;
        fs::path root;
        int depth = 0;
    };

    void Worker() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return g_interrupted.load() || !queue_.empty() || pending_ == 0;
                });
                if (g_interrupted.load() || pending_ == 0) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            Walk(task);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
                if (pending_ == 0) cv_.notify_all();
            }
        }
    }

    void Walk(const Task& task) {
        ++directories_;

        std::error_code ec;
        fs::directory_iterator it(task.dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) { ++denied_; return; }
        const fs::directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (g_interrupted.load()) return;
            if (ec) { ++denied_; break; }

            const fs::directory_entry& entry = *it;
            std::error_code sec;
            const fs::file_status status = entry.symlink_status(sec);
            if (sec) continue;

            if (fs::is_directory(status)) {
                // junction / 符号链接一律不跟随：既防环，也防重复统计。
                if (fs::is_symlink(status)) continue;
                if (maxDepth_ > 0 && task.depth >= maxDepth_) continue;

                const fs::path& child = entry.path();
                if (Excluded(child)) continue;
                Enqueue(Task{ child, task.root, task.depth + 1 });
            } else if (fs::is_regular_file(status)) {
                ++files_;
                const fs::path& path = entry.path();
                if (IsSevenZipExe(path)) {
                    std::error_code sizeEc;
                    AddHit(path, OwnerOf(path, task.root), entry.file_size(sizeEc));
                }
            }
        }
    }

    void Enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
            ++pending_;
        }
        cv_.notify_one();
    }

    static bool IsVersionDir(const std::wstring& name) {
        // 形如 5.1.2609.9 的版本目录：至少两个点，且只由数字和点组成。
        int dots = 0;
        for (const wchar_t ch : name) {
            if (ch == L'.') { ++dots; continue; }
            if (ch < L'0' || ch > L'9') return false;
        }
        return dots >= 2;
    }

    static bool IsGenericBinaryDir(const std::wstring& name) {
        static const wchar_t* kNames[] = {
            L"bin", L"binaries", L"x64", L"x86", L"arm64", L"arm",
            L"win32", L"win64", L"lib", L"lib64",
        };
        const std::wstring lower = Lower(name);
        for (const wchar_t* candidate : kNames) {
            if (lower == candidate) return true;
        }
        return false;
    }

    static fs::path OwnerOf(const fs::path& file, const fs::path& root) {
        // “软件目录”= 7z 程序所在目录；如果它位于 bin / x64 / 5.1.2609.9 这类
        // 二进制或版本子目录里，就向上退，避免同一个软件被拆成好几条。
        fs::path owner = file.parent_path();
        while (owner != root) {
            const std::wstring name = owner.filename().native();
            if (!IsGenericBinaryDir(name) && !IsVersionDir(name)) break;
            const fs::path parent = owner.parent_path();
            if (parent.empty() || parent == owner) break;
            owner = parent;
        }
        return owner;
    }

    void AddHit(const fs::path& path, const fs::path& owner, uint64_t size) {
        const std::wstring key = CanonicalKey(path);
        {
            std::lock_guard<std::mutex> lock(hitMutex_);
            if (!seen_.insert(key).second) return;
            hits_.push_back(Hit{ path, owner, size });
        }
        if (verbose_) {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            // 进度行没有换行，先擦掉再输出，避免出现 “扫描中... + 路径” 的残影。
            if (g_progressDirty) {
                std::fprintf(stderr, "\r%*s\r", 90, "");
                std::fflush(stderr);
                g_progressDirty = false;
            }
            std::cout << "      + " << PathUtf8(path) << "\n" << std::flush;
        }
    }

    bool Excluded(const fs::path& p) const {
        const std::wstring name = Lower(p.filename().native());
        for (const auto& excludedName : rules_.names) {
            if (name == excludedName) return true;
        }
        const std::wstring full = Lower(p.native());
        for (const auto& pattern : rules_.paths) {
            if (full.find(pattern) != std::wstring::npos) return true;
        }
        return false;
    }

    ExcludeRules rules_;
    int maxDepth_ = 0;
    bool verbose_ = true;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    size_t pending_ = 0;

    std::mutex hitMutex_;
    std::set<std::wstring> seen_;
    std::vector<Hit> hits_;

    std::atomic<uint64_t> directories_{ 0 };
    std::atomic<uint64_t> files_{ 0 };
    std::atomic<uint64_t> denied_{ 0 };
};

// -----------------------------------------------------------------------------
// 命令行
// -----------------------------------------------------------------------------

struct Options {
    std::vector<fs::path> roots;
    std::vector<std::wstring> excludes;
    fs::path jsonPath;
    fs::path csvPath;
    bool allDrives = false;
    bool quiet = false;
    bool noPause = false;
    bool help = false;
    bool version = false;
    int threads = 0;
    int maxDepth = 0;
};

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

bool TakeInline(const std::wstring& arg, const wchar_t* name, std::wstring& value) {
    const size_t n = std::wcslen(name);
    if (arg.size() > n && arg[n] == L'=' && _wcsnicmp(arg.c_str(), name, n) == 0) {
        value = arg.substr(n + 1);
        return true;
    }
    return false;
}

void PrintHelp() {
    std::cout <<
        "用法: check7z [选项] [目录...]\n"
        "\n"
        "扫描本机自带的 7z 解压程序（7z.exe / 7zG.exe / 7zFM.exe / 7za.exe / 7zr.exe），\n"
        "按“软件目录”汇总，并给出数量、版本和体积。\n"
        "\n"
        "选项:\n"
        "  -r, --root <目录>      指定扫描根目录，可重复（默认: Program Files 及其他固定磁盘）\n"
        "      --all-drives       扫描所有固定磁盘（包含 C:\\ 全盘）\n"
        "  -x, --exclude <目录>   额外排除的目录，可重复\n"
        "      --max-depth <n>    最大递归深度，0 表示不限（默认 0）\n"
        "  -j, --json <文件>      导出 JSON 结果\n"
        "      --csv <文件>       导出 CSV 结果（含 BOM，Excel 可直接打开）\n"
        "  -t, --threads <n>      扫描线程数（默认: CPU 核心数，最多 8）\n"
        "  -q, --quiet            安静模式：不打印扫描进度与逐个命中\n"
        "      --no-pause         结束后不等待回车\n"
        "  -h, --help             显示本帮助\n"
        "  -v, --version          显示版本\n"
        "\n"
        "示例:\n"
        "  check7z\n"
        "  check7z --root \"D:\\Games\" --json result.json\n"
        "  check7z --all-drives --csv result.csv --threads 8\n";
}

bool ParseArgs(int argc, wchar_t** argv, Options& opts, std::wstring& error) {
    bool noMoreOptions = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        if (!noMoreOptions && arg == L"--") { noMoreOptions = true; continue; }

        if (!noMoreOptions && (arg == L"-h" || arg == L"--help" || arg == L"/?")) {
            opts.help = true;
            return true;
        }
        if (!noMoreOptions && (arg == L"-v" || arg == L"--version")) {
            opts.version = true;
            return true;
        }
        if (!noMoreOptions && arg == L"--all-drives") { opts.allDrives = true; continue; }
        if (!noMoreOptions && (arg == L"-q" || arg == L"--quiet")) { opts.quiet = true; continue; }
        if (!noMoreOptions && arg == L"--no-pause") { opts.noPause = true; continue; }

        std::wstring value;
        const bool hasInline = StartsWith(arg, L"--root=") || StartsWith(arg, L"--exclude=") ||
                               StartsWith(arg, L"--json=") || StartsWith(arg, L"--csv=") ||
                               StartsWith(arg, L"--threads=") || StartsWith(arg, L"--max-depth=");

        if (hasInline) {
            if (TakeInline(arg, L"--root", value)) { opts.roots.emplace_back(value); continue; }
            if (TakeInline(arg, L"--exclude", value)) { opts.excludes.push_back(value); continue; }
            if (TakeInline(arg, L"--json", value)) { opts.jsonPath = value; continue; }
            if (TakeInline(arg, L"--csv", value)) { opts.csvPath = value; continue; }
            if (TakeInline(arg, L"--threads", value)) {
                opts.threads = std::max(1, std::min(64, _wtoi(value.c_str())));
                continue;
            }
            if (TakeInline(arg, L"--max-depth", value)) {
                opts.maxDepth = std::max(0, _wtoi(value.c_str()));
                continue;
            }
        }

        if (!noMoreOptions && (arg == L"-r" || arg == L"--root" ||
                               arg == L"-x" || arg == L"--exclude" ||
                               arg == L"-j" || arg == L"--json" ||
                               arg == L"--csv" ||
                               arg == L"-t" || arg == L"--threads" ||
                               arg == L"--max-depth")) {
            if (i + 1 >= argc) { error = L"选项 " + arg + L" 缺少参数"; return false; }
            const std::wstring next = argv[++i];
            if (arg == L"-r" || arg == L"--root") opts.roots.emplace_back(next);
            else if (arg == L"-x" || arg == L"--exclude") opts.excludes.push_back(next);
            else if (arg == L"-j" || arg == L"--json") opts.jsonPath = next;
            else if (arg == L"--csv") opts.csvPath = next;
            else if (arg == L"-t" || arg == L"--threads") opts.threads = std::max(1, std::min(64, _wtoi(next.c_str())));
            else opts.maxDepth = std::max(0, _wtoi(next.c_str()));
            continue;
        }

        if (!noMoreOptions && !arg.empty() && arg[0] == L'-') {
            error = L"未知选项: " + arg;
            return false;
        }

        opts.roots.emplace_back(arg);
    }

    return true;
}

// -----------------------------------------------------------------------------
// 扫描根目录
// -----------------------------------------------------------------------------

std::vector<fs::path> FixedDrives() {
    std::vector<fs::path> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) == DRIVE_FIXED) drives.emplace_back(root);
    }
    return drives;
}

void NormalizeRoots(std::vector<fs::path>& roots) {
    // 保持用户给定的顺序；去掉重复项，以及被已保留根目录覆盖的子目录。
    std::vector<fs::path> unique;
    for (const auto& root : roots) {
        std::error_code ec;
        fs::path absolute = fs::weakly_canonical(root, ec);
        if (ec) absolute = fs::absolute(root, ec);

        const std::wstring key = Lower(absolute.native());
        bool covered = false;
        for (auto it = unique.begin(); it != unique.end();) {
            const std::wstring keptKey = Lower(it->native());
            const bool sameOrChild = key == keptKey ||
                (key.size() > keptKey.size() && key.compare(0, keptKey.size(), keptKey) == 0 &&
                 (keptKey.back() == L'\\' || key[keptKey.size()] == L'\\'));
            if (sameOrChild) { covered = true; break; }

            const bool containsKept = keptKey.size() > key.size() &&
                keptKey.compare(0, key.size(), key) == 0 &&
                (key.back() == L'\\' || keptKey[key.size()] == L'\\');
            if (containsKept) { it = unique.erase(it); continue; }
            ++it;
        }
        if (!covered) unique.push_back(std::move(absolute));
    }
    roots.swap(unique);
}

std::vector<fs::path> BuildRoots(const Options& opts) {
    std::vector<fs::path> roots = opts.roots;

    if (opts.allDrives) {
        for (const auto& drive : FixedDrives()) roots.push_back(drive);
    } else if (opts.roots.empty()) {
        roots.emplace_back(L"C:\\Program Files");
        roots.emplace_back(L"C:\\Program Files (x86)");
        for (const auto& drive : FixedDrives()) {
            if (Lower(drive.native()) != L"c:\\") roots.push_back(drive);
        }
    }

    std::vector<fs::path> existing;
    for (auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            if (!opts.quiet) std::cout << "      跳过不存在的目录: " << PathUtf8(root) << "\n";
            continue;
        }
        existing.push_back(std::move(root));
    }

    NormalizeRoots(existing);
    return existing;
}

ExcludeRules BuildExcludes(const Options& opts) {
    ExcludeRules rules;
    rules.names = {
        L"$recycle.bin",
        L"system volume information",
        L".git",
        L"$winreagent",
        L"config.msi",
        L"recovery",
    };
    rules.paths = {
        L"\\windows\\winsxs",
        L"\\windows\\servicing",
        L"\\windows\\installer",
        L"\\windows\\softwaredistribution",
    };
    for (const auto& extra : opts.excludes) rules.paths.push_back(Lower(extra));
    return rules;
}

unsigned ThreadCount(const Options& opts) {
    if (opts.threads > 0) return static_cast<unsigned>(opts.threads);
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    return std::max(2u, std::min(hw, 8u));
}

// -----------------------------------------------------------------------------
// 结果聚合
// -----------------------------------------------------------------------------

struct Group {
    fs::path owner;
    std::vector<const Hit*> files;
};

std::vector<Group> BuildGroups(const std::vector<Hit>& hits) {
    std::map<std::wstring, Group> grouped;
    for (const auto& hit : hits) {
        Group& group = grouped[Lower(hit.owner.native())];
        if (group.owner.empty()) group.owner = hit.owner;
        group.files.push_back(&hit);
    }

    std::vector<Group> groups;
    groups.reserve(grouped.size());
    for (auto& entry : grouped) {
        std::sort(entry.second.files.begin(), entry.second.files.end(),
                  [](const Hit* a, const Hit* b) { return _wcsicmp(a->path.c_str(), b->path.c_str()) < 0; });
        groups.push_back(std::move(entry.second));
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return _wcsicmp(a.owner.c_str(), b.owner.c_str()) < 0;
    });
    return groups;
}

uint64_t TotalBytes(const std::vector<Group>& groups) {
    uint64_t total = 0;
    for (const auto& group : groups) {
        for (const Hit* hit : group.files) total += hit->size;
    }
    return total;
}

size_t TotalFiles(const std::vector<Group>& groups) {
    size_t total = 0;
    for (const auto& group : groups) total += group.files.size();
    return total;
}

// -----------------------------------------------------------------------------
// 输出
// -----------------------------------------------------------------------------

void PrintTextReport(const std::vector<Group>& groups, const Stats& stats,
                     const std::vector<fs::path>& roots, bool interrupted) {
    const size_t fileCount = TotalFiles(groups);
    const uint64_t bytes = TotalBytes(groups);

    if (interrupted) {
        std::cout << "\n扫描被中断，以下为已完成的部分结果。\n";
    }

    std::cout << "\n------------------------------------------------------------\n";
    if (fileCount == 0) {
        std::cout << "未找到任何 7z 可执行程序。\n";
        std::cout << "提示: 可用 --root 指定目录，或用 --all-drives 扫描全部固定磁盘。\n";
    } else {
        std::cout << "共找到 " << fileCount << " 个 7z 可执行文件，分布在 "
                  << groups.size() << " 个软件目录。\n";
        std::cout << "------------------------------------------------------------\n";

        size_t index = 1;
        for (const auto& group : groups) {
            std::vector<std::string> labels;
            labels.reserve(group.files.size());
            size_t width = 16;
            for (const Hit* hit : group.files) {
                std::string label = PathUtf8(hit->path.lexically_relative(group.owner));
                if (label.empty()) label = PathUtf8(hit->path.filename());
                width = std::max(width, label.size() + 2);
                labels.push_back(std::move(label));
            }

            std::cout << "[" << index++ << "] " << PathUtf8(group.owner) << "\n";
            for (size_t i = 0; i < group.files.size(); ++i) {
                const Hit* hit = group.files[i];
                std::string version = ToUtf8(FileVersion(hit->path));
                if (version.empty()) version = "-";

                std::cout << "      " << std::left << std::setw(static_cast<int>(width)) << labels[i]
                          << std::setw(14) << version
                          << FormatSize(hit->size) << "\n";
            }
        }
    }

    std::cout << "------------------------------------------------------------\n";
    if (fileCount == 0) {
        std::cout << "合计: 0 个软件 / 0 个文件\n";
    } else {
        std::cout << "合计: " << groups.size() << " 个软件 / " << fileCount
                  << " 个文件 / " << FormatSize(bytes) << "（exe 体积）\n";
    }

    std::cout << "耗时: " << std::fixed << std::setprecision(1) << stats.seconds << " 秒"
              << " | 目录 " << GroupDigits(stats.directories)
              << " | 文件 " << GroupDigits(stats.files)
              << " | 无权限 " << GroupDigits(stats.denied) << "\n";

    std::cout << "扫描范围: ";
    for (size_t i = 0; i < roots.size(); ++i) {
        if (i != 0) std::cout << " | ";
        std::cout << PathUtf8(roots[i]);
    }
    std::cout << "\n============================================================\n";
}

bool ExportJson(const fs::path& path, const std::vector<Group>& groups,
                const Stats& stats, const std::vector<fs::path>& roots, bool interrupted) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    std::ostringstream json;
    json << "{\n";
    json << "  \"tool\": \"check7z\",\n";
    json << "  \"version\": \"" << kVersion << "\",\n";
    json << "  \"generated_at\": \"" << LocalTimeIso() << "\",\n";
    json << "  \"interrupted\": " << (interrupted ? "true" : "false") << ",\n";

    json << "  \"roots\": [";
    for (size_t i = 0; i < roots.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << JsonEscape(PathUtf8(roots[i])) << "\"";
    }
    json << "],\n";

    json << "  \"summary\": {\n";
    json << "    \"software\": " << groups.size() << ",\n";
    json << "    \"files\": " << TotalFiles(groups) << ",\n";
    json << "    \"bytes\": " << TotalBytes(groups) << ",\n";
    json << "    \"directories_scanned\": " << stats.directories << ",\n";
    json << "    \"files_scanned\": " << stats.files << ",\n";
    json << "    \"access_denied\": " << stats.denied << ",\n";
    json << "    \"elapsed_seconds\": " << std::fixed << std::setprecision(2) << stats.seconds << "\n";
    json << "  },\n";

    json << "  \"software\": [\n";
    for (size_t i = 0; i < groups.size(); ++i) {
        const Group& group = groups[i];
        json << "    {\n";
        json << "      \"path\": \"" << JsonEscape(PathUtf8(group.owner)) << "\",\n";
        json << "      \"files\": [\n";
        for (size_t j = 0; j < group.files.size(); ++j) {
            const Hit* hit = group.files[j];
            json << "        {\"name\": \"" << JsonEscape(PathUtf8(hit->path.filename())) << "\", "
                 << "\"path\": \"" << JsonEscape(PathUtf8(hit->path)) << "\", "
                 << "\"version\": \"" << JsonEscape(ToUtf8(FileVersion(hit->path))) << "\", "
                 << "\"bytes\": " << hit->size << "}";
            if (j + 1 != group.files.size()) json << ",";
            json << "\n";
        }
        json << "      ]\n";
        json << "    }" << (i + 1 == groups.size() ? "" : ",") << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    out << json.str();
    return static_cast<bool>(out);
}

bool ExportCsv(const fs::path& path, const std::vector<Group>& groups) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out << "\xEF\xBB\xBF";
    out << "owner,name,version,bytes,path\r\n";
    for (const auto& group : groups) {
        for (const Hit* hit : group.files) {
            out << CsvField(PathUtf8(group.owner)) << ','
                << CsvField(PathUtf8(hit->path.filename())) << ','
                << CsvField(ToUtf8(FileVersion(hit->path))) << ','
                << hit->size << ','
                << CsvField(PathUtf8(hit->path)) << "\r\n";
        }
    }
    return static_cast<bool>(out);
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        std::fprintf(stderr, "无法解析命令行。\n");
        return 1;
    }

    Options opts;
    std::wstring error;
    const bool parsed = ParseArgs(argc, argv, opts, error);
    LocalFree(argv);

    if (!parsed) {
        std::fprintf(stderr, "参数错误: %s\n\n", ToUtf8(error).c_str());
        PrintHelp();
        return 2;
    }
    if (opts.help) { PrintHelp(); return 0; }
    if (opts.version) { std::printf("check7z %s\n", kVersion); return 0; }

    std::cout << "============================================================\n";
    std::cout << "        check7z v" << kVersion << " — 7z 解压程序扫描\n";
    std::cout << "============================================================\n";

    // 1. 注册表
    std::cout << "[1/3] 检索注册表\n";
    std::vector<fs::path> registryHits;
    ProbeRegistry(registryHits);
    size_t registryValid = 0;
    for (const auto& exe : registryHits) {
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec)) continue;
        ++registryValid;
        std::cout << "      + " << PathUtf8(exe) << "\n";
    }
    if (registryValid == 0) std::cout << "      未发现 7-Zip 注册表记录\n";

    // 2. 扫描
    const std::vector<fs::path> roots = BuildRoots(opts);
    const ExcludeRules excludes = BuildExcludes(opts);
    const unsigned threads = ThreadCount(opts);

    std::cout << "[2/3] 扫描目录（" << threads << " 线程）\n";
    for (const auto& root : roots) std::cout << "      根目录: " << PathUtf8(root) << "\n";
    if (roots.empty()) {
        std::cout << "      没有可扫描的目录，退出。\n";
        return 1;
    }

    Scanner scanner(excludes, opts.maxDepth, !opts.quiet);
    for (const auto& root : roots) scanner.AddRoot(root);

    Stats stats;
    const auto start = std::chrono::steady_clock::now();

    std::atomic<bool> progressStop{ false };
    std::thread progressThread;
    const bool showProgress = !opts.quiet && IsConsoleHandle(GetStdHandle(STD_ERROR_HANDLE));
    g_progressVisible.store(showProgress);
    if (showProgress) {
        progressThread = std::thread([&scanner, &progressStop] {
            while (!progressStop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (progressStop.load()) break;
                std::lock_guard<std::mutex> lock(g_consoleMutex);
                std::fprintf(stderr, "\r  扫描中: 目录 %s | 文件 %s   ",
                             GroupDigits(scanner.Directories()).c_str(),
                             GroupDigits(scanner.Files()).c_str());
                std::fflush(stderr);
                g_progressDirty = true;
            }
            {
                std::lock_guard<std::mutex> lock(g_consoleMutex);
                if (g_progressDirty) {
                    std::fprintf(stderr, "\r%*s\r", 90, "");
                    std::fflush(stderr);
                    g_progressDirty = false;
                }
            }
        });
    }

    scanner.Run(threads);

    progressStop.store(true);
    if (progressThread.joinable()) progressThread.join();
    g_progressVisible.store(false);

    stats.directories = scanner.Directories();
    stats.files = scanner.Files();
    stats.denied = scanner.Denied();
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    for (const auto& exe : registryHits) scanner.AddExternal(exe);
    const std::vector<Hit> hits = scanner.TakeHits();
    const std::vector<Group> groups = BuildGroups(hits);

    // 3. 汇总
    std::cout << "[3/3] 汇总\n";
    PrintTextReport(groups, stats, roots, g_interrupted.load());

    if (!opts.jsonPath.empty()) {
        std::error_code ec;
        const fs::path target = fs::absolute(opts.jsonPath, ec);
        if (ExportJson(target, groups, stats, roots, g_interrupted.load())) {
            std::cout << "已导出 JSON: " << PathUtf8(target) << "\n";
        } else {
            std::cerr << "写入 JSON 失败: " << PathUtf8(target) << "\n";
        }
    }
    if (!opts.csvPath.empty()) {
        std::error_code ec;
        const fs::path target = fs::absolute(opts.csvPath, ec);
        if (ExportCsv(target, groups)) {
            std::cout << "已导出 CSV: " << PathUtf8(target) << "\n";
        } else {
            std::cerr << "写入 CSV 失败: " << PathUtf8(target) << "\n";
        }
    }

    if (!opts.noPause && IsConsoleHandle(GetStdHandle(STD_INPUT_HANDLE))) {
        std::cout << "\n按回车键退出..." << std::flush;
        std::cin.get();
    }

    return g_interrupted.load() ? 130 : 0;
}
