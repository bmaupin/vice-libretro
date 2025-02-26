#include "libretro.h"
#include "libretro-core.h"
#include "file/file_path.h"
#include "log.h"

#include "archdep.h"
#include "c64.h"
#include "c64mem.h"
#include "mem.h"
#include "machine.h"
#include "snapshot.h"
#include "autostart.h"
#include "tape.h"
#ifndef __PET__
#include "cartridge.h"
#endif

//CORE VAR
#ifdef _WIN32
char slash = '\\';
#else
char slash = '/';
#endif

bool retro_load_ok = false;

retro_log_printf_t log_cb;

char RETRO_DIR[512];
char RPATH_basename[512];
char save_file[512];

int mapper_keys[37]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static char buf[64][4096] = { 0 };

// Our virtual time counter, increased by retro_run()
long microSecCounter=0;
int cpuloop=1;

#ifdef FRONTEND_SUPPORTS_RGB565
	uint16_t *Retro_Screen;
	uint16_t bmp[WINDOW_SIZE];
	uint16_t save_Screen[WINDOW_SIZE];
#else
	unsigned int *Retro_Screen;
	unsigned int bmp[WINDOW_SIZE];
	unsigned int save_Screen[WINDOW_SIZE];
#endif

//SOUND
short signed int SNDBUF[1024*2];

//PATH
char RPATH[512];

extern int SHOWKEY;
extern int REQUEST_SNAPSHOT_SAVE;
extern int REQUEST_SNAPSHOT_LOAD;

extern int app_init(void);
extern int app_free(void);
extern int app_render(int poll);

int CROP_WIDTH;
int CROP_HEIGHT;
int VIRTUAL_WIDTH;

int retroXS=0;
int retroYS=0;
int retroW=1024;
int retroH=768;
int retrow=1024;
int retroh=768;
int lastW=1024;
int lastH=768;

#include "vkbd.i"

unsigned vice_devices[5];

extern int RETROTDE;
extern int RETRODSE;
extern int RETRODSEVOL;
extern int RETROSTATUS;
extern int RETRORESET;
extern int RETROSIDMODL;
extern int RETRORESIDSAMPLING;
extern int RETROC64MODL;
extern int RETROUSERPORTJOY;
extern int RETROEXTPAL;
extern int RETROAUTOSTARTWARP;
extern int RETROBORDERS;
extern int RETROTHEME;
extern char RETROEXTPALNAME[512];
extern int retro_ui_finalized;
extern unsigned int cur_port;
extern unsigned int datasette;
extern int cur_port_locked;
extern void reset_mouse_pos();
extern uint8_t mem_ram[];
extern int g_mem_ram_size;

//VICE DEF BEGIN
#include "resources.h"
#include "sid.h"
#include "userport_joystick.h"
#if defined(__VIC20__)
#include "c64model.h"
#include "vic20model.h"
#elif defined(__PLUS4__)
#include "c64model.h"
#include "plus4model.h"
#elif defined(__X128__)
#include "c64model.h"
#include "c128model.h"
#elif defined(__PET__)
#include "petmodel.h"
#elif defined(__CBM2__)
#include "cbm2model.h"
#else
#include "c64model.h"
#endif
//VICE DEF END

const char *retro_save_directory;
const char *retro_system_directory;
const char *retro_content_directory;
char retro_system_data_directory[512];

/*static*/ retro_input_state_t input_state_cb;
/*static*/ retro_input_poll_t input_poll_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;

#include "retro_strings.h"
#include "retro_files.h"
#include "retro_disk_control.h"
static dc_storage* dc;
enum {
	RUNSTATE_FIRST_START = 0,
	RUNSTATE_LOADED_CONTENT,
	RUNSTATE_RUNNING,
};
static int runstate = RUNSTATE_FIRST_START; /* used to detect whether we are just starting the core from scratch */
/* runstate = RUNSTATE_FIRST_START: first time retro_run() is called after loading and starting core */
/* runstate = RUNSTATE_LOADED_CONTENT: load content was selected while core is running, so do an autostart_reset() */
/* runstate = RUNSTATE_RUNNING: core is running normally */


unsigned retro_get_borders(void);

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

static char CMDFILE[512];

int loadcmdfile(char *argv)
{
   int res=0;

   FILE *fp = fopen(argv,"r");

   if( fp != NULL )
   {
      if ( fgets (CMDFILE , 512 , fp) != NULL )
         res=1;
      fclose (fp);
   }

   return res;
}

int HandleExtension(char *path,char *ext)
{
   int len = strlen(path);

   if (len >= 4 &&
         path[len-4] == '.' &&
         path[len-3] == ext[0] &&
         path[len-2] == ext[1] &&
         path[len-1] == ext[2])
   {
      return 1;
   }

   return 0;
}

#include <ctype.h>

//Args for experimental_cmdline
static char ARGUV[64][1024];
static unsigned char ARGUC=0;

// Args for Core
static char XARGV[64][1024];
static const char* xargv_cmd[64];
int PARAMCOUNT=0;

extern int  skel_main(int argc, char *argv[]);
void parse_cmdline( const char *argv );

void Add_Option(const char* option)
{
   static int first=0;

   if(first==0)
   {
      PARAMCOUNT=0;
      first++;
   }

   sprintf(XARGV[PARAMCOUNT++],"%s",option);
}

int pre_main(const char *argv)
{
   int i=0;
   bool Only1Arg;
   bool Skip_Option=0;

   if (strlen(argv) > strlen("cmd"))
   {
      if( HandleExtension((char*)argv,"cmd") || HandleExtension((char*)argv,"CMD"))
         i=loadcmdfile((char*)argv);
   }

   if(i==1)
   {
      parse_cmdline(CMDFILE);
      LOGI("Starting game from command line :%s\n",CMDFILE);
   }
   else
      parse_cmdline(argv);

   Only1Arg = (strcmp(ARGUV[0],CORE_NAME) == 0) ? 0 : 1;

   for (i = 0; i<64; i++)
      xargv_cmd[i] = NULL;


   if(Only1Arg)
   {  Add_Option(CORE_NAME);
      /*
         if (strlen(RPATH) >= strlen("crt"))
         if(!strcasecmp(&RPATH[strlen(RPATH)-strlen("crt")], "crt"))
         Add_Option("-cartcrt");
         */

#if defined(__VIC20__)
     if (strlen(RPATH) >= strlen(".20"))
       if (!strcasecmp(&RPATH[strlen(RPATH)-strlen(".20")], ".20"))
	 Add_Option("-cart2");

     if (strlen(RPATH) >= strlen(".40"))
       if (!strcasecmp(&RPATH[strlen(RPATH)-strlen(".40")], ".40"))
	 Add_Option("-cart4");

     if (strlen(RPATH) >= strlen(".60"))
       if (!strcasecmp(&RPATH[strlen(RPATH)-strlen(".60")], ".60"))
	 Add_Option("-cart6");

     if (strlen(RPATH) >= strlen(".a0"))
       if (!strcasecmp(&RPATH[strlen(RPATH)-strlen(".a0")], ".a0"))
	 Add_Option("-cartA");

     if (strlen(RPATH) >= strlen(".b0"))
       if (!strcasecmp(&RPATH[strlen(RPATH)-strlen(".b0")], ".b0"))
	 Add_Option("-cartB");
#endif

     Add_Option(RPATH/*ARGUV[0]*/);
   }
   else
   { // Pass all cmdline args
      for(i = 0; i < ARGUC; i++) {
         Skip_Option=0;
         if(strstr(ARGUV[i], "-j1")) {
            Skip_Option=1;
            cur_port=1;
            cur_port_locked = 1;
         }
         if(strstr(ARGUV[i], "-j2")) {
            Skip_Option=1;
            cur_port=2;
            cur_port_locked = 1;
         }
         
         if(!Skip_Option)
            Add_Option(ARGUV[i]);
      }
   }

   for (i = 0; i < PARAMCOUNT; i++)
   {
      xargv_cmd[i] = (char*)(XARGV[i]);
      LOGI("%2d  %s\n",i,XARGV[i]);
   }

   skel_main(PARAMCOUNT,( char **)xargv_cmd);

   xargv_cmd[PARAMCOUNT - 2] = NULL;

   return 0;
}

