# bpf-tv is built THROUGH alive2's EXTERNAL_PROJECTS hook (an
# LLVM_EXTERNAL_PROJECTS analogue, alive2 commit eac00b2d): alive2's
# top-level CMake add_subdirectory()s this file, putting alive2's
# library targets, include dirs, ${Z3_LIBRARIES} etc. in scope.
#
#   cmake -S third_party/alive2 -B $BUILD_ROOT/alive2 -G Ninja \
#     -DBUILD_LLVM_UTILS=ON -DLLVM_DIR=... \
#     -DEXTERNAL_PROJECTS=bpf-tv \
#     -DEXTERNAL_BPF_TV_SOURCE_DIR=<this directory>
#
# scripts/build-deps.sh does exactly this.

project(bpf-tv)

# the lifter uses LLVM build-tree tablegen output (Target/BPF/*.inc)
# and MCTargetDesc headers from the LLVM source tree; neither is part
# of an installed LLVM
set(BPF_TV_LLVM_TARGET_INCLUDES
  ${LLVM_BINARY_DIR}/lib
  ${LLVM_MAIN_INCLUDE_DIR}/../lib
)

set(BPF_TV_LIFTER_SRCS
  src/lifter/bpf2llvm.cpp
  src/lifter/lifter.cpp
  src/lifter/mc2llvm.cpp
  src/lifter/run_backend.cpp
  src/lifter/streamerwrapper.cpp
)

# aarch64/riscv components are pulled in because alive2's
# llvm_util::optimize_module initializes those targets unconditionally;
# orcjit/native serve the conformance plugin's host JIT
llvm_map_components_to_libnames(BPF_TV_LLVM_LIBS
  CodeGen TargetParser support core irreader bitwriter analysis passes
  transformutils ipo asmparser asmprinter target mc mcparser mcdisassembler
  bpfasmparser bpfcodegen bpfdesc bpfdisassembler bpfinfo
  aarch64codegen aarch64desc aarch64info
  riscvcodegen riscvdesc riscvinfo
  orcjit native
)

add_library(bpftv_lifter STATIC ${BPF_TV_LIFTER_SRCS})
target_include_directories(bpftv_lifter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${BPF_TV_LLVM_TARGET_INCLUDES}
)

add_llvm_executable(bpf-tv src/bpf-tv.cpp)
target_link_libraries(bpf-tv PRIVATE
  bpftv_lifter
  ${ALIVE_LIBS_LLVM}
  ${Z3_LIBRARIES}
  ${HIREDIS_LIBRARIES}
  ${BPF_TV_LLVM_LIBS}
)

add_llvm_executable(bpf-tv-conformance-plugin src/conformance/plugin.cpp)
target_link_libraries(bpf-tv-conformance-plugin PRIVATE
  bpftv_lifter
  ${ALIVE_LIBS_LLVM}
  ${Z3_LIBRARIES}
  ${HIREDIS_LIBRARIES}
  ${BPF_TV_LLVM_LIBS}
)
