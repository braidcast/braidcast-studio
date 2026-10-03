#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_ASSETS_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_ASSETS_HPP_

#include <cstddef>
#include <string_view>

namespace Overlay {

// The largest file each kind of widget upload may be. The one table every reader of a cap
// uses: the bridge refuses an upload over it before storing anything
// (MethodOverlaysUploadAsset), the server refuses to serve a stored file over it
// (ServeWidget), and the editor is handed the same numbers by overlays.assetLimits so it can
// refuse before the transfer. A copy of any of these numbers elsewhere is a cap that drifts.
struct AssetKindCap {
	std::string_view kind;
	size_t maxBytes;
};

constexpr size_t kAssetMiB = 1024 * 1024;

inline constexpr AssetKindCap kAssetKindCaps[] = {
	{"sound", 8 * kAssetMiB},
	{"image", 8 * kAssetMiB},
	// WebM with alpha runs large for a few seconds of motion; the upload stays base64
	// through the bridge, which is what keeps this from going higher.
	{"video", 32 * kAssetMiB},
};

// Every upload recorded before kinds had caps, and any kind without a row, is held to the
// smallest one, which is what every upload was held to before the table existed.
constexpr size_t kDefaultAssetMaxBytes = 8 * kAssetMiB;

constexpr size_t MaxAssetBytes(std::string_view kind)
{
	for (const AssetKindCap &cap : kAssetKindCaps) {
		if (cap.kind == kind) {
			return cap.maxBytes;
		}
	}
	return kDefaultAssetMaxBytes;
}

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_ASSETS_HPP_
