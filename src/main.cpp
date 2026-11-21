// ePaperOS v0.1 - Waveshare ESP32-S3-Touch-LCD-2 (240x320, ST7789 + CST816)
// Styl "e-papier": płaskie kolory, 2px ramki, zero cieni, zero animacji.
// UWAGA: wbudowane fonty LVGL nie mają polskich znaków -> w v0.1 etykiety są bez ogonków.
//        Wygeneruj font z ą ć ę ł ń ó ś ź ż przez lv_font_conv i podmień FONT_* poniżej.
// UWAGA: sprawdź piny w wiki Waveshare (sekcja "Pin definition") - patrz LGFX niżej.

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

// ---------------------------------------------------------------- konfiguracja
#define WIFI_SSID ""   // wpisz, żeby pobierać czas z NTP
#define WIFI_PASS ""
#define WX_LAT "50.26"   // Katowice - zmien na swoje
#define WX_LON "19.02"

// ---------------------------------------------------------------- sprzęt
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host = SPI3_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.pin_sclk = 39; c.pin_mosi = 38; c.pin_miso = 40; c.pin_dc = 42;
      c.dma_channel = SPI_DMA_CH_AUTO;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 45; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 320;
      c.invert = true; c.rgb_order = false; c.readable = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 1; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    { auto c = _touch.config();
      c.x_min = 0; c.x_max = 239; c.y_min = 0; c.y_max = 319;
      c.pin_int = 46; c.pin_rst = -1; c.bus_shared = false; c.offset_rotation = 0;
      c.i2c_port = 1; c.i2c_addr = 0x15; c.pin_sda = 48; c.pin_scl = 47; c.freq = 400000;
      _touch.config(c); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};
static LGFX tft;

// ---------------------------------------------------------------- motyw e-papier
static bool g_dark = false;
static uint8_t g_bright = 200;
static bool g_wifi = false, g_bt = false, g_sound = true;

static lv_color_t PAPER() { return lv_color_hex(g_dark ? 0x14130F : 0xE9E6DC); }
static lv_color_t INK()   { return lv_color_hex(g_dark ? 0xDAD7CC : 0x1B1B18); }
static lv_color_t MID()   { return lv_color_hex(g_dark ? 0x3B3A35 : 0xC4C0B3); }

#define FONT_S  &lv_font_montserrat_14
#define FONT_M  &lv_font_montserrat_20
#define FONT_L  &lv_font_montserrat_28
#define FONT_XL &lv_font_montserrat_48

