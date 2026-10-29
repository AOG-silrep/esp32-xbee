#ifndef ESP32_XBEE_WWW_FILES_H
#define ESP32_XBEE_WWW_FILES_H

#include <stddef.h>
#include <stdint.h>

// Web UI file from www/, gzipped into the firmware at build time by tools/www_embed.py
typedef struct {
    const char *path; // URI path, e.g. "/index.html"
    const uint8_t *data;
    size_t size;
    const char *etag; // Quoted CRC32 of the gzipped data
} www_file_t;

extern const www_file_t www_files[];
extern const size_t www_files_count;

#endif //ESP32_XBEE_WWW_FILES_H
