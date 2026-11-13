Import("env")

from pathlib import Path


library = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "meshtastic-device-ui"
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
    + b"    for (lv_obj_t *screen : pagerMUIScreens) {" + newline
    + b"        if (screen)" + newline
    + b"            lv_obj_set_size(screen, pagerMUIWidth, pagerMUIHeight);" + newline
    + b"    }" + newline
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
    b"    if (h > 320 && v > 320) {" + newline
    + b"        lv_obj_t *button[] = {objects.home_button,     objects.nodes_button, objects.groups_button," + newline
    + b"                              objects.messages_button, objects.map_button,   objects.settings_button};" + newline
    + b"        for (int i = 0; i < 6; i++) {" + newline
    + b"            lv_obj_set_size(button[i], 72, 72);" + newline
    + b"        }" + newline
    + b"    }"
)
icon_patch = (
    icon_anchor
    + b" else if (h == 480 && v == 222) {" + newline
    + b"        lv_obj_t *pagerMUICompactIcons[] = {objects.home_button, objects.nodes_button, objects.groups_button," + newline
    + b"                                                objects.messages_button, objects.map_button, objects.settings_button};" + newline
    + b"        for (lv_obj_t *icon : pagerMUICompactIcons)" + newline
    + b"            lv_obj_set_size(icon, 32, 32);" + newline
    + b"    }"
)
if b"pagerMUICompactIcons" not in data:
    if data.count(icon_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui navigation icon sizing")
    data = data.replace(icon_anchor, icon_patch, 1)

legacy_scroll = b"void TFTView_320x240::ui_event_scroll_panel" not in data
focus_anchor = b"void TFTView_320x240::apply_hotfix(void)" + newline
focus_helpers = r'''static void pagerApplyInsetFocus(lv_obj_t *obj)
{
    if (!obj)
        return;
    if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, focus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0x67ea94), focus);
    } else if (lv_obj_check_type(obj, &lv_button_class) || lv_obj_check_type(obj, &lv_imagebutton_class) ||
               lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_dropdown_class) ||
               lv_obj_check_type(obj, &lv_switch_class) || lv_obj_check_type(obj, &lv_checkbox_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, focus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0x67ea94), focus);
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        const lv_style_selector_t focus = (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        const lv_style_selector_t knobFocus = (lv_style_selector_t)LV_PART_KNOB | (lv_style_selector_t)LV_STATE_FOCUS_KEY;
        lv_obj_set_style_outline_width(obj, 0, focus);
        lv_obj_set_style_border_width(obj, 2, knobFocus);
        lv_obj_set_style_border_color(obj, lv_color_hex(0x67ea94), knobFocus);
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

'''.replace("\n", newline.decode()).encode()
if legacy_scroll:
    pager_scroll_helper = r'''static void pagerScrollPanelKey(lv_event_t *e)
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

'''.replace("\n", newline.decode()).encode()
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
    helper_start = data.index(focus_helper_marker)
    helper_end = data.index(focus_anchor, helper_start)
    data = data[:helper_start] + focus_helpers + data[helper_end:]
elif data.count(focus_anchor) == 1:
    data = data.replace(focus_anchor, focus_helpers + focus_anchor, 1)
else:
    raise RuntimeError("unexpected meshtastic-device-ui hotfix function")

focus_patch = (
    b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline
    + b"    lv_obj_t *pagerFocusScreens[] = {objects.main_screen, objects.boot_screen, objects.blank_screen," + newline
    + b"                                       objects.lock_screen, objects.calibration_screen};" + newline
    + b"    for (lv_obj_t *screen : pagerFocusScreens) {" + newline
    + b"        if (screen) {" + newline
    + b"            lv_obj_tree_walk(screen, pagerApplyInsetFocusWalk, nullptr);" + newline
    + b"            lv_obj_add_event_cb(screen, pagerApplyInsetFocusChildCreated, LV_EVENT_CHILD_CREATED, nullptr);" + newline
    + b"        }" + newline
    + b"    }" + newline
    + b"    lv_obj_set_width(objects.settings_user_long_textarea, LV_PCT(80));" + newline
    + b"    lv_obj_set_width(objects.settings_wifi_ssid_textarea, LV_PCT(80));" + newline
    + b"    lv_obj_set_width(objects.settings_wifi_password_textarea, LV_PCT(80));" + newline
    + b"    lv_obj_set_width(objects.settings_modify_channel_name_textarea, LV_PCT(82));" + newline
    + b"    lv_obj_set_width(objects.settings_modify_channel_psk_textarea, LV_PCT(82));" + newline
    + b"    lv_obj_set_width(objects.setup_user_long_textarea, LV_PCT(80));" + newline
)
focus_block = focus_patch[len(b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline) :]
if b"pagerFocusScreens" in data:
    block_start = data.index(b"    lv_obj_t *pagerFocusScreens[] = {")
    block_end_marker = b"    lv_obj_set_width(objects.setup_user_long_textarea, LV_PCT(80));" + newline
    block_end = data.index(block_end_marker, block_start) + len(block_end_marker)
    data = data[:block_start] + focus_block + data[block_end:]
elif data.count(b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline) == 1:
    focus_function = b"void TFTView_320x240::apply_hotfix(void)" + newline + b"{" + newline
    data = data.replace(focus_function, focus_function + focus_block, 1)
else:
    raise RuntimeError("unexpected meshtastic-device-ui hotfix function")

scroll_anchor = (
    b"    // Define how many pixels to scroll per keypress" + newline
    + b"    const lv_coord_t scroll_step = 80;" + newline + newline
    + b"    if (key == LV_KEY_DOWN) {" + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, -scroll_step, LV_ANIM_ON);" + newline
    + b"    } else if (key == LV_KEY_UP) {" + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, scroll_step, LV_ANIM_ON);" + newline
    + b"    }" + newline
    + b"    if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {"
)
scroll_patch = (
    b"    lv_indev_t *indev = lv_indev_active();" + newline
    + b"    const bool encoder = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;" + newline
    + b"    const lv_coord_t scroll_step = 80;" + newline
    + b"    if (encoder && key == LV_KEY_ENTER) {" + newline
    + b"        if (lv_group_t *group = lv_indev_get_group(indev))" + newline
    + b"            lv_group_set_editing(group, false);" + newline
    + b"        lv_event_stop_processing(e);" + newline
    + b"        return;" + newline
    + b"    }" + newline
    + b"    if (encoder && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {" + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, key == LV_KEY_RIGHT ? -scroll_step : scroll_step, LV_ANIM_ON);" + newline
    + b"        lv_event_stop_processing(e);" + newline
    + b"        return;" + newline
    + b"    }" + newline
    + b"    if (key == LV_KEY_DOWN) {" + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, -scroll_step, LV_ANIM_ON);" + newline
    + b"    } else if (key == LV_KEY_UP) {" + newline
    + b"        lv_obj_scroll_by_bounded(panel, 0, scroll_step, LV_ANIM_ON);" + newline
    + b"    }" + newline
    + b"    if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {"
)
if b"const bool encoder = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;" not in data:
    if data.count(scroll_anchor) == 1:
        data = data.replace(scroll_anchor, scroll_patch, 1)
    elif not legacy_scroll:
        raise RuntimeError("unexpected meshtastic-device-ui scroll panel key handling")

