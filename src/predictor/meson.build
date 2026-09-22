predictor_inc = include_directories('include')

accurate_dep = declare_dependency(
    include_directories: predictor_inc,
    dependencies: [
        cpp_lib_dep,
        shards_dep,
    ],
)

predictive_lru_ttl_cache_dep = declare_dependency(
    link_with: library(
        'predictive_lru_ttl_cache_lib',
        'predictive_lru_ttl_cache.cpp',
        include_directories: predictor_inc,
        dependencies: [
            accurate_dep,
            cpp_lib_dep,
            cpp_struct_dep,
            glib_dep,
        ],
    ),
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        cpp_lib_dep,
    ],
)

predictive_lfu_ttl_cache_dep = declare_dependency(
    link_with: library(
        'predictive_lfu_ttl_cache_lib',
        'predictive_lfu_ttl_cache.cpp',
        include_directories: predictor_inc,
        dependencies: [
            accurate_dep,
            cpp_lib_dep,
            cpp_struct_dep,
            glib_dep,
        ],
    ),
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        cpp_lib_dep,
    ],
)

predictive_clock_ttl_cache_dep = declare_dependency(
    link_with: library(
        'predictive_clock_ttl_cache_lib',
        'predictive_clock_ttl_cache.cpp',
        include_directories: predictor_inc,
        dependencies: [
            accurate_dep,
            cpp_lib_dep,
            cpp_struct_dep,
            glib_dep,
        ],
    ),
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        cpp_lib_dep,
    ],
)

test_lru_ttl_cache_exe = executable(
    'test_lru_ttl_cache_exe',
    'test_lru_ttl_cache.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        cpp_lib_dep,
        cpp_struct_dep,
    ],
)

test_predictive_lru_ttl_cache_exe = executable(
    'test_predictive_lru_ttl_cache_exe',
    'test_predictive_lru_ttl_cache.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_dep,
        cpp_struct_dep,
        predictive_lru_ttl_cache_dep,
    ],
)

test_predictive_clock_ttl_cache_exe = executable(
    'test_predictive_clock_ttl_cache_exe',
    'test_predictive_clock_ttl_cache.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_dep,
        cpp_struct_dep,
        predictive_clock_ttl_cache_dep,
    ],
)

accurate_exe = executable(
    'accurate_exe',
    'accurate.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        common_dep,
        io_dep,
        cpp_struct_dep,
        trace_dep,
        cache_statistics_dep,
        cpp_lib_dep,
        shards_dep,
    ],
)

predictor_exe = executable(
    'predictor_exe',
    'predictor.cpp',
    include_directories: cache_inc,
    dependencies: [
        accurate_dep,
        cpp_lib_util_dep,
        common_dep,
        io_dep,
        cpp_struct_dep,
        trace_dep,
        cache_statistics_dep,
        cpp_lib_dep,
        predictive_lru_ttl_cache_dep,
        predictive_lfu_ttl_cache_dep,
        predictive_clock_ttl_cache_dep,
        shards_dep,
    ],
)

test_lifetime_thresholds_exe = executable(
    'test_lifetime_thresholds',
    'test_lifetime_thresholds.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        common_dep,
        cpp_lib_dep,
        glib_dep,
    ],
)

test_iterator_spaces_exe = executable(
    'test_iterator_spaces_exe',
    'test_iterator_spaces.cpp',
    include_directories: predictor_inc,
    dependencies: [
        accurate_dep,
        common_dep,
        cpp_lib_dep,
        glib_dep,
    ],
)

test('test_lifetime_thresholds', test_lifetime_thresholds_exe)
test('test_lru_ttl_cache', test_lru_ttl_cache_exe)
test('test_predictive_lru_ttl_cache', test_predictive_lru_ttl_cache_exe)
test('test_predictive_clock_ttl_cache', test_predictive_clock_ttl_cache_exe)
test('test_iterator_spaces', test_iterator_spaces_exe)