void parse_cmdline(const char *argv)
{
   char *p,*p2,*start_of_word;
   int c,c2;
   static char buffer[512*4];
   enum states { DULL, IN_WORD, IN_STRING } state = DULL;

   strcpy(buffer,argv);
   strcat(buffer," \0");

   for (p = buffer; *p != '\0'; p++)
   {
      c = (unsigned char) *p; /* convert to unsigned char for is* functions */
      switch (state)
      {
         case DULL: /* not in a word, not in a double quoted string */
            if (isspace(c)) /* still not in a word, so ignore this char */
               continue;
            /* not a space -- if it's a double quote we go to IN_STRING, else to IN_WORD */
            if (c == '"')
            {
               state = IN_STRING;
               start_of_word = p + 1; /* word starts at *next* char, not this one */
               continue;
            }
            state = IN_WORD;
            start_of_word = p; /* word starts here */
            continue;
         case IN_STRING:
            /* we're in a double quoted string, so keep going until we hit a close " */
            if (c == '"')
            {
               /* word goes from start_of_word to p-1 */
               //... do something with the word ...
               for (c2 = 0,p2 = start_of_word; p2 < p; p2++, c2++)
                  ARGUV[ARGUC][c2] = (unsigned char) *p2;
               ARGUC++;

               state = DULL; /* back to "not in word, not in string" state */
            }
            continue; /* either still IN_STRING or we handled the end above */
         case IN_WORD:
            /* we're in a word, so keep going until we get to a space */
            if (isspace(c))
            {
               /* word goes from start_of_word to p-1 */
               //... do something with the word ...
               for (c2 = 0,p2 = start_of_word; p2 <p; p2++,c2++)
                  ARGUV[ARGUC][c2] = (unsigned char) *p2;
               ARGUC++;

               state = DULL; /* back to "not in word, not in string" state */
            }
            continue; /* either still IN_WORD or we handled the end above */
      }
   }
}

long GetTicks(void) {
   // NOTE: Cores should normally not depend on real time, so we return a
   // counter here
   // GetTicks() is used by vsyncarch_gettime() which is used by
   // * Vsync (together with sleep) to sync to 50Hz
   // * Mouse timestamps
   // * Networking
   // Returning a frame based msec counter could potentially break
   // networking but it's not something libretro uses at the moment.
   return microSecCounter;
}

void save_bkg(void)
{
   memcpy(save_Screen,Retro_Screen,PITCH*WINDOW_SIZE);
}

void restore_bgk(void)
{
   memcpy(Retro_Screen,save_Screen,PITCH*WINDOW_SIZE);
}

void Screen_SetFullUpdate(int scr)
{
   if(scr==0 ||scr>1)
      memset(&Retro_Screen, 0, sizeof(Retro_Screen));
   if(scr>0)
      memset(&bmp,0,sizeof(bmp));
}

