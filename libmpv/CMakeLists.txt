add_library(libmpv INTERFACE IMPORTED GLOBAL)

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  target_include_directories(libmpv SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Windows/include
  )
  target_link_libraries(libmpv INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Windows/libmpv-2.lib
  )
endif()  # CMAKE_SYSTEM_NAME
