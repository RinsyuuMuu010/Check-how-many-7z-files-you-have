#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <filesystem>
#include <windows.h>
#include <algorithm>

namespace fs = std::filesystem;

// 使用 set 存储路径，自动去重
std::set<std::string> foundPaths;

// --- 辅助函数：从注册表获取路径 ---
std::string GetRegString(HKEY hKeyParent, const std::string& subKey, const std::string& valueName) {
    HKEY hKey;
    char buffer[MAX_PATH];
    DWORD bufferSize = sizeof(buffer);
    if (RegOpenKeyExA(hKeyParent, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, valueName.c_str(), NULL, NULL, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(buffer);
        }
        RegCloseKey(hKey);
    }
    return "";
}

// --- 核心逻辑 1：查找注册表（通常是正式安装的软件） ---
void SearchRegistry() {
    std::cout << "[1/2] 正在快速检索注册表..." << std::endl;

    // 7-Zip 官方安装版
    std::string p1 = GetRegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\7-Zip", "Path");
    if (!p1.empty()) foundPaths.insert(p1 + "7z.exe");

    // WinRAR
    std::string p2 = GetRegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WinRAR", "ExePath");
    if (!p2.empty()) foundPaths.insert(p2);

    // Bandizip
    std::string p3 = GetRegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Bandizip", "InstallDir");
    if (!p3.empty()) foundPaths.insert(p3 + "\\Bandizip.exe");
}

// --- 核心逻辑 2：递归扫描硬盘（查找软件自带或绿色的 .exe） ---
void DeepScanDirectories(const std::string& root) {
    if (!fs::exists(root)) return;

    std::cout << "[2/2] 正在扫描: " << root << " (排除 DLL，仅查找 EXE...)" << std::endl;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            try {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();

                    // 转为小写进行比较
                    std::string lowerFilename = filename;
                    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);

                    // 只匹配可执行程序，去掉了 7z.dll
                    if (lowerFilename == "7z.exe" ||
                        lowerFilename == "7zg.exe" ||
                        lowerFilename == "7za.exe") {

                        std::string fullPath = entry.path().string();
                        foundPaths.insert(fullPath);
                        std::cout << "    发现程序: " << fullPath << std::endl;
                    }
                }
            }
            catch (...) {
                continue; // 跳过无法读取的文件
            }
        }
    }
    catch (const std::exception& e) {
        // 忽略权限报错
    }
}

int main() {
    // 设置 UTF-8 编码，防止中文路径乱码
    system("chcp 65001 > nul");

    std::cout << "================================================" << std::endl;
    std::cout << "       7z 解压工具深度搜索 (仅限 EXE 版)" << std::endl;
    std::cout << "================================================" << std::endl;

    // 1. 注册表搜索
    SearchRegistry();

    // 2. 目录深度扫描 (可以在这里修改需要扫描的盘符)
    std::vector<std::string> scanRoots = {
        "C:\\Program Files",
        "C:\\Program Files (x86)",
        "D:\\"  // D 盘全盘扫描
    };

    for (const auto& root : scanRoots) {
        DeepScanDirectories(root);
    }

    // 3. 最终汇总输出
    std::cout << "\n================ 搜索结果汇总 ================" << std::endl;
    if (foundPaths.empty()) {
        std::cout << "未找到任何 7z 相关可执行程序。" << std::endl;
    }
    else {
        std::cout << "共找到 " << foundPaths.size() << " 个程序文件：" << std::endl;
        for (const auto& path : foundPaths) {
            std::cout << " [EXE] -> " << path << std::endl;
        }
    }
    std::cout << "================================================" << std::endl;

    std::cout << "\n扫描已完成。按回车键退出..." << std::endl;
    std::cin.get();

    return 0;
}