/**
 * @file ui.c
 * @brief LVGL UI for the 320x480 BodyGuard safety companion terminal.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "data_def.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_puhui_16_2);
LV_FONT_DECLARE(font_puhui_20_2);

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define UI_SCREEN_W             320
#define UI_SCREEN_H             480
#define UI_TOP_H                40
#define UI_COMPANION_H          240
#define UI_VITAL_H              80
#define UI_REPORT_H             120

#define UI_BG_COLOR             lv_color_hex(0x1F2937)
#define UI_TOP_COLOR            lv_color_hex(0x212121)
#define UI_CORE_COLOR           lv_color_hex(0x000000)
#define UI_REPORT_COLOR         lv_color_hex(0x262626)
#define UI_TEXT_COLOR           lv_color_hex(0xF8FAFC)
#define UI_SUB_TEXT_COLOR       lv_color_hex(0x9CA3AF)
#define UI_MUTED_COLOR          lv_color_hex(0x6B7280)
#define UI_GREEN                lv_color_hex(0x22C55E)
#define UI_GREEN_DARK           lv_color_hex(0x14532D)
#define UI_ORANGE               lv_color_hex(0xF97316)
#define UI_ORANGE_DARK          lv_color_hex(0x7C2D12)
#define UI_RED                  lv_color_hex(0xEF4444)
#define UI_RED_DARK             lv_color_hex(0x7F1D1D)
#define UI_BLUE                 lv_color_hex(0x38BDF8)

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    bool ready;
    lv_obj_t *root;
    lv_obj_t *page_host;
    lv_obj_t *page_monitor;
    lv_obj_t *page_learning;
    lv_obj_t *top_bar;
    lv_obj_t *core_area;
    lv_obj_t *vital_area;
    lv_obj_t *report_area;
    lv_obj_t *ble_dot;
    lv_obj_t *cloud_dot;
    lv_obj_t *mode_pill;
    lv_obj_t *mode_label;
    lv_obj_t *bubble;
    lv_obj_t *bubble_label;
    lv_obj_t *companion_glow;
    lv_obj_t *companion_body;
    lv_obj_t *companion_head;
    lv_obj_t *companion_ear_l;
    lv_obj_t *companion_ear_r;
    lv_obj_t *companion_face;
    lv_obj_t *companion_hand_l;
    lv_obj_t *companion_hand_r;
    lv_obj_t *risk_caption;
    lv_obj_t *risk_label;
    lv_obj_t *eeg_chip;
    lv_obj_t *eeg_label;
    lv_obj_t *pose_chip;
    lv_obj_t *pose_label;
    lv_obj_t *ai_label;
    lv_obj_t *event_label;
    lv_obj_t *alert_overlay;
    lv_obj_t *alert_title;
    lv_obj_t *alert_msg;
    lv_obj_t *page_dot_monitor;
    lv_obj_t *page_dot_learning;
    lv_obj_t *learn_status_label;
    lv_obj_t *learn_notice_label;
    lv_obj_t *posture_bar;
    lv_obj_t *posture_value_label;
    lv_obj_t *eeg_bar;
    lv_obj_t *eeg_value_label;
    lv_obj_t *burst_bar;
    lv_obj_t *burst_value_label;
    lv_obj_t *warning_value_label;
    lv_obj_t *danger_value_label;
    lv_obj_t *false_alarm_value_label;
    lv_obj_t *confirmed_value_label;
    lv_obj_t *missed_value_label;
    lv_obj_t *update_value_label;
    bool learn_counter_ready;
    uint32_t last_false_alarm_count;
    uint32_t last_missed_danger_count;
    uint32_t last_learning_update_count;
    uint32_t notice_until_ms;
    char notice_text[48];
} ui_ctx_t;

static ui_ctx_t s_ui = {0};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a plain rectangle object.
 * @param[in] parent Parent object.
 * @param[in] x X coordinate.
 * @param[in] y Y coordinate.
 * @param[in] w Width.
 * @param[in] h Height.
 * @param[in] color Background color.
 * @param[in] radius Corner radius.
 * @return Created object.
 */
