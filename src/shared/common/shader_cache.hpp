#pragma once

namespace shared::common
{
	// Vertex shader hash cache and hash whitelist.
	class ShaderCache
	{
	public:
		ShaderCache() = default;

		// Hash of the shader's bytecode, cached per shader object.
		uint32_t get_shader_hash(IDirect3DVertexShader9* shader)
		{
			if (!shader) {
				return 0;
			}

			if (const auto it = shader_hash_cache_.find(shader); it != shader_hash_cache_.end()) {
				return it->second;
			}

			UINT bytecode_size = 0;
			shader->GetFunction(nullptr, &bytecode_size);

			std::vector<BYTE> bytecode(bytecode_size);
			shader->GetFunction(bytecode.data(), &bytecode_size);

			const uint32_t hash = shared::utils::data_hash32(bytecode.data(), bytecode_size);
			shader_hash_cache_[shader] = hash;

#if DEBUG
			static std::set<uint32_t> seen_hashes;
			if (seen_hashes.insert(hash).second) {
				printf("New shader hash: 0x%08X\n", hash);
			}
#endif

			return hash;
		}

		void clear_cache() {
			shader_hash_cache_.clear();
		}

	private:
		std::unordered_map<IDirect3DVertexShader9*, uint32_t> shader_hash_cache_;
	};

	inline ShaderCache g_shader_cache;
}
