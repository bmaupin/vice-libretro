#include "libretro.h"
#include "joystick.h"
#include "keyboard.h"
#include "machine.h"
#include "mouse.h"
#include "resources.h"
#include "autostart.h"
#include "datasette.h"
#include "snapshot.h"

#include "kbd.h"
#include "mousedrv.h"
#include "libretro-core.h"
#include "archdep.h"

extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;

extern void emu_reset(void);
extern void save_bkg();
extern void Screen_SetFullUpdate(int scr);
extern unsigned vice_devices[5];
extern int cpupaused;

//EMU FLAGS
int SHOWKEY=-1;
int SHIFTON=-1;
int SND=1;
int vkey_pressed=-1;
char core_key_state[512];
char core_old_key_state[512];
int PAS=4;
int slowdown=0;
unsigned int cur_port=2;
unsigned int datasette=0;
bool num_locked = false;

extern bool retro_load_ok;
extern int mapper_keys[37];
int REQUEST_SNAPSHOT_SAVE=0;
int REQUEST_SNAPSHOT_LOAD=0;
int statusbar=0;

#define EMU_VKBD 1
#define EMU_STATUSBAR 2
#define EMU_JOYPORT 3
#define EMU_RESET 4
#define EMU_WARP_ON 5
#define EMU_WARP_OFF 6
#define EMU_SNAPSHOT_SAVE 7
#define EMU_SNAPSHOT_LOAD 8
#define EMU_DATASETTE_TOGGLE_HOTKEYS 9
#define EMU_DATASETTE_STOP 10
#define EMU_DATASETTE_START 11
#define EMU_DATASETTE_FORWARD 12
#define EMU_DATASETTE_REWIND 13
#define EMU_DATASETTE_RESET 14

void emu_function(int function) {
    switch (function)
    {
        case EMU_VKBD:
            SHOWKEY=-SHOWKEY;
            break;
        case EMU_STATUSBAR:
            resources_get_int("SDLStatusbar", &statusbar);
            statusbar = (statusbar) ? 0 : 1;
            resources_set_int("SDLStatusbar", statusbar);
            break;
        case EMU_JOYPORT:
            cur_port++;
            if (cur_port>2) cur_port = 1;
            break;
        case EMU_RESET:
            emu_reset();
            break;
        case EMU_WARP_ON:
            resources_set_int("WarpMode", 1);
            break;
        case EMU_WARP_OFF:
            resources_set_int("WarpMode", 0);
            break;

        case EMU_SNAPSHOT_SAVE:
            REQUEST_SNAPSHOT_SAVE=1;
            break;
        case EMU_SNAPSHOT_LOAD:
            REQUEST_SNAPSHOT_LOAD=1;
            break;
        
        case EMU_DATASETTE_TOGGLE_HOTKEYS:
            datasette = (datasette) ? 0 : 1;
            break;
        case EMU_DATASETTE_STOP:
            datasette_control(DATASETTE_CONTROL_STOP);
            break;
        case EMU_DATASETTE_START:
            datasette_control(DATASETTE_CONTROL_START);
            break;
        case EMU_DATASETTE_FORWARD:
            datasette_control(DATASETTE_CONTROL_FORWARD);
            break;
        case EMU_DATASETTE_REWIND:
            datasette_control(DATASETTE_CONTROL_REWIND);
            break;
        case EMU_DATASETTE_RESET:
            datasette_control(DATASETTE_CONTROL_RESET);
            break;
    } 
}

void Keymap_KeyUp(int symkey)
{
    if (symkey == RETROK_NUMLOCK)
        num_locked = false;
    else 
        kbd_handle_keyup(symkey);
}

void Keymap_KeyDown(int symkey)
{
    if (symkey == RETROK_NUMLOCK)
        num_locked = true;
    else
        kbd_handle_keydown(symkey);
}

