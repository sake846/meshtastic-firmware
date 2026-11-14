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
focus_event_marker = b"static void pagerNodeFocusEvent(lv_event_t *e)"
focus_event_start = data.find(focus_event_marker)
focus_helper_start = data.find(focus_helper_marker)
if 0 <= focus_event_start < focus_helper_start:
    data = data[:focus_event_start] + data[focus_helper_start:]
if focus_helper_marker in data:
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

source.write_bytes(data)


qzss_font_bdf = (
    Path(env.subst("$PROJECT_DIR"))
    / "variants"
    / "esp32s3"
    / "tlora-pager"
    / "fonts"
    / "shnmk12.bdf"
)
qzss_font_header = library / "source" / "graphics" / "TFT" / "pager_shinonome_12.h"
if not qzss_font_bdf.is_file():
    raise RuntimeError(f"Shinonome BDF source not found: {qzss_font_bdf}")


def decode_bdf_encoding(encoding):
    if encoding < 0:
        return None
    row, cell = encoding >> 8, encoding & 0xFF
    if 0x21 <= row <= 0x7E and 0x21 <= cell <= 0x7E:
        try:
            decoded = bytes((row + 0x80, cell + 0x80)).decode("euc_jp")
            return ord(decoded) if len(decoded) == 1 else None
        except UnicodeDecodeError:
            return None
    return encoding


