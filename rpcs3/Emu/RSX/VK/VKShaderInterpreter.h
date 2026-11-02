#pragma once
#include "Emu/RSX/VK/VKProgramPipeline.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
#include "vkutils/descriptors.h"
#include "Utilities/Thread.h"
#include "Utilities/lockless.h"
#include <unordered_map>

class VKVertexProgram;
class VKFragmentProgram;

namespace rsx
{
	struct shader_loading_dialog;
}

namespace vk
{
	using ::program_hash_util::fragment_program_utils;
	using ::program_hash_util::vertex_program_utils;

	class shader_interpreter
	{
		using async_build_fn_callback = std::function<void(std::shared_ptr<glsl::program>&)>;

		VkDevice m_device = VK_NULL_HANDLE;
		VkPipelineCache m_driver_pipeline_cache = VK_NULL_HANDLE;

		struct pipeline_key
		{
			u64 compiler_opt;
			vk::pipeline_props properties;

			bool operator == (const pipeline_key& other) const
			{
				return other.compiler_opt == compiler_opt && other.properties == properties;
			}
		};

		struct key_hasher
		{
			usz operator()(const pipeline_key& key) const
			{
				return rpcs3::hash_struct(key.properties) ^ key.compiler_opt;
			}
		};

		struct shader_cache_entry_t
		{
			std::shared_ptr<VKFragmentProgram> m_fs;
			std::shared_ptr<VKVertexProgram> m_vs;
		};

		struct pipeline_info_ex_t
		{
			u32 vertex_instruction_location = 0;
			u32 fragment_instruction_location = 0;
			u32 fragment_textures_location = 0;
		};

		struct pipeline_cache_entry_t
		{
			u32 flags = 0;
			std::shared_ptr<glsl::program> program;
		};

		std::unordered_map<pipeline_key, pipeline_cache_entry_t, key_hasher> m_program_cache;
		std::unordered_map<u64, std::shared_ptr<VKVertexProgram>> m_vs_shader_cache;
		std::unordered_map<u64, std::shared_ptr<VKFragmentProgram>> m_fs_shader_cache;
		std::unordered_map<u64, pipeline_info_ex_t> m_pipeline_info_cache;

		// Variants not built yet that a build was promised for, under m_program_cache_lock. True once queued for a draw or
		// building; false while it only waits in the prebuild list, or after the draw that asked for it stopped waiting,
		// where the next draw that needs it takes it over.
		std::unordered_map<pipeline_key, bool, key_hasher> m_pending;

		// The variant the RSX thread waits for, under m_program_cache_lock. A queued draw build it no longer waits for is
		// only recorded, not built.
		pipeline_key m_waiting_key{};
		bool m_waiting = false;

		mutable shared_mutex m_program_cache_lock;
		mutable shared_mutex m_vs_shader_cache_lock;
		mutable shared_mutex m_fs_shader_cache_lock;

		pipeline_key m_current_key{};
		pipeline_info_ex_t m_current_pipeline_info_ex{};
		std::shared_ptr<glsl::program> m_current_interpreter;

		// Work for the builder thread from draws: a variant to build, or only a record to renew.
		struct builder_job
		{
			pipeline_key key;
			bool record_only;
		};

		// Records of the variants draws needed (empty when the disk cache is off), and the GPU they were built for.
		std::string m_record_dir;
		u32 m_vendor_id = 0;
		u32 m_device_id = 0;
		lf_queue<builder_job> m_builder_jobs;
		std::unique_ptr<named_thread<std::function<void()>>> m_builder_thread;

		std::shared_ptr<VKVertexProgram> build_vs(u64 compiler_opt);
		std::shared_ptr<VKFragmentProgram> build_fs(u64 compiler_opt);
		std::shared_ptr<glsl::program> link(const vk::pipeline_props& properties, u64 compiler_opt, bool async = false, async_build_fn_callback async_callback = {});

		u32 init(std::shared_ptr<VKVertexProgram>& vk_prog, u64 compiler_opt) const;
		u32 init(std::shared_ptr<VKFragmentProgram>& vk_prog, u64 compiler_opt) const;

		const pipeline_info_ex_t* get_pipeline_info_ex(u64 compiler_opt);
		void set_current(const pipeline_key& key, const std::shared_ptr<glsl::program>& program);

		void build_off_thread(const pipeline_key& key, bool prebuild, std::string_view label);
		void write_record(const pipeline_key& key) const;
		bool read_record(const std::string& name, pipeline_key& key) const;
		std::vector<pipeline_key> load_records();
		void run_builder();

	public:
		// What get() reports when it returns no program because the variant is building on another thread.
		struct variant_request
		{
			bool pending = false;
			u64 options = 0;
			u64 state_hash = 0;
		};

		// A pipeline state's hash with its pointers cleared, the same in every process.
		static u64 state_hash(const vk::pipeline_props& properties);

		void init(const vk::render_device& dev);
		void destroy();

		void preload(rsx::shader_loading_dialog* dlg);

		// Starts the low priority builder thread. It builds the variants the RSX thread waits for, and until the fight begins
		// the newest recorded ones (up to the prebuild limit); every variant a draw needs is recorded under record_dir. An
		// empty record_dir records and prebuilds nothing.
		void start_builder(std::string record_dir);
		void stop_builder();

		// With a request and a running builder, a variant that is not built yet is built on the builder thread and the
		// call returns nullptr with request->pending set; the caller waits, then calls end_wait(). Otherwise the variant
		// is built here.
		glsl::program* get(
			const vk::pipeline_props& properties,
			const program_hash_util::fragment_program_utils::fragment_program_metadata& fp_metadata,
			const program_hash_util::vertex_program_utils::vertex_program_metadata& vp_metadata,
			u32 vp_ctrl,
			u32 fp_ctrl,
			variant_request* request = nullptr);

		void end_wait();

		// Retrieve the shader components that make up the current interpreter
		std::pair<std::shared_ptr<VKVertexProgram>, std::shared_ptr<VKFragmentProgram>> get_shaders() const;

		bool is_interpreter(const glsl::program* prog) const;

		u32 get_vertex_instruction_location() const;
		u32 get_fragment_instruction_location() const;

		void update_fragment_textures(const std::array<VkDescriptorImageInfoEx, 68>& sampled_images);
	};
}