int keyId(const char *val)
{
   int i=0;
   while (keyDesc[i]!=NULL)
   {
      if (!strcmp(keyDesc[i],val))
         return keyVal[i];
      i++;
   }
   return 0;
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_controller_description p1_controllers[] = {
      { "Vice Joystick", RETRO_DEVICE_VICE_JOYSTICK },
      { "Vice Keyboard", RETRO_DEVICE_VICE_KEYBOARD },
      { "Disconnected", RETRO_DEVICE_NONE },
   };
   static const struct retro_controller_description p2_controllers[] = {
      { "Vice Joystick", RETRO_DEVICE_VICE_JOYSTICK },
      { "Vice Keyboard", RETRO_DEVICE_VICE_KEYBOARD },
      { "Disconnected", RETRO_DEVICE_NONE },
   };
   static const struct retro_controller_description p3_controllers[] = {
      { "Vice Joystick", RETRO_DEVICE_VICE_JOYSTICK },
      { "Vice Keyboard", RETRO_DEVICE_VICE_KEYBOARD },
      { "Disconnected", RETRO_DEVICE_NONE },
   };
   static const struct retro_controller_description p4_controllers[] = {
      { "Vice Joystick", RETRO_DEVICE_VICE_JOYSTICK },
      { "Vice Keyboard", RETRO_DEVICE_VICE_KEYBOARD },
      { "Disconnected", RETRO_DEVICE_NONE },
   };
   static const struct retro_controller_description p5_controllers[] = {
      { "Vice Joystick", RETRO_DEVICE_VICE_JOYSTICK },
      { "Vice Keyboard", RETRO_DEVICE_VICE_KEYBOARD },
      { "Disconnected", RETRO_DEVICE_NONE },
   };

   static const struct retro_controller_info ports[] = {
      { p1_controllers, 3 }, // port 1
      { p2_controllers, 3 }, // port 2
      { p3_controllers, 3 }, // port 3
      { p4_controllers, 3 }, // port 4
      { p5_controllers, 3 }, // port 5
      { NULL, 0 }
   };

   static struct retro_core_option_definition core_options[] =
   {
      {
         "vice_statusbar",
         "Statusbar",
         "Display a statusbar with joystick indicators etc.",
         {
            { "disabled", NULL },
            { "enabled", NULL },
            { NULL, NULL },
         },
         "disabled"
      },
      {
         "vice_drive_true_emulation",
         "Drive true emulation",
         "True emulation loads much slower, but some games need it",
         {
            { "disabled", NULL },
            { "enabled", NULL },
            { NULL, NULL },
         },
         "disabled"
      },
      {
         "vice_drive_sound_emulation",
         "Drive sound emulation",
         "Emulates the iconic floppy drive sounds for nostalgia. D64 & true drive emulation required.",
         {
            { "disabled", NULL },
            { "enabled", NULL },
            { NULL, NULL },
         },
         "disabled"
      },
      {
         "vice_drive_sound_volume",
         "Drive sound volume",
         "Only makes a difference if drive sound emulation is on",
         {
            { "10\%", NULL },
            { "20\%", NULL },
            { "30\%", NULL },
            { "40\%", NULL },
            { "50\%", NULL },
            { "60\%", NULL },
            { "70\%", NULL },
            { "80\%", NULL },
            { "90\%", NULL },
            { "100\%", NULL },
            { NULL, NULL },
         },
         "10\%"
      },
      {
         "vice_autostart_warp",
         "Autostart warp",
         "Automatically turns on warp mode between load and run for faster loading",
         {
            { "disabled", NULL },
            { "enabled", NULL },
            { NULL, NULL },
         },
         "disabled"
      },
      {
         "vice_sid_model",
         "SID model",
         "ReSID is accurate but slower, 6581 was used in original C64",
         {
            { "6581F", "6581 FastSID" },
            { "6581R", "6581 ReSID" },
            { "8580F", "8580 FastSID" },
            { "8580R", "8580 ReSID" },
            { "8580RD", "8580 ReSID + digi boost" },
            { NULL, NULL },
         },
         "6581F"
      },
      {
         "vice_resid_sampling",
         "ReSID sampling",
         "Resampling for best quality. Fast setting improves performance dramatically on PS Vita",
         {
            { "Resampling", NULL },
            { "Fast resampling", NULL },
            { "Interpolation", NULL },
            { "Fast", NULL },
            { NULL, NULL },
         },
         "Resampling"
      },
#if  defined(__VIC20__)
      {
         "vice_vic20_model",
         "VIC20 model",
         "",
         {
            { "VIC20 PAL", NULL },
            { "VIC20 NTSC", NULL },
            { "SuperVIC (+16K)", NULL },
            { NULL, NULL },
         },
         "VIC20 PAL"
      },
#elif  defined(__PLUS4__)
      {
         "vice_plus4_model",
         "PLUS4 model",
         "",
         {
            { "C16 PAL", NULL },
            { "C16 NTSC", NULL },
            { "PLUS4 PAL", NULL },
            { "PLUS4 NTSC", NULL },
            { "V364 NTSC", NULL },
            { "232 NTSC", NULL },
            { NULL, NULL }
         },
         "C16 PAL"
      },
#elif  defined(__X128__)
      {
         "vice_c128_model",
         "C128 model",
         "",
         {
            { "C128 PAL", NULL },
            { "C128DCR PAL", NULL },
            { "C128 NTSC", NULL },
            { "C128DCR NTSC", NULL },
            { NULL, NULL },
         },
         "C128 PAL"
      },
#elif  defined(__PET__)
      {
         "vice_pet_model",
         "PET model",
         "",
         {
            { "2001", NULL },
            { "3008", NULL },
            { "3016", NULL },
            { "3032", NULL },
            { "3032B", NULL },
            { "4016", NULL },
            { "4032", NULL },
            { "4032B", NULL },
            { "8032", NULL },
            { "8096", NULL },
            { "8096", NULL },
            { "8296", NULL },
            { "SUPERPET", NULL },
            { NULL, NULL },
         },
         "2001"
      },
#elif  defined(__CBM2__)
      {
         "vice_cbm2_model",
         "CBM2 model",
         "",
         {
            { "510 PAL", NULL },
            { "610 PAL", NULL },
            { "610 NTSC", NULL },
            { "620 PAL", NULL },
            { "620 NTSC", NULL },
            { "620PLUS PAL", NULL },
            { "710 NTSC", NULL },
            { "720 NTSC", NULL },
            { "710 NTSC", NULL },
            { "720PLUS NTSC", NULL },
            { NULL, NULL },
         },
         "510 PAL"
      },
#else
      {
         "vice_c64_model",
         "C64 model",
         "",
         {
            { "C64 PAL", NULL },
            { "C64C PAL", NULL },
            //{ "C64 OLD PAL", NULL },
            { "C64 NTSC", NULL },
            { "C64C NTSC", NULL },
            //{ "C64 OLD NTSC", NULL },
            //{ "C64 PAL N", NULL },
            { "C64SX PAL", NULL },
            { "C64SX NTSC", NULL },
            { "C64 JAP", NULL },
            { "C64 GS", NULL },
            { "PET64 PAL", NULL },
            { "PET64 NTSC", NULL },
            { NULL, NULL },
         },
         "C64 PAL"
      },
#endif
#if !defined(__PET__) && !defined(__CBM2__)
      {
         "vice_border",
         "Display borders",
         "",
         {
            { "enabled", NULL },
            { "disabled", NULL },
            { NULL, NULL },
         },
         "enabled"
      },
#endif
#if defined(__VIC20__)
      {
         "vice_vic20_external_palette",
         "Color palette",
         "Colodore is recommended for the most accurate colors",
         {
            { "Default", NULL },
            { "Colodore VIC", NULL },
            { "Mike NTSC", NULL },
            { "Mike PAL", NULL },
            { "Vice", NULL },
            { NULL, NULL },
         },
         "Default"
      },
#elif defined(__PLUS4__) 
      {
         "vice_plus4_external_palette",
         "Color palette",
         "Colodore is recommended for the most accurate colors",
         {
            { "Default", NULL },
            { "Colodore TED", NULL },
            { "Yape PAL", NULL },
            { "Yape NTSC", NULL },
            { NULL, NULL },
         },
         "Default"
      },
#elif defined(__PET__)
      {
         "vice_pet_external_palette",
         "Color palette",
         "",
         {
            { "Default", NULL },
            { "Amber", NULL },
            { "Green", NULL },
            { "White", NULL },
            { NULL, NULL },
         },
         "Default"
      },
#elif defined(__CBM2__)
      {
         "vice_cbm2_external_palette",
         "Color palette",
         "",
         {
            { "Default", NULL },
            { "Amber", NULL },
            { "Green", NULL },
            { "White", NULL },
            { NULL, NULL },
         },
         "Default"
      },
#else
      {
         "vice_external_palette",
         "Color palette",
         "Colodore is recommended for most accurate colors",
         {
            { "Default", NULL },
            { "Colodore", NULL },
            { "Community Colors", NULL },
            { "Pepto PAL", NULL },
            { "Pepto PAL old", NULL },
            { "Pepto NTSC Sony", NULL },
            { "Pepto NTSC", NULL },
            { "C64HQ", NULL },
            { "C64S", NULL },
            { "CCS64", NULL },
            { "Deekay", NULL },
            { "Frodo", NULL },
            { "Godot", NULL },
            { "PC64", NULL },
            { "Ptoing", NULL },
            { "RGB", NULL },
            { "Vice", NULL },
            { NULL, NULL },
         },
         "Default"
      },
#endif
      {
         "vice_theme",
         "Virtual keyboard theme",
         "By default, the keyboard comes up with L button or F11 key",
         {
            { "C64", NULL },
            { "C64 transparent", NULL },
            { "C64C", NULL },
            { "C64C transparent", NULL },
            { "Transparent", NULL },
            { NULL, NULL },
         },
         "C64"
      },
      {
         "vice_reset",
         "Reset type",
         "Soft keeps some code in memory, hard erases all memory",
         {
            { "Autostart", NULL },
            { "Soft", NULL },
            { "Hard", NULL },
            { NULL, NULL },
         },
         "Autostart"
      },
      {
         "vice_userport_joytype",
         "4-player adapter",
         "Useful to play IK+ Gold with 3 players and more",
         {
            { "None", NULL },
            { "Protovision CGA", NULL },
            { "PET", NULL },
            { "Hummer", NULL },
            { "OEM", NULL },
            { "Hit", NULL }, 
            { "Kingsoft", NULL },
            { "Starbyte", NULL },
            { NULL, NULL },
         },
         "None"
      },
      {
         "vice_joyport",
         "RetroPad port",
         "Most games use port 2, some use port 1",
         {
            { "Port 2", NULL },
            { "Port 1", NULL },
            { NULL, NULL },
         },
         "Port 2"
      },
/* Button mappings */
      {
         "vice_mapper_select",
         "RetroPad Select",
         "",
         {{ NULL, NULL }},
         "---"
      },
      {
         "vice_mapper_start",
         "RetroPad Start",
         "",
         {{ NULL, NULL }},
         "---"
      },
      {
         "vice_mapper_y",
         "RetroPad Y",
         "",
         {{ NULL, NULL }},
         "RETROK_SPACE"
      },
      {
         "vice_mapper_x",
         "RetroPad X",
         "",
         {{ NULL, NULL }},
         "RETROK_F1"
      },
      {
         "vice_mapper_b",
         "RetroPad B",
         "",
         {{ NULL, NULL }},
         "RETROK_F7"
      },

      {
         "vice_mapper_l",
         "RetroPad L",
         "",
         {{ NULL, NULL }},
         "RETROK_F11"
      },
      {
         "vice_mapper_r",
         "RetroPad R",
         "",
         {{ NULL, NULL }},
         "RETROK_F10"
      },
      {
         "vice_mapper_l2",
         "RetroPad L2",
         "",
         {{ NULL, NULL }},
         "RETROK_ESCAPE"
      },
      {
         "vice_mapper_r2",
         "RetroPad R2",
         "",
         {{ NULL, NULL }},
         "RETROK_RETURN"
      },
      {
         "vice_mapper_l3",
         "RetroPad L3",
         "",
         {{ NULL, NULL }},
         "RETROK_h"
      },
      {
         "vice_mapper_r3",
         "RetroPad R3",
         "",
         {{ NULL, NULL }},
         "RETROK_t"
      },
/* Left Stick */
      {
         "vice_mapper_lu",
         "RetroPad L-Up",
         "Mapping for left analog stick up",
         {{ NULL, NULL }},
         "RETROK_UP"
      },
      {
         "vice_mapper_ld",
         "RetroPad L-Down",
         "Mapping for left analog stick down",
         {{ NULL, NULL }},
         "RETROK_DOWN"
      },
      {
         "vice_mapper_ll",
         "RetroPad L-Left",
         "Mapping for left analog stick left",
         {{ NULL, NULL }},
         "RETROK_LEFT"
      },
      {
         "vice_mapper_lr",
         "RetroPad L-Right",
         "Mapping for left analog stick right",
         {{ NULL, NULL }},
         "RETROK_RIGHT"
      },

/* Right Stick */
      {
         "vice_mapper_ru",
         "RetroPad R-Up",
         "Mapping for right analog stick up",
         {{ NULL, NULL }},
         "RETROK_y"
      },
      {
         "vice_mapper_rd",
         "RetroPad R-Down",
         "Mapping for right analog stick down",
         {{ NULL, NULL }},
         "RETROK_n"
      },
      {
         "vice_mapper_rl",
         "RetroPad R-Left",
         "Mapping for right analog stick left",
         {{ NULL, NULL }},
         "RETROK_l"
      },
      {
         "vice_mapper_rr",
         "RetroPad R-Right",
         "Mapping for right analog stick right",
         {{ NULL, NULL }},
         "RETROK_r"
      },
/* Hotkeys */
      {
         "vice_mapper_vkbd",
         "Hotkey: Toggle virtual keyboard",
         "Pressing a button mapped to this key opens the keyboard",
         {{ NULL, NULL }},
         "RETROK_F11"
      },
      {
         "vice_mapper_statusbar",
         "Hotkey: Toggle statusbar",
         "Pressing a button mapped to this key toggles statusbar",
         {{ NULL, NULL }},
         "RETROK_F10"
      },
      {
         "vice_mapper_joyport_switch",
         "Hotkey: Switch joyports",
         "Pressing a button mapped to this key swaps joyports",
         {{ NULL, NULL }},
         "RETROK_RCTRL"
      },
      {
         "vice_mapper_reset",
         "Hotkey: Reset",
         "Pressing a button mapped to this key triggers reset",
         {{ NULL, NULL }},
         "RETROK_END"
      },
      {
         "vice_mapper_warp_mode",
         "Hotkey: Warp mode",
         "Hold this key, or a button mapped to it, for warp mode",
         {{ NULL, NULL }},
         "RETROK_PAGEDOWN"
      },
      {
         "vice_mapper_snapshot_save",
         "Hotkey: Snapshot save",
         "Vice snapshot save, temporary",
         {{ NULL, NULL }},
         "---"
      },
      {
         "vice_mapper_snapshot_load",
         "Hotkey: Snapshot load",
         "Vice snapshot load, temporary",
         {{ NULL, NULL }},
         "---"
      },
/* Datasette controls */
      {
         "vice_mapper_datasette_toggle_hotkeys",
         "Hotkey: Toggle datasette hotkeys",
         "This key enables/disables the datasette hotkeys",
         {{ NULL, NULL }},
         "---"
      },
      {
         "vice_datasette_hotkeys",
         "Datasette hotkeys",
         "Enable or disable all datasette hotkeys",
         {
            { "disabled", NULL },
            { "enabled", NULL },
            { NULL, NULL },
         },
         "disabled"
      },
      {
         "vice_mapper_datasette_stop",
         "Hotkey: Datasette stop",
         "Press stop on tape",
         {{ NULL, NULL }},
         "RETROK_DOWN"
      },
      {
         "vice_mapper_datasette_start",
         "Hotkey: Datasette start",
         "Press start on tape",
         {{ NULL, NULL }},
         "RETROK_UP"
      },
      {
         "vice_mapper_datasette_forward",
         "Hotkey: Datasette forward",
         "Tape fast forward",
         {{ NULL, NULL }},
         "RETROK_RIGHT"
      },
      {
         "vice_mapper_datasette_rewind",
         "Hotkey: Datasette rewind",
         "Tape rewind",
         {{ NULL, NULL }},
         "RETROK_LEFT"
      },
      {
         "vice_mapper_datasette_reset",
         "Hotkey: Datasette reset",
         "Tape reset",
         {{ NULL, NULL }},
         "---"
      },
      { NULL, NULL, NULL, {{0}}, NULL },
   };

   /* fill in the values for all the mappers */
   int i = 0;
   int j = 0;
   while(core_options[i].key) 
   {
      if (strstr(core_options[i].key, "vice_mapper_"))
      {
         j = 0;
         while(keyDesc[j] && j < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1)
         {
            core_options[i].values[j].value = keyDesc[j];
            core_options[i].values[j].label = NULL;
            ++j;
         };
         core_options[i].values[j].value = NULL;
         core_options[i].values[j].label = NULL;
      };
      ++i;
   }

   static bool allowNoGameMode;

   environ_cb = cb;

   cb( RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports );

   unsigned version = 0;
   if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && (version == 1))
      cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, core_options);
   else
   {
      /* Fallback for older API */
      static struct retro_variable variables[64] = { 0 };
      i = 0;
      while(core_options[i].key) 
      {
         buf[i][0] = 0;
         variables[i].key = core_options[i].key;
         strcpy(buf[i], core_options[i].desc);
         strcat(buf[i], "; ");
         j = 0;
         while(core_options[i].values[j].value && j < RETRO_NUM_CORE_OPTION_VALUES_MAX)
         {
            if (j == 0)
            {
               strcat(buf[i], core_options[i].default_value);
            }
            else
            {
               strcat(buf[i], "|");
               strcat(buf[i], core_options[i].values[j].value);
            }
            ++j;
         };
         variables[i].value = buf[i];
         ++i;
      };
      variables[i].key = NULL;
      variables[i].value = NULL;      
      cb( RETRO_ENVIRONMENT_SET_VARIABLES, variables);
   }

   allowNoGameMode = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &allowNoGameMode);
}

