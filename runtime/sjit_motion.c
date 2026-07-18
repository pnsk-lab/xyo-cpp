#include "sjit_motion.h"

#include "sjit_number.h"

void sjit_motion_goto_xy(SRuntime *runtime, SSprite *sprite, SValue x, SValue y) {
    sjit_sprite_set_xy(runtime, sprite, sjit_to_number_fast(runtime, x), sjit_to_number_fast(runtime, y), 0);
}

void sjit_motion_point_in_direction(SRuntime *runtime, SSprite *sprite, SValue direction) {
    sjit_sprite_set_direction(runtime, sprite, sjit_to_number_fast(runtime, direction));
}
