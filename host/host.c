// Host test harness: runs the game logic on a PC, with a bot or scripted keys,
// and dumps screenshots (PPM). Not part of the calculator build.
#include <stdio.h>
#include <stdlib.h>
#include "../src/game.c"

static uint16_t fb[SW * SH];
void plat_push(int x, int y, int w, int h, const uint16_t *px) {
  for (int j = 0; j < h; j++) memcpy(&fb[(y + j) * SW + x], &px[j * w], w * 2);
}

static void save_ppm(const char *name) {
  FILE *f = fopen(name, "wb");
  fprintf(f, "P6 %d %d 255\n", SW, SH);
  for (int i = 0; i < SW * SH; i++) {
    uint16_t c = fb[i];
    unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                            (unsigned char)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static float wrap(float a) {
  while (a > PI) a -= 2 * PI;
  while (a < -PI) a += 2 * PI;
  return a;
}

// ground slope under x (first track segment covering x, highest below car)
static float ground_angle_below(float x, float y) {
  float best_y = -1e9f, ang = 0;
  for (int i = 0; i < L->nseg; i++) {
    const seg_t *s = &L->segs[i];
    float ex = s->ax + s->dx * s->len;
    if (s->dx <= 0) continue;
    if (x < s->ax || x > ex) continue;
    float gy = s->ay + s->dy * (x - s->ax) / s->dx;
    if (gy < y + 0.2f && gy > best_y) { best_y = gy; ang = atan2f(s->dy, s->dx); }
  }
  return ang;
}

static int bot_flip_mode = 0;
static uint64_t bot_keys(void) {
  bool grounded = ground_timer < 0.06f;
  if (grounded) return KEYBIT(K_OK);
  // predict landing x roughly
  float lx = car.x + vx * 0.35f;
  float target = ground_angle_below(lx, car.y);
  if (bot_flip_mode && air_time < 0.9f && vy > 0) return KEYBIT(K_OK);
  float pred = wrap(car.a + w / ANG_DRAG - target);
  return pred < -0.05f ? KEYBIT(K_OK) : 0;
}

int main(int argc, char **argv) {
  game_init();
  const char *mode = argc > 1 ? argv[1] : "bot";
  float dt = 1.0f / 40;
  if (!strcmp(mode, "bot") || !strcmp(mode, "flip")) {
    bot_flip_mode = !strcmp(mode, "flip");
    int first = argc > 2 ? atoi(argv[2]) : 0, last = argc > 3 ? atoi(argv[3]) : NUM_LEVELS - 1;
    int shots = argc > 4 ? atoi(argv[4]) : 0;
    for (int lv = first; lv <= last; lv++) {
      reset_level(lv);
      state = ST_PLAY;
      int deaths = 0, crashes = 0, falls = 0;
      float t = 0;
      int frame = 0;
      float maxx = car.x;
      while (t < 120 && state == ST_PLAY) {
        bool was_c = crashed, was_f = fell;
        float px = car.x;
        uint64_t k = bot_keys();
        game_update(k, dt);
        if (car.x > maxx) maxx = car.x;
        if (crashed && !was_c) { crashes++; deaths++; printf("  lv%d crash at x=%.1f y=%.1f a=%.2f frame=%d v=%.1f\n", lv, px, car.y, wrap(car.a), frame, sqrtf(vx*vx+vy*vy)); }
        if (fell && !was_f) { falls++; deaths++; printf("  lv%d fell at x=%.1f frame=%d\n", lv, px, frame); }
        if (deaths >= 4) break;
        if (shots && frame % shots == 0) {
          game_render();
          char nm[64];
          sprintf(nm, "shots/l%d_%04d.ppm", lv, frame);
          save_ppm(nm);
        }
        t += dt;
        frame++;
      }
      printf("level %d %-9s : %s time=%.2f crashes=%d falls=%d flips=%d maxx=%.1f/%.1f\n", lv, L->name,
             state == ST_DONE || finished ? "FINISHED" : "FAILED", play_time, crashes, falls, flips_total, maxx, L->finish);
    }
  } else if (!strcmp(mode, "shot")) {
    // shot <level> <seconds> <out>
    int lv = atoi(argv[2]);
    float secs = atof(argv[3]);
    reset_level(lv);
    state = ST_PLAY;
    for (float t = 0; t < secs; t += dt) game_update(bot_keys(), dt);
    game_render();
    save_ppm(argv[4]);
  } else if (!strcmp(mode, "ui")) {
    // ui screens
    game_update(0, dt); game_render(); save_ppm("shots/ui_menu.ppm");
    game_update(KEYBIT(K_RIGHT), dt); game_update(0, dt); game_render(); save_ppm("shots/ui_menu2.ppm");
    game_update(KEYBIT(K_OK), dt); game_update(0, dt); game_render(); save_ppm("shots/ui_levels.ppm");
    game_update(KEYBIT(K_BACK), dt); game_update(0, dt);
    menu_sel = 0;
    game_update(KEYBIT(K_OK), dt); game_update(0, dt); game_render(); save_ppm("shots/ui_help.ppm");
    game_update(KEYBIT(K_OK), dt); game_update(0, dt);
    for (int i = 0; i < 20; i++) game_update(0, dt);
    game_render(); save_ppm("shots/ui_start.ppm");
    game_update(KEYBIT(K_BACK), dt); game_update(0, dt); game_render(); save_ppm("shots/ui_pause.ppm");
    game_update(KEYBIT(K_BACK), dt);
    // play level 0 with bot to the end
    for (int i = 0; i < 40 * 60 && state == ST_PLAY; i++) game_update(bot_keys(), dt);
    for (int i = 0; i < 80; i++) game_update(0, dt);
    game_render(); save_ppm("shots/ui_done.ppm");
  }
  return 0;
}
