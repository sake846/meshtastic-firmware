Import("env")

from pathlib import Path

library = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "meshtastic-device-ui"
)
source = library / "source" / "graphics" / "TFT" / "TFTView_320x240.cpp"
if not source.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {source}")

data = source.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"

boot_anchor = b"    ui_init_boot();" + newline
boot_resize = (
    boot_anchor
    + b"    const uint32_t pagerMUIBootWidth = lv_display_get_horizontal_resolution(displaydriver->getDisplay());"
    + newline
    + b"    const uint32_t pagerMUIBootHeight = lv_display_get_vertical_resolution(displaydriver->getDisplay());"
    + newline
    + b"    lv_obj_set_size(objects.boot_screen, pagerMUIBootWidth, pagerMUIBootHeight);"
    + newline
)

screens_anchor = b"    ui_init();" + newline + b"    apply_hotfix();" + newline
screens_resize = (
    screens_anchor
    + b"    const uint32_t pagerMUIWidth = lv_display_get_horizontal_resolution(displaydriver->getDisplay());"
    + newline
    + b"    const uint32_t pagerMUIHeight = lv_display_get_vertical_resolution(displaydriver->getDisplay());"
    + newline
    + b"    lv_obj_t *pagerMUIScreens[] = {objects.main_screen, objects.boot_screen, objects.blank_screen,"
    + newline
    + b"                                       objects.lock_screen, objects.calibration_screen};"
    + newline
    + b"    for (lv_obj_t *screen : pagerMUIScreens) {"
    + newline
    + b"        if (screen)"
    + newline
    + b"            lv_obj_set_size(screen, pagerMUIWidth, pagerMUIHeight);"
    + newline
    + b"    }"
    + newline
)