qzss_glyphs = {}
qzss_ascent = 10
qzss_descent = 2
qzss_glyph = None
qzss_bitmap_rows = None
for line in qzss_font_bdf.read_text(encoding="ascii").splitlines():
    if line.startswith("FONT_ASCENT "):
        qzss_ascent = int(line.split()[1])
    elif line.startswith("FONT_DESCENT "):
        qzss_descent = int(line.split()[1])
    elif line.startswith("STARTCHAR "):
        qzss_glyph = {}
        qzss_bitmap_rows = None
    elif qzss_glyph is not None and line.startswith("ENCODING "):
        qzss_glyph["codepoint"] = decode_bdf_encoding(int(line.split()[1]))
    elif qzss_glyph is not None and line.startswith("DWIDTH "):
        qzss_glyph["advance"] = int(line.split()[1])
    elif qzss_glyph is not None and line.startswith("BBX "):
        (
            qzss_glyph["width"],
            qzss_glyph["height"],
            qzss_glyph["offset_x"],
            qzss_glyph["offset_y"],
        ) = map(int, line.split()[1:])
    elif qzss_glyph is not None and line == "BITMAP":
        qzss_bitmap_rows = []
    elif qzss_glyph is not None and line == "ENDCHAR":
        codepoint = qzss_glyph.get("codepoint")
        if codepoint is not None:
            width = qzss_glyph["width"]
            height = qzss_glyph["height"]
            pixels = []
            for row in qzss_bitmap_rows or []:
                values = bytes.fromhex(row)
                for x in range(width):
                    pixels.append(255 if values[x // 8] & (0x80 >> (x % 8)) else 0)
            if len(pixels) != width * height or len(qzss_bitmap_rows or []) != height:
                raise RuntimeError(f"Invalid BDF bitmap for U+{codepoint:04X}")
            qzss_glyphs[codepoint] = dict(qzss_glyph, bitmap=pixels)
        qzss_glyph = None
        qzss_bitmap_rows = None
    elif qzss_glyph is not None and qzss_bitmap_rows is not None:
        qzss_bitmap_rows.append(line)

qzss_text = "みちびき 災危通報"
qzss_chars = sorted({ord(char) for char in qzss_text if char != " "})
missing_qzss_chars = [
    chr(codepoint) for codepoint in qzss_chars if codepoint not in qzss_glyphs
]
if missing_qzss_chars:
    raise RuntimeError(f"Shinonome is missing QZSS chat glyphs: {missing_qzss_chars}")

qzss_selected = [qzss_glyphs[codepoint] for codepoint in qzss_chars]
qzss_line_height = max(qzss_ascent + qzss_descent, 19)
qzss_baseline = max(qzss_descent, 4)
qzss_header_lines = [
    "#pragma once",
    "#include <lvgl.h>",
    "#include <cstddef>",
    "#include <cstdint>",
    "extern const lv_font_t ui_font_montserrat_14;",
    "namespace {",
    "struct PagerShinonomeGlyph { uint32_t codepoint; uint16_t advance, width, height; int16_t offsetX, offsetY; const uint8_t *bitmap; };",
]
for i, glyph in enumerate(qzss_selected):
    qzss_header_lines.append(f"static const uint8_t pagerShinonomeBitmap{i}[] = {{")
    pixels = glyph["bitmap"]
    for offset in range(0, len(pixels), 18):
        qzss_header_lines.append(
            "    "
            + ", ".join(f"{value}" for value in pixels[offset : offset + 18])
            + ","
        )
    qzss_header_lines.append("};")
qzss_header_lines.append("static const PagerShinonomeGlyph pagerShinonomeGlyphs[] = {")
for i, glyph in enumerate(qzss_selected):
    qzss_header_lines.append(
        f"    {{0x{glyph['codepoint']:04X}, {glyph['advance']}, {glyph['width']}, {glyph['height']}, "
        f"{glyph['offset_x']}, {glyph['offset_y']}, pagerShinonomeBitmap{i}}},"
    )
qzss_header_lines.extend(
    [
        "};",
        "static bool pagerShinonomeGetGlyphDsc(const lv_font_t *, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t)",
        "{",
        "    size_t low = 0, high = sizeof(pagerShinonomeGlyphs) / sizeof(pagerShinonomeGlyphs[0]);",
        "    while (low < high) {",
        "        const size_t middle = low + (high - low) / 2;",
        "        if (pagerShinonomeGlyphs[middle].codepoint < letter) low = middle + 1; else high = middle;",
        "    }",
        "    if (low == sizeof(pagerShinonomeGlyphs) / sizeof(pagerShinonomeGlyphs[0]) || pagerShinonomeGlyphs[low].codepoint != letter) return false;",
        "    const PagerShinonomeGlyph &glyph = pagerShinonomeGlyphs[low];",
        "    dsc->adv_w = glyph.advance; dsc->box_w = glyph.width; dsc->box_h = glyph.height;",
        "    dsc->ofs_x = glyph.offsetX; dsc->ofs_y = glyph.offsetY; dsc->stride = glyph.width;",
        "    dsc->format = LV_FONT_GLYPH_FORMAT_A8; dsc->gid.index = static_cast<uint32_t>(low);",
        "    return true;",
        "}",
        "static const void *pagerShinonomeGetGlyphBitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *)",
        "{",
        "    const size_t count = sizeof(pagerShinonomeGlyphs) / sizeof(pagerShinonomeGlyphs[0]);",
        "    return dsc->gid.index < count ? pagerShinonomeGlyphs[dsc->gid.index].bitmap : nullptr;",
        "}",
        "}",
        "const lv_font_t pager_shinonome_12 = {pagerShinonomeGetGlyphDsc, pagerShinonomeGetGlyphBitmap, nullptr, "
        f"{qzss_line_height}, {qzss_baseline}, LV_FONT_SUBPX_NONE, LV_FONT_KERNING_NONE, 1, 0, 1, nullptr, "
        "&ui_font_montserrat_14, nullptr};",
    ]
)
qzss_font_header.write_text("\n".join(qzss_header_lines) + "\n", encoding="utf-8")

qzss_data = source.read_bytes()
qzss_newline = b"\r\n" if b"\r\n" in qzss_data else b"\n"
qzss_include = b'#include "pager_shinonome_12.h"'
if qzss_include not in qzss_data:
    qzss_include_anchor = b'#include "ui.h"' + qzss_newline
    if qzss_data.count(qzss_include_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui font include location")
    qzss_data = qzss_data.replace(
        qzss_include_anchor, qzss_include_anchor + qzss_include + qzss_newline, 1
    )
if b"#include <cstring>" not in qzss_data:
    qzss_data = qzss_data.replace(
        b"#include <algorithm>" + qzss_newline,
        b"#include <algorithm>" + qzss_newline + b"#include <cstring>" + qzss_newline,
        1,
    )
font_width_anchor = b"    lv_coord_t width = lv_txt_get_width(msg, strlen(msg), &ui_font_montserrat_14, 0);"
font_width_patch = (
    b'    const bool pagerQzssDcr = ch < c_max_channels && std::strcmp(db.channel[ch].settings.name, "QZSS-DCR") == 0;'
    + qzss_newline
    + b"    const lv_font_t *messageFont = pagerQzssDcr ? &pager_shinonome_12 : &ui_font_montserrat_14;"
    + qzss_newline
    + b"    lv_coord_t width = lv_txt_get_width(msg, strlen(msg), messageFont, 0);"
)
if b"const bool pagerQzssDcr" not in qzss_data:
    if qzss_data.count(font_width_anchor) != 1:
        raise RuntimeError("unexpected message bubble font width calculation")
    qzss_data = qzss_data.replace(font_width_anchor, font_width_patch, 1)
font_style_anchor = b"    add_style_new_message_style(msgLabel);"
font_style_patch = (
    font_style_anchor
    + qzss_newline
    + b"    if (pagerQzssDcr)"
    + qzss_newline
    + b"        lv_obj_set_style_text_font(msgLabel, messageFont, LV_PART_MAIN | LV_STATE_DEFAULT);"
)
if b"lv_obj_set_style_text_font(msgLabel, messageFont" not in qzss_data:
    if qzss_data.count(font_style_anchor) != 1:
        raise RuntimeError("unexpected message bubble style location")
    qzss_data = qzss_data.replace(font_style_anchor, font_style_patch, 1)
read_only_anchor = b"    bool usePkc = false;" + qzss_newline
read_only_patch = (
    b"    bool usePkc = false;"
    + qzss_newline
    + b'    if (channelOrNode < c_max_channels && std::strcmp(db.channel[channelOrNode].settings.name, "QZSS-DCR") == 0) {'
    + qzss_newline
    + b'        ILOG_INFO("QZSS-DCR chat is read-only");'
    + qzss_newline
    + b"        return;"
    + qzss_newline
    + b"    }"
)
if b"QZSS-DCR chat is read-only" not in qzss_data:
    if qzss_data.count(read_only_anchor) != 1:
        raise RuntimeError("unexpected message send guard location")
    qzss_data = qzss_data.replace(read_only_anchor, read_only_patch, 1)

source.write_bytes(qzss_data)
device_gui = library / "source" / "graphics" / "DeviceGUI.cpp"
if not device_gui.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {device_gui}")

data = device_gui.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
rotary_marker = b"class PagerRotaryInputDriver final"
rotary_driver = """#if defined(INPUTDRIVER_ROTARY_TYPE)
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
    if (!group || lv_screen_active() != objects.main_screen || !pagerMenuButtonAvailable(pagerActiveMenuButton))
        return false;

    lv_group_set_editing(group, false);
    lv_group_focus_obj(pagerActiveMenuButton);
    return true;
}

static bool pagerMoveFocus(lv_group_t *group, int16_t diff)
{
    if (!group || !diff)
        return false;

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
        lv_group_focus_next(group);
        for (int i = 0; i < 6 && pagerMenuButtonIndex(lv_group_get_focused(group)) >= 0; ++i)
            lv_group_focus_next(group);
        --diff;
    }
    while (diff < 0) {
        lv_group_focus_prev(group);
        for (int i = 0; i < 6 && pagerMenuButtonIndex(lv_group_get_focused(group)) >= 0; ++i)
            lv_group_focus_prev(group);
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
    }

    void task_handler() override
    {
        switch (rotary.process()) {
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
    }

  private:
    static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
    {
        const bool buttonPressed = rotaryInstance && rotaryInstance->readButton() == RotaryEncoder::BUTTON_PRESSED;
        bool endTextEdit = false;
        lv_group_t *group = indev ? lv_indev_get_group(indev) : nullptr;
        if (buttonPressed && !suppressEncoderClick && pagerFunctionModifierActive) {
            pagerClearFunctionModifier();
            pagerHandleBack(indev);
            suppressEncoderClick = true;
        }
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        if (buttonPressed && !suppressEncoderClick && group && lv_group_get_editing(group) && focused &&
            lv_obj_check_type(focused, &lv_textarea_class)) {
            lv_group_set_editing(group, false);
            suppressEncoderClick = true;
            endTextEdit = true;
        }

        const int16_t diff = encoderDiff;
        encoderDiff = 0;
        data->key = endTextEdit || suppressEncoderClick ? 0 : LV_KEY_ENTER;
        data->enc_diff = pagerMoveFocus(group, diff) ? 0 : diff;
        data->state = buttonPressed && !suppressEncoderClick ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (!buttonPressed)
            suppressEncoderClick = false;
    }

    RotaryEncoder rotary;
    inline static RotaryEncoder *rotaryInstance = nullptr;
    inline static int16_t encoderDiff = 0;
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
if b"pagerRotaryDriver->task_handler();" not in data:
    if data.count(task_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI task handler")
    data = data.replace(task_anchor, task_patch, 1)

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
    Wire.endTransmission();
    if (Wire.requestFrom(address, static_cast<uint8_t>(1)) == 0)
        return 0;
    return Wire.read();
}

static void pagerKeyboardWriteRegister(uint8_t address, uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
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
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    if ((pagerKeyboardReadRegister(address, 0x03) & 0x0F) == 0)
        return;

    uint8_t event = pagerKeyboardReadRegister(address, 0x04);
    if ((event & 0x80) == 0)
        return;

    uint8_t keyIndex = (event & 0x7F) - 1;
    uint32_t keyValue = 0;
    if (keyIndex == 20) {
        pagerKeyboardModifiers ^= 0x02;
        pagerFunctionModifierActive = (pagerKeyboardModifiers & 0x02) != 0;
    } else if (keyIndex == 28) {
        pagerKeyboardModifiers ^= 0x01;
    } else if (keyIndex == 29) {
        keyValue = (pagerKeyboardModifiers & 0x02) ? LV_KEY_ESC : LV_KEY_BACKSPACE;
        pagerKeyboardModifiers = 0;
        pagerFunctionModifierActive = false;
    } else if (keyIndex == 19) {
        lv_group_t *group = lv_indev_get_group(indev);
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        const bool oneLineTextArea = focused && lv_obj_check_type(focused, &lv_textarea_class) &&
                                       lv_textarea_get_one_line(focused);
#if defined(INPUTDRIVER_ROTARY_TYPE)
        if (pagerKeyboardModifiers & 0x02) {
            pagerClearFunctionModifier();
            extern bool pagerHandleBack(lv_indev_t *indev);
            pagerHandleBack(indev);
            return;
        }
#endif
        keyValue = ((pagerKeyboardModifiers & 0x02) || oneLineTextArea) ? LV_KEY_NEXT : LV_KEY_ENTER;
        pagerKeyboardModifiers = 0;
        pagerFunctionModifierActive = false;
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
    }
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
    if (
        b"static uint8_t modifiers = 0;" not in previous_reader
        or b"LV_KEY_NEXT : LV_KEY_ENTER" not in previous_reader
    ):
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard reader")
    data = data[:reader_start] + pager_read_new + newline + data[reader_end:]

keyboard_source.write_bytes(data)
