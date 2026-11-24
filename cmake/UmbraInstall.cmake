# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The install rules: `cmake --install <build> --prefix <dir>` lays out the
# tree a release ships (docs/adr/0005-releases-are-a-chiseled-image.md):
#
#   bin/umbra
#   lib/                  the GPU module, and the libraries that umbra and
#                         the libraries it loads need and that the system
#                         does not provide: those of the dependency prefix
#                         and of the toolchain
#   lib/usd/              the resources of OpenUSD's libraries, through
#                         which it loads them as plugins
#   plugin/usd/           OpenUSD's other plugins
#   share/licenses/<component>/
#                         the license texts of umbra and of every component
#                         in lib/ and plugin/
#
# The binaries find their libraries through RUNPATH=$ORIGIN/../lib, so the
# tree works from any prefix, without LD_LIBRARY_PATH. The libraries are
# collected when the tree is installed (cmake/InstallRuntime.cmake).
#
# Include it after the targets are defined.

include_guard(GLOBAL)

set(UMBRA_INSTALL_RUNTIME_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/InstallRuntime.cmake")

set(umbra_installed_targets umbra)
set(umbra_gpu_module "")
if(TARGET umbra_gpu_backend)
  list(APPEND umbra_installed_targets umbra_gpu_backend)
  set(umbra_gpu_module "$<TARGET_FILE:umbra_gpu_backend>")
endif()
set_target_properties(${umbra_installed_targets} PROPERTIES
                      INSTALL_RPATH "\$ORIGIN/../lib")
install(TARGETS ${umbra_installed_targets}
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib)

# Where the license texts of the components are: the sources the dependency
# prefix keeps in src/<component>/, and share/licenses/<component>/ of that
# prefix and of the toolchain, as configured: the install does not look at
# the environment again.
cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH umbra_compiler_bin)
string(REPLACE ":" ";" umbra_env_prefixes "$ENV{CMAKE_PREFIX_PATH}")
set(umbra_license_dirs)
foreach(umbra_prefix IN LISTS CMAKE_PREFIX_PATH umbra_env_prefixes)
  list(APPEND umbra_license_dirs "${umbra_prefix}/src"
       "${umbra_prefix}/share/licenses")
endforeach()
list(APPEND umbra_license_dirs "${umbra_compiler_bin}/../share/licenses")

# The dependencies are scanned in the build tree, whose binaries find the
# dependency prefix through their build RUNPATH.
install(CODE "
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM linux+elf)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND \"${CMAKE_OBJDUMP}\")
  set(EXECUTABLE \"$<TARGET_FILE:umbra>\")
  set(GPU_MODULE \"${umbra_gpu_module}\")
  set(USD_LIB_DIR \"${pxr_DIR}/lib\")
  set(LICENSE_DIRS \"${umbra_license_dirs}\")
  set(SOURCE_DIR \"${PROJECT_SOURCE_DIR}\")
  include(\"${UMBRA_INSTALL_RUNTIME_SCRIPT}\")
")