source.write_bytes(data)

device_gui = library / "source" / "graphics" / "DeviceGUI.cpp"
if not device_gui.is_file():
    raise RuntimeError(f"meshtastic-device-ui source not found: {device_gui}")

data = device_gui.read_bytes()
newline = b"\r\n" if b"\r\n" in data else b"\n"
rotary_marker = b"class PagerRotaryInputDriver final"
rotary_driver = """#if defined(INPUTDRIVER_ROTARY_TYPE)
#include "RotaryEncoder.h"

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
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        if (buttonPressed && !suppressEncoderClick && group && lv_group_get_editing(group) && focused &&
            lv_obj_check_type(focused, &lv_textarea_class)) {
            lv_group_set_editing(group, false);
            suppressEncoderClick = true;
            endTextEdit = true;
        }

        data->key = endTextEdit || suppressEncoderClick ? 0 : LV_KEY_ENTER;
        data->enc_diff = encoderDiff;
        encoderDiff = 0;
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
    rotary_start = data.rfind(b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline, 0, data.index(rotary_marker))
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
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline
    + b"    pagerRotaryDriver = new PagerRotaryInputDriver;" + newline
    + b"#endif" + newline
    + constructor_anchor
)
if b"pagerRotaryDriver = new PagerRotaryInputDriver;" not in data:
    if data.count(constructor_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI constructor")
    data = data.replace(constructor_anchor, constructor_patch, 1)

driver_pointer_anchor = b"static I2CKeyboardInputDriver *keyboardDriver = nullptr;" + newline
driver_pointer = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline
    + b"static PagerRotaryInputDriver *pagerRotaryDriver = nullptr;" + newline
    + b"#endif" + newline
)
if b"static PagerRotaryInputDriver *pagerRotaryDriver" not in data:
    if data.count(driver_pointer_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI driver declarations")
    data = data.replace(driver_pointer_anchor, driver_pointer_anchor + driver_pointer, 1)

init_anchor = b"    displaydriver->printConfig();" + newline
init_patch = (
    b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline
    + b"    if (pagerRotaryDriver)" + newline
    + b"        pagerRotaryDriver->init();" + newline
    + b"#endif" + newline
    + init_anchor
)
if b"pagerRotaryDriver->init();" not in data:
    if data.count(init_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui DeviceGUI init")
    data = data.replace(init_anchor, init_patch, 1)

task_anchor = b"#else" + newline + b"    displaydriver->task_handler();" + newline + b"#endif" + newline + b"};"
task_patch = (
    b"#else" + newline
    + b"    displaydriver->task_handler();" + newline
    + b"#if defined(INPUTDRIVER_ROTARY_TYPE)" + newline
    + b"    if (pagerRotaryDriver)" + newline
    + b"        pagerRotaryDriver->task_handler();" + newline
    + b"#endif" + newline
    + b"#endif" + newline
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
    b"    static const lv_point_t btn_points[1] = {" + newline
    + b"        {10, 235} // button0: (cog symbol) and button on blank screen" + newline
    + b"    };"
)
button_patch = (
    b"    static const lv_point_t btn_points[1] = {" + newline
    + b"        {10, static_cast<lv_coord_t>(lv_display_get_vertical_resolution(lv_display_get_default()) - 5)}" + newline
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
keyboard_anchor = b"// ---------- TLoraPagerKeyboardInputDriver Implementation ----------" + newline
keyboard_helpers = r'''static uint8_t pagerKeyboardReadRegister(uint8_t address, uint8_t reg)
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

static const char pagerKeyboardMap[31][3] = {
    {'q', 'Q', '1'}, {'w', 'W', '2'}, {'e', 'E', '3'}, {'r', 'R', '4'}, {'t', 'T', '5'},
    {'y', 'Y', '6'}, {'u', 'U', '7'}, {'i', 'I', '8'}, {'o', 'O', '9'}, {'p', 'P', '0'},
    {'a', 'A', '*'}, {'s', 'S', '/'}, {'d', 'D', '+'}, {'f', 'F', '-'}, {'g', 'G', '='},
    {'h', 'H', ':'}, {'j', 'J', 0x27}, {'k', 'K', 0x22}, {'l', 'L', '@'},
    {0, 0, 0}, {0, 0, 0}, {'z', 'Z', '_'}, {'x', 'X', '$'}, {'c', 'C', ';'},
    {'v', 'V', '?'}, {'b', 'B', '!'}, {'n', 'N', ','}, {'m', 'M', '.'},
    {0, 0, 0}, {0, 0, 0}, {' ', ' ', ' '}
};

'''.replace("\n", newline.decode()).encode()
if b"pagerKeyboardReadRegister" not in data:
    if data.count(keyboard_anchor) != 1:
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard section")
    data = data.replace(keyboard_anchor, keyboard_helpers + keyboard_anchor, 1)
else:
    keyboard_anchor_pos = data.index(keyboard_anchor)
    keyboard_helpers_pos = data.rfind(b"static uint8_t pagerKeyboardReadRegister", 0, keyboard_anchor_pos)
    if keyboard_helpers_pos < 0:
        raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard helpers")
    data = data[:keyboard_helpers_pos] + keyboard_helpers + data[keyboard_anchor_pos:]

pager_constructor_old = r'''TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
    registerI2CKeyboard(this, "TLora Pager Keyboard", address);
}'''.replace("\n", newline.decode()).encode()
pager_constructor_new = r'''TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
}'''.replace("\n", newline.decode()).encode()
if pager_constructor_old in data:
    data = data.replace(pager_constructor_old, pager_constructor_new, 1)
elif pager_constructor_new not in data:
    raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard constructor")

pager_init_old = r'''void TLoraPagerKeyboardInputDriver::init(void)
{
    // Additional initialization for TLora-Pager if needed
    TCA8418KeyboardInputDriver::init();
}'''.replace("\n", newline.decode()).encode()
pager_init_new = r'''void TLoraPagerKeyboardInputDriver::init(void)
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
}'''.replace("\n", newline.decode()).encode()
if pager_init_old in data:
    data = data.replace(pager_init_old, pager_init_new, 1)
elif pager_init_new not in data:
    raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard initialization")

pager_read_old = r'''void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}'''.replace("\n", newline.decode()).encode()
pager_read_new = r'''void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint8_t modifiers = 0;
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
        modifiers ^= 0x02;
    } else if (keyIndex == 28) {
        modifiers ^= 0x01;
    } else if (keyIndex == 29) {
        keyValue = (modifiers & 0x02) ? LV_KEY_ESC : LV_KEY_BACKSPACE;
        modifiers = 0;
    } else if (keyIndex == 19) {
        lv_group_t *group = lv_indev_get_group(indev);
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        const bool oneLineTextArea = focused && lv_obj_check_type(focused, &lv_textarea_class) &&
                                       lv_textarea_get_one_line(focused);
        keyValue = ((modifiers & 0x02) || oneLineTextArea) ? LV_KEY_NEXT : LV_KEY_ENTER;
        modifiers = 0;
    } else if (keyIndex < 31) {
        const uint8_t mapIndex = (modifiers & 0x02) ? 2 : ((modifiers & 0x01) ? 1 : 0);
        keyValue = static_cast<uint8_t>(pagerKeyboardMap[keyIndex][mapIndex]);
        if (keyValue != 0)
            modifiers = 0;
    }

    if (keyValue != 0) {
        data->key = keyValue;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}'''.replace("\n", newline.decode()).encode()
if pager_read_old in data:
    data = data.replace(pager_read_old, pager_read_new, 1)
elif pager_read_new not in data:
    raise RuntimeError("unexpected meshtastic-device-ui Pager keyboard reader")

keyboard_source.write_bytes(data)
