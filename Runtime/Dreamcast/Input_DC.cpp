/**
 * @file Input_DC.cpp
 * @brief Dreamcast maple-bus input backend for the engine's INP_* surface.
 *
 * PHASE 1 SKELETON. Lifecycle is stubbed to no-ops so the engine boots without
 * input; Phase 5 wires KOS maple/cont_* controller state, analog deadzone
 * (~0.30), and the +Y = up convention. See the addon README phase plan.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */
#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

// ---- Lifecycle (Phase-1 stubs) --------------------------------------------

void INP_Initialize() { LogDebug("Input_DC: stub (Phase 5 wires maple controllers)"); }
void INP_Shutdown() {}
void INP_Update() {}

// ---- Generated no-op stubs (engine ABI) -----------------------------------


void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON