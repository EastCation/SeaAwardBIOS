#ifndef __ESP_SCSI_H
#define __ESP_SCSI_H

struct disk_op_s;
struct drive_s;
int esp_scsi_process_op(struct disk_op_s *op);
int esp_scsi_drive_id(struct drive_s *drive, int *target, int *lun);
void esp_scsi_setup(void);

#endif /* __ESP_SCSI_H */
