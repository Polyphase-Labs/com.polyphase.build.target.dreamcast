/**
 * @file Input_DC.cpp
 * @brief Dreamcast controller input via the KOS maple bus. Maps the first
 *        connected standard controller into the engine's GamepadState slot 0.
 *
 * Face-button diamond on the Dreamcast pad matches the engine's Xbox-position
 * intent directly (Y top, X left, B right, A bottom):
 *   CONT_A -> GAMEPAD_A   CONT_B -> GAMEPAD_B
 *   CONT_X -> GAMEPAD_X   CONT_Y -> GAMEPAD_Y
 *
 * The Dreamcast pad has analog L/R triggers (0..255), one analog stick, a
 * d-pad, Start, and A/B/X/Y. It has NO Select, NO Home, NO right stick, NO
 * stick clicks, and NO digital shoulder bumpers — so those engine slots are
 * zeroed each frame so scripts never read stale state.
 *
 * maple_dev_status() is a non-blocking read of the last polled controller
 * state; KOS polls the maple bus on its own timer, so there is nothing to
 * wait on here.
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

void INP_Initialize()
{
    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = true;
    input.mNumControllers = 1;

    InputInit();
    LogDebug("Input_DC: maple controller input initialised");
}

void INP_Shutdown()
{
    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;
    GamepadState& gp = input.mGamepads[0];

    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t* st = dev ? (cont_state_t*)maple_dev_status(dev) : nullptr;
    gp.mConnected = (st != nullptr);

    if (st != nullptr)
    {
        const uint32_t b = st->buttons;

        // Face buttons (direct diamond mapping — see file comment).
        gp.mButtons[GAMEPAD_A] = (b & CONT_A) ? 1 : 0;
        gp.mButtons[GAMEPAD_B] = (b & CONT_B) ? 1 : 0;
        gp.mButtons[GAMEPAD_X] = (b & CONT_X) ? 1 : 0;
        gp.mButtons[GAMEPAD_Y] = (b & CONT_Y) ? 1 : 0;

        // D-pad.
        gp.mButtons[GAMEPAD_UP]    = (b & CONT_DPAD_UP)    ? 1 : 0;
        gp.mButtons[GAMEPAD_DOWN]  = (b & CONT_DPAD_DOWN)  ? 1 : 0;
        gp.mButtons[GAMEPAD_LEFT]  = (b & CONT_DPAD_LEFT)  ? 1 : 0;
        gp.mButtons[GAMEPAD_RIGHT] = (b & CONT_DPAD_RIGHT) ? 1 : 0;

        // Start only — the Dreamcast pad has no Select/Home/back button.
        gp.mButtons[GAMEPAD_START]  = (b & CONT_START) ? 1 : 0;
        gp.mButtons[GAMEPAD_SELECT] = 0;
        gp.mButtons[GAMEPAD_HOME]   = 0;

        // No digital bumpers on the DC pad; the analog triggers below double as
        // the L2/R2 trigger buttons once pressed past a threshold.
        gp.mButtons[GAMEPAD_L1] = 0;
        gp.mButtons[GAMEPAD_R1] = 0;
        gp.mButtons[GAMEPAD_L2] = (st->ltrig > 64) ? 1 : 0;
        gp.mButtons[GAMEPAD_R2] = (st->rtrig > 64) ? 1 : 0;

        // No stick clicks or right stick on DC hardware.
        gp.mButtons[GAMEPAD_THUMBL] = 0;
        gp.mButtons[GAMEPAD_THUMBR] = 0;
        gp.mButtons[GAMEPAD_R_LEFT]  = 0;
        gp.mButtons[GAMEPAD_R_RIGHT] = 0;
        gp.mButtons[GAMEPAD_R_UP]    = 0;
        gp.mButtons[GAMEPAD_R_DOWN]  = 0;

        // Analog stick. DC reports joyx/joyy in -128..127 with 0 at rest. joyx
        // grows rightward; joyy grows DOWNWARD (screen-y), but engine convention
        // is +Y = up, so invert Y. A 0.30 deadzone kills rest drift that would
        // otherwise wobble the virtual L_UP/L_DOWN nav buttons (see Input_PSP for
        // the full rationale).
        constexpr float kAnalogDeadzone = 0.30f;
        auto applyDeadzone = [](float v) -> float {
            if (v >  kAnalogDeadzone) return (v - kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
            if (v < -kAnalogDeadzone) return (v + kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
            return 0.0f;
        };

        const float ax = float(st->joyx) / 128.0f;
        const float ay = float(st->joyy) / 128.0f;
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(applyDeadzone( ax), -1.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(applyDeadzone(-ay), -1.0f, 1.0f);

        gp.mAxes[GAMEPAD_AXIS_RTHUMB_X] = 0.0f;
        gp.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = 0.0f;

        // Analog triggers, 0..255 -> 0..1.
        gp.mAxes[GAMEPAD_AXIS_LTRIGGER] = float(st->ltrig) / 255.0f;
        gp.mAxes[GAMEPAD_AXIS_RTRIGGER] = float(st->rtrig) / 255.0f;
    }

    InputPostUpdate();
}

// The Dreamcast has no cursor / mouse / soft-keyboard surface in the engine's
// interface, but the symbols are required for link.
void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
