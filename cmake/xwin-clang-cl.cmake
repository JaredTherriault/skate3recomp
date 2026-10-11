set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED XWIN_DIR)
    set(XWIN_DIR "$ENV{HOME}/.xwin" CACHE PATH "Path to xwin directory")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(XWIN_COMPILE_FLAGS "-vctoolsdir ${XWIN_DIR}/crt -winsdkdir ${XWIN_DIR}/sdk /DWIN32 /D_WINDOWS /w")

set(CMAKE_C_FLAGS_INIT "${XWIN_COMPILE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${XWIN_COMPILE_FLAGS}")

set(XWIN_LINK_FLAGS "/libpath:${XWIN_DIR}/crt/lib/x64 /libpath:${XWIN_DIR}/sdk/lib/um/x64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")

set(CMAKE_RC_FLAGS_INIT "-I ${XWIN_DIR}/sdk/include/um -I ${XWIN_DIR}/sdk/include/shared -I ${XWIN_DIR}/crt/include -I ${XWIN_DIR}/sdk/include/ucrt")

set(CMAKE_C_SIMULATE_ID "MSVC")
set(CMAKE_CXX_SIMULATE_ID "MSVC")
