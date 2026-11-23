/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/storage/flash_map.h>
int test_flash_erase(const struct flash_area *, off_t, size_t);
int test_flash_write(const struct flash_area *, off_t, const void *, size_t);
#define flash_area_erase test_flash_erase
#define flash_area_write test_flash_write
