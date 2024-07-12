#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <wifi_manager.hpp>
#include <ip_loc.hpp>
#include <ntp_time.hpp>
#include <gfx.hpp>
#include <lilygot54in7.hpp>
#define OPENSANS_REGULAR_IMPLEMENTATION
#include "assets/OpenSans_Regular.hpp"
const gfx::open_font &text_font = OpenSans_Regular;
using namespace gfx;
using namespace arduino;
static constexpr const char *wifi_ssid = "Communism_Will_Win";
static constexpr const char *wifi_pass = "mypalkarl";
static wifi_manager wifi_man;
static constexpr const uint32_t wifi_fetch_timeout = 30;

lilygot54in7 epd;
open_text_info oti;
static time_t time_now = 0;
static char time_buffer1[64];
static char time_buffer2[64];
static long time_offset = 0;
static ntp_time time_server;
static constexpr const uint32_t time_refresh_interval = 10 * 60;
static constexpr const char *time_minutes = "\0five past\0ten past\0quarter past\0twenty past\0twenty-five past\0half past\0twenty-five 'til\0twenty 'til\0quarter 'til\0ten 'til\0five 'til";
static constexpr const char *time_hours = "twelve\0one\0two\0three\0four\0five\0six\0seven\0eight\0nine\0ten\0eleven\0noon\0one\0two\0three\0four\0five\0six\0seven\0eight\0nine\0ten\0eleven";
static constexpr const char *time_oclock = "o'clock";
typedef struct {
    char line1[64];
    char line2[64];
} time_data_t;
QueueHandle_t draw_queue = nullptr;
typedef enum
{
    CS_IDLE = 0,
    CS_CONNECTING = 1,
    CS_CONNECTED = 2,
    CS_FETCHING = 3,
    CS_POLLING = 4
} connection_state_t;
static connection_state_t connection_state = CS_IDLE;
using color_t = color<decltype(epd)::pixel_type>;
static const char *get_str(const char *list, int index)
{
    char const *result = list;
    while (--index >= 0)
    {
        result = result + strlen(result) + 1;
    }
    return result;
}
static bool update_time_buffer(time_t now,time_data_t* out_data)
{
    tm *t = localtime(&now);
    char sz[32];
    strftime(sz, sizeof(sz), "%x %X", t);
    puts(sz);
    static int old_hour = -1;
    static int old_i = -1;
    int hour = t->tm_hour;
    int min = t->tm_min;
    int adj = 0;
    int i = (int)roundf(((float)min) / 5.0f);
    // printf("i: %d\n",i);
    if (i > 6)
    {
        adj = 1;
    }
    if ((i%12) == 0)
    {
        strcpy(out_data->line1, get_str(time_hours, hour % 24));
        if (hour == 11)
        {
            out_data->line2[0] = '\0';
        }
        else
        {
            strcpy(out_data->line2, time_oclock);
        }
    }
    else
    {
        strcpy(out_data->line1, get_str(time_minutes, i % 12));
        strcpy(out_data->line2, get_str(time_hours, (hour + adj) % 24));
    }
    bool result = old_hour != hour || old_i != i;
    old_hour = hour;
    old_i = i;
    return result;
}
void draw_task(void* arg)
{
    epd.initialize();
    epd.rotation(3);
    oti.font = &text_font;
    oti.scale = oti.font->scale(epd.dimensions().height * .5);
    oti.text = "[ clock ]";
    epd.suspend();
    epd.clear(epd.bounds());
    srect16 txt_rect = oti.font->measure_text(ssize16::max(), oti.offset, oti.text, oti.scale, oti.scaled_tab_width, oti.encoding, oti.cache).bounds();
    draw::text(epd, txt_rect.center((srect16)epd.bounds()), oti, color_t::black);
    epd.resume();
    
    while(1) {
        time_data_t data;
        if(pdTRUE==xQueueReceive(draw_queue,&data,portMAX_DELAY)) {
            puts("incoming draw");
            epd.suspend();
            epd.clear(epd.bounds());
            oti.text = data.line1;
            oti.scale = oti.font->scale(epd.dimensions().height / 4);
            srect16 txt_rect = oti.font->measure_text(ssize16::max(), oti.offset, oti.text, oti.scale, oti.scaled_tab_width, oti.encoding, oti.cache).bounds();
            draw::text(epd, txt_rect.center((srect16)epd.bounds()).offset(0, 0 - txt_rect.dimensions().height / 2 - 1), oti, color_t::black);
            oti.text = data.line2;
            txt_rect = oti.font->measure_text(ssize16::max(), oti.offset, oti.text, oti.scale, oti.scaled_tab_width, oti.encoding, oti.cache).bounds();
            draw::text(epd, txt_rect.center((srect16)epd.bounds()).offset(0, txt_rect.dimensions().height / 2 + 1), oti, color_t::black);
            epd.resume();
        }
    }
}
void update_task(void* arg)
{
    while(1) {
        ///////////////////////////////////
        // manage connection and fetching
        ///////////////////////////////////
        static uint32_t connection_refresh_ts = 0;
        static uint32_t time_ts = 0;
        switch (connection_state)
        {
        case CS_IDLE:
            if (connection_refresh_ts == 0 || millis() > (connection_refresh_ts + (time_refresh_interval * 1000)))
            {
                connection_refresh_ts = millis();
                connection_state = CS_CONNECTING;
            }
            break;
        case CS_CONNECTING:
            time_ts = 0;

            if (wifi_man.state() != wifi_manager_state::connected && wifi_man.state() != wifi_manager_state::connecting)
            {
                puts("Connecting to network...");
                wifi_man.connect(wifi_ssid, wifi_pass);
                connection_state = CS_CONNECTED;
            }
            else if (wifi_man.state() == wifi_manager_state::connected)
            {
                connection_state = CS_CONNECTED;
            }
            break;
        case CS_CONNECTED:
            if (wifi_man.state() == wifi_manager_state::connected)
            {
                puts("Connected.");
                connection_state = CS_FETCHING;
            }
            else
            {
                connection_refresh_ts = 0; // immediately try to connect again
                connection_state = CS_IDLE;
            }
            break;
        case CS_FETCHING:
            puts("Retrieving time info...");
            connection_refresh_ts = millis();
            // grabs the timezone and tz offset based on IP
            ip_loc::fetch(nullptr, nullptr, &time_offset, nullptr, 0, nullptr, 0, nullptr, 0);
            connection_state = CS_POLLING;
            time_ts = millis(); // we're going to correct for latency
            time_server.begin_request();
            break;
        case CS_POLLING:
            if (time_server.request_received())
            {
                const int latency_offset = (millis() - time_ts) / 1000;
                time_now = (time_t)(time_server.request_result() + time_offset + latency_offset);
                puts("Clock set.");
                // set the digital clock - otherwise it only updates once a minute
                time_data_t data;
                update_time_buffer(time_now,&data);
                puts("outgoing post");
                xQueueSend(draw_queue,&data,portMAX_DELAY);
            
                connection_state = CS_IDLE;
                puts("Turning WiFi off.");
                wifi_man.disconnect(true);
            }
            else if (millis() > time_ts + (wifi_fetch_timeout * 1000))
            {
                puts("Retrieval timed out. Retrying.");
                connection_state = CS_FETCHING;
            }
            break;
        }
        ///////////////////
        // Track time
        //////////////////
        static uint32_t loop_time_ts=0;
        static uint32_t loop_ts = millis();
        static time_t old_now = 0;
        if(old_now%300 != time_now%300) {
            time_data_t data;
            if (update_time_buffer(time_now,&data))
            {
                puts("outgoing post");
                xQueueSend(draw_queue,&data,portMAX_DELAY);
            }
        }
        uint32_t end_time_ts = millis();
        old_now = time_now;
        loop_time_ts += end_time_ts-loop_ts;
        while(loop_time_ts>=1000) {
            loop_time_ts-=1000;
            ++time_now;
        }
        loop_ts = millis();
        time_server.update();
        vTaskDelay(1);
    }
}
void setup()
{
    Serial.begin(115200);
    draw_queue = xQueueCreate(10,sizeof(time_data_t));
    TaskHandle_t task_handle;
    xTaskCreate(draw_task,"draw_task",4096,nullptr,1,&task_handle);
    xTaskCreate(update_task,"update_task",4096,nullptr,25,&task_handle);
}
void loop() {

}
