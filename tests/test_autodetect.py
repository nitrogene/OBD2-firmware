"""Host unit tests for obd2_autodetect.c waterfall, NVS caching, and LED state transitions."""

from __future__ import annotations

import ctypes
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
AUTODETECT_C = REPO_ROOT / "components" / "obd2_stack" / "obd2_autodetect.c"
OBD2_INC = REPO_ROOT / "components" / "obd2_stack" / "include"
BSP_INC = REPO_ROOT / "components" / "bsp" / "include"
TWAI_INC = REPO_ROOT / "components" / "twai_driver" / "include"
KLINE_INC = REPO_ROOT / "components" / "kline_driver" / "include"

STUB_CODE = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp.h"
#include "esp_err.h"
#include "kline_driver.h"
#include "twai_driver.h"

/* Controllable mock state exposed to Python */
int g_nvs_cached_proto = 0;
int g_nvs_saved_proto = -1;
int g_passive_probe_ok = 0;
int g_passive_bitrate = 0;
int g_can_500k_11b_ok = 0;
int g_can_500k_29b_ok = 0;
int g_can_250k_11b_ok = 0;
int g_can_250k_29b_ok = 0;
int g_kwp_fast_ok = 0;
int g_slow_init_ok = 0;
int g_slow_init_proto = 0;

int g_led_history[16];
int g_led_history_count = 0;

static twai_drv_bitrate_t s_cur_bitrate = TWAI_DRV_BITRATE_500K;
static bool s_last_tx_extd = false;

void mock_reset(void) {
    g_nvs_cached_proto = 0;
    g_nvs_saved_proto = -1;
    g_passive_probe_ok = 0;
    g_passive_bitrate = TWAI_DRV_BITRATE_500K;
    g_can_500k_11b_ok = 0;
    g_can_500k_29b_ok = 0;
    g_can_250k_11b_ok = 0;
    g_can_250k_29b_ok = 0;
    g_kwp_fast_ok = 0;
    g_slow_init_ok = 0;
    g_slow_init_proto = KLINE_PROTO_UNKNOWN;
    g_led_history_count = 0;
}

