/*
 * GStreamer
 * Copyright (C) 2019 RidgeRun
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "postprocess.h"
#include <assert.h>
#include <string.h>
#include <math.h>

#define TOTAL_BOXES_5 845

/* Functions declaration*/

static gdouble gst_intersection_over_union (BBox box_1, BBox box_2);
static void gst_delete_box (BBox * boxes, gint * num_boxes, gint index);
static void gst_remove_duplicated_boxes (gfloat iou_thresh, BBox * boxes,
    gint * num_boxes);
static void gst_box_to_pixels (BBox * normalized_box, gint row, gint col,
    gint box);
static gdouble gst_sigmoid (gdouble x);
static void gst_get_boxes_from_prediction (gfloat obj_thresh,
    gfloat prob_thresh, gpointer prediction, BBox * boxes, gint * elements,
    gint grid_h, gint grid_w, gint boxes_size);

gboolean
gst_fill_classification_meta (GstClassificationMeta * class_meta,
    const gpointer prediction, gsize predsize)
{
  assert (class_meta != NULL);
  assert (prediction != NULL);

  class_meta->num_labels = predsize / sizeof (gfloat);
  class_meta->label_probs =
      g_malloc (class_meta->num_labels * sizeof (gdouble));
  for (gint i = 0; i < class_meta->num_labels; ++i) {
    class_meta->label_probs[i] = (gdouble) ((gfloat *) prediction)[i];
  }
  return TRUE;

}

static gdouble
gst_intersection_over_union (BBox box_1, BBox box_2)
{
  /*
   * Evaluate the intersection-over-union for two boxes
   * The intersection-over-union metric determines how close
   * two boxes are to being the same box.
   */
  gdouble intersection_dim_1;
  gdouble intersection_dim_2;
  gdouble intersection_area;
  gdouble union_area;

  /* First diminsion of the intersecting box */
  intersection_dim_1 =
      MIN (box_1.x + box_1.width, box_2.x + box_2.width) - MAX (box_1.x,
      box_2.x);

  /* Second dimension of the intersecting box */
  intersection_dim_2 =
      MIN (box_1.y + box_1.height, box_2.y + box_2.height) - MAX (box_1.y,
      box_2.y);

  if ((intersection_dim_1 < 0) || (intersection_dim_2 < 0)) {
    intersection_area = 0;
  } else {
    intersection_area = intersection_dim_1 * intersection_dim_2;
  }
  union_area = box_1.width * box_1.height + box_2.width * box_2.height -
      intersection_area;
  return intersection_area / union_area;
}

static void
gst_delete_box (BBox * boxes, gint * num_boxes, gint index)
{
  gint i, last_index;

  assert (boxes != NULL);
  assert (num_boxes != NULL);

  if (*num_boxes > 0 && index < *num_boxes && index > -1) {
    last_index = *num_boxes - 1;
    for (i = index; i < last_index; i++) {
      boxes[i] = boxes[i + 1];
    }
    *num_boxes -= 1;
  }
}

static void
gst_remove_duplicated_boxes (gfloat iou_thresh, BBox * boxes, gint * num_boxes)
{
  /* Remove duplicated boxes. A box is considered a duplicate if its
   * intersection over union metric is above a threshold
   */
  gdouble iou;
  gint it1, it2;

  assert (boxes != NULL);
  assert (num_boxes != NULL);

  for (it1 = 0; it1 < *num_boxes - 1; it1++) {
    for (it2 = it1 + 1; it2 < *num_boxes; it2++) {
      if (boxes[it1].label == boxes[it2].label) {
        iou = gst_intersection_over_union (boxes[it1], boxes[it2]);
        if (iou > iou_thresh) {
          if (boxes[it1].prob > boxes[it2].prob) {
            gst_delete_box (boxes, num_boxes, it2);
            it2--;
          } else {
            gst_delete_box (boxes, num_boxes, it1);
            it1--;
            break;
          }
        }
      }
    }
  }
}

/* sigmoid approximation as a lineal function */
static gdouble
gst_sigmoid (gdouble x)
{
  return 1.0 / (1.0 + pow (M_E, -1.0 * x));
}

