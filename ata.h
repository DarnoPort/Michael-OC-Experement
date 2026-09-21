#ifndef NANOOS_ATA_H
#define NANOOS_ATA_H

#define ATA_SECTOR_SIZE 512U

int ata_init(void);
int ata_read_sector(unsigned int lba, void* buffer);
int ata_write_sector(unsigned int lba, const void* buffer);
int ata_flush(void);

#endif
