// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/ids.h"

namespace sb::gpu {

namespace {

struct ApiText {
    ApiId id;
    const char *key, *name, *shortName;
};
constexpr ApiText kApiText[] = {{ApiId::Vulkan, "vulkan", "Vulkan", "Vulkan"},
                                {ApiId::Dx12, "dx12", "DirectX 12", "DX12"},
                                {ApiId::Dx11, "dx11", "DirectX 11", "DX11"},
                                {ApiId::OpenGl, "opengl", "OpenGL", "OpenGL"}};
struct SceneText {
    SceneId id;
    const char *key, *name;
};
constexpr SceneText kSceneText[] = {{SceneId::Geometry, "geometry", "Geometry"},
                                    {SceneId::DrawCalls, "drawcalls", "Draw calls"},
                                    {SceneId::Shading, "shading", "Shading"},
                                    {SceneId::Particles, "particles", "Particles + post"}};
struct FillText {
    FillId id;
    const char *key, *name;
};
constexpr FillText kFillText[] = {{FillId::Quads, "quads", "Textured quads"}, {FillId::Sprites, "sprites", "Sprites"}};
struct PathText {
    NativePath id;
    const char *key, *name;
};
constexpr PathText kPathText[] = {{NativePath::Direct2D, "d2d", "Direct2D"}, {NativePath::QtRaster, "qt", "Qt raster"}};
struct NativeText {
    NativeWorkload id;
    const char *key, *name;
};
constexpr NativeText kNativeText[] = {{NativeWorkload::Shapes, "shapes", "Shapes"},
                                      {NativeWorkload::Text, "text", "Text"},
                                      {NativeWorkload::Blits, "blits", "Image blits"},
                                      {NativeWorkload::Gradients, "gradients", "Gradients"}};

template <typename Table, typename Id>
const auto &row(const Table &table, Id id)
{
    for (const auto &r : table) {
        if (r.id == id)
            return r;
    }
    return table[0];
}

template <typename Id, typename Table>
std::optional<Id> fromKey(const Table &table, QStringView key)
{
    for (const auto &r : table) {
        if (key == QLatin1StringView(r.key))
            return r.id;
    }
    return std::nullopt;
}

QString L(const char *s) { return QString::fromLatin1(s); }

} // namespace

QString apiKey(ApiId a) { return L(row(kApiText, a).key); }
QString apiName(ApiId a) { return L(row(kApiText, a).name); }
QString apiShort(ApiId a) { return L(row(kApiText, a).shortName); }
QString sceneKey(SceneId s) { return L(row(kSceneText, s).key); }
QString sceneName(SceneId s) { return L(row(kSceneText, s).name); }
QString fillKey(FillId f) { return L(row(kFillText, f).key); }
QString fillName(FillId f) { return L(row(kFillText, f).name); }
QString nativePathKey(NativePath p) { return L(row(kPathText, p).key); }
QString nativePathName(NativePath p) { return L(row(kPathText, p).name); }
QString nativeKey(NativeWorkload w) { return L(row(kNativeText, w).key); }
QString nativeName(NativeWorkload w) { return L(row(kNativeText, w).name); }

std::uint32_t nativeOpsPerFrame(NativeWorkload w)
{
    switch (w) {
    case NativeWorkload::Shapes: return 2000;
    case NativeWorkload::Text: return 8000;
    case NativeWorkload::Blits: return 1000;
    case NativeWorkload::Gradients: return 1000;
    }
    return 0;
}

std::optional<ApiId> apiFromKey(QStringView k) { return fromKey<ApiId>(kApiText, k); }
std::optional<SceneId> sceneFromKey(QStringView k) { return fromKey<SceneId>(kSceneText, k); }
std::optional<FillId> fillFromKey(QStringView k) { return fromKey<FillId>(kFillText, k); }
std::optional<NativePath> nativePathFromKey(QStringView k) { return fromKey<NativePath>(kPathText, k); }
std::optional<NativeWorkload> nativeFromKey(QStringView k) { return fromKey<NativeWorkload>(kNativeText, k); }

QString sceneApiKey(ApiId a, SceneId s, QStringView field)
{
    return QStringLiteral("scenes3d.%1.%2.%3").arg(apiKey(a), sceneKey(s), field);
}

QString sceneBestKey(SceneId s, QStringView field) { return QStringLiteral("scenes3d.%1.%2").arg(sceneKey(s), field); }

QString apiSummaryKey(ApiId a, QStringView field) { return QStringLiteral("scenes3d.%1.%2").arg(apiKey(a), field); }

QString fillApiKey(ApiId a, FillId f)
{
    return f == FillId::Quads ? QStringLiteral("fill2d.%1").arg(apiKey(a)) : QStringLiteral("fill2d.%1.sprites").arg(apiKey(a));
}

QString fillApiFpsKey(ApiId a, FillId f) { return QStringLiteral("fill2d.%1.%2.fps").arg(apiKey(a), fillKey(f)); }

QString fillBestKey(FillId f, bool apiField)
{
    return apiField ? QStringLiteral("fill2d.%1.api").arg(fillKey(f)) : QStringLiteral("fill2d.%1").arg(fillKey(f));
}

QString nativePathMetricKey(NativePath p, NativeWorkload w) { return QStringLiteral("native2d.%1.%2").arg(nativePathKey(p), nativeKey(w)); }

QString nativeBestKey(NativeWorkload w, bool pathField)
{
    return pathField ? QStringLiteral("native2d.%1.path").arg(nativeKey(w)) : QStringLiteral("native2d.%1").arg(nativeKey(w));
}

QString apiStatusKey(QStringView apiOrPath, QStringView field) { return QStringLiteral("api.%1.%2").arg(apiOrPath, field); }

QString seriesKey(QStringView api, QStringView workload) { return QStringLiteral("ft.%1.%2").arg(api, workload); }

} // namespace sb::gpu
