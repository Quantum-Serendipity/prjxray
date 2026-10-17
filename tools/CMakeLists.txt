add_executable(bitread bitread.cc)
target_link_libraries(bitread absl::optional absl::strings gflags libprjxray)

add_executable(segmatch segmatch.cc)
target_link_libraries(segmatch gflags absl::strings)

add_executable(bittool bittool.cc)
target_link_libraries(bittool libprjxray gflags absl::strings absl::span)

add_executable(frame_address_decoder frame_address_decoder.cc)
target_link_libraries(frame_address_decoder libprjxray)

add_executable(gen_part_base_yaml gen_part_base_yaml.cc)
target_link_libraries(gen_part_base_yaml
	absl::optional
	absl::span
	libprjxray
	yaml-cpp
	gflags
)
add_executable(xc7patch xc7patch.cc)
target_link_libraries(xc7patch
	absl::strings
	absl::time
	gflags
	libprjxray
)

add_executable(xc7frames2bit xc7frames2bit.cc)
target_link_libraries(xc7frames2bit
	absl::strings
	absl::time
	gflags
	libprjxray
)
set(XC7DPR_PRJXRAY_REV "unknown" CACHE STRING "prjxray fork revision reported by xc7frames2bit --version")
target_compile_definitions(xc7frames2bit PRIVATE XC7DPR_PRJXRAY_REV="${XC7DPR_PRJXRAY_REV}")

add_subdirectory(bits2rbt)

if(PRJXRAY_BUILD_TESTING)
	add_test(NAME xc7frames2bit_test
		COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/xc7frames2bit_test.sh
			$<TARGET_FILE:xc7frames2bit>
			${PROJECT_SOURCE_DIR}/lib/test_data/configuration_test.yaml)
	add_test(NAME xc7frames2bit_version_test
		COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/xc7frames2bit_version_test.sh
			$<TARGET_FILE:xc7frames2bit>
			${XC7DPR_PRJXRAY_REV})
	add_test(NAME xc7patch_test
		COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/xc7patch_test.sh
			$<TARGET_FILE:xc7frames2bit>
			$<TARGET_FILE:xc7patch>
			${PROJECT_SOURCE_DIR}/lib/test_data/configuration_test.yaml)
endif()

install(TARGETS xc7frames2bit xc7patch gen_part_base_yaml frame_address_decoder bittool segmatch bitread DESTINATION bin)
