# Static recompiler ARM64 -> C++.
#   core/   loader, link step, function discovery, CFG (P2.1, P2.2)
#   lifter/ A64 decoder and lifter (P2.3, P2.4)
#   codegen/ analysis -> lifter -> C++ with dispatch entries (P2.5), lgp_lift
#   cli/    lgp_recomp command line
add_subdirectory(core)
add_subdirectory(lifter)
add_subdirectory(codegen)
add_subdirectory(cli)
