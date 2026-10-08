# The 3dokit runtime for recompiled ARM60 code, included by the generated
# CMakeLists.txt (python -m 3dokit.recomp).
#   tdk_arm_core  guest memory, the active module, dispatch, the return check
#   tdk_arm_stub  no OS and no hardware: tests that only run the game's own code
#   tdk_pf        Portfolio at the folio boundary (pf.h); it needs the host's threads
# A target links tdk_arm_core, one set of services (tdk_arm_stub or tdk_pf),
# then the generated library `recomp`.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(tdk_arm_core OBJECT ${_rt}/arm_core.cpp)
add_library(tdk_arm_stub OBJECT ${_rt}/arm_stub.cpp)
add_library(tdk_pf OBJECT ${_rt}/pf_os.cpp ${_rt}/pf_kernel.cpp ${_rt}/pf_mem.cpp ${_rt}/pf_io.cpp ${_rt}/pf_file.cpp
            ${_rt}/pf_graphics.cpp ${_rt}/pf_audio.cpp ${_rt}/pf_dsp.cpp ${_rt}/pf_task.cpp ${_rt}/pf_time.cpp
            ${_rt}/pf_cel.cpp ${_rt}/pf_memtest.cpp ${_rt}/pf_msg.cpp ${_rt}/pf_event.cpp ${_rt}/pf_math.cpp ${_rt}/pf_aif.cpp ${_rt}/pf_font.cpp ${_rt}/pf_err.cpp)
find_package(Threads REQUIRED)
target_link_libraries(tdk_pf PUBLIC Threads::Threads)    # a host thread per task (pf_task.cpp)
foreach(t tdk_arm_core tdk_arm_stub tdk_pf)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
# pfboot's window (pf_window.cpp, pfboot --window) when SDL3 is found: the generated CMakeLists.txt
# makes pfboot after including this file, so the window joins it at the end of the directory.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  set(TDK_WINDOW_SRC ${_rt}/pf_window.cpp)
  cmake_language(DEFER CALL tdk_add_window)
endif()
function(tdk_add_window)
  if(TARGET pfboot)
    target_sources(pfboot PRIVATE ${TDK_WINDOW_SRC})
    if(TARGET SDL3::SDL3-static)
      target_link_libraries(pfboot SDL3::SDL3-static)
    else()
      target_link_libraries(pfboot SDL3::SDL3)
    endif()
    target_compile_definitions(pfboot PRIVATE TDK_WINDOW=1)
  endif()
endfunction()
