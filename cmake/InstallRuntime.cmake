# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Installs what umbra needs at run time besides its own binaries (run by the
# install rules of UmbraInstall.cmake, which set its inputs):
#
#   EXECUTABLE    umbra in the build tree
#   GPU_MODULE    the GPU module in the build tree, or empty
#   USD_LIB_DIR   OpenUSD's lib/, which holds usd/, and whose ../plugin/usd/
#                 holds its other plugins
#   LICENSE_DIRS  directories with a <component>/ of license texts each,
#                 searched in order
#   SOURCE_DIR    umbra's sources, for its own license
#
# It installs OpenUSD's plugins, the libraries that umbra, the GPU module and
# the plugins need and the system does not provide, and the license texts of
# umbra and of every component installed. A library that the table of
# components below does not cover, or a component without license texts,
# stops the install.

foreach(variable IN ITEMS EXECUTABLE GPU_MODULE USD_LIB_DIR LICENSE_DIRS
                          SOURCE_DIR)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "InstallRuntime.cmake needs ${variable}")
  endif()
endforeach()

set(lib_dir "${CMAKE_INSTALL_PREFIX}/lib")
set(usd_plugin_dir "${USD_LIB_DIR}/../plugin/usd")
cmake_path(NORMAL_PATH usd_plugin_dir)

# The value of `key` in a line of a plugInfo.json: "LibraryPath": "<value>".
function(plug_info_value line key out)
  string(REGEX REPLACE ".*\"${key}\": *\"([^\"]*)\".*" "\\1" value "${line}")
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# OpenUSD's plugins. It loads each with dlopen, from the LibraryPath of its
# plugInfo.json, relative to the plugin's Root, which is relative to that
# file. Every plugInfo.json here holds a single plugin. The plugInfo.json at
# the top of each directory includes */resources/plugInfo.json, the plugins'
# own, which these patterns follow.
file(INSTALL "${USD_LIB_DIR}/usd" DESTINATION "${lib_dir}")
file(INSTALL "${usd_plugin_dir}" DESTINATION "${CMAKE_INSTALL_PREFIX}/plugin")
file(GLOB plug_infos "${USD_LIB_DIR}/usd/*/resources/plugInfo.json"
     "${usd_plugin_dir}/*/resources/plugInfo.json")
set(plugins)
foreach(plug_info IN LISTS plug_infos)
  cmake_path(GET plug_info PARENT_PATH plug_info_dir)
  set(plugin_root "${plug_info_dir}")
  file(STRINGS "${plug_info}" root_lines REGEX "\"Root\"")
  if(root_lines)
    list(GET root_lines 0 line)
    plug_info_value("${line}" Root relative_root)
    cmake_path(ABSOLUTE_PATH relative_root BASE_DIRECTORY "${plug_info_dir}"
               OUTPUT_VARIABLE plugin_root)
  endif()
  file(STRINGS "${plug_info}" library_lines REGEX "\"LibraryPath\"")
  foreach(line IN LISTS library_lines)
    plug_info_value("${line}" LibraryPath path)
    cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${plugin_root}" NORMALIZE)
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "OpenUSD has no plugin library ${path}")
    endif()
    list(APPEND plugins "${path}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES plugins)

# The libraries of umbra, of the GPU module and of the plugins, except those
# of the system, which the image takes from the distribution.
file(GET_RUNTIME_DEPENDENCIES
     EXECUTABLES "${EXECUTABLE}"
     MODULES ${GPU_MODULE} ${plugins}
     RESOLVED_DEPENDENCIES_VAR libraries
     UNRESOLVED_DEPENDENCIES_VAR unresolved
     POST_EXCLUDE_REGEXES "^/lib/" "^/lib64/" "^/usr/lib/")
if(unresolved)
  message(FATAL_ERROR "cannot find these libraries of umbra: ${unresolved}")
endif()
# OpenUSD's own libraries are plugins too, loaded only with dlopen; the
# plugins in plugin/usd/ came with their directory.
foreach(plugin IN LISTS plugins)
  cmake_path(GET plugin PARENT_PATH plugin_dir)
  if(plugin_dir STREQUAL USD_LIB_DIR)
    list(APPEND libraries "${plugin}")
  endif()
endforeach()
list(REMOVE_DUPLICATES libraries)
foreach(library IN LISTS libraries)
  file(INSTALL "${library}" DESTINATION "${lib_dir}" FOLLOW_SYMLINK_CHAIN)
endforeach()

# The components of the libraries, by a pattern of the file name, each with
# the header-only components compiled into it: OpenImageIO and OSL contain
# robin-map.
set(component_patterns)
function(component pattern)
  list(LENGTH component_patterns index)
  list(APPEND component_patterns "${pattern}")
  set(component_patterns "${component_patterns}" PARENT_SCOPE)
  set(components_${index} ${ARGN} PARENT_SCOPE)
endfunction()
component("^lib(c\\+\\+|c\\+\\+abi|unwind|LLVM)\\." llvm-project)
component("^libtbb" oneTBB)
component("^libembree" embree)
component("^libImath" Imath)
component("^lib(Iex|IlmThread|OpenEXR)" openexr)
component("^libMaterialX" MaterialX)
component("^libOpenColorIO" OpenColorIO)
component("^libOpenImageIO" OpenImageIO robin-map)
component("^libosl" OpenShadingLanguage robin-map)
component("^libosd" OpenSubdiv)
component("^libopenvdb" openvdb)
component("^libfmt" fmt)
component("^libpugixml" pugixml)
component("^libPtex" ptex)
component("^libpystring" pystring)
component("^libyaml-cpp" yaml-cpp)
component("^libtiff" libtiff)
component("^libusd_" OpenUSD)

set(components OpenUSD)
foreach(library IN LISTS libraries)
  cmake_path(GET library FILENAME name)
  set(found)
  set(index 0)
  foreach(pattern IN LISTS component_patterns)
    if(name MATCHES "${pattern}")
      set(found ${components_${index}})
      break()
    endif()
    math(EXPR index "${index} + 1")
  endforeach()
  if(NOT found)
    message(FATAL_ERROR
            "${name} belongs to no known component; add it to the table of "
            "components in cmake/InstallRuntime.cmake")
  endif()
  list(APPEND components ${found})
endforeach()
list(REMOVE_DUPLICATES components)

# The license texts, and the lists of the third-party code each component
# contains, which some keep next to them.
set(licenses_dir "${CMAKE_INSTALL_PREFIX}/share/licenses")
file(INSTALL "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/NOTICE"
     DESTINATION "${licenses_dir}/umbra")
foreach(component IN LISTS components)
  set(texts)
  foreach(directory IN LISTS LICENSE_DIRS)
    set(base "${directory}/${component}")
    file(GLOB texts LIST_DIRECTORIES false "${base}/LICENSE*"
         "${base}/NOTICE*" "${base}/COPYING*" "${base}/THIRD-PARTY*"
         "${base}/third-party-programs*")
    if(texts)
      break()
    endif()
  endforeach()
  if(NOT texts)
    message(FATAL_ERROR
            "no license text of ${component} in any of ${LICENSE_DIRS}")
  endif()
  file(INSTALL ${texts} DESTINATION "${licenses_dir}/${component}")
endforeach()
