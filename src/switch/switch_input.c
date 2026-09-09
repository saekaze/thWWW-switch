#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "switch_input.h"
#include "platformdefs.h"

#define SWITCH_NPAD_COUNT 8

static PadState pads[SWITCH_NPAD_COUNT];
static bool initialized = false;

static void mapLibnxToGml(GamepadSlot* slot, PadState* pad, u64 cur) {
    // thWWW-switch deliberately uses the same fixed physical layout as
    // Saekaze's Touhou 7 port instead of exposing GameMaker's configurable
    // pad layout. The slots below are thWWW's authored logical actions:
    // FACE1=shoot, FACE2=bomb, FACE3=focus, SHOULDERL=pause.
    if (cur & HidNpadButton_B) slot->buttonDown[0] = true;
    if (cur & HidNpadButton_A) slot->buttonDown[1] = true;
    if (cur & HidNpadButton_L) slot->buttonDown[2] = true;
    if (cur & HidNpadButton_Plus) slot->buttonDown[4] = true;

    // R is kept on its own otherwise-unused logical slot. The VM exposes it
    // to obj_dialogue as held skip without aliasing it to gameplay shooting.
    if (cur & HidNpadButton_R) slot->buttonDown[5] = true;

    // Both the left stick and D-Pad provide movement. All remaining Switch
    // controls are intentionally inert.
    if (cur & HidNpadButton_Up) slot->buttonDown[12] = true;
    if (cur & HidNpadButton_Down) slot->buttonDown[13] = true;
    if (cur & HidNpadButton_Left) slot->buttonDown[14] = true;
    if (cur & HidNpadButton_Right) slot->buttonDown[15] = true;

    HidAnalogStickState l = padGetStickPos(pad, 0);
    slot->axisValue[0] = l.x / 32767.0f;
    slot->axisValue[1] = -l.y / 32767.0f;
}

static void fillGamepadSlot(GamepadSlot* slot, PadState* pad, u64 cur, bool connected, int jid, const char* guid) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDownPrev));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    slot->connected = connected;
    slot->jid = jid;
    strncpy(slot->description, "Nintendo Switch Controller", sizeof(slot->description) - 1);
    strncpy(slot->guid, guid, sizeof(slot->guid) - 1);

    if (connected) mapLibnxToGml(slot, pad, cur);

    for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
        bool wasDown = slot->buttonDownPrev[btn];
        if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
        if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
    }
}

bool SwitchInput_handleEvents(struct Runner* runner) {
    if (!initialized) {
        padConfigureInput(SWITCH_NPAD_COUNT, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pads[0]);
        for (int i = 1; SWITCH_NPAD_COUNT > i; i++) {
            padInitialize(&pads[i], HidNpadIdType_No1 + i);
        }
        initialized = true;
    }

    if (!appletMainLoop()) return true;

    int connectedCount = 0;
    for (int i = 0; SWITCH_NPAD_COUNT > i; i++) {
        padUpdate(&pads[i]);
        u64 cur = pads[i].buttons_cur;
        bool connected = padIsConnected(&pads[i]);

        char guid[32];
        snprintf(guid, sizeof(guid), "switch-nx-%d", i + 1);
        fillGamepadSlot(&runner->gamepads->slots[i], &pads[i], cur, connected, i, guid);

        if (connected) connectedCount++;
    }

    runner->gamepads->connectedCount = connectedCount;

    return false;
}
