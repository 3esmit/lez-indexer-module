cmake_minimum_required(VERSION 3.14)
project(LezIndexerModuleTests LANGUAGES CXX)

include(LogosTest)

# The test target compiles the module implementation directly, so it supplies
# deterministic Indexer FFI functions and the generated-event replacement.
# The module-builder test setup copies indexer_ffi.h from the external package
# into ../lib before CMake configures this directory.
logos_test(
    NAME lez_indexer_module_tests
    MODULE_SOURCES
        ../src/lez_indexer_module_impl.cpp
        ../src/lez_ffi_marshalling.cpp
    TEST_SOURCES
        main.cpp
        test_lifecycle.cpp
        event_stubs.cpp
    MOCK_C_SOURCES
        mocks/mock_indexer_ffi.cpp
    EXTRA_INCLUDES
        ../lib
)
