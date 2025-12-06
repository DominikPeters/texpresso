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

#include "json_writer.h"
#include <stdio.h>
#include <string.h>

struct json_writer {
    fz_context *ctx;
    fz_buffer *buf;
    int depth;
    bool needs_comma;
    bool at_key;
};

json_writer *json_writer_new(fz_context *ctx, fz_buffer *buf)
{
    json_writer *writer = fz_malloc_struct(ctx, json_writer);
    writer->ctx = ctx;
    writer->buf = buf;
    writer->depth = 0;
    writer->needs_comma = false;
    writer->at_key = false;
    return writer;
}

void json_writer_drop(fz_context *ctx, json_writer *writer)
{
    fz_free(ctx, writer);
}

static void write_comma_if_needed(json_writer *writer)
{
    if (writer->needs_comma)
    {
        fz_append_byte(writer->ctx, writer->buf, ',');
        writer->needs_comma = false;
    }
}

static void write_separator(json_writer *writer)
{
    if (writer->at_key)
    {
        fz_append_byte(writer->ctx, writer->buf, ':');
        writer->at_key = false;
    }
    else
    {
        write_comma_if_needed(writer);
    }
}

void json_write_object_start(fz_context *ctx, json_writer *writer)
{
    write_separator(writer);
    fz_append_byte(ctx, writer->buf, '{');
    writer->depth++;
    writer->needs_comma = false;
}

void json_write_object_end(fz_context *ctx, json_writer *writer)
{
    fz_append_byte(ctx, writer->buf, '}');
    writer->depth--;
    writer->needs_comma = true;
}

void json_write_array_start(fz_context *ctx, json_writer *writer)
{
    write_separator(writer);
    fz_append_byte(ctx, writer->buf, '[');
    writer->depth++;
    writer->needs_comma = false;
}

void json_write_array_end(fz_context *ctx, json_writer *writer)
{
    fz_append_byte(ctx, writer->buf, ']');
    writer->depth--;
    writer->needs_comma = true;
}

void json_write_key(fz_context *ctx, json_writer *writer, const char *key)
{
    write_comma_if_needed(writer);
    fz_append_byte(ctx, writer->buf, '"');
    fz_append_string(ctx, writer->buf, key);
    fz_append_byte(ctx, writer->buf, '"');
    writer->at_key = true;
}

static void write_escaped_string(fz_context *ctx, fz_buffer *buf, const char *str)
{
    while (*str)
    {
        unsigned char c = *str++;
        switch (c)
        {
            case '"':  fz_append_string(ctx, buf, "\\\""); break;
            case '\\': fz_append_string(ctx, buf, "\\\\"); break;
            case '\b': fz_append_string(ctx, buf, "\\b"); break;
            case '\f': fz_append_string(ctx, buf, "\\f"); break;
            case '\n': fz_append_string(ctx, buf, "\\n"); break;
            case '\r': fz_append_string(ctx, buf, "\\r"); break;
            case '\t': fz_append_string(ctx, buf, "\\t"); break;
            default:
                if (c < 32)
                    fz_append_printf(ctx, buf, "\\u%04x", c);
                else
                    fz_append_byte(ctx, buf, c);
                break;
        }
    }
}

void json_write_string(fz_context *ctx, json_writer *writer, const char *value)
{
    write_separator(writer);
    fz_append_byte(ctx, writer->buf, '"');
    write_escaped_string(ctx, writer->buf, value);
    fz_append_byte(ctx, writer->buf, '"');
    writer->needs_comma = true;
}

void json_write_int(fz_context *ctx, json_writer *writer, int value)
{
    write_separator(writer);
    fz_append_printf(ctx, writer->buf, "%d", value);
    writer->needs_comma = true;
}

void json_write_float(fz_context *ctx, json_writer *writer, float value)
{
    write_separator(writer);
    // Use %f format to ensure leading zero for values like 0.5 (not .5)
    // JSON requires the leading zero
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", value);
    // If the number starts with '.' or '-.' we need to add a leading zero
    if (buf[0] == '.')
    {
        fz_append_byte(ctx, writer->buf, '0');
        fz_append_string(ctx, writer->buf, buf);
    }
    else if (buf[0] == '-' && buf[1] == '.')
    {
        fz_append_byte(ctx, writer->buf, '-');
        fz_append_byte(ctx, writer->buf, '0');
        fz_append_string(ctx, writer->buf, buf + 1);
    }
    else
    {
        fz_append_string(ctx, writer->buf, buf);
    }
    writer->needs_comma = true;
}

void json_write_bool(fz_context *ctx, json_writer *writer, bool value)
{
    write_separator(writer);
    fz_append_string(ctx, writer->buf, value ? "true" : "false");
    writer->needs_comma = true;
}

void json_write_null(fz_context *ctx, json_writer *writer)
{
    write_separator(writer);
    fz_append_string(ctx, writer->buf, "null");
    writer->needs_comma = true;
}

void json_write_matrix(fz_context *ctx, json_writer *writer, fz_matrix m)
{
    json_write_array_start(ctx, writer);
    json_write_float(ctx, writer, m.a);
    json_write_float(ctx, writer, m.b);
    json_write_float(ctx, writer, m.c);
    json_write_float(ctx, writer, m.d);
    json_write_float(ctx, writer, m.e);
    json_write_float(ctx, writer, m.f);
    json_write_array_end(ctx, writer);
}

void json_write_color_rgb(fz_context *ctx, json_writer *writer, float r, float g, float b)
{
    json_write_array_start(ctx, writer);
    json_write_float(ctx, writer, r);
    json_write_float(ctx, writer, g);
    json_write_float(ctx, writer, b);
    json_write_array_end(ctx, writer);
}

void json_writer_flush_line(fz_context *ctx, json_writer *writer)
{
    fz_append_byte(ctx, writer->buf, '\n');
    fz_write_buffer(ctx, fz_stdout(ctx), writer->buf);
    fz_flush_output(ctx, fz_stdout(ctx));
    fz_clear_buffer(ctx, writer->buf);
    writer->needs_comma = false;
    writer->at_key = false;
    writer->depth = 0;
}
