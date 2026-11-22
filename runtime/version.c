#include "rtcp/version.h"

#ifdef RTCP_BUILD_GIT_SHA_HEADER
#include "rtcp_build_git_sha.h"
#endif

#ifndef RTCP_BUILD_GIT_SHA
#define RTCP_BUILD_GIT_SHA unknown
#endif

const char *rtcp_version_string(void)
{
    return RTCP_VERSION_STRING;
}

const char *rtcp_build_git_sha(void)
{
    return RTCP_VERSION_STRINGIFY(RTCP_BUILD_GIT_SHA);
}