static void update_variables(void)
{
   struct retro_variable var;

   var.key = "vice_statusbar";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if(retro_ui_finalized){
         if (strcmp(var.value, "enabled") == 0)
            resources_set_int("SDLStatusbar", 1);
         if (strcmp(var.value, "disabled") == 0)
            resources_set_int("SDLStatusbar", 0);
      }
      else {
         if (strcmp(var.value, "enabled") == 0)RETROSTATUS=1;
         if (strcmp(var.value, "disabled") == 0)RETROSTATUS=0;
      }
   }

   var.key = "vice_autostart_warp";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if(retro_ui_finalized){
         if (strcmp(var.value, "enabled") == 0)
            resources_set_int("AutostartWarp", 1);
         if (strcmp(var.value, "disabled") == 0)
            resources_set_int("AutostartWarp", 0);
      }
      else {
         if (strcmp(var.value, "enabled") == 0)RETROAUTOSTARTWARP=1;
         if (strcmp(var.value, "disabled") == 0)RETROAUTOSTARTWARP=0;
      }
   }

   var.key = "vice_drive_true_emulation";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if(retro_ui_finalized){
         if (strcmp(var.value, "enabled") == 0){
            if(RETROTDE==0){
               RETROTDE=1;
               resources_set_int("DriveTrueEmulation", 1);
               resources_set_int("VirtualDevices", 0);
            }
         }
         else if (strcmp(var.value, "disabled") == 0){
            if(RETROTDE==1){
               RETROTDE=0;
               resources_set_int("DriveTrueEmulation", 0);
               resources_set_int("VirtualDevices", 1);
            }
         }
      }
      else {
         if (strcmp(var.value, "enabled") == 0)RETROTDE=1;
         if (strcmp(var.value, "disabled") == 0)RETROTDE=0;
      }
   }

   var.key = "vice_drive_sound_emulation";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if(retro_ui_finalized){
         if (strcmp(var.value, "enabled") == 0){
            resources_set_int("DriveSoundEmulation", 1);
         }
         else if (strcmp(var.value, "disabled") == 0){
            resources_set_int("DriveSoundEmulation", 0);
    	 }
      }
      else {
         if (strcmp(var.value, "enabled") == 0)RETRODSE=1;
         if (strcmp(var.value, "disabled") == 0)RETRODSE=0;
      }
   }

   var.key = "vice_drive_sound_volume";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int val = atoi(var.value);
      val = val * 20;
      
      if(retro_ui_finalized)
         resources_set_int("DriveSoundEmulationVolume", val);
      else RETRODSEVOL=val;
   }


   var.key = "vice_sid_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int eng=0,modl=0,sidmdl=0;

      if (strcmp(var.value, "6581F") == 0){eng=0;modl=0;}
      else if (strcmp(var.value, "8580F") == 0){eng=0;modl=1;}
      else if (strcmp(var.value, "6581R") == 0){eng=1;modl=0;}
      else if (strcmp(var.value, "8580R") == 0){eng=1;modl=1;}
      else if (strcmp(var.value, "8580RD") == 0){eng=1;modl=2;}

      sidmdl=((eng << 8) | modl) ;

      if(retro_ui_finalized)
        sid_set_engine_model(eng, modl);
      else RETROSIDMODL=sidmdl;
   }

   var.key = "vice_resid_sampling";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int resid=0;
      
      if (strcmp(var.value, "Fast") == 0){ resid=0; }
      else if (strcmp(var.value, "Interpolation") == 0){ resid=1; }
      else if (strcmp(var.value, "Resampling") == 0){ resid=2; }
      else if (strcmp(var.value, "Fast resampling") == 0){ resid=3; }

      if(retro_ui_finalized)
        resources_set_int("SidResidSampling", resid);
      else RETRORESIDSAMPLING=resid;
   }

#if  defined(__VIC20__)
   var.key = "vice_vic20_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "VIC20 PAL") == 0)modl=VIC20MODEL_VIC20_PAL;
      else if (strcmp(var.value, "VIC20 NTSC") == 0)modl=VIC20MODEL_VIC20_NTSC;
      else if (strcmp(var.value, "SuperVIC (+16K)") == 0)modl=VIC20MODEL_VIC21;

      RETROC64MODL=modl;
      if(retro_ui_finalized)
        vic20model_set(modl);
   }
#elif  defined(__PLUS4__)
   var.key = "vice_plus4_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "C16 PAL") == 0)modl=PLUS4MODEL_C16_PAL;
      else if (strcmp(var.value, "C16 NTSC") == 0)modl=PLUS4MODEL_C16_NTSC;
      else if (strcmp(var.value, "PLUS4 PAL") == 0)modl=PLUS4MODEL_PLUS4_PAL;
      else if (strcmp(var.value, "PLUS4 NTSC") == 0)modl=PLUS4MODEL_PLUS4_NTSC;
      else if (strcmp(var.value, "V364 NTSC") == 0)modl=PLUS4MODEL_V364_NTSC;
      else if (strcmp(var.value, "232 NTSC") == 0)modl=PLUS4MODEL_232_NTSC;

      RETROC64MODL=modl;
      if(retro_ui_finalized)
        plus4model_set(modl);
   }
#elif  defined(__X128__)
   var.key = "vice_c128_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "C128 PAL") == 0)modl=C128MODEL_C128_PAL;
      else if (strcmp(var.value, "C128DCR PAL") == 0)modl=C128MODEL_C128DCR_PAL;
      else if (strcmp(var.value, "C128 NTSC") == 0)modl=C128MODEL_C128_NTSC;
      else if (strcmp(var.value, "C128DCR NTSC") == 0)modl=C128MODEL_C128DCR_NTSC;

      RETROC64MODL=modl;
      if(retro_ui_finalized)
        c128model_set(modl);
   }