void bsp_led_set_pattern(bsp_led_pattern_t pattern) {
    if (g_led_history_count < 16) {
        g_led_history[g_led_history_count++] = (int)pattern;
    }
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, uint32_t *out_handle) {
    (void)name;
    (void)mode;
    *out_handle = 1U;
    return ESP_OK;
}
esp_err_t nvs_get_u8(uint32_t handle, const char *key, uint8_t *out_val) {
    (void)handle;
    (void)key;
    if (g_nvs_cached_proto <= 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_val = (uint8_t)g_nvs_cached_proto;
    return ESP_OK;
}
esp_err_t nvs_set_u8(uint32_t handle, const char *key, uint8_t val) {
    (void)handle;
    (void)key;
    g_nvs_saved_proto = (int)val;
    return ESP_OK;
}
esp_err_t nvs_commit(uint32_t handle) {
    (void)handle;
    return ESP_OK;
}
void nvs_close(uint32_t handle) { (void)handle; }

esp_err_t twai_drv_probe_bitrate(uint32_t listen_ms, twai_drv_bitrate_t *out_bitrate) {
    (void)listen_ms;
    if (!g_passive_probe_ok) {
        return ESP_FAIL;
    }
    *out_bitrate = (twai_drv_bitrate_t)g_passive_bitrate;
    return ESP_OK;
}

esp_err_t twai_drv_init(twai_drv_bitrate_t bitrate, twai_drv_mode_t mode,
                        const twai_drv_filter_t *filter) {
    (void)mode;
    (void)filter;
    s_cur_bitrate = bitrate;
    return ESP_OK;
}

esp_err_t twai_drv_deinit(void) { return ESP_OK; }

esp_err_t twai_drv_send(const twai_drv_frame_t *frame, uint32_t timeout_ms) {
    (void)timeout_ms;
    s_last_tx_extd = frame->extd;
    return ESP_OK;
}

esp_err_t twai_drv_receive(twai_drv_frame_t *out_frame, uint32_t timeout_ms) {
    (void)out_frame;
    (void)timeout_ms;
    if (s_cur_bitrate == TWAI_DRV_BITRATE_500K && !s_last_tx_extd && g_can_500k_11b_ok) {
        return ESP_OK;
    }
    if (s_cur_bitrate == TWAI_DRV_BITRATE_500K && s_last_tx_extd && g_can_500k_29b_ok) {
        return ESP_OK;
    }
    if (s_cur_bitrate == TWAI_DRV_BITRATE_250K && !s_last_tx_extd && g_can_250k_11b_ok) {
        return ESP_OK;
    }
    if (s_cur_bitrate == TWAI_DRV_BITRATE_250K && s_last_tx_extd && g_can_250k_29b_ok) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t kline_drv_fast_init(uint8_t target_addr, uint8_t source_addr) {
    (void)target_addr;
    (void)source_addr;
    return g_kwp_fast_ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t kline_drv_slow_init(uint8_t target_addr, kline_proto_type_t *out_proto,
                              uint8_t *out_kb1, uint8_t *out_kb2) {
    (void)target_addr;
    (void)out_kb1;
    (void)out_kb2;
    if (!g_slow_init_ok) {
        return ESP_ERR_TIMEOUT;
    }
    if (out_proto != NULL) {
        *out_proto = (kline_proto_type_t)g_slow_init_proto;
    }
    return ESP_OK;
}
"""


@pytest.fixture(scope="session")
def autodetect_lib(tmp_path_factory: pytest.TempPathFactory) -> ctypes.CDLL:
    """Compile obd2_autodetect.c with host mocks for ESP-IDF headers and bus drivers."""
    build_dir = tmp_path_factory.mktemp("autodetect_build")
    inc_dir = build_dir / "mock_inc"
    inc_dir.mkdir()
    drv_dir = inc_dir / "driver"
    drv_dir.mkdir()
    hal_dir = inc_dir / "hal"
    hal_dir.mkdir()

    (drv_dir / "gpio.h").write_text(
        """#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H
typedef int gpio_num_t;
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#endif
""",
        encoding="utf-8",
    )
    (hal_dir / "adc_types.h").write_text(
        """#ifndef HAL_ADC_TYPES_H
#define HAL_ADC_TYPES_H
typedef int adc_unit_t;
typedef int adc_channel_t;
#define ADC_UNIT_1 0
#define ADC_CHANNEL_0 0
#endif
""",
        encoding="utf-8",
    )

    (inc_dir / "esp_err.h").write_text(
        """#ifndef ESP_ERR_H
#define ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1103
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#endif
""",
        encoding="utf-8",
    )
    (inc_dir / "esp_log.h").write_text(
        """#ifndef ESP_LOG_H
#define ESP_LOG_H
#define ESP_LOGI(tag, fmt, ...) (void)(tag)
#define ESP_LOGW(tag, fmt, ...) (void)(tag)
#define ESP_LOGE(tag, fmt, ...) (void)(tag)
#endif
""",
        encoding="utf-8",
    )
    (inc_dir / "esp_check.h").write_text(
        """#ifndef ESP_CHECK_H
#define ESP_CHECK_H
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg, ...) do { esp_err_t _e = (x); if (_e != ESP_OK) return _e; } while(0)
#endif
""",
        encoding="utf-8",
    )
    (inc_dir / "nvs.h").write_text(
        """#ifndef NVS_H
#define NVS_H
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out_handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_val);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t val);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
""",
        encoding="utf-8",
    )
    (inc_dir / "nvs_flash.h").write_text(
        """#ifndef NVS_FLASH_H
