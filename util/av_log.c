/**
 * @file
 * logging functions
 */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "av_log.h"

#if HAVE_PTHREADS
#include <pthread.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#define av_bprint_room(buf) ((buf)->size - FFMIN((buf)->len, (buf)->size))
#define av_bprint_is_allocated(buf) ((buf)->str != (buf)->reserved_internal_buffer)
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))

#define LINE_SZ 1024

static int av_log_level = AV_LOG_INFO;
static int flags;

#if defined(_WIN32)
#include <windows.h>
static const uint8_t color[16] =
{
    [AV_LOG_PANIC  / 8] = 12,
    [AV_LOG_FATAL  / 8] = 12,
    [AV_LOG_ERROR  / 8] = 12,
    [AV_LOG_WARNING / 8] = 14,
    [AV_LOG_INFO   / 8] =  7,
    [AV_LOG_VERBOSE / 8] = 10,
    [AV_LOG_DEBUG  / 8] = 10,
};

static int16_t background, attr_orig;
static HANDLE con;
#else
static const uint32_t color[16] =
{
    [AV_LOG_PANIC  / 8] =  52 << 16 | 196 << 8 | 0x41,
    [AV_LOG_FATAL  / 8] = 208 <<  8 | 0x41,
    [AV_LOG_ERROR  / 8] = 196 <<  8 | 0x11,
    [AV_LOG_WARNING / 8] = 226 <<  8 | 0x03,
    [AV_LOG_INFO   / 8] = 253 <<  8 | 0x09,
    [AV_LOG_VERBOSE / 8] =  40 <<  8 | 0x02,
    [AV_LOG_DEBUG  / 8] =  34 <<  8 | 0x02,
};
#endif
static int use_color = -1;

/**
* Clip a signed integer value into the amin-amax range.
* @param a value to clip
* @param amin minimum value of the clip range
* @param amax maximum value of the clip range
* @return clipped value
*/
static int av_clip(int a, int amin, int amax)
{
	if (a < amin) return amin;
	else if (a > amax) return amax;
	else               return a;
}

static int av_bprint_alloc(AVBPrint * buf, unsigned room)
{
	char * old_str, *new_str;
	unsigned min_size, new_size;

	if (buf->size == buf->size_max)
		return -1;
	if (!av_bprint_is_complete(buf))
		return -1; /* it is already truncated anyway */
	min_size = buf->len + 1 + FFMIN(UINT_MAX - buf->len - 1, room);
	new_size = buf->size > buf->size_max / 2 ? buf->size_max : buf->size * 2;
	if (new_size < min_size)
		new_size = FFMIN(buf->size_max, min_size);
	old_str = av_bprint_is_allocated(buf) ? buf->str : NULL;
	new_str = realloc(old_str, new_size);
	if (!new_str)
		return -1;
	if (!old_str)
		memcpy(new_str, buf->str, buf->len + 1);
	buf->str = new_str;
	buf->size = new_size;
	return 0;
}

static void av_bprint_grow(AVBPrint * buf, unsigned extra_len)
{
	/* arbitrary margin to avoid small overflows */
	extra_len = FFMIN(extra_len, UINT_MAX - 5 - buf->len);
	buf->len += extra_len;
	if (buf->size)
		buf->str[FFMIN(buf->len, buf->size - 1)] = 0;
}

void av_bprint_init(AVBPrint * buf, unsigned size_init, unsigned size_max)
{
	unsigned size_auto = (char *)buf + sizeof(*buf) -
		buf->reserved_internal_buffer;

	if (size_max == 1)
		size_max = size_auto;
	buf->str = buf->reserved_internal_buffer;
	buf->len = 0;
	buf->size = FFMIN(size_auto, size_max);
	buf->size_max = size_max;
	*buf->str = 0;
	if (size_init > buf->size)
		av_bprint_alloc(buf, size_init - 1);
}

void av_bprint_init_for_buffer(AVBPrint * buf, char * buffer, unsigned size)
{
	buf->str = buffer;
	buf->len = 0;
	buf->size = size;
	buf->size_max = size;
	*buf->str = 0;
}