static void paper(lv_obj_t* o, bool border = false, int bw = 2) {
  lv_obj_set_style_bg_color(o, PAPER(), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, INK(), 0);
  lv_obj_set_style_border_width(o, border ? bw : 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_set_style_outline_width(o, 0, 0);
  lv_obj_set_style_text_color(o, INK(), 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t* box(lv_obj_t* p, int w, int h, bool border = false) {
  lv_obj_t* o = lv_obj_create(p); lv_obj_set_size(o, w, h); paper(o, border); return o;
}
static lv_obj_t* text(lv_obj_t* p, const char* t, const lv_font_t* f) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, INK(), 0);
  return l;
}

// ---------------------------------------------------------------- stan UI
static lv_obj_t *scr_lock, *scr_home, *scr_app, *shade, *lock_time, *lock_date, *notif_list;
static lv_obj_t* lock_slider;
static const int SH = 300;                 // wysokość panelu powiadomień
static void build_ui(int reopen_app);
static void open_app(int id);

struct App { const char* name; const char* sym; };
static const App APPS[] = {
  {"Aparat", LV_SYMBOL_VIDEO},      {"Kalkulator", LV_SYMBOL_PLUS},   {"Kalendarz", LV_SYMBOL_LIST},
  {"Przegladarka", LV_SYMBOL_WIFI}, {"Sklep", LV_SYMBOL_DOWNLOAD},    {"Wiadomosci", LV_SYMBOL_ENVELOPE},
  {"Notatki", LV_SYMBOL_EDIT},      {"Dyktafon", LV_SYMBOL_VOLUME_MAX},{"Pliki", LV_SYMBOL_DIRECTORY},
  {"Galeria", LV_SYMBOL_IMAGE},     {"Muzyka", LV_SYMBOL_AUDIO},      {"Ustawienia", LV_SYMBOL_SETTINGS},
  {"Pogoda", LV_SYMBOL_TINT},
};
enum { A_CAM, A_CALC, A_CAL, A_WEB, A_STORE, A_MSG, A_NOTES, A_REC, A_FILES, A_GAL, A_MUSIC, A_SET, A_WEATHER };

// ---------------------------------------------------------------- powiadomienia
static void push_notif(const char* t) {
  if (!notif_list) return;
  lv_obj_t* r = box(notif_list, 216, 34, true);
  lv_obj_t* l = text(r, t, FONT_S);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_move_to_index(r, 0);
}

// ---------------------------------------------------------------- panel z góry (przeciąganie)
static void shade_set(int y) {
  if (y < 0) y = 0; if (y > SH) y = SH;
  lv_obj_set_y(shade, y - SH);
}
static void drag_cb(lv_event_t* e) {
  lv_event_code_t c = lv_event_get_code(e);
  lv_indev_t* in = lv_indev_get_act(); if (!in) return;
  lv_point_t p; lv_indev_get_point(in, &p);
  if (c == LV_EVENT_PRESSING) shade_set(p.y);
  else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    shade_set((lv_obj_get_y(shade) + SH) > SH / 3 ? SH : 0);
}
static void rebuild_async(void* arg) { build_ui((int)(intptr_t)arg); }

static void tog_cb(lv_event_t* e) {
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  if (id == 0) { g_wifi = on; if (on) { if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASS); } else WiFi.disconnect(); }
  if (id == 1) g_bt = on;        // TODO: BLE
  if (id == 3) g_sound = on;
  if (id == 2) { g_dark = on; lv_async_call(rebuild_async, (void*)(intptr_t)-1); }
}
static void bright_cb(lv_event_t* e) {
  g_bright = lv_slider_get_value(lv_event_get_target(e));
  tft.setBrightness(g_bright);
}
static void style_slider(lv_obj_t* s) {
  lv_obj_set_style_bg_color(s, MID(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, INK(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(s, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, INK(), LV_PART_KNOB);
  lv_obj_set_style_radius(s, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 4, LV_PART_KNOB);
}
static lv_obj_t* toggle(lv_obj_t* p, const char* name, int id, bool st) {
  lv_obj_t* b = box(p, 50, 50, true);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_style_bg_color(b, INK(), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(b, PAPER(), LV_STATE_CHECKED);
  if (st) lv_obj_add_state(b, LV_STATE_CHECKED);
  lv_obj_t* l = text(b, name, FONT_S); lv_obj_center(l);
  lv_obj_add_event_cb(b, tog_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)id);
  return b;
}
static void build_shade() {
  lv_obj_t* top = lv_layer_top();
  lv_obj_t* h = box(top, 240, 22); lv_obj_set_pos(h, 0, 0);
  lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(h, drag_cb, LV_EVENT_ALL, nullptr);

  shade = box(top, 240, SH, true);
  lv_obj_set_pos(shade, 0, -SH);
  lv_obj_t* row = box(shade, 232, 54); lv_obj_set_pos(row, 4, 8);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  toggle(row, "WiFi", 0, g_wifi); toggle(row, "BT", 1, g_bt);
  toggle(row, "Noc", 2, g_dark);  toggle(row, "Dzw", 3, g_sound);

  lv_obj_t* s = lv_slider_create(shade); style_slider(s);
  lv_obj_set_size(s, 216, 12); lv_obj_set_pos(s, 12, 76);
  lv_slider_set_range(s, 10, 255); lv_slider_set_value(s, g_bright, LV_ANIM_OFF);
  lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* t = text(shade, "Powiadomienia", FONT_S); lv_obj_set_pos(t, 12, 102);
  notif_list = box(shade, 220, 150); lv_obj_set_pos(notif_list, 10, 122);
  lv_obj_set_flex_flow(notif_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(notif_list, 4, 0);
  push_notif("System uruchomiony");

  lv_obj_t* grip = box(shade, 240, 24); lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* bar = box(grip, 48, 4); lv_obj_set_style_bg_color(bar, INK(), 0); lv_obj_center(bar);
  lv_obj_add_event_cb(grip, drag_cb, LV_EVENT_ALL, nullptr);
}

// ---------------------------------------------------------------- ekran blokady
static void slider_cb(lv_event_t* e) {
  lv_obj_t* s = lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
    if (lv_slider_get_value(s) >= 90) { lv_scr_load(scr_home); }
    lv_slider_set_value(s, 0, LV_ANIM_OFF);
  }
}
static void clock_tick(lv_timer_t*) {
  static const char* D[] = {"Niedziela","Poniedzialek","Wtorek","Sroda","Czwartek","Piatek","Sobota"};
  static const char* M[] = {"stycznia","lutego","marca","kwietnia","maja","czerwca","lipca","sierpnia","wrzesnia","pazdziernika","listopada","grudnia"};
  time_t now = time(nullptr);
  if (now < 100000) { lv_label_set_text(lock_time, "--:--"); lv_label_set_text(lock_date, "ustaw czas (WiFi)"); return; }
  struct tm t; localtime_r(&now, &t);
  char b[48];
  snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min); lv_label_set_text(lock_time, b);
  snprintf(b, sizeof b, "%s, %d %s", D[t.tm_wday], t.tm_mday, M[t.tm_mon]); lv_label_set_text(lock_date, b);
}
static void build_lock() {
  scr_lock = lv_obj_create(nullptr); paper(scr_lock); lv_obj_set_size(scr_lock, 240, 320);
  lock_time = text(scr_lock, "--:--", FONT_XL); lv_obj_align(lock_time, LV_ALIGN_TOP_MID, 0, 80);
  lock_date = text(scr_lock, "", FONT_M);       lv_obj_align(lock_date, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_t* hint = text(scr_lock, "przesun aby odblokowac", FONT_S); lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -64);
  lock_slider = lv_slider_create(scr_lock); style_slider(lock_slider);
  lv_obj_set_size(lock_slider, 196, 36); lv_obj_align(lock_slider, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_border_color(lock_slider, INK(), LV_PART_MAIN);
  lv_obj_set_style_border_width(lock_slider, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lock_slider, 6, LV_PART_KNOB);
  lv_slider_set_range(lock_slider, 0, 100);
  lv_obj_add_event_cb(lock_slider, slider_cb, LV_EVENT_RELEASED, nullptr);
  lv_timer_create(clock_tick, 1000, nullptr);
  clock_tick(nullptr);
}

// ---------------------------------------------------------------- ekran główny
static void tile_cb(lv_event_t* e) { open_app((int)(intptr_t)lv_event_get_user_data(e)); }
static void build_home() {
  scr_home = lv_obj_create(nullptr); paper(scr_home); lv_obj_set_size(scr_home, 240, 320);
  lv_obj_t* grid = box(scr_home, 240, 296); lv_obj_set_pos(grid, 0, 24);
  lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 8, 0);
  for (int i = 0; i < (int)(sizeof(APPS) / sizeof(APPS[0])); i++) {
    lv_obj_t* t = box(grid, 68, 80, true);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t, INK(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(t, PAPER(), LV_STATE_PRESSED);
    lv_obj_t* ic = text(t, APPS[i].sym, FONT_L); lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t* nm = text(t, APPS[i].name, FONT_S); lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

// ---------------------------------------------------------------- szkielet aplikacji
static void back_cb(lv_event_t*) {
  lv_scr_load(scr_home);
  if (scr_app) { lv_obj_del_async(scr_app); scr_app = nullptr; }
}
static lv_obj_t* app_frame(const char* title) {
  scr_app = lv_obj_create(nullptr); paper(scr_app); lv_obj_set_size(scr_app, 240, 320);
  lv_obj_t* hd = box(scr_app, 240, 30, true); lv_obj_set_pos(hd, 0, 22);
  lv_obj_t* bk = box(hd, 40, 30); lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = text(bk, LV_SYMBOL_LEFT, FONT_M); lv_obj_center(l);
  lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* tt = text(hd, title, FONT_M); lv_obj_align(tt, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t* body = box(scr_app, 240, 264); lv_obj_set_pos(body, 0, 56);
  return body;
}

// ---- Kalkulator
static double c_acc = 0, c_cur = 0; static char c_op = 0; static bool c_fresh = true; static lv_obj_t* c_lbl;
static void c_show(double v) { char b[24]; snprintf(b, sizeof b, "%.8g", v); lv_label_set_text(c_lbl, b); }
static void calc_cb(lv_event_t* e) {
  lv_obj_t* m = lv_event_get_target(e);
  const char* t = lv_btnmatrix_get_btn_text(m, lv_btnmatrix_get_selected_btn(m)); if (!t) return;
  char c = t[0];
  if (c >= '0' && c <= '9') { c_cur = c_fresh ? (c - '0') : c_cur * 10 + (c - '0'); c_fresh = false; c_show(c_cur); }
  else if (c == 'C') { c_acc = c_cur = 0; c_op = 0; c_fresh = true; c_show(0); }
  else {
    if (c_fresh && c_op && c != '=') { c_op = c; return; }
    if (c_op) switch (c_op) { case '+': c_acc += c_cur; break; case '-': c_acc -= c_cur; break;
      case '*': c_acc *= c_cur; break; case '/': c_acc = c_cur != 0 ? c_acc / c_cur : 0; break; }
    else c_acc = c_cur;
    c_show(c_acc); c_cur = c_acc; c_op = (c == '=') ? 0 : c; c_fresh = true;
  }
}
static void app_calc() {
  lv_obj_t* b = app_frame("Kalkulator");
  c_acc = c_cur = 0; c_op = 0; c_fresh = true;
  c_lbl = text(b, "0", FONT_L); lv_obj_align(c_lbl, LV_ALIGN_TOP_RIGHT, -10, 8);
  static const char* map[] = {"7","8","9","/","\n","4","5","6","*","\n","1","2","3","-","\n","C","0","=","+",""};
  lv_obj_t* m = lv_btnmatrix_create(b); lv_btnmatrix_set_map(m, map);
  lv_obj_set_size(m, 232, 196); lv_obj_set_pos(m, 4, 60);
  lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(m, 0, 0);
  lv_obj_set_style_pad_all(m, 0, 0); lv_obj_set_style_pad_gap(m, 4, 0);
  lv_obj_set_style_text_font(m, FONT_M, LV_PART_ITEMS);
  lv_obj_set_style_text_color(m, INK(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, PAPER(), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_color(m, INK(), LV_PART_ITEMS);
  lv_obj_set_style_border_width(m, 2, LV_PART_ITEMS);
  lv_obj_set_style_radius(m, 0, LV_PART_ITEMS); lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, INK(), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(m, PAPER(), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(m, calc_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  // TODO: kropka dziesiętna, zakładka przeliczania jednostek
}

// ---- Notatki (zapis w LittleFS)
static lv_obj_t *n_ta, *n_kb;
static void note_save_cb(lv_event_t*) {
  File f = LittleFS.open("/notes.txt", "w");
  if (f) { f.print(lv_textarea_get_text(n_ta)); f.close(); push_notif("Notatka zapisana"); }
}
static void app_notes() {
  lv_obj_t* b = app_frame("Notatki");
  n_ta = lv_textarea_create(b); lv_obj_set_size(n_ta, 232, 96); lv_obj_set_pos(n_ta, 4, 2);
  lv_obj_set_style_bg_color(n_ta, PAPER(), 0); lv_obj_set_style_text_color(n_ta, INK(), 0);
  lv_obj_set_style_border_color(n_ta, INK(), 0); lv_obj_set_style_border_width(n_ta, 2, 0);
  lv_obj_set_style_radius(n_ta, 0, 0); lv_obj_set_style_text_font(n_ta, FONT_S, 0);
  File f = LittleFS.open("/notes.txt", "r");
  if (f) { lv_textarea_set_text(n_ta, f.readString().c_str()); f.close(); }
  lv_obj_t* sv = box(b, 100, 28, true); lv_obj_set_pos(sv, 136, 102);
  lv_obj_add_flag(sv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = text(sv, "Zapisz", FONT_S); lv_obj_center(l);
  lv_obj_add_event_cb(sv, note_save_cb, LV_EVENT_CLICKED, nullptr);
  n_kb = lv_keyboard_create(b); lv_obj_set_size(n_kb, 240, 130); lv_obj_align(n_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(n_kb, PAPER(), 0);
  lv_obj_set_style_bg_color(n_kb, PAPER(), LV_PART_ITEMS);
  lv_obj_set_style_text_color(n_kb, INK(), LV_PART_ITEMS);
  lv_obj_set_style_border_color(n_kb, INK(), LV_PART_ITEMS);
  lv_obj_set_style_border_width(n_kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_radius(n_kb, 0, LV_PART_ITEMS); lv_obj_set_style_shadow_width(n_kb, 0, LV_PART_ITEMS);
  lv_keyboard_set_textarea(n_kb, n_ta);
}

// ---- Ustawienia
static void app_settings() {
  lv_obj_t* b = app_frame("Ustawienia");
  lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(b, 6, 0); lv_obj_set_style_pad_row(b, 6, 0);
  auto sec = [&](const char* t) { text(b, t, FONT_S); };
  sec("POLACZENIA");
  lv_obj_t* r = box(b, 228, 50); lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  toggle(r, "WiFi", 0, g_wifi); toggle(r, "BT", 1, g_bt); toggle(r, "Dzw", 3, g_sound);
  sec("WYSWIETLACZ");
  lv_obj_t* s = lv_slider_create(b); style_slider(s); lv_obj_set_size(s, 210, 12);
  lv_slider_set_range(s, 10, 255); lv_slider_set_value(s, g_bright, LV_ANIM_OFF);
  lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* d = box(b, 228, 50); lv_obj_set_flex_flow(d, LV_FLEX_FLOW_ROW);
  toggle(d, "Noc", 2, g_dark);
  // lv_async_call w tog_cb przebuduje UI; po tym otwórz ponownie Ustawienia:
  sec("EKRAN STARTOWY / BLOKADY");
  text(b, "uklad, motyw, typ blokady: TODO", FONT_S);
  sec("PAMIEC");
  char m[96];
  snprintf(m, sizeof m, "Flash FS: %u/%u KB\nPSRAM wolny: %u KB", (unsigned)(LittleFS.usedBytes() / 1024),
           (unsigned)(LittleFS.totalBytes() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  text(b, m, FONT_S);
  text(b, "Karta microSD: TODO", FONT_S);
}

// ---- Pogoda (Open-Meteo, bez klucza API)
static const char* wx_name(int c) {
  if (c == 0) return "Bezchmurnie"; if (c <= 3) return "Pochmurno"; if (c <= 48) return "Mgla";
  if (c <= 67) return "Deszcz"; if (c <= 77) return "Snieg"; if (c <= 82) return "Przelotny deszcz";
  return "Burza";
}
static void app_weather() {
  lv_obj_t* b = app_frame("Pogoda");
  if (WiFi.status() != WL_CONNECTED) {
    lv_obj_t* l = text(b, "Brak Wi-Fi\nwlacz w panelu", FONT_M);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(l); return;
  }
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(5000);
  http.begin(cl, "https://api.open-meteo.com/v1/forecast?latitude=" WX_LAT "&longitude=" WX_LON
    "&current=temperature_2m,weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min,weather_code"
    "&timezone=Europe%2FWarsaw&forecast_days=4");
  int code = http.GET();
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, http.getString())) {
    lv_obj_t* l = text(b, "Blad pobierania", FONT_M); lv_obj_center(l); http.end(); return;
  }
  http.end();
  char buf[64];
  snprintf(buf, sizeof buf, "%.0f C", (double)doc["current"]["temperature_2m"]);
  lv_obj_t* t = text(b, buf, FONT_XL); lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_t* n = text(b, wx_name(doc["current"]["weather_code"]), FONT_M); lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 70);
  snprintf(buf, sizeof buf, "Wiatr %.0f km/h", (double)doc["current"]["wind_speed_10m"]);
  lv_obj_t* w = text(b, buf, FONT_S); lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 98);
  static const char* DN[] = {"Nd","Pn","Wt","Sr","Cz","Pt","So"};
  time_t now = time(nullptr);
  for (int i = 1; i <= 3; i++) {
    lv_obj_t* r = box(b, 224, 40, true); lv_obj_set_pos(r, 8, 112 + (i - 1) * 46);
    struct tm tt; time_t d = now + i * 86400; localtime_r(&d, &tt);
    snprintf(buf, sizeof buf, "%s", now > 100000 ? DN[tt.tm_wday] : "+1d");
    lv_obj_t* a = text(r, buf, FONT_S); lv_obj_align(a, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* m = text(r, wx_name(doc["daily"]["weather_code"][i]), FONT_S); lv_obj_align(m, LV_ALIGN_LEFT_MID, 44, 0);
    snprintf(buf, sizeof buf, "%.0f/%.0f", (double)doc["daily"]["temperature_2m_max"][i], (double)doc["daily"]["temperature_2m_min"][i]);
    lv_obj_t* tp = text(r, buf, FONT_S); lv_obj_align(tp, LV_ALIGN_RIGHT_MID, -8, 0);
  }
}

// ---- reszta aplikacji (na razie szkielety)
static void app_stub(int id) {
  lv_obj_t* b = app_frame(APPS[id].name);
  lv_obj_t* l = text(b, "W budowie", FONT_M); lv_obj_center(l);
}

static void open_app(int id) {
  if (scr_app) { lv_obj_del_async(scr_app); scr_app = nullptr; }
  switch (id) {
    case A_CALC: app_calc(); break;
    case A_NOTES: app_notes(); break;
    case A_SET: app_settings(); break;
    case A_WEATHER: app_weather(); break;
    default: app_stub(id);
  }
  lv_scr_load(scr_app);
}

// ---------------------------------------------------------------- budowa całości
static void build_ui(int reopen_app) {
  lv_obj_t *old_l = scr_lock, *old_h = scr_home, *old_a = scr_app;
  scr_app = nullptr;
  lv_obj_clean(lv_layer_top());
  build_lock(); build_home(); build_shade();
  if (reopen_app >= 0) open_app(reopen_app); else lv_scr_load(scr_home);
  if (old_l) lv_obj_del_async(old_l);
  if (old_h) lv_obj_del_async(old_h);
  if (old_a) lv_obj_del_async(old_a);
}

// ---------------------------------------------------------------- LVGL <-> sprzęt
static lv_disp_draw_buf_t draw_buf; static lv_disp_drv_t disp_drv; static lv_indev_drv_t indev_drv;
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* p) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.pushImage(a->x1, a->y1, w, h, (lgfx::rgb565_t*)&p->full);
  lv_disp_flush_ready(d);
}
static void touch_cb(lv_indev_drv_t*, lv_indev_data_t* d) {
  uint16_t x, y;
  if (tft.getTouch(&x, &y)) { d->state = LV_INDEV_STATE_PR; d->point.x = x; d->point.y = y; }
  else d->state = LV_INDEV_STATE_REL;
}

void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(0); tft.setBrightness(g_bright);
  LittleFS.begin(true);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
  if (strlen(WIFI_SSID)) { WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS); g_wifi = true; }

  lv_init();
  const size_t n = 240 * 40;
  lv_color_t* buf = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&draw_buf, buf, nullptr, n);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 240; disp_drv.ver_res = 320; disp_drv.flush_cb = flush_cb; disp_drv.draw_buf = &draw_buf;
  lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
  lv_theme_basic_init(disp);
  lv_indev_drv_init(&indev_drv); indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&indev_drv);

  build_ui(-1);
  lv_scr_load(scr_lock);               // start od ekranu blokady
}

void loop() {
  static uint32_t last = millis();
  uint32_t now = millis(); lv_tick_inc(now - last); last = now;
  lv_timer_handler();
  delay(5);
}
