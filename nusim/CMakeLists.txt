cmake_minimum_required(VERSION 3.8)
project(nusim)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(std_msgs REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(std_srvs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(nuturtlebot_msgs REQUIRED)
find_package(turtlelib REQUIRED)
find_package(leo_ros_utils REQUIRED)
find_package(nav_msgs REQUIRED)

find_package(visualization_msgs REQUIRED)

find_package(rosidl_default_generators REQUIRED)
find_package(Doxygen)
option(BUILD_DOCS "Build the documentation" OFF)

rosidl_generate_interfaces(
  ${PROJECT_NAME}_IDL
  "srv/Teleport.srv"
  LIBRARY_NAME
  ${PROJECT_NAME} # This is a necessary line. And it MUST be PROJECT_NAME !!
  # DEPENDENCIES geometry_msgs # Add packages that above messages depend on, in
  # this case geometry_msgs for Sphere.msg
)

# NOTE:
# LIBRARY_NAME ${PROJECT_NAME} # This is a necessary line, Otherweise will get
# This error:
#
# [nusim-1] terminate called after throwing an instance of
# 'rclcpp::exceptions::RCLError' [nusim-1]   what():  could not create service:
# Type support not from this implementation. Got: [nusim-1]     Handle's
# typesupport identifier (rosidl_typesupport_cpp) is not supported by this
# library, at ./src/type_support_dispatch.hpp:111 [nusim-1]     Could not load
# library libnusim__rosidl_typesupport_fastrtps_cpp.so: dlopen error:
# libnusim__rosidl_typesupport_fastrtps_cpp.so: cannot open shared object file:
# No such file or directory, at ./src/shared_library.c:99, at
# ./src/type_support_dispatch.hpp:76
#
# Which in build folder: libnusim_srv__rosidl_typesupport_fastrtps_cpp.so
# exsits, but the Executable is looking for
# libnusim__rosidl_typesupport_fastrtps_cpp.so

rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME}_IDL
                              "rosidl_typesupport_cpp")

add_executable(nusim src/nusim.cpp)

# https://robotics.stackexchange.com/questions/23171/creating-a-custom-msg-in-ros2
ament_target_dependencies(
  nusim
  rclcpp
  std_msgs
  std_srvs
  tf2
  tf2_ros
  visualization_msgs
  nav_msgs
  nuturtlebot_msgs
  leo_ros_utils
  )
target_link_libraries(nusim ${cpp_typesupport_target} turtlelib::turtlelib)
install(TARGETS nusim DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY
  launch
  srv # Since this is built, I shouldn't need to install it?
  config DESTINATION share/${PROJECT_NAME}/)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights comment the
  # line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo) comment the line
  # when this package is in a git repo and when a copyright and license is added
  # to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()
if(${DOXYGEN_FOUND} AND ${BUILD_DOCS})
    # Turn the README.md into the homepage of the doxygen docs
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE README.md)

    # Tell Doxygen where to find the documentation
    doxygen_add_docs(doxygen include/ src/ README.md ALL)

    # The documentation will be in the build/html directory
    # The main page is build/html/index.html
endif()

ament_package()
