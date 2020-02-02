/*
 * (C) notaz, 2012,2014,2015
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1 // strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __MACH__
#include <unistd.h>
#include <sys/syscall.h>
#endif

#ifdef SWITCH
#include <switch.h>
#endif

#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/psxcounters.h"
#include "../libpcsxcore/psxmem_map.h"
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
#include "../libpcsxcore/cdrom.h"
#include "../libpcsxcore/cdriso.h"
#include "../libpcsxcore/cheat.h"
#include "../libpcsxcore/r3000a.h"
#include "../plugins/dfsound/out.h"
#include "../plugins/dfsound/spu_config.h"
#include "../plugins/dfinput/externals.h"
#include "cspace.h"
#include "main.h"
#include "menu.h"
#include "plugin.h"
#include "plugin_lib.h"
#include "arm_features.h"
#include "revision.h"

#include <libretro.h>
#include "libretro_core_options.h"

#ifdef _3DS
#include "3ds/3ds_utils.h"
#endif

#define PORTS_NUMBER 8

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define ISHEXDEC ((buf[cursor]>='0') && (buf[cursor]<='9')) || ((buf[cursor]>='a') && (buf[cursor]<='f')) || ((buf[cursor]>='A') && (buf[cursor]<='F'))

#define INTERNAL_FPS_SAMPLE_PERIOD 64

//hack to prevent retroarch freezing when reseting in the menu but not while running with the hot key
static int rebootemu = 0;

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_set_rumble_state_t rumble_cb;
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

static void *vout_buf;
static void * vout_buf_ptr;
static int vout_width, vout_height;
static int vout_doffs_old, vout_fb_dirty;
static bool vout_can_dupe;
static bool duping_enable;
static bool found_bios;
static bool display_internal_fps = false;
static unsigned frame_count = 0;
static bool libretro_supports_bitmasks = false;
static int show_advanced_gpu_peops_settings = -1;
static int show_advanced_gpu_unai_settings  = -1;

static unsigned previous_width = 0;
static unsigned previous_height = 0;

static int plugins_opened;
static int is_pal_mode;

/* memory card data */
extern char Mcd1Data[MCD_SIZE];
extern char Mcd2Data[MCD_SIZE];
extern char McdDisable[2];

/* PCSX ReARMed core calls and stuff */
int in_type[8] =  { PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE };
int in_analog_left[8][2] = {{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 }};
int in_analog_right[8][2] = {{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 }};
unsigned short in_keystate[PORTS_NUMBER];
int multitap1 = 0;
int multitap2 = 0;
int in_enable_vibration = 1;

// NegCon adjustment parameters
// > The NegCon 'twist' action is somewhat awkward when mapped
//   to a standard analog stick -> user should be able to tweak
//   response/deadzone for comfort
// > When response is linear, 'additional' deadzone (set here)
//   may be left at zero, since this is normally handled via in-game
//   options menus
// > When response is non-linear, deadzone should be set to match the
//   controller being used (otherwise precision may be lost)
// > negcon_linearity:
//   - 1: Response is linear - recommended when using racing wheel
//        peripherals, not recommended for standard gamepads
//   - 2: Response is quadratic - optimal setting for gamepads
//   - 3: Response is cubic - enables precise fine control, but
//        difficult to use...
#define NEGCON_RANGE 0x7FFF
static int negcon_deadzone = 0;
static int negcon_linearity = 1;

static bool axis_bounds_modifier;

/* PSX max resolution is 640x512, but with enhancement it's 1024x512 */
#define VOUT_MAX_WIDTH 1024
#define VOUT_MAX_HEIGHT 512

//Dummy functions
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info){return false;}
void retro_unload_game(void){}
static int vout_open(void){return 0;}
static void vout_close(void){}
static int snd_init(void){return 0;}
static void snd_finish(void){}
static int snd_busy(void){return 0;}

#define GPU_PEOPS_ODD_EVEN_BIT         (1 << 0)
#define GPU_PEOPS_EXPAND_SCREEN_WIDTH  (1 << 1)
#define GPU_PEOPS_IGNORE_BRIGHTNESS    (1 << 2)
#define GPU_PEOPS_DISABLE_COORD_CHECK  (1 << 3)
#define GPU_PEOPS_LAZY_SCREEN_UPDATE   (1 << 6)
#define GPU_PEOPS_OLD_FRAME_SKIP       (1 << 7)
#define GPU_PEOPS_REPEATED_TRIANGLES   (1 << 8)
#define GPU_PEOPS_QUADS_WITH_TRIANGLES (1 << 9)
#define GPU_PEOPS_FAKE_BUSY_STATE      (1 << 10)

static void init_memcard(char *mcd_data)
{
	unsigned off = 0;
	unsigned i;

	memset(mcd_data, 0, MCD_SIZE);

	mcd_data[off++] = 'M';
	mcd_data[off++] = 'C';
	off += 0x7d;
	mcd_data[off++] = 0x0e;

	for (i = 0; i < 15; i++) {
		mcd_data[off++] = 0xa0;
		off += 0x07;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x75;
		mcd_data[off++] = 0xa0;
	}

	for (i = 0; i < 20; i++) {
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x04;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x76;
	}
}

static void set_vout_fb()
{
  struct retro_framebuffer fb = {0};

  fb.width           = vout_width;
  fb.height          = vout_height;
  fb.access_flags    = RETRO_MEMORY_ACCESS_WRITE;

  if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) && fb.format == RETRO_PIXEL_FORMAT_RGB565)
     vout_buf_ptr = (uint16_t*)fb.data;
  else
     vout_buf_ptr = vout_buf;
}

static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
  vout_width = w;
  vout_height = h;

	if (previous_width != vout_width || previous_height != vout_height)
	{
		previous_width = vout_width;
		previous_height = vout_height;

	struct retro_system_av_info info;
	retro_get_system_av_info(&info);
	environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
	}

  set_vout_fb();
}

#ifndef FRONTEND_SUPPORTS_RGB565
static void convert(void *buf, size_t bytes)
{
	unsigned int i, v, *p = buf;

	for (i = 0; i < bytes / 4; i++) {
		v = p[i];
		p[i] = (v & 0x001f001f) | ((v >> 1) & 0x7fe07fe0);
	}
}
#endif

static void vout_flip(const void *vram, int stride, int bgr24, int w, int h)
{
	unsigned short *dest = vout_buf_ptr;
	const unsigned short *src = vram;
	int dstride = vout_width, h1 = h;
	int doffs;

	if (vram == NULL) {
		// blanking
		memset(vout_buf_ptr, 0, dstride * h * 2);
		goto out;
	}

	doffs = (vout_height - h) * dstride;
	doffs += (dstride - w) / 2 & ~1;
	if (doffs != vout_doffs_old) {
		// clear borders
		memset(vout_buf_ptr, 0, dstride * h * 2);
		vout_doffs_old = doffs;
	}
	dest += doffs;

	if (bgr24)
	{
		// XXX: could we switch to RETRO_PIXEL_FORMAT_XRGB8888 here?
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr888_to_rgb565(dest, src, w * 3);
		}
	}
	else
	{
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr555_to_rgb565(dest, src, w * 2);
		}
	}

out:
#ifndef FRONTEND_SUPPORTS_RGB565
	convert(vout_buf_ptr, vout_width * vout_height * 2);
#endif
	vout_fb_dirty = 1;
	pl_rearmed_cbs.flip_cnt++;
}

#ifdef _3DS
typedef struct
{
   void* buffer;
   uint32_t target_map;
   size_t size;
   enum psxMapTag tag;
}psx_map_t;

psx_map_t custom_psx_maps[] = {
   {NULL, 0x13000000, 0x210000, MAP_TAG_RAM},   // 0x80000000
   {NULL, 0x12800000, 0x010000, MAP_TAG_OTHER}, // 0x1f800000
   {NULL, 0x12c00000, 0x080000, MAP_TAG_OTHER}, // 0x1fc00000
   {NULL, 0x11000000, 0x800000, MAP_TAG_LUTS},  // 0x08000000
   {NULL, 0x12000000, 0x200000, MAP_TAG_VRAM},  // 0x00000000
};

