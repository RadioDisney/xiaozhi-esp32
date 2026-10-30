import importlib.util
import json
import os
import shlex
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLAYER = ROOT / "main/display/lvgl_display"


class MjpegPlayerTest(unittest.TestCase):
    def test_bounds_switching_pause_static_frames_and_buffer_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "host.h").write_text(HOST)
            for name in ("freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h",
                         "lvgl.h", "display.h", "esp_heap_caps.h", "esp_log.h",
                         "esp_timer.h", "esp_jpeg_dec.h",
                         "src/misc/cache/instance/lv_image_cache.h"):
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "host.h"\n')
            (work / "driver.cc").write_text(DRIVER)
            binary = work / "mjpeg_test"
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++23", "-pthread", "-I" + str(work), "-I" + str(PLAYER),
                str(work / "driver.cc"), str(PLAYER / "mjpeg_clip.cc"),
                str(PLAYER / "mjpeg_player.cc"), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([binary], check=True, timeout=15)


HOST = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
inline std::recursive_mutex gui;
inline thread_local int gui_depth=0;
struct Display { int width(){return 466;} int height(){return 466;} };
struct DisplayLockGuard {
    DisplayLockGuard(Display*) {gui.lock();++gui_depth;}
    ~DisplayLockGuard(){--gui_depth;gui.unlock();}
};
struct Header {int magic=0,cf=0,w=0,h=0,stride=0;};
struct lv_image_dsc_t {Header header;size_t data_size=0;const uint8_t* data=nullptr;};
struct lv_obj_t {const lv_image_dsc_t* src=nullptr;bool hidden=false;int x=0,y=0;};
inline std::vector<int> published;
constexpr int LV_IMAGE_HEADER_MAGIC=25,LV_COLOR_FORMAT_RGB565=1,LV_ALIGN_TOP_LEFT=0,LV_OBJ_FLAG_HIDDEN=1;
inline void lv_image_cache_drop(const void*){assert(gui_depth>0);}
inline void lv_image_set_src(lv_obj_t* obj,const lv_image_dsc_t* image){
    assert(gui_depth>0);obj->src=image;if(image)published.push_back(image->data[0]);
}
inline void lv_obj_align(lv_obj_t* obj,int,int x,int y){assert(gui_depth>0);obj->x=x;obj->y=y;}
inline void lv_obj_add_flag(lv_obj_t* obj,int){assert(gui_depth>0);obj->hidden=true;}
inline void lv_obj_remove_flag(lv_obj_t* obj,int){assert(gui_depth>0);obj->hidden=false;}
using TickType_t=uint32_t;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1,pdPASS=1;
#define pdMS_TO_TICKS(x) (x)
struct Task {std::thread thread;std::mutex mutex;std::condition_variable cv;unsigned notifications=0;};
using TaskHandle_t=Task*;
inline thread_local Task* current_task=nullptr;
inline std::vector<std::unique_ptr<Task>> tasks;
inline int xTaskCreate(void (*fn)(void*),const char*,int,void* arg,int,TaskHandle_t* out){
    auto task=std::make_unique<Task>();*out=task.get();tasks.push_back(std::move(task));
    (*out)->thread=std::thread([=]{current_task=*out;fn(arg);});return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task){std::lock_guard lock(task->mutex);++task->notifications;task->cv.notify_all();}
