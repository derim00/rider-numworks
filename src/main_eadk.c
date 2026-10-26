// NumWorks entry point (EADK external app)
#include <eadk.h>
#include "platform.h"

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Rider";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

void plat_push(int x, int y, int w, int h, const uint16_t *pixels) {
  eadk_display_push_rect((eadk_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}, pixels);
}

int main(int argc, char *argv[]) {
  game_init();
  uint64_t last = eadk_timing_millis();
  while (1) {
    uint64_t now = eadk_timing_millis();
    float dt = (float)(now - last) / 1000.0f;
    last = now;
    eadk_keyboard_state_t keys = eadk_keyboard_scan();
    if (!game_update(keys, dt)) break;
    eadk_display_wait_for_vblank();
    game_render();
    // keep at most ~60 fps on fast devices
    uint64_t spent = eadk_timing_millis() - now;
    if (spent < 16) eadk_timing_msleep(16 - spent);
  }
  return 0;
}
