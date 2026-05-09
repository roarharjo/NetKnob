#include "scr_group_menu.h"
#include "display.h"
#include "haptic.h"
#include <lvgl.h>

#define MAX_SCREENS_PER_GROUP 8

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_title = NULL;
static lv_obj_t* lbl_items[MAX_SCREENS_PER_GROUP] = {};

static ScreenGroup current_group = GROUP_WIFI;
static ScreenId group_screens[MAX_SCREENS_PER_GROUP];
static const char* group_screen_names[MAX_SCREENS_PER_GROUP];
static uint8_t group_screen_count = 0;
static uint8_t cursor = 0;

static void populate_group(ScreenGroup g) {
    group_screen_count = 0;
    current_group = g;
    switch (g) {
        case GROUP_WIFI:
            group_screens[0] = SCREEN_WIFI_SCAN;
            group_screen_names[0] = "Scanner";
            group_screens[1] = SCREEN_BEACON_FLOOD;
            group_screen_names[1] = "Beacon Flood";
            group_screens[2] = SCREEN_PROBE_SNIFF;
            group_screen_names[2] = "Probe Sniffer";
            group_screen_count = 3;
            break;
        case GROUP_BLE:
            group_screens[0] = SCREEN_BLE_SCAN;
            group_screen_names[0] = "Scanner";
            group_screen_count = 1;
            break;
        case GROUP_SYSTEM:
            group_screens[0] = SCREEN_SETTINGS;
            group_screen_names[0] = "Settings";
            group_screens[1] = SCREEN_DEBUG;
            group_screen_names[1] = "Debug";
            group_screen_count = 2;
            break;
        default: break;
    }
}

static void update_display() {
    static const char* group_titles[] = { "WiFi", "BLE", "System" };
    lv_label_set_text(lbl_title, group_titles[current_group]);

    for (int i = 0; i < MAX_SCREENS_PER_GROUP; i++) {
        if (i < group_screen_count) {
            lv_label_set_text(lbl_items[i], group_screen_names[i]);
            lv_obj_clear_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
            if (i == cursor) {
                lv_obj_set_style_text_color(lbl_items[i], COL_CYAN, 0);
                lv_obj_set_style_text_font(lbl_items[i], &lv_font_montserrat_28, 0);
            } else {
                lv_obj_set_style_text_color(lbl_items[i], COL_GRAY, 0);
                lv_obj_set_style_text_font(lbl_items[i], &lv_font_montserrat_20, 0);
            }
        } else {
            lv_obj_add_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < MAX_SCREENS_PER_GROUP; i++) {
        lbl_items[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_align(lbl_items[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_items[i], LV_ALIGN_TOP_MID, 0, 130 + i * 50);
        lv_obj_add_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show() {
    const NavigationState* nav = navigation_get_state();
    populate_group(nav->active_group);
    cursor = nav->screen_selection[current_group];
    if (cursor >= group_screen_count) cursor = 0;
    update_display();
    lv_screen_load(scr_root);
}

static void hide() {}

static void update() {}

void scr_group_menu_on_encoder(int8_t delta) {
    if (group_screen_count == 0) return;
    cursor = ((int8_t)cursor + delta + group_screen_count) % group_screen_count;
    navigation_set_screen_selection(current_group, cursor);
    update_display();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_group_menu_on_tap() {
    if (group_screen_count == 0) return;
    haptic_double_click();
    navigation_goto(group_screens[cursor]);
}

const ScreenDef scr_group_menu_def = {
    .name = "Group Menu",
    .group = GROUP_SYSTEM,
    .id = SCREEN_GROUP_MENU,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_MENU
};