#elif  defined(__PET__)
   var.key = "vice_pet_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "2001") == 0)modl=PETMODEL_2001;
      else if (strcmp(var.value, "3008") == 0)modl=PETMODEL_3008;
      else if (strcmp(var.value, "3016") == 0)modl=PETMODEL_3016;
      else if (strcmp(var.value, "3032") == 0)modl=PETMODEL_3032;
      else if (strcmp(var.value, "3032B") == 0)modl=PETMODEL_3032B;
      else if (strcmp(var.value, "4016") == 0)modl=PETMODEL_4016;
      else if (strcmp(var.value, "4032") == 0)modl=PETMODEL_4032;
      else if (strcmp(var.value, "4032B") == 0)modl=PETMODEL_4032B;
      else if (strcmp(var.value, "8032") == 0)modl=PETMODEL_8032;
      else if (strcmp(var.value, "8096") == 0)modl=PETMODEL_8096;
      else if (strcmp(var.value, "8296") == 0)modl=PETMODEL_8296;
      else if (strcmp(var.value, "SUPERPET") == 0)modl=PETMODEL_SUPERPET;
      
      RETROC64MODL=modl;
      if(retro_ui_finalized)
        petmodel_set(modl);
   }
#elif  defined(__CBM2__)
   var.key = "vice_cbm2_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "510 PAL") == 0)modl=CBM2MODEL_510_PAL;
      else if (strcmp(var.value, "510 NTSC") == 0)modl=CBM2MODEL_510_NTSC;
      else if (strcmp(var.value, "610 PAL") == 0)modl=CBM2MODEL_610_PAL;
      else if (strcmp(var.value, "610 NTSC") == 0)modl=CBM2MODEL_610_NTSC;
      else if (strcmp(var.value, "620 PAL") == 0)modl=CBM2MODEL_620_PAL;
      else if (strcmp(var.value, "620 NTSC") == 0)modl=CBM2MODEL_620_NTSC;
      else if (strcmp(var.value, "620PLUS PAL") == 0)modl=CBM2MODEL_620PLUS_PAL;
      else if (strcmp(var.value, "620PLUS NTSC") == 0)modl=CBM2MODEL_620PLUS_NTSC;
      else if (strcmp(var.value, "710 NTSC") == 0)modl=CBM2MODEL_710_NTSC;
      else if (strcmp(var.value, "720 NTSC") == 0)modl=CBM2MODEL_720_NTSC;
      else if (strcmp(var.value, "720PLUS NTSC") == 0)modl=CBM2MODEL_720PLUS_NTSC;

      RETROC64MODL=modl;
      if(retro_ui_finalized)
        cbm2model_set(modl);
   }
#else
   var.key = "vice_c64_model";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int modl=0;

      if (strcmp(var.value, "C64 PAL") == 0)modl=C64MODEL_C64_PAL;
      else if (strcmp(var.value, "C64C PAL") == 0)modl=C64MODEL_C64C_PAL;
      //else if (strcmp(var.value, "C64 OLD PAL") == 0)modl=C64MODEL_C64_OLD_PAL;
      else if (strcmp(var.value, "C64 NTSC") == 0)modl=C64MODEL_C64_NTSC;
      else if (strcmp(var.value, "C64C NTSC") == 0)modl=C64MODEL_C64C_NTSC;
      //else if (strcmp(var.value, "C64 OLD NTSC") == 0)modl=C64MODEL_C64_OLD_NTSC;
      //else if (strcmp(var.value, "C64 PAL N") == 0)modl=C64MODEL_C64_PAL_N;
      else if (strcmp(var.value, "C64SX PAL") == 0)modl=C64MODEL_C64SX_PAL;
      else if (strcmp(var.value, "C64SX NTSC") == 0)modl=C64MODEL_C64SX_NTSC;
      else if (strcmp(var.value, "C64 JAP") == 0)modl=C64MODEL_C64_JAP;
      else if (strcmp(var.value, "C64 GS") == 0)modl=C64MODEL_C64_GS;
      else if (strcmp(var.value, "PET64 PAL") == 0)modl=C64MODEL_PET64_PAL;
      else if (strcmp(var.value, "PET64 NTSC") == 0)modl=C64MODEL_PET64_NTSC;

      RETROC64MODL=modl;
      if(retro_ui_finalized)
        c64model_set(modl);
   }
#endif

#if !defined(__PET__) && !defined(__CBM2__)
   var.key = "vice_border";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int border=0; /* 0 : normal, 1: full, 2: debug, 3: none */
      if (strcmp(var.value, "enabled") == 0)border=0;
      else if (strcmp(var.value, "disabled") == 0)border=3;

      RETROBORDERS=border;
      if(retro_ui_finalized)
#if defined(__VIC20__)
        resources_set_int("VICBorderMode", border);
#elif defined(__PLUS4__)
        resources_set_int("TEDBorderMode", border);
#else 
        resources_set_int("VICIIBorderMode", border);
#endif
   }
#endif

#if defined(__VIC20__)
   var.key = "vice_vic20_external_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char extpal[20] = "";
      if (strcmp(var.value, "Default") == 0)sprintf(extpal, "%s", "");
      else if (strcmp(var.value, "Mike NTSC") == 0)sprintf(extpal, "%s", "mike-ntsc");
      else if (strcmp(var.value, "Mike PAL") == 0)sprintf(extpal, "%s", "mike-pal");
      else if (strcmp(var.value, "Colodore VIC") == 0)sprintf(extpal, "%s", "colodore_vic");
      else if (strcmp(var.value, "Vice") == 0)sprintf(extpal, "%s", "vice");

      if(retro_ui_finalized){
         if(!*extpal) {
            resources_set_int("VICExternalPalette", 0);
         } else {
            resources_set_int("VICExternalPalette", 1);
            resources_set_string_sprintf("%sPaletteFile", extpal, "VIC");
         }
      } else {
         if(!*extpal) {
            RETROEXTPAL=-1;
         } else {
            RETROEXTPAL=1;
            sprintf(RETROEXTPALNAME, "%s", extpal);
         }
      }
   }
#elif defined(__PLUS4__)
   var.key = "vice_plus4_external_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char extpal[20] = "";
      if (strcmp(var.value, "Default") == 0)sprintf(extpal, "%s", "");
      else if (strcmp(var.value, "Yape PAL") == 0)sprintf(extpal, "%s", "yape-pal");
      else if (strcmp(var.value, "Yape NTSC") == 0)sprintf(extpal, "%s", "yape-ntsc");
      else if (strcmp(var.value, "Colodore TED") == 0)sprintf(extpal, "%s", "colodore_ted");

      if(retro_ui_finalized){
         if(!*extpal) {
            resources_set_int("TEDExternalPalette", 0);
         } else {
            resources_set_int("TEDExternalPalette", 1);
            resources_set_string_sprintf("%sPaletteFile", extpal, "TED");
         }
      } else {
         if(!*extpal) {
            RETROEXTPAL=-1;
         } else {
            RETROEXTPAL=1;
            sprintf(RETROEXTPALNAME, "%s", extpal);
         }
      }
   }
#elif defined(__PET__)
   var.key = "vice_pet_external_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char extpal[20] = "";
      if (strcmp(var.value, "Default") == 0)sprintf(extpal, "%s", "");
      else if (strcmp(var.value, "Amber") == 0)sprintf(extpal, "%s", "amber");
      else if (strcmp(var.value, "Green") == 0)sprintf(extpal, "%s", "green");
      else if (strcmp(var.value, "White") == 0)sprintf(extpal, "%s", "white");

      if(retro_ui_finalized){
         if(!*extpal) {
            resources_set_int("CrtcExternalPalette", 0);
         } else {
            resources_set_int("CrtcExternalPalette", 1);
            resources_set_string_sprintf("%sPaletteFile", extpal, "Crtc");
         }
      } else {
         if(!*extpal) {
            RETROEXTPAL=-1;
         } else {
            RETROEXTPAL=1;
            sprintf(RETROEXTPALNAME, "%s", extpal);
         }
      }
   }
#elif defined(__CBM2__)
   var.key = "vice_cbm2_external_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char extpal[20] = "";
      if (strcmp(var.value, "Default") == 0)sprintf(extpal, "%s", "");
      else if (strcmp(var.value, "Amber") == 0)sprintf(extpal, "%s", "amber");
      else if (strcmp(var.value, "Green") == 0)sprintf(extpal, "%s", "green");
      else if (strcmp(var.value, "White") == 0)sprintf(extpal, "%s", "white");

      if(retro_ui_finalized){
         if(!*extpal) {
            resources_set_int("CrtcExternalPalette", 0);
         } else {
            resources_set_int("CrtcExternalPalette", 1);
            resources_set_string_sprintf("%sPaletteFile", extpal, "Crtc");
         }
      } else {
         if(!*extpal) {
            RETROEXTPAL=-1;
         } else {
            RETROEXTPAL=1;
            sprintf(RETROEXTPALNAME, "%s", extpal);
         }
      }
   }
