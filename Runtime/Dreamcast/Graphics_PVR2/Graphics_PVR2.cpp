/**
 * @file Graphics_PVR2.cpp
 * @brief Dreamcast PowerVR2 (PVR) backend for the engine's GFX_* surface.
 *
 * PHASE 1 SKELETON. The lifecycle entry points (Initialize/Shutdown/Begin/
 * EndFrame + viewport/scissor) do the minimum PVR setup to boot to a cleared
 * screen; every resource/draw entry point below is a no-op stub that keeps the
 * engine ABI satisfied so the build links and boots. Real 3D (Phase 2), UI
 * (Phase 3), etc. replace the stubs incrementally — see Makefile_Dreamcast and
 * the addon README phase plan.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */
#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsConstants.h"
#include "Engine/Maths.h"
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Font.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/ShadowMesh3d.h"
#include "Engine/Nodes/3D/SkeletalMesh3d.h"
#include "Engine/Nodes/3D/InstancedMesh3d.h"
#include "Engine/Nodes/3D/Particle3d.h"
#include "Engine/Nodes/3D/Camera3d.h"
#include "Engine/Nodes/Widgets/Widget.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"
#include "Log.h"

#include <kos.h>
#include <dc/pvr.h>
#include <cstdlib>
#include <cstring>
#include <vector>

// Dreamcast video is a fixed 640x480 framebuffer.
static const int kDcScreenW = 640;
static const int kDcScreenH = 480;
static bool sPvrInitialised = false;

// PVR requires all polys of one list type to be submitted together and in order:
// opaque (OP) -> punch-through (PT, alpha-tested cutouts) -> translucent (TR,
// alpha-blended). The engine issues draws in scene order, mixing blend modes, so
// each GFX_Draw*Comp transforms its geometry and BUFFERS it into a per-list bin;
// GFX_EndFrame then flushes OP -> PT -> TR in the required order. A single vertex
// pool + lightweight batch descriptors are cleared per frame but keep capacity.
enum DcList { DC_LIST_OP = 0, DC_LIST_PT, DC_LIST_TR, DC_LIST_COUNT };
static const pvr_list_t kPvrList[DC_LIST_COUNT] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };

struct DcBatch { pvr_poly_hdr_t hdr; uint32_t vertStart; uint32_t vertCount; uint8_t list; };
static std::vector<pvr_vertex_t> sVertPool;
static std::vector<DcBatch>      sBatches;

static bool  sInForwardPass = false;
static bool  sInUiPass      = false;
static bool  sSceneOpen     = false;
// UI overlays go in the translucent list with a huge 1/w so the PVR's depth-sort
// puts them on top of the 3D scene; each UI draw nudges it up for painter order.
static float sUiDepth       = 0.0f;

// ---- Lifecycle ------------------------------------------------------------

void GFX_Initialize()
{
    // Bring up the PowerVR2 TA with all three poly bins we use enabled. (The
    // *_defaults helper leaves punch-through disabled, which we need for Masked
    // cutout materials.) OP, TR, PT = 16-word bins; a 512 KB vertex buffer.
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        512 * 1024, 0, 0, 0, 0
    };
    if (pvr_init(&params) < 0)
    {
        LogError("Graphics_PVR2: pvr_init() failed");
        sPvrInitialised = false;
        return;
    }
    pvr_set_bg_color(0.10f, 0.15f, 0.35f);
    sPvrInitialised = true;
    LogDebug("Graphics_PVR2: PVR initialised (640x480, OP+PT+TR bins)");
}

void GFX_Shutdown()
{
    if (!sPvrInitialised) return;
    pvr_shutdown();
    sPvrInitialised = false;
}

