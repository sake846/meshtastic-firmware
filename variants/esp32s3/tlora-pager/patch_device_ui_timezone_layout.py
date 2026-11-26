Import("env")

from pathlib import Path

ui_source = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "meshtastic-device-ui"
    / "source"
    / "graphics"
    / "TFT"
    / "TFTView_320x240.cpp"
)
if not ui_source.is_file():
    raise RuntimeError(f"Pager device UI source not found: {ui_source}")

source = ui_source.read_bytes()
newline = b"\r\n" if b"\r\n" in source else b"\n"
changes = (
    (
        b"lv_obj_set_size(objects.settings_timezone, 220, 120);\n"
        b"    lv_obj_set_pos(objects.settings_timezone, LV_PCT(3), LV_PCT(25));",
        b"lv_obj_set_size(objects.settings_timezone, 220, 180);\n"
        b"    lv_obj_set_pos(objects.settings_timezone, LV_PCT(3), LV_PCT(10));",
    ),
    (
        b"lv_dropdown_set_options_static(objects.settings_tzzone, pagerTimezoneLabels);",
        b"lv_dropdown_set_options_static(objects.settings_tzzone, pagerTimezoneLabels);\n"
        b"    lv_obj_set_style_max_height(lv_dropdown_get_list(objects.settings_tzzone), 64, LV_PART_MAIN);",
    ),
)

for old, new in changes:
    old = old.replace(b"\n", newline)
    new = new.replace(b"\n", newline)
    if source.count(new) == 1:
        continue
    if source.count(old) != 1:
        raise RuntimeError("unexpected Pager timezone layout in device UI")
    source = source.replace(old, new, 1)

ui_source.write_bytes(source)
