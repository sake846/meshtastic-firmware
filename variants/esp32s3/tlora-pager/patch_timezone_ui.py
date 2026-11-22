Import("env")

from pathlib import Path

library = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "meshtastic-device-ui"
)
source = library / "source" / "graphics" / "TFT" / "TFTView_320x240.cpp"
header = library / "include" / "graphics" / "view" / "TFT" / "TFTView_320x240.h"
if not source.is_file() or not header.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {library}")


def insert_once(content, anchor, insertion, marker, description, newline):
    if marker in content:
        return content
    if content.count(anchor) != 1:
        raise RuntimeError(f"unexpected meshtastic-device-ui {description}")
    return content.replace(anchor, insertion + anchor, 1)


header_data = header.read_bytes()
header_newline = b"\r\n" if b"\r\n" in header_data else b"\n"
header_data = insert_once(
    header_data,
    b"        eLanguage," + header_newline,
    b"        eTimezone," + header_newline,
    b"eTimezone,",
    "settings enum",
    header_newline,
)
header_data = insert_once(
    header_data,
    b"    static void ui_event_language_button(lv_event_t *e);" + header_newline,
    b"    static void ui_event_timezone_button(lv_event_t *e);" + header_newline,
    b"ui_event_timezone_button",
    "settings event declaration",
    header_newline,
)
header.write_bytes(header_data)

data = source.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
timezone_helpers = r"""static constexpr struct PagerTimezoneOption {
    const char *label;
    const char *value;
} pagerTimezoneOptions[] = {
    {"UTC", "UTC0"},
    {"Japan (UTC+9)", "JST-9"},
    {"US/Hawaii", "HST10"},
    {"US/Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US/Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US/Arizona", "MST7"},
    {"US/Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US/Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US/Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"BR/Brasilia", "BRT3"},
    {"EU/Western", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"EU/Central", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"EU/Eastern", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"AU/AWST", "AWST-8"},
    {"AU/ACST", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"AU/AEST", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/NZ", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

static constexpr const char *pagerTimezoneLabels =
    "UTC\nJapan (UTC+9)\nUS/Hawaii\nUS/Alaska\nUS/Pacific\nUS/Arizona\nUS/Mountain\nUS/Central\nUS/Eastern\n"
    "BR/Brasilia\nEU/Western\nEU/Central\nEU/Eastern\nAsia/Kolkata\nAsia/Hong_Kong\nAU/AWST\nAU/ACST\nAU/AEST\nPacific/NZ";

static uint32_t pagerTimezoneIndex(const char *value)
{
    for (uint32_t i = 0; i < sizeof(pagerTimezoneOptions) / sizeof(pagerTimezoneOptions[0]); ++i) {
        if (value && strcmp(value, pagerTimezoneOptions[i].value) == 0)
            return i;
    }
    return 0;
}

static void pagerUpdateTimezoneLabel(const char *value)
{
    const char *label = value && *value ? value : pagerTimezoneOptions[0].label;
    for (const auto &option : pagerTimezoneOptions) {
        if (value && strcmp(value, option.value) == 0) {
            label = option.label;
            break;
        }
    }

    char text[64];
    lv_snprintf(text, sizeof(text), _("Timezone: %s"), label);
    lv_label_set_text(objects.basic_settings_timezone_label, text);
}

""".replace("\n", newline.decode()).encode()
data = insert_once(
    data,
    b"void TFTView_320x240::init_screens(void)",
    timezone_helpers,
    b"pagerTimezoneOptions[]",
    "timezone settings helpers",
    newline,
)

timezone_button_event = r"""void TFTView_320x240::ui_event_timezone_button(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && THIS->activeSettings == eNone && THIS->db.config.has_device) {
        lv_dropdown_set_selected(objects.settings_tzzone, pagerTimezoneIndex(THIS->db.config.device.tzdef));
        lv_obj_remove_flag(objects.settings_timezone, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_tzzone);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eTimezone;
    }
}

""".replace(
    "\n", newline.decode()
).encode()
data = insert_once(
    data,
    b"void TFTView_320x240::ui_event_channel_button(lv_event_t *e)",
    timezone_button_event,
    b"TFTView_320x240::ui_event_timezone_button",
    "timezone button handler",
    newline,
)

