add_library(wpe_jobs STATIC jobs.c)
add_library(wpe::jobs ALIAS wpe_jobs)

target_link_libraries(wpe_jobs
    PUBLIC wpe::core
    PRIVATE wpe_dep_sdl3)
wpe_configure_target(wpe_jobs)
