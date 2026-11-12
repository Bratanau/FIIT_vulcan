add_executable(lab3
	main.cpp
	application.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../source/graphics_internal.cpp
)

target_include_directories(lab3 PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../source
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends
	${stb_SOURCE_DIR}
)

target_link_libraries(lab3 PRIVATE
	glfw
	Vulkan::Vulkan
	vk-bootstrap::vk-bootstrap
	GPUOpen::VulkanMemoryAllocator
	glm::glm
)

target_sources(lab3 PRIVATE
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

if(MSVC)
	target_compile_options(lab3 PRIVATE /W4 /wd4201)
	target_link_options(lab3 PRIVATE /subsystem:windows /entry:mainCRTStartup)
	target_compile_definitions(lab3 PRIVATE _USE_MATH_DEFINES)
else()
	target_compile_options(lab3 PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/textures DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

find_program(LAB3_GLSLC glslc)
if(LAB3_GLSLC)
	set(_lab3_shader_outputs)
	foreach(_shader shader.vert shader.frag)
		set(_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader})
		set(_output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${_shader}.spv)
		add_custom_command(
			OUTPUT ${_output}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
			COMMAND ${LAB3_GLSLC} ${_source} -o ${_output}
			DEPENDS ${_source}
			COMMENT "Compiling Lab 3 ${_shader}"
			VERBATIM
		)
		list(APPEND _lab3_shader_outputs ${_output})
	endforeach()
	add_custom_target(lab3-shaders DEPENDS ${_lab3_shader_outputs})
	add_dependencies(lab3 lab3-shaders)
else()
	message(WARNING "glslc not found — Lab 3 shaders will not be compiled automatically")
endif()
