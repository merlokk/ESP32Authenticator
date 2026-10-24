#include "dialog.h"

// One dialog at a time on the home screen: a full-width layer under the
// status bar takes the taps around the box; the box is a column (flex).

namespace ui::dialog {

namespace {

constexpr int32_t kPad = 20;
constexpr int32_t kBorder = 3;
constexpr int32_t kButtonHeight = 72;
constexpr int32_t kNoWidth = 120;

lv_obj_t *screen = nullptr;
int32_t layer_top = 0;
int32_t box_width = 0;

lv_obj_t *layer = nullptr;
int answer = kNone;  // set by a button, taken by Poll
int cancel_id = kNone;
// Confirm's held Yes.
bool confirming = false;
int yes_answer = kNone;
Hold hold;
lv_obj_t *yes_fill = nullptr;
int32_t yes_width = 0;
uint8_t shown_fill = 0;

lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    return l;
}

int32_t Inner() { return box_width - 2 * kPad - 2 * kBorder; }

void OnButton(lv_event_t *event) {
    answer = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

// Counts only while held.
void OnYes(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        hold.Press(lv_tick_get());
    } else {
        hold.Release();
    }
}

// The layer and the box with the title; the box's column is filled by the caller.
lv_obj_t *OpenBox(const char *title) {
    Close();
    layer = lv_obj_create(screen);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(layer, 0, layer_top);
    lv_obj_set_size(layer, lv_obj_get_width(screen), lv_obj_get_height(screen) - layer_top);
    lv_obj_t *box = lv_obj_create(layer);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, box_width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, kBorder, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_pad_all(box, kPad, 0);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(box);
    lv_obj_t *t = Label(box, &lv_font_montserrat_28);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, Inner());
    lv_label_set_text(t, title);
    answer = kNone;
    confirming = false;
    hold.Reset();
    return box;
}

}  // namespace

lv_obj_t *FramedButton(lv_obj_t *parent, int32_t width, const char *text) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, width, kButtonHeight);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 10, 0);
    // An outline, not a border: the content area is the whole button (a fill's).
    lv_obj_set_style_outline_width(b, 2, 0);
    lv_obj_set_style_outline_color(b, lv_color_black(), 0);
    lv_obj_t *l = Label(b, &lv_font_montserrat_24);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void Start(lv_obj_t *on, int32_t top, int32_t width) {
    screen = on;
    layer_top = top;
    box_width = width;
}

void Choose(const char *title, const Button *buttons, size_t count) {
    lv_obj_t *box = OpenBox(title);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *b = FramedButton(box, Inner() - 4, buttons[i].label);
        lv_obj_add_event_cb(b, OnButton, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(buttons[i].id)));
    }
    cancel_id = count > 0 ? buttons[count - 1].id : kNone;
}

void Confirm(const char *title, const char *first, const char *second, const char *note,
             const char *yes, int no_id, int yes_id) {
    lv_obj_t *box = OpenBox(title);
    const char *const lines[2] = {first, second};
    for (const char *line : lines) {
        if (line == nullptr) {
            continue;
        }
        lv_obj_t *l = Label(box, &lv_font_montserrat_24);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, Inner());
        lv_label_set_text(l, line);
    }
    lv_obj_t *n = Label(box, &lv_font_montserrat_20);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(n, Inner());
    lv_label_set_text(n, note);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, Inner(), kButtonHeight + 8);
    lv_obj_t *no = FramedButton(row, kNoWidth, "No");
    lv_obj_align(no, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(no, OnButton, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(no_id)));
    yes_width = Inner() - kNoWidth - 20;
    lv_obj_t *y = FramedButton(row, yes_width, yes);
    lv_obj_align(y, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_remove_flag(y, LV_OBJ_FLAG_PRESS_LOCK);  // sliding off it lets go
    lv_obj_add_event_cb(y, OnYes, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(y, OnYes, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(y, OnYes, LV_EVENT_PRESS_LOST, nullptr);
    // The fill clips a white copy of the button's face to its width.
    yes_fill = lv_obj_create(y);
    lv_obj_remove_style_all(yes_fill);
    lv_obj_remove_flag(yes_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(yes_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(yes_fill, 0, kButtonHeight);
    lv_obj_set_style_bg_color(yes_fill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(yes_fill, LV_OPA_COVER, 0);
    lv_obj_t *face = lv_obj_create(yes_fill);
    lv_obj_remove_style_all(face);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(face, yes_width, kButtonHeight);
    lv_obj_t *l = Label(face, &lv_font_montserrat_24);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, yes);
    lv_obj_center(l);
    shown_fill = 0;
    confirming = true;
    yes_answer = yes_id;
    cancel_id = no_id;
}

bool IsOpen() { return layer != nullptr; }

void Close() {
    if (layer != nullptr) {
        lv_obj_delete(layer);  // a held Yes under the finger: LVGL lets go of it
        layer = nullptr;
    }
    yes_fill = nullptr;
    confirming = false;
    hold.Reset();
    answer = kNone;
}

void Cancel() {
    if (layer != nullptr) {
        answer = cancel_id;
    }
}

int Poll() {
    if (layer == nullptr) {
        return kNone;
    }
    if (confirming && answer == kNone) {
        const uint8_t fill = hold.Fill(lv_tick_get());
        if (hold.done()) {
            answer = yes_answer;
        } else if (fill != shown_fill) {
            shown_fill = fill;
            lv_obj_set_width(yes_fill, yes_width * fill / kHoldSteps);
        }
    }
    const int a = answer;
    if (a != kNone) {
        Close();
    }
    return a;
}

}  // namespace ui::dialog