if b"pagerMUIBootWidth" not in data:
    if data.count(boot_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui boot initialization")
    data = data.replace(boot_anchor, boot_resize, 1)

boot_version_anchor = (
    b"        lv_label_set_text(objects.firmware_label, firmware_version);" + newline
)
boot_version_patch = (
    b"        lv_label_set_text(objects.firmware_label, "
    b'"v2.8.2.d5f1619 +");' + newline
)
if b'"v2.8.2.d5f1619 +"' not in data:
    if data.count(boot_version_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui boot version label")
    data = data.replace(boot_version_anchor, boot_version_patch, 1)

if b"pagerMUIScreens" not in data:
    if data.count(screens_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui screen initialization")
    data = data.replace(screens_anchor, screens_resize, 1)

icon_anchor = (
    b"    if (h > 320 && v > 320) {"
    + newline
    + b"        lv_obj_t *button[] = {objects.home_button,     objects.nodes_button, objects.groups_button,"
    + newline
    + b"                              objects.messages_button, objects.map_button,   objects.settings_button};"
    + newline
    + b"        for (int i = 0; i < 6; i++) {"
    + newline
    + b"            lv_obj_set_size(button[i], 72, 72);"
    + newline
    + b"        }"
    + newline
    + b"    }"
)
icon_patch = (
    icon_anchor
    + b" else if (h == 480 && v == 222) {"
    + newline
    + b"        lv_obj_t *pagerMUICompactIcons[] = {objects.home_button, objects.nodes_button, objects.groups_button,"
    + newline
    + b"                                                objects.messages_button, objects.map_button, objects.settings_button};"
    + newline
    + b"        for (lv_obj_t *icon : pagerMUICompactIcons)"
    + newline
    + b"            lv_obj_set_size(icon, 32, 32);"
    + newline
    + b"    }"
)
if b"pagerMUICompactIcons" not in data:
    if data.count(icon_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui navigation icon sizing")
    data = data.replace(icon_anchor, icon_patch, 1)

legacy_scroll = b"void TFTView_320x240::ui_event_scroll_panel" not in data
focus_anchor = b"void TFTView_320x240::apply_hotfix(void)" + newline
focus_helpers = r"""static void pagerNodeFocusEvent(lv_event_t *e)
{
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target_obj(e));
    if (!row)
        return;
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED)
        lv_obj_add_state(row, LV_STATE_FOCUS_KEY);
    else
        lv_obj_remove_state(row, LV_STATE_FOCUS_KEY);
}

static void pagerApplyInsetFocus(lv_obj_t *obj)
{
    if (!obj)
        return;
    if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, focus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0xaafbff), focus);
    } else if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_parent(obj) &&
               lv_obj_get_parent(lv_obj_get_parent(obj)) == objects.nodes_panel) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        const lv_style_selector_t pressed = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_PRESSED;
        lv_obj_t *row = lv_obj_get_parent(obj);
        lv_obj_set_style_border_width(obj, 0, focus);
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(row, 2, focus);
        lv_obj_set_style_border_color(row, lv_color_hex(0xaafbff), focus);
        lv_obj_add_event_cb(obj, pagerNodeFocusEvent, LV_EVENT_FOCUSED, nullptr);
        lv_obj_add_event_cb(obj, pagerNodeFocusEvent, LV_EVENT_DEFOCUSED, nullptr);
        lv_obj_set_style_transform_width(obj, 0, pressed);
        lv_obj_set_style_transform_height(obj, 0, pressed);
    } else if (lv_obj_check_type(obj, &lv_button_class) || lv_obj_check_type(obj, &lv_imagebutton_class) ||
               lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_dropdown_class) ||
               lv_obj_check_type(obj, &lv_switch_class) || lv_obj_check_type(obj, &lv_checkbox_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, focus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0xaafbff), focus);
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        const lv_style_selector_t knobFocus = (lv_style_selector_t)LV_PART_KNOB | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, knobFocus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0xaafbff), knobFocus);
    }
}

static lv_obj_tree_walk_res_t pagerApplyInsetFocusWalk(lv_obj_t *obj, void *)
{
    pagerApplyInsetFocus(obj);
    return LV_OBJ_TREE_WALK_NEXT;
}

static void pagerApplyInsetFocusChildCreated(lv_event_t *e)
{
    pagerApplyInsetFocus(static_cast<lv_obj_t *>(lv_event_get_param(e)));
}

static lv_obj_tree_walk_res_t pagerApplyScrollOnFocusWalk(lv_obj_t *obj, void *data)
{
    lv_group_t *group = static_cast<lv_group_t *>(data);
    if (obj && group && lv_obj_get_group(obj) == group)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return LV_OBJ_TREE_WALK_NEXT;
}

static void pagerApplyScrollOnFocus(lv_obj_t *panel)
{
    lv_group_t *group = lv_group_get_default();
    if (panel && group)
        lv_obj_tree_walk(panel, pagerApplyScrollOnFocusWalk, group);
}

""".replace("\n", newline.decode()).encode()
if legacy_scroll:
    pager_scroll_helper = r"""static void pagerScrollPanelKey(lv_event_t *e)
{
    const void *param = lv_event_get_param(e);
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (!param || !target || lv_obj_check_type(target, &lv_textarea_class))
        return;

    const uint32_t key = *static_cast<const uint32_t *>(param);
    lv_obj_t *panel = target;
    while (panel && panel != objects.settings_about_panel && panel != objects.tools_statistics_panel &&
           panel != objects.tools_packet_log_panel)
        panel = lv_obj_get_parent(panel);
    if (!panel)
        return;

    if (key == LV_KEY_DOWN)
        lv_obj_scroll_by_bounded(panel, 0, -80, LV_ANIM_ON);
    else if (key == LV_KEY_UP)
        lv_obj_scroll_by_bounded(panel, 0, 80, LV_ANIM_ON);
    else if (key == LV_KEY_ESC || key == LV_KEY_LEFT)
        lv_group_focus_obj(objects.settings_button);
    else
        return;
    lv_event_stop_processing(e);
}

""".replace("\n", newline.decode()).encode()
    focus_helpers = focus_helpers.replace(
        b"static lv_obj_tree_walk_res_t pagerApplyInsetFocusWalk",
        pager_scroll_helper + b"static lv_obj_tree_walk_res_t pagerApplyInsetFocusWalk",
    )
    focus_helpers = focus_helpers.replace(
        b"    pagerApplyInsetFocus(obj);",
        b"    pagerApplyInsetFocus(obj);"
        + newline
        + b"    if (obj) lv_obj_add_event_cb(obj, pagerScrollPanelKey, LV_EVENT_KEY, nullptr);",
    )
    focus_helpers = focus_helpers.replace(
        b"    pagerApplyInsetFocus(static_cast<lv_obj_t *>(lv_event_get_param(e)));",
        b"    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_param(e));"
        + newline
        + b"    pagerApplyInsetFocus(obj);"
        + newline
        + b"    if (obj) lv_obj_add_event_cb(obj, pagerScrollPanelKey, LV_EVENT_KEY, nullptr);",
    )
focus_helper_marker = b"static void pagerApplyInsetFocus(lv_obj_t *obj)"
if focus_helper_marker in data:
    helper_start = data.find(b"static void pagerNodeFocusEvent(lv_event_t *e)")
    if helper_start < 0:
        helper_start = data.index(focus_helper_marker)
    helper_end = data.index(focus_anchor, helper_start)
    data = data[:helper_start] + focus_helpers + data[helper_end:]
elif data.count(focus_anchor) == 1:
    data = data.replace(focus_anchor, focus_helpers + focus_anchor, 1)
else:
    raise RuntimeError("unexpected meshtastic-device-ui hotfix function")

focus_patch = (
    b"void TFTView_320x240::apply_hotfix(void)"
    + newline
    + b"{"
    + newline
    + b"    lv_obj_set_style_flex_flow(objects.home_container, LV_FLEX_FLOW_ROW_WRAP, LV_PART_MAIN | LV_STATE_FOCUSED);"
    + newline
    + b"    lv_obj_set_style_flex_cross_place(objects.home_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_FOCUSED);"
    + newline
    + b"    lv_obj_t *pagerFocusScreens[] = {objects.main_screen, objects.boot_screen, objects.blank_screen,"
    + newline
    + b"                                       objects.lock_screen, objects.calibration_screen};"
    + newline
    + b"    for (lv_obj_t *screen : pagerFocusScreens) {"
    + newline
    + b"        if (screen) {"
    + newline
    + b"            lv_obj_tree_walk(screen, pagerApplyInsetFocusWalk, nullptr);"
    + newline
    + b"            lv_obj_add_event_cb(screen, pagerApplyInsetFocusChildCreated, LV_EVENT_CHILD_CREATED, nullptr);"
    + newline
    + b"        }"
    + newline
    + b"    }"
    + newline
    + b"    pagerApplyScrollOnFocus(objects.home_container);"
    + newline
    + b"    pagerApplyScrollOnFocus(objects.groups_panel);"
    + newline
    + b"    pagerApplyScrollOnFocus(objects.settings_channel_panel);"
    + newline
    + b"    pagerApplyScrollOnFocus(objects.tab_page_basic_settings);"
    + newline
    + b"    pagerApplyScrollOnFocus(objects.tab_page_tools);"
    + newline
    + b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"    if (lv_group_t *group = lv_group_get_default()) {"
    + newline
    + b"        lv_obj_t *mapButtons[] = {objects.gps_lock_button, objects.zoom_in_button, objects.zoom_out_button,"
    + newline
    + b"                                  objects.arrow_up_button, objects.arrow_left_button, objects.nav_button,"
    + newline
    + b"                                  objects.arrow_right_button, objects.arrow_down_button};"
    + newline
    + b"        for (lv_obj_t *button : mapButtons) {"
    + newline
    + b"            lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);"
    + newline
    + b"            lv_group_remove_obj(button);"
    + newline
    + b"        }"
    + newline
    + b"        for (lv_obj_t *button : mapButtons) {"
    + newline
    + b"            lv_group_add_obj(group, button);"
    + newline
    + b"        }"
    + newline
    + b"    }"
    + newline
    + b"#endif"
    + newline
    + b"    lv_obj_set_width(objects.settings_user_long_textarea, LV_PCT(80));"
    + newline
    + b"    lv_obj_set_width(objects.settings_wifi_ssid_textarea, LV_PCT(80));"
    + newline
    + b"    lv_obj_set_width(objects.settings_wifi_password_textarea, LV_PCT(80));"
    + newline
    + b"    lv_obj_set_width(objects.settings_modify_channel_name_textarea, LV_PCT(82));"
    + newline
    + b"    lv_obj_set_width(objects.settings_modify_channel_psk_textarea, LV_PCT(82));"
    + newline
    + b"    lv_obj_set_width(objects.setup_user_long_textarea, LV_PCT(80));"
    + newline
)

node_order_patch = r"""    // Keep node rows in last-heard order.
    lv_obj_t **children = objects.nodes_panel->spec_attr->children;
    int last = objects.nodes_panel->spec_attr->child_cnt - 1;
    int i = last;
    if (lastHeard) {
        while (i > 1) {
            if (lastHeard <= (time_t)(children[i - 1]->LV_OBJ_IDX(node_lh_idx)->user_data))
                break;
            i--;
        }
        if (i >= 1 && i < last)
            lv_obj_move_to_index(p, i);
    }

""".replace("\n", newline.decode()).encode()
if b"    // Keep node rows in last-heard order." not in data:
    previous_marker = b"    // Keep node focus order aligned with the visible list"
    if previous_marker in data:
        node_order_start = data.index(previous_marker)
        node_order_end = data.index(b"    if (!nodesChanged) {", node_order_start)
        data = data[:node_order_start] + node_order_patch + data[node_order_end:]
    else:
        node_order_start = data.index(
            b"    // move node into new position within nodePanel"
        )
        node_order_end = data.index(b"    if (!nodesChanged) {", node_order_start)
        data = data[:node_order_start] + node_order_patch + data[node_order_end:]

focus_block = focus_patch[
    len(b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline) :
]
if b"pagerFocusScreens" in data:
    block_start = data.index(b"    lv_obj_t *pagerFocusScreens[] = {")
    block_end_marker = (
        b"    lv_obj_set_width(objects.setup_user_long_textarea, LV_PCT(80));" + newline
    )
    block_end = data.index(block_end_marker, block_start) + len(block_end_marker)
    data = data[:block_start] + focus_block + data[block_end:]
elif (
    data.count(b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline)
    == 1
):
    focus_function = (
        b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline
    )
    data = data.replace(focus_function, focus_function + focus_block, 1)
else:
    raise RuntimeError("unexpected meshtastic-device-ui hotfix function")

scroll_anchor = (
    b"    // Define how many pixels to scroll per keypress"
    + newline
    + b"    const lv_coord_t scroll_step = 80;"
    + newline
    + newline
    + b"    if (key == LV_KEY_DOWN) {"
    + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, -scroll_step, LV_ANIM_ON);"
    + newline
    + b"    } else if (key == LV_KEY_UP) {"
    + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, scroll_step, LV_ANIM_ON);"
    + newline
    + b"    }"
    + newline
    + b"    if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {"
)
scroll_patch = (
    b"    lv_indev_t *indev = lv_indev_active();"
    + newline
    + b"    const bool encoder = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;"
    + newline
    + b"    const lv_coord_t scroll_step = 80;"
    + newline
    + b"    if (encoder && key == LV_KEY_ENTER) {"
    + newline
    + b"        if (lv_group_t *group = lv_indev_get_group(indev))"
    + newline
    + b"            lv_group_set_editing(group, false);"
    + newline
    + b"        lv_event_stop_processing(e);"
    + newline
    + b"        return;"
    + newline
    + b"    }"
    + newline
    + b"    if (encoder && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {"
    + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, key == LV_KEY_RIGHT ? -scroll_step : scroll_step, LV_ANIM_ON);"
    + newline
    + b"        lv_event_stop_processing(e);"
    + newline
    + b"        return;"
    + newline
    + b"    }"
    + newline
    + b"    if (key == LV_KEY_DOWN) {"
    + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, -scroll_step, LV_ANIM_ON);"
    + newline
    + b"    } else if (key == LV_KEY_UP) {"
    + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, scroll_step, LV_ANIM_ON);"
    + newline
    + b"    }"
    + newline
    + b"    if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {"
)
if (
    b"const bool encoder = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;"
    not in data
):
    if data.count(scroll_anchor) == 1:
        data = data.replace(scroll_anchor, scroll_patch, 1)
    elif not legacy_scroll:
        raise RuntimeError("unexpected meshtastic-device-ui scroll panel key handling")

