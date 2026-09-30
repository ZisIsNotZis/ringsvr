cmake_minimum_required(VERSION 3.16)
project(ringsvr C)

set(CMAKE_C_STANDARD 11)

add_executable(ringsvr main.c)

# io_uring (liburing). For a non-system install, point -DLIBURING_ROOT at a
# prefix with headers in include/ and libs in lib/ or lib/<arch>-linux-gnu/.
if(LIBURING_ROOT)
  target_include_directories(ringsvr PRIVATE ${LIBURING_ROOT}/include)
  file(GLOB _uring_libdirs ${LIBURING_ROOT}/lib ${LIBURING_ROOT}/lib/*-linux-gnu)
  target_link_directories(ringsvr PRIVATE ${_uring_libdirs})
endif()

target_link_libraries(ringsvr PRIVATE uring)
