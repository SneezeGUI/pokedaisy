/* Pixel-level tests for the portable display shader; no renderer or emulator required. */
#include "display_shader.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
  return (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | a;
}

static void put(uint8_t *p, size_t stride, size_t x, size_t y, uint32_t c) {
  p += y * stride + x * 4;
  p[0] = (uint8_t)(c >> 24);
  p[1] = (uint8_t)(c >> 16);
  p[2] = (uint8_t)(c >> 8);
  p[3] = (uint8_t)c;
}

static uint32_t at(const struct pocket_display_shader_frame *f, size_t x, size_t y) {
  const uint8_t *p = f->pixels + y * f->stride + x * 4;
  return rgba(p[0], p[1], p[2], p[3]);
}

static uint32_t dim(uint32_t p, unsigned n, unsigned d) {
  unsigned r = ((p >> 24) * n + d / 2) / d;
  unsigned g = (((p >> 16) & 0xff) * n + d / 2) / d;
  unsigned b = (((p >> 8) & 0xff) * n + d / 2) / d;
  return rgba(r, g, b, p & 0xff);
}

static struct pocket_display_shader_frame render(struct pocket_display_shader *s, const void *src, size_t sw,
                                                 size_t sh, size_t stride, size_t dw, size_t dh) {
  struct pocket_display_shader_frame f;
  assert(pocket_display_shader_process(s, src, sw, sh, stride, dw, dh, &f) == POCKET_DISPLAY_SHADER_OK);
  assert(f.pixels && f.width == dw && f.height == dh && f.stride == dw * 4);
  return f;
}

static struct pocket_display_shader *shader(enum pocket_display_shader_mode mode) {
  struct pocket_display_shader_config config = {mode, false};
  struct pocket_display_shader *s = pocket_display_shader_create(config);
  assert(s);
  return s;
}

static void test_nearest_rgba_and_ownership(void) {
  uint8_t src[24], original[24];
  memset(src, 0xcd, sizeof src);
  const uint32_t pixels[] = {rgba(0x11, 0x22, 0x33, 0x44), rgba(0x55, 0x66, 0x77, 0x88),
                             rgba(0x99, 0xaa, 0xbb, 0xcc), rgba(0xdd, 0xee, 0x12, 0x34)};
  put(src, 12, 0, 0, pixels[0]);
  put(src, 12, 1, 0, pixels[1]);
  put(src, 12, 0, 1, pixels[2]);
  put(src, 12, 1, 1, pixels[3]);
  memcpy(original, src, sizeof src);
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_NONE);
  struct pocket_display_shader_frame f = render(s, src, 2, 2, 12, 4, 4);
  assert(!memcmp(src, original, sizeof src));
  for (size_t y = 0; y < 4; y++)
    for (size_t x = 0; x < 4; x++)
      assert(at(&f, x, y) == pixels[(y / 2) * 2 + x / 2]);
  assert(f.pixels[0] == 0x11 && f.pixels[1] == 0x22 && f.pixels[2] == 0x33 && f.pixels[3] == 0x44);

  uint8_t *owned = f.pixels;
  f = render(s, src, 2, 2, 12, 4, 4);
  assert(f.pixels == owned);
  f = render(s, src, 2, 2, 12, 6, 2);
  for (size_t x = 0; x < 6; x++)
    assert(at(&f, x, 1) == pixels[2 + x / 3]);
  f = render(s, src, 2, 2, 12, 1, 1);
  assert(at(&f, 0, 0) == pixels[3]);
  pocket_display_shader_destroy(s);
  puts("PASS nearest RGBA, padded stride, resize, and owned output reuse");
}

static void test_invalid_arguments(void) {
  struct pocket_display_shader_config bad_config = {(enum pocket_display_shader_mode)99, false};
  assert(!pocket_display_shader_create(bad_config));
  pocket_display_shader_destroy(NULL);
  uint8_t src[16] = {0};
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_NONE);
  struct pocket_display_shader_frame f;
  assert(pocket_display_shader_process(s, NULL, 1, 1, 4, 1, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(!f.pixels && !f.width && !f.height && !f.stride);
  assert(pocket_display_shader_process(NULL, src, 1, 1, 4, 1, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 0, 1, 4, 1, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 1, 1, 3, 1, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 1, 1, 4, 0, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 1, 1, 4, POCKET_DISPLAY_SHADER_MAX_DIMENSION + 1, 1, &f) ==
         POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 2, 2, SIZE_MAX, 1, 1, &f) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  assert(pocket_display_shader_process(s, src, 1, 1, 4, 1, 1, NULL) == POCKET_DISPLAY_SHADER_INVALID_ARGUMENT);
  pocket_display_shader_destroy(s);
  puts("PASS invalid modes, nulls, dimensions, strides, and overflow rejection");
}

static void test_lcd_pixels(void) {
  uint8_t src[8];
  uint32_t left = rgba(200, 100, 40, 201), right = rgba(20, 40, 80, 202);
  put(src, 8, 0, 0, left);
  put(src, 8, 1, 0, right);
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_LCD);
  struct pocket_display_shader_frame f = render(s, src, 2, 1, 8, 6, 3);
  for (size_t y = 0; y < 3; y++)
    for (size_t x = 0; x < 6; x++) {
      uint32_t p = x < 3 ? left : right;
      bool grid = y == 0 || x == 0 || x == 3;
      assert(at(&f, x, y) == (grid ? dim(p, 13, 20) : p));
    }
  assert(at(&f, 1, 1) == left && at(&f, 0, 1) == dim(left, 13, 20));
  pocket_display_shader_destroy(s);
  puts("PASS LCD top/left thirds, RGB darkening, source cells, and alpha");
}

