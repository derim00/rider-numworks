// Rider for NumWorks - port of Brackeys' "Rider Game" (Unity) to the
// NumWorks calculator. Platform independent: rendering is done in
// horizontal strips that are pushed to the screen with plat_push().
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "platform.h"

typedef struct { float ax, ay, dx, dy, len; uint8_t color; int8_t loop; } seg_t;  // loop: +k entry half, -k exit half
typedef struct {
  const char *name;
  int nseg;
  const seg_t *segs;
  float bx0;
  int nb;
  const uint16_t *bstart, *bitems;
  const float *loopx;  // x after which loop k is left behind
  float startx, starty, finish, flagx, flagy, deathy, miny, maxy;
} level_t;

#include "assets.h"
#include "levels.h"

// ------------------------------------------------------------------ config
#define SW 320
#define SH 240
#define STRIP 30
#define PPU 20.0f            // pixels per world unit
#define CAM_OFF_X 4.6f       // camera looks ahead of the car
#define CAM_OFF_Y 1.4f

#define GRAVITY 9.81f
#define SUBSTEP (1.0f / 240.0f)
#define DRIVE_ACCEL 8.0f     // Unity: AddForce(speed*fixedDt*100 = 40N) / mass 5
#define AIR_TORQUE 14.0f     // Unity: AddTorque(rotationSpeed*fixedDt*100 = 14)
#define LIN_DRAG 0.3f
#define ANG_DRAG 3.0f
#define TRACK_HALF 0.075f    // half width of the drawn line (0.15 in Unity)

#define PI 3.14159265f

