/** @file version.h
 * Build and release identity. */
#ifndef RTCP_VERSION_H
#define RTCP_VERSION_H

#define RTCP_VERSION_MAJOR  0
#define RTCP_VERSION_MINOR  1
#define RTCP_VERSION_PATCH  0
#define RTCP_VERSION_SUFFIX "-dev"

#define RTCP_VERSION_STRINGIFY_VALUE(value) #value
#define RTCP_VERSION_STRINGIFY(value)       RTCP_VERSION_STRINGIFY_VALUE(value)
#define RTCP_VERSION_STRING                    \
    RTCP_VERSION_STRINGIFY(RTCP_VERSION_MAJOR) \
    "." RTCP_VERSION_STRINGIFY(RTCP_VERSION_MINOR) "." RTCP_VERSION_STRINGIFY(RTCP_VERSION_PATCH) RTCP_VERSION_SUFFIX

#ifdef __cplusplus
extern "C"
{
#endif

/** Return the static SDK version string, including any development suffix. */
const char *rtcp_version_string(void);
/** Return the static source revision string embedded at build time. */
const char *rtcp_build_git_sha(void);

#ifdef __cplusplus
}
#endif

#endif
