cmake_minimum_required(VERSION 3.20)

# 使用方法（先将 CMake 和 Ninja 加入 PATH）：
# cmake -S . -B build/cmake-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
# cmake --build build/cmake-debug --parallel
# 只需构建 Ethercat_Master_rzn2l_v300；链接成功后自动同步到 e2 studio Debug 目录。
# 当前 CLion 的下载配置读取该目录中的 ELF，因此复制失败也必须报告构建失败。
# 此配置使用工程内现有的 FSP 生成文件，不执行 FSP 代码生成。

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_TOOLCHAIN_DIR "" CACHE PATH "Directory containing arm-none-eabi-gcc")
set(ARM_TOOLCHAIN_HINTS
        "${ARM_TOOLCHAIN_DIR}"
        "D:/ide/Renesas/rzn/e2studio_v2025-10_fsp_v3.1.0/toolchains/gcc_arm/arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-eabi/bin"
        "D:/ide/Renesas/rz/e2studio_v2026-04.1_fsp_v4.1.0/toolchains/gcc_arm/arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-eabi/bin"
)

find_program(ARM_NONE_EABI_GCC arm-none-eabi-gcc HINTS ${ARM_TOOLCHAIN_HINTS} REQUIRED)
get_filename_component(ARM_GCC_BIN_DIR "${ARM_NONE_EABI_GCC}" DIRECTORY)
find_program(ARM_NONE_EABI_OBJCOPY arm-none-eabi-objcopy HINTS "${ARM_GCC_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
find_program(ARM_NONE_EABI_SIZE arm-none-eabi-size HINTS "${ARM_GCC_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
set(CMAKE_C_COMPILER "${ARM_NONE_EABI_GCC}" CACHE FILEPATH "ARM GCC C compiler" FORCE)

project(Ethercat_Master_rzn2l_v300 LANGUAGES C)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/cmake-debug -G Ninja")
endif ()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
endif ()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TARGET_NAME Ethercat_Master_rzn2l_v300)
set(E2STUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/project/e2studio")
set(E2STUDIO_DEBUG_DIR "${E2STUDIO_DIR}/Debug")
set(FSP_LINKER_DIR "${E2STUDIO_DIR}/script")
set(FSP_LINKER_SCRIPT "${FSP_LINKER_DIR}/fsp_xspi0_boot.ld")
set(FSP_REDEFINITION_FILE "${FSP_LINKER_DIR}/redefinition_memory_regions_gcc.h")
set(FSP_MEMORY_REGIONS "${E2STUDIO_DEBUG_DIR}/memory_regions.ld" CACHE FILEPATH
        "FSP-generated memory_regions.ld; may point to a saved copy outside Debug")
set(FSP_CMAKE_LINKER_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/fsp_xspi0_boot_cmake.ld")

foreach (REQUIRED_FILE IN ITEMS
        "${FSP_LINKER_SCRIPT}" "${FSP_REDEFINITION_FILE}" "${FSP_MEMORY_REGIONS}"
        "${E2STUDIO_DIR}/rzn_gen/main.c" "${E2STUDIO_DIR}/rzn_cfg/aws/FreeRTOSConfig.h")
    if (NOT EXISTS "${REQUIRED_FILE}")
        message(FATAL_ERROR "Required FSP file is missing: ${REQUIRED_FILE}. Generate the project in e2 studio first.")
    endif ()
endforeach ()

# 只修改构建目录中的副本，保留 e2 studio 原始链接脚本。
# e2 studio 使用 startup.c -> startup.o，CMake 使用 startup.c.o 或 startup.c.obj。
# 必须补齐启动对象的匹配规则，才能继续把 Loader 代码和数据放入 BTCM。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FSP_LINKER_SCRIPT}")
file(READ "${FSP_LINKER_SCRIPT}" FSP_LINKER_SCRIPT_CONTENT)

# 函数作用：为原有链接规则补充两种 CMake C 目标文件名。
# 参数 original：原始对象选择规则，例如 */startup.o(.text*)。
# 返回：通过父作用域的 FSP_LINKER_SCRIPT_CONTENT 返回更新后的脚本文本。
function(_add_cmake_object_patterns original)
    foreach (cmake_suffix IN ITEMS ".c.o" ".c.obj")
        string(REPLACE ".o(" "${cmake_suffix}(" cmake_object "${original}")
        string(REPLACE "${original}" "${original}\n        ${cmake_object}"
                FSP_LINKER_SCRIPT_CONTENT "${FSP_LINKER_SCRIPT_CONTENT}")
    endforeach ()
    set(FSP_LINKER_SCRIPT_CONTENT "${FSP_LINKER_SCRIPT_CONTENT}" PARENT_SCOPE)
endfunction()

foreach (section IN ITEMS ".text*" ".data*" ".bss*" "COMMON")
    foreach (object IN ITEMS startup_core system_core startup system r_ioport)
        _add_cmake_object_patterns("*/${object}.o(${section})")
    endforeach ()
    _add_cmake_object_patterns("*/all*/bsp_*.o(${section})")
endforeach ()

foreach (object IN ITEMS bsp_mpu_core bsp_mmu_core bsp_register_protection bsp_tzc400)
    _add_cmake_object_patterns("*/${object}.o(.rodata*)")
endforeach ()

file(WRITE "${FSP_CMAKE_LINKER_SCRIPT}" "${FSP_LINKER_SCRIPT_CONTENT}")
configure_file("${FSP_MEMORY_REGIONS}" "${CMAKE_CURRENT_BINARY_DIR}/memory_regions.ld" COPYONLY)
configure_file("${FSP_REDEFINITION_FILE}" "${CMAKE_CURRENT_BINARY_DIR}/redefinition_memory_regions_gcc.h" COPYONLY)

set(CPU_OPTIONS
        -mcpu=cortex-r52
        -mthumb
        -mfloat-abi=hard
        -mfpu=neon-fp-armv8
)

# 与当前 e2 studio Debug 的 C 编译选项一致；其他配置沿用 CMake 的优化级别。
set(COMMON_COMPILE_OPTIONS
        ${CPU_OPTIONS}
        "$<$<CONFIG:Debug>:-Og>"
        # 沿用参考模板的 DWARF 4 调试信息格式。
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:-gdwarf-4>"
        -fdiagnostics-parseable-fixits
        -fmessage-length=0
        -fsigned-char
        -ffunction-sections
        -fdata-sections
        -Wunused
        -Wuninitialized
        -Wall
        -Wextra
        -Wmissing-declarations
        -Wconversion
        -Wpointer-arith
        -Wshadow
        -Wlogical-op
        -Waggregate-return
        -Wfloat-equal
        -Wnull-dereference
        -Wno-format-truncation
        -Wno-stringop-overflow
        --param=min-pagesize=0
)

set(INCLUDE_DIRS
        "${E2STUDIO_DIR}/rzn_gen"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${E2STUDIO_DIR}/rzn_cfg/fsp_cfg/bsp"
        "${E2STUDIO_DIR}/rzn_cfg/fsp_cfg"
        "${E2STUDIO_DIR}/rzn_cfg/aws"
        "${E2STUDIO_DIR}/src"
        "${E2STUDIO_DIR}/rzn/fsp/inc"
        "${E2STUDIO_DIR}/rzn/fsp/inc/api"
        "${E2STUDIO_DIR}/rzn/fsp/inc/instances"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/mcu/all/cr"
        "${E2STUDIO_DIR}/rzn/aws/FreeRTOS-Kernel/include"
        "${E2STUDIO_DIR}/rzn/fsp/src/rm_freertos_port/cr"
        "${E2STUDIO_DIR}/rzn/fsp/src/rm_freertos_port"
        "${E2STUDIO_DIR}/rzn/arm/CMSIS_6/CMSIS/Core/Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/serial_io"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethernet"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/app"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/osal"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/oshw"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/src"
)

# 对照当前 e2 studio 的 sources.mk 收集源码。添加新模块时同步更新此列表及包含路径。
# 不递归收集整个 FSP，避免混入其他内核的启动代码或其他 FreeRTOS 堆实现。
set(SOURCE_DIRS
        "${E2STUDIO_DIR}/src"
        "${E2STUDIO_DIR}/rzn_gen"
        "${E2STUDIO_DIR}/rzn/fsp/src/rm_freertos_port/cr"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_sci_uart"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_ioport"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/mcu/rzn2l"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/mcu/all/cr"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/mcu/all"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/cmsis/Device/RENESAS/Source/cr"
        "${E2STUDIO_DIR}/rzn/fsp/src/bsp/cmsis/Device/RENESAS/Source"
        "${E2STUDIO_DIR}/rzn/board/rzn2l_rsk"
        "${E2STUDIO_DIR}/rzn/aws/FreeRTOS-Kernel"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_ethsw"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_gmac"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_ether_phy"
        "${E2STUDIO_DIR}/rzn/fsp/src/r_ether_selector"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/serial_io"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethernet"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/app"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/osal"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/oshw"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/module/ethercat/soem/src"
)

set(PROJECT_SOURCES "${E2STUDIO_DIR}/rzn/aws/FreeRTOS-Kernel/portable/MemMang/heap_4.c")
foreach (SOURCE_DIR IN LISTS SOURCE_DIRS)
    file(GLOB DIR_SOURCES CONFIGURE_DEPENDS "${SOURCE_DIR}/*.c")
    list(APPEND PROJECT_SOURCES ${DIR_SOURCES})
endforeach ()
list(REMOVE_DUPLICATES PROJECT_SOURCES)

add_executable(${TARGET_NAME} ${PROJECT_SOURCES})
set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
        LINK_DEPENDS "${FSP_CMAKE_LINKER_SCRIPT};${CMAKE_CURRENT_BINARY_DIR}/memory_regions.ld;${CMAKE_CURRENT_BINARY_DIR}/redefinition_memory_regions_gcc.h"
)
target_include_directories(${TARGET_NAME} PRIVATE ${INCLUDE_DIRS})
target_compile_definitions(${TARGET_NAME} PRIVATE _RENESAS_RZN_ _RZN_CORE=CR52_0 _RZN_ORDINAL=1)
target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_COMPILE_OPTIONS})

