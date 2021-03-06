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
 * Contributor(s): Joseph Eagar.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/bmesh/operators/bmo_triangulate.c
 *  \ingroup bmesh
 *
 * Triangulate faces, also defines triangle fill.
 */

#include "DNA_listBase.h"

#include "BLI_math.h"
#include "BLI_smallhash.h"
#include "BLI_scanfill.h"

#include "bmesh.h"
#include "bmesh_tools.h"
#include "intern/bmesh_operators_private.h"


#define ELE_NEW		1
#define EDGE_MARK	4

void bmo_triangulate_exec(BMesh *bm, BMOperator *op)
{
	const int quad_method = BMO_slot_int_get(op->slots_in, "quad_method");
	const int ngon_method = BMO_slot_int_get(op->slots_in, "ngon_method");

	BMOpSlot *slot_facemap_out = BMO_slot_get(op->slots_out, "face_map.out");

	BM_mesh_elem_hflag_disable_all(bm, BM_FACE | BM_EDGE, BM_ELEM_TAG, false);
	BMO_slot_buffer_hflag_enable(bm, op->slots_in, "faces", BM_FACE, BM_ELEM_TAG, false);

	BM_mesh_triangulate(bm, quad_method, ngon_method, true, op, slot_facemap_out);

	BMO_slot_buffer_from_enabled_hflag(bm, op, op->slots_out, "edges.out", BM_EDGE, BM_ELEM_TAG);
	BMO_slot_buffer_from_enabled_hflag(bm, op, op->slots_out, "faces.out", BM_FACE, BM_ELEM_TAG);
}


void bmo_triangle_fill_exec(BMesh *bm, BMOperator *op)
{
	const bool use_beauty = BMO_slot_bool_get(op->slots_in, "use_beauty");
	const bool use_dissolve = BMO_slot_bool_get(op->slots_in, "use_dissolve");
	BMOIter siter;
	BMEdge *e;
	ScanFillContext sf_ctx;
	/* ScanFillEdge *sf_edge; */ /* UNUSED */
	ScanFillVert *sf_vert_1, *sf_vert_2;
	ScanFillFace *sf_tri;
	SmallHash hash;
	float normal[3], *normal_pt;

	BLI_smallhash_init_ex(&hash, BMO_slot_buffer_count(op->slots_in, "edges"));

	BMO_slot_vec_get(op->slots_in, "normal", normal);
	
	BLI_scanfill_begin(&sf_ctx);
	
	BMO_ITER (e, &siter, op->slots_in, "edges", BM_EDGE) {
		BMO_elem_flag_enable(bm, e, EDGE_MARK);
		
		if ((sf_vert_1 = BLI_smallhash_lookup(&hash, (uintptr_t)e->v1)) == NULL) {
			sf_vert_1 = BLI_scanfill_vert_add(&sf_ctx, e->v1->co);
			sf_vert_1->tmp.p = e->v1;
			BLI_smallhash_insert(&hash, (uintptr_t)e->v1, sf_vert_1);
		}
		
		if ((sf_vert_2 = BLI_smallhash_lookup(&hash, (uintptr_t)e->v2)) == NULL) {
			sf_vert_2 = BLI_scanfill_vert_add(&sf_ctx, e->v2->co);
			sf_vert_2->tmp.p = e->v2;
			BLI_smallhash_insert(&hash, (uintptr_t)e->v2, sf_vert_2);
		}
		

		/* sf_edge = */ BLI_scanfill_edge_add(&sf_ctx, sf_vert_1, sf_vert_2);
		/* sf_edge->tmp.p = e; */ /* UNUSED */
	}
	
	if (is_zero_v3(normal)) {
		normal_pt = NULL;
	}
	else {
		normalize_v3(normal);
		normal_pt = normal;
	}

	BLI_scanfill_calc_ex(&sf_ctx, BLI_SCANFILL_CALC_HOLES | BLI_SCANFILL_CALC_LOOSE, normal_pt);
	
	for (sf_tri = sf_ctx.fillfacebase.first; sf_tri; sf_tri = sf_tri->next) {
		BMFace *f = BM_face_create_quad_tri(bm,
		                                    sf_tri->v1->tmp.p, sf_tri->v2->tmp.p, sf_tri->v3->tmp.p, NULL,
		                                    NULL, true);
		BMLoop *l;
		BMIter liter;
		
		BMO_elem_flag_enable(bm, f, ELE_NEW);
		BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
			if (!BMO_elem_flag_test(bm, l->e, EDGE_MARK)) {
				BMO_elem_flag_enable(bm, l->e, ELE_NEW);
			}
		}
	}
	
	BLI_scanfill_end(&sf_ctx);
	BLI_smallhash_release(&hash);
	
	if (use_beauty) {
		BMOperator bmop;

		BMO_op_initf(bm, &bmop, op->flag, "beautify_fill faces=%ff edges=%Fe", ELE_NEW, EDGE_MARK);
		BMO_op_exec(bm, &bmop);
		BMO_slot_buffer_flag_enable(bm, bmop.slots_out, "geom.out", BM_FACE | BM_EDGE, ELE_NEW);
		BMO_op_finish(bm, &bmop);
	}
	
	BMO_slot_buffer_from_enabled_flag(bm, op, op->slots_out, "geom.out", BM_EDGE | BM_FACE, ELE_NEW);

	if (use_dissolve) {
		BMO_ITER (e, &siter, op->slots_out, "geom.out", BM_EDGE) {
			if (LIKELY(e->l)) {  /* in rare cases the edges face will have already been removed from the edge */
				BMFace *f_new;
				f_new = BM_faces_join_pair(bm, e->l->f,
				                           e->l->radial_next->f, e,
				                           false); /* join faces */
				if (f_new) {
					BMO_elem_flag_enable(bm, f_new, ELE_NEW);
					BM_edge_kill(bm, e);
				}
				else {
					BMO_error_clear(bm);
				}
			}
			else {
				BM_edge_kill(bm, e);
			}
		}

		BMO_slot_buffer_from_enabled_flag(bm, op, op->slots_out, "geom.out", BM_EDGE | BM_FACE, ELE_NEW);
	}
}
