# The bx pin of bgfx_pin.cmake, on its own so that a build needing only bx (the header-only math of the
# fragment host checks, tests/sun_dynamic_clip) declares and fetches bx alone.
# Requires storm_fetch_declare (cmake/StormBuildCache.cmake). Offline: -DFETCHCONTENT_SOURCE_DIR_STORM_BX.
include(FetchContent)
storm_fetch_declare(storm_bx
    GIT_REPOSITORY https://github.com/bkaradzic/bx.git
    GIT_TAG 57548da89a07771c5b0f8a6711149be70e08a329
    STORM_POPULATE_ONLY)
