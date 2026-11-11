#include "stdafx.h"

#include "VKShaderInterpreter.h"
#include "VKVertexProgram.h"
#include "VKFragmentProgram.h"
#include "VKHelpers.h"
#include "VKRenderPass.h"

#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog.h"
#include "Emu/RSX/Program/GLSLCommon.h"
#include "Emu/RSX/Program/ShaderInterpreter.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/localized_string.h"
#include "Emu/PBD/pbd_shader_build.h"
#include "Emu/RSX/Core/RSXContext.h"

#include "Utilities/File.h"

#include <chrono>
#include <thread>
#include <unordered_set>

namespace vk
{
	using enum program_common::interpreter::compiler_option;
	using enum program_common::interpreter::cached_pipeline_flags;

	// Built ahead of play from a record; the first draw that uses it refreshes the record so it stays among the newest.
	constexpr u32 CACHED_PIPE_PREBUILT = (1u << 8);

	static u64 elapsed_ms(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
	}

	// The pipeline state as recorded and hashed: chained structures and the attachment pointer cleared, since they
	// differ between processes and every build points them at its own copy.
	static vk::pipeline_props portable_state(const vk::pipeline_props& properties)
	{
		vk::pipeline_props result = properties;
		result.state.ia.pNext = nullptr;
		result.state.ds.pNext = nullptr;
		result.state.cs.pNext = nullptr;
		result.state.cs.pAttachments = nullptr;
		result.state.rs.pNext = nullptr;
		result.state.ms.pNext = nullptr;
		result.state.ms.pSampleMask = nullptr;
		return result;
	}

	class async_pipe_compiler_context
	{
		vk::pipeline_props m_properties{};
		VkDevice m_device = VK_NULL_HANDLE;
		VkShaderModule m_vs = VK_NULL_HANDLE;
		VkShaderModule m_fs = VK_NULL_HANDLE;

		VkPipelineShaderStageCreateInfo m_shader_stages[2];
		VkPipelineDynamicStateCreateInfo m_dynamic_state_info{};
		VkPipelineVertexInputStateCreateInfo m_vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineViewportStateCreateInfo m_vp{};
		VkPipelineMultisampleStateCreateInfo m_ms{};
		VkPipelineColorBlendStateCreateInfo m_cs{};
		VkPipelineTessellationStateCreateInfo m_ts{};

		std::vector<VkDynamicState> m_dynamic_state_descriptors
		{
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
			VK_DYNAMIC_STATE_LINE_WIDTH,
			VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
			VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
			VK_DYNAMIC_STATE_STENCIL_REFERENCE,
			VK_DYNAMIC_STATE_DEPTH_BIAS
		};

	public:

		async_pipe_compiler_context(
			const vk::pipeline_props& props,
			VkDevice device,
			VkShaderModule vs,
			VkShaderModule fs)
			: m_properties(props)
			, m_device(device)
			, m_vs(vs)
			, m_fs(fs)
		{
		}

		VkGraphicsPipelineCreateInfo compile()
		{
			m_shader_stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			m_shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			m_shader_stages[0].module = m_vs;
			m_shader_stages[0].pName = "main";

			m_shader_stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO  };
			m_shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			m_shader_stages[1].module = m_fs;
			m_shader_stages[1].pName = "main";

			if (vk::get_current_renderer()->get_depth_bounds_support())
			{
				m_dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
			}

			m_dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			m_dynamic_state_info.pDynamicStates = m_dynamic_state_descriptors.data();
			m_dynamic_state_info.dynamicStateCount = ::size32(m_dynamic_state_descriptors);

			m_vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			m_vp.viewportCount = 1;
			m_vp.scissorCount = 1;

			m_ms = m_properties.state.ms;
			ensure(m_ms.rasterizationSamples == VkSampleCountFlagBits((m_properties.renderpass_key >> 16) & 0xF)); // "Multisample state mismatch!"
			if (m_ms.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
			{
				// Update the sample mask pointer
				m_ms.pSampleMask = &m_properties.state.temp_storage.msaa_sample_mask;
			}

			// Rebase pointers from pipeline structure in case it is moved/copied
			m_cs = m_properties.state.cs;
			m_cs.pAttachments = m_properties.state.att_state;

			m_ts = {};
			m_ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;

			VkGraphicsPipelineCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			info.pVertexInputState = &m_vi;
			info.pInputAssemblyState = &m_properties.state.ia;
			info.pRasterizationState = &m_properties.state.rs;
			info.pColorBlendState = &m_cs;
			info.pMultisampleState = &m_ms;
			info.pViewportState = &m_vp;
			info.pDepthStencilState = &m_properties.state.ds;
			info.pTessellationState = &m_ts;
			info.stageCount = 2;
			info.pStages = m_shader_stages;
			info.pDynamicState = &m_dynamic_state_info;
			info.layout = VK_NULL_HANDLE;
			info.basePipelineIndex = -1;
			info.basePipelineHandle = VK_NULL_HANDLE;
			info.renderPass = vk::get_renderpass(m_device, m_properties.renderpass_key);
			return info;
		}
	};

