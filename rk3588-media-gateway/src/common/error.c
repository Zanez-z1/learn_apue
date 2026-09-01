/* Common gw_error formatting and reset operations. */

#include "gateway/error.h"

#include <stdarg.h>
#include <stdio.h>

/*error set function*/
void gw_error_set(gw_error *error, gw_status code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}
/*error clear function*/
void gw_error_clear(gw_error *error)
{
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
}