void* pl_3ds_mmap(unsigned long addr, size_t size, int is_fixed,
	enum psxMapTag tag)
{
   (void)is_fixed;
   (void)addr;

   if (__ctr_svchax)
   {
      psx_map_t* custom_map = custom_psx_maps;

      for (; custom_map->size; custom_map++)
      {
         if ((custom_map->size == size) && (custom_map->tag == tag))
         {
            uint32_t ptr_aligned, tmp;

            custom_map->buffer = malloc(size + 0x1000);
            ptr_aligned = (((u32)custom_map->buffer) + 0xFFF) & ~0xFFF;

            if(svcControlMemory(&tmp, (void*)custom_map->target_map, (void*)ptr_aligned, size, MEMOP_MAP, 0x3) < 0)
            {
               SysPrintf("could not map memory @0x%08X\n", custom_map->target_map);
               exit(1);
            }

            return (void*)custom_map->target_map;
         }
      }
   }

   return malloc(size);
}

void pl_3ds_munmap(void *ptr, size_t size, enum psxMapTag tag)
{
   (void)tag;

   if (__ctr_svchax)
   {
      psx_map_t* custom_map = custom_psx_maps;

      for (; custom_map->size; custom_map++)
      {
         if ((custom_map->target_map == (uint32_t)ptr))
         {
            uint32_t ptr_aligned, tmp;

            ptr_aligned = (((u32)custom_map->buffer) + 0xFFF) & ~0xFFF;

            svcControlMemory(&tmp, (void*)custom_map->target_map, (void*)ptr_aligned, size, MEMOP_UNMAP, 0x3);

            free(custom_map->buffer);
            custom_map->buffer = NULL;
            return;
         }
      }
   }

   free(ptr);
}
#endif

#ifdef VITA
typedef struct
{
   void* buffer;
   uint32_t target_map;
   size_t size;
   enum psxMapTag tag;
}psx_map_t;

void* addr = NULL;

psx_map_t custom_psx_maps[] = {
   {NULL, NULL, 0x210000, MAP_TAG_RAM},   // 0x80000000
   {NULL, NULL, 0x010000, MAP_TAG_OTHER}, // 0x1f800000
   {NULL, NULL, 0x080000, MAP_TAG_OTHER}, // 0x1fc00000
   {NULL, NULL, 0x800000, MAP_TAG_LUTS},  // 0x08000000
   {NULL, NULL, 0x200000, MAP_TAG_VRAM},  // 0x00000000
};

int init_vita_mmap(){
  int n;
  void * tmpaddr;
  addr = malloc(64*1024*1024);
  if(addr==NULL)
    return -1;
  tmpaddr = ((u32)(addr+0xFFFFFF))&~0xFFFFFF;
  custom_psx_maps[0].buffer=tmpaddr+0x2000000;
  custom_psx_maps[1].buffer=tmpaddr+0x1800000;
  custom_psx_maps[2].buffer=tmpaddr+0x1c00000;
  custom_psx_maps[3].buffer=tmpaddr+0x0000000;
  custom_psx_maps[4].buffer=tmpaddr+0x1000000;
#if 0
  for(n = 0; n < 5; n++){
    sceClibPrintf("addr reserved %x\n",custom_psx_maps[n].buffer);
  }
#endif
  return 0;
}

void deinit_vita_mmap(){
  free(addr);
}

void* pl_vita_mmap(unsigned long addr, size_t size, int is_fixed,
	enum psxMapTag tag)
{
   (void)is_fixed;
   (void)addr;


    psx_map_t* custom_map = custom_psx_maps;

    for (; custom_map->size; custom_map++)
    {
       if ((custom_map->size == size) && (custom_map->tag == tag))
       {
          return custom_map->buffer;
       }
    }


   return malloc(size);
}

void pl_vita_munmap(void *ptr, size_t size, enum psxMapTag tag)
{
   (void)tag;

   psx_map_t* custom_map = custom_psx_maps;

  for (; custom_map->size; custom_map++)
  {
     if ((custom_map->buffer == ptr))
     {
        return;
     }
  }

   free(ptr);
}
#endif

static void *pl_mmap(unsigned int size)
{
	return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void pl_munmap(void *ptr, unsigned int size)
{
	psxUnmap(ptr, size, MAP_TAG_VRAM);
}

struct rearmed_cbs pl_rearmed_cbs = {
	.pl_vout_open = vout_open,
	.pl_vout_set_mode = vout_set_mode,
	.pl_vout_flip = vout_flip,
	.pl_vout_close = vout_close,
	.mmap = pl_mmap,
	.munmap = pl_munmap,
	/* from psxcounters */
	.gpu_hcnt = &hSyncCount,
	.gpu_frame_count = &frame_counter,
};

void pl_frame_limit(void)
{
	/* called once per frame, make psxCpu->Execute() above return */
	stop = 1;
}

void pl_timing_prepare(int is_pal)
{
	is_pal_mode = is_pal;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
	if (!rumble_cb)
		return;

	if (in_enable_vibration)
	{
		rumble_cb(pad, RETRO_RUMBLE_STRONG, high << 8);
		rumble_cb(pad, RETRO_RUMBLE_WEAK, low ? 0xffff : 0x0);
    }
}

void pl_update_gun(int *xn, int *yn, int *xres, int *yres, int *in)
{
}

/* sound calls */
static void snd_feed(void *buf, int bytes)
{
	if (audio_batch_cb != NULL)
		audio_batch_cb(buf, bytes / 4);
}

void out_register_libretro(struct out_driver *drv)
{
	drv->name = "libretro";
	drv->init = snd_init;
	drv->finish = snd_finish;
	drv->busy = snd_busy;
	drv->feed = snd_feed;
}

/* libretro */
void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;

   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

static int controller_port_variable(unsigned port, struct retro_variable *var)
{
	if (port >= PORTS_NUMBER)
		return 0;

	if (!environ_cb)
		return 0;

	var->value = NULL;
	switch (port) {
	case 0:
		var->key = "pcsx_rearmed_pad1type";
		break;
	case 1:
		var->key = "pcsx_rearmed_pad2type";
		break;
	case 2:
		var->key = "pcsx_rearmed_pad3type";
		break;
	case 3:
		var->key = "pcsx_rearmed_pad4type";
		break;
	case 4:
		var->key = "pcsx_rearmed_pad5type";
		break;
	case 5:
		var->key = "pcsx_rearmed_pad6type";
		break;
	case 6:
		var->key = "pcsx_rearmed_pad7type";
		break;
	case 7:
		var->key = "pcsx_rearmed_pad8type";
		break;
	}

	return environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, var) || var->value;
}

static void update_controller_port_variable(unsigned port)
{
	if (port >= PORTS_NUMBER)
		return;

	struct retro_variable var;

	if (controller_port_variable(port, &var))
	{
		if (strcmp(var.value, "standard") == 0)
			in_type[port] = PSE_PAD_TYPE_STANDARD;
		else if (strcmp(var.value, "analog") == 0)
			in_type[port] = PSE_PAD_TYPE_ANALOGJOY;
		else if (strcmp(var.value, "dualshock") == 0)
			in_type[port] = PSE_PAD_TYPE_ANALOGPAD;
		else if (strcmp(var.value, "negcon") == 0)
			in_type[port] = PSE_PAD_TYPE_NEGCON;
		else if (strcmp(var.value, "guncon") == 0)
			in_type[port] = PSE_PAD_TYPE_GUNCON;
		else if (strcmp(var.value, "none") == 0)
			in_type[port] = PSE_PAD_TYPE_NONE;
		// else 'default' case, do nothing
	}
}

static void update_controller_port_device(unsigned port, unsigned device)
{
	if (port >= PORTS_NUMBER)
		return;

	struct retro_variable var;

	if (!controller_port_variable(port, &var))
		return;

	if (strcmp(var.value, "default") != 0)
		return;

	switch (device)
	{
	case RETRO_DEVICE_JOYPAD:
		in_type[port] = PSE_PAD_TYPE_STANDARD;
		break;
	case RETRO_DEVICE_ANALOG:
		in_type[port] = PSE_PAD_TYPE_ANALOGPAD;
		break;
	case RETRO_DEVICE_MOUSE:
		in_type[port] = PSE_PAD_TYPE_MOUSE;
		break;
	case RETRO_DEVICE_LIGHTGUN:
		in_type[port] = PSE_PAD_TYPE_GUN;
		break;
	case RETRO_DEVICE_NONE:
	default:
		in_type[port] = PSE_PAD_TYPE_NONE;
	}
}

