Import("env")

from pathlib import Path

sensor_source = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "SensorLib"
    / "src"
    / "bosch"
    / "bhi260x"
    / "bhy2.c"
)
if not sensor_source.is_file():
    raise RuntimeError(f"SensorLib BHI260AP source not found: {sensor_source}")

sensor_data = sensor_source.read_bytes()
newline = b"\r\n" if b"\r\n" in sensor_data else b"\n"

copy_old = b"static int8_t parse_fifo_support(struct bhy2_fifo_buffer *fifo_buf)\n{\n    uint8_t i;"
copy_new = copy_old.replace(b"uint8_t i;", b"uint32_t i;")
event_old = b"""            default:
                rslt = get_callback_info(tmp_sensor_id, &info, dev);
                rslt = check_return_value(rslt);"""
event_new = event_old + b"""
                if (dev->event_size[tmp_sensor_id] == 0)
                    return BHY2_E_INVALID_PARAM;"""

for old, new in ((copy_old, copy_new), (event_old, event_new)):
    old = old.replace(b"\n", newline)
    new = new.replace(b"\n", newline)
    if sensor_data.count(new) == 1:
        continue
    if sensor_data.count(old) != 1:
        raise RuntimeError("unexpected SensorLib BHI260AP FIFO parser")
    sensor_data = sensor_data.replace(old, new, 1)

sensor_source.write_bytes(sensor_data)
