#!/usr/bin/env python3
import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    print(f"FAIL import: pyserial missing: {exc}", file=sys.stderr)
    sys.exit(2)


ROM_GREETING = bytes.fromhex("c0 c2 00 00 00 00 00 00 00 00 00 00 00 00 c0")
KSD_MAGIC = b"KSD1\n"


def log(msg):
    print(msg, flush=True)


def set_lines(ser, dtr, rts, label):
    ser.setDTR(dtr)
    ser.setRTS(rts)
    log(f"lines: {label}: DTR={int(dtr)} RTS={int(rts)}")


def reset_boot_dan(ser):
    log("stage: reset_boot_dan")
    set_lines(ser, False, False, "idle")
    time.sleep(0.1)
    set_lines(ser, False, True, "reset-low")
    time.sleep(0.1)
    set_lines(ser, False, False, "boot")
    time.sleep(0.1)


def reset_isp_dan(ser):
    log("stage: reset_isp_dan")
    set_lines(ser, False, False, "idle")
    time.sleep(0.1)
    set_lines(ser, False, True, "reset-low")
    time.sleep(0.1)
    set_lines(ser, True, False, "isp")
    time.sleep(0.1)


def read_window(ser, seconds, stop_on=None):
    end = time.monotonic() + seconds
    data = bytearray()
    while time.monotonic() < end:
        chunk = ser.read(256)
        if chunk:
            data.extend(chunk)
            if stop_on and stop_on in data:
                break
    return bytes(data)


def printable(data):
    text = data.decode("utf-8", errors="replace")
    return text.replace("\r", "\\r").replace("\n", "\\n\n")


def port_exists(name):
    ports = list(list_ports.comports())
    for p in ports:
        if p.device.upper() == name.upper():
            log(f"port: {p.device} {p.description} hwid={p.hwid}")
            return True
    log("ports: " + ", ".join(p.device for p in ports))
    return False


def open_port(name, baud):
    return serial.Serial(
        name,
        baudrate=baud,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=0.05,
        write_timeout=1.0,
    )


def app_ping(args):
    log(f"stage: app_ping port={args.port} baud={args.app_baud}")
    with open_port(args.port, args.app_baud) as ser:
        set_lines(ser, False, False, "app-idle")
        ser.reset_input_buffer()
        ser.write(KSD_MAGIC)
        ser.flush()
        data = read_window(ser, args.read_window, stop_on=b"KSD:HELLO")
    if data:
        log(f"app_rx_bytes: {len(data)}")
        log("app_rx_text:")
        log(printable(data))
        if b"KSD:HELLO" in data:
            return "APP_KSD_OK"
        return "APP_UART_ACTIVE_NO_KSD"
    return "APP_SILENT"


def boot_log_ping(args):
    log(f"stage: boot_log_ping port={args.port} baud={args.app_baud}")
    with open_port(args.port, args.app_baud) as ser:
        ser.reset_input_buffer()
        reset_boot_dan(ser)
        data = read_window(ser, args.boot_window)
    if data:
        log(f"boot_rx_bytes: {len(data)}")
        log("boot_rx_text:")
        log(printable(data))
        if b"KSD:HELLO" in data or b"[stack]" in data:
            return "BOOT_APP_LOG_OK"
        return "BOOT_UART_ACTIVE_UNKNOWN"
    return "BOOT_SILENT"


def ksd_after_boot_ping(args):
    log(f"stage: ksd_after_boot_ping port={args.port} baud={args.app_baud}")
    with open_port(args.port, args.app_baud) as ser:
        ser.reset_input_buffer()
        reset_boot_dan(ser)
        log_data = read_window(ser, args.boot_window, stop_on=b"PC UART KSD listener")
        if log_data:
            log(f"ksd_boot_rx_bytes: {len(log_data)}")
        ser.write(KSD_MAGIC)
        ser.flush()
        data = read_window(ser, args.read_window, stop_on=b"KSD:HELLO")
    if data:
        log(f"ksd_rx_bytes: {len(data)}")
        log("ksd_rx_text:")
        log(printable(data))
        if b"KSD:HELLO" in data:
            return "KSD_OK_AFTER_BOOT"
        return "KSD_UART_ACTIVE_NO_HELLO"
    return "KSD_NO_REPLY_AFTER_BOOT"


