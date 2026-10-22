// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU test's ids and its stable metric keys (Phase 4a design §10, P1).
// Ids are forever: the keys below are stored in every result. Pure.

#include <QString>
#include <QStringView>

#include <array>
#include <cstdint>
#include <optional>

namespace sb::gpu {

enum class ApiId { Vulkan, Dx12, Dx11, OpenGl };  // run order
enum class SceneId { Geometry, DrawCalls, Shading, Particles };
enum class FillId { Quads, Sprites };
enum class NativePath { Direct2D, QtRaster };
enum class NativeWorkload { Shapes, Text, Blits, Gradients };

inline constexpr std::array<ApiId, 4> kApis{ApiId::Vulkan, ApiId::Dx12, ApiId::Dx11, ApiId::OpenGl};
inline constexpr std::array<SceneId, 4> kScenes{SceneId::Geometry, SceneId::DrawCalls, SceneId::Shading, SceneId::Particles};
inline constexpr std::array<FillId, 2> kFills{FillId::Quads, FillId::Sprites};
inline constexpr std::array<NativePath, 2> kNativePaths{NativePath::Direct2D, NativePath::QtRaster};
inline constexpr std::array<NativeWorkload, 4> kNativeWorkloads{NativeWorkload::Shapes, NativeWorkload::Text, NativeWorkload::Blits,
                                                                NativeWorkload::Gradients};

QString apiKey(ApiId);       // "vulkan" "dx12" "dx11" "opengl"
QString apiName(ApiId);      // "Vulkan" "DirectX 12" "DirectX 11" "OpenGL"
QString apiShort(ApiId);     // "Vulkan" "DX12" "DX11" "OpenGL"
QString sceneKey(SceneId);   // "geometry" "drawcalls" "shading" "particles"
QString sceneName(SceneId);  // "Geometry" "Draw calls" "Shading" "Particles + post"
QString fillKey(FillId);     // "quads" "sprites"
QString fillName(FillId);    // "Textured quads" "Sprites"
QString nativePathKey(NativePath);   // "d2d" "qt"
QString nativePathName(NativePath);  // "Direct2D" "Qt raster"
QString nativeKey(NativeWorkload);   // "shapes" "text" "blits" "gradients"
QString nativeName(NativeWorkload);  // "Shapes" "Text" "Image blits" "Gradients"
// §7.1: the ops one frame draws at content scale 1 (2,000 shapes, 8,000 glyphs = 200 strings x 40, 1,000 blits, 1,000 gradients);
// MOps/s = this x frames / seconds / 1e6. (native_ops.h's opsPerFrame is the same, a test checks it.)
std::uint32_t nativeOpsPerFrame(NativeWorkload);

std::optional<ApiId> apiFromKey(QStringView);
std::optional<SceneId> sceneFromKey(QStringView);
std::optional<FillId> fillFromKey(QStringView);
std::optional<NativePath> nativePathFromKey(QStringView);
std::optional<NativeWorkload> nativeFromKey(QStringView);

// §10 keys
QString sceneApiKey(ApiId, SceneId, QStringView field);  // "scenes3d.vulkan.geometry.fps"; field "fps","low","ft.p1","ft.p50","ft.p99","gpu","cpu","frames","match"
QString sceneBestKey(SceneId, QStringView field);        // "scenes3d.geometry.fps"; field "fps","api","low"
QString apiSummaryKey(ApiId, QStringView field);         // "scenes3d.vulkan.fps", "scenes3d.vulkan.low"
QString fillApiKey(ApiId, FillId);                       // Quads: "fill2d.dx12"; Sprites: "fill2d.dx12.sprites"
QString fillApiFpsKey(ApiId, FillId);                    // "fill2d.dx12.quads.fps", "fill2d.dx12.sprites.fps"
QString fillBestKey(FillId, bool apiField = false);      // "fill2d.quads" / "fill2d.quads.api"
QString nativePathMetricKey(NativePath, NativeWorkload); // "native2d.qt.text"
QString nativeBestKey(NativeWorkload, bool pathField = false);  // "native2d.text" / "native2d.text.path"
QString apiStatusKey(QStringView apiOrPath, QStringView field); // ("vulkan","status") -> "api.vulkan.status"
QString seriesKey(QStringView api, QStringView workload);       // "ft.vulkan.geometry"

inline const QString kScenes3d = QStringLiteral("scenes3d"), kNative2d = QStringLiteral("native2d"), kFill2d = QStringLiteral("fill2d");

} // namespace sb::gpu
