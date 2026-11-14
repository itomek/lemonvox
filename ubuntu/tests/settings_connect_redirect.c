/* Test-process-only redirect of the factory loopback endpoint to synthetic HTTP.
 * Used by the missing-file acceptance case without touching a real server. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
int connect(int fd, const struct sockaddr *address, socklen_t length) {
    static int (*real_connect)(int, const struct sockaddr *, socklen_t);
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    const char *port = getenv("LEMONVOX_TEST_REDIRECT_PORT");
    if (port && address && address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in redirected;
        memcpy(&redirected, address, sizeof redirected);
        if (redirected.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && redirected.sin_port == htons(13305)) {
            redirected.sin_port = htons((unsigned short)atoi(port));
            return real_connect(fd, (const struct sockaddr *)&redirected, sizeof redirected);
        }
    }
    return real_connect(fd, address, length);
}
