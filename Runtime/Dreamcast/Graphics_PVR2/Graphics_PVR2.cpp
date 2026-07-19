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

// Dreamcast video is a fixed 640x480 framebuffer.
static const int kDcScreenW = 640;
static const int kDcScreenH = 480;
static bool sPvrInitialised = false;

// Frame/pass state. PVR needs exactly one list open across all the draws the
// engine issues between BeginFrame and EndFrame, so the opaque list is opened in
// BeginFrame and closed in EndFrame (not per-draw). We only submit geometry
// during the Forward pass (Shadows/HitCheck/Selected passes re-issue the same
// draws and would double-submit).
static bool sOpaqueListOpen = false;
static bool sInForwardPass  = false;

// Draw the engine's test cube while there is no real scene; suppressed once real
// meshes are being submitted so it doesn't overlap them.
static bool sDrewRealMeshThisFrame = false;

// ---- Lifecycle (minimal PVR setup: boots to a cleared screen) -------------

void GFX_Initialize()
{
    // Bring up the PowerVR2 tile accelerator. With no real draws yet (Phase 1),
    // an empty scene each frame clears the screen to this background colour —
    // visible proof the engine's render loop is running on the Dreamcast.
    if (pvr_init_defaults() < 0)
    {
        LogError("Graphics_PVR2: pvr_init_defaults() failed");
        sPvrInitialised = false;
        return;
    }
    pvr_set_bg_color(0.10f, 0.15f, 0.35f);  // distinct blue so a boot is obvious
    sPvrInitialised = true;
    LogDebug("Graphics_PVR2: PVR initialised (640x480); Phase-1 clears to bg colour");
}

void GFX_Shutdown()
{
    if (!sPvrInitialised) return;
    pvr_shutdown();
    sPvrInitialised = false;
}

// Phase-2 bring-up test geometry — a spinning 3D cube. This exercises the exact
// pipeline the engine's mesh draws will use: PVR2 has NO hardware T&L, so the
// SH4 multiplies each vertex by model·view·projection, does the perspective
// divide, and emits SCREEN-space pvr_vertex_t (x,y in pixels, z = 1/w for depth).
// Removed once GFX_DrawStaticMeshComp submits real engine geometry.
// (glm comes in via Engine/Maths.h; GFX_MakePerspectiveMatrix already uses it.)
static void DcSubmitTestCube()
{
    static const glm::vec3 kCorners[8] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };
    // 6 faces, corners in perimeter order; one flat colour each.
    static const int kFaces[6][4] = {
        {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
    };
    static const uint32_t kFaceColors[6] = {
        0xFFFF4040, 0xFF40FF40, 0xFF4040FF, 0xFFFFFF40, 0xFF40FFFF, 0xFFFF40FF,
    };

    static float sAngle = 0.0f;
    sAngle += 0.03f;

    const glm::mat4 proj  = glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 100.0f);
    const glm::mat4 view  = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    const glm::mat4 model = glm::rotate(glm::mat4(1.0f), sAngle, glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f)));
    const glm::mat4 mvp   = proj * view * model;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    for (int f = 0; f < 6; ++f)
    {
        float sx[4], sy[4], sz[4];
        bool clipped = false;
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec4 clip = mvp * glm::vec4(kCorners[kFaces[f][i]], 1.0f);
            if (clip.w <= 0.01f) { clipped = true; break; }   // behind the near plane
            const float invw = 1.0f / clip.w;
            sx[i] = (clip.x * invw * 0.5f + 0.5f) * kDcScreenW;
            sy[i] = (1.0f - (clip.y * invw * 0.5f + 0.5f)) * kDcScreenH;
            sz[i] = invw;                                     // PVR depth = 1/w
        }
        if (clipped) continue;

        // Quad as a PVR triangle strip: perimeter order 0,1,2,3 -> strip 0,1,3,2.
        static const int kStrip[4] = {0, 1, 3, 2};
        pvr_vertex_t v;
        v.oargb = 0;
        v.u = v.v = 0.0f;
        v.argb = kFaceColors[f];
        for (int k = 0; k < 4; ++k)
        {
            const int idx = kStrip[k];
            v.flags = (k == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = sx[idx]; v.y = sy[idx]; v.z = sz[idx];
            pvr_prim(&v, sizeof(v));
        }
    }
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