// Pack a shaded RGBA (0..1 floats) into PVR ARGB (0xAARRGGBB).
static inline uint32_t DcFloatsToArgb(glm::vec3 rgb, float a)
{
    const uint32_t R = (uint32_t)(glm::clamp(rgb.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t G = (uint32_t)(glm::clamp(rgb.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t B = (uint32_t)(glm::clamp(rgb.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t A = (uint32_t)(glm::clamp(a,     0.0f, 1.0f) * 255.0f + 0.5f);
    return (A << 24) | (R << 16) | (G << 8) | B;
}

// Resolve a Texture asset to its uploaded PVR handle + format + UV crop.
static void DcResolveTexture(Texture* tex, pvr_ptr_t& vram, uint32_t& w, uint32_t& h,
                             int& fmt, float& uvX, float& uvY)
{
    vram = nullptr; w = h = 0; fmt = PVR_TXRFMT_ARGB1555; uvX = uvY = 1.0f;
    if (tex == nullptr) return;
    TextureResource* tr = tex->GetResource();
    if (tr == nullptr || tr->mPixels == nullptr || tr->mWidth == 0) return;
    vram = (pvr_ptr_t)tr->mPixels;
    w = tr->mWidth; h = tr->mHeight;
    fmt = PVR_TXRFMT_ARGB1555 | (tr->mSwizzled ? 0 : PVR_TXRFMT_NONTWIDDLED);
    uvX = (float)tex->GetWidth()  / (float)tr->mWidth;
    uvY = (float)tex->GetHeight() / (float)tr->mHeight;
}

// CPU transform + light + submit an indexed triangle list. PVR2 has no hardware
// T&L, so each vertex is multiplied by MVP here, perspective-divided to screen
// space, and shaded with a single baked directional diffuse term (there is no
// hardware lighting either). `verts` is the engine Vertex/VertexColor array.
static void DcSubmitMeshTriangles(const uint8_t* verts, uint32_t stride, bool hasColor,
                                  const IndexType* indices, uint32_t numIndices,
                                  const glm::mat4& mvp, const glm::mat3& normalMat,
                                  glm::vec4 baseColor, bool unlit,
                                  pvr_ptr_t texVram, uint32_t texW, uint32_t texH, int texFmt,
                                  float uvMaxX, float uvMaxY,
                                  DcList list, bool additive)
{
    static const glm::vec3 kLightDir = glm::normalize(glm::vec3(0.35f, 0.75f, 0.55f));
    const float kAmbient = 0.55f;

    const pvr_list_t pvrList = kPvrList[list];
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    if (texVram != nullptr)
        pvr_poly_cxt_txr(&cxt, pvrList, texFmt, (int)texW, (int)texH, texVram, PVR_FILTER_BILINEAR);
    else
        pvr_poly_cxt_col(&cxt, pvrList);
    // Screen-space Y-flip inverts winding, so the default backface cull would drop
    // front faces. Disable culling — depth (opaque) / sorting (TR) resolves it.
    cxt.gen.culling = PVR_CULLING_NONE;
    if (additive) { cxt.blend.src = PVR_BLEND_SRCALPHA; cxt.blend.dst = PVR_BLEND_ONE; }
    pvr_poly_compile(&hdr, &cxt);

    // Buffer the transformed triangles; GFX_EndFrame flushes per list in order.
    DcBatch batch;
    batch.hdr       = hdr;
    batch.list      = (uint8_t)list;
    batch.vertStart = (uint32_t)sVertPool.size();

    pvr_vertex_t pv;
    pv.oargb = 0;

    for (uint32_t t = 0; t + 3 <= numIndices; t += 3)
    {
        float sx[3], sy[3], sz[3], su[3], sv[3];
        uint32_t argb[3];
        bool skip = false;

        for (int k = 0; k < 3; ++k)
        {
            const IndexType vi = indices[t + k];
            const Vertex* vtx = reinterpret_cast<const Vertex*>(verts + (size_t)vi * stride);

            const glm::vec4 clip = mvp * glm::vec4(vtx->mPosition, 1.0f);
            if (clip.w <= 0.01f) { skip = true; break; }   // behind near plane
            const float invw = 1.0f / clip.w;
            sx[k] = (clip.x * invw * 0.5f + 0.5f) * kDcScreenW;
            sy[k] = (1.0f - (clip.y * invw * 0.5f + 0.5f)) * kDcScreenH;
            sz[k] = invw;
            su[k] = vtx->mTexcoord0.x * uvMaxX;
            sv[k] = vtx->mTexcoord0.y * uvMaxY;

            float shade = 1.0f;
            if (!unlit)
            {
                const glm::vec3 n = glm::normalize(normalMat * vtx->mNormal);
                const float lambert = glm::max(glm::dot(n, kLightDir), 0.0f);
                shade = kAmbient + (1.0f - kAmbient) * lambert;
            }

            glm::vec3 rgb(baseColor);
            float a = baseColor.a;
            if (hasColor)
            {
                const VertexColor* vc = reinterpret_cast<const VertexColor*>(verts + (size_t)vi * stride);
                const uint32_t c = vc->mColor;   // engine packs R in the low byte
                rgb *= glm::vec3(( c        & 0xFF) / 255.0f,
                                 ((c >> 8)  & 0xFF) / 255.0f,
                                 ((c >> 16) & 0xFF) / 255.0f);
                a *= ((c >> 24) & 0xFF) / 255.0f;
            }
            argb[k] = DcFloatsToArgb(rgb * shade, a);
        }
        if (skip) continue;

        pv.flags = PVR_CMD_VERTEX;
        pv.x = sx[0]; pv.y = sy[0]; pv.z = sz[0]; pv.u = su[0]; pv.v = sv[0]; pv.argb = argb[0]; sVertPool.push_back(pv);
        pv.x = sx[1]; pv.y = sy[1]; pv.z = sz[1]; pv.u = su[1]; pv.v = sv[1]; pv.argb = argb[1]; sVertPool.push_back(pv);
        pv.flags = PVR_CMD_VERTEX_EOL;
        pv.x = sx[2]; pv.y = sy[2]; pv.z = sz[2]; pv.u = su[2]; pv.v = sv[2]; pv.argb = argb[2]; sVertPool.push_back(pv);
    }

    batch.vertCount = (uint32_t)sVertPool.size() - batch.vertStart;
    if (batch.vertCount > 0) sBatches.push_back(batch);
}

void GFX_BeginFrame()
{
    if (!sPvrInitialised) return;
    // The engine's console init redirects KOS's dbgio device away from SCIF just
    // before the render loop, killing serial output. Re-assert SCIF once so in-loop
    // logging keeps reaching flycast's Serial Console. (Debug aid.)
    static bool sSerialReasserted = false;
    if (!sSerialReasserted) { sSerialReasserted = true; dbgio_dev_select("scif"); }
    pvr_wait_ready();
    pvr_scene_begin();
    sVertPool.clear();       // keeps capacity — no per-frame allocation churn
    sBatches.clear();
    sUiDepth   = 0.0f;
    sSceneOpen = true;
}

void GFX_EndFrame()
{
    if (!sPvrInitialised || !sSceneOpen) return;

    // Flush the buffered geometry per list, in the PVR's required order
    // (opaque -> punch-through -> translucent). OP is always emitted so the tile
    // background clears even on an empty frame; PT/TR only if they have batches.
    for (int L = 0; L < DC_LIST_COUNT; ++L)
    {
        bool has = false;
        for (const DcBatch& b : sBatches) { if (b.list == L) { has = true; break; } }
        if (L != DC_LIST_OP && !has) continue;

        pvr_list_begin(kPvrList[L]);
        for (const DcBatch& b : sBatches)
        {
            if (b.list != L) continue;
            pvr_prim(&b.hdr, sizeof(b.hdr));
            pvr_prim(&sVertPool[b.vertStart], b.vertCount * sizeof(pvr_vertex_t));
        }
        pvr_list_finish();
    }

    pvr_scene_finish();
    sSceneOpen = false;
}

void GFX_SetViewport(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool)
{
    // Fixed-resolution console: engine viewport hints (from the editor tab)
    // don't apply. Real clipping is set up per-list in Phase 2/3.
    (void)kDcScreenW; (void)kDcScreenH;
}

void GFX_SetScissor(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool) {}

// ---- Generated no-op stubs (engine ABI) -----------------------------------


void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}
bool GFX_ShouldCullLights() { return true; }
void GFX_BeginRenderPass(RenderPassId pass)
{
    sInForwardPass = (pass == RenderPassId::Forward);
    sInUiPass      = (pass == RenderPassId::Ui);
}
void GFX_EndRenderPass() { sInForwardPass = false; sInUiPass = false; }
void GFX_SetPipelineState(PipelineConfig config) {}
glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar) { return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar); }
glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar) { return glm::ortho(left, right, bottom, top, zNear, zFar); }
void GFX_SetFog(const FogSettings& fogSettings) {}
void GFX_DrawLines(const std::vector<Line>& /*lines*/) {}
void GFX_DrawFullscreen() {}
void GFX_ResizeWindow() {}
void GFX_Reset() {}
Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* /*outInstance*/) { return nullptr; }
uint32_t GFX_GetNumViews() { return 1; }
void GFX_SetFrameRate(int32_t /*frameRate*/) {}
void GFX_PathTrace() {}
void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }
void GFX_EnableMaterials(bool /*enable*/) {}
void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}
static uint32_t DcNextPow2(uint32_t v)
{
    uint32_t p = 8;                 // PVR minimum texture dimension
    while (p < v) p <<= 1;
    return p;
}



