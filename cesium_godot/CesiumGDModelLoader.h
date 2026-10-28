#ifndef CESIUM_GD_MODEL_LOADER_H
#define CESIUM_GD_MODEL_LOADER_H

#include "glm/ext/matrix_double4x4.hpp"
#include <cstdint>
#if defined(CESIUM_GD_EXT)
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include "godot_cpp/classes/standard_material3d.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/templates/vector.hpp>
#include "godot_cpp/variant/array.hpp"
using namespace godot;

namespace godot {
	class MeshInstance;
}
#elif defined(CESIUM_GD_MODULE)
#include <scene/resources/mesh.h>
class MeshInstance3D;
#endif

//Do not trust the compiler to delete this, we'll need it if REAL_T is double
#include "Utils/CesiumMathUtils.h"
#include "CesiumGltf/Model.h"
#include <CesiumGltfReader/GltfReader.h>
#include <vector>


/// @brief One glTF material as plain values, its base colour texture as a CPU-side Image. Made on any thread; it holds
/// no server object (Godot, "Thread-safe APIs": avoid "creating new textures or modifying and retrieving image data"
/// on other threads). The StandardMaterial3D and its ImageTexture are made from it on the main thread.
struct CesiumGDMaterialData {
	bool hasPbr = false;
	bool hasAlbedo = false;
	Color albedo;
	double specular = 0.0;
	double roughness = 0.0;
	Ref<Image> albedoImage;
	BaseMaterial3D::Transparency transparency = BaseMaterial3D::TRANSPARENCY_DISABLED;
	BaseMaterial3D::CullMode cullMode = BaseMaterial3D::CULL_FRONT;
	String name;
};

/// @brief One glTF primitive decoded on the load thread: the surface arrays (Packed*Array), its material values and
/// the KHR_texture_transform values. No ArrayMesh, material, texture or shader: those are made on the main thread.
struct CesiumGDPrimitiveData {
	Mesh::PrimitiveType primitiveType = Mesh::PRIMITIVE_TRIANGLES;
	Array arrays;
	int32_t surfaceIndex = 0;
	CesiumGDMaterialData material;
	bool unshaded = false;
	bool hasTextureTransform = false;
	bool hasUvOffset = false;
	bool hasUvScale = false;
	Vector3 uvOffset = Vector3(0, 0, 0);
	Vector3 uvScale = Vector3(1, 1, 1);
	float uvRotation = 0.0f;
};

/// @brief The load-thread result of a tile (GodotPrepareRenderResources::prepareInLoadThread): CPU data only, so a
/// tile that never reaches its main-thread step is freed by plain destruction.
struct CesiumGDMeshData {
	std::vector<CesiumGDPrimitiveData> primitives;
};

class CesiumGDModelLoader {
public:
	/// @brief Load thread: decodes the glTF into CPU-side data. Touches no server.
	static Error decode_model(const CesiumGltf::Model& model, CesiumGDMeshData* out);

	/// @brief Main thread only: makes the ArrayMesh, its materials and textures from decoded data.
	static Ref<ArrayMesh> create_mesh(const CesiumGDMeshData& meshData);

	/// @brief Any thread: the material's values and base colour Image. Touches no server.
	static CesiumGDMaterialData decode_material(const CesiumGltf::Material& cesiumMaterial, const CesiumGltf::Model& modelReference);

	/// @brief Main thread only: sets the values on the material and creates its albedo ImageTexture.
	static void apply_material(const CesiumGDMaterialData& materialData, Ref<StandardMaterial3D>& godotMaterial);

	static glm::dmat4 apply_rtc_center(const CesiumGltf::Model& gltf, const glm::dmat4x4& rootTransform);

	static glm::dmat4 apply_gltf_up_axis_transform(const CesiumGltf::Model& model, const glm::dmat4x4& rootTransform);	

	static Error parse_gltf(const String& assetPath, CesiumGltfReader::GltfReaderResult* out);

