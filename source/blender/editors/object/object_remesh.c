/*
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
 * The Original Code is Copyright (C) 2019 by Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup edobj
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <ctype.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_mesh_types.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_mesh.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_customdata.h"
#include "BKE_mesh_remesh_voxel.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "ED_mesh.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "ED_undo.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "WM_api.h"
#include "WM_types.h"
#include "WM_message.h"
#include "WM_toolsystem.h"

#include "object_intern.h"  // own include

static bool object_remesh_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);

  if (BKE_object_is_in_editmode(ob)) {
    CTX_wm_operator_poll_msg_set(C, "The voxel remesher cannot run from edit mode.");
    return false;
  }

  if (ob->mode == OB_MODE_SCULPT && ob->sculpt->bm) {
    CTX_wm_operator_poll_msg_set(C, "The voxel remesher cannot run with dyntopo activated.");
  }

  return ED_operator_object_active_editable_mesh(C);
}

static int voxel_remesh_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);

  Mesh *mesh = ob->data;
  Mesh *new_mesh;

  if (mesh->remesh_voxel_size <= 0.0f) {
    BKE_report(op->reports, RPT_ERROR, "Voxel remesher cannot run with a voxel size of 0.0.");
    return OPERATOR_CANCELLED;
  }

  if (ob->mode == OB_MODE_SCULPT) {
    ED_sculpt_undo_geometry_begin(ob);
  }

  new_mesh = BKE_mesh_remesh_voxel_to_mesh_nomain(mesh, mesh->remesh_voxel_size);

  if (!new_mesh) {
    return OPERATOR_CANCELLED;
  }

  Mesh *obj_mesh_copy = NULL;
  if (mesh->flag & ME_REMESH_REPROJECT_PAINT_MASK) {
    obj_mesh_copy = BKE_mesh_new_nomain_from_template(mesh, mesh->totvert, 0, 0, 0, 0);
    CustomData_copy(
        &mesh->vdata, &obj_mesh_copy->vdata, CD_MASK_MESH.vmask, CD_DUPLICATE, mesh->totvert);
    for (int i = 0; i < mesh->totvert; i++) {
      copy_v3_v3(obj_mesh_copy->mvert[i].co, mesh->mvert[i].co);
    }
  }

  BKE_mesh_nomain_to_mesh(new_mesh, mesh, ob, &CD_MASK_MESH, true);

  if (mesh->flag & ME_REMESH_REPROJECT_PAINT_MASK) {
    BKE_remesh_reproject_paint_mask(mesh, obj_mesh_copy);
    BKE_mesh_free(obj_mesh_copy);
  }

  if (mesh->flag & ME_REMESH_SMOOTH_NORMALS) {
    BKE_mesh_smooth_flag_set(ob, true);
  }

  if (ob->mode == OB_MODE_SCULPT) {
    ED_sculpt_undo_geometry_end(ob);
  }

  BKE_mesh_batch_cache_dirty_tag(ob->data, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_voxel_remesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Voxel Remesh";
  ot->description =
      "Calculates a new manifold mesh based on the volume of the current mesh. All data layers "
      "will be lost";
  ot->idname = "OBJECT_OT_voxel_remesh";

  /* api callbacks */
  ot->poll = object_remesh_poll;
  ot->exec = voxel_remesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int quadriflow_remesh_exec(bContext *C, wmOperator *op)
{
  const bool is_frozen = RNA_boolean_get(op->ptr, "use_freeze");
  if (is_frozen) {
    BKE_report(
        op->reports,
        RPT_INFO,
        "Operator is frozen, changes to its settings won't take effect until you unfreeze it");
    return OPERATOR_FINISHED;
  }

  Object *ob = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);

  Mesh *mesh = ob->data;
  Mesh *new_mesh;

  if (ob->mode == OB_MODE_SCULPT) {
    ED_sculpt_undo_geometry_begin(ob);
  }
  const int target_faces = RNA_int_get(op->ptr, "target_faces");
  const int seed = RNA_int_get(op->ptr, "seed");
  const bool preserve_sharp = RNA_boolean_get(op->ptr, "preserve_sharp");
  const bool adaptive_scale = RNA_boolean_get(op->ptr, "adaptive_scale");

  const bool proj_paint_mask = RNA_boolean_get(op->ptr, "proj_paint_mask");
  const bool smooth_normals = RNA_boolean_get(op->ptr, "smooth_normals");

  new_mesh = BKE_mesh_remesh_quadriflow_to_mesh_nomain(
      mesh, target_faces, seed, preserve_sharp, adaptive_scale);

  if (!new_mesh) {
    return OPERATOR_CANCELLED;
  }

  Mesh *obj_mesh_copy = NULL;
  if (proj_paint_mask) {
    obj_mesh_copy = BKE_mesh_new_nomain_from_template(mesh, mesh->totvert, 0, 0, 0, 0);
    CustomData_copy(
        &mesh->vdata, &obj_mesh_copy->vdata, CD_MASK_MESH.vmask, CD_DUPLICATE, mesh->totvert);
    for (int i = 0; i < mesh->totvert; i++) {
      copy_v3_v3(obj_mesh_copy->mvert[i].co, mesh->mvert[i].co);
    }
  }

  BKE_mesh_nomain_to_mesh(new_mesh, mesh, ob, &CD_MASK_MESH, true);

  if (proj_paint_mask) {
    BKE_remesh_reproject_paint_mask(mesh, obj_mesh_copy);
    BKE_mesh_free(obj_mesh_copy);
  }

  if (smooth_normals) {
    BKE_mesh_smooth_flag_set(ob, true);
  }

  if (ob->mode == OB_MODE_SCULPT) {
    ED_sculpt_undo_geometry_end(ob);
  }

  BKE_mesh_batch_cache_dirty_tag(ob->data, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_quadriflow_remesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "QuadriFlow Remesh";
  ot->description =
      "Calculates a new quad based mesh using the surface data of the current mesh. All data "
      "layers will be lost";
  ot->idname = "OBJECT_OT_quadriflow_remesh";

  /* api callbacks */
  ot->poll = object_remesh_poll;
  ot->invoke = WM_operator_props_popup_confirm;
  ot->exec = quadriflow_remesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "preserve_sharp",
                  false,
                  "Preserve sharp",
                  "Try to preserve sharp features on the mesh");
  RNA_def_boolean(
      ot->srna, "adaptive_scale", false, "Adaptive scale", "Use adaptive scale when remeshing");

  RNA_def_boolean(ot->srna,
                  "proj_paint_mask",
                  false,
                  "Preserve paint mask",
                  "Reproject the paint mask onto the new mesh");

  RNA_def_boolean(ot->srna,
                  "smooth_normals",
                  false,
                  "Smooth normals",
                  "Set the output mesh normals to smooth");

  RNA_def_int(ot->srna,
              "target_faces",
              0,
              0,
              INT_MAX,
              "Output faces",
              "The amount of faces the solver should try to remesh with."
              "This is just a guideline and not a hard limit."
              "Zero output faces will set the face amount to auto",
              0,
              255);
  RNA_def_int(
      ot->srna, "seed", 0, 0, INT_MAX, "Seed", "Random seed to you with the solver", 0, 255);
  RNA_def_boolean(ot->srna,
                  "use_freeze",
                  false,
                  "Freeze Operator",
                  "Prevent changes to settings to re-run the operator, "
                  "handy to change several things at once with heavy geometry");
}
