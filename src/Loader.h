#pragma once
#include "Transfer.h"
#include <windows.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
namespace wkrgba::loader {
struct ImportedMap {
    ImportedMap(std::filesystem::path source,Image image,std::vector<uint8_t> rgbaPng,std::vector<uint8_t> nativePng);
    std::shared_ptr<const Image> artwork() const;
    void discardArtwork();
    std::filesystem::path source;
    uint32_t width{},height{};
    net::Manifest manifest;
    std::vector<uint8_t> rgbaPng;
    std::vector<uint8_t> nativePng;
    bool bordered{};
private:
    mutable std::mutex artworkMutex_;
    mutable std::shared_ptr<const Image> artwork_;
};
bool install(HMODULE module);
bool installed();
std::shared_ptr<const ImportedMap> lastImport();
// The renderer may replace lastImport() with a gameplay-padded view. Network
// serialization must always compare against the source-sized editor map.
std::shared_ptr<const ImportedMap> networkImport();
std::shared_ptr<const ImportedMap> networkPristineImport();
uint32_t networkEditorHoleCount();
void setNetworkCanonicalLand(std::vector<uint8_t> land);
// Converts W:A's editor LAND into the exact network-map geometry while
// preserving the holes and placement coordinates selected by W:A.
std::vector<uint8_t> canonicalizeNetworkLand(std::vector<uint8_t> land,
                                             const Image& source,
                                             uint32_t targetHeight,
                                             bool bordered);
// Applies the authoritative LAND collision plane to RGBA artwork so preview,
// renderer and simulation all show the same holes.
Image applyNetworkLandCollision(const std::vector<uint8_t>& land,const Image& artwork);
void acceptImage(Image image, const std::filesystem::path& source,
                 std::vector<uint8_t> rgbaPng = {}, std::vector<uint8_t> nativePng = {},
                 bool networkCanonical = false);
void clearImport();
// Network lobbies must not reshape land.dat while the editor is still
// generating borders/holes.  The game-start packet opens this gate on both
// host and wkRGBA clients; offline play remains enabled by default.
void setNetworkGameplayReady(bool ready);
// In a mixed lobby the native map is the simulation contract.  A legacy
// client cannot reproduce wkRGBA's local land.dat padding/restoration, so the
// host must leave current.thm and land.dat byte-for-byte under W:A's control.
void setLegacyCompatibilityGameplay(bool active,bool restoreTop);
bool setNetworkCanonicalGameplay(bool active);
// Activates the exact canonical map which was serialized to wkRGBA peers.
// Keeping these bytes authoritative prevents current.thm/land.dat from being
// rebuilt from a stale editor snapshot after holes or borders changed.
bool setNetworkCanonicalGameplayExact(Image image,std::vector<uint8_t> rgbaPng,
                                      std::vector<uint8_t> nativePng);
// Shared by the actual IAT hook and the native-loader integration test.
// Returned handle refers to a process-local cached native proxy. The cache
// keeps the temporary file alive so repeated W:A opens do not reconvert it.
HANDLE openNative(const wchar_t* path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,
                  DWORD disposition,DWORD flags,HANDLE templateFile);
#ifdef WKRGBA_LOADER_TESTING
bool installTestImports(HMODULE module,const std::filesystem::path& root);
void uninstallTestImports();
#endif
}
