set(LLVM_LINK_COMPONENTS support)

add_clang_executable(v8-wrench
	wrench.cpp
)

target_link_libraries(v8-wrench
	PRIVATE
	clangTooling
	clangBasic
	clangASTMatchers
)
