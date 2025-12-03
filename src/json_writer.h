/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef JSON_WRITER_H_
#define JSON_WRITER_H_

#include <mupdf/fitz.h>
#include <stdbool.h>

typedef struct json_writer json_writer;

/* Create a new JSON writer that appends to the given buffer */
json_writer *json_writer_new(fz_context *ctx, fz_buffer *buf);

/* Free the JSON writer */
void json_writer_drop(fz_context *ctx, json_writer *writer);

/* Write object start { */
void json_write_object_start(fz_context *ctx, json_writer *writer);

/* Write object end } */
void json_write_object_end(fz_context *ctx, json_writer *writer);

/* Write array start [ */
void json_write_array_start(fz_context *ctx, json_writer *writer);

/* Write array end ] */
void json_write_array_end(fz_context *ctx, json_writer *writer);

/* Write a key (for object properties) */
void json_write_key(fz_context *ctx, json_writer *writer, const char *key);

/* Write a string value (with quotes and escaping) */
void json_write_string(fz_context *ctx, json_writer *writer, const char *value);

/* Write an integer value */
void json_write_int(fz_context *ctx, json_writer *writer, int value);

/* Write a float value */
void json_write_float(fz_context *ctx, json_writer *writer, float value);

/* Write a boolean value */
void json_write_bool(fz_context *ctx, json_writer *writer, bool value);

/* Write null */
void json_write_null(fz_context *ctx, json_writer *writer);

/* Write a matrix (6 floats as array) */
void json_write_matrix(fz_context *ctx, json_writer *writer, fz_matrix m);

/* Write RGB color (3 floats as array) */
void json_write_color_rgb(fz_context *ctx, json_writer *writer, float r, float g, float b);

/* Flush the buffer and write a newline (for line-delimited JSON) */
void json_writer_flush_line(fz_context *ctx, json_writer *writer);

#endif // JSON_WRITER_H_
