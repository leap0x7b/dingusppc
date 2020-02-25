//DingusPPC
//Written by divingkatae and maximum
//(c)2018-20 (theweirdo)     spatium
//Please ask for permission
//if you want to distribute this.
//(divingkatae#1017 on Discord)

/** VIA-CUDA combo device emulation.

    Author: Max Poliakovski 2019
*/

#include <thirdparty/loguru.hpp>
#include <iostream>
#include <fstream>
#include <cinttypes>
#include <vector>
#include "viacuda.h"

using namespace std;

ViaCuda::ViaCuda()
{
    /* FIXME: is this the correct
       VIA initialization? */
    this->via_regs[VIA_A]    = 0x80;
    this->via_regs[VIA_DIRB] = 0xFF;
    this->via_regs[VIA_DIRA] = 0xFF;
    this->via_regs[VIA_T1LL] = 0xFF;
    this->via_regs[VIA_T1LH] = 0xFF;
    this->via_regs[VIA_IER]  = 0x7F;

    //PRAM Pre-Initialization
    this->pram_obj = new NVram("pram.bin", 256);

    this->cuda_init();
}

ViaCuda::~ViaCuda()
{
    if (this->pram_obj)
        delete (this->pram_obj);
}

void ViaCuda::cuda_init()
{
    this->old_tip = 0;
    this->old_byteack = 0;
    this->treq = 1;
    this->in_count = 0;
    this->out_count = 0;

}

uint8_t ViaCuda::read(int reg)
{
    uint8_t res;

    LOG_F(INFO, "Read VIA reg %x \n", (uint32_t)reg);

    res = this->via_regs[reg & 0xF];

    /* reading from some VIA registers triggers special actions */
    switch(reg & 0xF) {
    case VIA_B:
        res = this->via_regs[VIA_B];
        break;
    case VIA_A:
    case VIA_ANH:
        LOG_F(WARNING, "Attempted read from VIA Port A! \n");
        break;
    case VIA_IER:
        res |= 0x80; /* bit 7 always reads as "1" */
    }

    return res;
}

void ViaCuda::write(int reg, uint8_t value)
{
    switch(reg & 0xF) {
    case VIA_B:
        this->via_regs[VIA_B] = value;
        cuda_write(value);
        break;
    case VIA_A:
    case VIA_ANH:
        LOG_F(WARNING, "Attempted read from VIA Port A! \n");
        break;
    case VIA_DIRB:
        LOG_F(INFO, "VIA_DIRB = %x \n", (uint32_t)value);
        this->via_regs[VIA_DIRB] = value;
        break;
    case VIA_DIRA:
        LOG_F(INFO, "VIA_DIRA = %x \n", (uint32_t)value);
        this->via_regs[VIA_DIRA] = value;
        break;
    case VIA_PCR:
        LOG_F(INFO, "VIA_PCR =  %x \n", (uint32_t)value);
        this->via_regs[VIA_PCR] = value;
        break;
    case VIA_ACR:
        LOG_F(INFO, "VIA_ACR =  %x \n", (uint32_t)value);
        this->via_regs[VIA_ACR] = value;
        break;
    case VIA_IER:
        this->via_regs[VIA_IER] = (value & 0x80) ? value & 0x7F
                                  : this->via_regs[VIA_IER] & ~value;
        LOG_F(INFO, "VIA_IER updated to %d \n", (uint32_t)this->via_regs[VIA_IER]);
        print_enabled_ints();
        break;
    default:
        this->via_regs[reg & 0xF] = value;
    }
}

void ViaCuda::print_enabled_ints()
{
    vector<string> via_int_src = {"CA2", "CA1", "SR", "CB2", "CB1", "T2", "T1"};

    for (int i = 0; i < 7; i++) {
        if (this->via_regs[VIA_IER] & (1 << i))
            LOG_F(INFO, "VIA %s interrupt enabled \n", via_int_src[i].c_str());
    }
}

inline bool ViaCuda::cuda_ready()
{
    return ((this->via_regs[VIA_DIRB] & 0x38) == 0x30);
}

