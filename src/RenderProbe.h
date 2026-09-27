#pragma once
#include <windows.h>
#include <filesystem>
namespace wkrgba::probe {
// Observation only, explicitly enabled by wkRGBA_trace in the runtime directory.
// The caller must first validate the exact executable hash.
bool install(HMODULE host, const std::filesystem::path& directory) noexcept;
}