// CPU transform + light + submit an indexed triangle list. PVR2 has no hardware
// T&L, so each vertex is multiplied by MVP here, perspective-divided to screen
// space, and shaded with a single baked directional diffuse term (there is no
// hardware lighting either). `verts` is the engine Vertex/VertexColor array.
static void DcSubmitMeshTriangles(const uint8_t* verts, uint32_t stride, bool hasColor,
                                  const IndexType* indices, uint32_t numIndices,
                                  const glm::mat4& mvp, const glm::mat3& normalMat,
                                  glm::vec4 baseColor)
{
    static const glm::vec3 kLightDir = glm::normalize(glm::vec3(0.35f, 0.75f, 0.55f));
    const float kAmbient = 0.35f;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    pvr_vertex_t pv;
    pv.oargb = 0;
    pv.u = pv.v = 0.0f;

    for (uint32_t t = 0; t + 3 <= numIndices; t += 3)
    {
        float sx[3], sy[3], sz[3];
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

            const glm::vec3 n = glm::normalize(normalMat * vtx->mNormal);
            const float lambert = glm::max(glm::dot(n, kLightDir), 0.0f);
            const float shade = kAmbient + (1.0f - kAmbient) * lambert;

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
        pv.x = sx[0]; pv.y = sy[0]; pv.z = sz[0]; pv.argb = argb[0]; pvr_prim(&pv, sizeof(pv));
        pv.x = sx[1]; pv.y = sy[1]; pv.z = sz[1]; pv.argb = argb[1]; pvr_prim(&pv, sizeof(pv));
        pv.flags = PVR_CMD_VERTEX_EOL;
        pv.x = sx[2]; pv.y = sy[2]; pv.z = sz[2]; pv.argb = argb[2]; pvr_prim(&pv, sizeof(pv));
    }
}

void GFX_BeginFrame()
{
    if (!sPvrInitialised) return;
    pvr_wait_ready();
    pvr_scene_begin();
    // Open the opaque list and keep it open for the whole frame; the engine's
    // per-node GFX_Draw*Comp calls (during the Forward pass) submit into it. The
    // list clears the tile background to pvr_set_bg_color.
    pvr_list_begin(PVR_LIST_OP_POLY);
    sOpaqueListOpen = true;
    sDrewRealMeshThisFrame = false;
}

void GFX_EndFrame()
{
    if (!sPvrInitialised) return;
    if (sOpaqueListOpen)
    {
        // Fallback demo geometry only when the scene submitted nothing (e.g. no
        // scene/camera yet), so there is always something on screen.
        if (!sDrewRealMeshThisFrame)
            DcSubmitTestCube();
        pvr_list_finish();
        sOpaqueListOpen = false;
    }
    pvr_scene_finish();
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
void GFX_BeginRenderPass(RenderPassId pass) { sInForwardPass = (pass == RenderPassId::Forward); }
void GFX_EndRenderPass() { sInForwardPass = false; }
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
void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& /*data*/) {}
void GFX_DestroyTextureResource(Texture* texture) {}
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
void GFX_CreateSkeletalMeshResource(SkeletalMesh* sm,
                                    uint32_t /*numVerts*/,
                                    VertexSkinned* /*verts*/,
                                    uint32_t numIdx,
                                    IndexType* idx) {}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* sm) {}
void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*c*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* c) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*c*/) {}
void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (!sPvrInitialised || !sOpaqueListOpen || !sInForwardPass || comp == nullptr) return;

    StaticMesh* mesh = meshOverride ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr || r->mNumIndices == 0) return;

    World* world = Renderer::Get()->GetCurrentWorld();
    Camera3D* cam = world ? world->GetActiveCamera() : nullptr;
    if (cam == nullptr) return;

    const glm::mat4 model = comp->GetRenderTransform();
    const glm::mat4 mvp   = cam->GetViewProjectionMatrix() * model;
    const glm::mat3 nrm   = glm::mat3(model);   // TODO: inverse-transpose for non-uniform scale

    DcSubmitMeshTriangles(reinterpret_cast<const uint8_t*>(r->mVertexData), r->mVertexStride,
                          r->mVertexFlags != 0, reinterpret_cast<const IndexType*>(r->mIndexData),
                          r->mNumIndices, mvp, nrm, glm::vec4(1.0f));
    sDrewRealMeshThisFrame = true;
}
void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*c*/) {}
void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* c) {}
void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, uint32_t numVerts) {}
void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c,
                                            const std::vector<Vertex>& skinnedVertices) {}
void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* c) {}
bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/) { return false; }
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
void GFX_CreateQuadResource(Quad* quad) {}
void GFX_DestroyQuadResource(Quad* quad) {}
void GFX_UpdateQuadResourceVertexData(Quad* quad) {}
void GFX_DrawQuad(Quad* quad) {}
void GFX_CreateQuadBorderResource(Quad* quad) {}
void GFX_DestroyQuadBorderResource(Quad* quad) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* quad) {}
void GFX_DrawQuadBorder(Quad* quad) {}
void GFX_CreateTextResource(Text* text) {}
void GFX_DestroyTextResource(Text* text) {}
void GFX_UpdateTextResourceVertexData(Text* text) {}
void GFX_DrawText(Text* text) {}
void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* poly) {}
void GFX_UpdatePolyResourceVertexData(Poly* poly) {}
void GFX_DrawPoly(Poly* poly) {}
void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON