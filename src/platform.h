#pragma once
#include <stdint.h>

// Key indices (same numbering as eadk_key_t)
enum {
  K_LEFT = 0, K_UP = 1, K_DOWN = 2, K_RIGHT = 3, K_OK = 4, K_BACK = 5,
  K_HOME = 6, K_SHIFT = 12, K_BACKSPACE = 17, K_EXE = 52
};
#define KEYBIT(k) ((uint64_t)1 << (k))

// Implemented by the platform layer (main_eadk.c or host harness)
void plat_push(int x, int y, int w, int h, const uint16_t *pixels);

// Implemented by game.c
void game_init(void);
// dt in seconds; returns 0 when the application must quit
int game_update(uint64_t keys, float dt);
void game_render(void);
