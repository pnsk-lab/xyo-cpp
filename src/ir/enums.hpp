// xyo-cpp: IR 列挙 — refs/scratch-vm/src/compiler/enums.js の移植。
// 値は TurboWarp と一致させる（JS 側のツールとスナップショットを比較するため）。
#pragma once

#include <cstdint>

namespace xyo::ir {

/// 入力式の「取りうる型」ビットマスク。enums.js InputType。
enum class Type : uint16_t {
    NUMBER_POS_INF   = 0x001,
    NUMBER_POS_INT   = 0x002,
    NUMBER_POS_FRACT = 0x004,
    NUMBER_POS_REAL  = 0x006,   // POS_INT | POS_FRACT
    NUMBER_ZERO      = 0x008,
    NUMBER_NEG_ZERO  = 0x010,
    NUMBER_NEG_INT   = 0x020,
    NUMBER_NEG_FRACT = 0x040,
    NUMBER_NEG_REAL  = 0x060,   // NEG_INT | NEG_FRACT
    NUMBER_NEG_INF   = 0x080,
    NUMBER_NAN       = 0x100,
    NUMBER_ANY_ZERO  = 0x018,   // ZERO | NEG_ZERO
    NUMBER_INF       = 0x081,   // POS_INF | NEG_INF
    NUMBER_POS       = 0x007,   // POS_REAL | POS_INF
    NUMBER_NEG       = 0x0E0,   // NEG_REAL | NEG_INF
    NUMBER_WHOLE     = 0x00A,   // POS_INT | ZERO
    NUMBER_INT       = 0x03A,   // POS_INT | ANY_ZERO | NEG_INT
    NUMBER_INDEX     = 0x1BB,   // INT | INF | NAN
    NUMBER_FRACT     = 0x044,
    NUMBER_REAL      = 0x07E,   // POS_REAL | ANY_ZERO | NEG_REAL
    NUMBER           = 0x0FF,   // REAL | INF（NaN 除く）
    NUMBER_OR_NAN    = 0x1FF,   // NUMBER | NAN
    NUMBER_INTERPRETABLE = 0x12FF, // NUMBER | STRING_NUM | BOOLEAN

    STRING_NUM       = 0x200,
    STRING_NAN       = 0x400,
    STRING_BOOLEAN   = 0x800,
    STRING           = 0xE00,   // NUM | NAN | BOOLEAN

    BOOLEAN          = 0x1000,
    BOOLEAN_INTERPRETABLE = 0x1800, // BOOLEAN | STRING_BOOLEAN

    ANY              = 0x1FFF,  // NUMBER_OR_NAN | STRING | BOOLEAN
    COLOR            = 0x2000,
};

constexpr Type operator|(Type a, Type b) {
    return static_cast<Type>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr Type operator&(Type a, Type b) {
    return static_cast<Type>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr bool always(Type t, Type want) {
    return (static_cast<uint16_t>(t) & static_cast<uint16_t>(want)) == static_cast<uint16_t>(t);
}
constexpr bool sometimes(Type t, Type want) {
    return (static_cast<uint16_t>(t) & static_cast<uint16_t>(want)) != 0;
}

/// スタックブロック IR opcode。enums.js StackOpcode。
enum class StackOp : uint8_t {
    Nop,
    AddonCall,
    Debugger,
    VisualReport,
    CompatLayer,
    HatEdge,
    HatPredicate,
    ControlIfElse,
    ControlCloneCreate,
    ControlCloneDelete,
    ControlWhile,
    ControlFor,
    ControlRepeat,
    ControlStopAll,
    ControlStopOthers,
    ControlStopScript,
    ControlWait,
    ControlWaitUntil,
    ControlClearCounter,
    ControlIncrCounter,
    ListAdd,
    ListInsert,
    ListReplace,
    ListDeleteAll,
    ListDelete,
    ListShow,
    ListHide,
    VarSet,
    VarShow,
    VarHide,
    EventBroadcast,
    EventBroadcastAndWait,
    LooksEffectSet,
    LooksEffectChange,
    LooksEffectClear,
    LooksSizeChange,
    LooksSizeSet,
    LooksLayerForward,
    LooksLayerBackward,
    LooksLayerFront,
    LooksLayerBack,
    LooksHide,
    LooksShow,
    LooksBackdropNext,
    LooksBackdropSet,
    LooksCostumeNext,
    LooksCostumeSet,
    LooksSay,
    LooksThink,
    MotionXSet,
    MotionXChange,
    MotionYSet,
    MotionYChange,
    MotionXYSet,
    MotionIfOnEdgeBounce,
    MotionStep,
    MotionRotationStyleSet,
    MotionDirectionSet,
    PenUp,
    PenDown,
    PenClear,
    PenColorParamSet,
    PenColorParamChange,
    PenColorHueChangeLegacy,
    PenColorHueSetLegacy,
    PenColorShadeChangeLegacy,
    PenColorShadeSetLegacy,
    PenColorSet,
    PenSizeSet,
    PenSizeChange,
    PenStamp,
    SensingTimerReset,
    ProcedureReturn,
    ProcedureCall,
};

/// 入力式 IR opcode。enums.js InputOpcode。
enum class InputOp : uint8_t {
    Nop,
    AddonCall,
    Constant,
    CastNumber,
    CastNumberIndex,
    CastNumberOrNan,
    CastString,
    CastBoolean,
    CastColor,
    CompatLayer,
    LooksBackdropNumber,
    LooksBackdropName,
    LooksCostumeNumber,
    LooksCostumeName,
    LooksSizeGet,
    VarGet,
    ListGet,
    ListLength,
    ListContains,
    ListIndexOf,
    ListContents,
    MotionXGet,
    MotionYGet,
    MotionDirectionGet,
    OpAdd,
    OpAnd,
    OpContains,
    OpDivide,
    OpEquals,
    OpGreater,
    OpJoin,
    OpLength,
    OpLess,
    OpLetterOf,
    OpAbs,
    OpFloor,
    OpCeiling,
    OpSqrt,
    OpSin,
    OpCos,
    OpTan,
    OpAsin,
    OpAcos,
    OpAtan,
    OpLogE,
    OpLog10,
    OpPowE,
    OpPow10,
    OpMod,
    OpMultiply,
    OpNot,
    OpOr,
    OpRandom,
    OpRound,
    OpSubtract,
    SensingAnswer,
    SensingColorTouchingColor,
    SensingTimeYear,
    SensingTimeMonth,
    SensingTimeDate,
    SensingTimeWeekday,
    SensingTimeHour,
    SensingTimeMinute,
    SensingTimeSecond,
    SensingTimeDaysSince2000,
    SensingDistance,
    SensingKeyDown,
    SensingMouseDown,
    SensingMouseX,
    SensingMouseY,
    SensingOf,
    SensingOfBackdropName,
    SensingOfBackdropNumber,
    SensingOfCostumeName,
    SensingOfCostumeNumber,
    SensingOfVolume,
    SensingOfPosX,
    SensingOfPosY,
    SensingOfDirection,
    SensingOfSize,
    SensingOfVar,
    SensingTimerGet,
    SensingTouchingColor,
    SensingTouchingObject,
    SensingUsername,
    ProcedureCall,
    ProcedureArgument,
    ControlCounter,
    TwKeyLastPressed,
};

} // namespace xyo::ir