static void update_multitap()
{
	struct retro_variable var;
	int auto_case, port;

	var.value = NULL;
	var.key = "pcsx_rearmed_multitap1";
	auto_case = 0;
	if (environ_cb && (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value))
	{
		if (strcmp(var.value, "enabled") == 0)
			multitap1 = 1;
		else if (strcmp(var.value, "disabled") == 0)
			multitap1 = 0;
		else // 'auto' case
			auto_case = 1;
	}
	else
		auto_case = 1;

	if (auto_case)
	{
		// If a gamepad is plugged after port 2, we need a first multitap.
		multitap1 = 0;
		for (port = 2; port < PORTS_NUMBER; port++)
			multitap1 |= in_type[port] != PSE_PAD_TYPE_NONE;
	}

	var.value = NULL;
	var.key = "pcsx_rearmed_multitap2";
	auto_case = 0;
	if (environ_cb && (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value))
	{
		if (strcmp(var.value, "enabled") == 0)
			multitap2 = 1;
		else if (strcmp(var.value, "disabled") == 0)
			multitap2 = 0;
		else // 'auto' case
			auto_case = 1;
	}
	else
		auto_case = 1;

	if (auto_case)
	{
		// If a gamepad is plugged after port 4, we need a second multitap.
		multitap2 = 0;
		for (port = 4; port < PORTS_NUMBER; port++)
			multitap2 |= in_type[port] != PSE_PAD_TYPE_NONE;
	}
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	SysPrintf("port %u  device %u",port,device);

	if (port >= PORTS_NUMBER)
		return;

	update_controller_port_device(port, device);
	update_multitap();
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "PCSX-ReARMed";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = "r22" GIT_VERSION;
	info->valid_extensions = "bin|cue|img|mdf|pbp|toc|cbn|m3u|chd";
	info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{

	unsigned geom_height = vout_height > 0 ? vout_height : 240;
	unsigned geom_width = vout_width > 0 ? vout_width : 320;

	memset(info, 0, sizeof(*info));
	info->timing.fps            = is_pal_mode ? 50 : 60;
	info->timing.sample_rate    = 44100;
	info->geometry.base_width   = geom_width;
	info->geometry.base_height  = geom_height;
	info->geometry.max_width    = VOUT_MAX_WIDTH;
	info->geometry.max_height   = VOUT_MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

/* savestates */
size_t retro_serialize_size(void)
{
	// it's currently 4380651-4397047 bytes,
	// but have some reserved for future
	return 0x440000;
}

struct save_fp {
	char *buf;
	size_t pos;
	int is_write;
};

static void *save_open(const char *name, const char *mode)
{
	struct save_fp *fp;

	if (name == NULL || mode == NULL)
		return NULL;

	fp = malloc(sizeof(*fp));
	if (fp == NULL)
		return NULL;

	fp->buf = (char *)name;
	fp->pos = 0;
	fp->is_write = (mode[0] == 'w' || mode[1] == 'w');

	return fp;
}

static int save_read(void *file, void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(buf, fp->buf + fp->pos, len);
	fp->pos += len;
	return len;
}

static int save_write(void *file, const void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(fp->buf + fp->pos, buf, len);
	fp->pos += len;
	return len;
}

static long save_seek(void *file, long offs, int whence)
{
	struct save_fp *fp = file;
	if (fp == NULL)
		return -1;

	switch (whence) {
	case SEEK_CUR:
		fp->pos += offs;
		return fp->pos;
	case SEEK_SET:
		fp->pos = offs;
		return fp->pos;
	default:
		return -1;
	}
}

static void save_close(void *file)
{
	struct save_fp *fp = file;
	size_t r_size = retro_serialize_size();
	if (fp == NULL)
		return;

	if (fp->pos > r_size)
		SysPrintf("ERROR: save buffer overflow detected\n");
	else if (fp->is_write && fp->pos < r_size)
		// make sure we don't save trash in leftover space
		memset(fp->buf + fp->pos, 0, r_size - fp->pos);
	free(fp);
}

bool retro_serialize(void *data, size_t size)
{
	int ret = SaveState(data);
	return ret == 0 ? true : false;
}

bool retro_unserialize(const void *data, size_t size)
{
	int ret = LoadState(data);
	return ret == 0 ? true : false;
}

/* cheats */
void retro_cheat_reset(void)
{
	ClearAllCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	char buf[256];
	int ret;

	// cheat funcs are destructive, need a copy..
	strncpy(buf, code, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;

	//Prepare buffered cheat for PCSX's AddCheat fucntion.
	int cursor=0;
	int nonhexdec=0;
	while (buf[cursor]){
		if (!(ISHEXDEC)){
			if (++nonhexdec%2){
				buf[cursor]=' ';
			} else {
				buf[cursor]='\n';
			}
		}
		cursor++;
	}


	if (index < NumCheats)
		ret = EditCheat(index, "", buf);
	else
		ret = AddCheat("", buf);

	if (ret != 0)
		SysPrintf("Failed to set cheat %#u\n", index);
	else if (index < NumCheats)
		Cheats[index].Enabled = enabled;
}

// just in case, maybe a win-rt port in the future?
#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

#ifndef PATH_MAX
#define PATH_MAX  4096
#endif

/* multidisk support */
static unsigned int disk_initial_index;
static char disk_initial_path[PATH_MAX];
static bool disk_ejected;
static unsigned int disk_current_index;
static unsigned int disk_count;
static struct disks_state {
	char *fname;
	char *flabel;
	int internal_index; // for multidisk eboots
} disks[8];

static void get_disk_label(char *disk_label, const char *disk_path, size_t len)
{
	const char *base = NULL;

	if (!disk_path || (*disk_path == '\0'))
		return;

	base = strrchr(disk_path, SLASH);
	if (!base)
		base = disk_path;

	if (*base == SLASH)
		base++;

	strncpy(disk_label, base, len - 1);
	disk_label[len - 1] = '\0';

	char *ext = strrchr(disk_label, '.');
	if (ext)
		*ext = '\0';
}

static void disk_init(void)
{
	size_t i;

	disk_ejected       = false;
	disk_current_index = 0;
	disk_count         = 0;

	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		if (disks[i].fname != NULL) {
			free(disks[i].fname);
			disks[i].fname = NULL;
		}
		if (disks[i].flabel != NULL) {
			free(disks[i].flabel);
			disks[i].flabel = NULL;
		}
		disks[i].internal_index = 0;
	}
}

static bool disk_set_eject_state(bool ejected)
{
	// weird PCSX API..
	SetCdOpenCaseTime(ejected ? -1 : (time(NULL) + 2));
	LidInterrupt();

	disk_ejected = ejected;
	return true;
}

static bool disk_get_eject_state(void)
{
	/* can't be controlled by emulated software */
	return disk_ejected;
}

static unsigned int disk_get_image_index(void)
{
	return disk_current_index;
}

static bool disk_set_image_index(unsigned int index)
{
	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	CdromId[0] = '\0';
	CdromLabel[0] = '\0';

	if (disks[index].fname == NULL) {
		SysPrintf("missing disk #%u\n", index);
		CDR_shutdown();

		// RetroArch specifies "no disk" with index == count,
		// so don't fail here..
		disk_current_index = index;
		return true;
	}

	SysPrintf("switching to disk %u: \"%s\" #%d\n", index,
		disks[index].fname, disks[index].internal_index);

	cdrIsoMultidiskSelect = disks[index].internal_index;
	set_cd_image(disks[index].fname);
	if (ReloadCdromPlugin() < 0) {
		SysPrintf("failed to load cdr plugin\n");
		return false;
	}
	if (CDR_open() < 0) {
		SysPrintf("failed to open cdr plugin\n");
		return false;
	}

	if (!disk_ejected) {
		SetCdOpenCaseTime(time(NULL) + 2);
		LidInterrupt();
	}

	disk_current_index = index;
	return true;
}

static unsigned int disk_get_num_images(void)
{
	return disk_count;
}

static bool disk_replace_image_index(unsigned index,
	const struct retro_game_info *info)
{
	char *old_fname  = NULL;
	char *old_flabel = NULL;
	bool ret         = true;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	old_fname  = disks[index].fname;
	old_flabel = disks[index].flabel;

	disks[index].fname          = NULL;
	disks[index].flabel         = NULL;
	disks[index].internal_index = 0;

	if (info != NULL) {
		char disk_label[PATH_MAX];
		disk_label[0] = '\0';

		disks[index].fname = strdup(info->path);

		get_disk_label(disk_label, info->path, PATH_MAX);
		disks[index].flabel = strdup(disk_label);

		if (index == disk_current_index)
			ret = disk_set_image_index(index);
	}

	if (old_fname != NULL)
		free(old_fname);

	if (old_flabel != NULL)
		free(old_flabel);

	return ret;
}

static bool disk_add_image_index(void)
{
	if (disk_count >= 8)
		return false;

	disk_count++;
	return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	if (!path || (*path == '\0'))
		return false;

	disk_initial_index = index;

	strncpy(disk_initial_path, path, sizeof(disk_initial_path) - 1);
	disk_initial_path[sizeof(disk_initial_path) - 1] = '\0';

	return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
	const char *fname = NULL;

	if (len < 1)
		return false;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	fname = disks[index].fname;

	if (!fname || (*fname == '\0'))
		return false;

	strncpy(path, fname, len - 1);
	path[len - 1] = '\0';

	return true;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
	const char *flabel = NULL;

	if (len < 1)
		return false;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	flabel = disks[index].flabel;

	if (!flabel || (*flabel == '\0'))
		return false;

	strncpy(label, flabel, len - 1);
	label[len - 1] = '\0';

	return true;
}

static struct retro_disk_control_callback disk_control = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
};

static struct retro_disk_control_ext_callback disk_control_ext = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
	.set_initial_image = disk_set_initial_image,
	.get_image_path = disk_get_image_path,
	.get_image_label = disk_get_image_label,
};

