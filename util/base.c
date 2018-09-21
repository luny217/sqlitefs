/*
 * FileName:
 * Author:         luny  Version: 1.0  Date: 2016-9-6
 * Description:
 * Version:
 * Function List:
 *                 1.
 * History:
 *     <author>   <time>    <version >   <desc>
 */

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
//#include <WinSock2.h>
#pragma comment( lib,"winmm.lib" )
#else
#include <unistd.h>
#include <sys/time.h>
#endif

#include "base.h"
#include "nlist.h"

/*
* http://msdn.microsoft.com/en-us/library/ms684122(v=vs.85).aspx
*/
int32_t atomic_int_get(const volatile int32_t * atomic)
{
    MemoryBarrier();
    return *atomic;
}

void atomic_int_set(volatile int32_t * atomic, int32_t newval)
{
    *atomic = newval;
    MemoryBarrier();
}

void atomic_int_inc(volatile int32_t * atomic)
{
    InterlockedIncrement(atomic);
}

void get_current_time(n_timeval_t * result)
{
#ifndef _WIN32
    struct timeval r;

    g_return_if_fail(result != NULL);

    /*this is required on alpha, there the timeval structs are int's
    not longs and a cast only would fail horribly*/
    gettimeofday(&r, NULL);
    result->tv_sec = r.tv_sec;
    result->tv_usec = r.tv_usec;
#else
    FILETIME ft;
    uint64_t time64;

    GetSystemTimeAsFileTime(&ft);
    memmove(&time64, &ft, sizeof(FILETIME));

    /* Convert from 100s of nanoseconds since 1601-01-01
    * to Unix epoch. Yes, this is Y2038 unsafe.
    */
    time64 -= 116444736000000000i64;
    time64 /= 10;

    result->tv_sec = (int32_t)(time64 / 1000000);
    result->tv_usec = (int32_t)(time64 % 1000000);
#endif
}

void time_val_add(n_timeval_t  * _time, int32_t microseconds)
{
    if (microseconds >= 0)
    {
        _time->tv_usec += microseconds % USEC_PER_SEC;
        _time->tv_sec += microseconds / USEC_PER_SEC;
        if (_time->tv_usec >= USEC_PER_SEC)
        {
            _time->tv_usec -= USEC_PER_SEC;
            _time->tv_sec++;
        }
    }
    else
    {
        microseconds *= -1;
        _time->tv_usec -= microseconds % USEC_PER_SEC;
        _time->tv_sec -= microseconds / USEC_PER_SEC;
        if (_time->tv_usec < 0)
        {
            _time->tv_usec += USEC_PER_SEC;
            _time->tv_sec--;
        }
    }
}

void sleep_us(uint32_t microseconds)
{
#ifdef _WIN32
    Sleep(microseconds / 1000);
#else
    struct timespec request, remaining;
    request.tv_sec = microseconds / USEC_PER_SEC;
    request.tv_nsec = 1000 * (microseconds % USEC_PER_SEC);
    while (nanosleep(&request, &remaining) == -1 && errno == EINTR)
        request = remaining;
#endif
}

void sleep_ms(uint32_t milliseconds)
{
    sleep_us(milliseconds * 1000);
}

#if defined _WIN32
static ULONGLONG(*_GetTickCount64)(void) = NULL;
static uint32_t _win32_tick_epoch = 0;

void clock_win32_init(void)
{
    HMODULE kernel32;

    _GetTickCount64 = NULL;
    kernel32 = GetModuleHandle(L"kernel32.dll");
    if (kernel32 != NULL)
        _GetTickCount64 = (void *)GetProcAddress(kernel32, "GetTickCount64");
    _win32_tick_epoch = ((uint32_t)GetTickCount()) >> 31;
}

