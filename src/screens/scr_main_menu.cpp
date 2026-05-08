#include "scr_main_menu.h"
#include "display.h"
#include "haptic.h"
#include <lvgl.h>

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_groups[GROUP_COUNT] = {};
static lv_obj_t* lbl_title = NULL;
static uint8_t cursor = 0;

static const char* group_names[] = { "WiFi", "BLE", "System" };

static void update_highlight() {
    for (int i = 0; i < GROUP_COUNT; i++) {
        if (i == cursor) {
            lv_obj_set_style_text_color(lbl_groups[i], COL_CYAN, 0);
            lv_obj_set_style_text_font(lbl_groups[i], &lv_font_montserrat_28, 0);
        } else {
            lv_obj_set_style_text_color(lbl_groups[i], COL_GRAY, 0);
            lv_obj_set_style_text_font(lbl_groups[i], &lv_font_montserrat_20, 0);
        }
    }
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, "NetKnob");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 40);

    int y_start = 130;
    int y_step = 60;
    for (int i = 0; i < GROUP_COUNT; i++) {
        lbl_groups[i] = lv_label_create(scr_root);
        lv_label_set_text(lbl_groups[i], group_names[i]);
        lv_obj_set_style_text_align(lbl_groups[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_groups[i], LV_ALIGN_TOP_MID, 0, y_start + i * y_step);
    }

    update_highlight();
}

static void show() {
    const NavigationState* nav = navigation_get_state();
    cursor = nav->group_selection;
    update_highlight();
    lv_screen_load(scr_root);
}

static void hide() {}

static void update() {}

void scr_main_menu_on_encoder(int8_t delta) {
    cursor = ((int8_t)cursor + delta + GROUP_COUNT) % GROUP_COUNT;
    navigation_set_group_selection(cursor);
    update_highlight();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_main_menu_on_tap() {
    haptic_double_click();
    navigation_set_active_group((ScreenGroup)cursor);
    navigation_goto(SCREEN_GROUP_MENU);
}

const ScreenDef scr_main_menu_def = {
    .name = "Main Menu",
    .group = GROUP_SYSTEM,
    .id = SCREEN_MAIN_MENU,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_MENU
};
