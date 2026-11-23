#include "source/DataDirectory.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <vector>

std::filesystem::path executableDirectory() {
  std::error_code ec;
#if defined(_WIN32)
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size()) return {};
  path.resize(n);
  return std::filesystem::path(path).parent_path();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> path(size + 1, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
  return std::filesystem::weakly_canonical(std::filesystem::path(path.data()), ec).parent_path();
#else
  return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();  // empty where there is no /proc
#endif
}

std::vector<std::filesystem::path> dataRoots(const std::filesystem::path & exe) {
  std::vector<std::filesystem::path> roots = {"data"};
  if (!exe.empty()) {
    roots.push_back(exe / "data");
    roots.push_back(exe.parent_path() / "share" / "stocks");
  }
#ifdef STOCKS_DATA_DIR
  roots.push_back(STOCKS_DATA_DIR);
#endif
  return roots;
}

std::filesystem::path findIn(const std::vector<std::filesystem::path> & roots, const std::string & name) {
  std::error_code ec;
  for (const std::filesystem::path & root : roots)
    if (std::filesystem::is_directory(root / name, ec)) return root / name;
  return {};
}

std::filesystem::path findDataDirectory(const std::string & name) { return findIn(dataRoots(executableDirectory()), name); }
