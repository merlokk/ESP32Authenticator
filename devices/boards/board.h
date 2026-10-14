#pragma once

// The board this firmware is built for, chosen in devices/config.h at build
// time. Pure: `boards::kBoard` is constexpr, so a bad pin map fails the build.

#include "../config.h"
#include "board_spec.h"
#include "dev_board.h"
#include "x4_pro.h"

namespace boards {

// BoardId -> class. A new board: a class, an id, and a line here.
template <BoardId>
struct BoardFor;
template <>
struct BoardFor<BoardId::kDev> {
    using Type = DevBoard;
};
template <>
struct BoardFor<BoardId::kX4Pro> {
    using Type = X4Pro;
};

using Selected = BoardFor<kBoardId>::Type;
inline constexpr Selected kBoard;

static_assert(CheckPins(kBoard) == nullptr, "devices/config.h: the board's pin map is invalid");

}  // namespace boards
