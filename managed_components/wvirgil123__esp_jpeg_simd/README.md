# ESP JPEG SIMD

[![Component Registry](https://components.espressif.com/components/wvirgil123/esp_jpeg_simd/badge.svg)](https://components.espressif.com/components/wvirgil123/esp_jpeg_simd)

## Component Overview

ESP JPEG SIMD is a high-performance JPEG codec component for ESP platforms. It supports multiple pixel formats, rotation, and SIMD optimization, making it suitable for image acquisition and AI vision scenarios.


## Decode API Overview

### Header File
```c
#include "esp_jpeg_dec.h"
```

### Main Structures
- `jpeg_dec_config_t`: Decoder configuration (output format, rotation, etc.)
- `jpeg_dec_io_t`: Input/output buffer descriptor
- `jpeg_dec_header_info_t`: Image header information

### Main Functions
- `jpeg_dec_open`: Create a decoder handle
- `jpeg_dec_parse_header`: Parse JPEG header and get image info
- `jpeg_dec_process`: Perform decoding
- `jpeg_dec_close`: Release decoder resources

## Typical Decode Workflow Example

The following is a typical JPEG decode workflow

```c
#include "esp_jpeg_dec.h"

int esp_jpeg_decoder_one_picture(uint8_t *input_buf, int len, uint8_t *output_buf)
{
    // 1. Configure decoder parameters
    jpeg_dec_config_t config = { .output_type = JPEG_RAW_TYPE_RGB565_BE, .rotate = JPEG_ROTATE_0D };
    jpeg_dec_handle_t jpeg_dec = jpeg_dec_open(&config);
    if (!jpeg_dec) return ESP_FAIL;

    // 2. Allocate IO structures
    jpeg_dec_io_t *jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    jpeg_dec_header_info_t *out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (!jpeg_io || !out_info) return ESP_FAIL;

    jpeg_io->inbuf = input_buf;
    jpeg_io->inbuf_len = len;

    // 3. Parse JPEG header
    int ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret < 0) goto _exit;

    // 4. Set output buffer
    jpeg_io->outbuf = output_buf;
    int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
    jpeg_io->inbuf = input_buf + inbuf_consumed;
    jpeg_io->inbuf_len = jpeg_io->inbuf_remain;

    // 5. Decode
    ret = jpeg_dec_process(jpeg_dec, jpeg_io);

_exit:
    jpeg_dec_close(jpeg_dec);
    free(out_info);
    free(jpeg_io);
    return ret;
}
```

## Reference
- For detailed APIs, see `include/esp_jpeg_dec.h`