void av_bprintf(AVBPrint * buf, const char * fmt, ...)
{
	unsigned room;
	char * dst;
	va_list vl;
	int extra_len;

	while (1)
	{
		room = av_bprint_room(buf);
		dst = room ? buf->str + buf->len : NULL;
		va_start(vl, fmt);
		extra_len = vsnprintf(dst, room, fmt, vl);
		va_end(vl);
		if (extra_len <= 0)
			return;
		if ((unsigned)extra_len < room)
			break;
		if (av_bprint_alloc(buf, extra_len))
			break;
	}
	av_bprint_grow(buf, extra_len);
}

void av_vbprintf(AVBPrint * buf, const char * fmt, va_list vl_arg)
{
	unsigned room;
	char * dst;
	int extra_len;
	va_list vl;

	while (1)
	{
		room = av_bprint_room(buf);
		dst = room ? buf->str + buf->len : NULL;
		va_copy(vl, vl_arg);
		extra_len = vsnprintf(dst, room, fmt, vl);
		va_end(vl);
		if (extra_len <= 0)
			return;
		if ((unsigned)extra_len < room)
			break;
		if (av_bprint_alloc(buf, extra_len))
			break;
	}
	av_bprint_grow(buf, extra_len);
}

void av_bprint_chars(AVBPrint * buf, char c, unsigned n)
{
	unsigned room, real_n;

	while (1)
	{
		room = av_bprint_room(buf);
		if (n < room)
			break;
		if (av_bprint_alloc(buf, n))
			break;
	}
	if (room)
	{
		real_n = FFMIN(n, room - 1);
		memset(buf->str + buf->len, c, real_n);
	}
	av_bprint_grow(buf, n);
}

void av_bprint_append_data(AVBPrint * buf, const char * data, unsigned size)
{
	unsigned room, real_n;

	while (1)
	{
		room = av_bprint_room(buf);
		if (size < room)
			break;
		if (av_bprint_alloc(buf, size))
			break;
	}
	if (room)
	{
		real_n = FFMIN(size, room - 1);
		memcpy(buf->str + buf->len, data, real_n);
	}
	av_bprint_grow(buf, size);
}

void av_bprint_strftime(AVBPrint * buf, const char * fmt, const struct tm * tm)
{
	unsigned room;
	size_t l;

	if (!*fmt)
		return;
	while (1)
	{
		room = av_bprint_room(buf);
		if (room && (l = strftime(buf->str + buf->len, room, fmt, tm)))
			break;
		/* strftime does not tell us how much room it would need: let us
		retry with twice as much until the buffer is large enough */
		room = !room ? strlen(fmt) + 1 :
			room <= INT_MAX / 2 ? room * 2 : INT_MAX;
		if (av_bprint_alloc(buf, room))
		{
			/* impossible to grow, try to manage something useful anyway */
			room = av_bprint_room(buf);
			if (room < 1024)
			{
				/* if strftime fails because the buffer has (almost) reached
				its maximum size, let us try in a local buffer; 1k should
				be enough to format any real date+time string */
				char buf2[1024];
				if ((l = strftime(buf2, sizeof(buf2), fmt, tm)))
				{
					av_bprintf(buf, "%s", buf2);
					return;
				}
			}
			if (room)
			{
				/* if anything else failed and the buffer is not already
				truncated, let us add a stock string and force truncation */
				static const char txt[] = "[truncated strftime output]";
				memset(buf->str + buf->len, '!', room);
				memcpy(buf->str + buf->len, txt, FFMIN(sizeof(txt) - 1, room));
				av_bprint_grow(buf, room); /* force truncation */
			}
			return;
		}
	}
	av_bprint_grow(buf, l);
}

void av_bprint_get_buffer(AVBPrint * buf, unsigned size,
	unsigned char ** mem, unsigned * actual_size)
{
	if (size > av_bprint_room(buf))
		av_bprint_alloc(buf, size);
	*actual_size = av_bprint_room(buf);
	*mem = *actual_size ? buf->str + buf->len : NULL;
}

void av_bprint_clear(AVBPrint * buf)
{
	if (buf->len)
	{
		*buf->str = 0;
		buf->len = 0;
	}
}

