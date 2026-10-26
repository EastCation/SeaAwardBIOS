#ifndef __LSI_SCSI_H
#define __LSI_SCSI_H

struct disk_op_s;
struct drive_s;
int lsi_scsi_process_op(struct disk_op_s *op);
int lsi_scsi_drive_id(struct drive_s *drive, int *target, int *lun);
void lsi_scsi_setup(void);

#endif /* __LSI_SCSI_H */