inline void ViaCuda::assert_sr_int()
{
    this->via_regs[VIA_IFR] |= 0x84;
}

void ViaCuda::cuda_write(uint8_t new_state)
{
    if (!cuda_ready()) {
        LOG_F(ERROR, "Cuda not ready! \n");
        return;
    }

    int new_tip = !!(new_state & CUDA_TIP);
    int new_byteack = !!(new_state & CUDA_BYTEACK);

    /* return if there is no state change */
    if (new_tip == this->old_tip && new_byteack == this->old_byteack)
        return;

    LOG_F(INFO, "Cuda state changed! \n");

    this->old_tip = new_tip;
    this->old_byteack = new_byteack;

    if (new_tip) {
        if (new_byteack) {
            this->via_regs[VIA_B] |= CUDA_TREQ; /* negate TREQ */
            this->treq = 1;

            if (this->in_count) {
                cuda_process_packet();

                /* start response transaction */
                this->via_regs[VIA_B] &= ~CUDA_TREQ; /* assert TREQ */
                this->treq = 0;
            }

            this->in_count = 0;
        } else {
            LOG_F(INFO, "Cuda: enter sync state \n");
            this->via_regs[VIA_B] &= ~CUDA_TREQ; /* assert TREQ */
            this->treq = 0;
            this->in_count = 0;
            this->out_count = 0;
        }

        assert_sr_int(); /* send dummy byte as idle acknowledge or attention */
    } else {
        if (this->via_regs[VIA_ACR] & 0x10) { /* data transfer: Host --> Cuda */
            if (this->in_count < 16) {
                this->in_buf[this->in_count++] = this->via_regs[VIA_SR];
                assert_sr_int(); /* tell the system we've read the data */
            } else {
                LOG_F(WARNING, "Cuda input buffer exhausted! \n");
            }
        } else { /* data transfer: Cuda --> Host */
            if (this->out_count) {
                this->via_regs[VIA_SR] = this->out_buf[this->out_pos++];

                if (this->out_pos >= this->out_count) {
                    LOG_F(INFO, "Cuda: sending last byte \n");
                    this->out_count = 0;
                    this->via_regs[VIA_B] |= CUDA_TREQ; /* negate TREQ */
                    this->treq = 1;
                }

                assert_sr_int(); /* tell the system we've written the data */
            }
        }
    }
}

void ViaCuda::cuda_response_header(uint32_t pkt_type, uint32_t pkt_flag)
{
    this->out_buf[0] = pkt_type;
    this->out_buf[1] = pkt_flag;
    this->out_buf[2] = this->in_buf[1]; /* copy original cmd */
    this->out_count = 3;
    this->out_pos = 0;
}

void ViaCuda::cuda_error_response(uint32_t error)
{
    this->out_buf[0] = CUDA_PKT_ERROR;
    this->out_buf[1] = error;
    this->out_buf[2] = this->in_buf[0];
    this->out_buf[3] = this->in_buf[1]; /* copy original cmd */
    this->out_count = 4;
    this->out_pos = 0;
}

void ViaCuda::cuda_process_packet()
{
    if (this->in_count < 2) {
        LOG_F(ERROR, "Cuda: invalid packet (too few data)! \n");
        return;
    }

    switch(this->in_buf[0]) {
    case CUDA_PKT_ADB:
        LOG_F(INFO, "Cuda: ADB packet received \n");
        break;
    case CUDA_PKT_PSEUDO:
        LOG_F(INFO, "Cuda: pseudo command packet received \n");
        LOG_F(INFO, "Command: %x \n", (uint32_t)(this->in_buf[1]));
        LOG_F(INFO, "Data count: %d \n ", this->in_count);
        for (int i = 0; i < this->in_count; i++) {
            LOG_F(INFO, "%x ,", (uint32_t)(this->in_buf[i]));
        }
        LOG_F(INFO, "\n");
        cuda_pseudo_command(this->in_buf[1], this->in_count - 2);
        break;
    default:
        LOG_F(ERROR, "Cuda: unsupported packet type = %d \n", (uint32_t)(this->in_buf[0]));
    }
}

