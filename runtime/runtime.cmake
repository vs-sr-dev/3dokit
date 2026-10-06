# The 3dokit runtime for recompiled ARM60 code, included by the generated
# CMakeLists.txt (python -m 3dokit.recomp).
#   tdk_arm_core  guest memory, the active module, dispatch, the return check
#   tdk_arm_stub  no OS and no hardware: tests that only run the game's own code
#   tdk_pf        Portfolio at the folio boundary (pf.h)
# A target links tdk_arm_core, one set of services (tdk_arm_stub or tdk_pf),
# then the generated library `recomp`.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(tdk_arm_core OBJECT ${_rt}/arm_core.cpp)
add_library(tdk_arm_stub OBJECT ${_rt}/arm_stub.cpp)
add_library(tdk_pf OBJECT ${_rt}/pf_os.cpp ${_rt}/pf_kernel.cpp ${_rt}/pf_mem.cpp ${_rt}/pf_io.cpp ${_rt}/pf_file.cpp
            ${_rt}/pf_graphics.cpp ${_rt}/pf_memtest.cpp)
foreach(t tdk_arm_core tdk_arm_stub tdk_pf)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
