// IR-001: conservative interpreter-side Scratch VM-write classification.
// Used at the central flat-interpreter primitive dispatch. Fail-closed
// diagnostic evidence only; none of these operations is suppressed.
// This recognizes the known Scratch data, motion, looks, pen, sound,
// scheduler and sensing mutations in the built-in native primitives.
// Unknown extension/native callbacks require separate conservative guards.
#pragma once

#include <string_view>

namespace xyo::ir {

constexpr bool isInterpreterDataMutation(std::string_view opcode) {
    return opcode == "data_setvariableto" ||
           opcode == "data_changevariableby" ||
           opcode == "data_addtolist" ||
           opcode == "data_deleteoflist" ||
           opcode == "data_deletealloflist" ||
           opcode == "data_insertatlist" ||
           opcode == "data_replaceitemoflist";
}

// An explicit allowlist of known state-changing BUILT-IN interpreter
// primitives. Read-only reporters, argument reporters, pure operators,
// and loop controls are not treated as a state mutation by this list.
// This intentionally permits false-positive diagnostic denials (a
// no-op or rejected index), but never suppresses a real Scratch effect.
constexpr bool isInterpreterOtherMutation(std::string_view opcode) {
    constexpr std::string_view mutators[] = {
        // Scratch monitors.
        "data_showvariable", "data_hidevariable",
        "data_showlist", "data_hidelist",

        // Motion/target geometry and rotation.
        "motion_movesteps", "motion_turnright", "motion_turnleft",
        "motion_pointindirection", "motion_pointtowards",
        "motion_changexby", "motion_setx", "motion_changeyby",
        "motion_sety", "motion_gotoxy", "motion_goto",
        "motion_glidesecstoxy", "motion_glideto",
        "motion_ifonedgebounce", "motion_setrotationstyle",

        // Costume, bubble, appearance, layer and shared backdrop.
        "looks_say", "looks_sayforsecs", "looks_think",
        "looks_thinkforsecs", "looks_show", "looks_hide",
        "looks_switchcostumeto", "looks_nextcostume",
        "looks_switchbackdropto", "looks_switchbackdroptoandwait",
        "looks_nextbackdrop", "looks_changeeffectby",
        "looks_seteffectto", "looks_cleargraphiceffects",
        "looks_changesizeby", "looks_setsizeto",
        "looks_gotofrontback", "looks_goforwardbackwardlayers",

        // Pen layer and pen style; includes pen_down/up and legacy hue.
        "pen_clear", "pen_stamp", "pen_penDown", "pen_penUp",
        "pen_setPenColorToColor", "pen_changePenColorParamBy",
        "pen_setPenColorParamTo", "pen_changePenSizeBy",
        "pen_setPenSizeTo", "pen_setPenShadeToNumber",
        "pen_changePenShadeBy", "pen_setPenHueToNumber",
        "pen_changePenHueBy",

        // Sound playback, effects and volume.
        "sound_play", "sound_playuntildone", "sound_stopallsounds",
        "sound_seteffectto", "sound_changeeffectby",
        "sound_cleareffects", "sound_setvolumeto",
        "sound_changevolumeby",

        // Dynamic runtime, scheduler and shared state.
        "sensing_resettimer", "sensing_setdragmode",
        "sensing_askandwait", "sensing_keypressed",
        "sensing_loudness", "sensing_loud",
        "control_create_clone_of", "control_delete_this_clone",
        "control_stop", "control_wait",
        "control_incr_counter", "control_clear_counter",
        "event_broadcast", "event_broadcastandwait"
    };
    for (std::string_view candidate : mutators)
        if (opcode == candidate) return true;
    return false;
}

constexpr bool isInterpreterStateMutation(std::string_view opcode) {
    return isInterpreterDataMutation(opcode) ||
           isInterpreterOtherMutation(opcode);
}

} // namespace xyo::ir