#else
   var.key = "vice_external_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char extpal[20] = "";
      if (strcmp(var.value, "Default") == 0)sprintf(extpal, "%s", "");
      else if (strcmp(var.value, "Pepto PAL") == 0)sprintf(extpal, "%s", "pepto-pal");
      else if (strcmp(var.value, "Pepto PAL old") == 0)sprintf(extpal, "%s", "pepto-palold");
      else if (strcmp(var.value, "Pepto NTSC Sony") == 0)sprintf(extpal, "%s", "pepto-ntsc-sony");
      else if (strcmp(var.value, "Pepto NTSC") == 0)sprintf(extpal, "%s", "pepto-ntsc");
      else if (strcmp(var.value, "Colodore") == 0)sprintf(extpal, "%s", "colodore");
      else if (strcmp(var.value, "Vice") == 0)sprintf(extpal, "%s", "vice");
      else if (strcmp(var.value, "C64HQ") == 0)sprintf(extpal, "%s", "c64hq");
      else if (strcmp(var.value, "C64S") == 0)sprintf(extpal, "%s", "c64s");
      else if (strcmp(var.value, "CCS64") == 0)sprintf(extpal, "%s", "cc64s");
      else if (strcmp(var.value, "Frodo") == 0)sprintf(extpal, "%s", "frodo");
      else if (strcmp(var.value, "Godot") == 0)sprintf(extpal, "%s", "godot");
      else if (strcmp(var.value, "PC64") == 0)sprintf(extpal, "%s", "pc64");
      else if (strcmp(var.value, "RGB") == 0)sprintf(extpal, "%s", "rgb");
      else if (strcmp(var.value, "Deekay") == 0)sprintf(extpal, "%s", "deekay");
      else if (strcmp(var.value, "Ptoing") == 0)sprintf(extpal, "%s", "ptoing");
      else if (strcmp(var.value, "Community Colors") == 0)sprintf(extpal, "%s", "community-colors");

      if(retro_ui_finalized){
         if(!*extpal) {
            resources_set_int("VICIIExternalPalette", 0);
         } else {
            resources_set_int("VICIIExternalPalette", 1);
            resources_set_string_sprintf("%sPaletteFile", extpal, "VICII");
         }
      } else {
         if(!*extpal) {
            RETROEXTPAL=-1;
         } else {
            RETROEXTPAL=1;
            sprintf(RETROEXTPALNAME, "%s", extpal);
         }
      }
   }
#endif

   var.key = "vice_userport_joytype";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {

      int joyadaptertype=-1;
      if (strcmp(var.value, "None") == 0)joyadaptertype=-1;
      else if (strcmp(var.value, "Protovision CGA") == 0)joyadaptertype=USERPORT_JOYSTICK_CGA;
      else if (strcmp(var.value, "PET") == 0)joyadaptertype=USERPORT_JOYSTICK_PET;
      else if (strcmp(var.value, "Hummer") == 0)joyadaptertype=USERPORT_JOYSTICK_HUMMER;
      else if (strcmp(var.value, "OEM") == 0)joyadaptertype=USERPORT_JOYSTICK_OEM;
      else if (strcmp(var.value, "Hit") == 0)joyadaptertype=USERPORT_JOYSTICK_HIT;
      else if (strcmp(var.value, "Kingsoft") == 0)joyadaptertype=USERPORT_JOYSTICK_KINGSOFT;
      else if (strcmp(var.value, "Starbyte") == 0)joyadaptertype=USERPORT_JOYSTICK_STARBYTE;

      if(retro_ui_finalized){
           if(joyadaptertype==-1)resources_set_int("UserportJoy", 0);
           else {
             resources_set_int("UserportJoy", 1);
             resources_set_int("UserportJoyType", joyadaptertype);
           }
      } else {
        RETROUSERPORTJOY=joyadaptertype;
      }
   }

   var.key = "vice_joyport";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "Port 2") == 0 && !cur_port_locked) cur_port=2;
      else if (strcmp(var.value, "Port 1") == 0 && !cur_port_locked) cur_port=1;
   }

   var.key = "vice_reset";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "Autostart") == 0)
         RETRORESET=0;
      else if (strcmp(var.value, "Soft") == 0)
         RETRORESET=1;
      else if (strcmp(var.value, "Hard") == 0)
         RETRORESET=2;
   }

   var.key = "vice_theme";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "C64") == 0)
         RETROTHEME=0;
      else if (strcmp(var.value, "C64 transparent") == 0)
         RETROTHEME=1;
      else if (strcmp(var.value, "C64C") == 0)
         RETROTHEME=2;
      else if (strcmp(var.value, "C64C transparent") == 0)
         RETROTHEME=3;
      else if (strcmp(var.value, "Transparent") == 0) 
         RETROTHEME=4;
   }

   var.key = "vice_mapper_select";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[2] = keyId(var.value);
   }

   var.key = "vice_mapper_start";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[3] = keyId(var.value);
   }

   var.key = "vice_mapper_y";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[1] = keyId(var.value);
   }

   var.key = "vice_mapper_x";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[9] = keyId(var.value);
   }

   var.key = "vice_mapper_b";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[0] = keyId(var.value);
   }

   var.key = "vice_mapper_l";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[10] = keyId(var.value);
   }

   var.key = "vice_mapper_r";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[11] = keyId(var.value);
   }

   var.key = "vice_mapper_l2";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[12] = keyId(var.value);
   }

   var.key = "vice_mapper_r2";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[13] = keyId(var.value);
   }

   var.key = "vice_mapper_l3";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[14] = keyId(var.value);
   }

   var.key = "vice_mapper_r3";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[15] = keyId(var.value);
   }



   var.key = "vice_mapper_lr";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[16] = keyId(var.value);
   }

   var.key = "vice_mapper_ll";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[17] = keyId(var.value);
   }

   var.key = "vice_mapper_ld";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[18] = keyId(var.value);
   }

   var.key = "vice_mapper_lu";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[19] = keyId(var.value);
   }

   var.key = "vice_mapper_rr";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[20] = keyId(var.value);
   }

   var.key = "vice_mapper_rl";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[21] = keyId(var.value);
   }

   var.key = "vice_mapper_rd";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[22] = keyId(var.value);
   }

   var.key = "vice_mapper_ru";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[23] = keyId(var.value);
   }


   var.key = "vice_mapper_vkbd";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[24] = keyId(var.value);
   }

   var.key = "vice_mapper_statusbar";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[25] = keyId(var.value);
   }

   var.key = "vice_mapper_joyport_switch";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[26] = keyId(var.value);
   }

   var.key = "vice_mapper_reset";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[27] = keyId(var.value);
   }

   var.key = "vice_mapper_warp_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[28] = keyId(var.value);
   }

   var.key = "vice_mapper_snapshot_save";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[29] = keyId(var.value);
   }

   var.key = "vice_mapper_snapshot_load";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[30] = keyId(var.value);
   }

   var.key = "vice_datasette_hotkeys";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)datasette=1;
      else if (strcmp(var.value, "disabled") == 0)datasette=0;
   }

   var.key = "vice_mapper_datasette_toggle_hotkeys";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[31] = keyId(var.value);
   }
   
   var.key = "vice_mapper_datasette_stop";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[32] = keyId(var.value);
   }

   var.key = "vice_mapper_datasette_start";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[33] = keyId(var.value);
   }

   var.key = "vice_mapper_datasette_forward";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[34] = keyId(var.value);
   }

   var.key = "vice_mapper_datasette_rewind";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[35] = keyId(var.value);
   }

   var.key = "vice_mapper_datasette_reset";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      mapper_keys[36] = keyId(var.value);
   }
   
}

void emu_reset(void)
{
   switch(RETRORESET) {
      case 0:
         autostart_autodetect(RPATH, NULL, 0, AUTOSTART_MODE_RUN);
         break;
      case 1:
         machine_trigger_reset(MACHINE_RESET_MODE_SOFT);
         break;
      case 2:
         machine_trigger_reset(MACHINE_RESET_MODE_HARD);
         break;
   }
}

void retro_reset(void)
{
   microSecCounter = 0;
   /* Retro reset should always autostart */
   autostart_autodetect(RPATH, NULL, 0, AUTOSTART_MODE_RUN);
}

struct DiskImage {
    char* fname;
};

#include <attach.h>

static bool retro_set_eject_state(bool ejected) {
	if (dc)
	{
		dc->eject_state = ejected;
		
		if(dc->eject_state)
			if(strendswith(dc->files[dc->index], "tap"))
			    tape_image_detach_internal(1);
			else
			    file_system_detach_disk(8);
            
		else
			if(strendswith(dc->files[dc->index], "tap"))
			    tape_image_attach(1, dc->files[dc->index]);
			else
			    file_system_attach_disk(8, dc->files[dc->index]);		
	}
	
	return true;
}