void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    if (!sPvrInitialised || texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;

    const uint32_t srcW = texture->GetWidth();
    const uint32_t srcH = texture->GetHeight();
    const std::vector<uint8_t>& px = !texture->GetPixels().empty() ? texture->GetPixels() : data;
    if (srcW == 0 || srcH == 0 || px.size() < (size_t)srcW * srcH * 4)
        return;

    // PVR needs power-of-two dimensions. Convert RGBA8888 -> ARGB1555 (5-bit
    // colour + a 1-bit alpha for cutout, usable by the punch-through path later).
    const uint32_t w = DcNextPow2(srcW);
    const uint32_t h = DcNextPow2(srcH);
    uint16_t* conv = (uint16_t*)malloc((size_t)w * h * 2);
    if (conv == nullptr) return;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint32_t sy = (y < srcH) ? y : srcH - 1;    // edge-replicate padding
        for (uint32_t x = 0; x < w; ++x)
        {
            const uint32_t sx = (x < srcW) ? x : srcW - 1;
            const uint8_t* p = &px[((size_t)sy * srcW + sx) * 4];
            const uint16_t a = (p[3] >= 128) ? 0x8000 : 0;
            conv[(size_t)y * w + x] = (uint16_t)(a | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
        }
    }

    if (r->mPixels != nullptr) pvr_mem_free((pvr_ptr_t)r->mPixels);
    pvr_ptr_t vram = pvr_mem_malloc((size_t)w * h * 2);
    if (vram != nullptr)
    {
        // pvr_txr_load_ex twiddles the linear source into the PVR's native
        // Morton layout by default (PVR_TXRLOAD_FMT_NOTWIDDLE would skip it), so
        // hand it the plain conv[] and mark the texture twiddled for the poly ctx.
        pvr_txr_load_ex(conv, vram, w, h, PVR_TXRLOAD_16BPP);
    }
    free(conv);

    r->mPixels   = vram;
    r->mWidth    = w;
    r->mHeight   = h;
    r->mBufWidth = w;
    r->mSwizzled = 1;   // twiddled (pvr_txr_load_ex twiddled it)
    // Crop UVs to the real content within the padded POT texture.
    texture->SetUVMax(glm::vec2((float)srcW / (float)w, (float)srcH / (float)h));
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr || r->mPixels == nullptr) return;
    pvr_mem_free((pvr_ptr_t)r->mPixels);
    r->mPixels = nullptr;
    r->mWidth = r->mHeight = r->mBufWidth = 0;
}
void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* src,
                                     uint32_t srcWidth, uint32_t srcHeight) {}