void app_vkb_handle(void)
{

    static int last_vkey_pressed = -1;

    /* key up */
    if(vkey_pressed == -1 && last_vkey_pressed >= 0)
        kbd_handle_keyup(last_vkey_pressed);

    /* key down */
    if (vkey_pressed != -1 && last_vkey_pressed == -1)
    {
        switch (vkey_pressed)
        {
            case -4:
                emu_function(EMU_JOYPORT);
                break;
            case -5:
                emu_function(EMU_STATUSBAR);
                break;
            case -10: /* sticky shift */
                if(SHIFTON == 1)
                    kbd_handle_keyup(RETROK_LSHIFT);
                else
                    kbd_handle_keydown(RETROK_LSHIFT);
                SHIFTON=-SHIFTON;
                break;
            default:
                kbd_handle_keydown(vkey_pressed);
                break;
        }
    }
    last_vkey_pressed = vkey_pressed;
}

// Core input Key(not GUI) 
void Core_Processkey(int disable_physical_cursor_keys)
{
   int i;

   for(i=0; i<320; i++)
      core_key_state[i]=input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0,i) ? 0x80: 0;

   if (memcmp(core_key_state, core_old_key_state, sizeof(core_key_state)))
   {
      for(i=0; i<320; i++)
      {
         if(core_key_state[i] && core_key_state[i]!=core_old_key_state[i])
         {	
            if(i==RETROK_LALT)
               continue;
            if (disable_physical_cursor_keys && (i == RETROK_DOWN || i == RETROK_UP || i == RETROK_LEFT || i == RETROK_RIGHT))
               continue;
            Keymap_KeyDown(i);
         }
         else if (!core_key_state[i] && core_key_state[i] != core_old_key_state[i])
         {
            if(i==RETROK_LALT)
               continue;
            if (disable_physical_cursor_keys && (i == RETROK_DOWN || i == RETROK_UP || i == RETROK_LEFT || i == RETROK_RIGHT))
               continue;
            Keymap_KeyUp(i);

         }
      }
   }
   memcpy(core_old_key_state, core_key_state, sizeof(core_key_state));
}