// ------------------------------------------------------------------ helpers
static uint32_t rng_state = 0x12345678;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}
static float frand(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

// alpha 0..255
static inline uint16_t blend(uint16_t bg, uint16_t fg, int alpha) {
  uint32_t a = (uint32_t)(alpha + 4) >> 3;
  uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
  uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81F;
  uint32_t r = ((((f - b) * a) >> 5) + b) & 0x07E0F81F;
  return (uint16_t)((r >> 16) | r);
}

// ------------------------------------------------------------------ frame buffers
static uint16_t strip[SW * STRIP];
static uint8_t dist[SW * STRIP];
static int strip_y0;

// ------------------------------------------------------------------ track colours
static const uint8_t track_rgb[4][3] = {{252, 255, 6}, {255, 36, 36}, {40, 235, 255}, {255, 70, 220}};
#define DQ 4             // distance quantization (1/4 px)
#define DMAX 26          // max drawn distance in quarter pixels (6.5 px)
static uint16_t lut_col[4][DMAX];
static uint8_t lut_a[4][DMAX];

static void build_luts(void) {
  for (int c = 0; c < 4; c++) {
    int r = track_rgb[c][0], g = track_rgb[c][1], b = track_rgb[c][2];
    for (int q = 0; q < DMAX; q++) {
      float d = (q + 0.5f) / DQ;
      float a;
      int cr, cg, cb;
      if (d < 0.9f) {  // hot core
        cr = (r + 255 * 2) / 3; cg = (g + 255 * 2) / 3; cb = (b + 255 * 2) / 3; a = 1;
      } else if (d < 2.0f) {
        float t = (d - 0.9f) / 1.1f;
        cr = (int)((r + 255 * 2) / 3 * (1 - t) + r * t);
        cg = (int)((g + 255 * 2) / 3 * (1 - t) + g * t);
        cb = (int)((b + 255 * 2) / 3 * (1 - t) + b * t);
        a = 1;
      } else if (d < 2.6f) {
        cr = r; cg = g; cb = b; a = 1.0f - (d - 2.0f) / 0.6f * 0.45f;
      } else {  // glow halo
        float t = (d - 2.6f) / (DMAX / (float)DQ - 2.6f);
        cr = r; cg = g; cb = b;
        a = 0.55f * (1 - t) * (1 - t);
      }
      lut_col[c][q] = RGB(cr, cg, cb);
      lut_a[c][q] = (uint8_t)clampi((int)(a * 255), 0, 255);
    }
  }
}

// ------------------------------------------------------------------ game state
enum { ST_MENU, ST_LEVELS, ST_PLAY, ST_PAUSE, ST_DONE, ST_HELP };
static int state = ST_MENU;
static int menu_sel = 0, level_sel = 0, pause_sel = 0, done_sel = 0;
static float anim_t = 0;     // global animation clock
static uint64_t prev_keys = 0;
static int cur_level = 0;
static const level_t *L;

typedef struct { float x, y, a; } pose_t;
static pose_t car, car_prev;   // car = centre of mass pose
static float vx, vy, w;        // linear & angular velocity
static float ground_timer;     // time since last ground contact
static bool gas;
static bool crashed, fell, finished;
static float end_timer;        // time since crash / fall / finish
static float play_time;
static bool started;           // player pressed gas at least once
static float air_rot, air_time;
static int flips_total, flips_level_best[NUM_LEVELS];
static float best_time[NUM_LEVELS];
static bool new_record;
static float cam_x, cam_y;
static float popup_t = -1;
static char popup_text[24];
static float dust_acc;
static float time_acc;
static float best_x, stuck_t;  // progress tracking for the 'stuck' hint
static int loop_block;  // loop whose entry half is disabled (car passed its top)

// particles
#define MAXP 96
typedef struct { float x, y, vx, vy, life, max; uint16_t col; uint8_t size; } part_t;
static part_t parts[MAXP];
static int nparts;

static void spawn(float x, float y, float pvx, float pvy, float life, uint16_t col, int size) {
  if (nparts >= MAXP) {  // replace oldest-ish
    int i = rnd() % MAXP;
    parts[i] = (part_t){x, y, pvx, pvy, life, life, col, (uint8_t)size};
    return;
  }
  parts[nparts++] = (part_t){x, y, pvx, pvy, life, life, col, (uint8_t)size};
}

static void update_particles(float dt) {
  for (int i = 0; i < nparts;) {
    part_t *p = &parts[i];
    p->life -= dt;
    if (p->life <= 0) { parts[i] = parts[--nparts]; continue; }
    p->vy -= GRAVITY * 0.35f * dt;
    p->vx *= 1.0f - 1.5f * dt;
    p->vy *= 1.0f - 1.5f * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    i++;
  }
}

// ------------------------------------------------------------------ physics
typedef struct { const seg_t *s; float lx, ly; float depth; bool roof; float acc; float mass_n; } contact_t;
static contact_t contacts[CAR_NPTS];
static int ncontacts;

static inline void car_point(const pose_t *p, float lx, float ly, float *ox, float *oy) {
  float c = cosf(p->a), s = sinf(p->a);
  *ox = p->x + lx * c - ly * s;
  *oy = p->y + lx * s + ly * c;
}

static void find_contacts(const pose_t *prev, const pose_t *cur) {
  ncontacts = 0;
  int b = (int)((cur->x - L->bx0) / BUCKET_W);
  if (b < 0 || b >= L->nb) return;
  int i0 = L->bstart[b], i1 = L->bstart[b + 1];
  float cc = cosf(cur->a), cs = sinf(cur->a);
  float pc = cosf(prev->a), ps = sinf(prev->a);
  for (int i = 0; i < CAR_NPTS; i++) {
    float lx = car_pts[i][0], ly = car_pts[i][1];
    float px = cur->x + lx * cc - ly * cs, py = cur->y + lx * cs + ly * cc;
    float qx = prev->x + lx * pc - ly * ps, qy = prev->y + lx * ps + ly * pc;
    float best = 0;
    const seg_t *bs = 0;
    for (int k = i0; k < i1; k++) {
      const seg_t *s = &L->segs[L->bitems[k]];
      if (s->loop > 0 && s->loop == loop_block) continue;
      float rx = px - s->ax, ry = py - s->ay;
      float along = rx * s->dx + ry * s->dy;
      if (along < -0.04f || along > s->len + 0.04f) continue;
      float depth = TRACK_HALF - (s->dx * ry - s->dy * rx);
      if (depth <= 0 || depth > 0.6f) continue;
      // one-way: ignore if the point was already well behind the segment
      float qrx = qx - s->ax, qry = qy - s->ay;
      float pdepth = TRACK_HALF - (s->dx * qry - s->dy * qrx);
      if (pdepth > 0.05f) continue;
      if (depth > best) { best = depth; bs = s; }
    }
    if (bs) {
      contact_t *c = &contacts[ncontacts++];
      c->s = bs; c->lx = lx; c->ly = ly; c->depth = best; c->roof = car_pts[i][2] > 0.5f; c->acc = 0;
    }
  }
}

static void crash_car(void);

static void physics_substep(float h) {
  car_prev = car;
  // forces
  bool grounded = ground_timer < 0.06f;
  vy -= GRAVITY * h;
  if (gas && !crashed && !finished) {
    if (grounded) {
      vx += cosf(car.a) * DRIVE_ACCEL * h;
      vy += sinf(car.a) * DRIVE_ACCEL * h;
    } else {
      w += AIR_TORQUE / CAR_INERTIA * h;
    }
  }
  vx *= 1.0f / (1.0f + h * LIN_DRAG);
  vy *= 1.0f / (1.0f + h * LIN_DRAG);
  w *= 1.0f / (1.0f + h * ANG_DRAG);

  // predicted pose for detection
  pose_t pred = {car.x + vx * h, car.y + vy * h, car.a + w * h};
  find_contacts(&car_prev, &pred);

  float cc = cosf(pred.a), cs = sinf(pred.a);
  float impact = 0;
  bool roof_hit = false;
  for (int i = 0; i < ncontacts; i++) {
    contact_t *c = &contacts[i];
    float rx = c->lx * cc - c->ly * cs, ry = c->lx * cs + c->ly * cc;
    float nx = -c->s->dy, ny = c->s->dx;
    float rn = rx * ny - ry * nx;
    c->mass_n = 1.0f / (1.0f / CAR_MASS + rn * rn / CAR_INERTIA);
    float vn = (vx - w * ry) * nx + (vy + w * rx) * ny;
    if (-vn > impact) impact = -vn;
  }
  for (int it = 0; it < 8; it++) {
    for (int i = 0; i < ncontacts; i++) {
      contact_t *c = &contacts[i];
      float rx = c->lx * cc - c->ly * cs, ry = c->lx * cs + c->ly * cc;
      float nx = -c->s->dy, ny = c->s->dx;
      float vn = (vx - w * ry) * nx + (vy + w * rx) * ny;
      float lambda = -vn * c->mass_n;
      float na = c->acc + lambda;
      if (na < 0) na = 0;
      lambda = na - c->acc;
      c->acc = na;
      vx += nx * lambda / CAR_MASS;
      vy += ny * lambda / CAR_MASS;
      w += (rx * ny - ry * nx) * lambda / CAR_INERTIA;
    }
  }
  // integrate
  car.x += vx * h;
  car.y += vy * h;
  car.a += w * h;
  // position correction
  for (int it = 0; it < 3; it++) {
    float c0 = cosf(car.a), s0 = sinf(car.a);
    for (int i = 0; i < ncontacts; i++) {
      contact_t *c = &contacts[i];
      float rx = c->lx * c0 - c->ly * s0, ry = c->lx * s0 + c->ly * c0;
      float px = car.x + rx, py = car.y + ry;
      const seg_t *s = c->s;
      float depth = TRACK_HALF - (s->dx * (py - s->ay) - s->dy * (px - s->ax));
      if (depth <= 0.004f) continue;
      float nx = -s->dy, ny = s->dx;
      float rn = rx * ny - ry * nx;
      float P = 0.7f * (depth - 0.004f) / (1.0f / CAR_MASS + rn * rn / CAR_INERTIA);
      car.x += nx * P / CAR_MASS;
      car.y += ny * P / CAR_MASS;
      car.a += rn * P / CAR_INERTIA;
    }
  }

  // a contact is real only if it pushed (points leaving a one-way segment don't)
  int real = 0;
  for (int i = 0; i < ncontacts; i++) {
    if (contacts[i].acc > 1e-5f) {
      real++;
      if (contacts[i].roof) roof_hit = true;
      if (contacts[i].s->loop < 0) loop_block = -contacts[i].s->loop;
    }
  }
  if (real > 0) {
    if (ground_timer > 0.25f && impact > 4.0f) {
      // landing sparks
      for (int i = 0; i < 6; i++)
        spawn(car.x, car.y - 0.3f, frand(-2, 2) + vx * 0.3f, frand(0.5f, 2.5f), frand(0.2f, 0.45f), RGB(255, 230, 150), 2);
    }
    ground_timer = 0;
  } else {
    ground_timer += h;
  }
  if (loop_block && car.x > L->loopx[loop_block]) loop_block = 0;
  if (roof_hit && !crashed && !finished) crash_car();
}

// ------------------------------------------------------------------ level control
static void reset_level(int idx) {
  cur_level = idx;
  L = &levels[idx];
  // Unity start position is the transform pivot; physics works with the COM
  car.x = L->startx + CAR_COM_X;
  car.y = L->starty + CAR_COM_Y;
  car.a = 0;
  car_prev = car;
  vx = vy = w = 0;
  ground_timer = 1;
  crashed = fell = finished = false;
  end_timer = 0;
  play_time = 0;
  started = false;
  air_rot = air_time = 0;
  flips_total = 0;
  nparts = 0;
  popup_t = -1;
  new_record = false;
  cam_x = car.x + CAM_OFF_X;
  cam_y = car.y + CAM_OFF_Y;
  time_acc = 0;
  loop_block = 0;
  best_x = car.x;
  stuck_t = 0;
}

static void crash_car(void) {
  crashed = true;
  end_timer = 0;
  static const uint16_t cols[] = {RGB(255, 60, 30), RGB(255, 160, 20), RGB(255, 240, 80), RGB(200, 0, 0), RGB(80, 80, 80)};
  for (int i = 0; i < 70; i++) {
    float ang = frand(0, 2 * PI), sp = frand(1, 9);
    spawn(car.x + frand(-0.5f, 0.5f), car.y + frand(-0.2f, 0.2f), cosf(ang) * sp + vx * 0.3f, sinf(ang) * sp + vy * 0.3f,
          frand(0.4f, 1.2f), cols[rnd() % 5], 2 + rnd() % 3);
  }
}

static void show_popup(const char *t) {
  strncpy(popup_text, t, sizeof(popup_text) - 1);
  popup_text[sizeof(popup_text) - 1] = 0;
  popup_t = 0;
}

static void update_play(float dt, uint64_t keys) {
  gas = (keys & (KEYBIT(K_OK) | KEYBIT(K_EXE) | KEYBIT(K_UP))) != 0;
  if (gas) started = true;
  if (!crashed && !finished && !fell) play_time += dt;

  if (!crashed && !fell) {
    // Unity sets timeScale = 0 at the finish line: slow down to a stop
    time_acc += finished ? dt * clampf(1.0f - end_timer / 0.7f, 0, 1) : dt;
    if (time_acc > 0.1f) time_acc = 0.1f;
    while (time_acc >= SUBSTEP) {
      physics_substep(SUBSTEP);
      time_acc -= SUBSTEP;
      if (crashed) break;
    }
  }

  // flips: integrate rotation while airborne
  {
    static float last_a = 0;
    bool air = ground_timer > 0.06f;
    if (air && !crashed && !finished) {
      air_time += dt;
      air_rot += car.a - last_a;
    } else if (!air) {
      if (air_time > 0.25f && !crashed) {
        int flips = (int)((fabsf(air_rot) + 0.6f) / (2 * PI));
        if (flips > 0) {
          flips_total += flips;
          static const char *names[] = {"FLIP !", "DOUBLE FLIP !", "TRIPLE FLIP !", "QUADRUPLE FLIP !"};
          show_popup(names[clampi(flips - 1, 0, 3)]);
        }
      }
      air_time = 0;
      air_rot = 0;
    }
    last_a = car.a;
  }

  // dust when driving
  if (gas && ground_timer < 0.06f && !crashed && !finished) {
    dust_acc += dt * 40;
    float c = cosf(car.a), s = sinf(car.a);
    while (dust_acc > 1) {
      dust_acc -= 1;
      float lx = -0.45f, ly = -0.3f;
      spawn(car.x + lx * c - ly * s, car.y + lx * s + ly * c, -c * 2 + frand(-0.5f, 0.5f), -s * 2 + frand(0.3f, 1.2f),
            frand(0.25f, 0.5f), RGB(240, 220, 190), 2);
    }
  }
  update_particles(dt);
  if (popup_t >= 0) { popup_t += dt; if (popup_t > 1.2f) popup_t = -1; }

  // fall out of the world (Unity: y < -9.5 -> reload scene)
  if (!fell && !crashed && !finished && car.y < L->deathy) { fell = true; end_timer = 0; }

  // finish line
  if (!finished && !crashed && !fell && car.x - CAR_COM_X >= L->finish) {
    finished = true;
    end_timer = 0;
    if (best_time[cur_level] == 0 || play_time < best_time[cur_level]) {
      new_record = best_time[cur_level] != 0;
      best_time[cur_level] = play_time;
    }
    if (flips_total > flips_level_best[cur_level]) flips_level_best[cur_level] = flips_total;
    for (int i = 0; i < 40; i++) {
      static const uint16_t cc[] = {RGB(255, 255, 80), RGB(80, 255, 255), RGB(255, 90, 230), RGB(255, 255, 255)};
      spawn(car.x + frand(-1, 1), car.y + frand(0, 1), frand(-5, 5) + vx * 0.5f, frand(2, 8), frand(0.8f, 1.6f), cc[rnd() % 4], 2);
    }
  }
  if (car.x > best_x + 0.5f) { best_x = car.x; stuck_t = 0; } else if (started) stuck_t += dt;
  if (crashed || fell || finished) end_timer += dt;
  if ((crashed && end_timer > 1.3f) || (fell && end_timer > 0.45f)) reset_level(cur_level);
  if (finished && end_timer > 0.9f) { state = ST_DONE; done_sel = 0; }

  // camera
  if (!fell) {
    float tx = car.x + CAM_OFF_X;
    float ty = car.y + CAM_OFF_Y;
    cam_x = tx;
    float k = 1.0f - expf(-dt * 7.0f);
    cam_y += (ty - cam_y) * k;
  }
}

// ------------------------------------------------------------------ draw list
enum { C_SPRITE, C_CAR, C_TEXT, C_RECT, C_DONE, C_PARTS, C_FLAGLINE, C_RING };
typedef struct {
  uint8_t type, font, alpha;
  int16_t x, y, w, h;
  uint16_t color, color2;
  const uint16_t *rgb;
  const uint8_t *a;
  float fx, fy, ang;
  char text[48];
} cmd_t;
#define MAXCMD 48
static cmd_t cmds[MAXCMD];
static int ncmd;

static cmd_t *add_cmd(int type) {
  if (ncmd >= MAXCMD) return 0;
  cmd_t *c = &cmds[ncmd++];
  c->type = type;
  return c;
}

static void cmd_sprite(int x, int y, int w, int h, const uint16_t *rgb, const uint8_t *a) {
  cmd_t *c = add_cmd(C_SPRITE);
  if (!c) return;
  c->x = x; c->y = y; c->w = w; c->h = h; c->rgb = rgb; c->a = a; c->alpha = 255;
}

static void cmd_rect(int x, int y, int w, int h, uint16_t col, int alpha, int radius) {
  cmd_t *c = add_cmd(C_RECT);
  if (!c) return;
  c->x = x; c->y = y; c->w = w; c->h = h; c->color = col; c->alpha = alpha; c->font = radius;
}

// fonts
typedef struct { int h, asc; const uint32_t *off; const uint8_t *w, *adv, *data; } font_t;
static const font_t fonts[3] = {
    {FONT_S_H, FONT_S_ASC, font_s_off, font_s_w, font_s_adv, font_s_data},
    {FONT_M_H, FONT_M_ASC, font_m_off, font_m_w, font_m_adv, font_m_data},
    {FONT_L_H, FONT_L_ASC, font_l_off, font_l_w, font_l_adv, font_l_data},
};
enum { FS, FM, FL };

static int glyph_index(const char **p) {
  const uint8_t *s = (const uint8_t *)*p;
  uint32_t cp = *s++;
  if (cp >= 0xC0 && (*s & 0xC0) == 0x80) { cp = ((cp & 0x1F) << 6) | (*s++ & 0x3F); }
  *p = (const char *)s;
  if (cp >= 32 && cp < 127) return cp - 32;
  for (int i = 0; i < FONT_NEXTRA; i++)
    if (font_extra_cp[i] == cp) return 95 + i;
  return '?' - 32;
}

static int text_width(const char *t, int font) {
  int wsum = 0;
  while (*t) wsum += fonts[font].adv[glyph_index(&t)];
  return wsum;
}

// align: 0 left, 1 centre, 2 right ; shadow: draws a dark copy below
static void cmd_text(const char *t, int x, int y, int font, uint16_t col, int align, bool shadow) {
  int tw = text_width(t, font);
  if (align == 1) x -= tw / 2;
  else if (align == 2) x -= tw;
  if (shadow) {
    cmd_t *c = add_cmd(C_TEXT);
    if (!c) return;
    c->x = x + (font == FL ? 3 : 1); c->y = y + (font == FL ? 3 : 1); c->font = font; c->color = RGB(20, 10, 30); c->alpha = 160;
    strncpy(c->text, t, sizeof(c->text) - 1); c->text[sizeof(c->text) - 1] = 0;
    c->w = tw; c->h = fonts[font].h;
  }
  cmd_t *c = add_cmd(C_TEXT);
  if (!c) return;
  c->x = x; c->y = y; c->font = font; c->color = col; c->alpha = 255;
  strncpy(c->text, t, sizeof(c->text) - 1); c->text[sizeof(c->text) - 1] = 0;
  c->w = tw; c->h = fonts[font].h;
}

// ------------------------------------------------------------------ strip rendering
static float view_x0, view_y0;   // world coordinates of screen (0,0) (top-left)
static int bg_sx, bg_sy;         // background scroll in pixels
static bool draw_world;

typedef struct { float x0, y0, x1, y1; uint8_t col; int16_t ymin, ymax; } vseg_t;
#define MAXVSEG 400
static vseg_t vsegs[MAXVSEG];
static int nvseg;

static inline float sx_of(float x) { return (x - view_x0) * PPU; }
static inline float sy_of(float y) { return (view_y0 - y) * PPU; }

static void collect_visible_segments(void) {
  nvseg = 0;
  float wx0 = view_x0 - 1, wx1 = view_x0 + SW / PPU + 1;
  float wy1 = view_y0 + 1, wy0 = view_y0 - SH / PPU - 1;
  int b0 = clampi((int)((wx0 - L->bx0) / BUCKET_W), 0, L->nb - 1);
  int b1 = clampi((int)((wx1 - L->bx0) / BUCKET_W), 0, L->nb - 1);
  // buckets overlap; iterate segment index range instead of bucket items to avoid duplicates
  int lo = 1 << 30, hi = -1;
  for (int b = b0; b <= b1; b++)
    for (int k = L->bstart[b]; k < L->bstart[b + 1]; k++) {
      int i = L->bitems[k];
      if (i < lo) lo = i;
      if (i > hi) hi = i;
    }
  for (int i = lo; i <= hi && nvseg < MAXVSEG; i++) {
    const seg_t *s = &L->segs[i];
    float ex = s->ax + s->dx * s->len, ey = s->ay + s->dy * s->len;
    if ((s->ax < wx0 && ex < wx0) || (s->ax > wx1 && ex > wx1)) continue;
    if ((s->ay < wy0 && ey < wy0) || (s->ay > wy1 && ey > wy1)) continue;
    vseg_t *v = &vsegs[nvseg++];
    v->x0 = sx_of(s->ax); v->y0 = sy_of(s->ay);
    v->x1 = sx_of(ex); v->y1 = sy_of(ey);
    v->col = s->color;
    float r = DMAX / (float)DQ + 1;
    v->ymin = (int16_t)floorf((v->y0 < v->y1 ? v->y0 : v->y1) - r);
    v->ymax = (int16_t)ceilf((v->y0 > v->y1 ? v->y0 : v->y1) + r);
  }
}

static void render_bg(void) {
  for (int row = 0; row < STRIP; row++) {
    int by = clampi(strip_y0 + row + bg_sy, 0, BG_H - 1);
    const uint8_t *src = &bg_idx[by * BG_W];
    uint16_t *dst = &strip[row * SW];
    int bx = bg_sx % BG_W;
    if (bx < 0) bx += BG_W;
    int x = 0;
    while (x < SW) {
      int n = BG_W - bx;
      if (n > SW - x) n = SW - x;
      const uint8_t *s = src + bx;
      for (int i = 0; i < n; i++) dst[x + i] = bg_pal[s[i]];
      x += n;
      bx = 0;
    }
  }
}

static void render_lines(void) {
  bool any = false;
  float R = DMAX / (float)DQ;
  for (int i = 0; i < nvseg; i++) {
    vseg_t *v = &vsegs[i];
    if (v->ymax < strip_y0 || v->ymin >= strip_y0 + STRIP) continue;
    if (!any) { memset(dist, 0xFF, sizeof(dist)); any = true; }
    float dx = v->x1 - v->x0, dy = v->y1 - v->y0;
    float len2 = dx * dx + dy * dy;
    if (len2 < 1e-6f) continue;
    float inv = 1.0f / len2;
    float len = sqrtf(len2);
    float nx = -dy / len, ny = dx / len;
    int ya = clampi(v->ymin, strip_y0, strip_y0 + STRIP - 1);
    int yb = clampi(v->ymax, strip_y0, strip_y0 + STRIP - 1);
    float xmin = (v->x0 < v->x1 ? v->x0 : v->x1) - R, xmax = (v->x0 > v->x1 ? v->x0 : v->x1) + R;
    for (int y = ya; y <= yb; y++) {
      float py = y + 0.5f;
      float xa = xmin, xb = xmax;
      if (fabsf(nx) > 0.05f) {  // restrict to band |n.(p-a)| <= R
        float c = ny * (py - v->y0);
        float t0 = (-R - c) / nx + v->x0, t1 = (R - c) / nx + v->x0;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
        if (t0 > xa) xa = t0;
        if (t1 < xb) xb = t1;
      }
      int x0 = clampi((int)floorf(xa), 0, SW - 1), x1 = clampi((int)ceilf(xb), 0, SW - 1);
      uint8_t *drow = &dist[(y - strip_y0) * SW];
      for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f;
        float rx = px - v->x0, ry = py - v->y0;
        float t = (rx * dx + ry * dy) * inv;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        float ex = rx - t * dx, ey = ry - t * dy;
        float d2 = ex * ex + ey * ey;
        if (d2 >= R * R) continue;
        int q = (int)(sqrtf(d2) * DQ);
        if (q < (drow[x] & 63)) drow[x] = (uint8_t)(q | (v->col << 6));
      }
    }
  }
  if (!any) return;
  for (int i = 0; i < SW * STRIP; i++) {
    uint8_t d = dist[i];
    int q = d & 63;
    if (q >= DMAX) continue;
    int c = d >> 6;
    strip[i] = blend(strip[i], lut_col[c][q], lut_a[c][q]);
  }
}