static char base_dir[1024];

static bool read_m3u(const char *file)
{
	char line[1024];
	char name[PATH_MAX];
	FILE *f = fopen(file, "r");
	if (!f)
		return false;

	while (fgets(line, sizeof(line), f) && disk_count < sizeof(disks) / sizeof(disks[0])) {
		if (line[0] == '#')
			continue;
		char *carrige_return = strchr(line, '\r');
		if (carrige_return)
			*carrige_return = '\0';
		char *newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		if (line[0] != '\0')
		{
			char disk_label[PATH_MAX];
			disk_label[0] = '\0';

			snprintf(name, sizeof(name), "%s%c%s", base_dir, SLASH, line);
			disks[disk_count].fname = strdup(name);

			get_disk_label(disk_label, name, PATH_MAX);
			disks[disk_count].flabel = strdup(disk_label);

			disk_count++;
		}
	}

	fclose(f);
	return (disk_count != 0);
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

#if defined(__QNX__) || defined(_WIN32)
/* Blackberry QNX doesn't have strcasestr */

/*
 * Find the first occurrence of find in s, ignore case.
 */
char *
strcasestr(const char *s, const char*find)
{
	char c, sc;
	size_t len;

	if ((c = *find++) != 0) {
		c = tolower((unsigned char)c);
		len = strlen(find);
		do {
			do {
				if ((sc = *s++) == 0)
					return (NULL);
			} while ((char)tolower((unsigned char)sc) != c);
		} while (strncasecmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}
#endif

static void set_retro_memmap(void)
{
	struct retro_memory_map retromap = { 0 };
	struct retro_memory_descriptor mmap =
	{
		0, psxM, 0, 0, 0, 0, 0x200000
	};

	retromap.descriptors = &mmap;
	retromap.num_descriptors = 1;

    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retromap);
}

bool retro_load_game(const struct retro_game_info *info)
{
	size_t i;
	unsigned int cd_index = 0;
	bool is_m3u = (strcasestr(info->path, ".m3u") != NULL);

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },


      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 0 },
   };

	 frame_count = 0;

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

#ifdef FRONTEND_SUPPORTS_RGB565
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		SysPrintf("RGB565 supported, using it\n");
	}
#endif

	if (info == NULL || info->path == NULL) {
		SysPrintf("info->path required\n");
		return false;
	}

	if (plugins_opened) {
		ClosePlugins();
		plugins_opened = 0;
	}

	disk_init();

	extract_directory(base_dir, info->path, sizeof(base_dir));

	if (is_m3u) {
		if (!read_m3u(info->path)) {
			log_cb(RETRO_LOG_INFO, "failed to read m3u file\n");
			return false;
		}
	} else {
		char disk_label[PATH_MAX];
		disk_label[0] = '\0';

		disk_count = 1;
		disks[0].fname = strdup(info->path);

		get_disk_label(disk_label, info->path, PATH_MAX);
		disks[0].flabel = strdup(disk_label);
	}

	/* If this is an M3U file, attempt to set the
	 * initial disk image */
	if (is_m3u &&
		 (disk_initial_index > 0) &&
		 (disk_initial_index < disk_count))	{
		const char *fname = disks[disk_initial_index].fname;

		if (fname && (*fname != '\0'))
			if (strcmp(disk_initial_path, fname) == 0)
				cd_index = disk_initial_index;
	}

	set_cd_image(disks[cd_index].fname);
	disk_current_index = cd_index;

	/* have to reload after set_cd_image for correct cdr plugin */
	if (LoadPlugins() == -1) {
		log_cb(RETRO_LOG_INFO, "failed to load plugins\n");
		return false;
	}

	plugins_opened = 1;
	NetOpened = 0;

	if (OpenPlugins() == -1) {
		log_cb(RETRO_LOG_INFO, "failed to open plugins\n");
		return false;
	}

	/* Handle multi-disk images (i.e. PBP)
	 * > Cannot do this until after OpenPlugins() is
	 *   called (since this sets the value of
	 *   cdrIsoMultidiskCount) */
	if (!is_m3u && (cdrIsoMultidiskCount > 1)) {
		disk_count = cdrIsoMultidiskCount < 8 ? cdrIsoMultidiskCount : 8;

		/* Small annoyance: We need to change the label
		 * of disk 0, so have to clear existing entries */
		if (disks[0].fname != NULL)
			free(disks[0].fname);
		disks[0].fname = NULL;

		if (disks[0].flabel != NULL)
			free(disks[0].flabel);
		disks[0].flabel = NULL;

		for (i = 0; i < sizeof(disks) / sizeof(disks[0]) && i < cdrIsoMultidiskCount; i++) {
			char disk_name[PATH_MAX];
			char disk_label[PATH_MAX];
			disk_name[0]  = '\0';
			disk_label[0] = '\0';

			disks[i].fname = strdup(info->path);

			get_disk_label(disk_name, info->path, PATH_MAX);
			snprintf(disk_label, sizeof(disk_label), "%s #%u", disk_name, (unsigned)i + 1);
			disks[i].flabel = strdup(disk_label);

			disks[i].internal_index = i;
		}

		/* This is not an M3U file, so initial disk
		 * image has not yet been set - attempt to
		 * do so now */
		if ((disk_initial_index > 0) &&
			 (disk_initial_index < disk_count))	{
			const char *fname = disks[disk_initial_index].fname;

			if (fname && (*fname != '\0'))
				if (strcmp(disk_initial_path, fname) == 0)
					cd_index = disk_initial_index;
		}

		if (cd_index > 0) {
			CdromId[0]    = '\0';
			CdromLabel[0] = '\0';

			cdrIsoMultidiskSelect = disks[cd_index].internal_index;
			disk_current_index    = cd_index;
			set_cd_image(disks[cd_index].fname);

			if (ReloadCdromPlugin() < 0) {
				log_cb(RETRO_LOG_INFO, "failed to reload cdr plugins\n");
				return false;
			}
			if (CDR_open() < 0) {
				log_cb(RETRO_LOG_INFO, "failed to open cdr plugin\n");
				return false;
			}
		}
	}

	plugin_call_rearmed_cbs();
	dfinput_activate();

	if (CheckCdrom() == -1) {
        log_cb(RETRO_LOG_INFO, "unsupported/invalid CD image: %s\n", info->path);
		return false;
	}

	SysReset();

	if (LoadCdrom() == -1) {
		log_cb(RETRO_LOG_INFO, "could not load CD\n");
		return false;
	}
	emu_on_new_cd(0);

	set_retro_memmap();

	return true;
}

