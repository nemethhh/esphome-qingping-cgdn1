"""Run the real controller against GPIO, scheduler and reader fakes on the host.

Hardware noise/airflow still needs physical verification; these tests verify
that power state and restored settings cannot cause stale readings or a late
warmup callback to re-enable an explicitly disabled module.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_pm_power_and_recovery(tmp_path):
    def put(path, text):
        p = tmp_path / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
    put("esphome/core/component.h", r'''
#pragma once
#include <functional>
#include <string>
#include <cstdint>
namespace esphome {
namespace setup_priority { constexpr float DATA=600; }
class Component { public:
 virtual ~Component()=default; virtual void setup(){}; virtual void dump_config(){};
 virtual float get_setup_priority()const{return 600;}
 std::function<void()> timer; uint32_t delay=0;
 void set_timeout(const char*, uint32_t ms, std::function<void()> f){timer=f;delay=ms;}
 bool cancel_timeout(const char*){timer={};return true;}
 void fire(){auto f=timer;timer={};if(f)f();}
 void status_set_warning(){} void status_clear_warning(){}
}; }
''')
    put("esphome/core/hal.h", r'''
#pragma once
namespace esphome { class GPIOPin {public: bool value=true;int writes=0;void setup(){};
 void digital_write(bool v){value=v;writes++;}}; }
''')
    put("esphome/core/log.h", '#pragma once\n#define ESP_LOGCONFIG(...)\n#define ESP_LOGI(...)\n#define ESP_LOGW(...)\n#define LOG_PIN(...)\n')
    put("esphome/components/pmsa003i/pmsa003i.h", r'''
#pragma once
#include <cmath>
namespace esphome {
namespace sensor {class Sensor {public:float state=0; void publish_state(float x){state=x;}};}
namespace pmsa003i {class PMSA003IComponent {public:
 bool failed=false,running=true;int updates=0;
 void stop_poller(){running=false;} void start_poller(){running=true;}
 void update(){updates++;} bool is_failed(){return failed;} void status_clear_warning(){}
};} }
''')
    source = tmp_path / "test.cpp"
    source.write_text(r'''
#include "cgdn1_pm.h"
#include <cassert>
using namespace esphome;
int main(){
 cgdn1_pm::Cgdn1Pm p; GPIOPin pin; pmsa003i::PMSA003IComponent reader;
 sensor::Sensor a,b,c;p.set_reader(&reader);p.set_enable_pin(&pin);p.set_sensors(&a,&b,&c);
 p.set_enabled(false);assert(pin.writes==0); // restored OFF must not break initial reader probe
 p.setup();assert(!pin.value && !reader.running && !p.ready() && !p.timer);
 assert(std::isnan(a.state)&&std::isnan(b.state)&&std::isnan(c.state));
 p.set_enabled(true);assert(pin.value && !reader.running && !p.ready() && p.delay==30000);
 p.set_enabled(false);p.fire();assert(!pin.value && !reader.running && reader.updates==0);
 p.set_enabled(true);p.fire();assert(pin.value && reader.running && p.ready() && reader.updates==1);
 p.set_enabled(true);assert(!p.timer && reader.updates==1); // same-state request is idempotent
 p.set_enabled(false);assert(!reader.running && std::isnan(a.state));
 reader.failed=true;p.set_enabled(true);p.fire();assert(!reader.running && !p.ready());
 // A fresh install stays ON but also waits for warmup before polling.
 cgdn1_pm::Cgdn1Pm q; q.set_reader(&reader);q.set_enable_pin(&pin);q.set_sensors(&a,&b,&c);
 reader.failed=false;q.setup();assert(pin.value&&!reader.running&&!q.ready());q.fire();assert(q.ready());
}
''')
    cc = shutil.which("c++"); assert cc, "host C++ compiler is required"
    exe = tmp_path / "test"
    subprocess.run([cc,"-std=c++17","-Wall","-Wextra","-Werror",str(source),"-I",str(tmp_path),
                    "-I",str(ROOT/"components/cgdn1_pm"),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
