/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Chingiz Dyussenov, Arystanbek Dyussenov, Nathan Letwory.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/collada/collada_utils.cpp
 *  \ingroup collada
 */


/* COLLADABU_ASSERT, may be able to remove later */
#include "COLLADABUPlatform.h"

#include "COLLADAFWGeometry.h"
#include "COLLADAFWMeshPrimitive.h"
#include "COLLADAFWMeshVertexData.h"

#include "collada_utils.h"

extern "C" {
#include "DNA_modifier_types.h"
#include "DNA_customdata_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "DNA_armature_types.h"

#include "BLI_math.h"
#include "BLI_linklist.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_depsgraph.h"
#include "BKE_object.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_scene.h"
#include "BKE_DerivedMesh.h"

#include "WM_api.h" // XXX hrm, see if we can do without this
#include "WM_types.h"
}

float bc_get_float_value(const COLLADAFW::FloatOrDoubleArray& array, unsigned int index)
{
	if (index >= array.getValuesCount())
		return 0.0f;

	if (array.getType() == COLLADAFW::MeshVertexData::DATA_TYPE_FLOAT)
		return array.getFloatValues()->getData()[index];
	else 
		return array.getDoubleValues()->getData()[index];
}

// copied from /editors/object/object_relations.c
int bc_test_parent_loop(Object *par, Object *ob)
{
	/* test if 'ob' is a parent somewhere in par's parents */
	
	if (par == NULL) return 0;
	if (ob == par) return 1;
	
	return bc_test_parent_loop(par->parent, ob);
}

// a shortened version of parent_set_exec()
// if is_parent_space is true then ob->obmat will be multiplied by par->obmat before parenting
int bc_set_parent(Object *ob, Object *par, bContext *C, bool is_parent_space)
{
	Object workob;
	Main *bmain = CTX_data_main(C);
	Scene *sce = CTX_data_scene(C);
	
	if (!par || bc_test_parent_loop(par, ob))
		return false;

	ob->parent = par;
	ob->partype = PAROBJECT;

	ob->parsubstr[0] = 0;

	if (is_parent_space) {
		float mat[4][4];
		// calc par->obmat
		BKE_object_where_is_calc(sce, par);

		// move child obmat into world space
		mult_m4_m4m4(mat, par->obmat, ob->obmat);
		copy_m4_m4(ob->obmat, mat);
	}
	
	// apply child obmat (i.e. decompose it into rot/loc/size)
	BKE_object_apply_mat4(ob, ob->obmat, 0, 0);

	// compute parentinv
	BKE_object_workob_calc_parent(sce, ob, &workob);
	invert_m4_m4(ob->parentinv, workob.obmat);

	ob->recalc |= OB_RECALC_OB | OB_RECALC_DATA;
	par->recalc |= OB_RECALC_OB;

	/** done once after import
	DAG_scene_sort(bmain, sce);
	DAG_ids_flush_update(bmain, 0);
	WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, NULL);
    */


	return true;
}

Object *bc_add_object(Scene *scene, int type, const char *name)
{
	Object *ob = BKE_object_add_only_object(G.main, type, name);

	ob->data = BKE_object_obdata_add_from_type(type);
	ob->lay = scene->lay;
	ob->recalc |= OB_RECALC_OB | OB_RECALC_DATA | OB_RECALC_TIME;

	BKE_scene_base_select(scene, BKE_scene_base_add(scene, ob));

	return ob;
}

Mesh *bc_to_mesh_apply_modifiers(Scene *scene, Object *ob, BC_export_mesh_type export_mesh_type)
{
	Mesh *tmpmesh;
	CustomDataMask mask = CD_MASK_MESH;
	DerivedMesh *dm = NULL;
	switch (export_mesh_type) {
		case BC_MESH_TYPE_VIEW: {
			dm = mesh_create_derived_view(scene, ob, mask);
			break;
		}
		case BC_MESH_TYPE_RENDER: {
			dm = mesh_create_derived_render(scene, ob, mask);
			break;
		}
	}

	tmpmesh = BKE_mesh_add(G.main, "ColladaMesh"); // name is not important here
	DM_to_mesh(dm, tmpmesh, ob);
	dm->release(dm);
	return tmpmesh;
}

Object *bc_get_assigned_armature(Object *ob)
{
	Object *ob_arm = NULL;

	if (ob->parent && ob->partype == PARSKEL && ob->parent->type == OB_ARMATURE) {
		ob_arm = ob->parent;
	}
	else {
		ModifierData *mod;
		for (mod = (ModifierData *)ob->modifiers.first; mod; mod = mod->next) {
			if (mod->type == eModifierType_Armature) {
				ob_arm = ((ArmatureModifierData *)mod)->object;
			}
		}
	}

	return ob_arm;
}

// Returns the highest selected ancestor
// returns NULL if no ancestor is selected
// IMPORTANT: This function expects that
// all exported objects have set:
// ob->id.flag & LIB_DOIT
Object *bc_get_highest_selected_ancestor_or_self(LinkNode *export_set, Object *ob) 
{
	Object *ancestor = ob;
	while (ob->parent && bc_is_marked(ob->parent)) {
		ob = ob->parent;
		ancestor = ob;
	}
	return ancestor;
}