int64_t get_monotonic_time(void)
{
    uint64_t ticks;
    uint32_t ticks32;   

    if (_GetTickCount64 != NULL)
    {
        uint32_t ticks_as_32bit;

        ticks = _GetTickCount64();
        ticks32 = timeGetTime();

        ticks_as_32bit = (uint32_t)ticks;

        if (ticks32 - ticks_as_32bit <= INT_MAX)
            ticks += ticks32 - ticks_as_32bit;
        else
            ticks -= ticks_as_32bit - ticks32;
    }
    else
    {
        uint32_t epoch;

        epoch = atomic_int_get(&_win32_tick_epoch);

        ticks32 = timeGetTime();
 
        if ((ticks32 >> 31) != (epoch & 1))
        {
            epoch++;
            atomic_int_set(&_win32_tick_epoch, epoch);
        }

        ticks = (uint64_t)ticks32 | ((uint64_t)epoch) << 31;
    }

    return ticks * 1000;
}
#elif defined(HAVE_MACH_MACH_TIME_H) /* Mac OS */
int64_t get_monotonic_time(void)
{
    static mach_timebase_info_data_t timebase_info;

    if (timebase_info.denom == 0)
    {
        /* This is a fraction that we must use to scale
        * mach_absolute_time() by in order to reach nanoseconds.
        *
        * We've only ever observed this to be 1/1, but maybe it could be
        * 1000/1 if mach time is microseconds already, or 1/1000 if
        * picoseconds.  Try to deal nicely with that.
        */
        mach_timebase_info(&timebase_info);

        /* We actually want microseconds... */
        if (timebase_info.numer % 1000 == 0)
            timebase_info.numer /= 1000;
        else
            timebase_info.denom *= 1000;

        /* We want to make the numer 1 to avoid having to multiply... */
        if (timebase_info.denom % timebase_info.numer == 0)
        {
            timebase_info.denom /= timebase_info.numer;
            timebase_info.numer = 1;
        }
        else
        {
            /* We could just multiply by timebase_info.numer below, but why
            * bother for a case that may never actually exist...
            *
            * Plus -- performing the multiplication would risk integer
            * overflow.  If we ever actually end up in this situation, we
            * should more carefully evaluate the correct course of action.
            */
            mach_timebase_info(&timebase_info); /* Get a fresh copy for a better message */
            g_error("Got weird mach timebase info of %d/%d.  Please file a bug against GLib.",
                    timebase_info.numer, timebase_info.denom);
        }
    }

    return mach_absolute_time() / timebase_info.denom;
}
#else
int64_t  get_monotonic_time(void)
{
    struct timespec ts;
    int32_t result;

    result = clock_gettime(CLOCK_MONOTONIC, &ts);

    return (((int64_t)ts.tv_sec) * 1000000) + (ts.tv_nsec / 1000);
}
#endif

/**
* g_strdup:
* @str: (nullable): the string to duplicate
*
* Duplicates a string. If @str is %NULL it returns %NULL.
* The returned string should be freed with g_free()
* when no longer needed.
*
* Returns: a newly-allocated copy of @str
*/
char * n_strdup(const char * str)
{
    char * new_str;
    int32_t length;

    if (str)
    {
        length = strlen(str) + 1;
        //new_str = g_new(char, length);
        new_str = malloc(length * sizeof(char));
        memcpy(new_str, str, length);
    }
    else
        new_str = NULL;

    return new_str;
}

/**
* g_strndup:
* @str: the string to duplicate
* @n: the maximum number of bytes to copy from @str
*
* Duplicates the first @n bytes of a string, returning a newly-allocated
* buffer @n + 1 bytes long which will always be nul-terminated. If @str
* is less than @n bytes long the buffer is padded with nuls. If @str is
* %NULL it returns %NULL. The returned value should be freed when no longer
* needed.
*
* To copy a number of characters from a UTF-8 encoded string,
* use g_utf8_strncpy() instead.
*
* Returns: a newly-allocated buffer containing the first @n bytes
*     of @str, nul-terminated
*/
char * n_strndup(const char * str, int32_t n)
{
    char * new_str;

    if (str)
    {
        //new_str = g_new(char, n + 1);
        new_str = malloc((n + 1) * sizeof(char));
        strncpy(new_str, str, n);
        new_str[n] = '\0';
    }
    else
        new_str = NULL;

    return new_str;
}

