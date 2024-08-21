#ifndef DV_INJECTION_HH
#define DV_INJECTION_HH

#include <linux/amlogic/media/vfm/vframe.h>

bool dv_metadata_injection(struct vframe_s *vf);
void update_dv_hdr_dm_pkt(void *sei_data, uint32_t sei_size, struct vframe_s *vf);
bool should_block_vsif(bool dv_metadata);

#endif