unsigned retro_get_region(void)
{
	return is_pal_mode ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return Mcd1Data;
	else if (id == RETRO_MEMORY_SYSTEM_RAM)
		return psxM;
	else
		return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return MCD_SIZE;
	else if (id == RETRO_MEMORY_SYSTEM_RAM)
		return 0x200000;
	else
		return 0;
}

void retro_reset(void)
{
   //hack to prevent retroarch freezing when reseting in the menu but not while running with the hot key
   rebootemu = 1;
	//SysReset();
}

static const unsigned short retro_psx_map[] = {
	[RETRO_DEVICE_ID_JOYPAD_B]	= 1 << DKEY_CROSS,
	[RETRO_DEVICE_ID_JOYPAD_Y]	= 1 << DKEY_SQUARE,
	[RETRO_DEVICE_ID_JOYPAD_SELECT]	= 1 << DKEY_SELECT,
	[RETRO_DEVICE_ID_JOYPAD_START]	= 1 << DKEY_START,
	[RETRO_DEVICE_ID_JOYPAD_UP]	= 1 << DKEY_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN]	= 1 << DKEY_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT]	= 1 << DKEY_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT]	= 1 << DKEY_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A]	= 1 << DKEY_CIRCLE,
	[RETRO_DEVICE_ID_JOYPAD_X]	= 1 << DKEY_TRIANGLE,
	[RETRO_DEVICE_ID_JOYPAD_L]	= 1 << DKEY_L1,
	[RETRO_DEVICE_ID_JOYPAD_R]	= 1 << DKEY_R1,
	[RETRO_DEVICE_ID_JOYPAD_L2]	= 1 << DKEY_L2,
	[RETRO_DEVICE_ID_JOYPAD_R2]	= 1 << DKEY_R2,
	[RETRO_DEVICE_ID_JOYPAD_L3]	= 1 << DKEY_L3,
	[RETRO_DEVICE_ID_JOYPAD_R3]	= 1 << DKEY_R3,
};
#define RETRO_PSX_MAP_LEN (sizeof(retro_psx_map) / sizeof(retro_psx_map[0]))

static void update_variables(bool in_flight)
{
   struct retro_variable var;
   int i;
   int gpu_peops_fix = 0;

   var.value = NULL;
   var.key = "pcsx_rearmed_frameskip";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
      pl_rearmed_cbs.frameskip = atoi(var.value);

   var.value = NULL;
   var.key = "pcsx_rearmed_region";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      Config.PsxAuto = 0;
      if (strcmp(var.value, "auto") == 0)
         Config.PsxAuto = 1;
      else if (strcmp(var.value, "NTSC") == 0)
         Config.PsxType = 0;
      else if (strcmp(var.value, "PAL") == 0)
         Config.PsxType = 1;
   }

   for (i = 0; i < PORTS_NUMBER; i++)
      update_controller_port_variable(i);

   update_multitap();

   var.value = NULL;
   var.key = "pcsx_rearmed_negcon_deadzone";
   negcon_deadzone = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      negcon_deadzone = (int)(atoi(var.value) * 0.01f * NEGCON_RANGE);
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_negcon_response";
   negcon_linearity = 1;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "quadratic") == 0){
         negcon_linearity = 2;
      } else if (strcmp(var.value, "cubic") == 0){
         negcon_linearity = 3;
      }
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_analog_axis_modifier";
   axis_bounds_modifier = true;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "square") == 0) {
        axis_bounds_modifier = true;
	  } else if (strcmp(var.value, "circle") == 0) {
        axis_bounds_modifier = false;
	  }
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_vibration";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         in_enable_vibration = 0;
      else if (strcmp(var.value, "enabled") == 0)
         in_enable_vibration = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_dithering";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0) {
         pl_rearmed_cbs.gpu_peops.iUseDither = 0;
         pl_rearmed_cbs.gpu_peopsgl.bDrawDither = 0;
         pl_rearmed_cbs.gpu_unai.dithering = 0;
#ifdef __ARM_NEON__
         pl_rearmed_cbs.gpu_neon.allow_dithering = 0;
#endif
      }
      else if (strcmp(var.value, "enabled") == 0) {
         pl_rearmed_cbs.gpu_peops.iUseDither = 1;
         pl_rearmed_cbs.gpu_peopsgl.bDrawDither = 1;
         pl_rearmed_cbs.gpu_unai.dithering = 1;
#ifdef __ARM_NEON__
         pl_rearmed_cbs.gpu_neon.allow_dithering = 1;
#endif
      }
   }

#ifdef __ARM_NEON__
   var.value = "NULL";
   var.key = "pcsx_rearmed_neon_interlace_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_no_main";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 1;
   }
#endif

   var.value = "NULL";
   var.key = "pcsx_rearmed_duping_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         duping_enable = false;
      else if (strcmp(var.value, "enabled") == 0)
         duping_enable = true;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_display_internal_fps";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         display_internal_fps = false;
      else if (strcmp(var.value, "enabled") == 0)
         display_internal_fps = true;
   }

#ifndef DRC_DISABLE
   var.value = NULL;
   var.key = "pcsx_rearmed_drc";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      R3000Acpu *prev_cpu = psxCpu;

#ifdef _3DS
      if(!__ctr_svchax)
         Config.Cpu = CPU_INTERPRETER;
      else
#endif
      if (strcmp(var.value, "disabled") == 0)
         Config.Cpu = CPU_INTERPRETER;
      else if (strcmp(var.value, "enabled") == 0)
         Config.Cpu = CPU_DYNAREC;

      psxCpu = (Config.Cpu == CPU_INTERPRETER) ? &psxInt : &psxRec;
      if (psxCpu != prev_cpu) {
         prev_cpu->Shutdown();
         psxCpu->Init();
         psxCpu->Reset(); // not really a reset..
      }
   }
#endif

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_reverb";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         spu_config.iUseReverb = false;
      else if (strcmp(var.value, "enabled") == 0)
         spu_config.iUseReverb = true;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_interpolation";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "simple") == 0)
         spu_config.iUseInterpolation = 1;
      else if (strcmp(var.value, "gaussian") == 0)
         spu_config.iUseInterpolation = 2;
      else if (strcmp(var.value, "cubic") == 0)
         spu_config.iUseInterpolation = 3;
      else if (strcmp(var.value, "off") == 0)
         spu_config.iUseInterpolation = 0;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_pe2_fix";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.RCntFix = 0;
      else if (strcmp(var.value, "enabled") == 0)
         Config.RCntFix = 1;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_idiablofix";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         spu_config.idiablofix = 0;
      else if (strcmp(var.value, "enabled") == 0)
         spu_config.idiablofix = 1;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_inuyasha_fix";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.VSyncWA = 0;
      else if (strcmp(var.value, "enabled") == 0)
         Config.VSyncWA = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_noxadecoding";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.Xa = 1;
      else
         Config.Xa = 0;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_nocdaudio";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.Cdda = 1;
      else
         Config.Cdda = 0;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_spuirq";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.SpuIrq = 0;
      else
         Config.SpuIrq = 1;
   }

#ifndef DRC_DISABLE
   var.value = NULL;
   var.key = "pcsx_rearmed_nosmccheck";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         new_dynarec_hacks |= NDHACK_NO_SMC_CHECK;
      else
         new_dynarec_hacks &= ~NDHACK_NO_SMC_CHECK;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_gteregsunneeded";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         new_dynarec_hacks |= NDHACK_GTE_UNNEEDED;
      else
         new_dynarec_hacks &= ~NDHACK_GTE_UNNEEDED;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_nogteflags";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         new_dynarec_hacks |= NDHACK_GTE_NO_FLAGS;
      else
         new_dynarec_hacks &= ~NDHACK_GTE_NO_FLAGS;
   }
