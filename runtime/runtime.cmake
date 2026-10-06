# The 3dokit runtime for recompiled ARM60 code, included by the generated
# CMakeLists.txt (python -m 3dokit.recomp).
#   tdk_arm_core  guest memory, the active module, dispatch, the return check
#   tdk_arm_stub  no OS and no hardware: tests that only run the game's own code
# A target links tdk_arm_core, one set of services (tdk_arm_stub for now),
# then the generated library `recomp`.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(tdk_arm_core OBJECT ${_rt}/arm_core.cpp)
add_library(tdk_arm_stub OBJECT ${_rt}/arm_stub.cpp)
foreach(t tdk_arm_core tdk_arm_stub)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