int av_bprint_finalize(AVBPrint * buf, char ** ret_str)
{
	unsigned real_size = FFMIN(buf->len + 1, buf->size);
	char * str;
	int ret = 0;

	if (ret_str)
	{
		if (av_bprint_is_allocated(buf))
		{
			str = realloc(buf->str, real_size);
			if (!str)
				str = buf->str;
			buf->str = NULL;
		}
		else
		{
			str = malloc(real_size);
			if (str)
				memcpy(str, buf->str, real_size);
			else
				ret = -ENOMEM;
		}
		*ret_str = str;
	}
	else
	{
		if (av_bprint_is_allocated(buf))
			free(buf->str);
	}
	buf->size = real_size;
	return ret;
}

static void check_color_terminal(void)
{
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO con_info;
    con = GetStdHandle(STD_ERROR_HANDLE);
    use_color = (con != INVALID_HANDLE_VALUE) && !getenv("NO_COLOR") &&
                !getenv("AV_LOG_FORCE_NOCOLOR");
    if (use_color)
    {
        GetConsoleScreenBufferInfo(con, &con_info);
        attr_orig  = con_info.wAttributes;
        background = attr_orig & 0xF0;
    }
#elif HAVE_ISATTY
    char * term = getenv("TERM");
    use_color = !getenv("NO_COLOR") && !getenv("AV_LOG_FORCE_NOCOLOR") &&
                (getenv("TERM") && isatty(2) || getenv("AV_LOG_FORCE_COLOR"));
    if (getenv("AV_LOG_FORCE_256COLOR")
            || (term && strstr(term, "256color")))
        use_color *= 256;
#else
    use_color = getenv("AV_LOG_FORCE_COLOR") && !getenv("NO_COLOR") &&
                !getenv("AV_LOG_FORCE_NOCOLOR");
#endif
}

static void colored_fputs(int level, int tint, const char * str)
{
    int local_use_color;
    if (!*str)
        return;

    if (use_color < 0)
        check_color_terminal();

    if (level == AV_LOG_INFO / 8) 
		local_use_color = 0;
    else 
		local_use_color = use_color;

#if defined(_WIN32)
    if (local_use_color)
        SetConsoleTextAttribute(con, background | color[level]);
    fputs(str, stderr);
    if (local_use_color)
        SetConsoleTextAttribute(con, attr_orig);
#else
    if (local_use_color == 1)
    {
        fprintf(stderr,
                "\033[%d;3%dm%s\033[0m",
                (color[level] >> 4) & 15,
                color[level] & 15,
                str);
    }
    else if (tint && use_color == 256)
    {
        fprintf(stderr,
                "\033[48;5;%dm\033[38;5;%dm%s\033[0m",
                (color[level] >> 16) & 0xff,
                tint,
                str);
    }
    else if (local_use_color == 256)
    {
        fprintf(stderr,
                "\033[48;5;%dm\033[38;5;%dm%s\033[0m",
                (color[level] >> 16) & 0xff,
                (color[level] >> 8) & 0xff,
                str);
    }
    else
        fputs(str, stderr);
#endif

}