#endif

#ifdef GPU_PEOPS
   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_odd_even_bit";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_ODD_EVEN_BIT;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_expand_screen_width";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_EXPAND_SCREEN_WIDTH;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_ignore_brightness";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_IGNORE_BRIGHTNESS;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_disable_coord_check";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_DISABLE_COORD_CHECK;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_lazy_screen_update";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_LAZY_SCREEN_UPDATE;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_old_frame_skip";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_OLD_FRAME_SKIP;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_repeated_triangles";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_REPEATED_TRIANGLES;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_quads_with_triangles";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_QUADS_WITH_TRIANGLES;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_gpu_peops_fake_busy_state";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         gpu_peops_fix |= GPU_PEOPS_FAKE_BUSY_STATE;
   }

   if (pl_rearmed_cbs.gpu_peops.dwActFixes != gpu_peops_fix)
      pl_rearmed_cbs.gpu_peops.dwActFixes = gpu_peops_fix;


   /* Show/hide core options */

   var.key = "pcsx_rearmed_show_gpu_peops_settings";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int show_advanced_gpu_peops_settings_prev = show_advanced_gpu_peops_settings;

      show_advanced_gpu_peops_settings = 1;
      if (strcmp(var.value, "disabled") == 0)
         show_advanced_gpu_peops_settings = 0;

      if (show_advanced_gpu_peops_settings != show_advanced_gpu_peops_settings_prev)
      {
         unsigned i;
         struct retro_core_option_display option_display;
         char gpu_peops_option[9][45] = {
            "pcsx_rearmed_gpu_peops_odd_even_bit",
            "pcsx_rearmed_gpu_peops_expand_screen_width",
            "pcsx_rearmed_gpu_peops_ignore_brightness",
            "pcsx_rearmed_gpu_peops_disable_coord_check",
            "pcsx_rearmed_gpu_peops_lazy_screen_update",
            "pcsx_rearmed_gpu_peops_old_frame_skip",
            "pcsx_rearmed_gpu_peops_repeated_triangles",
            "pcsx_rearmed_gpu_peops_quads_with_triangles",
            "pcsx_rearmed_gpu_peops_fake_busy_state",
         };

         option_display.visible = show_advanced_gpu_peops_settings;

         for (i = 0; i < 9; i++)
         {
            option_display.key = gpu_peops_option[i];
            environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
         }
      }
   }
#endif

#ifdef GPU_UNAI
   var.key = "pcsx_rearmed_gpu_unai_ilace_force";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_unai.ilace_force = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_unai.ilace_force = 1;
   }

   var.key = "pcsx_rearmed_gpu_unai_pixel_skip";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_unai.pixel_skip = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_unai.pixel_skip = 1;
   }

   var.key = "pcsx_rearmed_gpu_unai_lighting";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_unai.lighting = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_unai.lighting = 1;
   }

   var.key = "pcsx_rearmed_gpu_unai_fast_lighting";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_unai.fast_lighting = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_unai.fast_lighting = 1;
   }

   var.key = "pcsx_rearmed_gpu_unai_blending";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_unai.blending = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_unai.blending = 1;
   }

   var.key = "pcsx_rearmed_show_gpu_unai_settings";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int show_advanced_gpu_unai_settings_prev = show_advanced_gpu_unai_settings;

      show_advanced_gpu_unai_settings = 1;
      if (strcmp(var.value, "disabled") == 0)
         show_advanced_gpu_unai_settings = 0;

      if (show_advanced_gpu_unai_settings != show_advanced_gpu_unai_settings_prev)
      {
         unsigned i;
         struct retro_core_option_display option_display;
         char gpu_unai_option[5][40] = {
            "pcsx_rearmed_gpu_unai_blending",
            "pcsx_rearmed_gpu_unai_lighting",
            "pcsx_rearmed_gpu_unai_fast_lighting",
            "pcsx_rearmed_gpu_unai_ilace_force",
            "pcsx_rearmed_gpu_unai_pixel_skip",
         };

         option_display.visible = show_advanced_gpu_unai_settings;

         for (i = 0; i < 5; i++)
         {
            option_display.key = gpu_unai_option[i];
            environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
         }
      }
   }
#endif // GPU_UNAI

   if (in_flight) {
      // inform core things about possible config changes
      plugin_call_rearmed_cbs();

      if (GPU_open != NULL && GPU_close != NULL) {
         GPU_close();
         GPU_open(&gpuDisp, "PCSX", NULL);
      }

      dfinput_activate();
   }
   else
   {
      //not yet running

      //bootlogo display hack
      if (found_bios) {
         var.value = "NULL";
         var.key = "pcsx_rearmed_show_bios_bootlogo";
         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
         {
            Config.SlowBoot = 0;
            rebootemu = 0;
            if (strcmp(var.value, "enabled") == 0)
            {
               Config.SlowBoot = 1;
               rebootemu = 1;
            }
         }
      }
#ifndef DRC_DISABLE
      var.value = "NULL";
      var.key = "pcsx_rearmed_psxclock";
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
      {
         int psxclock = atoi(var.value);
         cycle_multiplier = 10000 / psxclock;
      }
#endif
   }
}

// Taken from beetle-psx-libretro
static uint16_t get_analog_button(int16_t ret, retro_input_state_t input_state_cb, int player_index, int id)
{
	// NOTE: Analog buttons were added Nov 2017. Not all front-ends support this
	// feature (or pre-date it) so we need to handle this in a graceful way.

	// First, try and get an analog value using the new libretro API constant
	uint16_t button = input_state_cb(player_index,
									RETRO_DEVICE_ANALOG,
									RETRO_DEVICE_INDEX_ANALOG_BUTTON,
									id);
	button = MIN(button / 128, 255);

	if (button == 0)
	{
		// If we got exactly zero, we're either not pressing the button, or the front-end
		// is not reporting analog values. We need to do a second check using the classic
		// digital API method, to at least get some response - better than nothing.

		// NOTE: If we're really just not holding the button, we're still going to get zero.

		button = (ret & (1 << id)) ? 255 : 0;
	}

	return button;
}

unsigned char axis_range_modifier(int16_t axis_value, bool is_square) {
	float modifier_axis_range = 0;

	if(is_square) {
		modifier_axis_range = round((axis_value >> 8) / 0.785) + 128;
		if(modifier_axis_range < 0) {
			modifier_axis_range = 0;
		} else if(modifier_axis_range > 255) {
			modifier_axis_range = 255;
		}
	} else {
		modifier_axis_range = MIN(((axis_value >> 8) + 128), 255);
	}

	return modifier_axis_range;
}

