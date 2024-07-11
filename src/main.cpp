#include <Arduino.h>
#include <wifi_manager.hpp>
#include <ip_loc.hpp>
#include <ntp_time.hpp>
#include <gfx.hpp>
#include <lilygot54in7.hpp>
#define OPENSANS_REGULAR_IMPLEMENTATION
#include "assets/OpenSans_Regular.hpp"
const gfx::open_font& text_font = OpenSans_Regular;
using namespace gfx;
using namespace arduino;
static constexpr const char* wifi_ssid = "Communism_Will_Win";
static constexpr const char* wifi_pass = "mypalkarl";
static wifi_manager wifi_man;
static constexpr const uint32_t wifi_fetch_timeout = 30;

lilygot54in7 epd;
open_text_info oti;
static time_t time_now = 0;
static char time_buffer1[64];
static char time_buffer2[64];
static long time_offset = 0;
static ntp_time time_server;
static constexpr const uint32_t time_refresh_interval = 10*60;
static constexpr const char* time_minutes = "\0five past\0ten past\0quarter past\0twenty past\0twenty-five past\0half past\0twenty-five 'til\0twenty 'til\0quarter 'til\0ten 'til\0five 'til";
static constexpr const char* time_hours = "twelve\0one\0two\0three\0four\0five\0six\0seven\0eight\0nine\0ten\0eleven\0noon\0one\0two\0three\0four\0five\0six\0seven\0eight\0nine\0ten\0eleven";
static constexpr const char* time_oclock = "o'clock";

typedef enum {
    CS_IDLE = 0,
    CS_CONNECTING = 1,
    CS_CONNECTED = 2,
    CS_FETCHING = 3,
    CS_POLLING = 4
} connection_state_t;
static connection_state_t connection_state= CS_IDLE;
using color_t = color<decltype(epd)::pixel_type>;
static const char* get_str(const char* list,int index) {
    char const* result = list;
    while(--index>=0) {
        result = result + strlen(result)+1;
    }
    return result;
}
static bool update_time_buffer(time_t now) {
    tm* t = localtime(&now);
    static int old_hour = -1;
    static int old_i = -1;
    int hour = t->tm_hour;
    int min = t->tm_min;
    int adj = 0;
    int i = min/5;
    if(i>6) {
        adj = 1;
    }
    if(i==0) {
        strcpy(time_buffer1,get_str(time_hours,hour%24));
        if(hour==11) {
            time_buffer2[0]='\0';
        } else {
            strcpy(time_buffer2,time_oclock);
        }
    } else {
        strcpy(time_buffer1,get_str(time_minutes,i%12));
        strcpy(time_buffer2,get_str(time_hours,(hour+adj)%24));
    }
    bool result = old_hour!=hour || old_i!=i;
    old_hour = hour;
    old_i = i;
    return result;
}
void draw_time() {
    puts(time_buffer1);
    puts(time_buffer2);
    epd.suspend();
    epd.clear(epd.bounds());
    oti.text = time_buffer1;
    oti.scale = oti.font->scale(epd.dimensions().height/4);
    srect16 txt_rect = oti.font->measure_text(ssize16::max(),oti.offset,oti.text,oti.scale,oti.scaled_tab_width,oti.encoding,oti.cache).bounds();
    draw::text(epd,txt_rect.center((srect16)epd.bounds()).offset(0,0-txt_rect.dimensions().height/2-1),oti,color_t::black);
    oti.text = time_buffer2;
    txt_rect = oti.font->measure_text(ssize16::max(),oti.offset,oti.text,oti.scale,oti.scaled_tab_width,oti.encoding,oti.cache).bounds();
    draw::text(epd,txt_rect.center((srect16)epd.bounds()).offset(0,txt_rect.dimensions().height/2+1),oti,color_t::black);
    epd.resume();
}
void setup() {
    Serial.begin(115200);
    epd.initialize();
    epd.rotation(3);
    oti.font = &text_font;
    oti.scale = oti.font->scale(epd.dimensions().height*.5);
    oti.text = "[ clock ]";
    epd.suspend();
    epd.clear(epd.bounds());
    srect16 txt_rect = oti.font->measure_text(ssize16::max(),oti.offset,oti.text,oti.scale,oti.scaled_tab_width,oti.encoding,oti.cache).bounds();
    draw::text(epd,txt_rect.center((srect16)epd.bounds()),oti,color_t::black);
    epd.resume();
}

void loop() {
    ///////////////////////////////////
    // manage connection and fetching
    ///////////////////////////////////
    static uint32_t connection_refresh_ts = 0;
    static uint32_t time_ts = 0;
    switch(connection_state) { 
        case CS_IDLE:
        if(connection_refresh_ts==0 || millis() > (connection_refresh_ts+(time_refresh_interval*1000))) {
            connection_refresh_ts = millis();
            connection_state = CS_CONNECTING;
        }
        break;
        case CS_CONNECTING:
            time_ts = 0;
            
            if(wifi_man.state()!=wifi_manager_state::connected && wifi_man.state()!=wifi_manager_state::connecting) {
                puts("Connecting to network...");
                wifi_man.connect(wifi_ssid,wifi_pass);
                connection_state =CS_CONNECTED;
            } else if(wifi_man.state()==wifi_manager_state::connected) {
                connection_state = CS_CONNECTED;
            }
            break;
        case CS_CONNECTED:
            if(wifi_man.state()==wifi_manager_state::connected) {
                puts("Connected.");
                connection_state = CS_FETCHING;
            } else {
                connection_refresh_ts = 0; // immediately try to connect again
                connection_state = CS_IDLE;

            }
            break;
        case CS_FETCHING:
            puts("Retrieving time info...");
            connection_refresh_ts = millis();
            // grabs the timezone and tz offset based on IP
            ip_loc::fetch(nullptr,nullptr,&time_offset,nullptr,0,nullptr,0,nullptr,0);
            connection_state = CS_POLLING;
            time_ts = millis(); // we're going to correct for latency
            time_server.begin_request();
            break;
        case CS_POLLING:
            if(time_server.request_received()) {
                const int latency_offset = (millis()-time_ts)/1000;
                time_now=(time_t)(time_server.request_result()+time_offset+latency_offset);
                puts("Clock set.");
                // set the digital clock - otherwise it only updates once a minute
                update_time_buffer(time_now);
                draw_time();    
                connection_state = CS_IDLE;
                puts("Turning WiFi off.");
                wifi_man.disconnect(true);
                //wifi_icon.invalidate();
            } else if(millis()>time_ts+(wifi_fetch_timeout*1000)) {
                puts("Retrieval timed out. Retrying.");
                connection_state = CS_FETCHING;
            }
            break;
    }
    ///////////////////
    // Track time
    //////////////////
    static uint32_t tick_ts = millis();
    if(millis()>=tick_ts+1000) {
        tick_ts = millis();
        ++time_now;
        if(update_time_buffer(time_now)) {
           draw_time();
        }
    }
    time_server.update();
}