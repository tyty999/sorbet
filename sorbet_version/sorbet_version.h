#ifndef SORBET_VERSION_H
#define SORBET_VERSION_H

// We want this header to be both a C++ and a C header, so that it can be depended on by very
// low-level things
#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
namespace sorbet {

#if !defined(NDEBUG) || defined(FORCE_DEBUG)
#define DEBUG_MODE
constexpr bool debug_mode = true;
#else
#undef DEBUG_MODE
constexpr bool debug_mode = false;
#endif

#if !defined(EMSCRIPTEN)
constexpr bool emscripten_build = false;
#else
constexpr bool emscripten_build = true;
#endif

#if !defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
constexpr bool fuzz_mode = false;
#else
constexpr bool fuzz_mode = true;
#endif

} // namespace sorbet
#endif
// ^^^ __cplusplus

extern const char *sorbet_version;
extern const char *sorbet_codename;
extern const char *sorbet_build_scm_revision;
extern const int sorbet_build_scm_commit_count;
extern const char *sorbet_build_scm_status;
extern const long sorbet_build_timestamp;
extern const char *sorbet_full_version_string;
extern const int sorbet_isReleaseBuild;
extern const int sorbet_isWithDebugSymbols;

// Dynamically linking against a function symbol suffers fewer pitfalls vs linking against a data symbol.
// But using a data symbol directly can sometimes give better optimized code when there are no linking
// concerns (e.g., we're only statically linking and all source code is available).
//
// Provide both options so people can choose based on their needs. tl;dr if you're writing code inside Sorbet itself,
// prefer the global variables above.
const char *sorbet_getFullVersionString();

#ifdef __cplusplus
}
#endif

#endif // SORBET_VERSION_H