void retro_run(void)
{
	int i;
	//SysReset must be run while core is running,Not in menu (Locks up Retroarch)
	if (rebootemu != 0) {
		rebootemu = 0;
		SysReset();
		if (!Config.HLE && !Config.SlowBoot) {
			// skip BIOS logos
			psxRegs.pc = psxRegs.GPR.n.ra;
		}
	}

	if (display_internal_fps) {
		frame_count++;

		if (frame_count % INTERNAL_FPS_SAMPLE_PERIOD == 0) {
			unsigned internal_fps = pl_rearmed_cbs.flip_cnt * (is_pal_mode ? 50 : 60) / INTERNAL_FPS_SAMPLE_PERIOD;
			char str[64];
			const char *strc = (const char*)str;
			struct retro_message msg =
			{
				strc,
				180
			};

			str[0] = '\0';

			snprintf(str, sizeof(str), "Internal FPS: %2d", internal_fps);

			pl_rearmed_cbs.flip_cnt = 0;

			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
		}
	}
   else
		frame_count = 0;

	input_poll_cb();

	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		update_variables(true);

	// reset all keystate, query libretro for keystate
	int j;
	int lsx;
	int rsy;
	float negcon_twist_amplitude;
	int negcon_i_rs;
	int negcon_ii_rs;
	for(i = 0; i < PORTS_NUMBER; i++)
   {
      int16_t ret    = 0;
		in_keystate[i] = 0;

		if (in_type[i] == PSE_PAD_TYPE_NONE)
			continue;

      if (libretro_supports_bitmasks)
         ret = input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      else
      {
         unsigned j;
         for (j = 0; j < (RETRO_DEVICE_ID_JOYPAD_R3+1); j++)
         {
            if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, j))
               ret |= (1 << j);
         }
      }

		if (in_type[i] == PSE_PAD_TYPE_GUNCON)
		{
			//ToDo move across to:
			//RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X
			//RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y   
			//RETRO_DEVICE_ID_LIGHTGUN_TRIGGER
			//RETRO_DEVICE_ID_LIGHTGUN_RELOAD
			//RETRO_DEVICE_ID_LIGHTGUN_AUX_A 
			//RETRO_DEVICE_ID_LIGHTGUN_AUX_B
			//Though not sure these are hooked up properly on the Pi
			
			//ToDo
			//Put the controller index back to i instead of hardcoding to 1 when the libretro overlay crash bug is fixed
			//This is required for 2 player
			
			//GUNCON has 3 controls, Trigger,A,B which equal Circle,Start,Cross
			
			// Trigger
			//The 1 is hardcoded instead of i to prevent the overlay mouse button libretro crash bug
			if (input_state_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT)){
				in_keystate[i] |= (1 << DKEY_CIRCLE);
			}
			
			// A
			if (input_state_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)){
				in_keystate[i] |= (1 << DKEY_START);
			}
			
			// B
			if (input_state_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE)){
				in_keystate[i] |= (1 << DKEY_CROSS);
			}
			
			//The 1 is hardcoded instead of i to prevent the overlay mouse button libretro crash bug
			int gunx = input_state_cb(1, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
			int guny = input_state_cb(1, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
			
			//This adjustment process gives the user the ability to manually align the mouse up better 
			//with where the shots are in the emulator.
			
			//Percentage distance of screen to adjust 
			int GunconAdjustX = 0;
			int GunconAdjustY = 0;
			
			//Used when out by a percentage
			float GunconAdjustRatioX = 1;
			float GunconAdjustRatioY = 1;
				
			struct retro_variable var;
   			var.value = NULL;
   			var.key = "pcsx_rearmed_gunconadjustx";
   			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
			{
				GunconAdjustX = atoi(var.value);	
			}
      			
   			var.value = NULL;
   			var.key = "pcsx_rearmed_gunconadjusty";
   			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
			{
				GunconAdjustY = atoi(var.value);	
			} 
			
			
   			var.value = NULL;
   			var.key = "pcsx_rearmed_gunconadjustratiox";
   			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
			{
				GunconAdjustRatioX = atof(var.value);	
			} 
			
			
   			var.value = NULL;
   			var.key = "pcsx_rearmed_gunconadjustratioy";
   			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
			{
				GunconAdjustRatioY = atof(var.value);	
			} 
			
			//Mouse range is -32767 -> 32767
			//1% is about 655
			//Use the left analog stick field to store the absolute coordinates
			in_analog_left[0][0] = (gunx*GunconAdjustRatioX) + (GunconAdjustX * 655);
			in_analog_left[0][1] = (guny*GunconAdjustRatioY) + (GunconAdjustY * 655);
			
			
		}
		if (in_type[i] == PSE_PAD_TYPE_NEGCON)
		{
			// Query digital inputs
			//
			// > Pad-Up
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP))
				in_keystate[i] |= (1 << DKEY_UP);
			// > Pad-Right
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))
				in_keystate[i] |= (1 << DKEY_RIGHT);
			// > Pad-Down
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN))
				in_keystate[i] |= (1 << DKEY_DOWN);
			// > Pad-Left
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT))
				in_keystate[i] |= (1 << DKEY_LEFT);
			// > Start
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_START))
				in_keystate[i] |= (1 << DKEY_START);
			// > neGcon A
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_A))
				in_keystate[i] |= (1 << DKEY_CIRCLE);
			// > neGcon B
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_X))
				in_keystate[i] |= (1 << DKEY_TRIANGLE);
			// > neGcon R shoulder (digital)
			if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R))
				in_keystate[i] |= (1 << DKEY_R1);
			// Query analog inputs
			//
			// From studying 'libpcsxcore/plugins.c' and 'frontend/plugin.c':
			// >> pad->leftJoyX  == in_analog_left[i][0]  == NeGcon II
			// >> pad->leftJoyY  == in_analog_left[i][1]  == NeGcon L
			// >> pad->rightJoyX == in_analog_right[i][0] == NeGcon twist
			// >> pad->rightJoyY == in_analog_right[i][1] == NeGcon I
			// So we just have to map in_analog_left/right to more
			// appropriate inputs...
			//
			// > NeGcon twist
			// >> Get raw analog stick value and account for deadzone
			lsx = input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			if (lsx > negcon_deadzone)
				lsx = lsx - negcon_deadzone;
			else if (lsx < -negcon_deadzone)
				lsx = lsx + negcon_deadzone;
			else
				lsx = 0;
			// >> Convert to an 'amplitude' [-1.0,1.0] and adjust response
			negcon_twist_amplitude = (float)lsx / (float)(NEGCON_RANGE - negcon_deadzone);
			if (negcon_linearity == 2)
         {
				if (negcon_twist_amplitude < 0.0)
					negcon_twist_amplitude = -(negcon_twist_amplitude * negcon_twist_amplitude);
            else
					negcon_twist_amplitude = negcon_twist_amplitude * negcon_twist_amplitude;
			}
         else if (negcon_linearity == 3)
				negcon_twist_amplitude = negcon_twist_amplitude * negcon_twist_amplitude * negcon_twist_amplitude;
			// >> Convert to final 'in_analog' integer value [0,255]
			in_analog_right[i][0] = MAX(MIN((int)(negcon_twist_amplitude * 128.0f) + 128, 255), 0);
			// > NeGcon I + II
			// >> Handle right analog stick vertical axis mapping...
			//    - Up (-Y) == accelerate == neGcon I
			//    - Down (+Y) == brake == neGcon II
			negcon_i_rs = 0;
			negcon_ii_rs = 0;
			rsy = input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
			if (rsy >= 0){
				// Account for deadzone
				// (Note: have never encountered a gamepad with significant differences
				// in deadzone between left/right analog sticks, so use the regular 'twist'
				// deadzone here)
				if (rsy > negcon_deadzone)
					rsy = rsy - negcon_deadzone;
            else
					rsy = 0;
				// Convert to 'in_analog' integer value [0,255]
				negcon_ii_rs = MIN((int)(((float)rsy / (float)(NEGCON_RANGE - negcon_deadzone)) * 255.0f), 255);
			} else {
				if (rsy < -negcon_deadzone)
					rsy = -1 * (rsy + negcon_deadzone);
            else
					rsy = 0;
				negcon_i_rs = MIN((int)(((float)rsy / (float)(NEGCON_RANGE - negcon_deadzone)) * 255.0f), 255);
			}
			// >> NeGcon I
			in_analog_right[i][1] = MAX(
				MAX(
					get_analog_button(ret, input_state_cb, i, RETRO_DEVICE_ID_JOYPAD_R2),
					get_analog_button(ret, input_state_cb, i, RETRO_DEVICE_ID_JOYPAD_B)
				),
				negcon_i_rs
			);
			// >> NeGcon II
			in_analog_left[i][0] = MAX(
				MAX(
					get_analog_button(ret, input_state_cb, i, RETRO_DEVICE_ID_JOYPAD_L2),
					get_analog_button(ret, input_state_cb, i, RETRO_DEVICE_ID_JOYPAD_Y)
				),
				negcon_ii_rs
			);
			// > NeGcon L
			in_analog_left[i][1] = get_analog_button(ret, input_state_cb, i, RETRO_DEVICE_ID_JOYPAD_L);
		}
		if (in_type[i] != PSE_PAD_TYPE_NEGCON && in_type[i] != PSE_PAD_TYPE_GUNCON)
		{
			// Query digital inputs
			for (j = 0; j < RETRO_PSX_MAP_LEN; j++)
				if (ret & (1 << j))
					in_keystate[i] |= retro_psx_map[j];

			// Query analog inputs
			if (in_type[i] == PSE_PAD_TYPE_ANALOGJOY || in_type[i] == PSE_PAD_TYPE_ANALOGPAD)
			{
				in_analog_left[i][0] = axis_range_modifier(input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X), axis_bounds_modifier);
				in_analog_left[i][1] = axis_range_modifier(input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y), axis_bounds_modifier);
				in_analog_right[i][0] = axis_range_modifier(input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X), axis_bounds_modifier);
				in_analog_right[i][1] = axis_range_modifier(input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y), axis_bounds_modifier);
			}
		}
	}

	stop = 0;
	psxCpu->Execute();

	video_cb((vout_fb_dirty || !vout_can_dupe || !duping_enable) ? vout_buf_ptr : NULL,
		vout_width, vout_height, vout_width * 2);
	vout_fb_dirty = 0;

    set_vout_fb();
}

