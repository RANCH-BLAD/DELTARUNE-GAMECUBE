# GameCube.cmake — devkitPro libogc GameCube toolchain for the Cinnamon GC port.
# Modeled on devkitPro's ogc-common.cmake (NintendoGameCube branch), adapted to
# a relocatable DEVKITPRO (~/devkitpro, no root install).
cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_SYSTEM_NAME)
    set(CMAKE_SYSTEM_NAME NintendoGameCube)
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR ppc)
endif()

# Import devkitPPC toolchain (defines __dkp_toolchain + compiler settings)
include(${CMAKE_CURRENT_LIST_DIR}/devkitPPC.cmake)

set(OGC_ROOT ${DEVKITPRO}/libogc)
set(DKP_INSTALL_PREFIX_INIT ${DEVKITPRO}/portlibs/ppc)

# Platform prefix: gamecube portlibs if present, else ppc, plus libogc root.
__dkp_platform_prefix(
    ${DEVKITPRO}/portlibs/gamecube
    ${DEVKITPRO}/portlibs/ppc
    ${OGC_ROOT}
)

find_program(PKG_CONFIG_EXECUTABLE NAMES powerpc-eabi-pkg-config
    HINTS "${DEVKITPRO}/portlibs/gamecube/bin" "${DEVKITPRO}/portlibs/ppc/bin")

find_program(ELF2DOL_EXE NAMES elf2dol HINTS "${DEVKITPRO}/tools/bin")

# Import devkitPro generic platform settings (sets build type, suffix, flags)
include(${CMAKE_CURRENT_LIST_DIR}/Generic-dkP.cmake)

# Platform settings
set(OGC_ARCH_SETTINGS "-mogc -mcpu=750 -meabi -mhard-float")
set(OGC_COMMON_FLAGS  "-ffunction-sections -fdata-sections -D__gamecube__ -DGEKKO")
set(OGC_LINKER_FLAGS  "-L${OGC_ROOT}/lib/cube")
set(OGC_STANDARD_LIBRARIES "-logc -lm")
set(OGC_STANDARD_INCLUDE_DIRECTORIES "${OGC_ROOT}/include")

__dkp_init_platform_settings(OGC)

set(CMAKE_ASM_FLAGS_INIT "${CMAKE_ASM_FLAGS_INIT} -mregnames")

function(ogc_create_dol target)
    if(NOT ELF2DOL_EXE)
        message(FATAL_ERROR "elf2dol not found in ${DEVKITPRO}/tools/bin")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${ELF2DOL_EXE}" "$<TARGET_FILE:${target}>" "${target}.dol"
        BYPRODUCTS "${target}.dol"
        COMMENT "Converting ${target} to .dol"
        VERBATIM
    )
endfunction()