#define NVS_FLASH_H
#include "esp_err.h"
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
#endif
""",
        encoding="utf-8",
    )

    stub_c = build_dir / "stubs.c"
    stub_c.write_text(STUB_CODE, encoding="utf-8")

    ext = ".dll" if platform.system() == "Windows" else ".so"
    out_lib = build_dir / f"libautodetect{ext}"
    compiler = [shutil.which("gcc")] if shutil.which("gcc") else [sys.executable, "-m", "ziglang", "cc"]
    cmd = [
        *compiler,
        "-shared",
        "-fPIC",
        "-O2",
        "-Wall",
        "-Wextra",
        f"-I{inc_dir}",
        f"-I{OBD2_INC}",
        f"-I{BSP_INC}",
        f"-I{TWAI_INC}",
        f"-I{KLINE_INC}",
        str(AUTODETECT_C),
        str(stub_c),
        "-o",
        str(out_lib),
    ]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(out_lib))
    lib.mock_reset.argtypes = []
    lib.mock_reset.restype = None
    lib.obd2_autodetect_run.argtypes = [ctypes.POINTER(ctypes.c_int)]
    lib.obd2_autodetect_run.restype = ctypes.c_int
    lib.obd2_proto_name.argtypes = [ctypes.c_int]
    lib.obd2_proto_name.restype = ctypes.c_char_p
    return lib


def _led_history(lib: ctypes.CDLL) -> list[int]:
    count = ctypes.c_int.in_dll(lib, "g_led_history_count").value
    arr = (ctypes.c_int * 16).in_dll(lib, "g_led_history")
    return [arr[i] for i in range(count)]


def test_autodetect_waterfall_and_led_transitions(autodetect_lib: ctypes.CDLL) -> None:
    proto = ctypes.c_int(0)

    # 1. Total failure across all CAN & K-Line protocols -> ESP_ERR_NOT_FOUND + LED BREATHING (2 -> 1)
    autodetect_lib.mock_reset()
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0x105  # ESP_ERR_NOT_FOUND
    # BSP_LED_BLINK_SLOW (2) at start -> BSP_LED_BREATHING (1) on failure
    assert _led_history(autodetect_lib) == [2, 1]

    # 2. Fallback to CAN 250 kbps 29-bit (proto 9) -> saves to NVS + LED SOLID (3)
    autodetect_lib.mock_reset()
    ctypes.c_int.in_dll(autodetect_lib, "g_can_250k_29b_ok").value = 1
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0
    assert proto.value == 9  # OBD2_PROTO_CAN_29B_250K
    assert ctypes.c_int.in_dll(autodetect_lib, "g_nvs_saved_proto").value == 9
    assert _led_history(autodetect_lib) == [2, 3]

    # 3. Fallback to KWP2000 Fast Init (proto 5) when CAN fails
    autodetect_lib.mock_reset()
    ctypes.c_int.in_dll(autodetect_lib, "g_kwp_fast_ok").value = 1
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0
    assert proto.value == 5  # OBD2_PROTO_KWP_FAST

    # 4. Fallback to 5-baud Slow Init: keybytes select ISO 9141-2 (proto 3) vs KWP 5-baud (proto 4)
    autodetect_lib.mock_reset()
    ctypes.c_int.in_dll(autodetect_lib, "g_slow_init_ok").value = 1
    ctypes.c_int.in_dll(autodetect_lib, "g_slow_init_proto").value = 1  # KLINE_PROTO_ISO9141_2
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0
    assert proto.value == 3  # OBD2_PROTO_ISO9141_2

    autodetect_lib.mock_reset()
    ctypes.c_int.in_dll(autodetect_lib, "g_slow_init_ok").value = 1
    ctypes.c_int.in_dll(autodetect_lib, "g_slow_init_proto").value = 2  # KLINE_PROTO_KWP2000
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0
    assert proto.value == 4  # OBD2_PROTO_KWP_5BAUD

    # 5. Cached NVS protocol succeeds immediately without full scan
    autodetect_lib.mock_reset()
    ctypes.c_int.in_dll(autodetect_lib, "g_nvs_cached_proto").value = 8  # OBD2_PROTO_CAN_11B_250K
    ctypes.c_int.in_dll(autodetect_lib, "g_can_250k_11b_ok").value = 1
    assert autodetect_lib.obd2_autodetect_run(ctypes.byref(proto)) == 0
    assert proto.value == 8
    assert autodetect_lib.obd2_proto_name(8) == b"ISO 15765-4 (CAN 11/250)"