static bool try_use_bios(const char *path)
{
	FILE *f;
	long size;
	const char *name;

	f = fopen(path, "rb");
	if (f == NULL)
		return false;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fclose(f);

	if (size != 512 * 1024)
		return false;

	name = strrchr(path, SLASH);
	if (name++ == NULL)
		name = path;
	snprintf(Config.Bios, sizeof(Config.Bios), "%s", name);
	return true;
}

#ifndef VITA
#include <sys/types.h>
#include <dirent.h>

static bool find_any_bios(const char *dirpath, char *path, size_t path_size)
{
	DIR *dir;
	struct dirent *ent;
	bool ret = false;

	dir = opendir(dirpath);
	if (dir == NULL)
		return false;

	while ((ent = readdir(dir))) {
		if ((strncasecmp(ent->d_name, "scph", 4) != 0) && (strncasecmp(ent->d_name, "psx", 3) != 0))
			continue;

		snprintf(path, path_size, "%s%c%s", dirpath, SLASH, ent->d_name);
		ret = try_use_bios(path);
		if (ret)
			break;
	}
	closedir(dir);
	return ret;
}
#else
#define find_any_bios(...) false
#endif

static void check_system_specs(void)
{
   unsigned level = 6;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

static int init_memcards(void)
{
	int ret = 0;
	const char *dir;
	struct retro_variable var = { .key="pcsx_rearmed_memcard2", .value=NULL };
	static const char CARD2_FILE[] = "pcsx-card2.mcd";

	// Memcard2 will be handled and is re-enabled if needed using core
	// operations.
	// Memcard1 is handled by libretro, doing this will set core to
	// skip file io operations for memcard1 like SaveMcd
	snprintf(Config.Mcd1, sizeof(Config.Mcd1), "none");
	snprintf(Config.Mcd2, sizeof(Config.Mcd2), "none");
	init_memcard(Mcd1Data);
	// Memcard 2 is managed by the emulator on the filesystem,
	// There is no need to initialize Mcd2Data like Mcd1Data.

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		SysPrintf("Memcard 2: %s\n", var.value);
		if (memcmp(var.value, "enabled", 7) == 0) {
			if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) {
				if (strlen(dir) + strlen(CARD2_FILE) + 2 > sizeof(Config.Mcd2)) {
					SysPrintf("Path '%s' is too long. Cannot use memcard 2. Use a shorter path.\n", dir);
					ret = -1;
				} else {
					McdDisable[1] = 0;
					snprintf(Config.Mcd2, sizeof(Config.Mcd2), "%s/%s", dir, CARD2_FILE);
					SysPrintf("Use memcard 2: %s\n", Config.Mcd2);
				}
			} else {
				SysPrintf("Could not get save directory! Could not create memcard 2.");
				ret = -1;
			}
		}
	}
	return ret;
}

static void loadPSXBios(void)
{
	const char *dir;
	char path[PATH_MAX];
	unsigned useHLE = 0;

	const char *bios[] = {
		"PSXONPSP660", "psxonpsp660",
		"SCPH101", "scph101",
		"SCPH5501", "scph5501",
		"SCPH7001", "scph7001",
		"SCPH1001", "scph1001"
	};

	struct retro_variable var = {
		.key = "pcsx_rearmed_bios",
		.value = NULL
	};

	found_bios = 0;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (!strcmp(var.value, "HLE"))
			useHLE = 1;
	}

	if (!useHLE)
	{
		if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
		{
			unsigned i;
			snprintf(Config.BiosDir, sizeof(Config.BiosDir), "%s", dir);

			for (i = 0; i < sizeof(bios) / sizeof(bios[0]); i++) {
				snprintf(path, sizeof(path), "%s%c%s.bin", dir, SLASH, bios[i]);
				found_bios = try_use_bios(path);
				if (found_bios)
					break;
			}

			if (!found_bios)
				found_bios = find_any_bios(dir, path, sizeof(path));
		}
		if (found_bios) {
			SysPrintf("found BIOS file: %s\n", Config.Bios);
		}
	}

	if (useHLE || !found_bios)
	{
		SysPrintf("no BIOS files found.\n");
		struct retro_message msg =
		{
			"No PlayStation BIOS file found - add for better compatibility",
			180
		};
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
	}
}

void retro_init(void)
{
	unsigned dci_version = 0;
	struct retro_rumble_interface rumble;
	int ret;

#ifdef __MACH__
	// magic sauce to make the dynarec work on iOS
	syscall(SYS_ptrace, 0 /*PTRACE_TRACEME*/, 0, 0, 0);
#endif

#ifdef _3DS
   psxMapHook = pl_3ds_mmap;
   psxUnmapHook = pl_3ds_munmap;
#endif
#ifdef VITA
   if(init_vita_mmap()<0)
      abort();
   psxMapHook = pl_vita_mmap;
   psxUnmapHook = pl_vita_munmap;
#endif
	ret = emu_core_preinit();
#ifdef _3DS
   /* emu_core_preinit sets the cpu to dynarec */
   if(!__ctr_svchax)
      Config.Cpu = CPU_INTERPRETER;
#endif
	ret |= init_memcards();

	ret |= emu_core_init();
	if (ret != 0) {
		SysPrintf("PCSX init failed.\n");
		exit(1);
	}

#ifdef _3DS
   vout_buf = linearMemAlign(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2, 0x80);
#elif defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L) && !defined(VITA) && !defined(__SWITCH__)
	posix_memalign(&vout_buf, 16, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
#else
	vout_buf = malloc(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
#endif

	vout_buf_ptr = vout_buf;

	loadPSXBios();

	environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &vout_can_dupe);

	disk_initial_index   = 0;
	disk_initial_path[0] = '\0';
	if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dci_version) && (dci_version >= 1))
		environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control_ext);
	else
		environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);

	rumble_cb = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
		rumble_cb = rumble.set_rumble_state;

	/* Set how much slower PSX CPU runs * 100 (so that 200 is 2 times)
	 * we have to do this because cache misses and some IO penalties
	 * are not emulated. Warning: changing this may break compatibility. */
	cycle_multiplier = 175;
#ifdef HAVE_PRE_ARMV7
	cycle_multiplier = 200;
#endif
	pl_rearmed_cbs.gpu_peops.iUseDither = 1;
	pl_rearmed_cbs.gpu_peops.dwActFixes = GPU_PEOPS_OLD_FRAME_SKIP;
	spu_config.iUseFixedUpdates = 1;

	SaveFuncs.open = save_open;
	SaveFuncs.read = save_read;
	SaveFuncs.write = save_write;
	SaveFuncs.seek = save_seek;
	SaveFuncs.close = save_close;

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

	update_variables(false);
	check_system_specs();
}

void retro_deinit(void)
{
	ClosePlugins();
	SysClose();
#ifdef _3DS
   linearFree(vout_buf);
#else
	free(vout_buf);
#endif
	vout_buf = NULL;

#ifdef VITA
  deinit_vita_mmap();
#endif
   libretro_supports_bitmasks = false;

	/* Have to reset disks struct, otherwise
	 * fnames/flabels will leak memory */
	disk_init();
}

#ifdef VITA
#include <psp2/kernel/threadmgr.h>
int usleep (unsigned long us)
{
   sceKernelDelayThread(us);
}
#endif

void SysPrintf(const char *fmt, ...) {
	va_list list;
	char msg[512];

	va_start(list, fmt);
	vsprintf(msg, fmt, list);
	va_end(list);

	if (log_cb)
		log_cb(RETRO_LOG_INFO, "%s", msg);
}
