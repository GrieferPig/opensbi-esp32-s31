#
# SPDX-License-Identifier: BSD-2-Clause
#

carray-platform_override_modules-$(CONFIG_PLATFORM_ESPRESSIF_ESP32S31) += esp32s31
platform-objs-$(CONFIG_PLATFORM_ESPRESSIF_ESP32S31) += espressif/esp32s31.o
platform-objs-$(CONFIG_PLATFORM_ESPRESSIF_ESP32S31) += espressif/esp32s31_coproc.o