static void draw_sprite_strip(int x, int y, int w, int h, const uint16_t *rgb, const uint8_t *a, int alpha) {
  int ya = y > strip_y0 ? y : strip_y0;
  int yb = (y + h < strip_y0 + STRIP) ? y + h : strip_y0 + STRIP;
  int xa = x > 0 ? x : 0, xb = (x + w < SW) ? x + w : SW;
  for (int yy = ya; yy < yb; yy++) {
    const uint16_t *sr = &rgb[(yy - y) * w];
    const uint8_t *sa = &a[(yy - y) * w];
    uint16_t *d = &strip[(yy - strip_y0) * SW];
    for (int xx = xa; xx < xb; xx++) {
      int al = sa[xx - x];
      if (!al) continue;
      if (alpha != 255) al = al * alpha / 255;
      d[xx] = al >= 250 ? sr[xx - x] : blend(d[xx], sr[xx - x], al);
    }
  }
}

static void draw_car_strip(cmd_t *c) {
  // c->fx, fy: pivot in screen px ; c->ang: angle (CCW, radians)
  float co = cosf(c->ang), si = sinf(c->ang);
  int r = 26;
  int cx = (int)c->fx, cy = (int)c->fy;
  int ya = clampi(cy - r, strip_y0, strip_y0 + STRIP), yb = clampi(cy + r, strip_y0, strip_y0 + STRIP);
  int xa = clampi(cx - r, 0, SW), xb = clampi(cx + r, 0, SW);
  static const float so[4][2] = {{-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f}};
  for (int y = ya; y < yb; y++) {
    uint16_t *d = &strip[(y - strip_y0) * SW];
    for (int x = xa; x < xb; x++) {
      int sa = 0, sr = 0, sg = 0, sb = 0;
      for (int k = 0; k < 4; k++) {
        float dx = x + 0.5f + so[k][0] - c->fx;
        float dy = -(y + 0.5f + so[k][1] - c->fy);
        float lx = dx * co + dy * si, ly = -dx * si + dy * co;
        int u = (int)floorf(CAR_PIVOT_X + lx * CAR_SS);
        int v = (int)floorf(CAR_PIVOT_Y - ly * CAR_SS);
        if ((unsigned)u >= CAR_W || (unsigned)v >= CAR_H) continue;
        int al = car_a[v * CAR_W + u];
        if (!al) continue;
        uint16_t px = car_rgb[v * CAR_W + u];
        sa += al;
        sr += ((px >> 11) & 31) * al;
        sg += ((px >> 5) & 63) * al;
        sb += (px & 31) * al;
      }
      if (!sa) continue;
      uint16_t bg = d[x];
      int inv = 1020 - sa;
      int rr = (sr + ((bg >> 11) & 31) * inv) / 1020;
      int gg = (sg + ((bg >> 5) & 63) * inv) / 1020;
      int bb = (sb + (bg & 31) * inv) / 1020;
      d[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
    }
  }
}

static void draw_text_strip(cmd_t *c) {
  if (c->y >= strip_y0 + STRIP || c->y + c->h <= strip_y0) return;
  const font_t *f = &fonts[c->font];
  const char *t = c->text;
  int x = c->x;
  while (*t) {
    int g = glyph_index(&t);
    int gw = f->w[g];
    const uint8_t *data = &f->data[f->off[g]];
    if (x + gw > 0 && x < SW) {
      int ya = c->y > strip_y0 ? c->y : strip_y0;
      int yb = (c->y + f->h < strip_y0 + STRIP) ? c->y + f->h : strip_y0 + STRIP;
      for (int y = ya; y < yb; y++) {
        uint16_t *d = &strip[(y - strip_y0) * SW];
        int base = (y - c->y) * gw;
        for (int i = 0; i < gw; i++) {
          int xx = x + i;
          if (xx < 0 || xx >= SW) continue;
          int idx = base + i;
          int v = (data[idx >> 1] >> ((idx & 1) * 4)) & 15;
          if (!v) continue;
          int al = v * 17 * c->alpha / 255;
          d[xx] = blend(d[xx], c->color, al);
        }
      }
    }
    x += f->adv[g];
  }
}

static void draw_rect_strip(cmd_t *c) {
  int ya = c->y > strip_y0 ? c->y : strip_y0;
  int yb = (c->y + c->h < strip_y0 + STRIP) ? c->y + c->h : strip_y0 + STRIP;
  int r = c->font;
  for (int y = ya; y < yb; y++) {
    int inset = 0;
    if (r) {
      int dy = -1;
      if (y < c->y + r) dy = c->y + r - y;
      else if (y >= c->y + c->h - r) dy = y - (c->y + c->h - r - 1);
      if (dy > 0) inset = r - (int)sqrtf((float)(r * r - dy * dy));
    }
    int xa = clampi(c->x + inset, 0, SW), xb = clampi(c->x + c->w - inset, 0, SW);
    uint16_t *d = &strip[(y - strip_y0) * SW];
    if (c->alpha == 255)
      for (int x = xa; x < xb; x++) d[x] = c->color;
    else
      for (int x = xa; x < xb; x++) d[x] = blend(d[x], c->color, c->alpha);
  }
}

static void draw_done_strip(cmd_t *c) {
  int ya = c->y > strip_y0 ? c->y : strip_y0;
  int yb = (c->y + DONE_H < strip_y0 + STRIP) ? c->y + DONE_H : strip_y0 + STRIP;
  for (int y = ya; y < yb; y++) {
    uint16_t *d = &strip[(y - strip_y0) * SW];
    for (int i = 0; i < DONE_W; i++) {
      int x = c->x + i;
      if (x < 0 || x >= SW) continue;
      int idx = (y - c->y) * DONE_W + i;
      int v = (done_a4[idx >> 1] >> ((idx & 1) * 4)) & 15;
      if (v) d[x] = blend(d[x], c->color, v * 17 * c->alpha / 255);
    }
  }
}

static void draw_particles_strip(void) {
  for (int i = 0; i < nparts; i++) {
    part_t *p = &parts[i];
    int x = (int)sx_of(p->x), y = (int)sy_of(p->y);
    int s = p->size;
    if (y + s <= strip_y0 || y >= strip_y0 + STRIP || x + s <= 0 || x >= SW) continue;
    int al = (int)(255 * clampf(p->life / p->max * 1.5f, 0, 1));
    for (int yy = (y > strip_y0 ? y : strip_y0); yy < y + s && yy < strip_y0 + STRIP; yy++) {
      uint16_t *d = &strip[(yy - strip_y0) * SW];
      for (int xx = (x > 0 ? x : 0); xx < x + s && xx < SW; xx++) d[xx] = blend(d[xx], p->col, al);
    }
  }
}

static void draw_flagline_strip(cmd_t *c) {
  // checkered finish strip (vertical, 6px wide)
  int x0 = c->x;
  for (int y = strip_y0; y < strip_y0 + STRIP; y++) {
    uint16_t *d = &strip[(y - strip_y0) * SW];
    for (int i = 0; i < 6; i++) {
      int x = x0 + i;
      if (x < 0 || x >= SW) continue;
      int wy = (int)floorf((y - c->fy) / 3.0f);
      bool white = (((i / 3) + wy) & 1) != 0;
      d[x] = blend(d[x], white ? 0xFFFF : 0x0000, 110);
    }
  }
}

static void draw_ring_strip(cmd_t *c) {
  // ring centred on (x,y), radius w, thickness h
  int cx = c->x, cy = c->y, r = c->w, th = c->h;
  int ya = clampi(cy - r - 1, strip_y0, strip_y0 + STRIP), yb = clampi(cy + r + 2, strip_y0, strip_y0 + STRIP);
  for (int y = ya; y < yb; y++) {
    uint16_t *d = &strip[(y - strip_y0) * SW];
    for (int x = clampi(cx - r - 1, 0, SW); x < clampi(cx + r + 2, 0, SW); x++) {
      float dd = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
      float e = fabsf(dd - (r - th * 0.5f)) - th * 0.5f;
      if (e < 1) {
        int al = e <= 0 ? c->alpha : (int)(c->alpha * (1 - e));
        d[x] = blend(d[x], c->color, al);
      }
    }
  }
}

static void render_frame(void) {
  if (draw_world) collect_visible_segments();
  for (strip_y0 = 0; strip_y0 < SH; strip_y0 += STRIP) {
    render_bg();
    if (draw_world) {
      render_lines();
    }
    for (int i = 0; i < ncmd; i++) {
      cmd_t *c = &cmds[i];
      switch (c->type) {
        case C_SPRITE:
          if (c->y < strip_y0 + STRIP && c->y + c->h > strip_y0) draw_sprite_strip(c->x, c->y, c->w, c->h, c->rgb, c->a, c->alpha);
          break;
        case C_CAR: draw_car_strip(c); break;
        case C_TEXT: draw_text_strip(c); break;
        case C_RECT:
          if (c->y < strip_y0 + STRIP && c->y + c->h > strip_y0) draw_rect_strip(c);
          break;
        case C_DONE: draw_done_strip(c); break;
        case C_PARTS: draw_particles_strip(); break;
        case C_FLAGLINE: draw_flagline_strip(c); break;
        case C_RING: draw_ring_strip(c); break;
      }
    }
    plat_push(0, strip_y0, SW, STRIP, strip);
  }
}

// ------------------------------------------------------------------ scene building
static void fmt_time(char *buf, float t) {
  int cs = (int)(t * 100 + 0.5f);
  int s = cs / 100;
  cs %= 100;
  int m = s / 60;
  s %= 60;
  char *p = buf;
  if (m > 0) {
    if (m >= 10) *p++ = '0' + m / 10;
    *p++ = '0' + m % 10;
    *p++ = ':';
    *p++ = '0' + s / 10;
  } else if (s >= 10) {
    *p++ = '0' + s / 10;
  }
  *p++ = '0' + s % 10;
  *p++ = '.';
  *p++ = '0' + cs / 10;
  *p++ = '0' + cs % 10;
  *p = 0;
}

static void itoa_s(char *buf, int v) {
  char tmp[12];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; }
  while (n) *buf++ = tmp[--n];
  *buf = 0;
}