// Core input (not GUI) 
int Core_PollEvent(int disable_physical_cursor_keys)
{
    //   RETRO        B    Y    SLT  STA  UP   DWN  LEFT RGT  A    X    L    R    L2   R2   L3   R3  LR  LL  LD  LU  RR  RL  RD  RU
    //   INDEX        0    1    2    3    4    5    6    7    8    9    10   11   12   13   14   15  16  17  18  19  20  21  22  23

    int i, mk;
    static int jbt[24]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    static int kbt[11]={0,0,0,0,0,0,0,0,0,0};
    
    if(!retro_load_ok)return 1;
    input_poll_cb();

    int mouse_l;
    int mouse_r;
    int16_t mouse_x,mouse_y;
    mouse_x=mouse_y=0;
    
    int LX, LY, RX, RY;
    int threshold=20000;

    /* Iterate hotkeys, skip datasette control if not enabled */
    int imax = (datasette) ? 13 : 8;
    /* Skip datasette hotkeys if vkbd is on */
    imax = (SHOWKEY==1) ? 8 : imax;
    
    for(i = 0; i < imax; i++) {
        mk = i + 24;
        
        if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, mapper_keys[mk]) && kbt[i]==0 && mapper_keys[mk]!=0)
        {
            kbt[i]=1;
            switch(mk) {
                case 24:
                    emu_function(EMU_VKBD);
                    break;
                case 25:
                    emu_function(EMU_STATUSBAR);
                    break;
                case 26:
                    emu_function(EMU_JOYPORT);
                    break;
                case 27:
                    emu_function(EMU_RESET);
                    break;
                case 28:
                    emu_function(EMU_WARP_ON);
                    break;

                case 31:
                    emu_function(EMU_DATASETTE_TOGGLE_HOTKEYS);
                    break;
                case 32:
                    emu_function(EMU_DATASETTE_STOP);
                    break;
                case 33:
                    emu_function(EMU_DATASETTE_START);
                    break;
                case 34:
                    emu_function(EMU_DATASETTE_FORWARD);
                    break;
                case 35:
                    emu_function(EMU_DATASETTE_REWIND);
                    break;
                case 36:
                    emu_function(EMU_DATASETTE_RESET);
                    break;
            }
        }
        else if ( kbt[i]==1 && ! input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, mapper_keys[mk]) && mapper_keys[mk]!=0)
        {
            kbt[i]=0;
            switch(mk) {
                case 28:
                    emu_function(EMU_WARP_OFF);
                    break;

                case 29:
                    emu_function(EMU_SNAPSHOT_SAVE);
                    break;
                case 30:
                    emu_function(EMU_SNAPSHOT_LOAD);
                    break;
            }
        }
    }

    /* The check for kbt[i] here prevents the hotkey from generating C64 key events */
    if(SHOWKEY==-1 && kbt[0] == 0 && kbt[1] == 0 && kbt[2] == 0 && kbt[3] == 0 && kbt[4] == 0)
        Core_Processkey(disable_physical_cursor_keys);

    if (vice_devices[0] == RETRO_DEVICE_VICE_JOYSTICK || vice_devices[0] == RETRO_DEVICE_JOYPAD)
    {
        LX = input_state_cb(0, RETRO_DEVICE_ANALOG, 0, 0);
        LY = input_state_cb(0, RETRO_DEVICE_ANALOG, 0, 1);
        RX = input_state_cb(0, RETRO_DEVICE_ANALOG, 1, 0);
        RY = input_state_cb(0, RETRO_DEVICE_ANALOG, 1, 1);

        /* shortcut for joy mode only */
        for(i = 0; i < 24; i++)
        {
            int just_pressed = 0;
            int just_released = 0;
            if((i<4 || i>8) && i < 16) /* remappable retropad buttons (all apart from DPAD and A) */
            {
                if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) && jbt[i]==0)
                    just_pressed = 1;
                else if (jbt[i]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
                    just_released = 1;
            }
            else if (i >= 16) /* remappable retropad joystick directions */
            {
                switch (i)
                {
                    case 16: /* LR */
                        if (LX > threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (LX < threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 17: /* LL */
                        if (LX < -threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (LX > -threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 18: /* LD */
                        if (LY > threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (LY < threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 19: /* LU */
                        if (LY < -threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (LY > -threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 20: /* RR */
                        if (RX > threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (RX < threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 21: /* RL */
                        if (RX < -threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (RX > -threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 22: /* RD */
                        if (RY > threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (RY < threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    case 23: /* RU */
                        if (RY < -threshold && jbt[i] == 0)
                            just_pressed = 1;
                        else if (RY > -threshold && jbt[i] == 1)
                            just_released = 1;
                        break;
                    default:
                        break;
                }
            }

            if (just_pressed)
            {
                jbt[i] = 1;
                if(mapper_keys[i] == 0) /* unmapped, e.g. set to "---" in core options */
                    continue;

                if(mapper_keys[i] == mapper_keys[24]) /* Virtual keyboard */
                    emu_function(EMU_VKBD);
                else if(mapper_keys[i] == mapper_keys[25]) /* Statusbar */
                    emu_function(EMU_STATUSBAR);
                else if(mapper_keys[i] == mapper_keys[26]) /* Switch joyport */
                    emu_function(EMU_JOYPORT);
                else if(mapper_keys[i] == mapper_keys[27]) /* Reset */
                    emu_function(EMU_RESET);
                else if(mapper_keys[i] == mapper_keys[28]) /* Warp mode */
                    emu_function(EMU_WARP_ON);
                else if(mapper_keys[i] == mapper_keys[29]) /* Snapshot save */
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[30]) /* Snapshot load */
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[31]) /* Datasette toggle */
                    emu_function(EMU_DATASETTE_TOGGLE_HOTKEYS);
                else if(datasette && mapper_keys[i] == mapper_keys[32]) /* Datasette stop */
                    emu_function(EMU_DATASETTE_STOP);
                else if(datasette && mapper_keys[i] == mapper_keys[33]) /* Datasette start */
                    emu_function(EMU_DATASETTE_START);
                else if(datasette && mapper_keys[i] == mapper_keys[34]) /* Datasette forward */
                    emu_function(EMU_DATASETTE_FORWARD);
                else if(datasette && mapper_keys[i] == mapper_keys[35]) /* Datasette rewind */
                    emu_function(EMU_DATASETTE_REWIND);
                else if(datasette && mapper_keys[i] == mapper_keys[36]) /* Datasette reset */
                    emu_function(EMU_DATASETTE_RESET);
                else
                    Keymap_KeyDown(mapper_keys[i]);
            }
            else if (just_released)
            {
                jbt[i] = 0;
                if(mapper_keys[i] == 0) /* unmapped, e.g. set to "---" in core options */
                    continue;

                if(mapper_keys[i] == mapper_keys[24])
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[25])
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[26])
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[27])
                    ; /* nop */
                else if(mapper_keys[i] == mapper_keys[28])
                    emu_function(EMU_WARP_OFF);
                else if(mapper_keys[i] == mapper_keys[29])
                    emu_function(EMU_SNAPSHOT_SAVE);
                else if(mapper_keys[i] == mapper_keys[30])
                    emu_function(EMU_SNAPSHOT_LOAD);
                else if(mapper_keys[i] == mapper_keys[31])
                    ; /* nop */
                else if(datasette && mapper_keys[i] == mapper_keys[32])
                    ; /* nop */
                else if(datasette && mapper_keys[i] == mapper_keys[33])
                    ; /* nop */
                else if(datasette && mapper_keys[i] == mapper_keys[34])
                    ; /* nop */
                else if(datasette && mapper_keys[i] == mapper_keys[35])
                    ; /* nop */
                else if(datasette && mapper_keys[i] == mapper_keys[36])
                    ; /* nop */
                else
                    Keymap_KeyUp(mapper_keys[i]);
            }
        } /* for i */
    } /* if vice_devices[0]==joypad or joystick */

    return 1;
}

void retro_poll_event()
{
    /* if user plays with cursor keys, then prevent up/down/left/right from generating */
    /* keyboard key presses, this prevent cursor up from becoming a run/stop input */
    if (
        (vice_devices[0] == RETRO_DEVICE_VICE_JOYSTICK || vice_devices[0] == RETRO_DEVICE_JOYPAD) &&
        (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) ||
         input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) ||
         input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) ||
         input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
    )
        Core_PollEvent(1); /* Process all inputs but disable cursor keys */
    else
        Core_PollEvent(0); /* Process all inputs */

    //if(SHOWKEY==-1) /* retro joypad take control over keyboard joy */
    /* override keydown, but allow keyup, to prevent key sticking during keyboard use, if held down on opening keyboard */
    /* keyup allowing most likely not needed on actual keyboard presses even though they get stuck also */
    {
        int retro_port;
        for (retro_port = 0; retro_port <= 4; retro_port++)
        {
            if (vice_devices[retro_port] == RETRO_DEVICE_VICE_JOYSTICK || vice_devices[retro_port] == RETRO_DEVICE_JOYPAD)
            {
                int vice_port = cur_port;
                BYTE j = 0;

                if (retro_port == 1) /* second joypad controls other player */
                {
                    if (cur_port == 2)
                        vice_port = 1;
                    else
                        vice_port = 2;
                }
                else if (retro_port == 2)
                    vice_port = 3;
                else if (retro_port == 3)
                    vice_port = 4;
                else if (retro_port == 4)
                    vice_port = 5;

                j = joystick_value[vice_port];

                if (input_state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) ||
                    (vice_port < 3 && vice_port != cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP9)) ||
                    (vice_port < 3 && vice_port == cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP8)) 			
                )
                    j |= (SHOWKEY==-1) ? 0x01 : j;
                else
                    j &= ~0x01;

                if (input_state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) || 
                    (vice_port < 3 && vice_port != cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP3)) ||
                    (vice_port < 3 && vice_port == cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP2)) 			
                )
                    j |= (SHOWKEY==-1) ? 0x02 : j;
                else
                    j &= ~0x02;

                if (input_state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) || 
                    (vice_port < 3 && vice_port != cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP7)) ||
                    (vice_port < 3 && vice_port == cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP4)) 
                )
                    j |= (SHOWKEY==-1) ? 0x04 : j;
                else
                    j &=~ 0x04;

                if (input_state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) || 
                    (vice_port < 3 && vice_port != cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP1)) ||
                    (vice_port < 3 && vice_port == cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP6)) 			    
                )
                    j |= (SHOWKEY==-1) ? 0x08 : j;
                else
                    j &= ~0x08;
                    
                if (input_state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) || 
                    (vice_port < 3 && vice_port != cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP0)) ||
                    (vice_port < 3 && vice_port == cur_port && input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP5)) 
                )
                    j |= (SHOWKEY==-1) ? 0x10 : j;
                else
                    j &= ~0x10;

                joystick_value[vice_port] = j;
            }
        }
    }
}

