#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>
namespace wkrgba::experimental {
void configure(const std::filesystem::path& directory);
bool hasPresented() noexcept;
bool blackBackdropActive() noexcept;
uint8_t opaqueBlackPaletteIndex() noexcept;
FARPROC resolve(HMODULE library,LPCSTR name,FARPROC original) noexcept;
// Returns true only when the native terrain pass has been captured.
bool beginTerrainPass(void* display,int x,int y,int rows,unsigned flags) noexcept;
void completeTerrainPass() noexcept;
}
