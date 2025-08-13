#include <rocprofiler-systems/user.h>

#include <cassert>
#include <cstdio>
#include <cstring>

// Dummy callback functions to bind to the API
int
dummy_trace()
{
    return 0;
}
int
dummy_region(const char*)
{
    return 0;
}
int
dummy_annotated(const char*, rocprofsys_annotation_t*, size_t)
{
    return 0;
}

// Main test function
int
main()
{
    printf("[user-api-coverage] Starting user API coverage test...\n");

    // Test 1: Call all functions with no bindings to trigger ROCPROFSYS_USER_ERROR_NO_BINDING
    assert(rocprofsys_user_start_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_stop_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_start_thread_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_stop_thread_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_push_region("test") == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_pop_region("test") == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_progress("test") == ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_push_annotated_region("test", nullptr, 0) ==
           ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_pop_annotated_region("test", nullptr, 0) ==
           ROCPROFSYS_USER_ERROR_NO_BINDING);
    assert(rocprofsys_user_annotated_progress("test", nullptr, 0) ==
           ROCPROFSYS_USER_ERROR_NO_BINDING);

    // Test 2: Test rocprofsys_user_error_string for all error codes
    assert(strcmp(rocprofsys_user_error_string(ROCPROFSYS_USER_SUCCESS), "Success") == 0);
    assert(strcmp(rocprofsys_user_error_string(ROCPROFSYS_USER_ERROR_NO_BINDING),
                  "Function pointer not assigned") == 0);
    assert(strcmp(rocprofsys_user_error_string(ROCPROFSYS_USER_ERROR_BAD_VALUE),
                  "Invalid value was provided") == 0);
    assert(strcmp(rocprofsys_user_error_string(ROCPROFSYS_USER_ERROR_INVALID_CATEGORY),
                  "Invalid user binding category") == 0);
    assert(strcmp(rocprofsys_user_error_string(ROCPROFSYS_USER_ERROR_INTERNAL),
                  "An unknown error occurred within rocprof-sys library") == 0);
    assert(strcmp(rocprofsys_user_error_string(999), "No error") == 0);  // Default case

    // Test 3: Test ROCPROFSYS_USER_REPLACE_CONFIG
    rocprofsys_user_callbacks_t callbacks = ROCPROFSYS_USER_CALLBACKS_INIT;
    callbacks.start_trace                 = &dummy_trace;
    callbacks.push_region                 = &dummy_region;
    callbacks.push_annotated_region       = &dummy_annotated;
    rocprofsys_user_configure(ROCPROFSYS_USER_REPLACE_CONFIG, callbacks, nullptr);
    assert(rocprofsys_user_start_trace() == ROCPROFSYS_USER_SUCCESS);
    assert(rocprofsys_user_push_region("test") == ROCPROFSYS_USER_SUCCESS);
    assert(rocprofsys_user_push_annotated_region("test", nullptr, 0) ==
           ROCPROFSYS_USER_SUCCESS);
    // This should fail now because REPLACE removed the other bindings
    assert(rocprofsys_user_stop_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);

    // Test 4: Test ROCPROFSYS_USER_INTERSECT_CONFIG
    rocprofsys_user_callbacks_t empty_callbacks = ROCPROFSYS_USER_CALLBACKS_INIT;
    rocprofsys_user_configure(ROCPROFSYS_USER_INTERSECT_CONFIG, empty_callbacks, nullptr);
    // All callbacks should be null now
    assert(rocprofsys_user_start_trace() == ROCPROFSYS_USER_ERROR_NO_BINDING);

    // Test 5: Test invalid configure mode
    assert(rocprofsys_user_configure((rocprofsys_user_configure_mode_t) 99,
                                     empty_callbacks,
                                     nullptr) == ROCPROFSYS_USER_ERROR_INVALID_CATEGORY);

    printf("[user-api-coverage] All coverage tests passed.\n");
    return 0;
}