/* Gets current eject state. The initial state is 'not ejected'. */
static bool retro_get_eject_state(void) {
	if (dc)
		return dc->eject_state;
	
	return true;
}

/* Gets current disk index. First disk is index 0.
 * If return value is >= get_num_images(), no disk is currently inserted.
 */
static unsigned retro_get_image_index(void) {
	if (dc)
		return dc->index;
	
	return 0;
}

/* Sets image index. Can only be called when disk is ejected.
 * The implementation supports setting "no disk" by using an
 * index >= get_num_images().
 */
static bool retro_set_image_index(unsigned index) {
	// Insert disk
	if (dc)
	{
		// Same disk...
		// This can mess things in the emu
		if(index == dc->index)
			return true;
		
		if ((index < dc->count) && (dc->files[index]))
		{
			dc->index = index;
			if(strendswith(dc->files[dc->index], "tap"))
			    log_cb(RETRO_LOG_INFO, "Tape (%d) inserted into datasette: %s\n", dc->index+1, dc->files[dc->index]);
			else 
			    log_cb(RETRO_LOG_INFO, "Disk (%d) inserted into drive 8: %s\n", dc->index+1, dc->files[dc->index]);
			return true;
		}
	}
	
	return false;
}

/* Gets total number of images which are available to use. */
static unsigned retro_get_num_images(void) {
	if (dc)
		return dc->count;

	return 0;
}


/* Replaces the disk image associated with index.
 * Arguments to pass in info have same requirements as retro_load_game().
 * Virtual disk tray must be ejected when calling this.
 *
 * Replacing a disk image with info = NULL will remove the disk image
 * from the internal list.
 * As a result, calls to get_image_index() can change.
 *
 * E.g. replace_image_index(1, NULL), and previous get_image_index()
 * returned 4 before.
 * Index 1 will be removed, and the new index is 3.
 */
static bool retro_replace_image_index(unsigned index,
      const struct retro_game_info *info) {
	if (dc)
	{
		if(dc->files[index])
		{
			free(dc->files[index]);
			dc->files[index] = NULL;
		}
		
		// TODO : Handling removing of a disk image when info = NULL
		
		if(info != NULL) {
			dc->files[index] = strdup(info->path);
		}
	}
	
    return false;	
}

/* Adds a new valid index (get_num_images()) to the internal disk list.
 * This will increment subsequent return values from get_num_images() by 1.
 * This image index cannot be used until a disk image has been set
 * with replace_image_index. */
static bool retro_add_image_index(void) {
	if (dc)
	{
		if(dc->count <= DC_MAX_SIZE)
		{
			dc->files[dc->count] = NULL;
			dc->count++;
			return true;
		}
	}
	
    return false;
}

static struct retro_disk_control_callback diskControl = {
    retro_set_eject_state,
    retro_get_eject_state,
    retro_get_image_index,
    retro_set_image_index,
    retro_get_num_images,
    retro_replace_image_index,
    retro_add_image_index,
};

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   /* stub */
}

void retro_init(void)
{
   struct retro_log_callback log;

	// Init disk control context
   	dc = dc_create();

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

   const char *system_dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
   {
      // if defined, use the system directory
      retro_system_directory=system_dir;
   }

   const char *content_dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir)
   {
      // if defined, use the system directory
      retro_content_directory=content_dir;
   }

   const char *save_dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
   {
      // If save directory is defined use it, otherwise use system directory
      retro_save_directory = *save_dir ? save_dir : retro_system_directory;
   }
   else
   {
      // make retro_save_directory the same in case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY is not implemented by the frontend
      retro_save_directory=retro_system_directory;
   }

   if(retro_system_directory==NULL)
   {
#if defined(__ANDROID__) || defined(ANDROID)
      strcpy(RETRO_DIR, "/mnt/sdcard");
#elif defined(VITA)
      strcpy(RETRO_DIR, "ux0:/data");
#elif defined(__SWITCH__)
      strcpy(RETRO_DIR, "/");
#else
      strcpy(RETRO_DIR, ".");
#endif
   }
   else
      sprintf(RETRO_DIR, "%s", retro_system_directory);

   /* Use system directory for data files such as C64/.vpl etc. */
#if defined(__WIN32__)
   snprintf(retro_system_data_directory, sizeof(retro_system_data_directory), "%s\\vice", RETRO_DIR);
#else
   snprintf(retro_system_data_directory, sizeof(retro_system_data_directory), "%s/vice", RETRO_DIR);
#endif

   archdep_mkdir(retro_system_data_directory, 0);

#ifdef FRONTEND_SUPPORTS_RGB565
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#endif

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "PIXEL FORMAT is not supported.\n");
      exit(0);
   }

#define RETRO_DESCRIPTOR_BLOCK( _user )                                            \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },     \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },       \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },       \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },         \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },             \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },         \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },               \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2" },             \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2" },             \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3" },             \
   { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3" },			   \
   { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" },			   \
   { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" },			   \
   { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" },			   \
   { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" }
   
   static struct retro_input_descriptor inputDescriptors[] =
   {
      RETRO_DESCRIPTOR_BLOCK( 0 ),
      RETRO_DESCRIPTOR_BLOCK( 1 ),
      RETRO_DESCRIPTOR_BLOCK( 2 ),
      RETRO_DESCRIPTOR_BLOCK( 3 ),
      RETRO_DESCRIPTOR_BLOCK( 4 ),

      { 0 },
   };

#undef RETRO_DESCRIPTOR_BLOCK

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &inputDescriptors);
   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &diskControl);

   static uint32_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE | RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE | RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
   environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);

   microSecCounter = 0;
}

