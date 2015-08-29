#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include <3ds.h>

#include "allegro_compat.h"
#include "vb_dsp.h"
#include "vb_set.h"

// The max number of items that can fit in the screen
#define MAX_ITEMS 28
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

// Graphics stuff

void masked_blit(BITMAP *src, BITMAP *dst, int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (src->line[(src_y+y)%(src->h)][(src_x+x)%(src->w)]) {
                dst->line[(dst_y+y)%(dst->h)][(dst_x+x)%(dst->w)] = src->line[(src_y+y)%(src->h)][(src_x+x)%(src->w)];
            }
        }
    }
}

void masked_stretch_blit(BITMAP *src, BITMAP *dst, int src_x, int src_y, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
    int x, y, xpos, ypos;
    float xratio = ((float) src_w) / ((float) dst_w);
    float yratio = ((float) src_h) / ((float) dst_h);

    for (y = 0; y < dst_h; y++) {
        for (x = 0; x < dst_w; x++) {
            xpos = ((int) ((x + src_x) * xratio)) % src->w;
            ypos = ((int) ((y + src_y) * yratio)) % src->h;
            if (src->line[ypos][xpos]) {
                dst->line[(dst_y+y)%dst->h][(dst_x+x)%dst->w] = src->line[ypos][xpos];
            }
        }
    }
}

// Mostly taken from allegro
BITMAP *create_bitmap(int w, int h) {
    int nr_pointers;
    int padding;
    int i;

    // We need at least two pointers when drawing, otherwise we get crashes with
    // Electric Fence. We think some of the assembly code assumes a second line
    // pointer is always available.
    nr_pointers = ((h>2) ? h : 2) ;

    // Padding avoids a crash for assembler code accessing the last pixel, as it
    // read 4 bytes instead of 3.
    padding = 1;

    BITMAP *bm = (BITMAP*)malloc(sizeof(BITMAP) + (sizeof(char *) * nr_pointers)); // Mod by NOP90 according to Allegro source

    bm->w = w;
    bm->h = h;

    bm->line = malloc(h*sizeof(uint8_t*) + padding);

    bm->dat = malloc(w * h * sizeof(char));
    if (h > 0) {
        bm->line[0] = bm->dat;
        for (i = 1; i < h; i++)
            bm->line[i] = bm->line[i-1] + w;
    }

    return bm;
}

void destroy_bitmap(BITMAP *bitmap) {
    if (bitmap) {
        if (bitmap->dat)
            free(bitmap->dat);
        free(bitmap->line);
        free(bitmap);
    }
}

void clear_to_color(BITMAP *bitmap, int color) {
    if (bitmap) {
        memset(bitmap->dat, color, (bitmap->w)*(bitmap->h));
    }
}

// Sound stuff

typedef struct {
    bool playing;
    int pos;
    int rate;
    float vol;
    float pan;
    SAMPLE* spl;
} csnd_channel;

csnd_channel* channels;
u32 available;

int install_sound(int digi, int midi, const char *config_path) {
#if DEBUGLEVEL == 0
    if (csndInit())
        return -1;
    available = csndChannels;
#else
    available = 0xFFFFFF00;
#endif

    channels = calloc(32, sizeof(csnd_channel));

    return 0;
}

void remove_sound() {
    free(channels);
}

SAMPLE* create_sample(int bits, int stereo, int freq, int len) {
    SAMPLE* spl;

    spl = malloc(sizeof(SAMPLE));
    if (!spl)
        return NULL;

    spl->bits = bits;
    spl->stereo = stereo;
    spl->freq = freq;
    spl->priority = 128;
    spl->len = len;
    spl->loop_start = 0;
    spl->loop_end = len;
    spl->param = 0;

    spl->data = linearAlloc(len * ((bits==8) ? 1 : sizeof(short)) * ((stereo) ? 2 : 1));
    if (!spl->data) {
        free(spl);
        return NULL;
    }

    return spl;
}

void destroy_sample(SAMPLE *spl) {
    if (spl) {
        if (spl->data)
            linearFree(spl->data);
        free(spl);
    }
}

int allocate_voice(SAMPLE* spl) {
    int i;

    for (i = 0; i < 32; i++) {
        if ((available>>i) & 1) {
            available &= ~(1<<i);
            channels[i].playing = false;
            channels[i].pos = 0;
            channels[i].rate = 0;
            channels[i].spl = spl;
            return i;
        }
    }

    return -1;
}

void voice_set_playmode(int voice, int playmode) {
    if (playmode == PLAYMODE_LOOP)
        CSND_SetLooping(voice, 1);
    else
        CSND_SetLooping(voice, 0);
}

void voice_stop(int voice) {
    CSND_SetPlayState(voice, 0);
    channels[voice].playing = false;
}

void deallocate_voice(int voice) {
    voice_stop(voice);
    available |= 1<<voice;
}

void voice_sweep_frequency(int voice, int time, int endfreq) {
    // TODO: Actually sweep
    channels[voice].rate = endfreq;
}

void voice_set_position(int voice, int position) {
    channels[voice].pos = position;
    if (channels[voice].playing && position == 0)
        voice_start(voice);
}

void voice_start(int voice) {
    // Don't play the sound if the voice isn't allocated
    if (available & (1<<voice))
        return;
    if (!channels[voice].spl || !channels[voice].spl->data)
        return;

    int sound_format = channels[voice].spl->bits == 8 ? SOUND_FORMAT_8BIT : SOUND_FORMAT_16BIT;
    channels[voice].playing = true;
    csndPlaySound(SOUND_CHANNEL(voice),
                  SOUND_REPEAT | sound_format,
                  (u32)(channels[voice].rate),
                  channels[voice].vol,
                  channels[voice].pan,
                  channels[voice].spl->data,
                  NULL,
                  (u32)(channels[voice].spl->len * channels[voice].spl->bits/8)
    );
}