timezone_init = (
    b"    apply_hotfix();"
    + newline
    + b"    lv_obj_remove_flag(objects.basic_settings_timezone_button, LV_OBJ_FLAG_HIDDEN);"
    + newline
    + b"    lv_obj_set_size(objects.settings_timezone, 220, 120);"
    + newline
    + b'    lv_label_set_text(lv_obj_get_child(objects.settings_timezone, 0), _("Timezone"));'
    + newline
    + b"    lv_obj_add_flag(lv_obj_get_child(objects.settings_timezone, 2), LV_OBJ_FLAG_HIDDEN);"
    + newline
    + b"    lv_obj_add_flag(objects.settings_tzcity, LV_OBJ_FLAG_HIDDEN);"
    + newline
    + b"    lv_obj_set_pos(objects.settings_tzzone, 5, 30);"
    + newline
    + b"    lv_obj_set_width(objects.settings_tzzone, 200);"
    + newline
    + b"    lv_dropdown_set_options_static(objects.settings_tzzone, pagerTimezoneLabels);"
    + newline
    + b'    pagerUpdateTimezoneLabel(db.config.has_device ? db.config.device.tzdef : "");'
    + newline
)
if b"pagerTimezoneLabels);" not in data:
    if data.count(b"    apply_hotfix();" + newline) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui screen initialization")
    data = data.replace(b"    apply_hotfix();" + newline, timezone_init, 1)

data = insert_once(
    data,
    b"    lv_obj_add_event_cb(objects.basic_settings_language_button, ui_event_language_button, LV_EVENT_CLICKED, NULL);"
    + newline,
    b"    lv_obj_add_event_cb(objects.basic_settings_timezone_button, ui_event_timezone_button, LV_EVENT_CLICKED, NULL);"
    + newline,
    b"objects.basic_settings_timezone_button, ui_event_timezone_button",
    "timezone button registration",
    newline,
)
data = insert_once(
    data,
    b"    case eLanguage:"
    + newline
    + b"        return objects.settings_language_panel;"
    + newline,
    b"    case eTimezone:"
    + newline
    + b"        return objects.settings_timezone;"
    + newline,
    b"case eTimezone:" + newline + b"        return objects.settings_timezone;",
    "timezone panel mapping",
    newline,
)

timezone_save = (
    b"        case eTimezone: {"
    + newline
    + b"            meshtastic_Config_DeviceConfig &device = THIS->db.config.device;"
    + newline
    + b"            const uint32_t index = lv_dropdown_get_selected(objects.settings_tzzone);"
    + newline
    + b"            if (index < sizeof(pagerTimezoneOptions) / sizeof(pagerTimezoneOptions[0]) &&"
    + newline
    + b"                strcmp(device.tzdef, pagerTimezoneOptions[index].value) != 0) {"
    + newline
    + b"                strncpy(device.tzdef, pagerTimezoneOptions[index].value, sizeof(device.tzdef));"
    + newline
    + b"                device.tzdef[sizeof(device.tzdef) - 1] = '\\0';"
    + newline
    + b'                setenv("TZ", device.tzdef, 1);'
    + newline
    + b"                tzset();"
    + newline
    + b"                THIS->controller->sendConfig(meshtastic_Config_DeviceConfig{device}, THIS->ownNode);"
    + newline
    + b"                pagerUpdateTimezoneLabel(device.tzdef);"
    + newline
    + b"            }"
    + newline
    + b"            lv_obj_add_flag(objects.settings_timezone, LV_OBJ_FLAG_HIDDEN);"
    + newline
    + b"            lv_group_focus_obj(objects.basic_settings_timezone_button);"
    + newline
    + b"            break;"
    + newline
    + b"        }"
    + newline
)
data = insert_once(
    data,
    b"        case eScreenTimeout: {" + newline,
    timezone_save,
    b"case eTimezone: {",
    "timezone save handler",
    newline,
)

timezone_cancel = (
    b"        case TFTView_320x240::eTimezone: {"
    + newline
    + b"            lv_obj_add_flag(objects.settings_timezone, LV_OBJ_FLAG_HIDDEN);"
    + newline
    + b"            lv_group_focus_obj(objects.basic_settings_timezone_button);"
    + newline
    + b"            break;"
    + newline
    + b"        }"
    + newline
)
data = insert_once(
    data,
    b"        case TFTView_320x240::eScreenTimeout: {" + newline,
    timezone_cancel,
    b"case TFTView_320x240::eTimezone: {",
    "timezone cancel handler",
    newline,
)

device_config_anchor = (
    b"    lv_label_set_text(objects.basic_settings_role_label, buf2);"
    + newline
    + b"}"
    + newline
    + newline
    + b"void TFTView_320x240::updatePositionConfig"
)
device_config_patch = (
    b"    lv_label_set_text(objects.basic_settings_role_label, buf2);"
    + newline
    + b"    pagerUpdateTimezoneLabel(cfg.tzdef);"
    + newline
    + b"}"
    + newline
    + newline
    + b"void TFTView_320x240::updatePositionConfig"
)
if b"pagerUpdateTimezoneLabel(cfg.tzdef);" not in data:
    if data.count(device_config_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui device config handler")
    data = data.replace(device_config_anchor, device_config_patch, 1)

source.write_bytes(data)