	template<class BT>
	static inline Vector<BT> get_attribute_from_primitive(const CesiumGltf::MeshPrimitive& primitive, const CesiumGltf::Model& model, const std::string_view& attributeName, std::optional<std::function<void(BT&)>> callback = std::nullopt){

		Vector<BT> resultBuffer;
		const auto& attributeIterator = primitive.attributes.find(attributeName.data());
		if (attributeIterator == primitive.attributes.end()) {
			return Vector<BT>();
		}

		const CesiumGltf::Accessor& attrAccessor = model.accessors[attributeIterator->second];
		const CesiumGltf::BufferView& attrBufferView = model.bufferViews[attrAccessor.bufferView];
		const CesiumGltf::Buffer& attBuffer = model.buffers[attrBufferView.buffer];

		const std::byte* attrbuteData = &attBuffer.cesium.data[attrBufferView.byteOffset + attrAccessor.byteOffset];
		//If we have a double precision build, we need to account that in, as models will come with single precision attributes
#ifdef REAL_T_IS_DOUBLE
		//The destination here will be a vector of floats and then we cast that to the actual type
		constexpr size_t ATTR_SIZE = sizeof(BT) / 2;
#else

		constexpr size_t ATTR_SIZE = sizeof(BT);
#endif
		const int8_t componentSize = attrAccessor.computeByteSizeOfComponent();
		const int8_t componentCount = attrAccessor.computeNumberOfComponents();

		ERR_FAIL_COND_V_MSG((componentCount * componentSize) != ATTR_SIZE, resultBuffer, String("Attribute component length does not match template argument length"));

		int64_t stride = attrBufferView.byteStride.value_or(ATTR_SIZE);

		for (size_t i = 0; i < attrAccessor.count; ++i) {
			const std::byte* data = attrbuteData + i * stride;
			BT attribute = copy_to_buffer_type<BT>(data, ATTR_SIZE);
			if (callback) {
				(*callback)(attribute);
			}
			resultBuffer.push_back(attribute);
		}

		//Correct the attribute too
		while (primitive.mode == CesiumGltf::MeshPrimitive::Mode::TRIANGLES && resultBuffer.size() % 3 != 0) {
			resultBuffer.push_back(resultBuffer.get(resultBuffer.size() - 1)); // Duplicate the last normal
		}
		return resultBuffer;
	}

	static Error copy_material_properties(const CesiumGltf::Material& cesiumMaterial, Ref<StandardMaterial3D>& godotMaterial, const CesiumGltf::Model& modelReference);

	static Error apply_surface_to_mesh(const CesiumGltf::MeshPrimitive& meshPrimitive, Ref<ArrayMesh>& meshInstance, const Array& arrays);

private:

	static constexpr Mesh::PrimitiveType cesium_to_godot_primitive_mode(int32_t mode);

	static Vector<Vector3> generate_normals(const Vector<Vector3>& vertices, const Vector<int32_t>& indices);


	#if defined(CESIUM_GD_EXT)
	static Array generate_array_mesh_ext(const Vector<Vector3>& vertices, const Vector<int32_t>& indices, const Vector<Vector3>& normals, const Vector<Vector2>& textureCoords, const Vector<Vector2>& textureCoords2);
	#endif
	
	template<class T>
	static inline T copy_to_buffer_type(const std::byte* data, size_t attributeSize) {
#ifndef REAL_T_IS_DOUBLE
		//We can just copy as usual, real_t is float
		T attribute;
		//TODO: Check if we can just reinterpret cast here
		memcpy(&attribute, data, attributeSize);
		return attribute;
#else
		if constexpr (std::is_same<T, Vector2>::value) {
			const glm::vec2* tempAttribute = reinterpret_cast<const glm::vec2*>(data);
			Vector2 attribute = CesiumMathUtils::from_glm_vec2(*tempAttribute);
			return static_cast<T>(attribute);
		}
		else if constexpr (std::is_same<T, Vector3>::value) {
			//Vec3
			const glm::vec3* tempAttribute = reinterpret_cast<const glm::vec3*>(data);
			Vector3 attribute = CesiumMathUtils::from_glm_vec3(*tempAttribute);
			return static_cast<T>(attribute);
		}
#endif
	}

	static Vector<int32_t> get_index_buffer_from_primitive(const CesiumGltf::MeshPrimitive& primitive, const CesiumGltf::Model& model, Error* error);

	static Ref<Shader> load_texture_transform_shader();

	static Error generate_normals(Vector<Vector3>* normalBuffer, const Vector<Vector3>& vertexBuffer, const Vector<int32_t>& indexBuffer, bool flip = false);
};

#endif // !CESIUM_GD_MODEL_LOADER_H
