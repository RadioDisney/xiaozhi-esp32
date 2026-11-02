"""Keep the StopWatch battery fixed while its enlarged bolt is toggled."""

import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"


def definition(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class StopwatchChargingIconTest(unittest.TestCase):
    def test_charge_transitions_keep_battery_fixed_and_enlarged_bolt_centered(self):
        source = (BOARD / "m5stack_stopwatch.cc").read_text()
        methods = "\n".join(definition(source, signature) for signature in (
            "void UpdateChargingIconColor()",
            "void InitializeChargingIcon()",
            "void UpdateStatusBar(bool update_all) override",
        ))
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "lvgl.h").write_text(textwrap.dedent(r'''
                #pragma once
                #include <cstdint>
                #include <cstddef>
                struct ImageHeader {uint32_t magic=0,cf=0,w=0,h=0,stride=0;};
                struct lv_image_dsc_t {
                    ImageHeader header;
                    uint32_t data_size=0;
                    const uint8_t* data=nullptr;
                };
                constexpr int LV_IMAGE_HEADER_MAGIC=25, LV_COLOR_FORMAT_A8=14;
            '''))
            platform = r'''
                #include "charging_icon.h"
                #include <cassert>
                #include <cstring>
                #include <string>
                #define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT "combined"
                #define MATERIAL_SYMBOLS_BATTERY_ANDROID_0 "empty"
                #define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL "full"
                constexpr int LV_PART_MAIN=0, LV_OPA_COVER=255;
                constexpr int LV_OBJ_FLAG_HIDDEN=1, LV_OBJ_FLAG_IGNORE_LAYOUT=2;
                struct lv_obj_t {
                    lv_obj_t* parent=nullptr;
                    const lv_image_dsc_t* image=nullptr;
                    std::string text="full";
                    int x=100,y=55,width=30,height=30,font_size=30,flags=0,recolor=0,opacity=0;
                    int pivot_x=0,pivot_y=0,scale=256;
                    uint32_t text_color=0x123A52;
                };
                lv_obj_t overlay;
                int creations=0;
                lv_obj_t* lv_image_create(lv_obj_t* parent) {
                    ++creations;overlay.parent=parent;return &overlay;
                }
                void lv_image_set_src(lv_obj_t* obj,const lv_image_dsc_t* image) {
                    obj->image=image;obj->width=image->header.w;obj->height=image->header.h;
                }
                void lv_image_set_pivot(lv_obj_t* obj,int x,int y){obj->pivot_x=x;obj->pivot_y=y;}
                void lv_image_set_scale(lv_obj_t* obj,int scale){obj->scale=scale;}
                void lv_obj_set_pos(lv_obj_t* obj,int x,int y){obj->x=x;obj->y=y;}
                void lv_obj_add_flag(lv_obj_t* obj,int flag){obj->flags|=flag;}
                void lv_obj_remove_flag(lv_obj_t* obj,int flag){obj->flags&=~flag;}
                uint32_t lv_obj_get_style_text_color(lv_obj_t* obj,int){return obj->text_color;}
                void lv_obj_set_style_image_recolor(lv_obj_t* obj,uint32_t color,int){obj->recolor=color;}
                void lv_obj_set_style_image_recolor_opa(lv_obj_t* obj,int opacity,int){obj->opacity=opacity;}
                const char* lv_label_get_text(lv_obj_t* obj){return obj->text.c_str();}
                void lv_label_set_text(lv_obj_t* obj,const char* value){obj->text=value;}
                struct DisplayLockGuard {explicit DisplayLockGuard(void*){}};
                struct SpiLcdDisplay {
                    lv_obj_t battery;
                    lv_obj_t* battery_label_=&battery;
                    const char* battery_icon_=nullptr;
                    bool charging=false;
                    virtual void UpdateStatusBar(bool) {
                        const char* desired=charging?MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT:
                                                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL;
                        if(battery_label_ && desired!=battery_icon_) {
                            battery_icon_=desired;lv_label_set_text(battery_label_,desired);
                        }
                    }
                };
            '''
            driver = r'''
                int main() {
                    TestDisplay display;
                    display.InitializeChargingIcon();display.InitializeChargingIcon();
                    assert(creations==1 && overlay.parent==display.battery_label_);
                    assert(overlay.width==11 && overlay.height==14 && overlay.opacity==255);
                    assert(overlay.x==5 && overlay.y==4 && overlay.scale==384);
                    assert(overlay.pivot_x==0 && overlay.pivot_y==0);
                    const double displayed_width=overlay.width*overlay.scale/256.0;
                    const double displayed_height=overlay.height*overlay.scale/256.0;
                    assert(displayed_width==16.5 && displayed_height==21);
                    assert(overlay.y<7 && overlay.y+displayed_height>23);
                    assert(overlay.x+displayed_width/2==13.25);
                    assert(overlay.y+displayed_height/2==14.5);
                    assert(overlay.image->header.cf==LV_COLOR_FORMAT_A8);
                    assert(overlay.image->data_size==154);
                    int covered=0;
                    for(size_t i=0;i<overlay.image->data_size;i++) covered+=overlay.image->data[i]!=0;
                    assert(covered==59);
                    assert(overlay.flags&LV_OBJ_FLAG_IGNORE_LAYOUT);
                    for(bool charging:{false,true,true,false,true,false}) {
                        display.charging=charging;display.UpdateStatusBar(false);
                        const auto& battery=display.battery;
                        assert(battery.x==100 && battery.y==55 && battery.width==30 && battery.height==30);
                        assert(battery.font_size==30);
                        assert(battery.text==(charging?"empty":"full"));
                        assert(bool(overlay.flags&LV_OBJ_FLAG_HIDDEN)==!charging);
                        assert(overlay.x==5 && overlay.y==4 && overlay.width==11 && overlay.height==14);
                        assert(overlay.scale==384 && overlay.pivot_x==0 && overlay.pivot_y==0);
                    }
                    display.battery.text_color=0xFFFFFF;
                    display.UpdateChargingIconColor();assert(overlay.recolor==0xFFFFFF);
                    TestDisplay unavailable;unavailable.battery_label_=nullptr;
                    unavailable.InitializeChargingIcon();unavailable.UpdateStatusBar(false);
                    assert(unavailable.charging_bolt_==nullptr && creations==1);
                }
            '''
            path = work / "charging.cc"
            path.write_text(platform + "\nstruct TestDisplay:SpiLcdDisplay {\n"
                            "lv_obj_t* charging_bolt_=nullptr;\n" + methods + "\n};\n" + driver)
            executable = work / "charging"
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++23", "-I" + str(work), "-I" + str(BOARD), str(path),
                "-o", str(executable),
            ], check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
