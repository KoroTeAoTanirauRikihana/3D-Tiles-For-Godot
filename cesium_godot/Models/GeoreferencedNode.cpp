#include "GeoreferencedNode.h"
#include "Models/CesiumGlobe.h"
#include "Utils/CesiumMathUtils.h"
#include "glm/ext/vector_double3.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/tile_set.hpp"


void GeoreferencedMesh::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) return;

	// True origin is a no-op
	if (this->get_georeference()->get_origin_type_raw() == CesiumGeoreference::OriginType::TrueOrigin) {
		return;
	}
	// Use the georeference's origin to apply the difference in engine coords
	Vector3 ecefOrigin = CesiumMathUtils::from_glm_vec3(this->get_georeference()->get_ecef_position());
	Vector3 objectPos = this->get_georeference()->get_tx_engine_to_ecef().xform(this->get_global_position());
	this->set_ecef_position(objectPos + ecefOrigin);
}

void GeoreferencedMesh::apply_position_on_globe(const glm::dvec3& engineOrigin) {
	// The offset from the origin is in the TILESET'S frame (swapped ECEF axes, the same frame the tile's own
	// rotation lives in), so it is the tile's LOCAL position under the tileset. It used to be set as the GLOBAL
	// position, which was the same thing only while the georeference (-90 X) and the tileset (+90 X) cancelled
	// exactly. Once the georeference is levelled at a cartographic origin (level_at_origin), every tile's mesh
	// turned with the levelled frame while its centre stayed on the unlevelled, tilted surface - a staircase of
	// slanted plates at eye level, Rapa Nui, 28 Oct 2026 22:07 and 22:10 (187 tiles in the tree, none in place).
	glm::dvec3 localPos = this->m_originalPosition - engineOrigin;
	this->set_position(CesiumMathUtils::from_glm_vec3(localPos));
}

const glm::dvec3& GeoreferencedMesh::get_original_position() const {
	return this->m_originalPosition;
}

void GeoreferencedMesh::set_original_position(const glm::dvec3& position) {
	this->m_originalPosition = position;
}

void GeoreferencedMesh::set_engine_position(const Vector3& position) {
	const CesiumGeoreference* georef = this->m_tileset->get_georeference_node();
	this->m_originalPosition = CesiumMathUtils::to_glm_dvec3(position);
	glm::dvec3 engineOrigin = CesiumMathUtils::ecef_to_engine(georef->get_ecef_position());
	this->apply_position_on_globe(engineOrigin);
}

void GeoreferencedMesh::set_ecef_position(const Vector3& position) {
	const CesiumGeoreference* georef = this->m_tileset->get_georeference_node();
	Vector3 enginePos = georef->get_tx_ecef_to_engine().xform(position);
	
	this->m_originalPosition = CesiumMathUtils::to_glm_dvec3(enginePos);
	glm::dvec3 engineOrigin = CesiumMathUtils::ecef_to_engine(georef->get_ecef_position());
	this->apply_position_on_globe(engineOrigin);
}


Vector3 GeoreferencedMesh::get_ecef_position() const {
	Vector3 originalPos = CesiumMathUtils::from_glm_vec3(this->m_originalPosition);
	return this->m_tileset->get_georeference_node()->get_tx_engine_to_ecef().xform(originalPos);
}

void GeoreferencedMesh::set_tileset(Cesium3DTileset* tileset) {
	this->m_tileset = tileset;
}


void GeoreferencedMesh::set_tileset_no_reparent(Cesium3DTileset* tileset) {
	this->m_tileset = tileset;
}

Vector3 GeoreferencedMesh::get_engine_position() const {
	return CesiumMathUtils::from_glm_vec3(this->m_originalPosition);
}


Cesium3DTileset* GeoreferencedMesh::get_tileset() const {
	return this->m_tileset;
}

CesiumGeoreference* GeoreferencedMesh::get_georeference() const {
	return this->m_tileset->get_georeference_node();
}


void GeoreferencedMesh::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_engine_position"), &GeoreferencedMesh::get_engine_position);
    ClassDB::bind_method(D_METHOD("set_engine_position", "position"), &GeoreferencedMesh::set_engine_position);
    ClassDB::bind_method(D_METHOD("get_ecef_position"), &GeoreferencedMesh::get_ecef_position);
    ClassDB::bind_method(D_METHOD("set_ecef_position", "position"), &GeoreferencedMesh::set_ecef_position);
    ClassDB::bind_method(D_METHOD("set_tileset", "tileset"), &GeoreferencedMesh::set_tileset);
    ClassDB::bind_method(D_METHOD("get_tileset"), &GeoreferencedMesh::get_tileset);
    ClassDB::bind_method(D_METHOD("get_georeference"), &GeoreferencedMesh::get_georeference);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tileset", PropertyHint::PROPERTY_HINT_NODE_TYPE, "Cesium3DTileset"), "set_tileset", "get_tileset");
}