void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}
void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices, void* vertices, uint32_t numIndices, IndexType* indices)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;

    // PVR needs the geometry CPU-side (it transforms every frame), so keep our
    // own copy of the engine vertex/index data. Free any prior allocation.
    if (r->mVertexData) { free(r->mVertexData); r->mVertexData = nullptr; }
    if (r->mIndexData)  { free(r->mIndexData);  r->mIndexData  = nullptr; }

    const uint32_t stride = hasColor ? (uint32_t)sizeof(VertexColor) : (uint32_t)sizeof(Vertex);
    if (numVertices && vertices)
    {
        r->mVertexData = malloc((size_t)numVertices * stride);
        if (r->mVertexData) memcpy(r->mVertexData, vertices, (size_t)numVertices * stride);
    }
    if (numIndices && indices)
    {
        r->mIndexData = malloc((size_t)numIndices * sizeof(IndexType));
        if (r->mIndexData) memcpy(r->mIndexData, indices, (size_t)numIndices * sizeof(IndexType));
    }
    r->mNumVertices  = numVertices;
    r->mNumIndices   = numIndices;
    r->mVertexStride = stride;
    r->mVertexFlags  = hasColor ? 1u : 0u;   // reused as a has-vertex-colour flag
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData) { free(r->mVertexData); r->mVertexData = nullptr; }
    if (r->mIndexData)  { free(r->mIndexData);  r->mIndexData  = nullptr; }
    r->mNumVertices = r->mNumIndices = 0;
}
void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*c*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* c) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*c*/) {}
// Shared draw path for static and (CPU-skinned) skeletal meshes: resolve the
// material's texture, colour, shading and blend mode, then buffer the transformed
// geometry into the right list. `verts` is an engine Vertex/VertexColor array.
static void DcDrawMesh(const uint8_t* verts, uint32_t stride, bool hasColor,
                       const IndexType* indices, uint32_t numIndices,
                       const glm::mat4& model, Material* material)
{
    if (verts == nullptr || indices == nullptr || numIndices == 0) return;
    World* world = Renderer::Get()->GetCurrentWorld();
    Camera3D* cam = world ? world->GetActiveCamera() : nullptr;
    if (cam == nullptr) return;

    const glm::mat4 mvp = cam->GetViewProjectionMatrix() * model;
    const glm::mat3 nrm = glm::mat3(model);   // TODO: inverse-transpose for non-uniform scale

    pvr_ptr_t texVram; uint32_t texW, texH; int texFmt; float uvMaxX, uvMaxY;
    MaterialLite* lite = Material::AsLite(material);
    DcResolveTexture(lite ? lite->GetTexture(0) : nullptr, texVram, texW, texH, texFmt, uvMaxX, uvMaxY);
    glm::vec4  base  = lite ? lite->GetColor() : glm::vec4(1.0f);
    const bool unlit = lite ? (lite->GetShadingModel() == ShadingModel::Unlit) : false;

    // Route by blend mode: Opaque->OP, Masked (alpha-tested cutout)->PT,
    // Translucent->TR (alpha blend), Additive->TR with an additive blend func.
    DcList   list     = DC_LIST_OP;
    bool     additive = false;
    switch (lite ? lite->GetBlendMode() : BlendMode::Opaque)
    {
        case BlendMode::Masked:      list = DC_LIST_PT; break;
        case BlendMode::Translucent: list = DC_LIST_TR; break;
        case BlendMode::Additive:    list = DC_LIST_TR; additive = true; break;
        default:                     list = DC_LIST_OP; break;
    }
    base.a = (list == DC_LIST_TR) ? base.a * (lite ? lite->GetOpacity() : 1.0f) : 1.0f;

    DcSubmitMeshTriangles(verts, stride, hasColor, indices, numIndices, mvp, nrm, base, unlit,
                          texVram, texW, texH, texFmt, uvMaxX, uvMaxY, list, additive);
}

