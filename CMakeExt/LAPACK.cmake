
# set (BLA_STATIC ON)
find_package(BLAS)
find_package(LAPACK)

if (BLAS_FOUND)
  check_library_exists(${BLAS_LIBRARIES} "cblas_sgemm" "" BLAS_IS_CBLAS)
  if(${BLAS_IS_CBLAS})
  else()
    message(STATUS "No CBLAS Interface found, try manually")
    find_library(LIBCBLAS_LIBRARY NAMES cblas HINTS "${BLAS_DIR}")
    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(LIBCBLAS
      DEFAULT_MSG LIBCBLAS_LIBRARY)
    if(${LIBCBLAS_FOUND})
      set(BLAS_LIBRARIES ${LIBCBLAS_LIBRARY})
    else()
      unset(BLAS_FOUND)
    endif()
  endif()
  message(STATUS "BLAS includes:  " ${BLAS_INCLUDE_DIRS})
  message(STATUS "BLAS libraries: " ${BLAS_LIBRARIES})
endif()

if (LAPACK_FOUND)
  if ("${LAPACK_INCLUDE_DIRS}" STREQUAL "")
    # Temporary workaround
    set(LAPACK_INCLUDE_DIRS "/usr/include/atlas")
  endif()
  message(STATUS "LAPACK includes:  " ${LAPACK_INCLUDE_DIRS})
  message(STATUS "LAPACK libraries: " ${LAPACK_LIBRARIES})
else()
  message(STATUS "LAPACK not found")
endif()