inline unsigned ulTaskNotifyTake(int,uint32_t timeout){
    auto* t=current_task;assert(t);std::unique_lock lock(t->mutex);
    if(timeout==portMAX_DELAY)t->cv.wait(lock,[=]{return t->notifications>0;});
    else t->cv.wait_for(lock,std::chrono::milliseconds(timeout),[=]{return t->notifications>0;});
    unsigned value=t->notifications;t->notifications=0;return value;
}
inline void vTaskDelete(void*){}
struct Sem {std::mutex mutex;std::condition_variable cv;bool ready=false;};
using SemaphoreHandle_t=Sem*;
inline std::vector<std::unique_ptr<Sem>> semaphores;
inline SemaphoreHandle_t xSemaphoreCreateBinary(){semaphores.push_back(std::make_unique<Sem>());return semaphores.back().get();}
inline void xSemaphoreGive(Sem* s){std::lock_guard lock(s->mutex);s->ready=true;s->cv.notify_all();}
inline void xSemaphoreTake(Sem* s,uint32_t){std::unique_lock lock(s->mutex);s->cv.wait(lock,[=]{return s->ready;});}
inline void vSemaphoreDelete(Sem*){}
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
inline std::atomic<int> allocations=0,frees=0,handles=0;
inline void* heap_caps_malloc(size_t n,int){++allocations;return malloc(n);}
inline void* heap_caps_aligned_alloc(size_t,size_t n,int){++allocations;return malloc(n);}
inline void heap_caps_free(void* p){if(p){++frees;free(p);}}
inline int64_t esp_timer_get_time(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
#define ESP_LOGE(...) ((void)0)
constexpr int JPEG_ERR_OK=0,JPEG_PIXEL_FORMAT_RGB565_LE=1;
struct jpeg_dec_config_t {int output_type=0;};
#define DEFAULT_JPEG_DEC_CONFIG() {}
struct jpeg_dec_io_t {uint8_t* inbuf=nullptr;int inbuf_len=0;uint8_t* outbuf=nullptr;};
struct jpeg_dec_header_info_t {uint16_t width,height;};
inline std::mutex decode_mutex;
inline std::condition_variable decode_cv;
inline bool block_old=true,entered=false;
inline std::atomic<int> failures=0;
inline int jpeg_dec_open(jpeg_dec_config_t*,void** p){*p=new int(1);++handles;return 0;}
inline void jpeg_dec_close(void* p){delete static_cast<int*>(p);--handles;}
inline int jpeg_dec_parse_header(void*,jpeg_dec_io_t*,jpeg_dec_header_info_t* p){p->width=16;p->height=16;return 0;}
inline int jpeg_dec_get_outbuf_len(void*,int* p){*p=512;return 0;}
inline int jpeg_dec_process(void*,jpeg_dec_io_t* io){
    assert(gui_depth==0);const auto color=io->inbuf[2];
    {std::unique_lock lock(decode_mutex);if(color==0x11&&block_old){entered=true;decode_cv.notify_all();decode_cv.wait(lock,[]{return !block_old;});}}
    assert(io->inbuf[2]==color); // Replacing an emotion must not overwrite in-flight input.
    if(color==0x66){++failures;return -1;}
    memset(io->outbuf,color,512);return 0;
}
'''

DRIVER = r'''
#include "mjpeg_player.h"
#include "host.h"
void Put16(std::vector<uint8_t>& data,size_t offset,uint16_t n){data[offset]=n;data[offset+1]=n>>8;}
void Put32(std::vector<uint8_t>& data,size_t offset,uint32_t n){for(int i=0;i<4;i++)data[offset+i]=n>>(8*i);}
std::vector<uint8_t> Index(uint32_t count,uint32_t loops){
    std::vector<uint8_t> data(24+count*12);memcpy(data.data(),"MJI1",4);
    Put16(data,4,16);Put16(data,6,16);Put16(data,8,16);Put16(data,10,80);
    Put32(data,12,count);Put32(data,16,count*6);Put32(data,20,loops);
    for(uint32_t i=0;i<count;i++){Put32(data,24+i*12,i*6);Put32(data,28+i*12,6);Put32(data,32+i*12,20);}
    return data;
}
std::vector<uint8_t> Jpeg(int color){return {255,216,(uint8_t)color,0,255,217};}
template<class F>void Wait(F condition){
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
size_t Count(){std::lock_guard lock(gui);return published.size();}
int Last(){std::lock_guard lock(gui);return published.empty()?0:published.back();}
int main(){
    auto data=Jpeg(0x11);auto index=Index(1,1);assert(MjpegClip::Parse(data,index));
    for(size_t n=0;n<index.size();n++)assert(!MjpegClip::Parse(data,std::span(index).first(n)));
    for(size_t n=0;n<data.size();n++)assert(!MjpegClip::Parse(std::span(data).first(n),index));
    for(auto [offset,value]:std::vector<std::pair<size_t,uint32_t>>{{12,0},{12,65},{16,UINT32_MAX},{20,0},{20,10001},{24,1},{28,UINT32_MAX},{32,0},{32,10001}}){
        auto bad=index;Put32(bad,offset,value);assert(!MjpegClip::Parse(data,bad));
    }
    for(auto [offset,value]:std::vector<std::pair<size_t,uint16_t>>{{4,0},{4,467},{6,0},{8,465},{10,465}}){
        auto bad=index;Put16(bad,offset,value);assert(!MjpegClip::Parse(data,bad));
    }
    auto bad=data;bad.back()=0;assert(!MjpegClip::Parse(bad,index));
    Display display;lv_obj_t image,label;
    auto player=std::make_unique<MjpegPlayer>(&display,&image,&label);
    assert(player->Start(data,index));
    {std::unique_lock lock(decode_mutex);decode_cv.wait(lock,[]{return entered;});}
    auto replacement=Jpeg(0x33);assert(player->Start(replacement,index));
    data.clear();data.shrink_to_fit();replacement.clear();replacement.shrink_to_fit();
    {std::lock_guard lock(decode_mutex);block_old=false;decode_cv.notify_all();}
    Wait([]{return Last()==0x33;});assert(Count()==1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));assert(Count()==1);
    {std::lock_guard lock(gui);assert(image.x==16&&image.y==80&&label.hidden&&!image.hidden);}
    assert(allocations==4&&handles==1);
    auto animation=Jpeg(0x11);auto second=Jpeg(0x22);animation.insert(animation.end(),second.begin(),second.end());
    auto moving=Index(2,0);assert(player->Start(animation,moving));Wait([]{return Count()>=5;});
    player->Pause();auto paused=Count();std::this_thread::sleep_for(std::chrono::milliseconds(80));assert(Count()==paused);
    player->Resume();Wait([=]{return Count()>=paused+2;});
    player->Stop();auto stopped=Count();std::this_thread::sleep_for(std::chrono::milliseconds(40));assert(Count()==stopped);
    auto broken=index;broken[0]=0;assert(!player->Start(Jpeg(0x44),broken));
    assert(player->Start(Jpeg(0x66),index));Wait([]{return failures>0;});assert(Count()==stopped);
    assert(player->Start(Jpeg(0x44),index));Wait([]{return Last()==0x44;});
    assert(allocations==4);player.reset();assert(allocations==frees&&handles==0);
    {std::lock_guard lock(gui);assert(image.src==nullptr);}
    for(auto& task:tasks)task->thread.join();
}
'''


try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None


@unittest.skipIf(Image is None, "requires Pillow for conversion checks")
class MjpegConversionTest(unittest.TestCase):
    def test_conversion_composes_background_and_packages_frame_indexes(self):
        spec = importlib.util.spec_from_file_location("convert_mjpeg", ROOT / "scripts/convert_stopwatch_mjpeg.py")
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        pack_spec = importlib.util.spec_from_file_location("pack_mjpeg", ROOT / "scripts/build_default_assets.py")
        builder = importlib.util.module_from_spec(pack_spec)
        pack_spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "gifs"
            source.mkdir()
            background = root / "background.jpg"
            Image.new("RGB", (466, 466), (185, 227, 252)).save(background)
            for name in converter.EMOTIONS:
                first = Image.new("RGBA", (466, 466))
                ImageDraw.Draw(first).rectangle((180, 160, 280, 300), fill=(255, 0, 0, 255))
                options = {"transparency": 0, "disposal": 2}
                if name == "neutral":
                    second = first.copy()
                    ImageDraw.Draw(second).rectangle((180, 160, 280, 300), fill=(0, 255, 0, 255))
                    options.update(save_all=True, append_images=[second], duration=[100, 200], loop=0)
                first.save(source / (name + ".gif"), **options)
            output = root / "output"
            report = converter.convert(source, background, output)
            self.assertEqual(len(report["clips"]), 21)
            original = {p.name: p.read_bytes() for p in output.iterdir()}
            converter.convert(source, background, output)
            self.assertEqual(original, {p.name: p.read_bytes() for p in output.iterdir()})
            destination = root / "assets"
            destination.mkdir()
            packed = builder.process_emoji_collection(str(output), str(destination))
            self.assertEqual(len(packed), 21)
            for record in packed:
                self.assertTrue((destination / record["index"]).is_file())
                raw = (destination / record["file"]).read_bytes()
                metadata = (destination / record["index"]).read_bytes()
                magic, w, h, x, y, count, size, loops = converter.HEADER.unpack_from(metadata)
                self.assertEqual((magic, size), (b"MJI1", len(raw)))
                self.assertLessEqual(x + w, 466)
                self.assertLessEqual(y + h, 466)
                if record["name"] == "neutral":
                    self.assertEqual((count, loops), (2, 0))
                    self.assertEqual([converter.FRAME.unpack_from(metadata, 24 + n * 12)[2]
                                      for n in range(count)], [100, 200])
                else:
                    self.assertEqual((count, loops), (1, 1))
            (output / "neutral.idx").unlink()
            with self.assertRaisesRegex(ValueError, "Missing.*frame index"):
                builder.process_emoji_collection(str(output), str(destination))


if __name__ == "__main__":
    unittest.main()