/**
* n_strsplit:
* @string: a string to split
* @delimiter: a string which specifies the places at which to split
*     the string. The delimiter is not included in any of the resulting
*     strings, unless @max_tokens is reached.
* @max_tokens: the maximum number of pieces to split @string into.
*     If this is less than 1, the string is split completely.
*
* Splits a string into a maximum of @max_tokens pieces, using the given
* @delimiter. If @max_tokens is reached, the remainder of @string is
* appended to the last token.
*
* As an example, the result of g_strsplit (":a:bc::d:", ":", -1) is a
* %NULL-terminated vector containing the six strings "", "a", "bc", "", "d"
* and "".
*
* As a special case, the result of splitting the empty string "" is an empty
* vector, not a vector containing a single string. The reason for this
* special case is that being able to represent a empty vector is typically
* more useful than consistent handling of empty elements. If you do need
* to represent empty elements, you'll need to check for the empty string
* before calling g_strsplit().
*
* Returns: a newly-allocated %NULL-terminated array of strings. Use
*    g_strfreev() to free it.
*/
char ** n_strsplit(const char * string, const char * delimiter, uint32_t  max_tokens)
{
    n_slist_t * string_list = NULL, *slist;
    char ** str_array, * s;
    uint32_t n = 0;
    const char * remainder;

    if (max_tokens < 1)
        max_tokens = INT_MAX;

    remainder = string;
    s = strstr(remainder, delimiter);
    if (s)
    {
        uint32_t delimiter_len = strlen(delimiter);

        while (--max_tokens && s)
        {
            uint32_t len;

            len = s - remainder;
            string_list = n_slist_prepend(string_list, n_strndup(remainder, len));
            n++;
            remainder = s + delimiter_len;
            s = strstr(remainder, delimiter);
        }
    }
    if (*string)
    {
        n++;
        string_list = n_slist_prepend(string_list, n_strdup(remainder));
    }

    //str_array = g_new(char*, n + 1);
    str_array = malloc((n + 1) * sizeof(char *));

    str_array[n--] = NULL;
    for (slist = string_list; slist; slist = slist->next)
        str_array[n--] = slist->data;

    n_slist_free(string_list);

    return str_array;
}

/**
* g_strsplit_set:
* @string: The string to be tokenized
* @delimiters: A nul-terminated string containing bytes that are used
*     to split the string.
* @max_tokens: The maximum number of tokens to split @string into.
*     If this is less than 1, the string is split completely
*
* Splits @string into a number of tokens not containing any of the characters
* in @delimiter. A token is the (possibly empty) longest string that does not
* contain any of the characters in @delimiters. If @max_tokens is reached, the
* remainder is appended to the last token.
*
* For example the result of g_strsplit_set ("abc:def/ghi", ":/", -1) is a
* %NULL-terminated vector containing the three strings "abc", "def",
* and "ghi".
*
* The result of g_strsplit_set (":def/ghi:", ":/", -1) is a %NULL-terminated
* vector containing the four strings "", "def", "ghi", and "".
*
* As a special case, the result of splitting the empty string "" is an empty
* vector, not a vector containing a single string. The reason for this
* special case is that being able to represent a empty vector is typically
* more useful than consistent handling of empty elements. If you do need
* to represent empty elements, you'll need to check for the empty string
* before calling g_strsplit_set().
*
* Note that this function works on bytes not characters, so it can't be used
* to delimit UTF-8 strings for anything but ASCII characters.
*
* Returns: a newly-allocated %NULL-terminated array of strings. Use
*    g_strfreev() to free it.
*
* Since: 2.4
**/
char ** n_strsplit_set(const char * string, const char * delimiters, int32_t max_tokens)
{
    int delim_table[256];
    n_slist_t * tokens, *list;
    int32_t n_tokens;
    const char * s;
    const char * current;
    char * token;
    char ** result;

    if (max_tokens < 1)
        max_tokens = INT_MAX;

    if (*string == '\0')
    {
        //result = g_new(char *, 1);
        result = malloc(1 * sizeof(char *));
        result[0] = NULL;
        return result;
    }

    memset(delim_table, FALSE, sizeof(delim_table));
    for (s = delimiters; *s != '\0'; ++s)
        delim_table[*(uint8_t *)s] = TRUE;

    tokens = NULL;
    n_tokens = 0;

    s = current = string;
    while (*s != '\0')
    {
        if (delim_table[*(uint8_t *)s] && n_tokens + 1 < max_tokens)
        {
            token = n_strndup(current, s - current);
            tokens = n_slist_prepend(tokens, token);
            ++n_tokens;

            current = s + 1;
        }

        ++s;
    }

    token = n_strndup(current, s - current);
    tokens = n_slist_prepend(tokens, token);
    ++n_tokens;

    //result = g_new(char *, n_tokens + 1);
    result = malloc((n_tokens + 1) * sizeof(char *));

    result[n_tokens] = NULL;
    for (list = tokens; list != NULL; list = list->next)
        result[--n_tokens] = list->data;

    n_slist_free(tokens);

    return result;
}