def rom_ping(args):
    log(f"stage: rom_ping port={args.port} baud=115200")
    with open_port(args.port, 115200) as ser:
        ser.reset_input_buffer()
        reset_isp_dan(ser)
        ser.write(ROM_GREETING)
        ser.flush()
        data = read_window(ser, args.read_window, stop_on=b"\xc0")
        if not args.leave_isp:
            reset_boot_dan(ser)
    if data:
        log(f"rom_rx_bytes: {len(data)}")
        log("rom_rx_hex: " + data.hex(" "))
        if data.startswith(b"\xc0") or b"\xc0" in data:
            return "ROM_ISP_RESPONDED"
        return "ROM_UART_ACTIVE_UNKNOWN"
    return "ROM_SILENT"


def rom_after_boot_ping(args):
    log(f"stage: rom_after_boot_ping port={args.port} baud=115200")
    with open_port(args.port, 115200) as ser:
        ser.reset_input_buffer()
        reset_boot_dan(ser)
        ser.write(ROM_GREETING)
        ser.flush()
        data = read_window(ser, args.read_window, stop_on=b"\xc0")
    if data:
        log(f"boot_rom_rx_bytes: {len(data)}")
        log("boot_rom_rx_hex: " + data.hex(" "))
        if data.startswith(b"\xc0") or b"\xc0" in data:
            return "BOOT_RESET_ENTERS_ROM"
        return "BOOT_RESET_UART_ACTIVE_UNKNOWN"
    return "BOOT_RESET_NOT_ROM"


def main():
    ap = argparse.ArgumentParser(description="Strict one-pass K210 health probe for dan DTR/RTS wiring.")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--app-baud", type=int, default=921600)
    ap.add_argument("--read-window", type=float, default=1.0, help="single finite read window per stage; no retries")
    ap.add_argument("--boot-window", type=float, default=2.0, help="single finite boot-log window after reset; no retries")
    ap.add_argument("--skip-reset", action="store_true")
    ap.add_argument("--leave-isp", action="store_true", help="leave K210 in ISP mode after ROM ping")
    args = ap.parse_args()

    log("k210-health: strict single-pass probe, no automatic retries")
    if not port_exists(args.port):
        log("verdict: NO_PORT")
        return 2

    try:
        app = app_ping(args)
    except serial.SerialException as exc:
        log(f"verdict: PORT_OPEN_FAIL {exc}")
        return 2

    log(f"result: {app}")
    if app == "APP_KSD_OK":
        log("verdict: APP_ALIVE")
        return 0

    boot = "BOOT_SKIPPED"
    if not args.skip_reset:
        boot = boot_log_ping(args)
        log(f"result: {boot}")
        if boot == "BOOT_APP_LOG_OK":
            ksd = ksd_after_boot_ping(args)
            log(f"result: {ksd}")
            if ksd == "KSD_OK_AFTER_BOOT":
                log("verdict: APP_KSD_ALIVE")
                return 0
            log("verdict: APP_BOOTED_KSD_NOT_READY")
            return 1

    boot_rom = rom_after_boot_ping(args)
    log(f"result: {boot_rom}")
    if boot_rom == "BOOT_RESET_ENTERS_ROM":
        log("verdict: BOOT_LINE_HELD_ISP_OR_DTR_RTS_MAPPING_WRONG")
        return 1

    rom = rom_ping(args)
    log(f"result: {rom}")

    if rom == "ROM_ISP_RESPONDED":
        if app == "APP_SILENT" and boot == "BOOT_SILENT":
            log("verdict: APP_DEAD_OR_BOOT_HELD_ROM_ALIVE")
            return 1
        log("verdict: APP_BAD_PROTOCOL_ROM_ALIVE")
        return 1

    log("verdict: ROM_AND_APP_SILENT_CHECK_POWER_WIRING_OR_PORT")
    return 1


if __name__ == "__main__":
    sys.exit(main())
