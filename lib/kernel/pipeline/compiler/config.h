#ifndef PIPELINE_KERNEL_COMPILER_CONFIG_H
#define PIPELINE_KERNEL_COMPILER_CONFIG_H

#define PRINT_DEBUG_MESSAGES

// #define PRINT_DEBUG_MESSAGES_FOR_KERNEL_NUM 31,32,33,34,35,36

// #define PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM

// #define DISABLE_ZERO_EXTEND

// #define DISABLE_INPUT_ZEROING

// #define DISABLE_OUTPUT_ZEROING

// #define DISABLE_PARTITION_JUMPING

#define FORCE_PIPELINE_ASSERTIONS

// #define FORCE_EACH_KERNEL_INTO_UNIQUE_PARTITION

// #define TEST_ALL_KERNEL_INPUTS

// #define TEST_ALL_CONSUMERS

// #define PRINT_BUFFER_GRAPH

#define PERMIT_BUFFER_MEMORY_REUSE

#define ENABLE_GRAPH_TESTING_FUNCTIONS

#define USE_FIXED_SEGMENT_NUMBER_INCREMENTS

// #define FORCE_ALL_STREAMSETS_TO_BE_LINEAR

// #define FORCE_ALL_INTER_PARTITION_STREAMSETS_TO_BE_LINEAR

// #define USE_LOOKBEHIND_FOR_LAST_VALUE // must match pipeline/internal/popcount_kernel.h

// #define COMPUTE_SYMBOLIC_RATE_IDS



#endif // PIPELINE_KERNEL_COMPILER_CONFIG_H
