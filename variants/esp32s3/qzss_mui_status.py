Import("env")

from pathlib import Path


def replace_once(data, anchor, replacement, description):
    if data.count(anchor) != 1:
        raise RuntimeError(f"unexpected MUI {description} location")
    return data.replace(anchor, replacement, 1)


def patch_mui_status():
    library = (
        Path(env.subst("$PROJECT_LIBDEPS_DIR"))
        / env.subst("$PIOENV")
        / "meshtastic-device-ui"
    )
    source = library / "source" / "graphics" / "TFT" / "TFTView_320x240.cpp"
    if not source.is_file():
        return

    data = source.read_bytes()
    newline = b"\r\n" if b"\r\n" in data else b"\n"
    include_anchor = b'#include "graphics/view/TFT/TFTView_320x240.h"' + newline
    if b'#include "gps/QzssDcr.h"' not in data:
        data = replace_once(
            data,
            include_anchor,
            include_anchor + b'#include "gps/QzssDcr.h"' + newline,
            "QZSS include",
        )

    badges = (
        (b"pagerQzssBadge", b"pagerRefreshQzssBadge", b"pagerQzssBadgeTimer"),
        (b"tDeckQzssBadge", b"tDeckRefreshQzssBadge", b"tDeckQzssBadgeTimer"),
        (b"muiQzssBadge", b"muiRefreshQzssBadge", b"muiQzssBadgeTimer"),
    )
    badge, refresh, timer = next(
        (item for item in badges if item[0] in data), badges[-1]
    )
    if badge == b"muiQzssBadge" and badge not in data:
        decl_anchor = b"#include <time.h>" + newline
        declaration = (
            decl_anchor
            + b"static lv_obj_t *muiQzssBadge = nullptr;"
            + newline
            + b"static void muiRefreshQzssBadge()"
            + newline
            + b"{"
            + newline
            + b"    if (!muiQzssBadge)"
            + newline
            + b"        return;"
            + newline
            + b"}"
            + newline
            + b"static void muiQzssBadgeTimer(lv_timer_t *)"
            + newline
            + b"{"
            + newline
            + b"    muiRefreshQzssBadge();"
            + newline
            + b"}"
            + newline
        )
        data = replace_once(data, decl_anchor, declaration, "QZSS status declaration")
    else:
        create_anchor = badge + b" = lv_label_create(objects.home_location_label);"
        if create_anchor in data:
            data = data.replace(
                create_anchor,
                badge
                + b" = lv_label_create(lv_obj_get_parent(objects.home_location_label));",
                1,
            )

    helper_start = data.index(b"static void " + refresh + b"()")
    timer_start = data.index(b"static void " + timer + b"(", helper_start)
    helper = (
        b"static void "
        + refresh
        + b"()"
        + newline
        + b"{"
        + newline
        + b"    if (!"
        + badge
        + b")"
        + newline
        + b"        return;"
        + newline
        + b"    const bool detected = QzssDcr::isM10QDetected();"
        + newline
        + b"    const bool enabled = QzssDcr::isEnabled();"
        + newline
        + b"    uint8_t svId = 0;"
        + newline
        + b"    const bool captured = QzssDcr::getCapturedSatellite(svId);"
        + newline
        + b"    if (!detected) lv_label_set_text("
        + badge
        + b', "NO M10Q");'
        + newline
        + b"    else if (!enabled) lv_label_set_text("
        + badge
        + b', "M10Q L1S OFF");'
        + newline
        + b"    else if (captured) lv_label_set_text_fmt("
        + badge
        + b', "M10Q Q%u", svId);'
        + newline
        + b"    else lv_label_set_text("
        + badge
        + b', "M10Q SEARCH");'
        + newline
        + b"    lv_obj_set_style_text_color("
        + badge
        + b","
        + newline
        + b"                                lv_color_hex(!detected || (enabled && !captured) ? 0xFFAB00 :"
        + newline
        + b"                                            enabled ? 0x00C853 : 0x808080),"
        + newline
        + b"                                LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_label, LV_ALIGN_TOP_RIGHT, 0, 0);"
        + newline
        + b"    lv_obj_move_foreground("
        + badge
        + b");"
        + newline
        + b"}"
        + newline
    )
    data = data[:helper_start] + helper + data[timer_start:]

    old_align = b"    lv_obj_align(" + badge + b", LV_ALIGN_TOP_RIGHT, -2, 0);"
    data = data.replace(
        old_align,
        b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_label, LV_ALIGN_TOP_RIGHT, 0, 0);",
    )
    data = data.replace(
        b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_button, LV_ALIGN_OUT_RIGHT_MID, 3, 0);",
        b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_label, LV_ALIGN_TOP_RIGHT, 0, 0);",
    )
    old_align = (
        b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 1);"
    )
    data = data.replace(
        old_align,
        b"    lv_obj_align_to("
        + badge
        + b", objects.home_location_label, LV_ALIGN_TOP_RIGHT, 0, 0);",
    )

    short_event = (
        b"    lv_obj_add_event_cb(objects.home_location_button, this->ui_event_LocationButton, LV_EVENT_SHORT_CLICKED, NULL);"
        + newline
    )
    long_event = (
        b"    lv_obj_add_event_cb(objects.home_location_button, this->ui_event_LocationButton, LV_EVENT_LONG_PRESSED, NULL);"
        + newline
    )
    if short_event not in data:
        data = replace_once(
            data, long_event, long_event + short_event, "location button event"
        )

    create_line = (
        badge
        + b" = lv_label_create(lv_obj_get_parent(objects.home_location_label));"
        + newline
    )
    if (
        create_line in data
        and b"lv_obj_add_flag(" + badge + b", LV_OBJ_FLAG_IGNORE_LAYOUT);" not in data
    ):
        data = replace_once(
            data,
            create_line,
            create_line
            + b"    lv_obj_add_flag("
            + badge
            + b", LV_OBJ_FLAG_IGNORE_LAYOUT);"
            + newline,
            "QZSS status layout",
        )
    data = data.replace(
        b"lv_obj_set_style_text_font(" + badge + b", &ui_font_montserrat_12,",
        b"lv_obj_set_style_text_font(" + badge + b", &lv_font_montserrat_10,",
    )

    init = (
        b"    muiQzssBadge = lv_label_create(lv_obj_get_parent(objects.home_location_label));"
        + newline
        + b"    lv_obj_add_flag(muiQzssBadge, LV_OBJ_FLAG_IGNORE_LAYOUT);"
        + newline
        + b"    lv_obj_set_style_text_font(muiQzssBadge, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    lv_obj_set_style_bg_color(muiQzssBadge, lv_color_hex(0x202020), LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    lv_obj_set_style_bg_opa(muiQzssBadge, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    lv_obj_set_style_radius(muiQzssBadge, 3, LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    lv_obj_set_style_pad_all(muiQzssBadge, 2, LV_PART_MAIN | LV_STATE_DEFAULT);"
        + newline
        + b"    muiRefreshQzssBadge();"
        + newline
        + b"    lv_timer_create(muiQzssBadgeTimer, 1000, nullptr);"
        + newline
    )
    if badge == b"muiQzssBadge" and b"muiQzssBadge = lv_label_create" not in data:
        data = replace_once(
            data, short_event, short_event + init, "QZSS status initialization"
        )

    toggle = b"QzssDcr::setReceptionEnabled(!QzssDcr::isEnabled());"
    if toggle not in data:
        long_press = (
            b"    } else if (event_code == LV_EVENT_LONG_PRESSED && THIS->configComplete) {"
            + newline
        )
        short_press = (
            b"    } else if (event_code == LV_EVENT_SHORT_CLICKED && THIS->configComplete && THIS->activeSettings == eNone) {"
            + newline
            + b"        QzssDcr::setReceptionEnabled(!QzssDcr::isEnabled());"
            + newline
            + b"        "
            + refresh
            + b"();"
            + newline
        )
        data = replace_once(
            data, long_press, short_press + long_press, "location button handler"
        )
    else:
        old_toggle_refresh = toggle + newline + b"        " + refresh + b"();"
        if old_toggle_refresh not in data:
            data = data.replace(
                toggle, toggle + newline + b"        " + refresh + b"();", 1
            )

    source.write_bytes(data)


patch_mui_status()
