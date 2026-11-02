"""Exercise the board's real callbacks with a queued application and rear mux."""

import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from test_stopwatch_charging_icon import definition


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"


class StopwatchButtonsTest(unittest.TestCase):
    def test_buttons_on_stock_and_rear_variants(self):
        source = (BOARD / "m5stack_stopwatch.cc").read_text()
        methods = "\n".join(definition(source, signature) for signature in (
            "void HandleChatButton()", "void ChangeVolume(int step, bool wrap = false)",
            "void InitializeButtons()",
        ))
        rear_method = definition(source, "void InitializeRearButtons()")
        platform = r'''
            #include <algorithm>
            #include <cassert>
            #include <functional>
            #include <memory>
            #include <string>
            #include <utility>
            #include <vector>
            #include "config.h"
            #define ESP_LOGE(...) ((void)0)
            constexpr int M5IOE1_PIN_1=1, OUTPUT=1, HIGH=1;
            constexpr int M5IOE1_OK=0, M5IOE1_DRIVE_PUSHPULL=0;
            constexpr int kDeviceStateStarting=0, kDeviceStateIdle=1;
            using m5ioe1_err_t=int;
            namespace Lang::Strings { constexpr const char* VOLUME="volume"; }
            struct Application {
                int state=kDeviceStateIdle, toggles=0;
                std::vector<std::function<void()>> tasks;
                static Application& GetInstance() {static Application app;return app;}
                void Schedule(std::function<void()>&& task){tasks.push_back(std::move(task));}
                void Drain(){auto pending=std::move(tasks);tasks.clear();for(auto& f:pending)f();}
                int GetDeviceState(){return state;}
                void ToggleChatState(){++toggles;}
            };
            struct WifiManager {
                bool connected=true;
                static WifiManager& GetInstance(){static WifiManager wifi;return wifi;}
                bool IsConnected(){return connected;}
            };
            struct Button {
                static inline std::vector<int> rear_pins;
                std::function<void()> click;
                explicit Button(int pin,bool active_high=false){
                    assert(!active_high);
                    if(pin!=1 && pin!=2)rear_pins.push_back(pin);
                }
                void OnClick(std::function<void()> cb){click=std::move(cb);}
            };
            struct Mux {
                int calls=0,fail_at=0,level=0;
                int Step(int pin){assert(pin==1);++calls;return calls==fail_at?-1:0;}
                void pinModeWithRes(int pin,int mode,int* err){assert(mode==OUTPUT);*err=Step(pin);}
                int setDriveMode(int pin,int mode){assert(mode==0);return Step(pin);}
                void digitalWriteWithRes(int pin,int value,int* err){
                    *err=Step(pin);if(*err==0)level=value;
                }
            };
            struct Codec {
                int volume=50;
                int output_volume(){return volume;}
                void SetOutputVolume(int v){assert(v>=0 && v<=100);volume=v;}
            };
            struct Display {
                std::string notification;
                void ShowNotification(std::string value){notification=std::move(value);}
            };
        '''
        driver = r'''
            int main(){
                auto& app=Application::GetInstance();
                Board board;board.InitializeButtons();
                board.button1_.click();assert(app.toggles==0);
                app.Drain();assert(app.toggles==1);
                app.state=kDeviceStateStarting;WifiManager::GetInstance().connected=false;
                board.button1_.click();app.Drain();assert(board.wifi_config==1 && app.toggles==1);
                app.state=kDeviceStateIdle;
                board.codec.volume=100;board.button2_.click();
                assert(board.codec.volume==100);app.Drain();assert(board.codec.volume==0);
            #if CONFIG_STOPWATCH_REAR_BUTTONS
                assert(board.ioe_.calls==3 && board.ioe_.level==HIGH);
                assert((Button::rear_pins==std::vector<int>{3,19,20}));
                board.rear_chat_button_->click();app.Drain();assert(app.toggles==2);
                app.state=kDeviceStateStarting;
                board.rear_chat_button_->click();app.Drain();assert(board.wifi_config==2);
                app.state=kDeviceStateIdle;
                board.codec.volume=95;board.rear_volume_up_button_->click();
                app.Drain();assert(board.codec.volume==100);
                board.rear_volume_up_button_->click();app.Drain();assert(board.codec.volume==100);
                board.codec.volume=5;board.rear_volume_down_button_->click();
                app.Drain();assert(board.codec.volume==0);
                board.rear_volume_down_button_->click();app.Drain();assert(board.codec.volume==0);
                board.codec.volume=50;
                board.rear_volume_up_button_->click();board.rear_volume_up_button_->click();
                board.rear_volume_down_button_->click();board.button2_.click();
                assert(board.codec.volume==50);app.Drain();assert(board.codec.volume==70);
                assert(board.display.notification=="volume:70%");
                for(int fail_at:{1,2,3}){
                    Board failed;failed.ioe_.fail_at=fail_at;failed.InitializeButtons();
                    assert(failed.ioe_.calls==fail_at);
                    assert(!failed.rear_chat_button_ && !failed.rear_volume_up_button_ &&
                           !failed.rear_volume_down_button_);
                    failed.button1_.click();app.Drain();
                }
                assert(Button::rear_pins.size()==3);
            #else
                assert(Button::rear_pins.empty() && board.ioe_.calls==0);
            #endif
            }
        '''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "driver").mkdir()
            (work / "driver/gpio.h").write_text(textwrap.dedent('''
                #pragma once
                constexpr int GPIO_NUM_1=1,GPIO_NUM_2=2,GPIO_NUM_3=3;
                constexpr int GPIO_NUM_19=19,GPIO_NUM_20=20;
            '''))
            program = work / "buttons.cc"
            program.write_text(textwrap.dedent(platform) + '''
                struct Board {
                    Mux ioe_;Codec codec;Display display;int wifi_config=0;
                    Button button1_{BUTTON1_GPIO},button2_{BUTTON2_GPIO};
                    std::unique_ptr<Button> rear_chat_button_,rear_volume_up_button_,rear_volume_down_button_;
                    Codec* GetAudioCodec(){return &codec;}
                    Display* GetDisplay(){return &display;}
                    void EnterWifiConfigMode(){++wifi_config;}
                ''' + methods + "\n#if CONFIG_STOPWATCH_REAR_BUTTONS\n" + rear_method +
                "\n#endif\n};\n" + driver)
            compiler = shlex.split(os.environ.get("CXX", "c++"))
            for rear in (0, 1):
                with self.subTest(rear=rear):
                    executable = work / f"buttons-{rear}"
                    command = compiler + ["-std=c++23", f"-DCONFIG_STOPWATCH_REAR_BUTTONS={rear}",
                                          "-I" + str(work), "-I" + str(BOARD), str(program),
                                          "-o", str(executable)]
                    subprocess.run(command, check=True)
                    subprocess.run([executable], check=True)
            conflict = subprocess.run(compiler + ["-std=c++23", "-fsyntax-only",
                "-DCONFIG_STOPWATCH_REAR_BUTTONS=1", "-DCONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=1",
                "-I" + str(work), "-I" + str(BOARD), str(program)], capture_output=True, text=True)
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn("disable USB consoles", conflict.stderr)


if __name__ == "__main__":
    unittest.main()