static void sanitize(uint8_t * line)
{
    while (*line)
    {
        if (*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line = '?';
        line++;
    }
}

static const char * get_level_str(int level)
{
    switch (level)
    {
        case AV_LOG_QUIET:
            return "quiet";
        case AV_LOG_DEBUG:
            return "debug";
        case AV_LOG_VERBOSE:
            return "verbose";
        case AV_LOG_INFO:
            return "info";
        case AV_LOG_WARNING:
            return "warning";
        case AV_LOG_ERROR:
            return "error";
        case AV_LOG_FATAL:
            return "fatal";
        case AV_LOG_PANIC:
            return "panic";
        default:
            return "";
    }
}

static void format_line(int level, const char * fmt, va_list vl,
                        AVBPrint part[4], int * print_prefix, int type[2])
{    
    av_bprint_init(part + 0, 0, 1);
    av_bprint_init(part + 1, 0, 1);
    av_bprint_init(part + 2, 0, 1);
    av_bprint_init(part + 3, 0, 65536);

    if (type) type[0] = type[1] = 16;

    av_vbprintf(part + 3, fmt, vl);

    if (*part[0].str || *part[1].str || *part[2].str || *part[3].str)
    {
        char lastc = part[3].len && part[3].len <= part[3].size ? part[3].str[part[3].len - 1] : 0;
        *print_prefix = lastc == '\n' || lastc == '\r';
    }
}

void av_log_format_line(int level, const char * fmt, va_list vl,
                        char * line, int line_size, int * print_prefix)
{
    AVBPrint part[4];
    format_line(level, fmt, vl, part, print_prefix, NULL);
    snprintf(line, line_size, "%s%s%s%s", part[0].str, part[1].str, part[2].str, part[3].str);
    av_bprint_finalize(part + 3, NULL);
}

void av_log_default_callback(int level, const char * fmt, va_list vl)
{
    static int print_prefix = 1;
    static int count;
    static char prev[LINE_SZ];
    AVBPrint part[4];
    char line[LINE_SZ];
    static int is_atty;
    int type[2];
    unsigned tint = 0;

    if (level >= 0)
    {
        tint = level & 0xff00;
        level &= 0xff;
    }

    if (level > av_log_level)
        return;
#if HAVE_PTHREADS
    pthread_mutex_lock(&mutex);
#endif

    format_line(level, fmt, vl, part, &print_prefix, type);
    snprintf(line, sizeof(line), "%s%s%s%s", part[0].str, part[1].str, part[2].str, part[3].str);

#if HAVE_ISATTY
    if (!is_atty)
        is_atty = isatty(2) ? 1 : -1;
#endif

    if (print_prefix && (flags & AV_LOG_SKIP_REPEATED) && !strcmp(line, prev) &&
            *line && line[strlen(line) - 1] != '\r')
    {
        count++;
        if (is_atty == 1)
            fprintf(stderr, "    Last message repeated %d times\r", count);
        goto end;
    }
    if (count > 0)
    {
        fprintf(stderr, "    Last message repeated %d times\n", count);
        count = 0;
    }
    strcpy(prev, line);
    sanitize(part[0].str);
    colored_fputs(type[0], 0, part[0].str);
    sanitize(part[1].str);
    colored_fputs(type[1], 0, part[1].str);
    sanitize(part[2].str);
    colored_fputs(av_clip(level >> 3, 0, 6), tint >> 8, part[2].str);
    sanitize(part[3].str);
    colored_fputs(av_clip(level >> 3, 0, 6), tint >> 8, part[3].str);
end:
    av_bprint_finalize(part + 3, NULL);
#if HAVE_PTHREADS
    pthread_mutex_unlock(&mutex);
#endif
}

static void (*av_log_callback)(int, const char *, va_list) = av_log_default_callback;

void av_log(int level, const char * fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    av_vlog(level, fmt, vl);
    va_end(vl);
}

void av_vlog(int level, const char * fmt, va_list vl)
{
    void (*log_callback)(int, const char *, va_list) = av_log_callback;
    if (log_callback)
        log_callback(level, fmt, vl);
}

int av_log_get_level(void)
{
    return av_log_level;
}

void av_log_set_level(int level)
{
    av_log_level = level;
}

void av_log_set_callback(void (*callback)(int, const char *, va_list))
{
    av_log_callback = callback;
}

#define HEXDUMP_PRINT(...)                      \
    do {                                        \
        if (!f)                                 \
            av_log(level, __VA_ARGS__);   \
        else                                    \
            fprintf(f, __VA_ARGS__);            \
    } while (0)

static void hex_dump_internal(FILE * f, int level,
	const uint8_t * buf, int size)
{
	int len, i, j;

	for (i = 0; i < size; i += 16)
	{
		len = size - i;
		if (len > 16)
			len = 16;
		HEXDUMP_PRINT("%08x ", i);
		for (j = 0; j < 16; j++)
		{
			if (j < len)
				HEXDUMP_PRINT(" %02x", buf[i + j]);
			else
				HEXDUMP_PRINT("   ");
		}
		HEXDUMP_PRINT(" ");
		/*for (j = 0; j < len; j++)
		{
		c = buf[i + j];
		if (c < ' ' || c > '~')
		c = '.';
		HEXDUMP_PRINT("%c", c);
		}*/
		HEXDUMP_PRINT("\n");
	}
}

void av_hex_dump(void * f, const char * buf, int size)
{
	hex_dump_internal(f, 0, buf, size);
}