menu_focus_decl = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"extern void pagerSetActiveMenuButton(lv_obj_t *button);"
    + newline
    + b"#endif"
    + newline
)
if b"extern void pagerSetActiveMenuButton" not in data:
    include_anchor = b'#include "ui.h"' + newline
    if data.count(include_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui TFT view includes")
    data = data.replace(include_anchor, include_anchor + menu_focus_decl, 1)

active_button_anchor = b"    activeButton = b;" + newline
active_button_patch = (
    active_button_anchor
    + b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"    pagerSetActiveMenuButton(b);"
    + newline
    + b"#endif"
    + newline
)
if b"pagerSetActiveMenuButton(b);" not in data:
    if data.count(active_button_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui active menu button")
    data = data.replace(active_button_anchor, active_button_patch, 1)

map_anchor = b"        if (THIS->activePanel == objects.map_panel) {" + newline
map_patch = r"""#if defined(INPUTDRIVER_ROTARY_TYPE)
        lv_indev_t *indev = lv_indev_active();
        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            if (THIS->activePanel != objects.map_panel) {
                THIS->ui_set_active(objects.map_button, objects.map_panel, objects.top_map_panel);
                THIS->loadMap();
            }
            lv_obj_clear_flag(objects.navigation_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(objects.gps_lock_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(objects.zoom_in_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(objects.zoom_out_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.map_osd_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.gps_lock_button);
            return;
        }
#endif
""".replace("\n", newline.decode()).encode()
if b"lv_obj_t *mapButtons[]" not in data:
    raise RuntimeError("map navigation buttons were not registered")
if (
    b"lv_indev_t *indev = lv_indev_active();"
    + newline
    + b"        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {"
    not in data
):
    if data.count(map_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui map button handling")
    data = data.replace(map_anchor, map_patch + map_anchor, 1)

nav_focus = b"lv_group_focus_obj(objects.nav_button);"
map_start_focus = b"lv_group_focus_obj(objects.gps_lock_button);"
if nav_focus in data:
    if data.count(nav_focus) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui map focus entry")
    data = data.replace(nav_focus, map_start_focus, 1)

last_heard_anchor = (
    b"            if (lv_group_ll && act)"
    + newline
    + b"                _lv_ll_move_before(lv_group_ll, act, _lv_ll_get_next(lv_group_ll, topNodeLL));"
)
last_heard_patch = (
    last_heard_anchor
    + newline
    + b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"            if (THIS->activePanel == objects.nodes_panel) {"
    + newline
    + b"                lv_group_t *group = lv_group_get_default();"
    + newline
    + b"                lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;"
    + newline
    + b"                if (focused && lv_obj_get_parent(focused) == it->second)"
    + newline
    + b"                    lv_obj_scroll_to_view(focused, LV_ANIM_ON);"
    + newline
    + b"            }"
    + newline
    + b"#endif"
)
if b"lv_obj_scroll_to_view(focused, LV_ANIM_ON);" not in data:
    if data.count(last_heard_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui last-heard ordering")
    data = data.replace(last_heard_anchor, last_heard_patch, 1)

source.write_bytes(data)

device_gui = library / "source" / "graphics" / "DeviceGUI.cpp"
if not device_gui.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {device_gui}")

data = device_gui.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
rotary_marker = b"class PagerRotaryInputDriver final"
rotary_driver = """#if defined(INPUTDRIVER_ROTARY_TYPE)
#include <Arduino.h>
#include "RotaryEncoder.h"
#include "ui.h"

extern bool pagerFunctionModifierActive;
extern void pagerClearFunctionModifier();
static lv_obj_t *pagerActiveMenuButton = nullptr;

void pagerSetActiveMenuButton(lv_obj_t *button)
{
    pagerActiveMenuButton = button;
}

static int pagerMenuButtonIndex(lv_obj_t *obj)
{
    if (!obj)
        return -1;
    if (obj == objects.home_button) return 0;
    if (obj == objects.nodes_button) return 1;
    if (obj == objects.groups_button) return 2;
    if (obj == objects.messages_button) return 3;
    if (obj == objects.map_button) return 4;
    if (obj == objects.settings_button) return 5;
    return -1;
}

static lv_obj_t *pagerNodeButtonInRow(lv_group_t *group, lv_obj_t *row)
{
    if (!row || lv_obj_get_parent(row) != objects.nodes_panel)
        return nullptr;
    for (lv_obj_t *parent = row; parent; parent = lv_obj_get_parent(parent)) {
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
            return nullptr;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t *child = lv_obj_get_child(row, i);
        if (lv_obj_check_type(child, &lv_button_class) && lv_obj_get_group(child) == group &&
            !(lv_obj_get_state(child) & LV_STATE_DISABLED))
            return child;
    }
    return nullptr;
}

static bool pagerMoveNodeFocus(lv_group_t *group, int16_t diff)
{
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    lv_obj_t *focusedRow = focused ? lv_obj_get_parent(focused) : nullptr;
    if (pagerNodeButtonInRow(group, focusedRow) != focused)
        return false;

    int count = 0;
    int current = -1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(objects.nodes_panel); ++i) {
        lv_obj_t *button = pagerNodeButtonInRow(group, lv_obj_get_child(objects.nodes_panel, i));
        if (!button)
            continue;
        if (button == focused)
            current = count;
        ++count;
    }
    if (current < 0 || count == 0)
        return false;

    int targetIndex = (current + diff % count + count) % count;
    for (uint32_t i = 0; i < lv_obj_get_child_count(objects.nodes_panel); ++i) {
        lv_obj_t *button = pagerNodeButtonInRow(group, lv_obj_get_child(objects.nodes_panel, i));
        if (!button)
            continue;
        if (targetIndex-- == 0) {
            lv_group_focus_obj(button);
            break;
        }
    }
    return true;
}

static lv_obj_t *pagerMapFocusButtonAt(int index)
{
    switch (index) {
    case 0: return objects.gps_lock_button;
    case 1: return objects.zoom_in_button;
    case 2: return objects.zoom_out_button;
    case 3: return objects.arrow_up_button;
    case 4: return objects.arrow_left_button;
    case 5: return objects.nav_button;
    case 6: return objects.arrow_right_button;
    case 7: return objects.arrow_down_button;
    default: return nullptr;
    }
}

static int pagerMapFocusButtonIndex(lv_obj_t *obj)
{
    for (int i = 0; i < 8; ++i) {
        if (pagerMapFocusButtonAt(i) == obj)
            return i;
    }
    return -1;
}

static bool pagerMapFocusButtonAvailable(lv_group_t *group, lv_obj_t *button)
{
    if (!button || lv_obj_get_group(button) != group || (lv_obj_get_state(button) & LV_STATE_DISABLED))
        return false;
    for (lv_obj_t *parent = button; parent; parent = lv_obj_get_parent(parent)) {
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
            return false;
    }
    return true;
}

static bool pagerMoveMapFocus(lv_group_t *group, int16_t diff)
{
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (pagerMapFocusButtonIndex(focused) < 0)
        return false;

    int count = 0;
    int current = -1;
    for (int i = 0; i < 8; ++i) {
        lv_obj_t *button = pagerMapFocusButtonAt(i);
        if (!pagerMapFocusButtonAvailable(group, button))
            continue;
        if (button == focused)
            current = count;
        ++count;
    }
    if (current < 0 || count == 0)
        return false;

    int targetIndex = (current + diff % count + count) % count;
    for (int i = 0; i < 8; ++i) {
        lv_obj_t *button = pagerMapFocusButtonAt(i);
        if (!pagerMapFocusButtonAvailable(group, button))
            continue;
        if (targetIndex-- == 0) {
            lv_group_focus_obj(button);
            break;
        }
    }
    return true;
}

static lv_obj_t *pagerMenuButtonAt(int index)
{
    switch (index) {
    case 0: return objects.home_button;
    case 1: return objects.nodes_button;
    case 2: return objects.groups_button;
    case 3: return objects.messages_button;
    case 4: return objects.map_button;
    case 5: return objects.settings_button;
    default: return nullptr;
    }
}

static bool pagerMenuButtonAvailable(lv_obj_t *button)
{
    return button && !lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(button, LV_STATE_DISABLED);
}

static bool pagerFocusTargetAvailable(lv_obj_t *obj)
{
    if (!obj || obj == objects.home_container)
        return false;

    lv_obj_t *inactiveTab = lv_tabview_get_tab_active(objects.controller_tab_view) == 0
                                ? objects.tab_page_tools
                                : objects.tab_page_basic_settings;
    bool inScrollablePanel = false;
    for (lv_obj_t *parent = obj; parent; parent = lv_obj_get_parent(parent)) {
        if (parent == inactiveTab || lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
            return false;
        if (parent != obj && lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE))
            inScrollablePanel = true;
    }
    return lv_obj_is_visible(obj) ||
           (lv_obj_get_screen(obj) == lv_screen_active() && inScrollablePanel);
}

static void pagerFocusContentStep(lv_group_t *group, int direction)
{
    const uint32_t count = lv_group_get_obj_count(group);
    if (!count)
        return;

    int current = -1;
    lv_obj_t *focused = lv_group_get_focused(group);
    for (uint32_t i = 0; i < count; ++i) {
        if (lv_group_get_obj_by_index(group, i) == focused) {
            current = static_cast<int>(i);
            break;
        }
    }
    if (current < 0)
        current = direction > 0 ? -1 : 0;

    for (uint32_t step = 1; step <= count; ++step) {
        int index = (current + direction * static_cast<int>(step)) % static_cast<int>(count);
        if (index < 0)
            index += static_cast<int>(count);
        lv_obj_t *candidate = lv_group_get_obj_by_index(group, static_cast<uint32_t>(index));
        if (!candidate || (lv_obj_get_state(candidate) & LV_STATE_DISABLED) ||
            pagerMenuButtonIndex(candidate) >= 0 || !pagerFocusTargetAvailable(candidate))
            continue;
        lv_group_focus_obj(candidate);
        return;
    }
}

static void pagerFocusMenuStep(lv_group_t *group, int direction)
{
    int index = pagerMenuButtonIndex(lv_group_get_focused(group));
    if (index < 0)
        index = pagerMenuButtonIndex(pagerActiveMenuButton);
    if (index < 0)
        index = 0;

    for (int i = 0; i < 6; ++i) {
        index = (index + direction + 6) % 6;
        lv_obj_t *button = pagerMenuButtonAt(index);
        if (pagerMenuButtonAvailable(button)) {
            lv_group_focus_obj(button);
            return;
        }
    }
}

bool pagerHandleBack(lv_indev_t *indev)
{
    lv_group_t *group = indev ? lv_indev_get_group(indev) : nullptr;
    if (!group)
        return false;

    lv_obj_t *focused = lv_group_get_focused(group);
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *cancelButtons[] = {
        objects.obj2__cancel_button_w, objects.obj3__cancel_button_w, objects.obj4__cancel_button_w,
        objects.obj5__cancel_button_w, objects.obj6__cancel_button_w, objects.obj7__cancel_button_w,
        objects.obj8__cancel_button_w, objects.obj9__cancel_button_w, objects.obj10__cancel_button_w,
        objects.obj11__cancel_button_w, objects.obj12__cancel_button_w, objects.obj13__cancel_button_w,
        objects.obj14__cancel_button_w, objects.obj15__cancel_button_w, objects.obj16__cancel_button_w,
        objects.obj17__cancel_button_w, objects.obj18__cancel_button_w, objects.obj21__cancel_button_w,
        objects.obj27__cancel_button_w,
    };
    for (lv_obj_t *parent = focused; parent && parent != screen; parent = lv_obj_get_parent(parent)) {
        for (lv_obj_t *button : cancelButtons) {
            for (lv_obj_t *ancestor = button; ancestor && ancestor != screen; ancestor = lv_obj_get_parent(ancestor)) {
                if (lv_obj_has_flag(ancestor, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_state(ancestor, LV_STATE_DISABLED))
                    break;
                if (ancestor == parent) {
                    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
                    return true;
                }
            }
        }
    }

    if (screen != objects.main_screen || !pagerMenuButtonAvailable(pagerActiveMenuButton) ||
        pagerMenuButtonIndex(focused) >= 0)
        return false;
    lv_group_set_editing(group, false);
    lv_group_focus_obj(pagerActiveMenuButton);
    return true;
}

static bool pagerMoveFocus(lv_group_t *group, int16_t diff)
{
    if (!group || !diff)
        return false;

    if (lv_screen_active() == objects.blank_screen)
        return true;

    lv_obj_t *scrollPanel = nullptr;
    if (!lv_obj_has_flag(objects.settings_about_panel, LV_OBJ_FLAG_HIDDEN))
        scrollPanel = objects.settings_about_panel;
    else if (!lv_obj_has_flag(objects.tools_statistics_panel, LV_OBJ_FLAG_HIDDEN))
        scrollPanel = objects.tools_statistics_panel;
    else if (!lv_obj_has_flag(objects.tools_packet_log_panel, LV_OBJ_FLAG_HIDDEN))
        scrollPanel = objects.tools_packet_log_panel;
    if (scrollPanel) {
        for (int16_t step = 0; step < abs(diff); ++step)
            lv_obj_scroll_by_bounded(scrollPanel, 0, diff > 0 ? -80 : 80, LV_ANIM_ON);
        return true;
    }

    if (pagerMoveMapFocus(group, diff))
        return true;

    if (pagerMoveNodeFocus(group, diff))
        return true;

    if (pagerMenuButtonIndex(lv_group_get_focused(group)) >= 0) {
        lv_group_set_editing(group, false);
        while (diff > 0) {
            pagerFocusMenuStep(group, 1);
            --diff;
        }
        while (diff < 0) {
            pagerFocusMenuStep(group, -1);
            ++diff;
        }
        return true;
    }

    if (lv_group_get_editing(group))
        return false;

    while (diff > 0) {
        pagerFocusContentStep(group, 1);
        --diff;
    }
    while (diff < 0) {
        pagerFocusContentStep(group, -1);
        ++diff;
    }
    return true;
}

class PagerRotaryInputDriver final : public InputDriver
{
  public:
    PagerRotaryInputDriver() : rotary(INPUTDRIVER_ROTARY_UP, INPUTDRIVER_ROTARY_DOWN, INPUTDRIVER_ROTARY_BTN)
    {
        rotaryInstance = &rotary;
    }

    void init() override
    {
        encoder = lv_indev_create();
        lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
        lv_indev_set_read_cb(encoder, encoder_read);
        if (!inputGroup) {
            inputGroup = lv_group_create();
            lv_group_set_default(inputGroup);
        }
        lv_indev_set_group(encoder, inputGroup);
        attachInterrupt(INPUTDRIVER_ROTARY_UP, rotary_interrupt, CHANGE);
        attachInterrupt(INPUTDRIVER_ROTARY_DOWN, rotary_interrupt, CHANGE);
    }

  private:
    static void rotary_interrupt()
    {
        portENTER_CRITICAL_ISR(&rotaryLock);
        switch (rotaryInstance->process()) {
        case RotaryEncoder::DIRECTION_CW:
            if (encoderDiff < INT16_MAX)
                encoderDiff++;
            break;
        case RotaryEncoder::DIRECTION_CCW:
            if (encoderDiff > INT16_MIN)
                encoderDiff--;
            break;
        default:
            break;
        }
        portEXIT_CRITICAL_ISR(&rotaryLock);
    }

    static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
    {
        const bool buttonPressed = rotaryInstance && rotaryInstance->readButton() == RotaryEncoder::BUTTON_PRESSED;
        lv_group_t *group = indev ? lv_indev_get_group(indev) : nullptr;
        if (buttonPressed && !suppressEncoderClick && pagerFunctionModifierActive) {
            pagerClearFunctionModifier();
            if (!pagerHandleBack(indev) && group) {
                lv_group_send_data(group, LV_KEY_ESC);
                lv_obj_t *target = lv_group_get_focused(group);
                if (target)
                    lv_obj_send_event(target, LV_EVENT_CANCEL, indev);
            }
            suppressEncoderClick = true;
        }
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        if (buttonPressed && !suppressEncoderClick && group && focused && lv_obj_check_type(focused, &lv_textarea_class)) {
            lv_group_set_editing(group, false);
            lv_group_focus_next(group);
            suppressEncoderClick = true;
        }

        portENTER_CRITICAL(&rotaryLock);
        const int16_t diff = encoderDiff;
        encoderDiff = 0;
        portEXIT_CRITICAL(&rotaryLock);
        if (diff || buttonPressed)
            lv_display_trigger_activity(lv_indev_get_display(indev));
        data->key = suppressEncoderClick ? 0 : LV_KEY_ENTER;
        data->enc_diff = pagerMoveFocus(group, diff) ? 0 : diff;
        data->state = buttonPressed && !suppressEncoderClick ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (!buttonPressed)
            suppressEncoderClick = false;
    }

    RotaryEncoder rotary;
    inline static RotaryEncoder *rotaryInstance = nullptr;
    inline static int16_t encoderDiff = 0;
    inline static portMUX_TYPE rotaryLock = portMUX_INITIALIZER_UNLOCKED;
    inline static bool suppressEncoderClick = false;
};
#endif
""".replace("\n", newline.decode()).encode()

if rotary_marker in data:
    rotary_start = data.rfind(
        b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline, 0, data.index(rotary_marker)
    )
    rotary_end = data.index(b"#endif", data.index(rotary_marker)) + len(b"#endif")
    if data[rotary_end : rotary_end + len(newline)] == newline:
        rotary_end += len(newline)
    if rotary_start < 0:
        raise RuntimeError("unexpected meshtastic-device-ui rotary driver block")
    data = data[:rotary_start] + rotary_driver + data[rotary_end:]
else:
    include_anchor = b"#include <thread>" + newline
    if data.count(include_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI includes")
    data = data.replace(include_anchor, include_anchor + rotary_driver, 1)

constructor_anchor = b"    if (!inputdriver)" + newline
constructor_patch = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"    pagerRotaryDriver = new PagerRotaryInputDriver;"
    + newline
    + b"#endif"
    + newline
    + constructor_anchor
)
if b"pagerRotaryDriver = new PagerRotaryInputDriver;" not in data:
    if data.count(constructor_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI constructor")
    data = data.replace(constructor_anchor, constructor_patch, 1)

driver_pointer_anchor = (
    b"static I2CKeyboardInputDriver *keyboardDriver = nullptr;" + newline
)
driver_pointer = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"static PagerRotaryInputDriver *pagerRotaryDriver = nullptr;"
    + newline
    + b"#endif"
    + newline
)
if b"static PagerRotaryInputDriver *pagerRotaryDriver" not in data:
    if data.count(driver_pointer_anchor) != 1:
        raise RuntimeError(
            "unexpected meshtastic-device-ui DeviceGUI driver declarations"
        )
    data = data.replace(
        driver_pointer_anchor, driver_pointer_anchor + driver_pointer, 1
    )

init_anchor = b"    displaydriver->printConfig();" + newline
init_patch = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"    if (pagerRotaryDriver)"
    + newline
    + b"        pagerRotaryDriver->init();"
    + newline
    + b"#endif"
    + newline
    + init_anchor
)
if b"pagerRotaryDriver->init();" not in data:
    if data.count(init_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI init")
    data = data.replace(init_anchor, init_patch, 1)

task_anchor = (
    b"#else"
    + newline
    + b"    displaydriver->task_handler();"
    + newline
    + b"#endif"
    + newline
    + b"};"
)
task_patch = (
    b"#else"
    + newline
    + b"    displaydriver->task_handler();"
    + newline
    + b"#if defined(INPUTDRIVER_ROTARY_TYPE)"
    + newline
    + b"    if (pagerRotaryDriver)"
    + newline
    + b"        pagerRotaryDriver->task_handler();"
    + newline
    + b"#endif"
    + newline
    + b"#endif"
    + newline
    + b"};"
)
if task_patch in data:
    data = data.replace(task_patch, task_anchor, 1)
elif data.count(task_anchor) != 1:
    raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI task handler")

device_gui.write_bytes(data)

button_source = library / "source" / "input" / "ButtonInputDriver.cpp"
if not button_source.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {button_source}")

data = button_source.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
button_anchor = (
    b"    static const lv_point_t btn_points[1] = {"
    + newline
    + b"        {10, 235} // button0: (cog symbol) and button on blank screen"
    + newline
    + b"    };"
)
button_patch = (
    b"    static const lv_point_t btn_points[1] = {"
    + newline
    + b"        {10, static_cast<lv_coord_t>(lv_display_get_vertical_resolution(lv_display_get_default()) - 5)}"
    + newline
    + b"    };"
)
if b"lv_display_get_vertical_resolution(lv_display_get_default()) - 5" not in data:
    if data.count(button_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui button points")
    data = data.replace(button_anchor, button_patch, 1)

button_source.write_bytes(data)

keyboard_source = library / "source" / "input" / "I2CKeyboardInputDriver.cpp"
if not keyboard_source.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {keyboard_source}")

data = keyboard_source.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
keyboard_anchor = (
    b"// ---------- TLoraPagerKeyboardInputDriver Implementation ----------" + newline
)
keyboard_helpers = r"""static uint8_t pagerKeyboardReadRegister(uint8_t address, uint8_t reg)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    const uint8_t status = Wire.endTransmission(false);
    const size_t count = Wire.requestFrom(address, static_cast<uint8_t>(1));
    if (status != 0 || count == 0)
        return 0;
    return Wire.read();
}

static void pagerKeyboardWriteRegister(uint8_t address, uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    // Release the Wire lock if its buffers were removed during bus recovery.
    if (Wire.endTransmission() == 4)
        Wire.requestFrom(address, static_cast<uint8_t>(1));
}

static uint8_t pagerKeyboardModifiers = 0;
bool pagerFunctionModifierActive = false;

void pagerClearFunctionModifier()
{
    pagerKeyboardModifiers &= ~0x02;
    pagerFunctionModifierActive = false;
}

static const char pagerKeyboardMap[31][3] = {
    {'q', 'Q', '1'}, {'w', 'W', '2'}, {'e', 'E', '3'}, {'r', 'R', '4'}, {'t', 'T', '5'},
    {'y', 'Y', '6'}, {'u', 'U', '7'}, {'i', 'I', '8'}, {'o', 'O', '9'}, {'p', 'P', '0'},
    {'a', 'A', '*'}, {'s', 'S', '/'}, {'d', 'D', '+'}, {'f', 'F', '-'}, {'g', 'G', '='},
    {'h', 'H', ':'}, {'j', 'J', 0x27}, {'k', 'K', 0x22}, {'l', 'L', '@'},
    {0, 0, 0}, {0, 0, 0}, {'z', 'Z', '_'}, {'x', 'X', '$'}, {'c', 'C', ';'},
    {'v', 'V', '?'}, {'b', 'B', '!'}, {'n', 'N', ','}, {'m', 'M', '.'},
    {0, 0, 0}, {0, 0, 0}, {' ', ' ', ' '}
};

""".replace(
    "\n", newline.decode()
).encode()
if b"pagerKeyboardReadRegister" not in data:
    if data.count(keyboard_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard section")
    data = data.replace(keyboard_anchor, keyboard_helpers + keyboard_anchor, 1)
else:
    keyboard_anchor_pos = data.index(keyboard_anchor)
    keyboard_helpers_pos = data.rfind(
        b"static uint8_t pagerKeyboardReadRegister", 0, keyboard_anchor_pos
    )
    if keyboard_helpers_pos < 0:
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard helpers")
    data = data[:keyboard_helpers_pos] + keyboard_helpers + data[keyboard_anchor_pos:]

pager_constructor_old = r"""TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
    registerI2CKeyboard(this, "TLora Pager Keyboard", address);
}""".replace(
    "\n", newline.decode()
).encode()
pager_constructor_new = r"""TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
}""".replace(
    "\n", newline.decode()
).encode()
if pager_constructor_old in data:
    data = data.replace(pager_constructor_old, pager_constructor_new, 1)
elif pager_constructor_new not in data:
    raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard constructor")

pager_init_old = r"""void TLoraPagerKeyboardInputDriver::init(void)
{
    // Additional initialization for TLora-Pager if needed
    TCA8418KeyboardInputDriver::init();
}""".replace("\n", newline.decode()).encode()
pager_init_new = r"""void TLoraPagerKeyboardInputDriver::init(void)
{
    I2CKeyboardInputDriver::init();
    pagerKeyboardWriteRegister(0x34, 0x23, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x24, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x25, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x20, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x21, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x22, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x26, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x27, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x28, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x1A, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x1B, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x1C, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x1D, 0x0F);
    pagerKeyboardWriteRegister(0x34, 0x1E, 0xFF);
    pagerKeyboardWriteRegister(0x34, 0x1F, 0x03);
    pagerKeyboardWriteRegister(0x34, 0x29, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x2A, 0x00);
    pagerKeyboardWriteRegister(0x34, 0x2B, 0x00);
    while ((pagerKeyboardReadRegister(0x34, 0x03) & 0x0F) != 0)
        pagerKeyboardReadRegister(0x34, 0x04);
    pagerKeyboardWriteRegister(0x34, 0x02, 0x03);
}""".replace("\n", newline.decode()).encode()
if pager_init_old in data:
    data = data.replace(pager_init_old, pager_init_new, 1)
elif pager_init_new not in data:
    raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard initialization")

pager_read_old = r"""void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}""".replace(
    "\n", newline.decode()
).encode()
pager_read_new = r"""void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool releasePending = false;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    if (releasePending) {
        releasePending = false;
        data->continue_reading = true;
        return;
    }

    uint8_t count = pagerKeyboardReadRegister(address, 0x03) & 0x0F;
    if (!count)
        return;
    while (count--) {
        uint8_t event = pagerKeyboardReadRegister(address, 0x04);
        if ((event & 0x80) == 0)
            continue;

        lv_display_trigger_activity(lv_indev_get_display(indev));
        uint8_t keyIndex = (event & 0x7F) - 1;
        uint32_t keyValue = 0;
        lv_group_t *group = lv_indev_get_group(indev);
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        const bool textArea = focused && lv_obj_check_type(focused, &lv_textarea_class);
        if (keyIndex == 20) {
            pagerKeyboardModifiers ^= 0x02;
            pagerFunctionModifierActive = (pagerKeyboardModifiers & 0x02) != 0;
        } else if (keyIndex == 28) {
            pagerKeyboardModifiers ^= 0x01;
        } else if (keyIndex == 19 || keyIndex == 29) {
            const bool back = !textArea || (pagerKeyboardModifiers & 0x02);
            pagerKeyboardModifiers = 0;
            pagerFunctionModifierActive = false;
            if (back) {
#if defined(INPUTDRIVER_ROTARY_TYPE)
                extern bool pagerHandleBack(lv_indev_t *indev);
                if (pagerHandleBack(indev))
                    continue;
#endif
                keyValue = LV_KEY_ESC;
            } else {
                keyValue = keyIndex == 19 ? LV_KEY_ENTER : LV_KEY_BACKSPACE;
            }
        } else if (keyIndex < 31) {
            const uint8_t mapIndex = (pagerKeyboardModifiers & 0x02) ? 2 : ((pagerKeyboardModifiers & 0x01) ? 1 : 0);
            keyValue = static_cast<uint8_t>(pagerKeyboardMap[keyIndex][mapIndex]);
            if (keyValue != 0) {
                pagerKeyboardModifiers = 0;
                pagerFunctionModifierActive = false;
            }
        }

        if (keyValue != 0) {
            data->key = keyValue;
            data->state = LV_INDEV_STATE_PRESSED;
            data->continue_reading = true;
            releasePending = true;
            return;
        }
    }
    pagerKeyboardWriteRegister(address, 0x02, 0x01);
}""".replace(
    "\n", newline.decode()
).encode()
if pager_read_old in data:
    data = data.replace(pager_read_old, pager_read_new, 1)
elif pager_read_new not in data:
    reader_start = data.find(b"void TLoraPagerKeyboardInputDriver::readKeyboard")
    reader_end = data.find(b"// ---------- TDeckProKeyboardInputDriver", reader_start)
    previous_reader = (
        data[reader_start:reader_end] if reader_start >= 0 and reader_end >= 0 else b""
    )
    if b"pagerKeyboardReadRegister(address, 0x03)" not in previous_reader and (
        b"static uint8_t modifiers = 0;" not in previous_reader
        or b"LV_KEY_NEXT : LV_KEY_ENTER" not in previous_reader
    ):
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard reader")
    data = data[:reader_start] + pager_read_new + newline + data[reader_end:]

keyboard_source.write_bytes(data)

sensor_source = library.parent / "SensorLib" / "src" / "bosch" / "bhi260x" / "bhy2.c"
sensor_data = sensor_source.read_bytes()
sensor_newline = b"\r\n" if b"\r\n" in sensor_data else b"\n"

copy_old = b"static int8_t parse_fifo_support(struct bhy2_fifo_buffer *fifo_buf)\n{\n    uint8_t i;"
copy_new = copy_old.replace(b"uint8_t i;", b"uint32_t i;")
event_old = b"""            default:
                rslt = get_callback_info(tmp_sensor_id, &info, dev);
                rslt = check_return_value(rslt);"""
event_new = event_old + b"""
                if (dev->event_size[tmp_sensor_id] == 0)
                    return BHY2_E_INVALID_PARAM;"""

for old, new in ((copy_old, copy_new), (event_old, event_new)):
    old = old.replace(b"\n", sensor_newline)
    new = new.replace(b"\n", sensor_newline)
    if sensor_data.count(new) == 1:
        continue
    if sensor_data.count(old) != 1:
        raise RuntimeError("unexpected SensorLib BHI260AP FIFO parser")
    sensor_data = sensor_data.replace(old, new, 1)

sensor_source.write_bytes(sensor_data)