static void test_scanline_pixels(void) {
  uint8_t src[8];
  uint32_t top = rgba(200, 100, 40, 201), bottom = rgba(20, 40, 80, 202);
  put(src, 4, 0, 0, top);
  put(src, 4, 0, 1, bottom);
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_SCANLINES);
  struct pocket_display_shader_frame f = render(s, src, 1, 2, 4, 2, 4);
  for (size_t y = 0; y < 4; y++)
    for (size_t x = 0; x < 2; x++) {
      uint32_t p = y < 2 ? top : bottom;
      assert(at(&f, x, y) == (!(y & 1) ? dim(p, 1, 2) : p));
    }
  pocket_display_shader_destroy(s);
  puts("PASS scanline top halves, row mapping, RGB darkening, and alpha");
}

static uint32_t reference_gba(uint32_t p) {
  double in[3] = {pow((p >> 24) / 255.0, 2.7) * 0.99, pow(((p >> 16) & 0xff) / 255.0, 2.7) * 0.99,
                  pow(((p >> 8) & 0xff) / 255.0, 2.7) * 0.99};
  double out[3] = {0.84 * in[0] + 0.18 * in[1], 0.09 * in[0] + 0.67 * in[1] + 0.26 * in[2],
                   0.15 * in[0] + 0.10 * in[1] + 0.73 * in[2]};
  unsigned c[3];
  for (unsigned i = 0; i < 3; i++) {
    if (out[i] < 0)
      out[i] = 0;
    if (out[i] > 1)
      out[i] = 1;
    c[i] = (unsigned)lrint(pow(out[i], 0.4625) * 255.0);
  }
  return rgba(c[0], c[1], c[2], p & 0xff);
}

static unsigned channel_delta(uint32_t a, uint32_t b, unsigned shift) {
  unsigned x = (a >> shift) & 0xff, y = (b >> shift) & 0xff;
  return x > y ? x - y : y - x;
}

static void test_gba_color_pixels(void) {
  const uint32_t colors[] = {rgba(0, 0, 0, 1), rgba(255, 255, 255, 2), rgba(255, 0, 0, 3),
                                    rgba(0, 255, 0, 4), rgba(0, 0, 255, 5), rgba(37, 121, 203, 6)};
  uint8_t src[sizeof colors / sizeof *colors * 4];
  for (size_t i = 0; i < sizeof colors / sizeof *colors; i++)
    put(src, sizeof src, i, 0, colors[i]);
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_GBA_COLOR);
  struct pocket_display_shader_frame f = render(s, src, 6, 1, sizeof src, 6, 1);
  for (size_t i = 0; i < 6; i++) {
    uint32_t actual = at(&f, i, 0), expected = reference_gba(colors[i]);
    for (unsigned shift = 0; shift <= 24; shift += 8)
      assert(channel_delta(actual, expected, shift) <= 2);
    assert((actual & 0xff) == (colors[i] & 0xff));
  }
  struct pocket_display_shader *once = shader(POCKET_DISPLAY_SHADER_GBA_COLOR);
  struct pocket_display_shader_config config = {POCKET_DISPLAY_SHADER_GBA_COLOR, true};
  struct pocket_display_shader *also_once = pocket_display_shader_create(config);
  assert(also_once);
  struct pocket_display_shader_frame a = render(once, src, 6, 1, sizeof src, 12, 2);
  struct pocket_display_shader_frame b = render(also_once, src, 6, 1, sizeof src, 12, 2);
  assert(!memcmp(a.pixels, b.pixels, a.height * a.stride));
  pocket_display_shader_destroy(also_once);
  pocket_display_shader_destroy(once);
  pocket_display_shader_destroy(s);
  puts("PASS GBA colour reference values, matrix/gamma, alpha, and single application");
}

