/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2012-2015 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "ilo_debug.h"
#include "ilo_state_sol.h"

static bool
sol_stream_validate_gen7(const struct ilo_dev *dev,
                         const struct ilo_state_sol_stream_info *stream)
{
   uint8_t i;

   ILO_DEV_ASSERT(dev, 7, 8);

   assert(stream->vue_read_base + stream->vue_read_count <=
         stream->cv_vue_attr_count);

   /*
    * From the Ivy Bridge PRM, volume 2 part 1, page 200:
    *
    *     "(Stream 0 Vertex Read Offset)
    *      Format: U1 count of 256-bit units
    *
    *      Specifies amount of data to skip over before reading back Stream 0
    *      vertex data. Must be zero if the GS is enabled and the Output
    *      Vertex Size field in 3DSTATE_GS is programmed to 0 (i.e., one 16B
    *      unit)."
    *
    *     "(Stream 0 Vertex Read Length)
    *      Format: U5-1 count of 256-bit units
    *
    *      Specifies amount of vertex data to read back for Stream 0 vertices,
    *      starting at the Stream 0 Vertex Read Offset location. Maximum
    *      readback is 17 256-bit units (34 128-bit vertex attributes). Read
    *      data past the end of the valid vertex data has undefined contents,
    *      and therefore shouldn't be used to source stream out data.  Must be
    *      zero (i.e., read length = 256b) if the GS is enabled and the Output
    *      Vertex Size field in 3DSTATE_GS is programmed to 0 (i.e., one 16B
    *      unit)."
    */
   assert(stream->vue_read_base == 0 || stream->vue_read_base == 2);
   assert(stream->vue_read_count <= 34);

   assert(stream->decl_count <= ILO_STATE_SOL_MAX_DECL_COUNT);

   for (i = 0; i < stream->decl_count; i++) {
      const struct ilo_state_sol_decl_info *decl = &stream->decls[i];

      assert(decl->is_hole || decl->attr < stream->vue_read_count);

      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 205:
       *
       *     "There is only enough internal storage for the 128-bit vertex
       *      header and 32 128-bit vertex attributes."
       */
      assert(decl->attr < 33);

      assert(decl->component_base < 4 &&
             decl->component_base + decl->component_count <= 4);
      assert(decl->buffer < ILO_STATE_SOL_MAX_BUFFER_COUNT);
   }

   return true;
}

static bool
sol_validate_gen7(const struct ilo_dev *dev,
                  const struct ilo_state_sol_info *info)
{
   uint8_t i;

   ILO_DEV_ASSERT(dev, 7, 8);

   /*
    * From the Ivy Bridge PRM, volume 2 part 1, page 198:
    *
    *     "This bit (Render Stream Select) is used even if SO Function Enable
    *      is DISABLED."
    *
    * From the Haswell PRM, volume 2b, page 796:
    *
    *     "SO Function Enable must also be ENABLED in order for thiis field
    *      (Render Stream Select) to select a stream for rendering. When SO
    *      Function Enable is DISABLED and Rendering Disable is cleared (i.e.,
    *      rendering is enabled), StreamID is ignored downstream of the SO
    *      stage, allowing any stream to be rendered."
    *
    * We want Gen7 behavior, but we have to require users to follow Gen7.5
    * behavior: info->sol_enable must be set for info->render_stream to work.
    */

   for (i = 0; i < ARRAY_SIZE(info->streams); i++) {
      if (!sol_stream_validate_gen7(dev, &info->streams[i]))
         return false;
   }

   /*
    * From the Ivy Bridge PRM, volume 2 part 1, page 208:
    *
    *     "(Surface Pitch)
    *      [0,2048]  Must be 0 or a multiple of 4 Bytes."
    */
   for (i = 0; i < ARRAY_SIZE(info->buffer_strides); i++) {
      assert(info->buffer_strides[i] <= 2048 &&
             info->buffer_strides[i] % 4 == 0);
   }

   return true;
}

