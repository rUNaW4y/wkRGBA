#pragma once
#include <stdint.h>
#include <stddef.h>
struct IDirect3DDevice9;
#ifdef WKRGBA_BUILD
#define WKRGBA_API extern "C" __declspec(dllexport)
#else
#define WKRGBA_API extern "C" __declspec(dllimport)
#endif
// Developer API, serialized internally. D3D calls must run on device thread.
// The supported W:A host gets PNG file-loading, D3D9 scene and replay hooks at
// startup when the module is enabled. Multiplayer transport is
// still under development.
WKRGBA_API const char* __cdecl wkRGBAStatus();
WKRGBA_API const char* __cdecl wkRGBALastError();
WKRGBA_API int __cdecl wkRGBALoad(const wchar_t* png, unsigned alphaThreshold);
WKRGBA_API int __cdecl wkRGBASync(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* mask, size_t bytes, size_t stride);
WKRGBA_API int __cdecl wkRGBADraw(IDirect3DDevice9* device, float left, float top, float scale);
WKRGBA_API void __cdecl wkRGBAClear();