static lv_obj_t *__rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t color, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    return obj;
}

/**
 * @brief Create a label.
 * @param[in] parent Parent object.
 * @param[in] font Label font.
 * @param[in] color Text color.
 * @param[in] text Initial text.
 * @return Created label.
 */
static lv_obj_t *__label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/**
 * @brief Create a compact value card.
 * @param[in] parent Parent object.
 * @param[in] x X coordinate.
 * @param[in] y Y coordinate.
 * @param[in] w Width.
 * @param[in] h Height.
 * @param[in] title Card title.
 * @return Value label inside the card.
 */
static lv_obj_t *__value_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *title)
{
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *value_label;

    card = __rect(parent, x, y, w, h, lv_color_hex(0x111827), 8);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x374151), LV_PART_MAIN);

    title_label = __label(card, &font_puhui_16_2, UI_SUB_TEXT_COLOR, title);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 4);

    value_label = __label(card, &font_puhui_20_2, UI_TEXT_COLOR, "0");
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    return value_label;
}

/**
 * @brief Create a learning weight row with a progress bar.
 * @param[in] parent Parent object.
 * @param[in] y Y coordinate.
 * @param[in] title Row title.
 * @param[out] bar Created bar.
 * @param[out] value_label Created value label.
 * @return none
 */
static void __weight_row(lv_obj_t *parent, int32_t y, const char *title, lv_obj_t **bar, lv_obj_t **value_label)
{
    lv_obj_t *title_label;

    title_label = __label(parent, &font_puhui_16_2, UI_TEXT_COLOR, title);
    lv_obj_set_pos(title_label, 12, y);

    *bar = lv_bar_create(parent);
    lv_obj_set_pos(*bar, 92, y + 3);
    lv_obj_set_size(*bar, 150, 12);
    lv_bar_set_range(*bar, 0, 100);
    lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*bar, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_bg_color(*bar, UI_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(*bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(*bar, 6, LV_PART_INDICATOR);

    *value_label = __label(parent, &font_puhui_16_2, UI_SUB_TEXT_COLOR, "0%");
    lv_obj_set_pos(*value_label, 252, y - 2);
}

/**
 * @brief Get risk display color.
 * @param[in] risk Risk score.
 * @return LVGL color.
 */
static lv_color_t __risk_color(uint8_t risk)
{
    if (risk >= 71) {
        return UI_RED;
    }
    if (risk >= 31) {
        return UI_ORANGE;
    }
    return UI_GREEN;
}

/**
 * @brief Get status color for EEG or pose state.
 * @param[in] st State value.
 * @return LVGL color.
 */
static lv_color_t __state_color(uint8_t st)
{
    if (st >= 3) {
        return UI_RED;
    }
    if (st >= 1) {
        return UI_ORANGE;
    }
    return UI_GREEN;
}

/**
 * @brief Get main companion color by system state.
 * @param[in] st System state.
 * @return LVGL color.
 */
static lv_color_t __companion_color(uint8_t st)
{
    if (st == T5_SYS_EMERGENCY) {
        return UI_RED;
    }
    if (st == T5_SYS_PRE_ALERT) {
        return UI_ORANGE;
    }
    return UI_GREEN;
}

/**
 * @brief Get dark companion color by system state.
 * @param[in] st System state.
 * @return LVGL color.
 */
static lv_color_t __companion_dark_color(uint8_t st)
{
    if (st == T5_SYS_EMERGENCY) {
        return UI_RED_DARK;
    }
    if (st == T5_SYS_PRE_ALERT) {
        return UI_ORANGE_DARK;
    }
    return UI_GREEN_DARK;
}

/**
 * @brief Get mode text by system state.
 * @param[in] st System state.
 * @return Text.
 */
static const char *__mode_text(uint8_t st)
{
    switch (st) {
    case T5_SYS_STANDBY:
        return "待机中";
    case T5_SYS_NORMAL:
        return "监护中";
    case T5_SYS_PRE_ALERT:
        return "预警";
    case T5_SYS_EMERGENCY:
        return "报警";
    default:
        return "未知";
    }
}

/**
 * @brief Get companion bubble text by system state.
 * @param[in] st System state.
 * @return Text.
 */
static const char *__bubble_text(uint8_t st)
{
    switch (st) {
    case T5_SYS_PRE_ALERT:
        return "我不喜欢这情况";
    case T5_SYS_EMERGENCY:
        return "危险！紧急！！";
    case T5_SYS_STANDBY:
        return "我在待命";
    case T5_SYS_NORMAL:
    default:
        return "一切安全！";
    }
}

/**
 * @brief Get companion face text by system state.
 * @param[in] st System state.
 * @return Text.
 */
static const char *__face_text(uint8_t st)
{
    switch (st) {
    case T5_SYS_PRE_ALERT:
        return "o_o";
    case T5_SYS_EMERGENCY:
        return "O_O";
    case T5_SYS_STANDBY:
        return "-_-";
    case T5_SYS_NORMAL:
    default:
        return "^_^";
    }
}

/**
 * @brief Get EEG text.
 * @param[in] st EEG state.
 * @return Text.
 */
static const char *__eeg_text(uint8_t st)
{
    switch (st) {
    case 0:
        return "脑电正常";
    case 1:
        return "疲劳";
    case 2:
        return "注意力下降";
    case 3:
        return "脑电异常";
    default:
        return "未知";
    }
}

/**
 * @brief Get pose text.
 * @param[in] st Pose state.
 * @return Text.
 */
static const char *__pose_text(uint8_t st)
{
    switch (st) {
    case 0:
        return "姿态正常";
    case 1:
        return "轻微晃动";
    case 2:
        return "身体倾斜";
    case 3:
        return "疑似跌倒";
    default:
        return "未知";
    }
}

/**
 * @brief Create top status bar.
 * @return none
 */
static void __create_top_bar(void)
{
    s_ui.top_bar = __rect(s_ui.page_monitor, 0, 0, UI_SCREEN_W, UI_TOP_H, UI_TOP_COLOR, 0);

    s_ui.ble_dot = __rect(s_ui.top_bar, 10, 8, 24, 24, UI_MUTED_COLOR, 12);
    s_ui.cloud_dot = __rect(s_ui.top_bar, 42, 8, 24, 24, UI_MUTED_COLOR, 12);

    s_ui.mode_pill = __rect(s_ui.top_bar, 214, 7, 96, 26, UI_MUTED_COLOR, 13);
    s_ui.mode_label = __label(s_ui.mode_pill, &font_puhui_16_2, UI_TEXT_COLOR, "待机中");
    lv_obj_center(s_ui.mode_label);
}

/**
 * @brief Create native LVGL companion bear.
 * @return none
 */
static void __create_companion(void)
{
    s_ui.bubble = __rect(s_ui.core_area, 58, 12, 204, 36, lv_color_hex(0x111827), 10);
    lv_obj_set_style_border_width(s_ui.bubble, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.bubble, UI_GREEN, LV_PART_MAIN);
    s_ui.bubble_label = __label(s_ui.bubble, &font_puhui_16_2, UI_TEXT_COLOR, "一切安全！");
    lv_obj_center(s_ui.bubble_label);

    s_ui.companion_glow = __rect(s_ui.core_area, 100, 56, 120, 120, UI_GREEN_DARK, 60);
    lv_obj_set_style_bg_opa(s_ui.companion_glow, LV_OPA_40, LV_PART_MAIN);

    s_ui.companion_ear_l = __rect(s_ui.core_area, 112, 70, 30, 30, UI_GREEN_DARK, 15);
    s_ui.companion_ear_r = __rect(s_ui.core_area, 178, 70, 30, 30, UI_GREEN_DARK, 15);
    s_ui.companion_body = __rect(s_ui.core_area, 119, 130, 82, 78, UI_GREEN, 38);
    s_ui.companion_head = __rect(s_ui.core_area, 108, 78, 104, 88, UI_GREEN, 42);
    s_ui.companion_face = __label(s_ui.companion_head, &font_puhui_20_2, lv_color_hex(0x111827), "^_^");
    lv_obj_center(s_ui.companion_face);
    s_ui.companion_hand_l = __rect(s_ui.core_area, 96, 142, 34, 24, UI_GREEN_DARK, 12);
    s_ui.companion_hand_r = __rect(s_ui.core_area, 190, 142, 34, 24, UI_GREEN_DARK, 12);
}

/**
 * @brief Create companion and risk core area.
 * @return none
 */
static void __create_core_area(void)
{
    s_ui.core_area = __rect(s_ui.page_monitor, 0, UI_TOP_H, UI_SCREEN_W, UI_COMPANION_H, UI_CORE_COLOR, 0);
    __create_companion();

    s_ui.risk_caption = __label(s_ui.core_area, &font_puhui_16_2, UI_SUB_TEXT_COLOR, "风险评分");
    lv_obj_align(s_ui.risk_caption, LV_ALIGN_BOTTOM_MID, 0, -38);

    s_ui.risk_label = __label(s_ui.core_area, &font_puhui_20_2, UI_GREEN, "000");
    lv_obj_align(s_ui.risk_label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/**
 * @brief Create EEG and pose status area.
 * @return none
 */
static void __create_vital_area(void)
{
    s_ui.vital_area = __rect(s_ui.page_monitor, 0, UI_TOP_H + UI_COMPANION_H, UI_SCREEN_W, UI_VITAL_H, UI_BG_COLOR, 0);

    s_ui.eeg_chip = __rect(s_ui.vital_area, 10, 10, 145, 60, lv_color_hex(0x111827), 8);
    lv_obj_set_style_border_width(s_ui.eeg_chip, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.eeg_chip, UI_GREEN, LV_PART_MAIN);
    s_ui.eeg_label = __label(s_ui.eeg_chip, &font_puhui_16_2, UI_TEXT_COLOR, "脑电正常");
    lv_obj_center(s_ui.eeg_label);

    s_ui.pose_chip = __rect(s_ui.vital_area, 165, 10, 145, 60, lv_color_hex(0x111827), 8);
    lv_obj_set_style_border_width(s_ui.pose_chip, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.pose_chip, UI_GREEN, LV_PART_MAIN);
    s_ui.pose_label = __label(s_ui.pose_chip, &font_puhui_16_2, UI_TEXT_COLOR, "姿态正常");
    lv_obj_center(s_ui.pose_label);
}

/**
 * @brief Create AI report area.
 * @return none
 */
static void __create_report_area(void)
{
    lv_obj_t *title;

    s_ui.report_area = __rect(s_ui.page_monitor, 0, UI_TOP_H + UI_COMPANION_H + UI_VITAL_H,
                              UI_SCREEN_W, UI_REPORT_H, UI_REPORT_COLOR, 0);

    title = __label(s_ui.report_area, &font_puhui_16_2, UI_SUB_TEXT_COLOR, "AI 分析报告");
    lv_obj_set_pos(title, 10, 8);

    s_ui.ai_label = __label(s_ui.report_area, &font_puhui_16_2, UI_TEXT_COLOR, "等待AI分析...");
    lv_obj_set_pos(s_ui.ai_label, 10, 32);
    lv_obj_set_width(s_ui.ai_label, 300);
    lv_label_set_long_mode(s_ui.ai_label, LV_LABEL_LONG_WRAP);

    s_ui.event_label = __label(s_ui.report_area, &font_puhui_16_2, UI_MUTED_COLOR, "Event ID: -");
    lv_obj_set_pos(s_ui.event_label, 10, 96);
    lv_obj_set_width(s_ui.event_label, 300);
    lv_label_set_long_mode(s_ui.event_label, LV_LABEL_LONG_DOT);
}

/**
 * @brief Create emergency overlay.
 * @return none
 */
static void __create_alert_overlay(void)
{
    s_ui.alert_overlay = __rect(s_ui.page_monitor, 0, 0, UI_SCREEN_W, UI_SCREEN_H, UI_RED, 0);
    lv_obj_set_style_bg_opa(s_ui.alert_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.alert_overlay, LV_OBJ_FLAG_HIDDEN);

    s_ui.alert_title = __label(s_ui.alert_overlay, &font_puhui_20_2, UI_TEXT_COLOR, "紧急报警");
    lv_obj_align(s_ui.alert_title, LV_ALIGN_CENTER, 0, -30);

    s_ui.alert_msg = __label(s_ui.alert_overlay, &font_puhui_20_2, UI_TEXT_COLOR, "请立即处理");
    lv_obj_align(s_ui.alert_msg, LV_ALIGN_CENTER, 0, 18);
}

/**
 * @brief Create bottom page indicators.
 * @return none
 */
static void __create_page_dots(void)
{
    s_ui.page_dot_monitor = __rect(s_ui.page_monitor, 75, 454, 70, 20, UI_BLUE, 10);
    s_ui.page_dot_learning = __rect(s_ui.page_monitor, 175, 454, 70, 20, UI_MUTED_COLOR, 10);

    lv_obj_t *monitor_text = __label(s_ui.page_dot_monitor, &font_puhui_16_2, UI_TEXT_COLOR, "监护");
    lv_obj_t *learning_text = __label(s_ui.page_dot_learning, &font_puhui_16_2, UI_TEXT_COLOR, "AI学习");
    lv_obj_center(monitor_text);
    lv_obj_center(learning_text);
}

/**
 * @brief Create the independent learning page.
 * @return none
 */
static void __create_learning_page(void)
{
    lv_obj_t *title;
    lv_obj_t *weight_panel;
    lv_obj_t *stat_title;
    lv_obj_t *monitor_dot;
    lv_obj_t *learning_dot;
    lv_obj_t *monitor_text;
    lv_obj_t *learning_text;

    s_ui.page_learning = __rect(s_ui.page_host, UI_SCREEN_W, 0, UI_SCREEN_W, UI_SCREEN_H, UI_BG_COLOR, 0);

    title = __label(s_ui.page_learning, &font_puhui_20_2, UI_TEXT_COLOR, "闭环AI学习");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_ui.learn_status_label = __label(s_ui.page_learning, &font_puhui_16_2, UI_GREEN, "学习待机");
    lv_obj_align(s_ui.learn_status_label, LV_ALIGN_TOP_MID, 0, 42);

    s_ui.learn_notice_label = __label(s_ui.page_learning, &font_puhui_16_2, UI_SUB_TEXT_COLOR, "等待学习数据");
    lv_obj_align(s_ui.learn_notice_label, LV_ALIGN_TOP_MID, 0, 68);

    weight_panel = __rect(s_ui.page_learning, 10, 96, 300, 104, lv_color_hex(0x111827), 8);
    lv_obj_set_style_border_width(weight_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(weight_panel, lv_color_hex(0x374151), LV_PART_MAIN);
    __weight_row(weight_panel, 12, "姿态权重", &s_ui.posture_bar, &s_ui.posture_value_label);
    __weight_row(weight_panel, 42, "脑电权重", &s_ui.eeg_bar, &s_ui.eeg_value_label);
    __weight_row(weight_panel, 72, "冲击权重", &s_ui.burst_bar, &s_ui.burst_value_label);

    s_ui.warning_value_label = __value_card(s_ui.page_learning, 10, 210, 145, 58, "预警阈值");
    s_ui.danger_value_label = __value_card(s_ui.page_learning, 165, 210, 145, 58, "报警阈值");

    stat_title = __label(s_ui.page_learning, &font_puhui_16_2, UI_SUB_TEXT_COLOR, "学习统计");
    lv_obj_set_pos(stat_title, 10, 278);

    s_ui.false_alarm_value_label = __value_card(s_ui.page_learning, 10, 302, 145, 48, "误报修正");
    s_ui.confirmed_value_label = __value_card(s_ui.page_learning, 165, 302, 145, 48, "确认危险");
    s_ui.missed_value_label = __value_card(s_ui.page_learning, 10, 358, 145, 48, "漏报修正");
    s_ui.update_value_label = __value_card(s_ui.page_learning, 165, 358, 145, 48, "参数优化");

    monitor_dot = __rect(s_ui.page_learning, 75, 454, 70, 20, UI_MUTED_COLOR, 10);
    learning_dot = __rect(s_ui.page_learning, 175, 454, 70, 20, UI_BLUE, 10);
    monitor_text = __label(monitor_dot, &font_puhui_16_2, UI_TEXT_COLOR, "监护");
    learning_text = __label(learning_dot, &font_puhui_16_2, UI_TEXT_COLOR, "AI学习");
    lv_obj_center(monitor_text);
    lv_obj_center(learning_text);
}

/**
 * @brief Initialize LVGL UI.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET ui_init(void)
{
    if (s_ui.ready) {
        return OPRT_OK;
    }

    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();

    s_ui.root = lv_screen_active();
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_set_style_bg_color(s_ui.root, UI_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.page_host = __rect(s_ui.root, 0, 0, UI_SCREEN_W, UI_SCREEN_H, UI_BG_COLOR, 0);
    lv_obj_set_scroll_dir(s_ui.page_host, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_ui.page_host, LV_SCROLL_SNAP_CENTER);
    lv_obj_clear_flag(s_ui.page_host, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(s_ui.page_host, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    s_ui.page_monitor = __rect(s_ui.page_host, 0, 0, UI_SCREEN_W, UI_SCREEN_H, UI_BG_COLOR, 0);
    __create_top_bar();
    __create_core_area();
    __create_vital_area();
    __create_report_area();
    __create_alert_overlay();
    __create_page_dots();
    __create_learning_page();

    lv_vendor_disp_unlock();
    lv_vendor_start(T5_UI_TASK_PRIO, T5_UI_TASK_STACK_SIZE);

    s_ui.ready = true;
    T5_LOG_I("BodyGuard companion UI ready, %dx%d", UI_SCREEN_W, UI_SCREEN_H);
    return OPRT_OK;
}

/**
 * @brief Refresh connection indicators.
 * @param[in] state Shared app state.
 * @return none
 */
static void __refresh_connection(const t5_app_state_t *state)
{
    lv_obj_set_style_bg_color(s_ui.ble_dot, state->ble_connected ? UI_BLUE : UI_MUTED_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.cloud_dot, state->packet.cloud_conn_state ? UI_GREEN : UI_MUTED_COLOR, LV_PART_MAIN);
}

/**
 * @brief Refresh companion widgets.
 * @param[in] state Shared app state.
 * @return none
 */
static void __refresh_companion(const t5_app_state_t *state)
{
    lv_color_t main_color = __companion_color(state->packet.sys_state);
    lv_color_t dark_color = __companion_dark_color(state->packet.sys_state);

    lv_obj_set_style_bg_color(s_ui.companion_glow, dark_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_body, main_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_head, main_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_ear_l, dark_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_ear_r, dark_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_hand_l, dark_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.companion_hand_r, dark_color, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.bubble, main_color, LV_PART_MAIN);
    lv_label_set_text(s_ui.companion_face, __face_text(state->packet.sys_state));
    lv_label_set_text(s_ui.bubble_label, __bubble_text(state->packet.sys_state));
}

/**
 * @brief Convert a 0.0~1.0 weight to percent.
 * @param[in] weight Weight value.
 * @return Percent value.
 */
static int32_t __weight_percent(float weight)
{
    if (weight < 0.0f) {
        return 0;
    }
    if (weight > 1.0f) {
        return 100;
    }
    return (int32_t)(weight * 100.0f + 0.5f);
}

/**
 * @brief Refresh one learning progress row.
 * @param[in] bar Progress bar.
 * @param[in] value_label Value label.
 * @param[in] weight Weight value.
 * @return none
 */
static void __refresh_weight(lv_obj_t *bar, lv_obj_t *value_label, float weight)
{
    char text[16];
    int32_t percent = __weight_percent(weight);

    lv_bar_set_value(bar, percent, LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%ld%%", (long)percent);
    lv_label_set_text(value_label, text);
}

/**
 * @brief Track learning counter changes and choose a transient notice.
 * @param[in] state Shared app state.
 * @return none
 */
static void __update_learning_notice(const t5_app_state_t *state)
{
    uint32_t now_ms = tal_system_get_millisecond();

    if (!state->learning_valid || !state->ble_connected) {
        s_ui.learn_counter_ready = false;
        s_ui.notice_until_ms = 0;
        s_ui.notice_text[0] = '\0';
        return;
    }

    if (!s_ui.learn_counter_ready) {
        s_ui.last_false_alarm_count = state->packet.false_alarm_count;
        s_ui.last_missed_danger_count = state->packet.missed_danger_count;
        s_ui.last_learning_update_count = state->packet.learning_update_count;
        s_ui.learn_counter_ready = true;
        return;
    }

    if (state->packet.learning_update_count > s_ui.last_learning_update_count) {
        strncpy(s_ui.notice_text, "参数已优化", sizeof(s_ui.notice_text) - 1);
        s_ui.notice_until_ms = now_ms + 2500;
    } else if (state->packet.false_alarm_count > s_ui.last_false_alarm_count) {
        strncpy(s_ui.notice_text, "已降低误报敏感度", sizeof(s_ui.notice_text) - 1);
        s_ui.notice_until_ms = now_ms + 2500;
    } else if (state->packet.missed_danger_count > s_ui.last_missed_danger_count) {
        strncpy(s_ui.notice_text, "已提高危险识别敏感度", sizeof(s_ui.notice_text) - 1);
        s_ui.notice_until_ms = now_ms + 2500;
    }

    s_ui.notice_text[sizeof(s_ui.notice_text) - 1] = '\0';
    s_ui.last_false_alarm_count = state->packet.false_alarm_count;
    s_ui.last_missed_danger_count = state->packet.missed_danger_count;
    s_ui.last_learning_update_count = state->packet.learning_update_count;
}

/**
 * @brief Refresh learning page.
 * @param[in] state Shared app state.
 * @return none
 */
static void __refresh_learning(const t5_app_state_t *state)
{
    char text[32];
    uint32_t now_ms = tal_system_get_millisecond();

    __update_learning_notice(state);

    if (!state->ble_connected) {
        lv_label_set_text(s_ui.learn_status_label, "连接中断");
        lv_obj_set_style_text_color(s_ui.learn_status_label, UI_RED, LV_PART_MAIN);
        lv_label_set_text(s_ui.learn_notice_label, "连接中断");
        return;
    }

    if (!state->learning_valid) {
        lv_label_set_text(s_ui.learn_status_label, "学习待机");
        lv_obj_set_style_text_color(s_ui.learn_status_label, UI_SUB_TEXT_COLOR, LV_PART_MAIN);
        lv_label_set_text(s_ui.learn_notice_label, "等待学习数据");
        return;
    }

    if (state->packet.learning_enabled) {
        lv_label_set_text(s_ui.learn_status_label, "闭环学习中");
        lv_obj_set_style_text_color(s_ui.learn_status_label, UI_GREEN, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_ui.learn_status_label, "学习待机");
        lv_obj_set_style_text_color(s_ui.learn_status_label, UI_SUB_TEXT_COLOR, LV_PART_MAIN);
    }

    if (s_ui.notice_until_ms > now_ms && s_ui.notice_text[0] != '\0') {
        lv_label_set_text(s_ui.learn_notice_label, s_ui.notice_text);
        lv_obj_set_style_text_color(s_ui.learn_notice_label, UI_ORANGE, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_ui.learn_notice_label, state->packet.learning_enabled ? "闭环学习中" : "学习待机");
        lv_obj_set_style_text_color(s_ui.learn_notice_label, UI_SUB_TEXT_COLOR, LV_PART_MAIN);
    }

    __refresh_weight(s_ui.posture_bar, s_ui.posture_value_label, state->packet.posture_weight);
    __refresh_weight(s_ui.eeg_bar, s_ui.eeg_value_label, state->packet.eeg_weight);
    __refresh_weight(s_ui.burst_bar, s_ui.burst_value_label, state->packet.burst_weight);

    snprintf(text, sizeof(text), "%u", (unsigned)state->packet.warning_threshold);
    lv_label_set_text(s_ui.warning_value_label, text);
    snprintf(text, sizeof(text), "%u", (unsigned)state->packet.danger_threshold);
    lv_label_set_text(s_ui.danger_value_label, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)state->packet.false_alarm_count);
    lv_label_set_text(s_ui.false_alarm_value_label, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)state->packet.confirmed_danger_count);
    lv_label_set_text(s_ui.confirmed_value_label, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)state->packet.missed_danger_count);
    lv_label_set_text(s_ui.missed_value_label, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)state->packet.learning_update_count);
    lv_label_set_text(s_ui.update_value_label, text);
}

/**
 * @brief Refresh UI from shared state.
 * @return none
 */
void ui_refresh(void)
{
    t5_app_state_t state;
    char text[256];
    lv_color_t risk_color;
    lv_color_t mode_color;

    if (!s_ui.ready) {
        return;
    }

    if (app_state_get(&state) != OPRT_OK) {
        return;
    }

    lv_vendor_disp_lock();

    risk_color = __risk_color(state.packet.risk_score);
    mode_color = __companion_color(state.packet.sys_state);

    __refresh_connection(&state);
    __refresh_companion(&state);
    __refresh_learning(&state);

    lv_obj_set_style_bg_color(s_ui.mode_pill, mode_color, LV_PART_MAIN);
    lv_label_set_text(s_ui.mode_label, __mode_text(state.packet.sys_state));

    snprintf(text, sizeof(text), "%03u", (unsigned)state.packet.risk_score);
    lv_label_set_text(s_ui.risk_label, text);
    lv_obj_set_style_text_color(s_ui.risk_label, risk_color, LV_PART_MAIN);

    lv_label_set_text(s_ui.eeg_label, __eeg_text(state.packet.eeg_state));
    lv_obj_set_style_border_color(s_ui.eeg_chip, __state_color(state.packet.eeg_state), LV_PART_MAIN);

    lv_label_set_text(s_ui.pose_label, __pose_text(state.packet.pose_state));
    lv_obj_set_style_border_color(s_ui.pose_chip, __state_color(state.packet.pose_state), LV_PART_MAIN);

    snprintf(text, sizeof(text), "%s", state.packet.ai_report[0] ? state.packet.ai_report : "等待AI分析...");
    lv_label_set_text(s_ui.ai_label, text);

    snprintf(text, sizeof(text), "事件ID: %s", state.packet.event_id[0] ? state.packet.event_id : "-");
    lv_label_set_text(s_ui.event_label, text);

    if (state.packet.sys_state == T5_SYS_EMERGENCY) {
        lv_obj_clear_flag(s_ui.alert_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.alert_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    lv_vendor_disp_unlock();
}