/**
* g_strfreev:
* @str_array: (nullable): a %NULL-terminated array of strings to free
*
* Frees a %NULL-terminated array of strings, as well as each
* string it contains.
*
* If @str_array is %NULL, this function simply returns.
*/
void n_strfreev(char ** str_array)
{
    if (str_array)
    {
        int i;

        for (i = 0; str_array[i] != NULL; i++)
            free(str_array[i]);

        free(str_array);
    }
}

/**
* n_memdup:
* @mem: the memory to copy.
* @byte_size: the number of bytes to copy.
*
* Allocates @byte_size bytes of memory, and copies @byte_size bytes into it
* from @mem. If @mem is %NULL it returns %NULL.
*
* Returns: a pointer to the newly-allocated copy of the memory, or %NULL if @mem
*  is %NULL.
*/
void * n_memdup(const void * mem, uint32_t  byte_size)
{
    void * new_mem;

    if (mem)
    {
        new_mem = malloc(byte_size);
        memcpy(new_mem, mem, byte_size);
    }
    else
        new_mem = NULL;

    return new_mem;
}

#if 0 //def _WIN32
int _poll(struct pollfd * fds, nfds_t numfds, int timeout)
{
    fd_set read_set;
    fd_set write_set;
    fd_set exception_set;
    nfds_t i;
    int n;
    int rc;

    if (numfds >= FD_SETSIZE)
    {
        errno = EINVAL;
        return -1;
    }

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&exception_set);

    n = 0;
    for (i = 0; i < numfds; i++)
    {
        if (fds[i].fd < 0)
            continue;

        if (fds[i].events & POLLIN)
            FD_SET(fds[i].fd, &read_set);
        if (fds[i].events & POLLOUT)
            FD_SET(fds[i].fd, &write_set);
        if (fds[i].events & POLLERR)
            FD_SET(fds[i].fd, &exception_set);

        if (fds[i].fd >= n)
            n = fds[i].fd + 1;
    }

    if (n == 0)
        /* Hey!? Nothing to poll, in fact!!! */
        return 0;

    if (timeout < 0)
    {
        rc = select(n, &read_set, &write_set, &exception_set, NULL);
    }
    else
    {
        struct timeval tv;
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = 1000 * (timeout % 1000);
        rc = select(n, &read_set, &write_set, &exception_set, &tv);
    }

    if (rc < 0)
        return rc;

    for (i = 0; i < numfds; i++)
    {
        fds[i].revents = 0;

        if (FD_ISSET(fds[i].fd, &read_set))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &write_set))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &exception_set))
            fds[i].revents |= POLLERR;
    }

    return rc;
}
#endif /* !HAVE_POLL_H */

#if 1
int net_errno(void)
{
#ifdef _WIN32
	int err = WSAGetLastError();
	switch (err)
	{
	case WSAEWOULDBLOCK:
		return -EAGAIN;
	case WSAEINTR:
		return -EINTR;
	case WSAEPROTONOSUPPORT:
		return -EPROTONOSUPPORT;
	case WSAETIMEDOUT:
		return -ETIMEDOUT;
	case WSAECONNREFUSED:
		return -ECONNREFUSED;
	case WSAEINPROGRESS:
		return -EINPROGRESS;
	}
	return -err;
#else
	return -errno;
#endif
}
#endif