static void test_edge_smooth_pixels(void) {
  uint32_t a = rgba(10, 10, 10, 50), red = rgba(200, 0, 0, 51);
  uint32_t green = rgba(0, 200, 0, 52), blue = rgba(0, 0, 200, 53);
  const uint32_t source[] = {a, red, a, red, green, blue, a, blue, a};
  uint8_t src[36];
  for (size_t y = 0; y < 3; y++)
    for (size_t x = 0; x < 3; x++)
      put(src, 12, x, y, source[y * 3 + x]);
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_EDGE_SMOOTH);
  struct pocket_display_shader_frame f = render(s, src, 3, 3, 12, 6, 6);
  assert(at(&f, 2, 2) == red && at(&f, 3, 2) == green);
  assert(at(&f, 2, 3) == green && at(&f, 3, 3) == blue);
  assert(at(&f, 0, 0) == a && at(&f, 5, 5) == a);

  uint8_t flat[16];
  for (size_t y = 0; y < 2; y++)
    for (size_t x = 0; x < 2; x++)
      put(flat, 8, x, y, rgba(12, 34, 56, 173));
  f = render(s, flat, 2, 2, 8, 5, 5);
  for (size_t y = 0; y < 5; y++)
    for (size_t x = 0; x < 5; x++)
      assert(at(&f, x, y) == rgba(12, 34, 56, 173));
  pocket_display_shader_destroy(s);
  puts("PASS Scale2x edge quadrants, clamped borders, flat colors, resize, and alpha");
}

static uint32_t reference_crt_fetch(const uint32_t *src, size_t w, size_t h, long x, long y) {
  x = x < 0 ? 0 : x >= (long)w ? (long)w - 1 : x;
  y = y < 0 ? 0 : y >= (long)h ? (long)h - 1 : y;
  return src[y * w + x];
}

static double reference_beam(double color, double distance) {
  double width = 0.22 + 0.10 * color;
  return exp(-distance * distance / (2.0 * width * width));
}

static uint32_t reference_crt(const uint32_t *src, size_t sw, size_t sh, size_t dw, size_t dh, size_t x, size_t y) {
  double px = (x + 0.5) * sw / dw - 0.5, py = (y + 0.5) * sh / dh - 0.5;
  long bx = (long)floor(px), by = (long)floor(py);
  double fx = px - bx, fy = py - by;
  fx = (fx - 0.2) / 0.6;
  fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
  fx = fx * fx * (3.0 - 2.0 * fx);
  uint32_t p00 = reference_crt_fetch(src, sw, sh, bx, by);
  uint32_t p10 = reference_crt_fetch(src, sw, sh, bx + 1, by);
  uint32_t p01 = reference_crt_fetch(src, sw, sh, bx, by + 1);
  uint32_t p11 = reference_crt_fetch(src, sw, sh, bx + 1, by + 1);
  unsigned rgb[3];
  for (unsigned channel = 0; channel < 3; channel++) {
    unsigned shift = 24 - channel * 8;
    double a = pow(((p00 >> shift) & 0xff) / 255.0, 2.2);
    double b = pow(((p10 >> shift) & 0xff) / 255.0, 2.2);
    double c = pow(((p01 >> shift) & 0xff) / 255.0, 2.2);
    double d = pow(((p11 >> shift) & 0xff) / 255.0, 2.2);
    double row0 = a + (b - a) * fx, row1 = c + (d - c) * fx;
    double linear = row0 * reference_beam(row0, fy) + row1 * reference_beam(row1, 1.0 - fy);
    linear *= (x % 3 == channel ? 1.0 : 0.6) * 1.5;
    linear = linear < 0 ? 0 : linear > 1 ? 1 : linear;
    rgb[channel] = (unsigned)lrint(pow(linear, 1.0 / 2.2) * 255.0);
  }
  size_t sx = (2 * x + 1) * sw / (2 * dw), sy = (2 * y + 1) * sh / (2 * dh);
  return rgba(rgb[0], rgb[1], rgb[2], src[sy * sw + sx] & 0xff);
}

