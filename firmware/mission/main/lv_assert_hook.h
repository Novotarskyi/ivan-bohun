/* lv_assert_hook.h - what LVGL does when one of its assertions fails here.
 *
 * LVGL's default handler is `while(1);` - a silent halt. On mission v11 that
 * froze the kobzar for good: an lv_malloc out of the builtin 64 KB pool failed
 * on the second refresh, the render task spun inside the UI lock, touch died
 * with it (the OTA window is opened through that lock), the radio kept beating,
 * nothing rebooted and the trial even marked the image valid. abort() instead:
 * a panic the blackbox names, a coredump, and a reboot the trial can judge.
 * Pulled in through CONFIG_LV_ASSERT_HANDLER_INCLUDE (sdkconfig.defaults) and
 * the -I in the project CMakeLists, so LVGL's own sources see it. */
#pragma once
#include <stdlib.h>
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();