static void setup_world_view(void) {
  view_x0 = cam_x - SW / 2 / PPU;
  view_y0 = cam_y + SH / 2 / PPU;
  bg_sx = (int)(cam_x * PPU * 0.22f) + 140;
  float mid = (L->miny + L->maxy) * 0.5f;
  bg_sy = clampi((int)((BG_H - SH) * 0.6f - (cam_y - mid) * PPU * 0.12f), 0, BG_H - SH);
  draw_world = true;
}

static void build_world_cmds(void) {
  // finish line + flag
  int fx = (int)sx_of(L->finish);
  if (fx > -10 && fx < SW + 10) {
    cmd_t *c = add_cmd(C_FLAGLINE);
    if (c) { c->x = fx - 3; c->fy = sy_of(0); }
  }
  int flx = (int)sx_of(L->flagx), fly = (int)sy_of(L->flagy);
  if (flx > -60 && flx < SW + 60) cmd_sprite(flx - (int)FLAG_OX, fly - (int)FLAG_OY, FLAG_W, FLAG_H, flag_rgb, flag_a);
  // particles behind car
  add_cmd(C_PARTS);
  if (!crashed) {
    cmd_t *c = add_cmd(C_CAR);
    if (c) {
      float co = cosf(car.a), si = sinf(car.a);
      // pivot = COM - R*COM_offset
      float px = car.x - (CAR_COM_X * co - CAR_COM_Y * si);
      float py = car.y - (CAR_COM_X * si + CAR_COM_Y * co);
      c->fx = sx_of(px);
      c->fy = sy_of(py);
      c->ang = car.a;
    }
  }
}

static void build_hud(void) {
  char buf[32];
  cmd_rect(4, 4, 150, 22, RGB(20, 12, 30), 120, 8);
  cmd_text(L->name, 12, 6, FS, RGB(255, 230, 120), 0, false);
  fmt_time(buf, play_time);
  cmd_text(buf, 148, 6, FS, 0xFFFF, 2, false);
  if (flips_total > 0) {
    char b2[24] = "FLIPS ";
    itoa_s(b2 + 6, flips_total);
    int tw = text_width(b2, FS) + 16;
    cmd_rect(SW - 4 - tw, 4, tw, 22, RGB(20, 12, 30), 120, 8);
    cmd_text(b2, SW - 12, 6, FS, RGB(120, 255, 255), 2, false);
  }
  if (!started && !crashed) {
    float blink = sinf(anim_t * 6);
    if (blink > -0.3f) {
      cmd_rect(60, 196, 200, 26, RGB(20, 12, 30), 140, 10);
      cmd_text("Maintiens OK pour rouler", SW / 2, 200, FS, 0xFFFF, 1, false);
    }
  }
  if (popup_t >= 0) {
    float a = popup_t < 0.9f ? 1.0f : 1.0f - (popup_t - 0.9f) / 0.3f;
    int y = 46 - (int)(popup_t * 10);
    cmd_t *c;
    cmd_text(popup_text, SW / 2, y, FM, RGB(255, 255, 90), 1, true);
    c = &cmds[ncmd - 1];
    c->alpha = (uint8_t)(255 * clampf(a, 0, 1));
    cmds[ncmd - 2].alpha = (uint8_t)(160 * clampf(a, 0, 1));
  }
  if (stuck_t > 5 && !finished && !crashed && !fell) {
    cmd_rect(40, 196, 240, 26, RGB(20, 12, 30), 160, 10);
    cmd_text("Bloqué ? Retour > Recommencer", SW / 2, 200, FS, RGB(255, 245, 60), 1, false);
  }
  if (crashed) {
    cmd_text("CRASH !", SW / 2, 80, FL, RGB(255, 70, 50), 1, true);
  }
  if (fell) {
    int al = (int)clampf(end_timer / 0.45f * 255, 0, 255);
    cmd_rect(0, 0, SW, SH, 0, al, 0);
  }
  if (!fell && !crashed && play_time < 0.25f && !finished) {  // fade in
    cmd_rect(0, 0, SW, SH, 0, (int)(255 * (1 - play_time / 0.25f)), 0);
  }
}

static void build_button(int cx, int cy, bool big, const char *label, bool sel) {
  int wb = big ? BTN_BIG_W : BTN_SMALL_W, hb = big ? BTN_BIG_H : BTN_SMALL_H;
  if (sel) {
    cmd_t *c = add_cmd(C_RING);
    if (c) {
      c->x = cx; c->y = cy;
      c->w = wb / 2 + 5 + (int)(2 * sinf(anim_t * 6)); c->h = 4;
      c->color = 0xFFFF; c->alpha = 230;
    }
  }
  cmd_sprite(cx - wb / 2, cy - hb / 2, wb, hb, big ? btn_big_rgb : btn_small_rgb, big ? btn_big_a : btn_small_a);
  int f = big ? FM : FS;
  cmd_text(label, cx, cy - fonts[f].h / 2, f, RGB(50, 50, 50), 1, false);
}

static void build_menu(void) {
  bg_sx = (int)(anim_t * 18);
  bg_sy = BG_H - SH - 10;
  draw_world = false;
  cmd_rect(0, 0, SW, SH, RGB(40, 10, 50), 50, 0);
  cmd_text("RIDER", 24, 14, FL, RGB(255, 245, 60), 0, true);
  cmd_text("NumWorks edition", 28, 62, FS, 0xFFFF, 0, true);
  float bob = sinf(anim_t * 3) * 3;
  cmd_sprite(18, 120 + (int)bob, MENUCAR_W, MENUCAR_H, menucar_rgb, menucar_a);
  build_button(232, 76, true, "JOUER", menu_sel == 0);
  build_button(188, 180, false, "NIVEAUX", menu_sel == 1);
  build_button(272, 180, false, "QUITTER", menu_sel == 2);
  cmd_text("Flèches + OK     shift : aide", 8, SH - 20, FS, RGB(255, 255, 255), 0, true);
}

static void build_levels(void) {
  bg_sx = (int)(anim_t * 18);
  bg_sy = BG_H - SH - 10;
  draw_world = false;
  cmd_rect(0, 0, SW, SH, RGB(30, 10, 40), 110, 0);
  cmd_text("NIVEAUX", SW / 2, 6, FM, RGB(255, 245, 60), 1, true);
  for (int i = 0; i < NUM_LEVELS; i++) {
    int col = i % 3, row = i / 3;
    int x = 12 + col * 101, y = 38 + row * 66;
    bool sel = i == level_sel;
    if (sel) cmd_rect(x - 3, y - 3, 96, 64, RGB(255, 235, 40), 255, 10);
    cmd_rect(x, y, 90, 58, sel ? RGB(60, 30, 70) : RGB(25, 12, 35), sel ? 255 : 190, 8);
    char nb[4];
    itoa_s(nb, i + 1);
    cmd_text(nb, x + 8, y + 2, FM, sel ? RGB(255, 245, 60) : RGB(200, 200, 220), 0, false);
    cmd_text(levels[i].name, x + 45, y + 22, FS, 0xFFFF, 1, false);
    if (best_time[i] > 0) {
      char tb[16];
      fmt_time(tb, best_time[i]);
      cmd_text(tb, x + 84, y + 4, FS, RGB(120, 255, 140), 2, false);
    }
    if (flips_level_best[i] > 0) {
      char fb[16] = "flips ";
      itoa_s(fb + 6, flips_level_best[i]);
      cmd_text(fb, x + 45, y + 39, FS, RGB(120, 230, 255), 1, false);
    }
  }
}

static void build_overlay_menu(const char *title, const char *const *items, int n, int sel) {
  cmd_rect(0, 0, SW, SH, RGB(10, 5, 20), 140, 0);
  int h = 50 + n * 30;
  int y0 = (SH - h) / 2;
  cmd_rect(70, y0, 180, h, RGB(30, 15, 45), 230, 12);
  cmd_text(title, SW / 2, y0 + 6, FM, RGB(255, 245, 60), 1, false);
  for (int i = 0; i < n; i++) {
    int y = y0 + 40 + i * 30;
    if (i == sel) cmd_rect(84, y - 2, 152, 26, RGB(255, 235, 40), 255, 8);
    cmd_text(items[i], SW / 2, y + 2, FS, i == sel ? RGB(40, 20, 40) : 0xFFFF, 1, false);
  }
}

static const char *const pause_items[] = {"Continuer", "Recommencer", "Niveaux", "Menu principal"};

