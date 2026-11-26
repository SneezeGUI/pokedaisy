/* GPL-3.0. Portable, output-space display effects over packed RGBA pixels. */
#ifndef POCKET_DISPLAY_SHADER_H
#define POCKET_DISPLAY_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_DISPLAY_SHADER_MAX_DIMENSION 16384u

enum pocket_display_shader_mode {
  POCKET_DISPLAY_SHADER_NONE = 0,
  POCKET_DISPLAY_SHADER_LCD,
  POCKET_DISPLAY_SHADER_SCANLINES,
  POCKET_DISPLAY_SHADER_CRT,
  POCKET_DISPLAY_SHADER_GBA_COLOR,
  POCKET_DISPLAY_SHADER_EDGE_SMOOTH
};

struct pocket_display_shader_config {
  enum pocket_display_shader_mode mode;
  /* Apply the GBA colour transform before the selected mode. GBA_COLOR never applies it twice. */
  bool gba_colors;
};

/* pixels are R, G, B, A bytes, top row first. stride is bytes, not pixels. */
struct pocket_display_shader_frame {
  uint8_t *pixels;
  size_t width;
  size_t height;
  size_t stride;
};

enum pocket_display_shader_result {
  POCKET_DISPLAY_SHADER_OK = 0,
  POCKET_DISPLAY_SHADER_INVALID_ARGUMENT,
  POCKET_DISPLAY_SHADER_NO_MEMORY
};

struct pocket_display_shader;

struct pocket_display_shader *pocket_display_shader_create(struct pocket_display_shader_config config);
void pocket_display_shader_destroy(struct pocket_display_shader *shader);

/* Resize and render into storage owned by shader. The input is read synchronously and never retained.
 * src_stride includes any row padding. Output is tightly packed and has the requested dimensions.
 * On success, out remains valid until the next process call or destroy. On failure, out is zeroed.
 * Input must not overlap shader's current output. Rendering may upscale or downscale. */
enum pocket_display_shader_result
pocket_display_shader_process(struct pocket_display_shader *shader, const void *src, size_t src_width,
                              size_t src_height, size_t src_stride, size_t dst_width, size_t dst_height,
                              struct pocket_display_shader_frame *out);

#ifdef __cplusplus
}
#endif
#endif
