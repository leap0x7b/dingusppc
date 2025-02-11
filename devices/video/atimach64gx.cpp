/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-24 divingkatae and maximum
                      (theweirdo)     spatium

(Contact divingkatae#1017 or powermax#2286 on Discord for more info)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/** ATI Mach64 GX emulation.
   It emulates an ATI88800GX controller with an IBM RGB514 style RAMDAC.
   Emulation is limited to a basic frame buffer for now.
 */

#include <core/bitops.h>
#include <devices/deviceregistry.h>
#include <devices/video/atimach64defs.h>
#include <devices/video/atimach64gx.h>
#include <devices/video/displayid.h>
#include <devices/video/rgb514defs.h>
#include <loguru.hpp>
#include <memaccess.h>

#include <string>

/* Human readable Mach64 HW register names for easier debugging. */
static const std::map<uint16_t, std::string> mach64_reg_names = {
    #define one_reg_name(x) {ATI_ ## x, #x}
    one_reg_name(CRTC_H_TOTAL_DISP),
    one_reg_name(CRTC_H_SYNC_STRT_WID),
    one_reg_name(CRTC_V_TOTAL_DISP),
    one_reg_name(CRTC_V_SYNC_STRT_WID),
    one_reg_name(CRTC_VLINE_CRNT_VLINE),
    one_reg_name(CRTC_OFF_PITCH),
    one_reg_name(CRTC_INT_CNTL),
    one_reg_name(CRTC_GEN_CNTL),
    one_reg_name(DSP_CONFIG),
    one_reg_name(DSP_ON_OFF),
    one_reg_name(MEM_BUF_CNTL),
    one_reg_name(MEM_ADDR_CFG),
    one_reg_name(OVR_CLR),
    one_reg_name(OVR_WID_LEFT_RIGHT),
    one_reg_name(OVR_WID_TOP_BOTTOM),
    one_reg_name(CUR_CLR0),
    one_reg_name(CUR_CLR1),
    one_reg_name(CUR_OFFSET),
    one_reg_name(CUR_HORZ_VERT_POSN),
    one_reg_name(CUR_HORZ_VERT_OFF),
    one_reg_name(GP_IO),
    one_reg_name(HW_DEBUG),
    one_reg_name(SCRATCH_REG0),
    one_reg_name(SCRATCH_REG1),
    one_reg_name(SCRATCH_REG2),
    one_reg_name(SCRATCH_REG3),
    one_reg_name(CLOCK_CNTL),
    one_reg_name(BUS_CNTL),
    one_reg_name(EXT_MEM_CNTL),
    one_reg_name(MEM_CNTL),
    one_reg_name(DAC_REGS),
    one_reg_name(DAC_CNTL),
    one_reg_name(GEN_TEST_CNTL),
    one_reg_name(CUSTOM_MACRO_CNTL),
    one_reg_name(CONFIG_CNTL),
    one_reg_name(CONFIG_CHIP_ID),
    one_reg_name(CONFIG_STAT0),
    one_reg_name(DST_OFF_PITCH),
    one_reg_name(DST_X),
    one_reg_name(DST_Y),
    one_reg_name(DST_WIDTH),
    one_reg_name(DST_HEIGHT),
    one_reg_name(SRC_CNTL),
    one_reg_name(SCALE_3D_CNTL),
    one_reg_name(PAT_REG0),
    one_reg_name(PAT_REG1),
    one_reg_name(SC_LEFT),
    one_reg_name(SC_RIGHT),
    one_reg_name(SC_TOP),
    one_reg_name(SC_BOTTOM),
    one_reg_name(DP_BKGD_CLR),
    one_reg_name(DP_FRGD_CLR), // also DP_FOG_CLR for GT
    one_reg_name(DP_WRITE_MSK),
    one_reg_name(DP_PIX_WIDTH),
    one_reg_name(DP_MIX),
    one_reg_name(DP_SRC),
    one_reg_name(CLR_CMP_CNTL),
    one_reg_name(FIFO_STAT),
    one_reg_name(GUI_TRAJ_CNTL),
    one_reg_name(GUI_STAT),
    one_reg_name(MPP_CONFIG),
    one_reg_name(MPP_STROBE_SEQ),
    one_reg_name(MPP_ADDR),
    one_reg_name(MPP_DATA),
    one_reg_name(TVO_CNTL),
    one_reg_name(SETUP_CNTL),
    #undef one_reg_name
};

static const std::map<uint16_t, std::string> rgb514_reg_names = {
    #define one_reg_name(x) {Rgb514::x, #x}
    one_reg_name(MISC_CLK_CNTL),
    one_reg_name(HOR_SYNC_POS),
    one_reg_name(PWR_MNMGMT),
    one_reg_name(PIX_FORMAT),
    one_reg_name(PLL_CTL_1),
    one_reg_name(F0_M0),
    one_reg_name(F1_N0),
    one_reg_name(MISC_CNTL_1),
    one_reg_name(MISC_CNTL_2),
    one_reg_name(VRAM_MASK_LO),
    one_reg_name(VRAM_MASK_HI),
    #undef one_reg_name
};

AtiMach64Gx::AtiMach64Gx()
    : PCIDevice("ati-mach64-gx"), VideoCtrlBase(1024, 768)
{
    supports_types(HWCompType::MMIO_DEV | HWCompType::PCI_DEV);

    // set up PCI configuration space header
    this->vendor_id   = PCI_VENDOR_ATI;
    this->device_id   = ATI_MACH64_GX_DEV_ID;
    this->class_rev   = (0x030000 << 8) | 0x03;
    for (int i = 0; i < this->aperture_count; i++) {
        this->bars_cfg[i] = (uint32_t)(-this->aperture_size[i] | this->aperture_flag[i]);
    }
    this->finish_config_bars();

    this->pci_notify_bar_change = [this](int bar_num) {
        this->notify_bar_change(bar_num);
    };

    // declare expansion ROM containing FCode and Mac OS drivers
    if (this->attach_exp_rom_image(std::string("113-32900-004_Apple_MACH64.bin"))) {
        ABORT_F("%s: could not load ROM - this device won't work properly!",
                this->name.c_str());
    }

    // initialize display identification
    this->disp_id = std::unique_ptr<DisplayID> (new DisplayID(0x07, 0x3A));

    // allocate video RAM
    this->vram_size = 2 << 20; // 2MB ; up to 6MB supported
    this->vram_ptr = std::unique_ptr<uint8_t[]> (new uint8_t[this->vram_size]);

    // set up RAMDAC identification
    this->regs[ATI_CONFIG_STAT0] = 1 << 9;

    // stuff default values into chip registers
    // this->regs[ATI_CONFIG_CHIP_ID] = (asic_id << ATI_CFG_CHIP_MAJOR) | (dev_id << ATI_CFG_CHIP_TYPE);

    // set the FIFO
    insert_bits<uint32_t>(this->regs[ATI_GUI_STAT], 32, ATI_FIFO_CNT, ATI_FIFO_CNT_size);

    set_bit(regs[ATI_CRTC_GEN_CNTL], ATI_CRTC_DISPLAY_DIS); // because blank_on is true
}

void AtiMach64Gx::change_one_bar(uint32_t &aperture, uint32_t aperture_size, uint32_t aperture_new, int bar_num) {
    if (aperture != aperture_new) {
        if (aperture)
            this->host_instance->pci_unregister_mmio_region(aperture, aperture_size, this);

        aperture = aperture_new;
        if (aperture)
            this->host_instance->pci_register_mmio_region(aperture, aperture_size, this);

        LOG_F(INFO, "%s: aperture[%d] set to 0x%08X", this->name.c_str(), bar_num, aperture);
    }
}

void AtiMach64Gx::notify_bar_change(int bar_num)
{
    if (bar_num) // only BAR0 is supported
        return;

    {
        change_one_bar(this->aperture_base[bar_num], this->aperture_size[bar_num], this->bars[bar_num] & ~15, bar_num);
        // copy aperture address to CONFIG_CNTL:CFG_MEM_AP_LOC
        insert_bits<uint32_t>(this->config_cntl, this->aperture_base[0] >> 22, ATI_CFG_MEM_AP_LOC, ATI_CFG_MEM_AP_LOC_size);
    }
}

#if 0
uint32_t AtiMach64Gx::pci_cfg_read(uint32_t reg_offs, AccessDetails &details)
{
    if (reg_offs < 64) {
        return PCIDevice::pci_cfg_read(reg_offs, details);
    }

    switch (reg_offs) {
    default:
        LOG_READ_UNIMPLEMENTED_CONFIG_REGISTER();
    }

    return 0;
}

void AtiMach64Gx::pci_cfg_write(uint32_t reg_offs, uint32_t value, AccessDetails &details)
{
    if (reg_offs < 64) {
        PCIDevice::pci_cfg_write(reg_offs, value, details);
        return;
    }

    switch (reg_offs) {
    default:
        LOG_WRITE_UNIMPLEMENTED_CONFIG_REGISTER();
    }
}
#endif

// map I/O register index to MMIO register offset
static const uint32_t io_idx_to_reg_offset[32] = {
    ATI_CRTC_H_TOTAL_DISP,
    ATI_CRTC_H_SYNC_STRT_WID,
    ATI_CRTC_V_TOTAL_DISP,
    ATI_CRTC_V_SYNC_STRT_WID,
    ATI_CRTC_VLINE_CRNT_VLINE,
    ATI_CRTC_OFF_PITCH,
    ATI_CRTC_INT_CNTL,
    ATI_CRTC_GEN_CNTL,
    ATI_OVR_CLR,
    ATI_OVR_WID_LEFT_RIGHT,
    ATI_OVR_WID_TOP_BOTTOM,
    ATI_CUR_CLR0,
    ATI_CUR_CLR1,
    ATI_CUR_OFFSET,
    ATI_CUR_HORZ_VERT_POSN,
    ATI_CUR_HORZ_VERT_OFF,
    ATI_SCRATCH_REG0,
    ATI_SCRATCH_REG1,
    ATI_CLOCK_CNTL,
    ATI_BUS_CNTL,
    ATI_MEM_CNTL,
    ATI_MEM_VGA_WP_SEL,
    ATI_MEM_VGA_RP_SEL,
    ATI_DAC_REGS,
    ATI_DAC_CNTL,
    ATI_GEN_TEST_CNTL,
    ATI_CONFIG_CNTL,
    ATI_CONFIG_CHIP_ID,
    ATI_CONFIG_STAT0,
    ATI_GX_CONFIG_STAT1,
    ATI_INVALID,
    ATI_CRTC_H_TOTAL_DISP,
};

enum {
    SPARSE_IO_BASE = 0x2EC
};

bool AtiMach64Gx::io_access_allowed(uint32_t offset) {
    if ((offset & 0xFFFF03FC) == SPARSE_IO_BASE) {
        if (this->command & 1) {
            return true;
        }
        LOG_F(WARNING, "ATI I/O space disabled in the command reg");
    }
    return false;
}

bool AtiMach64Gx::pci_io_read(uint32_t offset, uint32_t size, uint32_t* res)
{
    if (!this->io_access_allowed(offset)) {
        return false;
    }

    uint32_t result = 0;

    // convert ISA-style I/O address to MMIO register offset
    offset = io_idx_to_reg_offset[(offset >> 10) & 0x1F] * 4 + (offset & 3);

    // CONFIG_CNTL is accessible from I/O space only
    if ((offset >> 2) == ATI_CONFIG_CNTL) {
        result = read_mem(((uint8_t *)&this->config_cntl) + (offset & 3), size);
    } else {
        result = BYTESWAP_SIZED(this->read_reg(offset, size), size);
    }

    *res = result;
    return true;
}

bool AtiMach64Gx::pci_io_write(uint32_t offset, uint32_t value, uint32_t size)
{
    if (!this->io_access_allowed(offset)) {
        return false;
    }

    // convert ISA-style I/O address to MMIO register offset
    offset = io_idx_to_reg_offset[(offset >> 10) & 0x1F] * 4 + (offset & 3);

    // CONFIG_CNTL is accessible from I/O space only
    if ((offset >> 2) == ATI_CONFIG_CNTL) {
        write_mem(((uint8_t *)&this->config_cntl) + (offset & 3), value, size);
        switch (this->config_cntl & 3) {
        case 0:
            LOG_F(WARNING, "%s: linear aperture disabled!", this->name.c_str());
            break;
        case 1:
            LOG_F(INFO, "%s: aperture size set to 4MB", this->name.c_str());
            this->mm_regs_offset = MM_REGS_2_OFF;
            break;
        case 2:
            LOG_F(INFO, "%s: aperture size set to 8MB", this->name.c_str());
            this->mm_regs_offset = MM_REGS_0_OFF;
            break;
        default:
            LOG_F(ERROR, "%s: invalid aperture size in CONFIG_CNTL", this->name.c_str());
        }
    } else {
        this->write_reg(offset, BYTESWAP_SIZED(value, size), size);
    }

    return true;
}

const char* AtiMach64Gx::get_reg_name(uint32_t reg_num) {
    auto iter = mach64_reg_names.find(reg_num);
    if (iter != mach64_reg_names.end()) {
        return iter->second.c_str();
    } else {
        return "unknown Mach64 register";
    }
}

uint32_t AtiMach64Gx::read_reg(uint32_t reg_offset, uint32_t size)
{
    uint32_t reg_num = reg_offset >> 2;
    uint32_t offset = reg_offset & 3;
    uint64_t result = this->regs[reg_num];

    if (offset || size != 4) { // slow path
        if ((offset + size) > 4) {
            result |= (uint64_t)(this->regs[reg_num + 1]) << 32;
        }
        result = extract_bits<uint64_t>(result, offset * 8, size * 8);
    }

    return static_cast<uint32_t>(result);
}

void AtiMach64Gx::write_reg(uint32_t reg_offset, uint32_t value, uint32_t size)
{
    uint32_t reg_num = reg_offset >> 2;
    uint32_t offset = reg_offset & 3;
    uint32_t old_value = this->regs[reg_num];
    uint32_t new_value;

    if (offset || size != 4) { // slow path
        if ((offset + size) > 4) {
            ABORT_F("%s: unaligned DWORD writes not implemented", this->name.c_str());
        }
        uint64_t val = old_value;
        insert_bits<uint64_t>(val, value, offset * 8, size * 8);
        value = static_cast<uint32_t>(val);
    }

    switch (reg_num) {
    case ATI_CRTC_OFF_PITCH:
        new_value = value;
        this->fb_pitch = extract_bits<uint32_t>(new_value, ATI_CRTC_PITCH, ATI_CRTC_PITCH_size) * 8;
        this->fb_ptr = &this->vram_ptr[extract_bits<uint32_t>(new_value, ATI_CRTC_OFFSET, ATI_CRTC_OFFSET_size) * 8];
        break;
    case ATI_CRTC_GEN_CNTL:
    {
        new_value = value;
        if (bit_changed(old_value, new_value, ATI_CRTC_DISPLAY_DIS)) {
            if (bit_set(new_value, ATI_CRTC_DISPLAY_DIS)) {
                this->blank_on = true;
                this->blank_display();
            } else {
                this->blank_on = false;
            }
        }

        if (bit_changed(old_value, new_value, ATI_CRTC_ENABLE)) {
            if (!bit_set(new_value, ATI_CRTC_ENABLE)) {
                this->blank_on = true;
                this->blank_display();
            } else {
                this->blank_on = false;
            }
        }
        break;
    }
    case ATI_DAC_REGS:
        new_value = old_value; // no change
        if (size == 1) { // only byte accesses are allowed for DAC registers
            int dac_reg_addr = ((this->regs[ATI_DAC_CNTL] & 1) << 2) | offset;
            rgb514_write_reg(dac_reg_addr, extract_bits<uint32_t>(value, offset * 8, 8));
        }
        break;
    case ATI_DAC_CNTL:
        new_value = value;
        // monitor ID is usually accessed using 8bit writes
        if (offset <= 3 && offset + size > 3) {
            uint8_t gpio_dirs   = extract_bits<uint32_t>(new_value, ATI_DAC_GIO_DIR, ATI_DAC_GIO_DIR_size);
            uint8_t gpio_levels = extract_bits<uint32_t>(new_value, ATI_DAC_GIO_STATE, ATI_DAC_GIO_STATE_size);
            gpio_levels = this->disp_id->read_monitor_sense(gpio_levels, gpio_dirs);
            insert_bits<uint32_t>(new_value, gpio_levels, ATI_DAC_GIO_STATE, ATI_DAC_GIO_STATE_size);
        }
        break;
    case ATI_CONFIG_STAT0:
        new_value = old_value; // prevent writes to this read-only register
        break;
    default:
        new_value = value;
        break;
    }

    this->regs[reg_num] = new_value;
}

uint32_t AtiMach64Gx::read(uint32_t rgn_start, uint32_t offset, int size)
{
    if (rgn_start == this->aperture_base[0]) {
        if (offset < this->vram_size) {
            return read_mem(&this->vram_ptr[offset], size);
        }
        if (offset >= this->mm_regs_offset) {
            return BYTESWAP_SIZED(read_reg(offset - this->mm_regs_offset, size), size);
        }
        return 0;
    }

    // memory mapped expansion ROM region
    if (rgn_start == this->exp_rom_addr) {
        if (offset < this->exp_rom_size)
            return read_mem(&this->exp_rom_data[offset], size);
        return 0;
    }

    return 0;
}

void AtiMach64Gx::write(uint32_t rgn_start, uint32_t offset, uint32_t value, int size)
{
    if (rgn_start == this->aperture_base[0]) {
        if (offset < this->vram_size) {
            return write_mem(&this->vram_ptr[offset], value, size);
        }
        if (offset >= this->mm_regs_offset) {
            return write_reg(offset - this->mm_regs_offset, BYTESWAP_SIZED(value, size), size);
        }
        return;
    }
}

void AtiMach64Gx::verbose_pixel_format(int crtc_index) {
    if (crtc_index) {
        LOG_F(ERROR, "CRTC2 not supported yet");
        return;
    }

/*
    int fmt = extract_bits<uint32_t>(this->regs[ATI_CRTC_GEN_CNTL], ATI_CRTC_PIX_WIDTH, ATI_CRTC_PIX_WIDTH_size);
*/
    int pix_fmt = this->pixel_format;

    const char* what = "Pixel format:";

    switch (pix_fmt) {
    case 2:
        LOG_F(INFO, "%s 4 bpp with DAC palette", what);
        break;
    case 3:
        LOG_F(INFO, "%s 8 bpp with DAC palette", what);
        break;
    case 4:
        LOG_F(INFO, "%s 15 bpp direct color (RGB555)", what);
        break;
    case 5:
        LOG_F(INFO, "%s 24 bpp direct color (RGB888)", what);
        break;
    case 6:
        LOG_F(INFO, "%s 32 bpp direct color (ARGB8888)", what);
        break;
    default:
        LOG_F(ERROR, "%s: CRTC pixel format %d not supported", this->name.c_str(), pix_fmt);
    }
}

void AtiMach64Gx::crtc_update()
{
    uint32_t new_width, new_height;

    // check for unsupported modes and fail early
    if (!bit_set(this->regs[ATI_CRTC_GEN_CNTL], ATI_CRTC_EXT_DISP_EN))
        ABORT_F("%s: VGA not supported", this->name.c_str());

    new_width  = (extract_bits<uint32_t>(this->regs[ATI_CRTC_H_TOTAL_DISP], ATI_CRTC_H_DISP, ATI_CRTC_H_DISP_size) + 1) * 8;
    new_height =  extract_bits<uint32_t>(this->regs[ATI_CRTC_V_TOTAL_DISP], ATI_CRTC_V_DISP, ATI_CRTC_V_DISP_size) + 1;

    if (new_width != this->active_width || new_height != this->active_height) {
        this->create_display_window(new_width, new_height);
    }

    // calculate display refresh rate
    this->hori_total = (extract_bits<uint32_t>(this->regs[ATI_CRTC_H_TOTAL_DISP], ATI_CRTC_H_TOTAL, ATI_CRTC_H_TOTAL_size) + 1) * 8;
    this->vert_total = extract_bits<uint32_t>(this->regs[ATI_CRTC_V_TOTAL_DISP], ATI_CRTC_V_TOTAL, ATI_CRTC_V_TOTAL_size) + 1;

    this->refresh_rate = this->pixel_clock / this->hori_total / this->vert_total;

    // specify framebuffer converter
    switch (this->pixel_depth) {
    case 8:
        this->convert_fb_cb = [this](uint8_t *dst_buf, int dst_pitch) {
            this->convert_frame_8bpp_indexed(dst_buf, dst_pitch);
        };
        break;
    default:
        ABORT_F("%s: unsupported pixel depth %d", this->name.c_str(), this->pixel_depth);
    }

    this->stop_refresh_task();
    this->start_refresh_task();

    this->crtc_on = true;
}

// ========================== IBM RGB514 related code ==========================
void AtiMach64Gx::rgb514_write_reg(uint8_t reg_addr, uint8_t value)
{
    switch (reg_addr) {
    case Rgb514::CLUT_ADDR_WR:
        this->clut_index = value;
        break;
    case Rgb514::CLUT_DATA:
        this->clut_color[this->comp_index++] = value;
        if (this->comp_index >= 3) {
            this->set_palette_color(this->clut_index, clut_color[0],
                                    clut_color[1], clut_color[2], 0xFF);
            this->clut_index++;
            this->comp_index = 0;
        }
        break;
    case Rgb514::CLUT_MASK:
        if (value != 0xFF) {
            LOG_F(WARNING, "RGB514: pixel mask set to 0x%X", value);
        }
        break;
    case Rgb514::INDEX_LOW:
        this->dac_idx_lo = value;
        break;
    case Rgb514::INDEX_HIGH:
        this->dac_idx_hi = value;
        break;
    case Rgb514::INDEX_DATA:
        this->rgb514_write_ind_reg((this->dac_idx_hi << 8) + this->dac_idx_lo, value);
        break;
    default:
        ABORT_F("RGB514: access to unimplemented register at 0x%X", reg_addr);
    }
}

const char* AtiMach64Gx::rgb514_get_reg_name(uint32_t reg_addr)
{
    auto iter = rgb514_reg_names.find(reg_addr);
    if (iter != rgb514_reg_names.end()) {
        return iter->second.c_str();
    } else {
        return "unknown rgb514 register";
    }
}

void AtiMach64Gx::rgb514_write_ind_reg(uint8_t reg_addr, uint8_t value)
{
    this->dac_regs[reg_addr] = value;

    switch (reg_addr) {
    case Rgb514::MISC_CLK_CNTL:
        if (value & PLL_ENAB) {
            if ((this->dac_regs[Rgb514::PLL_CTL_1] & 3) != 1)
                ABORT_F("RGB514: unsupported PLL source");

            int m = 8 >> (this->dac_regs[Rgb514::F0_M0] >> 6);
            int vco_div = (this->dac_regs[Rgb514::F0_M0] & 0x3F) + 65;
            int ref_div = (this->dac_regs[Rgb514::F1_N0] & 0x1F) * m;
            this->pixel_clock = ATI_XTAL * vco_div / ref_div;
            LOG_F(INFO, "RGB514: dot clock set to %f Hz", this->pixel_clock);
        }
        break;
    case Rgb514::PIX_FORMAT:
        if (value == 3) {
            this->pixel_depth = 8;
            // HACK: not the best place for enabling display output!
            this->crtc_update();
        } else {
            ABORT_F("RGB514: unimplemented pixel format %d", value);
        }
        break;
    }
}

static const DeviceDescription AtiMach64Gx_Descriptor = {
    AtiMach64Gx::create, {}, {}
};

REGISTER_DEVICE(AtiMach64Gx, AtiMach64Gx_Descriptor);
