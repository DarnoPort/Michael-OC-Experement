#include "ata.h"

#define ATA_PRIMARY_DATA      0x1F0
#define ATA_PRIMARY_ERROR     0x1F1
#define ATA_PRIMARY_FEATURES  0x1F1
#define ATA_PRIMARY_SECCOUNT  0x1F2
#define ATA_PRIMARY_LBA_LOW   0x1F3
#define ATA_PRIMARY_LBA_MID   0x1F4
#define ATA_PRIMARY_LBA_HIGH  0x1F5
#define ATA_PRIMARY_DRIVE     0x1F6
#define ATA_PRIMARY_COMMAND   0x1F7
#define ATA_PRIMARY_STATUS    0x1F7

#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_STATUS_ERR        0x01
#define ATA_STATUS_DRQ        0x08
#define ATA_STATUS_DF         0x20
#define ATA_STATUS_BSY        0x80

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ __volatile__(
        "inb %1, %0"
        : "=a"(result)
        : "Nd"(port)
    );
    return result;
}

static inline unsigned short inw(unsigned short port) {
    unsigned short result;
    __asm__ __volatile__(
        "inw %1, %0"
        : "=a"(result)
        : "Nd"(port)
    );
    return result;
}

static inline void outb(
    unsigned short port,
    unsigned char data
) {
    __asm__ __volatile__(
        "outb %0, %1"
        :
        : "a"(data), "Nd"(port)
    );
}

static inline void outw(
    unsigned short port,
    unsigned short data
) {
    __asm__ __volatile__(
        "outw %0, %1"
        :
        : "a"(data), "Nd"(port)
    );
}

static void ata_delay_400ns(void) {
    (void)inb(ATA_PRIMARY_STATUS);
    (void)inb(ATA_PRIMARY_STATUS);
    (void)inb(ATA_PRIMARY_STATUS);
    (void)inb(ATA_PRIMARY_STATUS);
}

static unsigned int irq_save_ata(void) {
    unsigned int flags;

    __asm__ __volatile__(
        "pushfl\n"
        "popl %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void irq_restore_ata(unsigned int flags) {
    __asm__ __volatile__(
        "pushl %0\n"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static int ata_wait_ready(void) {
    unsigned int timeout = 1000000U;

    while (timeout-- > 0U) {
        unsigned char status =
            inb(ATA_PRIMARY_STATUS);

        if (status == 0) {
            return 0;
        }

        if (status & ATA_STATUS_ERR) {
            return 0;
        }

        if (status & ATA_STATUS_DF) {
            return 0;
        }

        if (!(status & ATA_STATUS_BSY)) {
            return (status & ATA_STATUS_DRQ) != 0;
        }
    }

    return 0;
}

static int ata_wait_not_busy(void) {
    unsigned int timeout = 1000000U;

    while (timeout-- > 0U) {
        unsigned char status =
            inb(ATA_PRIMARY_STATUS);

        if (status == 0) {
            return 0;
        }

        if (status & ATA_STATUS_ERR) {
            return 0;
        }

        if (status & ATA_STATUS_DF) {
            return 0;
        }

        if (!(status & ATA_STATUS_BSY)) {
            return 1;
        }
    }

    return 0;
}

static void ata_select_lba(unsigned int lba) {
    outb(
        ATA_PRIMARY_DRIVE,
        (unsigned char)(
            0xE0U |
            ((lba >> 24) & 0x0FU)
        )
    );
    ata_delay_400ns();
}

int ata_init(void) {
    unsigned int flags;
    unsigned char status;

    flags = irq_save_ata();

    ata_select_lba(0);

    outb(
        ATA_PRIMARY_SECCOUNT,
        0
    );
    outb(
        ATA_PRIMARY_LBA_LOW,
        0
    );
    outb(
        ATA_PRIMARY_LBA_MID,
        0
    );
    outb(
        ATA_PRIMARY_LBA_HIGH,
        0
    );
    outb(
        ATA_PRIMARY_COMMAND,
        ATA_CMD_IDENTIFY
    );

    status = inb(ATA_PRIMARY_STATUS);

    if (status == 0) {
        irq_restore_ata(flags);
        return 0;
    }

    if (!ata_wait_ready()) {
        irq_restore_ata(flags);
        return 0;
    }

    for (unsigned int i = 0; i < 256U; i++) {
        (void)inw(ATA_PRIMARY_DATA);
    }

    irq_restore_ata(flags);
    return 1;
}

int ata_read_sector(
    unsigned int lba,
    void* buffer
) {
    unsigned int flags;

    if (!buffer) {
        return 0;
    }

    flags = irq_save_ata();

    ata_select_lba(lba);

    outb(
        ATA_PRIMARY_SECCOUNT,
        1
    );
    outb(
        ATA_PRIMARY_LBA_LOW,
        (unsigned char)(lba & 0xFFU)
    );
    outb(
        ATA_PRIMARY_LBA_MID,
        (unsigned char)((lba >> 8) & 0xFFU)
    );
    outb(
        ATA_PRIMARY_LBA_HIGH,
        (unsigned char)((lba >> 16) & 0xFFU)
    );
    outb(
        ATA_PRIMARY_COMMAND,
        ATA_CMD_READ_PIO
    );

    if (!ata_wait_ready()) {
        irq_restore_ata(flags);
        return 0;
    }

    for (unsigned int i = 0; i < 256U; i++) {
        unsigned short word =
            inw(ATA_PRIMARY_DATA);

        ((unsigned char*)buffer)[i * 2U] =
            (unsigned char)(word & 0xFFU);
        ((unsigned char*)buffer)[i * 2U + 1U] =
            (unsigned char)(word >> 8);
    }

    if (!ata_wait_not_busy()) {
        irq_restore_ata(flags);
        return 0;
    }

    irq_restore_ata(flags);
    return 1;
}

int ata_write_sector(
    unsigned int lba,
    const void* buffer
) {
    unsigned int flags;

    if (!buffer) {
        return 0;
    }

    flags = irq_save_ata();

    ata_select_lba(lba);

    outb(
        ATA_PRIMARY_SECCOUNT,
        1
    );
    outb(
        ATA_PRIMARY_LBA_LOW,
        (unsigned char)(lba & 0xFFU)
    );
    outb(
        ATA_PRIMARY_LBA_MID,
        (unsigned char)((lba >> 8) & 0xFFU)
    );
    outb(
        ATA_PRIMARY_LBA_HIGH,
        (unsigned char)((lba >> 16) & 0xFFU)
    );
    outb(
        ATA_PRIMARY_COMMAND,
        ATA_CMD_WRITE_PIO
    );

    if (!ata_wait_ready()) {
        irq_restore_ata(flags);
        return 0;
    }

    for (unsigned int i = 0; i < 256U; i++) {
        unsigned short word =
            (unsigned short)(
                ((const unsigned char*)buffer)[i * 2U] |
                ((unsigned short)
                    ((const unsigned char*)buffer)[i * 2U + 1U]
                    << 8)
            );

        outw(
            ATA_PRIMARY_DATA,
            word
        );
    }

    if (!ata_wait_not_busy()) {
        irq_restore_ata(flags);
        return 0;
    }

    if (!ata_flush()) {
        irq_restore_ata(flags);
        return 0;
    }

    irq_restore_ata(flags);
    return 1;
}

int ata_flush(void) {
    outb(
        ATA_PRIMARY_COMMAND,
        ATA_CMD_CACHE_FLUSH
    );

    return ata_wait_not_busy();
}
