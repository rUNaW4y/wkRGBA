#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>
#include <cstdint>
namespace wkrgba::labels {
void configure(HMODULE host,const std::filesystem::path& directory) noexcept;
void ensureInstalled() noexcept;
void protect(std::vector<uint8_t>& visibility,std::vector<uint8_t>* blackout,
             uint32_t viewportWidth,uint32_t viewportHeight,int64_t mapLeft,int64_t mapTop,
             int32_t cameraX,int32_t cameraY,bool bordered) noexcept;
}
