# Overlay triplet: vcpkg's builtin x64-windows-static-md, plus release-only.
#
# The wheel build linked vcpkg's *debug* snappy into a *release* module:
#
#   snappy.lib : error LNK2038: mismatch detected for 'RuntimeLibrary':
#     value 'MDd_DynamicDebug' doesn't match value 'MD_DynamicRelease'
#   snappy.lib : error LNK2001: unresolved external symbol __imp__CrtDbgReport
#   fatal error LNK1120: 1 unresolved externals
#
# pypi-deploy.yml already intended release-only -- it sets VCPKG_BUILD_TYPE in
# CIBW_ENVIRONMENT_WINDOWS -- but VCPKG_BUILD_TYPE is a triplet variable, not an
# environment variable, so vcpkg ignored it and kept building both
# configurations. This file is where it actually takes effect.
#
# Named identically to the builtin triplet so VCPKG_TARGET_TRIPLET, the cache
# key and CMAKE_ARGS are all unchanged; overlay triplets take precedence.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Wheels are built Release (pyproject.toml: cmake.build-type = "Release"), so a
# debug half is never linked -- only available to be linked by mistake. Building
# one configuration instead of two also roughly halves vcpkg time.
set(VCPKG_BUILD_TYPE release)
