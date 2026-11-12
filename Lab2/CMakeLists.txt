add_executable(lab2
	main.cpp
	application.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../source/graphics_internal.cpp
)

target_include_directories(lab2 PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../source
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends
)

target_link_libraries(lab2 PRIVATE
	glfw
	Vulkan::Vulkan
	vk-bootstrap::vk-bootstrap
	GPUOpen::VulkanMemoryAllocator
	glm::glm
)

target_sources(lab2 PRIVATE
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

if(MSVC)
	target_compile_options(lab2 PRIVATE /W4 /wd4201)
	target_link_options(lab2 PRIVATE /subsystem:windows /entry:mainCRTStartup)
	target_compile_definitions(lab2 PRIVATE _USE_MATH_DEFINES)
else()
	target_compile_options(lab2 PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

find_program(LAB2_GLSLC glslc)
if(LAB2_GLSLC)
	set(_lab2_shader_outputs)
	foreach(_shader shader.vert shader.frag)
		set(_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader})
		set(_output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${_shader}.spv)
		add_custom_command(
			OUTPUT ${_output}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
			COMMAND ${LAB2_GLSLC} ${_source} -o ${_output}
			DEPENDS ${_source}
			COMMENT "Compiling Lab 2 ${_shader}"
			VERBATIM
		)
		list(APPEND _lab2_shader_outputs ${_output})
	endforeach()
	add_custom_target(lab2-shaders DEPENDS ${_lab2_shader_outputs})
	add_dependencies(lab2 lab2-shaders)
else()
	message(WARNING "glslc not found — Lab 2 shaders will not be compiled automatically")
endif()