void voice_set_volume(int voice, int volume) {
    channels[voice].vol = volume/255.0f;
    if (channels[voice].playing)
        CSND_SetVol((u32)voice, CSND_VOL(channels[voice].vol, channels[voice].pan), 0);
}

int voice_get_volume(int voice) {
    return (int)(channels[voice].vol*255);
}

void voice_set_pan(int voice, int pan) {
    channels[voice].pan = pan/128.0f - 1;
    if (channels[voice].playing)
        CSND_SetVol((u32)voice, CSND_VOL(channels[voice].vol, channels[voice].pan), 0);
}

void voice_ramp_volume(int voice, int time, int endvol) {
    // TODO: Actually ramp
    voice_set_volume(voice, endvol);
}

void voice_set_frequency(int voice, int frequency) {
    channels[voice].rate = frequency;
    if (channels[voice].playing)
        voice_start(voice);
}

int voice_get_frequency(int voice) {
    return channels[voice].rate;
}

// The following is not exactly allegro stuff

void toggle3D() {
    tVBOpt.DSPMODE = !tVBOpt.DSPMODE;
    gfxSet3D(tVBOpt.DSPMODE);
}

// Returns the position of the selected item or -1 if the menu was cancelled
int openMenu(menu_t* menu) {
    int i, numitems, pos, startpos;
    menu_t* cur_menu = menu;
    u32 keys;
    bool loop = true;

    gfxSetDoubleBuffering(GFX_TOP, false);

    while (loop) {
        pos = 0;
        startpos = 0;
        numitems = cur_menu->numitems;
        consoleClear();

        while (aptMainLoop() && loop) {
            hidScanInput();
            keys = hidKeysDown();

            if (keys & KEY_DUP) {
                pos--;
                if (pos < startpos) {
                    if (pos >= 0) {
                        startpos--;
                    } else {
                        pos = numitems - 1;
                        startpos = MAX(0, numitems - MAX_ITEMS + 1);
                    }
                    consoleClear();
                }
            } else if (keys & KEY_DDOWN) {
                pos++;
                if (pos >= MIN(numitems, startpos + MAX_ITEMS - 1)) {
                    if (pos >= numitems) {
                        pos = 0;
                        startpos = 0;
                    } else {
                        startpos++;
                    }
                    consoleClear();
                }
            } else if (keys & KEY_A) {
                if (cur_menu->items[pos].proc) {
                    int res = cur_menu->items[pos].proc();
                    if (res == D_EXIT) {
                        loop = false;
                        break;
                    }
                }
                if (cur_menu->items[pos].child) {
                    cur_menu = cur_menu->items[pos].child;
                    break;
                } else {
                    loop = false;
                    break;
                }
            } else if (keys & KEY_B) {
                if (cur_menu->parent) {
                    cur_menu = cur_menu->parent;
                } else {
                    pos = -1;
                    loop = false;
                    break;
                }
                break;
            } else if ((CONFIG_3D_SLIDERSTATE > 0.0f) && !tVBOpt.DSPMODE) {
                toggle3D();
            } else if ((CONFIG_3D_SLIDERSTATE == 0.0f) && tVBOpt.DSPMODE) {
                toggle3D();
            }

            printf("\x1b[;H\x1b[7m%s:\n\x1b[0m", cur_menu->title);

            for (i = startpos; i < MIN(numitems, startpos + MAX_ITEMS - 1); i++) {
                char line[40];

                strncpy(line, cur_menu->items[i].text, 38);

                if (i == pos)
                    printf("\x1b[1m>");
                else
                    printf(" ");

                printf(line);
                printf("\x1b[0m\n");
            }

            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }

    gfxSetDoubleBuffering(GFX_TOP, true);
    return pos;
}

// Taken from github.com/smealum/3ds_hb_menu
static inline void unicodeToChar(char* dst, uint16_t* src, int max) {
    if(!src || !dst) return;
    int n = 0;
    while (*src && n < max - 1) {
        *(dst++) = (*(src++)) & 0xFF;
        n++;
    }
    *dst = 0x00;
}

FS_archive sdmcArchive;
// TODO: Only show files that match the extension
int fileSelect(const char* message, char* path, const char* ext) {
    int i, pos = 0, item;
    menu_item_t files[64];
    char filenames[64][128];
    Handle dirHandle;
    uint32_t entries_read = 1;
    FS_dirent entry;

    sdmcArchive = (FS_archive){0x9, (FS_path){PATH_EMPTY, 1, (uint8_t*)"/"}};
    FSUSER_OpenArchive(NULL, &sdmcArchive);

    // Scan directory. Partially taken from github.com/smealum/3ds_hb_menu
    FSUSER_OpenDirectory(NULL, &dirHandle, sdmcArchive, FS_makePath(PATH_CHAR, "/vb/"));

    for(i = 0; i < 32 && entries_read; i++) {
        memset(&entry, 0, sizeof(FS_dirent));
        FSDIR_Read(dirHandle, &entries_read, 1, &entry);
        if(entries_read && !entry.isDirectory) {
            //if(!strncmp("VB", (char*) entry.shortExt, 2)) {
            unicodeToChar(filenames[i], entry.name, 128);
            files[pos].text = filenames[i];
            pos++;
            //}
        }
    }

    FSDIR_Close(dirHandle);
    FSUSER_CloseArchive(NULL, &sdmcArchive);

    item = openMenu(&(menu_t){message, NULL, pos, files});
    if (item >= 0 && item < pos)
        strncpy(path, files[item].text, 128);

    return item;
}
