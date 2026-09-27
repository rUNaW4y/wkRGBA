#pragma once
#include <windows.h>
#include <filesystem>
namespace wkrgba::replay {bool installHooks(HMODULE host,const std::filesystem::path& directory) noexcept;}
