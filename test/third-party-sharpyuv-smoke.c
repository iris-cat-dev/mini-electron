#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sharpyuv/sharpyuv.h"
#include "sharpyuv/sharpyuv_csp.h"
#include "src/webp/decode.h"
#include "src/webp/encode.h"

#define REQUIRE(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
  } \
} while (0)

static void CheckConstantConversion(uint8_t value, uint8_t expected_y) {
  uint8_t rgb[5 * 7 * 3];
  uint8_t y[5 * 7], u[3 * 4], v[3 * 4];
  const SharpYuvConversionMatrix* matrix =
      SharpYuvGetConversionMatrix(kSharpYuvMatrixWebp);
  REQUIRE(matrix != NULL);
  memset(rgb, value, sizeof(rgb));
  REQUIRE(SharpYuvConvert(rgb, rgb + 1, rgb + 2, 3, 5 * 3, 8,
                         y, 5, u, 3, v, 3, 8, 5, 7, matrix));
  for (size_t i = 0; i < sizeof(y); ++i) REQUIRE(y[i] == expected_y);
  for (size_t i = 0; i < sizeof(u); ++i) {
    REQUIRE(u[i] == 128);
    REQUIRE(v[i] == 128);
  }
}

static void CheckEncoder(void) {
  uint8_t rgb[5 * 7 * 3];
  WebPConfig config;
  WebPPicture picture;
  WebPMemoryWriter writer;
  int width = 0, height = 0;
  uint8_t* decoded;
  REQUIRE(WebPConfigInit(&config));
  REQUIRE(WebPPictureInit(&picture));
  WebPMemoryWriterInit(&writer);
  config.quality = 100;
  config.use_sharp_yuv = 1;
  picture.width = 5;
  picture.height = 7;
  picture.use_argb = 1;
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;
  for (size_t i = 0; i < sizeof(rgb); i += 3) {
    rgb[i] = (uint8_t)(i * 7);
    rgb[i + 1] = (uint8_t)(i * 13);
    rgb[i + 2] = (uint8_t)(i * 19);
  }
  REQUIRE(WebPPictureImportRGB(&picture, rgb, 5 * 3));
  REQUIRE(WebPEncode(&config, &picture));
  REQUIRE(writer.size > 0);
  decoded = WebPDecodeRGB(writer.mem, writer.size, &width, &height);
  REQUIRE(decoded != NULL);
  REQUIRE(width == 5 && height == 7);
  WebPFree(decoded);
  WebPPictureFree(&picture);
  WebPMemoryWriterClear(&writer);
}

int main(void) {
  CheckConstantConversion(0, 16);
  CheckConstantConversion(255, 235);
  CheckEncoder();
  puts("SharpYUV conversion and WebP encode/decode passed");
  return 0;
}