void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (!sPvrInitialised || !sInForwardPass || comp == nullptr) return;
    StaticMesh* mesh = meshOverride ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr || r->mNumIndices == 0) return;

    DcDrawMesh(reinterpret_cast<const uint8_t*>(r->mVertexData), r->mVertexStride, r->mVertexFlags != 0,
               reinterpret_cast<const IndexType*>(r->mIndexData), r->mNumIndices,
               comp->GetRenderTransform(), comp->GetMaterial());
}

// ---- Skeletal meshes (CPU-skinned) ----------------------------------------
// GFX_IsCpuSkinningRequired returns true, so the engine skins on the CPU and
// hands us the posed vertices each frame via UpdateSkeletalMeshCompVertexBuffer.
// The mesh resource holds only the (static) index data; the per-comp resource
// holds the per-frame posed vertex buffer.

void GFX_CreateSkeletalMeshResource(SkeletalMesh* sm, uint32_t /*numVertices*/,
                                    VertexSkinned* /*vertices*/, uint32_t numIndices, IndexType* indices)
{
    if (sm == nullptr) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr) return;
    if (r->mIndexData) { free(r->mIndexData); r->mIndexData = nullptr; }
    if (numIndices && indices)
    {
        r->mIndexData = malloc((size_t)numIndices * sizeof(IndexType));
        if (r->mIndexData) memcpy(r->mIndexData, indices, (size_t)numIndices * sizeof(IndexType));
    }
    r->mNumIndices = numIndices;
}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* sm)
{
    if (sm == nullptr) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr) return;
    if (r->mIndexData) { free(r->mIndexData); r->mIndexData = nullptr; }
    r->mNumIndices = 0;
}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*c*/) {}   // buffer alloc'd lazily

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* c)
{
    if (c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = r->mNumVertices = 0;
}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, uint32_t numVerts)
{
    if (c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    const uint32_t bytes = numVerts * (uint32_t)sizeof(Vertex);
    if (bytes > r->mVertexCapacity)
    {
        if (r->mVertexData) free(r->mVertexData);
        r->mVertexData     = malloc(bytes);
        r->mVertexCapacity = r->mVertexData ? bytes : 0;
    }
    r->mVertexStride = (uint32_t)sizeof(Vertex);
}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c,
                                            const std::vector<Vertex>& skinnedVertices)
{
    if (c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    const uint32_t bytes = (uint32_t)(skinnedVertices.size() * sizeof(Vertex));
    if (bytes > r->mVertexCapacity)
    {
        if (r->mVertexData) free(r->mVertexData);
        r->mVertexData     = malloc(bytes);
        r->mVertexCapacity = r->mVertexData ? bytes : 0;
    }
    if (r->mVertexData && bytes) memcpy(r->mVertexData, skinnedVertices.data(), bytes);
    r->mNumVertices  = (uint32_t)skinnedVertices.size();
    r->mVertexStride = (uint32_t)sizeof(Vertex);
}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* c)
{
    if (!sPvrInitialised || !sInForwardPass || c == nullptr) return;
    SkeletalMesh* mesh = c->GetSkeletalMesh();
    if (mesh == nullptr) return;
    SkeletalMeshResource*     mr = mesh->GetResource();
    SkeletalMeshCompResource* cr = c->GetResource();
    if (mr == nullptr || cr == nullptr) return;
    if (mr->mIndexData == nullptr || mr->mNumIndices == 0) return;
    if (cr->mVertexData == nullptr || cr->mNumVertices == 0) return;

    DcDrawMesh(reinterpret_cast<const uint8_t*>(cr->mVertexData), cr->mVertexStride, false,
               reinterpret_cast<const IndexType*>(mr->mIndexData), mr->mNumIndices,
               c->GetRenderTransform(), c->GetMaterial());
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/) { return true; }
void GFX_DrawShadowMeshComp(ShadowMesh3D* c) {}
void GFX_DrawInstancedMeshComp(InstancedMesh3D* comp) {}
void GFX_CreateTextMeshCompResource(TextMesh3D* /*c*/) {}
void GFX_DestroyTextMeshCompResource(TextMesh3D* /*c*/) {}
void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* /*c*/, const std::vector<Vertex>& /*v*/) {}
void GFX_DrawTextMeshComp(TextMesh3D* /*c*/) {}
void GFX_CreateVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_DestroyVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_UpdateVoxel3DResource(Voxel3D* /*v*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawVoxel3D(Voxel3D* /*v*/) {}
void GFX_CreateTerrain3DResource(Terrain3D* /*t*/) {}
void GFX_DestroyTerrain3DResource(Terrain3D* /*t*/) {}
void GFX_UpdateTerrain3DResource(Terrain3D* /*t*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawTerrain3D(Terrain3D* /*t*/) {}
void GFX_CreateTileMap2DResource(TileMap2D* /*t*/) {}
void GFX_DestroyTileMap2DResource(TileMap2D* /*t*/) {}
void GFX_UpdateTileMap2DResource(TileMap2D* /*t*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawTileMap2D(TileMap2D* /*t*/) {}
void GFX_CreateParticleCompResource(Particle3D* /*c*/) {}
void GFX_DestroyParticleCompResource(Particle3D* c) {}
void GFX_UpdateParticleCompVertexBuffer(Particle3D* c, const std::vector<VertexParticle>& vertices) {}
void GFX_DrawParticleComp(Particle3D* c) {}
// ---- 2D UI (Quad / Text / Poly) -------------------------------------------
// UI widgets provide screen-space VertexUI (pixel coords, uv, packed colour). We
// submit them straight to the translucent list at a huge 1/w so they overlay the
// 3D scene, ignoring the depth buffer. The widgets hold their verts directly
// (GetVertices), so Create/Update/Destroy resource hooks are no-ops.
enum DcUITopo { DC_UI_FAN, DC_UI_TRILIST };

