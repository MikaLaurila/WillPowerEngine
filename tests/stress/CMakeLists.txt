wpe_add_test(stress_ecs SOURCES test_stress_ecs.c LIBS wpe::scene LABELS stress ecs scene)
wpe_add_test(stress_physics SOURCES test_stress_physics.c LIBS wpe::physics LABELS stress physics)
wpe_add_test(stress_navigation SOURCES test_stress_navigation.c LIBS wpe::navigation LABELS stress navigation)
