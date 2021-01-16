#ifndef PIPELINE_KERNEL_COMPILER_CONFIG_H
#define PIPELINE_KERNEL_COMPILER_CONFIG_H

// #define PRINT_DEBUG_MESSAGES

// #define DISABLE_ZERO_EXTEND

// #define DISABLE_INPUT_ZEROING

// #define DISABLE_OUTPUT_ZEROING

#define INITIALLY_TERMINATED_KERNELS_JUMP_TO_NEXT_PARTITION

#define FORCE_PIPELINE_ASSERTIONS

// #define FORCE_EACH_KERNEL_INTO_UNIQUE_PARTITION

// #define TEST_ALL_CONSUMERS

// #define PRINT_BUFFER_GRAPH

#define PERMIT_BUFFER_MEMORY_REUSE

// #define TEST_EXPECTED_NUM_OF_STRIDES

#endif // PIPELINE_KERNEL_COMPILER_CONFIG_H