static void
gst_box_to_pixels (BBox * normalized_box, gint row, gint col, gint box)
{
  gint grid_size = 32;
  const gfloat box_anchors[] =
      { 1.08, 1.19, 3.42, 4.41, 6.63, 11.38, 9.42, 5.11, 16.62, 10.52 };

  assert (normalized_box != NULL);

  /* adjust the box center according to its cell and grid dim */
  normalized_box->x = (col + gst_sigmoid (normalized_box->x)) * grid_size;
  normalized_box->y = (row + gst_sigmoid (normalized_box->y)) * grid_size;

  /* adjust the lengths and widths */
  normalized_box->width =
      pow (M_E, normalized_box->width) * box_anchors[2 * box] * grid_size;
  normalized_box->height =
      pow (M_E, normalized_box->height) * box_anchors[2 * box + 1] * grid_size;
}

static void
gst_get_boxes_from_prediction (gfloat obj_thresh, gfloat prob_thresh,
    gpointer prediction, BBox * boxes, gint * elements, gint grid_h,
    gint grid_w, gint boxes_size)
{
  gint i, j, c, b;
  gint index;
  gdouble obj_prob;
  gdouble cur_class_prob, max_class_prob;
  gint max_class_prob_index;
  gint counter = 0;
  gint box_dim = 5;
  gint classes = 20;

  assert (boxes != NULL);
  assert (elements != NULL);

  /* Iterate rows */
  for (i = 0; i < grid_h; i++) {
    /* Iterate colums */
    for (j = 0; j < grid_w; j++) {
      /* Iterate boxes */
      for (b = 0; b < boxes_size; b++) {
        index = ((i * grid_w + j) * boxes_size + b) * (box_dim + classes);
        obj_prob = ((gfloat *) prediction)[index + 4];
        /* If the Objectness score is over the threshold add it to the boxes list */
        if (obj_prob > obj_thresh) {
          max_class_prob = 0;
          max_class_prob_index = 0;
          for (c = 0; c < classes; c++) {
            cur_class_prob = ((gfloat *) prediction)[index + box_dim + c];
            if (cur_class_prob > max_class_prob) {
              max_class_prob = cur_class_prob;
              max_class_prob_index = c;
            }
          }
          if (max_class_prob > prob_thresh) {
            BBox result;
            result.label = max_class_prob_index;
            result.prob = max_class_prob;
            result.x = ((gfloat *) prediction)[index];
            result.y = ((gfloat *) prediction)[index + 1];
            result.width = ((gfloat *) prediction)[index + 2];
            result.height = ((gfloat *) prediction)[index + 3];
            gst_box_to_pixels (&result, i, j, b);
            result.x = result.x - result.width * 0.5;
            result.y = result.y - result.height * 0.5;
            boxes[counter] = result;
            counter = counter + 1;
          }
        }
      }
    }
    *elements = counter;
  }
}

gboolean
gst_create_boxes (GstVideoInference * vi, const gpointer prediction,
    GstDetectionMeta * detect_meta, GstVideoInfo * info_model,
    gboolean * valid_prediction, BBox ** resulting_boxes,
    gint * elements, gfloat obj_thresh, gfloat prob_thresh, gfloat iou_thresh)
{
  gint grid_h = 13;
  gint grid_w = 13;
  gint boxes_size = 5;
  BBox boxes[TOTAL_BOXES_5];
  *elements = 0;

  assert (vi != NULL);
  assert (prediction != NULL);
  assert (detect_meta != NULL);
  assert (info_model != NULL);
  assert (valid_prediction != NULL);
  assert (resulting_boxes != NULL);
  assert (elements != NULL);

  gst_get_boxes_from_prediction (obj_thresh, prob_thresh, prediction, boxes,
      elements, grid_h, grid_w, boxes_size);
  gst_remove_duplicated_boxes (iou_thresh, boxes, elements);

  *resulting_boxes = g_malloc (*elements * sizeof (BBox));
  memcpy (*resulting_boxes, boxes, *elements * sizeof (BBox));
  return TRUE;
}