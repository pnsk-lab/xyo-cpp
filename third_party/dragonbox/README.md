# Dragonbox and V8-derived numeric formatting

Dragonbox was vendored from
[jk-jeon/dragonbox](https://github.com/jk-jeon/dragonbox),
file include/dragonbox/dragonbox.h, Git blob
84d6e24e3f49c6ad2952bec9ac53cfeabb5ebc2c
(master branch as retrieved on 2026-11-07).
Copyright 2020–2025 Junekey Jeon. It is distributed under the
Boost Software License 1.0 (LICENSE-Boost). The original header retains
its upstream dual-license notice (Apache-2.0 with LLVM Exceptions OR BSL-1.0).

The number-to-decimal digit formatting functions in
src/core/v8_decimal.cpp are adapted from the V8 project's
src/numbers/conversions.cc, Git blob
cfbaf358521c6c5a2d5f858d8063ab44039e6664
(main branch as retrieved on 2026-11-07).
Copyright 2011 the V8 project authors. All rights reserved.
The V8 BSD-style license is reproduced in LICENSE-V8.

The Dragonbox result is used as (integer significand, integer exponent),
then the V8-derived SignificandToChars helpers generate digits. Scratch's
existing exceptional-value rules and small-integer fast path are retained.

Build option XYO_NUM_TO_STR_DRAGONBOX defaults to ON. It does not fetch
any dependencies at build time. Other modules can build without this header.