void ViaCuda::cuda_pseudo_command(int cmd, int data_count)
{
    switch(cmd) {
    case CUDA_READ_PRAM:
        cuda_response_header(CUDA_PKT_PSEUDO, 0);
        this->pram_obj->read_byte(this->in_buf[2]);
        break;
    case CUDA_WRITE_PRAM:
        cuda_response_header(CUDA_PKT_PSEUDO, 0);
        this->pram_obj->write_byte(this->in_buf[2], this->in_buf[3]);
        break;
    case CUDA_READ_WRITE_I2C:
        cuda_response_header(CUDA_PKT_PSEUDO, 0);
        /* bit 0 of the I2C address byte indicates operation kind:
           0 - write to device, 1 - read from device
           In the case of reading, Cuda will append one-byte result
           to the response packet header */
        i2c_simple_transaction(this->in_buf[2], &this->in_buf[3], this->in_count - 3);
        break;
    case CUDA_COMB_FMT_I2C:
        /* HACK:
           This command performs the so-called open-ended transaction, i.e.
           Cuda will continue to send data as long as handshaking is completed
           for each byte. To support that, we'd need another emulation approach.
           Fortunately, HWInit is known to read/write max. 4 bytes at once
           so we're going to use a prefilled buffer to make it work.
        */
        cuda_response_header(CUDA_PKT_PSEUDO, 0);
        if (this->in_count >= 5) {
            i2c_comb_transaction(this->in_buf[2], this->in_buf[3], this->in_buf[4],
                &this->in_buf[5], this->in_count - 5);
        }
        break;
    case CUDA_OUT_PB0: /* undocumented call! */
        LOG_F(INFO, "Cuda: send %d to PB0 \n", (int)(this->in_buf[2]));
        cuda_response_header(CUDA_PKT_PSEUDO, 0);
        break;
    default:
        LOG_F(ERROR, "Cuda: unsupported pseudo command 0x%x \n", cmd);
        cuda_error_response(CUDA_ERR_BAD_CMD);
    }
}

void ViaCuda::i2c_simple_transaction(uint8_t dev_addr, const uint8_t *in_buf,
                                     int in_bytes)
{
    int tr_type = dev_addr & 1;

    switch(dev_addr & 0xFE) {
    case 0x50: /* unknown device on the Gossamer board */
        if (tr_type) { /* read */
            /* send dummy byte for now */
            this->out_buf[this->out_count++] = 0xDD;
        } else {
            /* ignore writes */
        }
        break;
    default:
        LOG_F(ERROR, "Unsupported I2C device 0x%x \n", (int)dev_addr);
        cuda_error_response(CUDA_ERR_I2C);
    }
}

void ViaCuda::i2c_comb_transaction(uint8_t dev_addr, uint8_t sub_addr,
    uint8_t dev_addr1, const uint8_t *in_buf, int in_bytes)
{
    int tr_type = dev_addr1 & 1;

    if ((dev_addr & 0xFE) != (dev_addr1 & 0xFE)) {
        LOG_F(ERROR, "I2C combined, dev_addr mismatch! \n");
        return;
    }

    switch(dev_addr1 & 0xFE) {
    case 0xAE: /* SDRAM EEPROM, no clue which one */
        if (tr_type) { /* read */
            if (sub_addr != 2) {
                LOG_F(ERROR, "Unsupported read position 0x%x in SDRAM EEPROM 0x%x", \
                    (int)sub_addr, (int)dev_addr1);
                return;
            }
            /* FIXME: hardcoded SPD EEPROM values! This should be a proper
               I2C device with user-configurable params */
            this->out_buf[this->out_count++] = 0x04; /* memory type = SDRAM */
            this->out_buf[this->out_count++] = 0x0B; /* row address bits per bank */
            this->out_buf[this->out_count++] = 0x09; /* col address bits per bank */
            this->out_buf[this->out_count++] = 0x02; /* num of RAM banks */
        } else {
            /* ignore writes */
        }
        break;
    default:
        LOG_F(ERROR, "Unsupported I2C device 0x%x \n", (int)dev_addr1);
        cuda_error_response(CUDA_ERR_I2C);
    }
}
