wpe_add_test(core_result SOURCES core/test_result.c LIBS wpe::core LABELS unit core)
wpe_add_test(core_log SOURCES core/test_log.c LIBS wpe::core LABELS unit core)
wpe_add_test(core_debug SOURCES core/test_debug.c LIBS wpe::core LABELS unit core)
wpe_add_test(core_alloc SOURCES core/test_alloc.c LIBS wpe::core LABELS unit core)