target_link_options(${TARGET_NAME} PRIVATE
        ${CPU_OPTIONS}
        "-T${FSP_CMAKE_LINKER_SCRIPT}"
        "-L${CMAKE_CURRENT_BINARY_DIR}"
        -nostartfiles
        -Wl,--gc-sections
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.map"
        --specs=nano.specs
        # 链入 newlib-nano 的浮点格式化实现，供 printf/vsprintf 的 %f、%e、%g 使用。
        -Wl,-u,_printf_float
        -Wl,-esystem_init
        --specs=rdimon.specs
)

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND "${ARM_NONE_EABI_OBJCOPY}" -O srec "$<TARGET_FILE:${TARGET_NAME}>"
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.srec"
        COMMAND "${ARM_NONE_EABI_SIZE}" --format=berkeley "$<TARGET_FILE:${TARGET_NAME}>"
        # 与参考模板一致：主目标链接成功后，立即更新调试器实际使用的三个文件。
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${E2STUDIO_DEBUG_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>" "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.srec"
        "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.srec"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.map"
        "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.map"
        BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.srec"
        "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.elf"
        "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.srec"
        "${E2STUDIO_DEBUG_DIR}/${TARGET_NAME}.map"
        COMMENT "Generating firmware and updating the ELF used by the debugger"
        VERBATIM
)
