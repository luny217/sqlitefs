#ifndef AVUTIL_LOG_H
#define AVUTIL_LOG_H

#include <stdarg.h>

#ifdef __GNUC__
#    define av_builtin_constant_p __builtin_constant_p
#    define av_printf_format(fmtpos, attrpos) __attribute__((__format__(__printf__, fmtpos, attrpos)))
#else
#    define av_builtin_constant_p(x) 0
#    define av_printf_format(fmtpos, attrpos)
#endif

typedef struct AVBPrint
{
	char *str;         /**< string so far */
	unsigned len;      /**< length so far */
	unsigned size;     /**< allocated memory */
	unsigned size_max; /**< maximum allocated memory */
	char reserved_internal_buffer[1];
} AVBPrint;

/**
* Convenience macros for special values for av_bprint_init() size_max
* parameter.
*/
#define AV_BPRINT_SIZE_UNLIMITED  ((unsigned)-1)
#define AV_BPRINT_SIZE_AUTOMATIC  1
#define AV_BPRINT_SIZE_COUNT_ONLY 0

/**
* Init a print buffer.
*/
void av_bprint_init(AVBPrint *buf, unsigned size_init, unsigned size_max);

/**
* Init a print buffer using a pre-existing buffer.
*/
void av_bprint_init_for_buffer(AVBPrint *buf, char *buffer, unsigned size);

/**
* Append a formatted string to a print buffer.
*/
void av_bprintf(AVBPrint *buf, const char *fmt, ...) av_printf_format(2, 3);

/**
* Append a formatted string to a print buffer.
*/
void av_vbprintf(AVBPrint *buf, const char *fmt, va_list vl_arg);

/**
* Append char c n times to a print buffer.
*/
void av_bprint_chars(AVBPrint *buf, char c, unsigned n);

/**
* Append data to a print buffer.
*/
void av_bprint_append_data(AVBPrint *buf, const char *data, unsigned size);

struct tm;
/**
* Append a formatted date and time to a print buffer.
*/
void av_bprint_strftime(AVBPrint *buf, const char *fmt, const struct tm *tm);

/**
* Allocate bytes in the buffer for external use.
*/
void av_bprint_get_buffer(AVBPrint *buf, unsigned size,	unsigned char **mem, unsigned *actual_size);

/**
* Reset the string to "" but keep internal allocated data.
*/
void av_bprint_clear(AVBPrint *buf);

/**
* Test if the print buffer is complete (not truncated).
*
* It may have been truncated due to a memory allocation failure
* or the size_max limit (compare size and size_max if necessary).
*/
static inline int av_bprint_is_complete(AVBPrint *buf)
{
	return buf->len < buf->size;
}

int av_bprint_finalize(AVBPrint *buf, char **ret_str);

#define AV_LOG_QUIET    -8
#define AV_LOG_PANIC     0
#define AV_LOG_FATAL     8
#define AV_LOG_ERROR    16
#define AV_LOG_WARNING  24
#define AV_LOG_INFO     32
#define AV_LOG_VERBOSE  40
#define AV_LOG_DEBUG    48

#define AV_LOG_MAX_OFFSET (AV_LOG_DEBUG - AV_LOG_QUIET)
void av_log(int level, const char *fmt, ...) av_printf_format(3, 4);
void av_vlog(int level, const char *fmt, va_list vl);
int av_log_get_level(void);
void av_log_set_level(int level);
void av_log_set_callback(void (*callback)(int, const char*, va_list));
void av_log_default_callback(int level, const char *fmt, va_list vl);

/**
 * Format a line of log the same way as the default callback. 
 */
void av_log_format_line(int level, const char *fmt, va_list vl, char *line, int line_size, int *print_prefix);

#define AV_LOG_SKIP_REPEATED 1

void av_hex_dump(void * f, const char * buf, int size);


#endif /* AVUTIL_LOG_H */
