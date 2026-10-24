#pragma once

// Dialogs over the screens (x4-design.md), one at a time, in the ui task:
// a choice (a title, a button per answer) or a question with a held Yes (a
// title, two lines, a note; No at once, Yes held dialog_logic.h kHoldMs, its
// fill growing from the left, the text white on it). The page under it takes
// no taps. Answers come out of Poll, once; the dialog closes on its answer.

#include <cstddef>
#include <cstdint>

#include "dialog_logic.h"
#include "lvgl.h"

namespace ui::dialog {

inline constexpr int kNone = 0;  // no answer yet (an answer's id is never 0)

struct Button {
    const char *label;
    int id;
};

// Where dialogs open: on `screen`, under the status bar (`top` px).
void Start(lv_obj_t *screen, int32_t top, int32_t width);

// A choice: a tap on a button answers its id. The last button is what Right
// (Cancel) answers.
void Choose(const char *title, const Button *buttons, size_t count);

// A question: No (`no_id`) at once, `yes` held (`yes_id`). `first`, `second`:
// lines under the title (nullptr: none); `note` wraps.
void Confirm(const char *title, const char *first, const char *second, const char *note,
             const char *yes, int no_id, int yes_id);

bool IsOpen();
// Closed with no answer (the page left).
void Close();
// Right pressed: the cancel answer (Confirm's No, Choose's last button).
void Cancel();

// The ui task's timer: the held fill drawn; the answer once (closed), else kNone.
int Poll();

// The framed button the dialogs use (and the pages): `width` x 72, an
// outline (the content area is the whole button), black on white.
lv_obj_t *FramedButton(lv_obj_t *parent, int32_t width, const char *text);

}  // namespace ui::dialog