static void DcSubmitUI(const VertexUI* verts, uint32_t n, Texture* tex, glm::vec4 tint, DcUITopo topo,
                       glm::vec2 posScale, glm::vec2 posOffset)
{
    if (verts == nullptr || n < 3 || tint.a <= 0.0f) return;

    pvr_ptr_t texVram; uint32_t texW, texH; int texFmt; float uvX, uvY;
    DcResolveTexture(tex, texVram, texW, texH, texFmt, uvX, uvY);

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    if (texVram != nullptr)
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, texFmt, (int)texW, (int)texH, texVram, PVR_FILTER_BILINEAR);
    else
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling      = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;   // overlay: ignore scene depth
    cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr, &cxt);

    const float z = 60000.0f + sUiDepth;          // sorts above 3D; painter order
    sUiDepth += 1.0f;

    DcBatch batch;
    batch.hdr       = hdr;
    batch.list      = (uint8_t)DC_LIST_TR;
    batch.vertStart = (uint32_t)sVertPool.size();

    auto emit = [&](const VertexUI& s, bool eol)
    {
        pvr_vertex_t v;
        v.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.oargb = 0;
        v.x = s.mPosition.x * posScale.x + posOffset.x;
        v.y = s.mPosition.y * posScale.y + posOffset.y;
        v.z = z;
        v.u = s.mTexcoord.x * uvX; v.v = s.mTexcoord.y * uvY;
        const uint32_t c = s.mColor;   // engine packs R in the low byte
        glm::vec4 col(( c        & 0xFF) / 255.0f, ((c >> 8)  & 0xFF) / 255.0f,
                      ((c >> 16) & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
        col *= tint;
        v.argb = DcFloatsToArgb(glm::vec3(col), col.a);
        sVertPool.push_back(v);
    };

    if (topo == DC_UI_FAN)
        for (uint32_t i = 1; i + 1 < n; ++i) { emit(verts[0], false); emit(verts[i], false); emit(verts[i + 1], true); }
    else
        for (uint32_t i = 0; i + 2 < n; i += 3) { emit(verts[i], false); emit(verts[i + 1], false); emit(verts[i + 2], true); }

    batch.vertCount = (uint32_t)sVertPool.size() - batch.vertStart;
    if (batch.vertCount) sBatches.push_back(batch);
}

void GFX_CreateQuadResource(Quad* /*quad*/) {}
void GFX_DestroyQuadResource(Quad* /*quad*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuad(Quad* quad)
{
    if (!sPvrInitialised || !sInUiPass || quad == nullptr) return;
    DcSubmitUI(quad->GetVertices(), quad->GetNumVertices(), quad->GetTexture(), quad->GetColor(),
               DC_UI_FAN, glm::vec2(1.0f), glm::vec2(0.0f));
}

void GFX_CreateQuadBorderResource(Quad* /*quad*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*quad*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuadBorder(Quad* quad)
{
    if (!sPvrInitialised || !sInUiPass || quad == nullptr) return;
    DcSubmitUI(quad->GetBorderVertices(), quad->GetNumVertices(), nullptr, quad->GetColor(),
               DC_UI_FAN, glm::vec2(1.0f), glm::vec2(0.0f));
}

void GFX_CreateTextResource(Text* /*text*/) {}
void GFX_DestroyTextResource(Text* /*text*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*text*/) {}
void GFX_DrawText(Text* text)
{
    if (!sPvrInitialised || !sInUiPass || text == nullptr) return;
    Font* font = text->GetFont();
    Texture* atlas = font ? font->GetTexture() : nullptr;
    const uint32_t n = text->GetNumVisibleCharacters() * TEXT_VERTS_PER_CHAR;

    // Text glyph verts are widget-LOCAL at the font's native point size; bake in
    // the anchor rect + justification offset and the scaledTextSize/fontSize
    // scale (the desktop backends do this via a shader/TEV uniform).
    const int32_t fontSize = font ? font->GetSize() : 32;
    const float   scale    = (fontSize > 0) ? (text->GetScaledTextSize() / (float)fontSize) : 1.0f;
    const Rect    rect     = text->GetRect();
    const glm::vec2 just   = text->GetJustifiedOffset();

    DcSubmitUI(text->GetVertices(), n, atlas, text->GetColor(), DC_UI_TRILIST,
               glm::vec2(scale, scale), glm::vec2(rect.mX + just.x, rect.mY + just.y));
}

void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* /*poly*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*poly*/) {}
void GFX_DrawPoly(Poly* poly)
{
    if (!sPvrInitialised || !sInUiPass || poly == nullptr) return;
    DcSubmitUI(poly->GetVertices(), poly->GetNumVertices(), poly->GetTexture(), poly->GetColor(),
               DC_UI_FAN, glm::vec2(1.0f), glm::vec2(0.0f));
}
void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON