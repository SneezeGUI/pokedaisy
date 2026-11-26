/* GPL-3.0. Portable, output-space display effects over packed RGBA pixels.
 * GBA colour follows mGBA's gba-color.shader defaults (Pokefan531 and hunterk, MPL-2.0).
 * LCD and scanline cell masks follow mGBA's shaders by Dominus Iniquitatis (MIT). */
#include "display_shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GAMMA_STEPS 4096u
#define BEAM_COLOR_STEPS 65u
#define BEAM_DISTANCE_STEPS 256u

struct crt_axis {
  size_t first, second;
  uint16_t fraction;
};

struct pocket_display_shader {
  struct pocket_display_shader_config config;
  uint8_t *pixels;
  size_t pixel_capacity;
  uint8_t *color;
  size_t color_capacity;
  size_t *map;
  size_t map_capacity;
  uint8_t *phase;
  size_t phase_capacity;
  struct crt_axis *crt_axis;
  size_t crt_capacity;
  float gba_in[256];
  float gba_out[GAMMA_STEPS + 1];
  uint16_t crt_in[256];
  uint8_t crt_out[GAMMA_STEPS + 1];
  uint16_t beam[BEAM_COLOR_STEPS][BEAM_DISTANCE_STEPS];
};

static uint32_t load_rgba(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void store_rgba(uint8_t *p, uint32_t c) {
  p[0] = (uint8_t)(c >> 24);
  p[1] = (uint8_t)(c >> 16);
  p[2] = (uint8_t)(c >> 8);
  p[3] = (uint8_t)c;
}

static void *grow(void *old, size_t *capacity, size_t bytes) {
  if (*capacity >= bytes)
    return old;
  void *p = realloc(old, bytes);
  if (p)
    *capacity = bytes;
  return p;
}

static bool frame_layout(size_t w, size_t h, size_t *stride, size_t *bytes) {
  if (!w || !h || w > POCKET_DISPLAY_SHADER_MAX_DIMENSION || h > POCKET_DISPLAY_SHADER_MAX_DIMENSION ||
      w > SIZE_MAX / 4)
    return false;
  *stride = w * 4;
  if (h > SIZE_MAX / *stride)
    return false;
  *bytes = h * *stride;
  return true;
}

static size_t map_pixel(size_t pos, size_t src_size, size_t dst_size) {
  return (size_t)((((uint64_t)pos * 2 + 1) * src_size) / ((uint64_t)dst_size * 2));
}

static uint8_t map_phase(size_t pos, size_t src_size, size_t dst_size, unsigned parts) {
  uint64_t subpixel = (((uint64_t)pos * 2 + 1) * src_size * parts) / ((uint64_t)dst_size * 2);
  return (uint8_t)(subpixel % parts);
}

static uint32_t dim(uint32_t p, unsigned numerator, unsigned denominator) {
  uint32_t a = p & 0xffu;
  uint32_t r = ((p >> 24) * numerator + denominator / 2) / denominator;
  uint32_t g = (((p >> 16) & 0xffu) * numerator + denominator / 2) / denominator;
  uint32_t b = (((p >> 8) & 0xffu) * numerator + denominator / 2) / denominator;
  return r << 24 | g << 16 | b << 8 | a;
}

static bool needs_color(const struct pocket_display_shader *s) {
  return s->config.gba_colors || s->config.mode == POCKET_DISPLAY_SHADER_GBA_COLOR;
}

struct pocket_display_shader *pocket_display_shader_create(struct pocket_display_shader_config config) {
  if ((unsigned)config.mode > POCKET_DISPLAY_SHADER_EDGE_SMOOTH)
    return NULL;
  struct pocket_display_shader *s = calloc(1, sizeof *s);
  if (!s)
    return NULL;
  s->config = config;
  if (needs_color(s)) {
    for (unsigned i = 0; i < 256; i++)
      s->gba_in[i] = (float)pow(i / 255.0, 2.7);
    for (unsigned i = 0; i <= GAMMA_STEPS; i++)
      s->gba_out[i] = (float)pow(i / (double)GAMMA_STEPS, 0.4625);
  }
  if (config.mode == POCKET_DISPLAY_SHADER_CRT) {
    for (unsigned i = 0; i < 256; i++)
      s->crt_in[i] = (uint16_t)lrint(pow(i / 255.0, 2.2) * 65535.0);
    for (unsigned i = 0; i <= GAMMA_STEPS; i++)
      s->crt_out[i] = (uint8_t)lrint(pow(i / (double)GAMMA_STEPS, 1.0 / 2.2) * 255.0);
    for (unsigned c = 0; c < BEAM_COLOR_STEPS; c++) {
      double intensity = c / (double)(BEAM_COLOR_STEPS - 1);
      double width = 0.22 + 0.10 * intensity;
      for (unsigned d = 0; d < BEAM_DISTANCE_STEPS; d++) {
        double distance = d / (double)(BEAM_DISTANCE_STEPS - 1);
        s->beam[c][d] = (uint16_t)lrint(exp(-distance * distance / (2.0 * width * width)) * 65535.0);
      }
    }
  }
  return s;
}

void pocket_display_shader_destroy(struct pocket_display_shader *s) {
  if (!s)
    return;
  free(s->crt_axis);
  free(s->phase);
  free(s->map);
  free(s->color);
  free(s->pixels);
  free(s);
}

static float clamp01(float c) {
  return c < 0 ? 0 : c > 1 ? 1 : c;
}

static uint8_t encode_gamma(const float *table, float c) {
  c = clamp01(c);
  unsigned i = (unsigned)(c * GAMMA_STEPS + 0.5f);
  return (uint8_t)(table[i] * 255.0f + 0.5f);
}

static void colorize(struct pocket_display_shader *s, const uint8_t *src, size_t src_stride, size_t w, size_t h) {
  size_t dst_stride = w * 4;
  for (size_t y = 0; y < h; y++) {
    const uint8_t *in = src + y * src_stride;
    uint8_t *out = s->color + y * dst_stride;
    for (size_t x = 0; x < w; x++, in += 4, out += 4) {
      float r = s->gba_in[in[0]] * 0.99f;
      float g = s->gba_in[in[1]] * 0.99f;
      float b = s->gba_in[in[2]] * 0.99f;
      out[0] = encode_gamma(s->gba_out, 0.84f * r + 0.18f * g);
      out[1] = encode_gamma(s->gba_out, 0.09f * r + 0.67f * g + 0.26f * b);
      out[2] = encode_gamma(s->gba_out, 0.15f * r + 0.10f * g + 0.73f * b);
      out[3] = in[3];
    }
  }
}

static uint32_t fetch(const uint8_t *src, size_t stride, size_t w, size_t h, ptrdiff_t x, ptrdiff_t y) {
  x = x < 0 ? 0 : x >= (ptrdiff_t)w ? (ptrdiff_t)w - 1 : x;
  y = y < 0 ? 0 : y >= (ptrdiff_t)h ? (ptrdiff_t)h - 1 : y;
  return load_rgba(src + (size_t)y * stride + (size_t)x * 4);
}

static float smooth_fraction(float f) {
  float t = clamp01((f - 0.2f) / 0.6f);
  return t * t * (3.0f - 2.0f * t);
}

static size_t crt_index(ptrdiff_t p, size_t size) {
  return p < 0 ? 0 : p >= (ptrdiff_t)size ? size - 1 : (size_t)p;
}

static void prepare_crt_axis(struct pocket_display_shader *s, size_t sw, size_t sh, size_t dw, size_t dh) {
  for (size_t x = 0; x < dw; x++) {
    float px = ((x + 0.5f) * sw / dw) - 0.5f;
    ptrdiff_t base = (ptrdiff_t)floorf(px);
    s->crt_axis[x] = (struct crt_axis){crt_index(base, sw), crt_index(base + 1, sw),
                                       (uint16_t)lrintf(smooth_fraction(px - base) * 1024.0f)};
  }
  for (size_t y = 0; y < dh; y++) {
    float py = ((y + 0.5f) * sh / dh) - 0.5f;
    ptrdiff_t base = (ptrdiff_t)floorf(py);
    s->crt_axis[dw + y] = (struct crt_axis){crt_index(base, sh), crt_index(base + 1, sh),
                                            (uint16_t)lrintf((py - base) * 255.0f)};
  }
}

static uint8_t encode_crt(const struct pocket_display_shader *s, uint32_t linear) {
  if (linear >= 65535)
    return 255;
  return s->crt_out[(linear + 8) >> 4];
}

static void render_crt(const struct pocket_display_shader *s, const uint8_t *src, size_t src_stride, size_t dw,
                       size_t dh) {
  size_t dst_stride = dw * 4;
  for (size_t y = 0; y < dh; y++) {
    const struct crt_axis vertical = s->crt_axis[dw + y];
    unsigned distance0 = vertical.fraction, distance1 = 255 - distance0;
    const uint8_t *row0 = src + vertical.first * src_stride;
    const uint8_t *row1 = src + vertical.second * src_stride;
    uint8_t *out = s->pixels + y * dst_stride;
    for (size_t x = 0; x < dw; x++, out += 4) {
      const struct crt_axis horizontal = s->crt_axis[x];
      uint32_t p00 = load_rgba(row0 + horizontal.first * 4);
      uint32_t p10 = load_rgba(row0 + horizontal.second * 4);
      uint32_t p01 = load_rgba(row1 + horizontal.first * 4);
      uint32_t p11 = load_rgba(row1 + horizontal.second * 4);
      uint8_t rgb[3];
      for (unsigned channel = 0; channel < 3; channel++) {
        unsigned shift = 24 - channel * 8;
        int32_t a = s->crt_in[(p00 >> shift) & 0xffu];
        int32_t b = s->crt_in[(p10 >> shift) & 0xffu];
        int32_t c = s->crt_in[(p01 >> shift) & 0xffu];
        int32_t d = s->crt_in[(p11 >> shift) & 0xffu];
        uint32_t row0 = (uint32_t)(a + (b - a) * horizontal.fraction / 1024);
        uint32_t row1 = (uint32_t)(c + (d - c) * horizontal.fraction / 1024);
        uint32_t linear = ((row0 >> 2) * s->beam[row0 >> 10][distance0] +
                           (row1 >> 2) * s->beam[row1 >> 10][distance1] + 8192) >> 14;
        linear = x % 3 == channel ? (linear * 3 + 1) / 2 : linear * 9 / 10;
        rgb[channel] = encode_crt(s, linear);
      }
      size_t alpha_x = s->map[x], alpha_y = s->map[dw + y];
      out[0] = rgb[0];
      out[1] = rgb[1];
      out[2] = rgb[2];
      out[3] = src[alpha_y * src_stride + alpha_x * 4 + 3];
    }
  }
}

static void render_frame(struct pocket_display_shader *s, const uint8_t *src, size_t src_stride, size_t sw, size_t sh,
                         size_t dw, size_t dh) {
  if (s->config.mode == POCKET_DISPLAY_SHADER_CRT) {
    render_crt(s, src, src_stride, dw, dh);
    return;
  }
  size_t dst_stride = dw * 4;
  for (size_t y = 0; y < dh; y++) {
    uint8_t *out = s->pixels + y * dst_stride;
    for (size_t x = 0; x < dw; x++, out += 4) {
      uint32_t p;
      if (s->config.mode == POCKET_DISPLAY_SHADER_EDGE_SMOOTH) {
        size_t ex = s->map[x], ey = s->map[dw + y];
        size_t sx = ex / 2, sy = ey / 2;
        uint32_t e = fetch(src, src_stride, sw, sh, (ptrdiff_t)sx, (ptrdiff_t)sy);
        uint32_t b = fetch(src, src_stride, sw, sh, (ptrdiff_t)sx, (ptrdiff_t)sy - 1);
        uint32_t d = fetch(src, src_stride, sw, sh, (ptrdiff_t)sx - 1, (ptrdiff_t)sy);
        uint32_t f = fetch(src, src_stride, sw, sh, (ptrdiff_t)sx + 1, (ptrdiff_t)sy);
        uint32_t h = fetch(src, src_stride, sw, sh, (ptrdiff_t)sx, (ptrdiff_t)sy + 1);
        if (!(ey & 1))
          p = !(ex & 1) ? (d == b && b != f && d != h ? d : e) : (b == f && b != d && f != h ? f : e);
        else
          p = !(ex & 1) ? (d == h && d != b && h != f ? d : e) : (h == f && d != h && b != f ? f : e);
      } else {
        size_t sx = s->map[x], sy = s->map[dw + y];
        p = load_rgba(src + sy * src_stride + sx * 4);
        if (s->config.mode == POCKET_DISPLAY_SHADER_LCD && (!(s->phase[x] & 3) || !(s->phase[dw + y] & 3)))
          p = dim(p, 13, 20);
        else if (s->config.mode == POCKET_DISPLAY_SHADER_SCANLINES && !(s->phase[dw + y] & 1))
          p = dim(p, 1, 2);
      }
      store_rgba(out, p);
    }
  }
}

enum pocket_display_shader_result
pocket_display_shader_process(struct pocket_display_shader *s, const void *src, size_t sw, size_t sh,
                              size_t src_stride, size_t dw, size_t dh, struct pocket_display_shader_frame *out) {
  if (out)
    *out = (struct pocket_display_shader_frame){0};
  size_t src_row, src_bytes, dst_stride, dst_bytes;
  if (!s || !src || !out || !frame_layout(sw, sh, &src_row, &src_bytes) ||
      !frame_layout(dw, dh, &dst_stride, &dst_bytes) || src_stride < src_row ||
      (sh > 1 && src_stride > (SIZE_MAX - src_row) / (sh - 1)))
    return POCKET_DISPLAY_SHADER_INVALID_ARGUMENT;

  size_t map_count = dw + dh;
  size_t *map = grow(s->map, &s->map_capacity, map_count * sizeof *map);
  if (!map)
    return POCKET_DISPLAY_SHADER_NO_MEMORY;
  s->map = map;
  size_t sample_w = s->config.mode == POCKET_DISPLAY_SHADER_EDGE_SMOOTH ? sw * 2 : sw;
  size_t sample_h = s->config.mode == POCKET_DISPLAY_SHADER_EDGE_SMOOTH ? sh * 2 : sh;
  for (size_t x = 0; x < dw; x++)
    s->map[x] = map_pixel(x, sample_w, dw);
  for (size_t y = 0; y < dh; y++)
    s->map[dw + y] = map_pixel(y, sample_h, dh);

  if (s->config.mode == POCKET_DISPLAY_SHADER_CRT) {
    struct crt_axis *axis = grow(s->crt_axis, &s->crt_capacity, map_count * sizeof *axis);
    if (!axis)
      return POCKET_DISPLAY_SHADER_NO_MEMORY;
    s->crt_axis = axis;
    prepare_crt_axis(s, sw, sh, dw, dh);
  }

  if (s->config.mode == POCKET_DISPLAY_SHADER_LCD || s->config.mode == POCKET_DISPLAY_SHADER_SCANLINES) {
    uint8_t *phase = grow(s->phase, &s->phase_capacity, map_count);
    if (!phase)
      return POCKET_DISPLAY_SHADER_NO_MEMORY;
    s->phase = phase;
    unsigned parts = s->config.mode == POCKET_DISPLAY_SHADER_LCD ? 3 : 2;
    for (size_t x = 0; x < dw; x++)
      s->phase[x] = map_phase(x, sw, dw, parts);
    for (size_t y = 0; y < dh; y++)
      s->phase[dw + y] = map_phase(y, sh, dh, parts);
  }

  if (needs_color(s)) {
    uint8_t *color = grow(s->color, &s->color_capacity, src_bytes);
    if (!color)
      return POCKET_DISPLAY_SHADER_NO_MEMORY;
    s->color = color;
    colorize(s, src, src_stride, sw, sh);
    src = s->color;
    src_stride = src_row;
  }

  uint8_t *pixels = grow(s->pixels, &s->pixel_capacity, dst_bytes);
  if (!pixels)
    return POCKET_DISPLAY_SHADER_NO_MEMORY;
  s->pixels = pixels;
  render_frame(s, src, src_stride, sw, sh, dw, dh);
  *out = (struct pocket_display_shader_frame){s->pixels, dw, dh, dst_stride};
  return POCKET_DISPLAY_SHADER_OK;
}