void retro_deinit(void)
{
   /* Clean the disk control context */
   if(dc)
      dc_free(dc);
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device( unsigned port, unsigned device )
{
   if ( port < 5 )
      vice_devices[port] = device;
}

void retro_get_system_info(struct retro_system_info *info)
{
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   memset(info, 0, sizeof(*info));
   info->library_name     = "VICE " CORE_NAME;
   info->library_version  = "3.3" GIT_VERSION;
#if defined(__VIC20__)
   info->valid_extensions = "20|40|60|a0|b0|d64|d71|d80|d81|d82|g64|g41|x64|t64|tap|prg|p00|crt|bin|zip|gz|d6z|d7z|d8z|g6z|g4z|x6z|cmd|m3u";
#else
   info->valid_extensions = "d64|d71|d80|d81|d82|g64|g41|x64|t64|tap|prg|p00|crt|bin|zip|gz|d6z|d7z|d8z|g6z|g4z|x6z|cmd|m3u";
#endif
   info->need_fullpath    = true;
   info->block_extract    = false;

}

void update_geometry()
{
   struct retro_system_av_info system_av_info;
   system_av_info.geometry.base_width = retroW;
   system_av_info.geometry.base_height = retroH;
   if (retro_get_borders())
     // When borders are disabled, each system has a different aspect ratio.
     // For example, C64 & C128 have 320 / 200 pixel resolution with a 15 / 16
     // pixel aspect ratio leading to a total aspect of 320 / 200 * 15 / 16 = 1.5
#if defined(__VIC20__)
     system_av_info.geometry.aspect_ratio = (float)1.6;
#elif defined(__PLUS4__)
     system_av_info.geometry.aspect_ratio = (float)1.65;
#elif defined(__PET__)
     system_av_info.geometry.aspect_ratio = (float)4.0/3.0;
#else
     system_av_info.geometry.aspect_ratio = (float)1.5;
#endif
   else
     system_av_info.geometry.aspect_ratio = (float)4.0/3.0;
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &system_av_info);
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.max_width = retrow;
   info->geometry.max_height = retroh;

   info->timing.sample_rate = 44100.0;

   switch(retro_get_region()) {
      case RETRO_REGION_PAL:
         info->geometry.base_width = 384;
         info->geometry.base_height = 272;
         info->geometry.aspect_ratio = 4.0 / 3.0;
         info->timing.fps = C64_PAL_RFSH_PER_SEC;
         break;
      
      case RETRO_REGION_NTSC:
         info->geometry.base_width = 384;
         info->geometry.base_height = 247;
         info->geometry.aspect_ratio = 4.0 / 3.0;
         info->timing.fps = C64_NTSC_RFSH_PER_SEC;
         break;
   }
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_audio_cb( short l, short r)
{
   audio_cb(l,r);
}

void retro_audiocb(signed short int *sound_buffer,int sndbufsize){
   int x;
   for(x=0;x<sndbufsize;x++)audio_cb(sound_buffer[x],sound_buffer[x]);
}

void retro_blit(void)
{
   memcpy(Retro_Screen,bmp,PITCH*WINDOW_SIZE);
}

void retro_run(void)
{
   bool updated = false;

   if(lastW!=retroW || lastH!=retroH){
      update_geometry();
      log_cb(RETRO_LOG_INFO, "Update Geometry Old(%d,%d) New(%d,%d)\n",lastW,lastH,retroW,retroH);
      lastW=retroW;
      lastH=retroH;
      reset_mouse_pos();
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   if(runstate == RUNSTATE_FIRST_START)
   {
      /* this is only done once after just loading the core from scratch and starting it */
      log_cb(RETRO_LOG_INFO, "First time we return from retro_run()!\n");
      retro_load_ok=true;
      app_init();
      memset(SNDBUF,0,1024*2*2);
      update_variables();
      pre_main(RPATH);
      runstate = RUNSTATE_RUNNING;
      return;
   } 
   else if (runstate == RUNSTATE_LOADED_CONTENT)
   {
      /* Load content was called while core was already running, just do a reset with autostart */
      autostart_autodetect(RPATH, NULL, 0, AUTOSTART_MODE_RUN);
      /* After retro_load_game, get_system_av_info is always called by the frontend */
      /* resetting the aspect to 4/3 etc. So we inform the frontend of the actual */
      /* current aspect ratio and screen size again here */
      update_geometry();
      runstate = RUNSTATE_RUNNING;
   } 

   while(cpuloop==1)
      maincpu_mainloop_retro();
   cpuloop=1;

   retro_blit();
   if(SHOWKEY==1)app_render(0);

   video_cb(Retro_Screen,retroW,retroH,retrow<<PIXEL_BYTES);

   microSecCounter += (1000000/(retro_get_region() == RETRO_REGION_NTSC ? C64_NTSC_RFSH_PER_SEC : C64_PAL_RFSH_PER_SEC));
   
   if (REQUEST_SNAPSHOT_SAVE)
   {
      snprintf(RPATH_basename, sizeof(RPATH_basename), "%s", path_basename(RPATH));
      snprintf(save_file, sizeof(save_file), "%s%s%s.vsf", retro_save_directory, FSDEV_DIR_SEP_STR, RPATH_basename);
      log_message(LOG_DEFAULT, "Saving snapshot: %s", save_file);
      if (machine_write_snapshot(save_file, 0, 1, 0) < 0) { /* filename, save_roms, save_disks, event_mode */
          snapshot_display_error();
      }
      REQUEST_SNAPSHOT_SAVE=0;
   }
   else if (REQUEST_SNAPSHOT_LOAD)
   {
      snprintf(RPATH_basename, sizeof(RPATH_basename), "%s", path_basename(RPATH));
      snprintf(save_file, sizeof(save_file), "%s%s%s.vsf", retro_save_directory, FSDEV_DIR_SEP_STR, RPATH_basename);
      log_message(LOG_DEFAULT, "Loading snapshot: %s", save_file);
      if (machine_read_snapshot(save_file, 0) < 0) {
          snapshot_display_error();
      }
      REQUEST_SNAPSHOT_LOAD=0;
   }
}

#define M3U_FILE_EXT "m3u"

bool retro_load_game(const struct retro_game_info *info)
{
   if (info)
   {
	const char *full_path;

	(void)info;

	full_path = info->path;

	// If it's a m3u file
	if(strendswith(full_path, M3U_FILE_EXT))
	{
		// Parse the m3u file
		dc_parse_m3u(dc, full_path);

		// Some debugging
		log_cb(RETRO_LOG_INFO, "m3u file parsed, %d file(s) found\n", dc->count);
		for(unsigned i = 0; i < dc->count; i++)
		{
			log_cb(RETRO_LOG_INFO, "file %d: %s\n", i+1, dc->files[i]);
		}	
	}
	else
	{
		// Add the file to disk control context
		// Maybe, in a later version of retroarch, we could add disk on the fly (didn't find how to do this)
		dc_add_file(dc, full_path);
	}

	// Init first disk
	dc->index = 0;
	dc->eject_state = false;
	if(strendswith(dc->files[dc->index], "tap"))
	    log_cb(RETRO_LOG_INFO, "Tape (%d) inserted into datasette: %s\n", dc->index+1, dc->files[dc->index]);
    else 
	    log_cb(RETRO_LOG_INFO, "Disk (%d) inserted into drive 8: %s\n", dc->index+1, dc->files[dc->index]);
	    
	strcpy(RPATH,dc->files[0]);
   }
   else
   {
      RPATH[0]=0;
   }

   cur_port_locked = 0;

   if (strlen(RPATH) >= strlen("j1"))
     if (strstr(RPATH, "_j1.") || strstr(RPATH, "(j1).") || strstr(RPATH, "_J1.") || strstr(RPATH, "(J1)."))
     {
        cur_port = 1;
        cur_port_locked = 1;
     }

   if (strlen(RPATH) >= strlen("j2"))
     if (strstr(RPATH, "_j2.") || strstr(RPATH, "(j2).") || strstr(RPATH, "_J2.") || strstr(RPATH, "(J2)."))
     {
        cur_port = 2;
        cur_port_locked = 1;
     }

   update_variables();
   
   if (runstate == RUNSTATE_RUNNING) {
      /* load game was called while core is already running */
      /* so we update runstate and do the deferred autostart_reset in retro_run */
      /* the autostart_reset has to be deferred because a bunch of other init stuff */
      /* is done between retro_load_game() and retro_run() at a higher level */
      runstate = RUNSTATE_LOADED_CONTENT;
   }

   return true;
}

void retro_unload_game(void){
    file_system_detach_disk(8);
    tape_image_detach_internal(1);
#ifndef __PET__
    cartridge_detach_image(-1);
#endif
    RPATH[0] = 0;
}

unsigned retro_get_region(void)
{
#if defined(__PET__)
   return RETRO_REGION_PAL;
#else
   switch(RETROC64MODL) {
#if defined(__VIC20__)
      case VIC20MODEL_VIC20_NTSC:
#elif defined(__CBM2__)
      case CBM2MODEL_510_NTSC:
      case CBM2MODEL_610_NTSC:
      case CBM2MODEL_620_NTSC:
      case CBM2MODEL_620PLUS_NTSC:
      case CBM2MODEL_710_NTSC:
      case CBM2MODEL_720_NTSC:
      case CBM2MODEL_720PLUS_NTSC:
#elif defined(__PLUS4__)
      case PLUS4MODEL_C16_NTSC:
      case PLUS4MODEL_PLUS4_NTSC:
      case PLUS4MODEL_V364_NTSC:
      case PLUS4MODEL_232_NTSC:
#elif defined(__X128__)
      case C128MODEL_C128_NTSC:
      case C128MODEL_C128DCR_NTSC:
#else
      case C64MODEL_C64_NTSC:
      case C64MODEL_C64C_NTSC:
      case C64MODEL_C64_OLD_NTSC:
      case C64MODEL_C64SX_NTSC:
      case C64MODEL_PET64_NTSC:
#endif
         return RETRO_REGION_NTSC;
         break;
      default:
         return RETRO_REGION_PAL;
         break;
   }
#endif /* __PET__ */
}

unsigned retro_get_borders(void) {
   return RETROBORDERS;
} 

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   if (retro_ui_finalized)
   {
      size_t size = 0;
      snprintf(save_file, sizeof(save_file), "%s%svice_tempsave.vsf", retro_save_directory, FSDEV_DIR_SEP_STR);
      if (machine_write_snapshot(save_file, 0, 1, 0) >= 0) /* filename, save_roms, save_disks, event_mode */
      {
         FILE *file = fopen(save_file, "rb");
         if (file)
         {
            fseek(file, 0L, SEEK_END);
            size = ftell(file);
            fclose(file);
            return size;
         }
      }
   }
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   if (retro_ui_finalized)
   {
      snprintf(save_file, sizeof(save_file), "%s%svice_tempsave.vfs", retro_save_directory, FSDEV_DIR_SEP_STR);
      if (machine_write_snapshot(save_file, 0, 1, 0) >= 0) /* filename, save_roms, save_disks, event_mode */
      {
         FILE *file = fopen(save_file, "rb");
         if (file)
         {
            if (fread(data_, size, 1, file) == 1)
            {
               fclose(file);
               return true;
            }
            fclose(file);
         }
      }
   }
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   if (retro_ui_finalized)
   {
      snprintf(save_file, sizeof(save_file), "%s%svice_tempsave.vfs", retro_save_directory, FSDEV_DIR_SEP_STR);
      FILE *file = fopen(save_file, "wb");
      if (file)
      {
         if (fwrite(data_, size, 1, file) == 1)
         {
            fclose(file);
            if (machine_read_snapshot(save_file, 0) >= 0)
               return true;
         }
         else
            fclose(file);
      }
   }
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return mem_ram;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return g_mem_ram_size;
   return 0;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

