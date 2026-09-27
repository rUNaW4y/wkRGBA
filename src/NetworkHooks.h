#pragma once
#include <windows.h>
#include <filesystem>

namespace wkrgba::network {
bool installHooks(HMODULE host, const std::filesystem::path& directory) noexcept;
// Called by the loader after W:A has generated and opened land.dat from the
// canonical wire PNG. At that point the lobby map buffer can safely be restored.
void canonicalTerrainConsumed() noexcept;
}