	u32 shader_interpreter::init(std::shared_ptr<VKVertexProgram>& vk_prog, u64 compiler_options) const
	{
		std::memset(&vk_prog->binding_table, 0xff, sizeof(vk_prog->binding_table));

		u32 location = 0;
		vk_prog->binding_table.vertex_buffers_location = location;
		location += 3;

		vk_prog->binding_table.context_buffer_location = location++;

		if (vk::emulate_conditional_rendering())
		{
			vk_prog->binding_table.cr_pred_buffer_location = location++;
		}

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			vk_prog->binding_table.instanced_lut_buffer_location = location++;
			vk_prog->binding_table.instanced_cbuf_location = location++;
		}
		else
		{
			vk_prog->binding_table.cbuf_location = location++;
		}

		// Return next index
		return location;
	}

	u32 shader_interpreter::init(std::shared_ptr<VKFragmentProgram>& vk_prog, u64 /*compiler_opt*/) const
	{
		std::memset(&vk_prog->binding_table, 0xff, sizeof(vk_prog->binding_table));

		vk_prog->binding_table.context_buffer_location = 0;
		vk_prog->binding_table.tex_param_location = 1;
		vk_prog->binding_table.polygon_stipple_params_location = 2;

		// Return next index
		return 3;
	}

	std::shared_ptr<VKVertexProgram> shader_interpreter::build_vs(u64 compiler_options)
	{
		compiler_options &= COMPILER_OPT_ALL_VS_MASK;
		{
			reader_lock lock(m_vs_shader_cache_lock);
			if (auto found = m_vs_shader_cache.find(compiler_options);
				found != m_vs_shader_cache.end())
			{
				return found->second;
			}
		}

		::glsl::shader_properties properties{};
		properties.domain = ::glsl::program_domain::glsl_vertex_program;
		properties.require_lit_emulation = true;
		properties.require_clip_plane_functions = true;

		RSXVertexProgram null_prog;
		std::string shader_str;
		ParamArray arr;

		// Initialize binding layout
		auto vk_prog = std::make_shared<VKVertexProgram>();
		const u32 vertex_instruction_start = init(vk_prog, compiler_options);

		null_prog.ctrl = (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
			? RSX_SHADER_CONTROL_INSTANCED_CONSTANTS
			: 0;
		VKVertexDecompilerThread comp(null_prog, shader_str, arr, *vk_prog);

		// Initialize compiler properties
		comp.properties.has_indexed_constants = true;

		ParamType uniforms = { PF_PARAM_UNIFORM, "vec4" };
		uniforms.items.emplace_back("vc[468]", -1);

		std::stringstream builder;
		comp.insertHeader(builder);
		comp.insertConstants(builder, { uniforms });
		comp.insertInputs(builder, {});

		// Outputs
		builder << "layout(location=16) out flat uvec4 draw_params_payload;\n\n";

		builder <<
		"#define xform_constants_offset get_draw_params().xform_constants_offset\n"
		"#define scale_offset_mat get_vertex_context().scale_offset_mat\n"
		"#define transform_branch_bits get_vertex_context().transform_branch_bits\n"
		"#define point_size get_vertex_context().point_size\n"
		"#define z_near get_vertex_context().z_near\n"
		"#define z_far get_vertex_context().z_far\n\n";

		// Insert vp stream input
		builder << "\n"
		"layout(std140, set=0, binding=" << vertex_instruction_start << ") readonly restrict buffer VertexInstructionBlock\n"
		"{\n"
		"	uint base_address;\n"
		"	uint entry;\n"
		"	uint output_mask;\n"
		"	uint control;\n"
		"	uvec4 vp_instructions[];\n"
		"};\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_VTX_TEXTURES)
		{
			// FIXME: Unimplemented
			rsx_log.todo("Vertex textures are currently not implemented for the shader interpreter.");
		}

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			builder << "#define _ENABLE_INSTANCED_CONSTANTS\n";
		}

		if (compiler_options)
		{
			builder << "\n";
		}

		::glsl::insert_glsl_legacy_function(builder, properties);
		::glsl::insert_vertex_input_fetch(builder, ::glsl::glsl_rules::glsl_rules_vulkan);
		comp.insertFSExport(builder);

		builder << program_common::interpreter::get_vertex_interpreter();
		const std::string s = builder.str();

		auto vs = &vk_prog->shader;
		vs->create(::glsl::program_domain::glsl_vertex_program, s);
		vs->compile();

		// Declare local inputs
		auto vs_inputs = comp.get_inputs();

		vs_inputs.push_back(vk::glsl::program_input::make
		(
			::glsl::glsl_vertex_program,
			"VertexInstructionBlock",
			glsl::input_type_storage_buffer,
			0,
			vertex_instruction_start
		));

		vk_prog->SetInputs(vs_inputs);

		// The builder thread and the RSX thread can build the same options at once. The first one stays: a deferred link
		// holds only its module handle, so a cached shader is never replaced.
		std::lock_guard lock(m_vs_shader_cache_lock);
		return m_vs_shader_cache.try_emplace(compiler_options, vk_prog).first->second;
	}

	std::shared_ptr<VKFragmentProgram> shader_interpreter::build_fs(u64 compiler_options)
	{
		compiler_options &= COMPILER_OPT_ALL_FS_MASK;
		{
			reader_lock lock(m_fs_shader_cache_lock);
			if (auto found = m_fs_shader_cache.find(compiler_options);
				found != m_fs_shader_cache.end())
			{
				return found->second;
			}
		}

		::glsl::shader_properties properties
		{
			.domain = ::glsl::program_domain::glsl_fragment_program,
			.require_lit_emulation = true,
			.ROP_channel_remap = !!(compiler_options & COMPILER_OPT_ENABLE_ROP_REMAP),
		};

		u32 len;
		ParamArray arr;
		std::string shader_str;
		RSXFragmentProgram frag;

		frag.ctrl |= RSX_SHADER_CONTROL_INTERPRETER_MODEL;

		auto vk_prog = std::make_shared<VKFragmentProgram>();
		const u32 fragment_instruction_start = init(vk_prog, compiler_options);
		const u32 fragment_textures_start = fragment_instruction_start + 1;

		VKFragmentDecompilerThread comp(shader_str, arr, frag, len, *vk_prog);

		std::stringstream builder;
		builder <<
		"#version 450\n"
		"#extension GL_EXT_scalar_block_layout : require\n"
		"#extension GL_EXT_uniform_buffer_unsized_array : require\n"
		"#extension GL_ARB_separate_shader_objects : enable\n\n";

		::glsl::insert_subheader_block(builder);
		comp.insertConstants(builder);

		builder << "layout(location=16) in flat uvec4 draw_params_payload;\n\n";

		builder <<
		"#define fog_param0 fs_contexts[_fs_context_offset].fog_param0\n"
		"#define fog_param1 fs_contexts[_fs_context_offset].fog_param1\n"
		"#define fog_mode fs_contexts[_fs_context_offset].fog_mode\n"
		"#define wpos_scale fs_contexts[_fs_context_offset].wpos_scale\n"
		"#define wpos_bias fs_contexts[_fs_context_offset].wpos_bias\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_GE)
		{
			builder << "#define ALPHA_TEST_GEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_G)
		{
			builder << "#define ALPHA_TEST_GREATER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_LE)
		{
			builder << "#define ALPHA_TEST_LEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_L)
		{
			builder << "#define ALPHA_TEST_LESS\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_EQ)
		{
			builder << "#define ALPHA_TEST_EQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_NE)
		{
			builder << "#define ALPHA_TEST_NEQUAL\n";
		}

		if (!(compiler_options & COMPILER_OPT_ENABLE_F32_EXPORT))
		{
			builder << "#define WITH_HALF_OUTPUT_REGISTER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
		{
			builder << "#define WITH_DEPTH_EXPORT\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_FLOW_CTRL)
		{
			builder << "#define WITH_FLOW_CTRL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_PACKING)
		{
			builder << "#define WITH_PACKING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_KIL)
		{
			builder << "#define WITH_KIL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_STIPPLING)
		{
			builder << "#define WITH_STIPPLING\n";
		}

		const char* type_names[] = { "sampler1D", "sampler2D", "sampler3D", "samplerCube" };
		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			builder << "#define WITH_TEXTURES\n\n";

			for (int i = 0, bind_location = fragment_textures_start; i < 4; ++i)
			{
				builder << "layout(set=1, binding=" << bind_location++ << ") " << "uniform " << type_names[i] << " " << type_names[i] << "_array[16];\n";
			}

			builder << "\n"
				"#undef  TEX_PARAM\n"
				"#define TEX_PARAM(index) texture_parameters[index + texture_base_index]\n"
				"#define IS_TEXTURE_RESIDENT(index) true\n"
				"#define SAMPLER1D(index) sampler1D_array[index]\n"
				"#define SAMPLER2D(index) sampler2D_array[index]\n"
				"#define SAMPLER3D(index) sampler3D_array[index]\n"
				"#define SAMPLERCUBE(index) samplerCube_array[index]\n"
				"#define texture_base_index _fs_texture_base_index\n\n";
		}

		builder <<
			"layout(std430, set=1, binding=" << fragment_instruction_start << ") readonly restrict buffer FragmentInstructionBlock\n"
			"{\n"
			"	uint shader_control;\n"
			"	uint texture_control;\n"
			"	uint reserved1;\n"
			"	uint reserved2;\n"
			"	uvec4 fp_instructions[];\n"
			"};\n\n";

		builder <<
			"	uint rop_control = fs_contexts[_fs_context_offset].rop_control;\n"
			"	float alpha_ref = fs_contexts[_fs_context_offset].alpha_ref;\n\n";

		::glsl::insert_glsl_legacy_function(builder, properties);
		builder << program_common::interpreter::get_fragment_interpreter();
		const std::string s = builder.str();

		auto fs = &vk_prog->shader;
		fs->create(::glsl::program_domain::glsl_fragment_program, s);
		fs->compile();

		// Declare local inputs
		auto inputs = comp.get_inputs();

		vk::glsl::program_input in = vk::glsl::program_input::make
		(
			::glsl::glsl_fragment_program,
			"FragmentInstructionBlock",
			glsl::input_type_storage_buffer,
			1,
			fragment_instruction_start
		);
		inputs.push_back(in);

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			for (int i = 0, location = fragment_textures_start; i < 4; ++i, ++location)
			{
				in.location = location;
				in.name = std::string(type_names[i]) + "_array[16]";
				in.type = glsl::input_type_texture;
				inputs.push_back(in);
			}
		}

		vk_prog->SetInputs(inputs);

		// The first one built stays, as in build_vs.
		std::lock_guard lock(m_fs_shader_cache_lock);
		return m_fs_shader_cache.try_emplace(compiler_options, vk_prog).first->second;
	}

	void shader_interpreter::init(rsx::context* ctx, const vk::render_device& dev)
	{
		m_ctx = ctx;
		m_device = dev;

		VkPhysicalDeviceProperties gpu_properties{};
		vkGetPhysicalDeviceProperties(dev.gpu(), &gpu_properties);
		m_vendor_id = gpu_properties.vendorID;
		m_device_id = gpu_properties.deviceID;
	}

	void shader_interpreter::destroy()
	{
		stop_builder();

		m_current_interpreter.reset();
		m_program_cache.clear();
		m_pending.clear();
		m_vs_shader_cache.clear();
		m_fs_shader_cache.clear();
	}

	std::shared_ptr<glsl::program> shader_interpreter::link(const vk::pipeline_props& properties, u64 compiler_opt, bool async, async_build_fn_callback async_callback)
	{
		auto vs = build_vs(compiler_opt);
		auto fs = build_fs(compiler_opt);

		async_pipe_compiler_context context{ properties, m_device, vs->shader.get_handle(), fs->shader.get_handle() };
		auto create_graphics_info_fn = [=]() mutable
		{
			return context.compile();
		};

		auto callback_fn = [=, this](std::unique_ptr<glsl::program>& prog)
		{
			if (!async)
			{
				return;
			}

			pipeline_key key{};
			key.compiler_opt = compiler_opt;
			key.properties = properties;

			std::shared_ptr<glsl::program> result = std::move(prog);
			std::lock_guard lock(this->m_program_cache_lock);

			pipeline_cache_entry_t cached_program
			{
				.flags = 0,
				.program = result
			};
			this->m_program_cache[key] = cached_program;

			if (async_callback)
			{
				async_callback(result);
			}
		};

		vk::pipe_compiler::op_flags flags = vk::pipe_compiler::SEPARATE_SHADER_OBJECTS;
		flags |= (async ? vk::pipe_compiler::COMPILE_DEFERRED : vk::pipe_compiler::COMPILE_INLINE);

		auto compiler = vk::get_pipe_compiler();
		auto program = compiler->compile(
			create_graphics_info_fn,
			flags,
			callback_fn,
			vs->uniforms,
			fs->uniforms);

		return program;
	}

	void shader_interpreter::update_fragment_textures(const std::array<VkDescriptorImageInfoEx, 68>& sampled_images)
	{
		// FIXME: Cannot use m_fragment_textures.start now since each interpreter has its own binding layout
		auto [set, binding] = m_current_interpreter->get_uniform_location(::glsl::glsl_fragment_program, glsl::input_type_texture, "sampler1D_array[16]");
		if (binding == umax)
		{
			return;
		}

		const VkDescriptorImageInfoEx* texture_ptr = sampled_images.data();
		for (u32 i = 0; i < 4; ++i, ++binding, texture_ptr += 16)
		{
			m_current_interpreter->bind_uniform_array({ texture_ptr, 16 }, set, binding);
		}
	}

	glsl::program* shader_interpreter::get(
		const vk::pipeline_props& properties,
		const program_hash_util::fragment_program_utils::fragment_program_metadata& fp_metadata,
		const program_hash_util::vertex_program_utils::vertex_program_metadata& vp_metadata,
		u32 vp_ctrl,
		u32 fp_ctrl,
		variant_request* request)
	{
		pipeline_key key;
		key.compiler_opt = 0;
		key.properties = properties;

		if (fp_ctrl & RSX_SHADER_CONTROL_ALPHA_TEST) [[unlikely]]
		{
			switch (m_ctx->draw_state.fragment_env.alpha_func)
			{
			case rsx::comparison_function::always:
				break;
			case rsx::comparison_function::never:
				return nullptr;
			case rsx::comparison_function::greater_or_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
				break;
			case rsx::comparison_function::greater:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_G;
				break;
			case rsx::comparison_function::less_or_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_LE;
				break;
			case rsx::comparison_function::less:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_L;
				break;
			case rsx::comparison_function::equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_EQ;
				break;
			case rsx::comparison_function::not_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
				break;
			}
		}

		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) key.compiler_opt |= COMPILER_OPT_ENABLE_DEPTH_EXPORT;
		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) key.compiler_opt |= COMPILER_OPT_ENABLE_F32_EXPORT;
		if (fp_ctrl & RSX_SHADER_CONTROL_USES_KIL) key.compiler_opt |= COMPILER_OPT_ENABLE_KIL;
		if (fp_ctrl & RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP) key.compiler_opt |= COMPILER_OPT_ENABLE_ROP_REMAP;
		if (fp_metadata.referenced_textures_mask) key.compiler_opt |= COMPILER_OPT_ENABLE_TEXTURES;
		if (fp_metadata.has_branch_instructions) key.compiler_opt |= COMPILER_OPT_ENABLE_FLOW_CTRL;
		if (fp_metadata.has_pack_instructions) key.compiler_opt |= COMPILER_OPT_ENABLE_PACKING;
		if (fp_ctrl & RSX_SHADER_CONTROL_POLYGON_STIPPLE) key.compiler_opt |= COMPILER_OPT_ENABLE_STIPPLING;
		if (vp_ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) key.compiler_opt |= COMPILER_OPT_ENABLE_INSTANCING;
		if (vp_metadata.referenced_textures_mask) key.compiler_opt |= COMPILER_OPT_ENABLE_VTX_TEXTURES;

		// The current key changes only together with the program it names, so a miss never leaves a stale program behind.
		if (m_current_interpreter && m_current_key == key) [[likely]]
		{
			return m_current_interpreter.get();
		}

		bool found_program = false;
		bool first_use = false;
		{
			reader_lock lock(m_program_cache_lock);

			auto found = m_program_cache.find(key);
			if (found != m_program_cache.end()) [[likely]]
			{
				set_current(key, found->second.program);
				found_program = true;

				if ((found->second.flags & (CACHED_PIPE_UNOPTIMIZED | CACHED_PIPE_RECOMPILING)) == CACHED_PIPE_UNOPTIMIZED)
				{
					found->second.flags |= CACHED_PIPE_RECOMPILING;
					link(properties, key.compiler_opt, true, {});
				}

				if (found->second.flags & CACHED_PIPE_PREBUILT)
				{
					found->second.flags &= ~CACHED_PIPE_PREBUILT;
					first_use = true;
				}
			}
		}

		if (found_program)
		{
			if (first_use && m_builder_thread)
			{
				m_builder_jobs.push(builder_job{key, true});
			}

			return m_current_interpreter.get();
		}

		if (request && m_builder_thread)
		{
			// Built on the builder thread. Building it here held this thread, and the game waiting on it, for a whole driver
			// compile of the interpreter: seconds on slow drivers, many times the compiled program it stands in for. It is not
			// built on the compile workers either, where it would hold back the compiled programs queued behind it.
			request->pending = true;
			request->options = key.compiler_opt;
			request->state_hash = state_hash(properties);

			std::lock_guard lock(m_program_cache_lock);
			m_waiting_key = key;
			m_waiting = true;

			if (m_program_cache.contains(key))
			{
				// Finished since the lookup above; the caller's next call finds it.
				return nullptr;
			}

			auto [pending, inserted] = m_pending.try_emplace(key, true);
			if (inserted || !pending->second)
			{
				// New, or still waiting in the prebuild list: built next, ahead of that list.
				pending->second = true;
				m_builder_jobs.push(builder_job{key, false});
			}

			return nullptr;
		}

		const u64 hash = state_hash(properties);
		rsx_log.warning("Interpreter variant build start on the RSX thread: options 0x%04x, state %016llx", key.compiler_opt, hash);

		const auto start = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point shaders_done, end;
		std::shared_ptr<glsl::program> program;
		{
			pbd::shader_build::scope build(pbd::shader_build::renderer(), pbd::shader_build::kind::variant, key.compiler_opt, hash);
			build_vs(key.compiler_opt);
			build_fs(key.compiler_opt);
			shaders_done = std::chrono::steady_clock::now();
			program = link(properties, key.compiler_opt);
			end = std::chrono::steady_clock::now();
		}

		rsx_log.warning("Interpreter variant build end on the RSX thread after %u ms: shaders %u ms, pipeline %u ms (options 0x%04x, state %016llx)",
			elapsed_ms(start, end), elapsed_ms(start, shaders_done), elapsed_ms(shaders_done, end), key.compiler_opt, hash);

		if (end - start > std::chrono::milliseconds(1000))
		{
			rsx_log.error("Cache miss");
		}

		pipeline_cache_entry_t cache_entry{
			.flags = 0,
			.program = program};

		{
			std::lock_guard lock(m_program_cache_lock);
			m_program_cache[key] = cache_entry;
		}

		set_current(key, program);
		return m_current_interpreter.get();
	}

	void shader_interpreter::end_wait()
	{
		std::lock_guard lock(m_program_cache_lock);
		m_waiting = false;
	}

	void shader_interpreter::set_current(const pipeline_key& key, const std::shared_ptr<glsl::program>& program)
	{
		m_current_pipeline_info_ex = *get_pipeline_info_ex(key.compiler_opt);
		m_current_key = key;
		m_current_interpreter = program;
	}

	u64 shader_interpreter::state_hash(const vk::pipeline_props& properties)
	{
		const vk::pipeline_props state = portable_state(properties);
		return pbd::shader_build::fnv1a64(&state, sizeof(state));
	}

	// Runs on the builder thread. Reads no guest memory and no RSX registers: the key holds all of it.
	void shader_interpreter::build_off_thread(const pipeline_key& key, bool prebuild, std::string_view label)
	{
		const auto start = std::chrono::steady_clock::now();
		const auto vs = build_vs(key.compiler_opt);
		const auto fs = build_fs(key.compiler_opt);
		const auto shaders_done = std::chrono::steady_clock::now();

		async_pipe_compiler_context context{key.properties, m_device, vs->shader.get_handle(), fs->shader.get_handle()};
		const VkGraphicsPipelineCreateInfo info = context.compile();
		pipe_compiler::op_flags flags = pipe_compiler::COMPILE_INLINE;
		flags |= pipe_compiler::SEPARATE_SHADER_OBJECTS;
		std::shared_ptr<glsl::program> program = get_pipe_compiler()->compile(info, flags, {}, vs->uniforms, fs->uniforms);
		const auto end = std::chrono::steady_clock::now();

		{
			std::lock_guard lock(m_program_cache_lock);
			m_program_cache[key] = pipeline_cache_entry_t{.flags = prebuild ? CACHED_PIPE_PREBUILT : 0u, .program = program};
			m_pending.erase(key);
		}

		rsx_log.warning("Interpreter variant built %s off the RSX thread: options 0x%04x, state %016llx, shaders %u ms, pipeline %u ms",
			label, key.compiler_opt, state_hash(key.properties), elapsed_ms(start, shaders_done), elapsed_ms(shaders_done, end));

		if (!prebuild)
		{
			write_record(key);
		}
	}

	void shader_interpreter::write_record(const pipeline_key& key) const
	{
		if (m_record_dir.empty())
		{
			return;
		}

		const vk::pipeline_props state = portable_state(key.properties);
		const pbd::shader_build::record_header header{
			.magic = pbd::shader_build::record_magic,
			.version = pbd::shader_build::record_version,
			.vendor_id = m_vendor_id,
			.device_id = m_device_id,
			.options = key.compiler_opt,
			.state_bytes = sizeof(state),
			.reserved = 0};

		std::array<u8, sizeof(header) + sizeof(state)> bytes{};
		std::memcpy(bytes.data(), &header, sizeof(header));
		std::memcpy(bytes.data() + sizeof(header), static_cast<const void*>(&state), sizeof(state));

		// Rewriting an existing record only renews its time, which keeps the variants in use among the newest.
		const std::string name = pbd::shader_build::record_name(key.compiler_opt, pbd::shader_build::fnv1a64(&state, sizeof(state)));
		if (!fs::write_file(m_record_dir + name, fs::rewrite, bytes.data(), bytes.size()))
		{
			rsx_log.warning("Interpreter record %s could not be written: %s", name, fs::g_tls_error);
		}
	}

	bool shader_interpreter::read_record(const std::string& name, pipeline_key& key) const
	{
		constexpr u32 state_bytes = sizeof(vk::pipeline_props);
		std::array<u8, sizeof(pbd::shader_build::record_header) + state_bytes> bytes{};

		fs::file file(m_record_dir + name);
		if (!file || file.size() != bytes.size() || file.read(bytes.data(), bytes.size()) != bytes.size())
		{
			return false;
		}

		pbd::shader_build::record_header header{};
		std::memcpy(&header, bytes.data(), sizeof(header));
		const u8* state = bytes.data() + sizeof(header);
		if (!pbd::shader_build::record_valid(name, header, bytes.size(), m_vendor_id, m_device_id, state, state_bytes))
		{
			return false;
		}

		vk::pipeline_props properties{};
		std::memcpy(static_cast<void*>(&properties), state, state_bytes);

		// Skipped, never asserted: anything write_record would not have written, a sample count the build would reject
		// against its render pass, or depth bounds the device does not have.
		const auto& s = properties.state;
		if (s.ia.pNext || s.ds.pNext || s.cs.pNext || s.cs.pAttachments || s.rs.pNext || s.ms.pNext || s.ms.pSampleMask ||
			s.ia.sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
			s.ds.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
			s.cs.sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
			s.rs.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
			s.ms.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
			s.cs.attachmentCount > std::size(s.att_state) ||
			s.ms.rasterizationSamples != VkSampleCountFlagBits((properties.renderpass_key >> 16) & 0xF) ||
			(s.ds.depthBoundsTestEnable && !vk::get_current_renderer()->get_depth_bounds_support()))
		{
			return false;
		}

		properties.state.cs.pAttachments = properties.state.att_state;
		key.compiler_opt = header.options;
		key.properties = properties;
		return true;
	}

	void shader_interpreter::start_builder(std::string record_dir)
	{
		m_record_dir = std::move(record_dir);

		// Below the emulation threads. While the RSX thread waits for a variant the game waits with it and leaves the CPU
		// free; any other build only has to finish before a later draw or the fight.
		m_builder_thread = std::make_unique<named_thread<std::function<void()>>>("RSX Interpreter Builder", [this]()
			{
				thread_ctrl::scoped_priority low_priority(-1);
				run_builder();
			});
	}

	void shader_interpreter::stop_builder()
	{
		// Finishes the build in progress, then stops. Nothing waits for the builds still queued once the renderer stops.
		m_builder_thread.reset();
	}

	void shader_interpreter::run_builder()
	{
		const auto start = std::chrono::steady_clock::now();
		const std::vector<pipeline_key> records = load_records();
		usz next = 0;
		u32 built = 0;
		bool prebuilding = !records.empty();

		while (thread_ctrl::state() != thread_state::aborting)
		{
			if (m_builder_jobs)
			{
				// Draws first: the RSX thread may be waiting for one of these.
				for (auto&& job : m_builder_jobs.pop_all())
				{
					if (thread_ctrl::state() == thread_state::aborting)
					{
						break;
					}

					bool build = !job.record_only;
					if (build)
					{
						// A draw whose compiled program was ready first no longer waits. Building its variant anyway would
						// queue seconds of driver work per new state at the fight start for nothing; the record still
						// lets the next process prebuild it.
						std::lock_guard lock(m_program_cache_lock);
						if (!m_waiting || !(m_waiting_key == job.key))
						{
							build = false;
							if (const auto pending = m_pending.find(job.key); pending != m_pending.end())
							{
								pending->second = false;
							}
						}
					}

					if (build)
					{
						build_off_thread(job.key, false, "for a draw");
					}
					else
					{
						write_record(job.key);
					}
				}

				continue;
			}

			// Prebuilding stops when the fight begins: from then on it would only take CPU time and driver work from play.
			// What is left is built when a draw first needs it.
			if (prebuilding && (next == records.size() || pbd::shader_build::renderer().fight_started()))
			{
				prebuilding = false;
				rsx_log.warning("Interpreter prebuild: built %u of %u in %u ms%s", built, records.size(), elapsed_ms(start, std::chrono::steady_clock::now()),
					next < records.size() ? ", stopped when the fight began" : "");
			}

			if (prebuilding)
			{
				const pipeline_key& key = records[next++];
				{
					std::lock_guard lock(m_program_cache_lock);
					const auto pending = m_pending.find(key);
					if (pending == m_pending.end() || pending->second)
					{
						// Built already, or a draw took it over.
						continue;
					}

					pending->second = true;
				}

				built++;
				build_off_thread(key, true, fmt::format("ahead of play (%u of %u)", built, records.size()));
				pbd::shader_build::renderer().prebuilt();
				continue;
			}

			thread_ctrl::wait_on(m_builder_jobs);
		}

		if (prebuilding)
		{
			rsx_log.warning("Interpreter prebuild: stopped with the renderer after %u of %u in %u ms", built, records.size(), elapsed_ms(start, std::chrono::steady_clock::now()));
		}
	}

	// The newest valid records for this GPU, marked as waiting in m_pending, in the order the last process first used them.
	std::vector<shader_interpreter::pipeline_key> shader_interpreter::load_records()
	{
		if (m_record_dir.empty())
		{
			return {};
		}

		fs::dir root(m_record_dir);
		if (!root)
		{
			fs::create_path(m_record_dir);
			rsx_log.warning("Interpreter prebuild: no records yet in %s", m_record_dir);
			return {};
		}

		std::vector<pbd::shader_build::record_file> files;
		for (auto&& entry : root)
		{
			if (!entry.is_directory)
			{
				files.push_back({entry.name, entry.mtime, entry.size});
			}
		}

		const usz on_disk = files.size();
		std::vector<pipeline_key> keys;
		pbd::shader_build::take_newest(std::move(files), sizeof(pbd::shader_build::record_header) + sizeof(vk::pipeline_props), pbd::shader_build::prebuild_limit,
			[&](const std::string& name)
			{
				pipeline_key key{};
				if (!read_record(name, key))
				{
					return false;
				}

				keys.push_back(key);
				return true;
			});

		// A record is rewritten when a process first uses its variant, so among the newest the oldest were needed first. Built
		// in that order, the variants the fight start needs are not left behind those of the screens after it.
		std::reverse(keys.begin(), keys.end());

		std::vector<pipeline_key> queued;
		{
			std::lock_guard lock(m_program_cache_lock);
			for (const auto& key : keys)
			{
				if (!m_program_cache.contains(key) && m_pending.try_emplace(key, false).second)
				{
					queued.push_back(key);
				}
			}
		}

		pbd::shader_build::renderer().prebuild_queued(::size32(queued));
		rsx_log.warning("Interpreter prebuild: %u variants queued from %u valid of %u records in %s", queued.size(), keys.size(), on_disk, m_record_dir);
		return queued;
	}

	bool shader_interpreter::is_interpreter(const glsl::program* prog) const
	{
		return prog == m_current_interpreter.get();
	}

	u32 shader_interpreter::get_vertex_instruction_location() const
	{
		return m_current_pipeline_info_ex.vertex_instruction_location;
	}

	u32 shader_interpreter::get_fragment_instruction_location() const
	{
		return m_current_pipeline_info_ex.fragment_instruction_location;
	}

	std::pair<std::shared_ptr<VKVertexProgram>, std::shared_ptr<VKFragmentProgram>> shader_interpreter::get_shaders() const
	{
		const auto vs_opt = m_current_key.compiler_opt & COMPILER_OPT_ALL_VS_MASK;
		const auto fs_opt = m_current_key.compiler_opt & COMPILER_OPT_ALL_FS_MASK;

		std::shared_ptr<VKVertexProgram> vs;
		std::shared_ptr<VKFragmentProgram> fs;

		{
			reader_lock lock(m_vs_shader_cache_lock);
			if (auto found = m_vs_shader_cache.find(vs_opt);
				found != m_vs_shader_cache.end())
			{
				vs = found->second;
			}
		}

		{
			reader_lock lock(m_fs_shader_cache_lock);
			if (auto found = m_fs_shader_cache.find(fs_opt);
				found != m_fs_shader_cache.end())
			{
				fs = found->second;
			}
		}

		return { vs, fs };
	}

	const shader_interpreter::pipeline_info_ex_t* shader_interpreter::get_pipeline_info_ex(u64 compiler_opt)
	{
		if (auto found = m_pipeline_info_cache.find(compiler_opt); found != m_pipeline_info_cache.end())
		{
			return &found->second;
		}

		auto vs_stub = std::make_shared<VKVertexProgram>();
		auto fs_stub = std::make_shared<VKFragmentProgram>();
		const auto vi_location = init(vs_stub, compiler_opt);
		const auto fi_location = init(fs_stub, compiler_opt);

		pipeline_info_ex_t result
		{
			.vertex_instruction_location = vi_location,
			.fragment_instruction_location = fi_location,
			.fragment_textures_location = fi_location + 1
		};

		auto it = m_pipeline_info_cache.insert_or_assign(compiler_opt, result);
		return &it.first->second;
	}

	void shader_interpreter::preload(rsx::shader_loading_dialog* dlg)
	{
		dlg->create(get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS), get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_TITLE));

		// Create some basic pipelines that we'll use to seed the base pipeline queue
		std::vector<vk::pipeline_props> pipe_properties;
		auto pdev = vk::get_current_renderer();

		// PBD: only the three B8G8R8A8 states (plain, blended, blended with depth). Every set, Practice run and setup boots
		// its own profile, and preloading every colour format and target count (35 states) held each fresh profile's boot
		// for minutes on all threads before the game could start. The variants a fight draws are built off the RSX thread by
		// the interpreter builder and recorded for the next boot (start_builder).
		const auto depth_format = pdev->get_formats_support().d24_unorm_s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
		vk::pipeline_props base_props{};
		base_props.state.set_attachment_count(1);
		base_props.state.enable_cull_face(VK_CULL_MODE_BACK_BIT);
		base_props.state.set_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		base_props.state.set_color_mask(0, true, true, true, true);
		base_props.state.enable_depth_bias(true);
		base_props.state.enable_depth_clamp(true);
		base_props.state.enable_depth_bounds_test(pdev->get_depth_bounds_support());
		base_props.renderpass_key = vk::get_renderpass_key(VK_FORMAT_B8G8R8A8_UNORM);
		pipe_properties.push_back(base_props);

		base_props.state.enable_blend(0,
			VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
		pipe_properties.push_back(base_props);

		base_props.renderpass_key = vk::get_renderpass_key(VK_FORMAT_B8G8R8A8_UNORM, depth_format);
		base_props.state.enable_depth_test(VK_COMPARE_OP_LESS);
		base_props.state.set_depth_mask(true);
		pipe_properties.push_back(base_props);

		// How many variants to generate before we allow the user to skip
		u32 min_variants_to_build = ::size32(pipe_properties);

		const auto variants = program_common::interpreter::get_interpreter_variants();
		const u32 limit1 = ::size32(variants.base_pipelines) * ::size32(pipe_properties);
		const u32 limit2 = ::size32(variants.pipelines) * ::size32(pipe_properties);
		dlg->set_limit(0, limit1);
		dlg->set_limit(1, limit2);

		// Map each base pipeline to the full variants that can alias it
		std::unordered_set<u64> base_opts;
		for (const auto& variant : variants.base_pipelines)
		{
			base_opts.insert(variant.first | variant.second);
		}

		std::unordered_map<u64, std::vector<u64>> variant_map;
		for (const auto& variant : variants.pipelines)
		{
			const u64 compat_opt = variant.vs_opts.compatible_shader_opts | variant.fs_opts.compatible_shader_opts;
			const u64 exact_opt = variant.vs_opts.shader_opt | variant.fs_opts.shader_opt;

			if (!base_opts.contains(compat_opt))
			{
				fmt::throw_exception("Invalid interpreter configuration.");
			}

			if (compat_opt != exact_opt)
			{
				variant_map[compat_opt].push_back(exact_opt);
			}
		}

		auto ctr = std::make_shared<atomic_t<u32>>(0);
		for (const auto& props : pipe_properties)
		{
			for (auto& variant : variants.base_pipelines)
			{
				const u64 base_opt = variant.first | variant.second;
				const auto found = variant_map.find(base_opt);
				auto derived_opts = found != variant_map.end() ? found->second : std::vector<u64>{};

				// The callback runs under m_program_cache_lock and may outlive this function if the user skips
				link(props, base_opt, true, [this, ctr, props, derived_opts = std::move(derived_opts)](std::shared_ptr<glsl::program>& prog)
				{
					pipeline_key key{};
					key.properties = props;

					for (const u64 opt : derived_opts)
					{
						// Never replace an existing entry, it is either the same alias or a real pipeline
						key.compiler_opt = opt;
						m_program_cache.try_emplace(key, pipeline_cache_entry_t{ .flags = CACHED_PIPE_UNOPTIMIZED, .program = prog });
					}

					(*ctr)++;
				});
			}
		}

		// It doesn't need to be exact but at least generate enough to cover most of the base pipelines.
		// Even if we miss the pipelines, the driver already saw them, so processing will be fast.
		min_variants_to_build *= ::size32(variants.base_pipelines);

		// Drain the queue.
		do
		{
			std::this_thread::sleep_for(16ms);

			const auto completed = ctr->load();
			dlg->update_msg(0, get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_VULKAN, "%u %s %u", completed, get_localized_string(localized_string_id::PROGRESS_DIALOG_OF), limit1));
			dlg->set_value(0, completed);

			if (min_variants_to_build &&
				completed >= min_variants_to_build)
			{
				min_variants_to_build = 0u;
				dlg->enable_hold_to_skip();
			}
		}
		while (ctr->load() < limit1 && thread_ctrl::state() != thread_state::aborting && !dlg->skip_requested());

		dlg->set_value(1, limit2);
		dlg->refresh();
	}
};
