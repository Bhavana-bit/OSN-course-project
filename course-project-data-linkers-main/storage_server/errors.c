#include "errors.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>


/*
   Return human readable explanation for each error code.
*/
const char *error_message(const char *code)
{
    if (strcmp(code, E400_INVALID_CMD) == 0)
        return "E400: Invalid Command\n";

    if (strcmp(code, E401_UNAUTHORIZED) == 0)
        return "E401: Unauthorized Access\n";

    if (strcmp(code, E403_FORBIDDEN) == 0)
        return "E403: Forbidden\n";

    if (strcmp(code, E404_NOT_FOUND) == 0)
        return "E404: File Not Found\n";

    if (strcmp(code, E409_CONFLICT) == 0)
        return "E409: Conflict\n";

    if (strcmp(code, E423_LOCKED) == 0)
        return "E423: File Locked\n";

    if (strcmp(code, E500_INTERNAL) == 0)
        return "E500: Internal Server Error\n";

    if (strcmp(code, E503_NM_FAILURE) == 0)
        return "E503: Name Server Failure\n";

    if (strcmp(code, OK200_SUCCESS) == 0)
        return "OK200: Success\n";

    return "E500: Unknown Error\n";
}

/*
   send_error(): send formatted message to client.
*/
void send_error(int sock, const char *code)
{
    const char *msg = error_message(code);
    send(sock, msg, strlen(msg), 0);
}
