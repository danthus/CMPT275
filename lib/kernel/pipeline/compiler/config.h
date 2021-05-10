#ifndef PIPELINE_KERNEL_COMPILER_CONFIG_H
#define PIPELINE_KERNEL_COMPILER_CONFIG_H

#define PRINT_DEBUG_MESSAGES

// #define PRINT_DEBUG_MESSAGES_FOR_KERNEL_NUM 21,22,23,24,25,26,27,28,29,30

#define PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM

// #define DISABLE_ZERO_EXTEND

// #define DISABLE_INPUT_ZEROING

// #define DISABLE_OUTPUT_ZEROING

// #define DISABLE_PARTITION_JUMPING

#define INITIALLY_TERMINATED_KERNELS_JUMP_TO_NEXT_PARTITION

#define FORCE_PIPELINE_ASSERTIONS

// #define FORCE_EACH_KERNEL_INTO_UNIQUE_PARTITION

// #define TEST_ALL_CONSUMERS

#define PRINT_BUFFER_GRAPH

#define PERMIT_BUFFER_MEMORY_REUSE

// #define TEST_EXPECTED_NUM_OF_STRIDES

// #define CHECK_EVERY_IO_PORT

#define ENABLE_GRAPH_TESTING_FUNCTIONS

// #define USE_LOOKBEHIND_FOR_LAST_VALUE // must match pipeline/internal/popcount_kernel.h

#endif // PIPELINE_KERNEL_COMPILER_CONFIG_H