static void build_done(void) {
  float t = end_timer - 0.9f;
  cmd_rect(0, 0, SW, SH, RGB(10, 5, 20), (int)clampf(t * 400, 0, 150), 0);
  cmd_rect(10, 10, 300, 220, 0xFFFF, (int)clampf(t * 900, 0, 235), 14);
  cmd_t *c = add_cmd(C_DONE);
  if (c) { c->x = 20; c->y = 26; c->color = RGB(20, 20, 20); c->alpha = (uint8_t)clampf(t * 900, 0, 255); }
  char buf[32];
  int xr = 236;
  cmd_text("TEMPS", xr, 30, FS, RGB(110, 110, 120), 1, false);
  fmt_time(buf, play_time);
  cmd_text(buf, xr, 46, FM, RGB(20, 20, 20), 1, false);
  cmd_text("RECORD", xr, 76, FS, RGB(110, 110, 120), 1, false);
  fmt_time(buf, best_time[cur_level]);
  cmd_text(buf, xr, 92, FM, new_record ? RGB(220, 40, 40) : RGB(20, 20, 20), 1, false);
  if (new_record && sinf(anim_t * 8) > -0.2f) cmd_text("NOUVEAU !", xr, 116, FS, RGB(220, 40, 40), 1, false);
  char fb[20] = "Flips : ";
  itoa_s(fb + 8, flips_total);
  cmd_text(fb, xr, 136, FS, RGB(20, 120, 160), 1, false);
  static const char *const items[] = {"SUIVANT", "REJOUER", "NIVEAUX"};
  for (int i = 0; i < 3; i++) {
    int x = 22 + i * 96, y = 192;
    bool sel = i == done_sel;
    cmd_rect(x, y, 88, 28, sel ? RGB(255, 210, 20) : RGB(225, 225, 230), 255, 10);
    cmd_text(items[i], x + 44, y + 5, FS, sel ? RGB(30, 30, 30) : RGB(90, 90, 100), 1, false);
  }
}

static void build_help(void) {
  bg_sx = (int)(anim_t * 18);
  bg_sy = BG_H - SH - 10;
  draw_world = false;
  cmd_rect(0, 0, SW, SH, RGB(20, 8, 30), 150, 0);
  cmd_text("COMMENT JOUER", SW / 2, 8, FM, RGB(255, 245, 60), 1, true);
  static const char *const lines[] = {
      "Maintiens OK (ou EXE / Haut) :",
      "  - au sol : la voiture accélère",
      "  - en l'air : elle tourne (flip !)",
      "Relâche pour atterrir sur les roues.",
      "Le toit touche le sol = CRASH.",
      "Franchis le drapeau pour gagner.",
      "Retour : pause / menu",
  };
  for (int i = 0; i < 7; i++) cmd_text(lines[i], 16, 40 + i * 23, FS, 0xFFFF, 0, false);
  if (sinf(anim_t * 5) > -0.3f) cmd_text("OK : c'est parti !", SW / 2, 212, FS, RGB(255, 245, 60), 1, true);
}

// ------------------------------------------------------------------ main update
static int exit_requested = 0;

void game_init(void) {
  build_luts();
  reset_level(0);
  state = ST_MENU;
}

int game_update(uint64_t keys, float dt) {
  if (dt > 0.1f) dt = 0.1f;
  anim_t += dt;
  uint64_t pressed = keys & ~prev_keys;
  prev_keys = keys;
  if (pressed & KEYBIT(K_HOME)) return 0;
  bool k_ok = (pressed & (KEYBIT(K_OK) | KEYBIT(K_EXE))) != 0;
  bool k_back = (pressed & (KEYBIT(K_BACK) | KEYBIT(K_BACKSPACE))) != 0;
  bool k_up = (pressed & KEYBIT(K_UP)) != 0, k_down = (pressed & KEYBIT(K_DOWN)) != 0;
  bool k_left = (pressed & KEYBIT(K_LEFT)) != 0, k_right = (pressed & KEYBIT(K_RIGHT)) != 0;

  switch (state) {
    case ST_MENU:
      if (k_left || k_up) menu_sel = (menu_sel + 2) % 3;
      if (k_right || k_down) menu_sel = (menu_sel + 1) % 3;
      if (k_back) return 0;
      if (pressed & KEYBIT(K_SHIFT)) state = ST_HELP;
      if (k_ok) {
        if (menu_sel == 0) {
          // play: first level without a time (or the selected one)
          int lv = 0;
          while (lv < NUM_LEVELS - 1 && best_time[lv] > 0) lv++;
          reset_level(lv);
          level_sel = lv;
          static bool help_seen = false;  // show the controls the first time
          state = help_seen ? ST_PLAY : ST_HELP;
          help_seen = true;
          prev_keys |= KEYBIT(K_OK) | KEYBIT(K_EXE);
        } else if (menu_sel == 1) {
          state = ST_LEVELS;
        } else {
          return 0;
        }
      }
      break;
    case ST_HELP:
      if (k_ok || k_back) {
        state = (k_ok) ? ST_PLAY : ST_MENU;
        if (k_ok) reset_level(cur_level);
      }
      break;
    case ST_LEVELS:
      if (k_left) level_sel = (level_sel + NUM_LEVELS - 1) % NUM_LEVELS;
      if (k_right) level_sel = (level_sel + 1) % NUM_LEVELS;
      if (k_up) level_sel = (level_sel + NUM_LEVELS - 3) % NUM_LEVELS;
      if (k_down) level_sel = (level_sel + 3) % NUM_LEVELS;
      if (k_back) state = ST_MENU;
      if (k_ok) { reset_level(level_sel); state = ST_PLAY; }
      break;
    case ST_PLAY:
      if (k_back) { state = ST_PAUSE; pause_sel = 0; break; }
      update_play(dt, keys);
      break;
    case ST_PAUSE:
      if (k_up) pause_sel = (pause_sel + 3) % 4;
      if (k_down) pause_sel = (pause_sel + 1) % 4;
      if (k_back) state = ST_PLAY;
      if (k_ok) {
        if (pause_sel == 0) state = ST_PLAY;
        else if (pause_sel == 1) { reset_level(cur_level); state = ST_PLAY; }
        else if (pause_sel == 2) { level_sel = cur_level; state = ST_LEVELS; }
        else state = ST_MENU;
        prev_keys |= KEYBIT(K_OK) | KEYBIT(K_EXE);
      }
      break;
    case ST_DONE:
      end_timer += dt;
      update_particles(dt);
      if (k_left) done_sel = (done_sel + 2) % 3;
      if (k_right) done_sel = (done_sel + 1) % 3;
      if (k_back) { level_sel = cur_level; state = ST_LEVELS; }
      if (k_ok && end_timer > 1.2f) {
        if (done_sel == 0) { reset_level((cur_level + 1) % NUM_LEVELS); level_sel = cur_level; state = ST_PLAY; }
        else if (done_sel == 1) { reset_level(cur_level); state = ST_PLAY; }
        else { level_sel = cur_level; state = ST_LEVELS; }
        prev_keys |= KEYBIT(K_OK) | KEYBIT(K_EXE);
      }
      break;
  }
  return !exit_requested;
}

void game_render(void) {
  ncmd = 0;
  switch (state) {
    case ST_MENU: build_menu(); break;
    case ST_LEVELS: build_levels(); break;
    case ST_HELP: build_help(); break;
    case ST_PLAY:
    case ST_PAUSE:
    case ST_DONE:
      setup_world_view();
      build_world_cmds();
      if (state == ST_PLAY) build_hud();
      else if (state == ST_PAUSE) build_overlay_menu("PAUSE", pause_items, 4, pause_sel);
      else build_done();
      break;
  }
  render_frame();
}