static void test_crt_pixels(void) {
  uint8_t src[4];
  put(src, 4, 0, 0, rgba(255, 255, 255, 123));
  struct pocket_display_shader *s = shader(POCKET_DISPLAY_SHADER_CRT);
  struct pocket_display_shader_frame f = render(s, src, 1, 1, 4, 3, 3);
  double bloom = 1.0 + exp(-1.0 / (2.0 * 0.32 * 0.32));
  unsigned off = (unsigned)lrint(pow(bloom * 0.6 * 1.5, 1.0 / 2.2) * 255.0);
  const uint32_t expected[] = {rgba(255, off, off, 123), rgba(off, 255, off, 123), rgba(off, off, 255, 123)};
  for (size_t x = 0; x < 3; x++) {
    uint32_t actual = at(&f, x, 1);
    for (unsigned shift = 0; shift <= 24; shift += 8)
      assert(channel_delta(actual, expected[x], shift) <= 2);
  }
  for (size_t y = 0; y < 3; y++)
    for (size_t x = 0; x < 3; x++)
      assert((at(&f, x, y) & 0xff) == 123);

  const uint32_t colors[] = {rgba(230, 20, 10, 173), rgba(15, 190, 45, 173), rgba(25, 60, 220, 173),
                             rgba(180, 160, 90, 173)};
  uint8_t sample[16];
  for (size_t y = 0; y < 2; y++)
    for (size_t x = 0; x < 2; x++)
      put(sample, 8, x, y, colors[y * 2 + x]);
  f = render(s, sample, 2, 2, 8, 5, 4);
  for (size_t y = 0; y < 4; y++)
    for (size_t x = 0; x < 5; x++) {
      uint32_t actual = at(&f, x, y), reference = reference_crt(colors, 2, 2, 5, 4, x, y);
      for (unsigned shift = 0; shift <= 24; shift += 8)
        assert(channel_delta(actual, reference, shift) <= 4);
      assert((actual & 0xff) == 173);
    }
  pocket_display_shader_destroy(s);
  puts("PASS CRT linear beams, RGB mask, full fractional-scale reference, clamping, and alpha");
}

static void test_determinism_and_input_ownership(void) {
  uint8_t src[32], original[32];
  for (size_t i = 0; i < sizeof src; i++)
    src[i] = (uint8_t)(i * 37 + 11);
  for (size_t y = 0; y < 2; y++)
    for (size_t x = 0; x < 3; x++)
      src[y * 16 + x * 4 + 3] = 173;
  memcpy(original, src, sizeof src);
  for (unsigned mode = 0; mode <= POCKET_DISPLAY_SHADER_EDGE_SMOOTH; mode++) {
    struct pocket_display_shader_config config = {(enum pocket_display_shader_mode)mode, mode & 1};
    struct pocket_display_shader *s = pocket_display_shader_create(config);
    assert(s);
    struct pocket_display_shader_frame f = render(s, src, 3, 2, 16, 9, 6);
    size_t bytes = f.height * f.stride;
    uint8_t *copy = malloc(bytes);
    assert(copy);
    memcpy(copy, f.pixels, bytes);
    f = render(s, src, 3, 2, 16, 9, 6);
    assert(!memcmp(copy, f.pixels, bytes));
    assert(!memcmp(src, original, sizeof src));
    for (size_t y = 0; y < f.height; y++)
      for (size_t x = 0; x < f.width; x++)
        assert(f.pixels[y * f.stride + x * 4 + 3] == 173);
    free(copy);
    pocket_display_shader_destroy(s);
  }
  puts("PASS every mode deterministic, composable with GBA colour, and input read-only");
}

static void benchmark(void) {
  const size_t sw = 240, sh = 160;
  uint8_t *src = malloc(sw * sh * 4);
  assert(src);
  for (size_t i = 0; i < sw * sh; i++) {
    src[i * 4] = (uint8_t)(i * 3);
    src[i * 4 + 1] = (uint8_t)(i * 5);
    src[i * 4 + 2] = (uint8_t)(i * 7);
    src[i * 4 + 3] = 255;
  }
  static const char *const names[] = {"none", "lcd", "scanlines", "crt", "gba-color", "edge-smooth"};
  const size_t sizes[][2] = {{480, 320}, {960, 640}, {1920, 1080}};
  for (size_t size = 0; size < 3; size++) {
    for (unsigned mode = 0; mode <= POCKET_DISPLAY_SHADER_EDGE_SMOOTH; mode++) {
      struct pocket_display_shader *s = shader((enum pocket_display_shader_mode)mode);
      unsigned iterations = size == 0 ? 20 : size == 1 ? 10 : 5;
      clock_t start = clock();
      struct pocket_display_shader_frame f = {0};
      for (unsigned i = 0; i < iterations; i++)
        f = render(s, src, sw, sh, sw * 4, sizes[size][0], sizes[size][1]);
      double ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC / iterations;
      printf("BENCH %ux%u %-11s %.3f ms/frame (last pixel %02x%02x%02x%02x)\n", (unsigned)f.width,
             (unsigned)f.height, names[mode], ms, f.pixels[f.height * f.stride - 4], f.pixels[f.height * f.stride - 3],
             f.pixels[f.height * f.stride - 2], f.pixels[f.height * f.stride - 1]);
      pocket_display_shader_destroy(s);
    }
  }
  free(src);
}

int main(int argc, char **argv) {
  assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--benchmark")));
  test_nearest_rgba_and_ownership();
  test_invalid_arguments();
  test_lcd_pixels();
  test_scanline_pixels();
  test_gba_color_pixels();
  test_edge_smooth_pixels();
  test_crt_pixels();
  test_determinism_and_input_ownership();
  puts("PASS all 8 display-shader test groups");
  if (argc == 2)
    benchmark();
  return 0;
}
