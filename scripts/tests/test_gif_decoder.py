import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DECODER = ROOT / "main/display/lvgl_display/gif"

# Three red pixels, followed by a frame with a blue center and transparent sides.
# Frame disposal changes from restore-background (2) to keep (1).
STATIC_GIF = bytes.fromhex(
    "47494638376103000100810000ff0000000000000000000000"
    "21f90408000000002c00000000030001000008050001080c08003b"
)
FINITE_GIF = bytes.fromhex(
    "47494638396103000100810000ff0000000000000000000000"
    "21f904080a0000002c00000000030001000008050001080c0800"
    "21f904050a0000002c0000000003000100810000000000ff000000000000"
    "0806000104001010003b"
)


def transparent_background(data):
    data = bytearray(data)
    data[11] = 3  # Global palette index 3 is the transparent background.
    gce = data.index(b"\x21\xf9\x04")
    data[gce + 3] |= 1
    data[gce + 6] = 3
    return bytes(data)


class GifDecoderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.work = Path(cls.directory.name)
        (cls.work / "lvgl.h").write_text(textwrap.dedent(r"""
            #pragma once
            #include <stdint.h>
            #include <stdlib.h>
            #include <limits.h>
            #define LV_USE_DRAW_SW_ASM 0
            #define LV_DRAW_SW_ASM_HELIUM 1
            #define LV_GIF_CACHE_DECODE_DATA 0
            #define lv_malloc malloc
            #define lv_realloc realloc
            #define lv_free free
            typedef struct { int unused; } lv_fs_file_t;
            typedef int lv_fs_res_t;
            #define LV_FS_MODE_RD 0
            #define LV_FS_RES_OK 0
            #define LV_FS_SEEK_CUR 1
            #define LV_FS_SEEK_SET 0
            static inline int lv_fs_open(lv_fs_file_t *f, const void *p, int m) { return 1; }
            static inline int lv_fs_read(lv_fs_file_t *f, void *p, size_t n, void *r) { return 1; }
            static inline int lv_fs_seek(lv_fs_file_t *f, size_t n, int m) { return 1; }
            static inline int lv_fs_tell(lv_fs_file_t *f, uint32_t *n) { return 1; }
            static inline int lv_fs_close(lv_fs_file_t *f) { return 1; }
        """), encoding="utf-8")
        (cls.work / "esp_log.h").write_text("#define ESP_LOGW(...) ((void)0)\n")
        driver = cls.work / "driver.c"
        driver.write_text(textwrap.dedent(r"""
            #include "gifdec.h"
            #include <assert.h>
            #include <stdio.h>
            #include <string.h>

            int main(int argc, char **argv) {
                assert(argc == 3);
                FILE *file = fopen(argv[1], "rb");
                assert(file);
                unsigned char data[256];
                size_t size = fread(data, 1, sizeof(data), file);
                fclose(file);
                assert(size > 0 && size < sizeof(data));
                gd_GIF *gif = gd_open_gif_data(data);
                assert(gif && gif->width == 3 && gif->height == 1);
                const unsigned char red[] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
                const unsigned char blue[] = {0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 0, 0};
                int looping = strcmp(argv[2], "loop") == 0;
                int frames = strcmp(argv[2], "static") == 0 ? 1 : 2;
                for (int cycle = 0; cycle < (looping ? 3 : 1); cycle++) {
                    for (int frame = 0; frame < frames; frame++) {
                        assert(gd_get_frame(gif) == 1);
                        gd_render_frame(gif, gif->canvas);
                        const unsigned char *expected = frame == 0 ? red : blue;
                        for (int pixel = 0; pixel < 3; pixel++) {
                            int offset = pixel * 4;
                            assert(gif->canvas[offset + 3] == expected[offset + 3]);
                            if (expected[offset + 3]) {
                                assert(memcmp(gif->canvas + offset, expected + offset, 3) == 0);
                            }
                        }
                    }
                }
                if (!looping) {
                    unsigned char last_frame[12];
                    memcpy(last_frame, gif->canvas, sizeof(last_frame));
                    assert(gd_get_frame(gif) == 0);
                    assert(memcmp(gif->canvas, last_frame, sizeof(last_frame)) == 0);
                }
                gd_close_gif(gif);
                return 0;
            }
        """), encoding="utf-8")
        cls.executable = cls.work / "gif_decoder_test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", f"-I{cls.work}", f"-I{DECODER}", str(driver),
            str(DECODER / "gifdec.c"), "-o", str(cls.executable),
        ]
        subprocess.run(command, cwd=cls.work, check=True)

    def check_gif(self, data, mode):
        path = self.work / f"{mode}.gif"
        path.write_bytes(data)
        subprocess.run([self.executable, path, mode], cwd=self.work, check=True)

    def test_single_frame_remains_visible_at_end(self):
        self.check_gif(transparent_background(STATIC_GIF), "static")

    def test_finite_animation_preserves_last_frame_after_comment(self):
        data = bytearray(transparent_background(FINITE_GIF))
        gce = data.rindex(b"\x21\xf9\x04")
        data[gce + 3] = 9  # Restore to transparent background after the last frame.
        self.check_gif(bytes(data[:-1]) + b"\x21\xfe\x04test\x00;", "finite")

    def test_looping_animation_uses_previous_frame_disposal(self):
        data = transparent_background(FINITE_GIF)
        loop = b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
        self.check_gif(data[:25] + loop + data[25:], "loop")


if __name__ == "__main__":
    unittest.main()
