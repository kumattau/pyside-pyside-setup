include(CheckIncludeFileCXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

include(ShibokenHelpers)

option(BUILD_TESTS "Build tests." TRUE)
option(USE_PYTHON_VERSION "Use specific python version to build shiboken6." "")
option(DISABLE_DOCSTRINGS "Disable documentation extraction." FALSE)

shiboken_internal_disable_pkg_config_if_needed()

set(QT_MAJOR_VERSION 6)
message(STATUS "Using Qt ${QT_MAJOR_VERSION}")
find_package(Qt${QT_MAJOR_VERSION} 6.0 REQUIRED COMPONENTS Core)

if(QUIET_BUILD)
    set_quiet_build()
endif()

if (USE_PYTHON_VERSION)
    shiboken_find_required_python(${USE_PYTHON_VERSION})
else()
    shiboken_find_required_python()
endif()

setup_clang()

set(SHIBOKEN_VERSION_FILE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/shiboken_version.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${SHIBOKEN_VERSION_FILE_PATH}
)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} "${SHIBOKEN_VERSION_FILE_PATH}"
  OUTPUT_VARIABLE SHIBOKEN_VERSION_OUTPUT
  ERROR_VARIABLE SHIBOKEN_VERSION_OUTPUT_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT SHIBOKEN_VERSION_OUTPUT)
    message(FATAL_ERROR "Could not identify shiboken version. \
                         Error: ${SHIBOKEN_VERSION_OUTPUT_ERROR}")
endif()

list(GET SHIBOKEN_VERSION_OUTPUT 0 shiboken_MAJOR_VERSION)
list(GET SHIBOKEN_VERSION_OUTPUT 1 shiboken_MINOR_VERSION)
list(GET SHIBOKEN_VERSION_OUTPUT 2 shiboken_MICRO_VERSION)
# a - alpha, b - beta, rc - rc
list(GET SHIBOKEN_VERSION_OUTPUT 3 shiboken_PRE_RELEASE_VERSION_TYPE)
# the number of the pre release (alpha1, beta3, rc7, etc.)
list(GET SHIBOKEN_VERSION_OUTPUT 4 shiboken_PRE_RELEASE_VERSION)

set(shiboken6_VERSION "${shiboken_MAJOR_VERSION}.${shiboken_MINOR_VERSION}.${shiboken_MICRO_VERSION}")
set(shiboken6_library_so_version "${shiboken_MAJOR_VERSION}.${shiboken_MINOR_VERSION}")

compute_config_py_values(shiboken6_VERSION)

## For debugging the PYTHON* variables
message(STATUS "PYTHONLIBS_FOUND:       " ${PYTHONLIBS_FOUND})
message(STATUS "PYTHON_LIBRARIES:       " ${PYTHON_LIBRARIES})
message(STATUS "PYTHON_INCLUDE_DIRS:    " ${PYTHON_INCLUDE_DIRS})
message(STATUS "PYTHON_DEBUG_LIBRARIES: " ${PYTHON_DEBUG_LIBRARIES})
message(STATUS "PYTHONINTERP_FOUND:     " ${PYTHONINTERP_FOUND})
message(STATUS "PYTHON_EXECUTABLE:      " ${PYTHON_EXECUTABLE})
message(STATUS "PYTHON_VERSION:         " ${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}.${PYTHON_VERSION_PATCH})

if(NOT PYTHON_EXTENSION_SUFFIX)
    get_python_extension_suffix()
endif()

option(FORCE_LIMITED_API "Enable the limited API." "yes")
set(PYTHON_LIMITED_API 0)

shiboken_check_if_limited_api()

if(PYTHON_LIMITED_API)
    set_limited_api()
endif()

if(NOT PYTHON_CONFIG_SUFFIX)
    set_python_config_suffix()
endif()

set(PYTHON_SHARED_LIBRARY_SUFFIX "${PYTHON_CONFIG_SUFFIX}")

if(NOT PYTHON_CONFIG_SUFFIX)
    message(FATAL_ERROR
        "PYTHON_CONFIG_SUFFIX is empty. It should never be empty. Please file a bug report.")
endif()

message(STATUS "PYTHON_EXTENSION_SUFFIX:      ${PYTHON_EXTENSION_SUFFIX}")
message(STATUS "PYTHON_CONFIG_SUFFIX:         ${PYTHON_CONFIG_SUFFIX}")
message(STATUS "PYTHON_SHARED_LIBRARY_SUFFIX: ${PYTHON_SHARED_LIBRARY_SUFFIX}")


if(NOT PYTHON_SITE_PACKAGES)
    set_python_site_packages()
endif()

set_cmake_cxx_flags()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D QT_NO_CAST_FROM_ASCII -D QT_NO_CAST_TO_ASCII")

# Force usage of the C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
set(LIB_INSTALL_DIR "lib${LIB_SUFFIX}" CACHE PATH "The subdirectory relative to the install \
    prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)
set(BIN_INSTALL_DIR "bin" CACHE PATH "The subdirectory relative to the install prefix where \
    dlls will be installed (default is /bin)" FORCE)

if(WIN32)
    set(PATH_SEP "\;")
else()
    set(PATH_SEP ":")
endif()

if(CMAKE_HOST_APPLE)
    set(OSX_USE_LIBCPP "OFF" CACHE BOOL "Explicitly link the libc++ standard library \
        (useful for osx deployment targets lower than 10.9.")
    if(OSX_USE_LIBCPP)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

# Build with Address sanitizer enabled if requested.
# This may break things, so use at your own risk.
if(SANITIZE_ADDRESS AND NOT MSVC)
    setup_sanitize_address()
endif()

# Detect if the python libs were compiled in debug mode
# On Linux distros there is no standard way to check that.
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        import sys
        import sysconfig
        config_py_debug = sysconfig.get_config_var('Py_DEBUG')
        print(bool(config_py_debug))
        "
    OUTPUT_VARIABLE PYTHON_WITH_DEBUG
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Detect if python interpeter was compiled with COUNT_ALLOCS define
# Linux distros are inconsistent in setting the sysconfig.get_config_var('COUNT_ALLOCS') value
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        count_allocs = False
        import sys
        try:
            if sys.getcounts:
                count_allocs = True
        except:
            pass

        print(bool(count_allocs))
        "
    OUTPUT_VARIABLE PYTHON_WITH_COUNT_ALLOCS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(SHIBOKEN_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_debug_build()
endif()

######################################################################
## Define the Python files involved in the build process.
##
## They are installed into the file system (see shibokenmodule)
## and embedded into the libshiboken binary through a .zip file.
######################################################################

set(shiboken_python_files
    "signature/lib/__init__.py"
    "signature/lib/enum_sig.py"
    "signature/lib/pyi_generator.py"
    "signature/lib/tool.py"
    "signature/__init__.py"
    "signature/errorhandler.py"
    "signature/importhandler.py"
    "signature/layout.py"
    "signature/loader.py"
    "signature/mapping.py"
    "signature/parser.py"
    "__init__.py"
    "feature.py"
    )

# uninstall target
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
               IMMEDIATE @ONLY)
add_custom_target(uninstall "${CMAKE_COMMAND}"
                  -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")

set(generator_plugin_DIR ${LIB_INSTALL_DIR}/generatorrunner${generator_SUFFIX})
