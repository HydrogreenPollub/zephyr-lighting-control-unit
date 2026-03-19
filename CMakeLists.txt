cmake_minimum_required(VERSION 3.13.1)

if(NOT DEFINED BOARD)
    set(BOARD lighting_control_unit CACHE STRING "Zephyr target board")
    message(STATUS "BOARD not defined externally, using default: ${BOARD}")
endif()

if(NOT DEFINED BOARD_ROOT)
    set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ CACHE PATH "Root directory for custom boards")
    message(STATUS "BOARD_ROOT not defined externally, using default: ${BOARD_ROOT}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app LANGUAGES C)

target_include_directories(app PRIVATE
        peripherals
        can-definitions
        src
)

if(CONFIG_BOOTLOADER_MCUBOOT)
    set(SIGNED_BIN  ${CMAKE_BINARY_DIR}/zephyr/zephyr.signed.bin)
    set(ELF_FILE    ${CMAKE_BINARY_DIR}/zephyr/zephyr.elf)
    set(HEADER_BIN  ${CMAKE_BINARY_DIR}/zephyr/mcuboot_header.bin)

    add_custom_target(patch_elf_mcuboot_header ALL
        COMMAND ${PYTHON_EXECUTABLE} -c
            "h=open('${SIGNED_BIN}','rb').read(${CONFIG_ROM_START_OFFSET}); open('${HEADER_BIN}','wb').write(h)"
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_elf_mcuboot_header.py
            ${ELF_FILE} ${HEADER_BIN} ${CONFIG_ROM_START_OFFSET}
        COMMENT "Patching MCUboot image header into zephyr.elf for GDB"
        VERBATIM
    )
    add_dependencies(patch_elf_mcuboot_header zephyr_final)
endif()

target_sources(app PRIVATE
        src/main.c
        src/lighting_control_unit.c
        src/lcu_dfu.c
        src/led_strip_ctrl.c
        peripherals/can.c
        peripherals/can_dfu.c
        peripherals/gpio.c
        peripherals/pwm.c
        peripherals/status_led.c
        peripherals/test_button.c
        can-definitions/candef.c
)