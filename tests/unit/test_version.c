#include "rtcp/version.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char expected[64];
    int length = snprintf(expected, sizeof(expected), "%d.%d.%d%s", RTCP_VERSION_MAJOR,
                          RTCP_VERSION_MINOR, RTCP_VERSION_PATCH, RTCP_VERSION_SUFFIX);
    assert(length > 0 && (size_t)length < sizeof(expected));
    assert(strcmp(rtcp_version_string(), expected) == 0);

    const char *sha = rtcp_build_git_sha();
    size_t sha_length = strlen(sha);
    assert(strcmp(sha, "unknown") == 0 ||
           ((sha_length == 40 || sha_length == 64) && strspn(sha, "0123456789abcdef") == sha_length));
    puts(rtcp_version_string());
    puts(sha);
    return 0;
}