static bool
sol_set_gen7_3DSTATE_STREAMOUT(struct ilo_state_sol *so,
                               const struct ilo_dev *dev,
                               const struct ilo_state_sol_info *info)
{
   struct {
      uint8_t offset;
      uint8_t len;
   } vue_read[ILO_STATE_SOL_MAX_STREAM_COUNT];
   uint8_t i;
   uint32_t dw1, dw2;

   ILO_DEV_ASSERT(dev, 7, 8);

   if (!sol_validate_gen7(dev, info))
      return false;

   for (i = 0; i < ARRAY_SIZE(info->streams); i++) {
      const struct ilo_state_sol_stream_info *stream = &info->streams[i];

      vue_read[i].offset = stream->vue_read_base / 2;
      /*
       * In pairs minus 1.  URB entries are aligned to 512-bits.  There is no
       * need to worry about reading past entries.
       */
      vue_read[i].len = (stream->vue_read_count + 1) / 2;
      if (vue_read[i].len)
         vue_read[i].len--;
   }

   dw1 = info->render_stream << GEN7_SO_DW1_RENDER_STREAM_SELECT__SHIFT |
         info->tristrip_reorder << GEN7_SO_DW1_REORDER_MODE__SHIFT;

   if (info->sol_enable)
      dw1 |= GEN7_SO_DW1_SO_ENABLE;

   if (info->render_disable)
      dw1 |= GEN7_SO_DW1_RENDER_DISABLE;

   if (info->stats_enable)
      dw1 |= GEN7_SO_DW1_STATISTICS;

   if (ilo_dev_gen(dev) < ILO_GEN(8)) {
      const uint8_t buffer_enables =
         ((bool) info->buffer_strides[3]) << 3 |
         ((bool) info->buffer_strides[2]) << 2 |
         ((bool) info->buffer_strides[1]) << 1 |
         ((bool) info->buffer_strides[0]);

      dw1 |= buffer_enables << GEN7_SO_DW1_BUFFER_ENABLES__SHIFT;
   }

   dw2 = vue_read[3].offset << GEN7_SO_DW2_STREAM3_READ_OFFSET__SHIFT |
         vue_read[3].len << GEN7_SO_DW2_STREAM3_READ_LEN__SHIFT |
         vue_read[2].offset << GEN7_SO_DW2_STREAM2_READ_OFFSET__SHIFT |
         vue_read[2].len << GEN7_SO_DW2_STREAM2_READ_LEN__SHIFT |
         vue_read[1].offset << GEN7_SO_DW2_STREAM1_READ_OFFSET__SHIFT |
         vue_read[1].len << GEN7_SO_DW2_STREAM1_READ_LEN__SHIFT |
         vue_read[0].offset << GEN7_SO_DW2_STREAM0_READ_OFFSET__SHIFT |
         vue_read[0].len << GEN7_SO_DW2_STREAM0_READ_LEN__SHIFT;

   STATIC_ASSERT(ARRAY_SIZE(so->so) >= 4);
   so->so[0] = dw1;
   so->so[1] = dw2;

   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      uint32_t dw3, dw4;

      dw3 = info->buffer_strides[1] << GEN8_SO_DW3_BUFFER1_PITCH__SHIFT |
            info->buffer_strides[0] << GEN8_SO_DW3_BUFFER0_PITCH__SHIFT;
      dw4 = info->buffer_strides[3] << GEN8_SO_DW4_BUFFER3_PITCH__SHIFT |
            info->buffer_strides[2] << GEN8_SO_DW4_BUFFER2_PITCH__SHIFT;

      so->so[2] = dw3;
      so->so[3] = dw4;
   }

   return true;
}