bool bc_is_base_node(LinkNode *export_set, Object *ob)
{
	Object *root = bc_get_highest_selected_ancestor_or_self(export_set, ob);
	return (root == ob);
}

bool bc_is_in_Export_set(LinkNode *export_set, Object *ob)
{
	return (BLI_linklist_index(export_set, ob) != -1);
}

bool bc_has_object_type(LinkNode *export_set, short obtype)
{
	LinkNode *node;
	
	for (node = export_set; node; node = node->next) {
		Object *ob = (Object *)node->link;
		/* XXX - why is this checking for ob->data? - we could be looking for empties */
		if (ob->type == obtype && ob->data) {
			return true;
		}
	}
	return false;
}

int bc_is_marked(Object *ob)
{
	return ob && (ob->id.flag & LIB_DOIT);
}

void bc_remove_mark(Object *ob)
{
	ob->id.flag &= ~LIB_DOIT;
}

void bc_set_mark(Object *ob)
{
	ob->id.flag |= LIB_DOIT;
}

// Use bubble sort algorithm for sorting the export set
void bc_bubble_sort_by_Object_name(LinkNode *export_set)
{
	bool sorted = false;
	LinkNode *node;
	for (node = export_set; node->next && !sorted; node = node->next) {

		sorted = true;
		
		LinkNode *current;
		for (current = export_set; current->next; current = current->next) {
			Object *a = (Object *)current->link;
			Object *b = (Object *)current->next->link;

			if (strcmp(a->id.name, b->id.name) > 0) {
				current->link       = b;
				current->next->link = a;
				sorted = false;
			}
			
		}
	}
}

/* Check if a bone is the top most exportable bone in the bone hierarchy. 
 * When deform_bones_only == false, then only bones with NO parent 
 * can be root bones. Otherwise the top most deform bones in the hierarchy
 * are root bones.
 */
bool bc_is_root_bone(Bone *aBone, bool deform_bones_only)
{
	if (deform_bones_only) {
		Bone *root = NULL;
		Bone *bone = aBone;
		while (bone) {
			if (!(bone->flag & BONE_NO_DEFORM))
				root = bone;
			bone = bone->parent;
		}
		return (aBone == root);
	}
	else
		return !(aBone->parent);
}

int bc_get_active_UVLayer(Object *ob)
{
	Mesh *me = (Mesh *)ob->data;
	return CustomData_get_active_layer_index(&me->fdata, CD_MTFACE);
}

std::string bc_url_encode(std::string data) {
	/* XXX We probably do not need to do a full encoding.
	   But in case that is necessary,then it can be added here.
	*/
	return bc_replace_string(data,"#", "%23");
}

std::string bc_replace_string(std::string data, const std::string& pattern,
							  const std::string& replacement) {
	size_t pos = 0;
	while((pos = data.find(pattern, pos)) != std::string::npos) {
		data.replace(pos, pattern.length(), replacement);
		pos += replacement.length();
	}
	return data;
}

/**
	Calculate a rescale factor such that the imported scene's scale
	is preserved. I.e. 1 meter in the import will also be
	1 meter in the curretn scene.
	XXX : I am not sure if it is correct to map 1 Blender Unit
	to 1 Meter for unit type NONE. But it looks reasonable to me.
*/
void bc_match_scale(std::vector<Object *> *objects_done, 
					Scene &sce, 
					UnitConverter &unit_converter) {

	Object *ob = NULL;

	PointerRNA scene_ptr, unit_settings;
	PropertyRNA *system_ptr, *scale_ptr;
	RNA_id_pointer_create(&sce.id, &scene_ptr);

	unit_settings = RNA_pointer_get(&scene_ptr, "unit_settings");
	system_ptr = RNA_struct_find_property(&unit_settings, "system");
	scale_ptr = RNA_struct_find_property(&unit_settings, "scale_length");

	int   type  = RNA_property_enum_get(&unit_settings, system_ptr);

	float bl_scale;
	
	switch (type) {
		case USER_UNIT_NONE:
			bl_scale = 1.0; // map 1 Blender unit to 1 Meter
			break;
		case USER_UNIT_METRIC:
			bl_scale = RNA_property_float_get(&unit_settings, scale_ptr);
			break;
		default:
			bl_scale = RNA_property_float_get(&unit_settings, scale_ptr);
			// it looks like the conversion to Imperial is done implicitly.
			// So nothing to do here.
			break;
	}
	
	float size_mat3[3][3];
	float size_mat4[4][4];

	float scale_conv = unit_converter.getLinearMeter() / bl_scale;

	float rescale[3];
	rescale[0] = rescale[1] = rescale[2] = scale_conv;

	size_to_mat3(size_mat3, rescale);
	copy_m4_m3(size_mat4, size_mat3);

	for (std::vector<Object *>::iterator it = objects_done->begin(); it != objects_done->end(); ++it) {
		ob = *it;
		mult_m4_m4m4(ob->obmat, ob->obmat, size_mat4);
		ob->obmat[3][0] = ob->loc[0] * rescale[0];
		ob->obmat[3][1] = ob->loc[1] * rescale[1];
		ob->obmat[3][2] = ob->loc[2] * rescale[2];
		BKE_object_apply_mat4(ob, ob->obmat, 0, 0);
	}

}