static bool
sol_set_gen7_3DSTATE_SO_DECL_LIST(struct ilo_state_sol *so,
                                  const struct ilo_dev *dev,
                                  const struct ilo_state_sol_info *info,
                                  uint8_t max_decl_count)
{
   uint64_t decl_list[ILO_STATE_SOL_MAX_DECL_COUNT];
   uint8_t decl_counts[ILO_STATE_SOL_MAX_STREAM_COUNT];
   uint8_t buffer_selects[ILO_STATE_SOL_MAX_STREAM_COUNT];
   uint32_t dw1, dw2;
   uint8_t i, j;

   ILO_DEV_ASSERT(dev, 7, 8);

   memset(decl_list, 0, sizeof(decl_list[0]) * max_decl_count);

   for (i = 0; i < ARRAY_SIZE(info->streams); i++) {
      const struct ilo_state_sol_stream_info *stream = &info->streams[i];

      assert(stream->decl_count <= max_decl_count);
      decl_counts[i] = stream->decl_count;
      buffer_selects[i] = 0;

      for (j = 0; j < stream->decl_count; j++) {
         const struct ilo_state_sol_decl_info *decl = &stream->decls[j];
         const uint8_t mask = ((1 << decl->component_count) - 1) <<
            decl->component_base;
         uint16_t val;

         val = decl->buffer << GEN7_SO_DECL_OUTPUT_SLOT__SHIFT |
               mask << GEN7_SO_DECL_COMPONENT_MASK__SHIFT;

         if (decl->is_hole)
            val |= GEN7_SO_DECL_HOLE_FLAG;
         else
            val |= decl->attr << GEN7_SO_DECL_REG_INDEX__SHIFT;

         decl_list[j] |= (uint64_t) val << (16 * i);
         buffer_selects[i] |= 1 << decl->buffer;
      }
   }

   dw1 = buffer_selects[3] << GEN7_SO_DECL_DW1_STREAM3_BUFFER_SELECTS__SHIFT |
         buffer_selects[2] << GEN7_SO_DECL_DW1_STREAM2_BUFFER_SELECTS__SHIFT |
         buffer_selects[1] << GEN7_SO_DECL_DW1_STREAM1_BUFFER_SELECTS__SHIFT |
         buffer_selects[0] << GEN7_SO_DECL_DW1_STREAM0_BUFFER_SELECTS__SHIFT;
   dw2 = decl_counts[3] << GEN7_SO_DECL_DW2_STREAM3_ENTRY_COUNT__SHIFT |
         decl_counts[2] << GEN7_SO_DECL_DW2_STREAM2_ENTRY_COUNT__SHIFT |
         decl_counts[1] << GEN7_SO_DECL_DW2_STREAM1_ENTRY_COUNT__SHIFT |
         decl_counts[0] << GEN7_SO_DECL_DW2_STREAM0_ENTRY_COUNT__SHIFT;

   STATIC_ASSERT(ARRAY_SIZE(so->so) >= 6);
   so->so[4] = dw1;
   so->so[5] = dw2;

   STATIC_ASSERT(ARRAY_SIZE(so->decl[0]) == 2);
   memcpy(so->decl, decl_list, sizeof(so->decl[0]) * max_decl_count);
   so->decl_count = max_decl_count;

   return true;
}

bool
ilo_state_sol_init(struct ilo_state_sol *so,
                   const struct ilo_dev *dev,
                   const struct ilo_state_sol_info *info)
{
   bool ret = true;

   assert(ilo_is_zeroed(so, sizeof(*so)));
   assert(ilo_is_zeroed(info->data, info->data_size));

   if (ilo_dev_gen(dev) >= ILO_GEN(7)) {
      uint8_t max_decl_count, i;

      max_decl_count = info->streams[0].decl_count;
      for (i = 1; i < ARRAY_SIZE(info->streams); i++) {
         if (max_decl_count < info->streams[i].decl_count)
            max_decl_count = info->streams[i].decl_count;
      }

      assert(ilo_state_sol_data_size(dev, max_decl_count) <= info->data_size);
      so->decl = (uint32_t (*)[2]) info->data;

      ret &= sol_set_gen7_3DSTATE_STREAMOUT(so, dev, info);
      ret &= sol_set_gen7_3DSTATE_SO_DECL_LIST(so, dev, info, max_decl_count);
   }

   assert(ret);

   return ret;
}

bool
ilo_state_sol_init_disabled(struct ilo_state_sol *sol,
                            const struct ilo_dev *dev,
                            bool render_disable)
{
   struct ilo_state_sol_info info;

   memset(&info, 0, sizeof(info));
   info.render_disable = render_disable;

   return ilo_state_sol_init(sol, dev, &info);
}